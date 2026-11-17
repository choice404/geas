// The host driven order saga from Node: skeleton/saga.geas served by the
// generic bridge server, the .proto loaded at runtime and the session and
// its signal helper from the emitted TypeScript wrapper. The happy path
// lands every step and the delivered signal and reads Fulfilled; the break
// path ships a reservation the carrier refuses, reads the Err arm as a
// value, and sees the saga Broken with ship's error on its partial surface.

import grpc from "@grpc/grpc-js";

import {
  OrderSagaShapeHash,
  openOrderSagaSession,
  signalOrderSagaDelivered,
} from "./gen/saga_session.ts";
import { call, check, expectCode, makeClient, sliceEq, waitReady, waitUntil } from "./kit.mjs";

const HOUR_MS = 60 * 60 * 1000;
const GRACE_MS = 30 * 60 * 1000;

// Lands authorize then capture after capture is refused early, and checks
// the receipt carries the amount vow
async function landPayment(client, id, amount) {
  await expectCode(
    call(client, "Capture", { instance_id: id, auth: { token: "early" } }),
    grpc.status.FAILED_PRECONDITION,
    "FAILED_PRECONDITION",
    "capture before authorize",
  );
  const auth = await call(client, "Authorize", { instance_id: id, card: { number: "4111" } });
  check(auth.result === "ok" && auth.ok.token === "AUTH-4111", "authorize answers the AuthId record");
  const receipt = await call(client, "Capture", { instance_id: id, auth: auth.ok });
  check(receipt.result === "ok" && receipt.ok.amount === String(amount),
    "capture's receipt carries the amount vow");
}

async function reserve(client, id, sku) {
  const res = await call(client, "Reserve", { instance_id: id, sku, hold: String(HOUR_MS) });
  check(res.result === "ok" && res.ok.hold === String(HOUR_MS + GRACE_MS),
    "reserve's Geas body adds the grace vow to the hold");
  return res.ok;
}

async function happyPath(client, pinned) {
  const s = await openOrderSagaSession(client, { expected_hash: pinned });
  const id = s.signed.instance_id;
  check(s.signed.amount === "100" && s.signed.grace === String(GRACE_MS),
    "the signature carries the declared vows");
  await landPayment(client, id, 100);
  await expectCode(
    signalOrderSagaDelivered(client, s, { tracking: { carrier: "ACME", code: "early" } }),
    grpc.status.FAILED_PRECONDITION,
    "FAILED_PRECONDITION",
    "delivered before ship",
  );
  const res = await reserve(client, id, "sku-1");
  const shipped = await call(client, "Ship", { instance_id: id, res });
  check(shipped.result === "ok" && shipped.ok.code === "T-R-sku-1", "ship answers the tracking");
  let p = await call(client, "GetPartial", { instance_id: id });
  check(p.state === "PARTIAL" && sliceEq(p.pending, ["delivered"]),
    "every step landed, the saga waits on its signal");
  await signalOrderSagaDelivered(client, s, { tracking: shipped.ok });
  p = await call(client, "GetPartial", { instance_id: id });
  check(p.state === "FULFILLED", "the delivered signal fulfills the saga");
  console.log(`[bridge_client_node] saga ${id} fulfilled on its delivered signal`);
  s.close();
}

async function breakPath(client, pinned) {
  const s = await openOrderSagaSession(client, { amount: "250", expected_hash: pinned });
  const id = s.signed.instance_id;
  await landPayment(client, id, 250);
  const res = await reserve(client, id, "refuse");
  const shipped = await call(client, "Ship", { instance_id: id, res });
  check(shipped.result === "err" && shipped.err.arm === "refused" &&
    shipped.err.refused.reason === "no carrier takes it",
  "ship's Err arrives as a value carrying its sum arm");
  const p = await waitUntil(() => call(client, "GetPartial", { instance_id: id }),
    (r) => r.state === "BROKEN", 5000);
  check(p.state === "BROKEN", "the break line fired and the remedies ran");
  check((p.errors ?? []).length === 1 && p.errors[0].pledge === "ship" && p.errors[0].err === "1",
    "the partial surface names ship's error arm");
  await expectCode(
    signalOrderSagaDelivered(client, s, { tracking: { carrier: "ACME", code: "T-1" } }),
    grpc.status.FAILED_PRECONDITION,
    "FAILED_PRECONDITION",
    "a delivery into a broken saga",
  );
  console.log(`[bridge_client_node] saga ${id} broke on ship and read BROKEN`);
  s.close();
}

export async function walk(addr) {
  const client = makeClient("saga", "saga", "OrderSaga", addr);
  await waitReady(client);
  const pinned = OrderSagaShapeHash.toString();
  await happyPath(client, pinned);
  await breakPath(client, pinned);
}
