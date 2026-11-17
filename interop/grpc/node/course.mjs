// The engine driven order saga from Node: skeleton/order_course.geas served
// by the generic bridge server. There is no pledge rpc to call; the course
// runs every step on the server through the handler bodies it was started
// with, and this walk opens the session, watches the course through the
// wrapper's getOrderCourseCourse, hands the delivered signal in, and reads
// the course FINISHED and the saga Fulfilled.

import {
  OrderCourseShapeHash,
  getOrderCourseCourse,
  openOrderCourseSession,
  signalOrderCourseDelivered,
} from "./gen/order_course_session.ts";
import { call, check, makeClient, waitReady, waitUntil } from "./kit.mjs";

const WAIT_MS = 10000;

export async function walk(addr) {
  const client = makeClient("order_course", "order_course", "OrderCourse", addr);
  await waitReady(client);
  const s = await openOrderCourseSession(client, {
    expected_hash: OrderCourseShapeHash.toString(),
  });
  const id = s.signed.instance_id;
  let course = await waitUntil(() => getOrderCourseCourse(client, s),
    (c) => c.state === "COURSE_WAITING", WAIT_MS);
  check(course.state === "COURSE_WAITING" && course.wait_due_ms !== "0",
    "the course rests at its select with a wait due");
  let p = await call(client, "GetPartial", { instance_id: id });
  check(p.state === "PARTIAL", "the course landed its steps and waits on delivered");

  await signalOrderCourseDelivered(client, s, { tracking: { carrier: "ACME", code: "T-R-sku-1" } });
  course = await waitUntil(() => getOrderCourseCourse(client, s),
    (c) => c.state === "COURSE_FINISHED", WAIT_MS);
  check(course.state === "COURSE_FINISHED", "the course ran to its end");
  p = await call(client, "GetPartial", { instance_id: id });
  check(p.state === "FULFILLED", "the course fulfilled the saga");
  console.log(`[bridge_client_node] course ${id} finished, saga FULFILLED`);
  s.close();
}
