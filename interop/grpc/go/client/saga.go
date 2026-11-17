// The host driven order saga from Go: skeleton/saga.geas served by the
// generic bridge server, driven through protoc's stubs over the emitted
// saga.proto and the emitted session wrapper, whose signal helper hands the
// delivered signal in keyed by the session's own id. The happy path lands
// every step and the signal and reads Fulfilled; the break path ships a
// reservation the carrier refuses, reads the Err arm as a value, and sees
// the saga Broken with ship's error on its partial surface.
package main

import (
	"context"
	"fmt"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/protobuf/proto"

	"geasbridge/sagapb"
)

const (
	sagaAmount  = 100
	sagaGraceMs = 30 * 60 * 1000
	sagaHourMs  = 60 * 60 * 1000
	// ShipError's Refused arm, its declaration index
	shipRefused = 1
)

func sagaPartial(ctx context.Context, c sagapb.OrderSagaClient, id uint64) *sagapb.PartialReply {
	p, err := c.GetPartial(ctx, &sagapb.PartialRequest{InstanceId: id})
	if err != nil {
		check(false, fmt.Sprintf("GetPartial %d: %v", id, err))
		return &sagapb.PartialReply{}
	}
	return p
}

// sagaPayment lands authorize then capture, the Payment subcontract,
// through the server's handler bodies, after capture is refused early.
func sagaPayment(ctx context.Context, c sagapb.OrderSagaClient, id uint64, amount int64) {
	_, err := c.Capture(ctx, &sagapb.CaptureRequest{InstanceId: id, Auth: &sagapb.AuthId{Token: "early"}})
	expectCode(err, codes.FailedPrecondition, "capture before authorize")
	auth, err := c.Authorize(ctx, &sagapb.AuthorizeRequest{InstanceId: id, Card: &sagapb.Card{Number: "4111"}})
	check(err == nil && auth.GetOk().GetToken() == "AUTH-4111", "authorize answers the AuthId record")
	receipt, err := c.Capture(ctx, &sagapb.CaptureRequest{InstanceId: id, Auth: auth.GetOk()})
	check(err == nil && receipt.GetOk().GetAmount() == amount, "capture's receipt carries the amount vow")
}

func sagaReserve(ctx context.Context, c sagapb.OrderSagaClient, id uint64, sku string) *sagapb.Reservation {
	res, err := c.Reserve(ctx, &sagapb.ReserveRequest{InstanceId: id, Sku: sku, Hold: sagaHourMs})
	check(err == nil && res.GetOk().GetHold() == sagaHourMs+sagaGraceMs,
		"reserve's Geas body adds the grace vow to the hold")
	return res.GetOk()
}

func sagaHappy(ctx context.Context, c sagapb.OrderSagaClient) {
	s, err := sagapb.OpenOrderSagaSession(ctx, c, &sagapb.SignRequest{ExpectedHash: sagapb.OrderSagaShapeHash})
	if err != nil {
		check(false, fmt.Sprintf("the pinned saga sign refused: %v", err))
		return
	}
	defer s.Close()
	id := s.Signed.GetInstanceId()
	check(s.Signed.GetAmount() == sagaAmount && s.Signed.GetGrace() == sagaGraceMs,
		"the signature carries the declared vows")
	sagaPayment(ctx, c, id, sagaAmount)
	err = s.SignalDelivered(ctx, c, &sagapb.SignalDeliveredRequest{Tracking: &sagapb.Tracking{Carrier: "ACME"}})
	expectCode(err, codes.FailedPrecondition, "delivered before ship")
	res := sagaReserve(ctx, c, id, "sku-1")
	shipped, err := c.Ship(ctx, &sagapb.ShipRequest{InstanceId: id, Res: res})
	check(err == nil && shipped.GetOk().GetCode() == "T-R-sku-1", "ship answers the tracking")
	p := sagaPartial(ctx, c, id)
	check(p.GetState() == sagapb.ContractState_PARTIAL && sliceEq(p.GetPending(), "delivered"),
		"every step landed, the saga waits on its signal")
	err = s.SignalDelivered(ctx, c, &sagapb.SignalDeliveredRequest{Tracking: shipped.GetOk()})
	check(err == nil, "the delivered signal lands")
	p = sagaPartial(ctx, c, id)
	check(p.GetState() == sagapb.ContractState_FULFILLED, "the delivered signal fulfills the saga")
	fmt.Printf("[bridge_client_go] saga %d fulfilled on its delivered signal\n", id)
}

func sagaBreak(ctx context.Context, c sagapb.OrderSagaClient) {
	s, err := sagapb.OpenOrderSagaSession(ctx, c, &sagapb.SignRequest{
		Amount: proto.Int64(250), ExpectedHash: sagapb.OrderSagaShapeHash,
	})
	if err != nil {
		check(false, fmt.Sprintf("the saga sign refused: %v", err))
		return
	}
	defer s.Close()
	id := s.Signed.GetInstanceId()
	sagaPayment(ctx, c, id, 250)
	res := sagaReserve(ctx, c, id, "refuse")
	shipped, err := c.Ship(ctx, &sagapb.ShipRequest{InstanceId: id, Res: res})
	check(err == nil && shipped.GetErr().GetRefused().GetReason() == "no carrier takes it",
		"ship's Err arrives as a value carrying its sum arm")
	p := sagaPartial(ctx, c, id)
	for deadline := time.Now().Add(5 * time.Second); p.GetState() != sagapb.ContractState_BROKEN && time.Now().Before(deadline); {
		time.Sleep(10 * time.Millisecond)
		p = sagaPartial(ctx, c, id)
	}
	check(p.GetState() == sagapb.ContractState_BROKEN, "the break line fired and the remedies ran")
	check(len(p.GetErrors()) == 1 && p.GetErrors()[0].GetPledge() == "ship" &&
		p.GetErrors()[0].GetErr() == shipRefused, "the partial surface names ship's error arm")
	err = s.SignalDelivered(ctx, c, &sagapb.SignalDeliveredRequest{Tracking: &sagapb.Tracking{Carrier: "ACME"}})
	expectCode(err, codes.FailedPrecondition, "a delivery into a broken saga")
	fmt.Printf("[bridge_client_go] saga %d broke on ship and read BROKEN\n", id)
}

func runSaga(ctx context.Context, conn *grpc.ClientConn) {
	c := sagapb.NewOrderSagaClient(conn)
	if !serverUp(func() error {
		_, err := c.GetPartial(ctx, &sagapb.PartialRequest{InstanceId: 1})
		return err
	}) {
		return
	}
	sagaHappy(ctx, c)
	sagaBreak(ctx, c)
}
