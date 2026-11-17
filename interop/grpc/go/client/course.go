// The engine driven order saga from Go: skeleton/order_course.geas served
// by the generic bridge server. The emitted surface carries no pledge rpc,
// the course runs every step on the server through the handler bodies it
// was started with, and this client only opens the session, watches the
// course through the wrapper's GetCourse, hands the delivered signal in,
// and reads the course FINISHED and the saga Fulfilled.
package main

import (
	"context"
	"fmt"
	"time"

	"google.golang.org/grpc"

	"geasbridge/order_coursepb"
)

const courseWait = 10 * time.Second

func courseUntil(ctx context.Context, s *order_coursepb.OrderCourseSession, c order_coursepb.OrderCourseClient,
	want order_coursepb.CourseState) *order_coursepb.CourseReply {
	deadline := time.Now().Add(courseWait)
	for {
		reply, err := s.GetCourse(ctx, c)
		if err != nil {
			check(false, fmt.Sprintf("GetCourse: %v", err))
			return &order_coursepb.CourseReply{}
		}
		if reply.GetState() == want || time.Now().After(deadline) {
			return reply
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func runCourse(ctx context.Context, conn *grpc.ClientConn) {
	c := order_coursepb.NewOrderCourseClient(conn)
	if !serverUp(func() error {
		_, err := c.GetPartial(ctx, &order_coursepb.PartialRequest{InstanceId: 1})
		return err
	}) {
		return
	}
	s, err := order_coursepb.OpenOrderCourseSession(ctx, c, &order_coursepb.SignRequest{
		ExpectedHash: order_coursepb.OrderCourseShapeHash,
	})
	if err != nil {
		check(false, fmt.Sprintf("the pinned course sign refused: %v", err))
		return
	}
	defer s.Close()
	id := s.Signed.GetInstanceId()

	reply := courseUntil(ctx, s, c, order_coursepb.CourseState_COURSE_WAITING)
	check(reply.GetState() == order_coursepb.CourseState_COURSE_WAITING && reply.GetWaitDueMs() > 0,
		"the course rests at its select with a wait due")
	p, err := c.GetPartial(ctx, &order_coursepb.PartialRequest{InstanceId: id})
	check(err == nil && p.GetState() == order_coursepb.ContractState_PARTIAL,
		"the course landed its steps and waits on delivered")

	err = s.SignalDelivered(ctx, c, &order_coursepb.SignalDeliveredRequest{
		Tracking: &order_coursepb.Tracking{Carrier: "ACME", Code: "T-R-sku-1"},
	})
	check(err == nil, "the delivered signal lands")
	reply = courseUntil(ctx, s, c, order_coursepb.CourseState_COURSE_FINISHED)
	check(reply.GetState() == order_coursepb.CourseState_COURSE_FINISHED, "the course ran to its end")
	p, err = c.GetPartial(ctx, &order_coursepb.PartialRequest{InstanceId: id})
	check(err == nil && p.GetState() == order_coursepb.ContractState_FULFILLED, "the course fulfilled the saga")
	fmt.Printf("[bridge_client_go] course %d finished, saga FULFILLED\n", id)
}
