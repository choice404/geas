// What every Node bridge walk shares: the check tally, the unary surface
// as promises, the status assertion, and a client over an emitted .proto
// loaded at runtime through @grpc/proto-loader, no protoc and no stub.

import { fileURLToPath } from "node:url";
import { setTimeout as sleep } from "node:timers/promises";

import grpc from "@grpc/grpc-js";
import protoLoader from "@grpc/proto-loader";

export const tally = { failures: 0 };

export function check(cond, what) {
  if (!cond) {
    console.error(`[bridge_client_node] FAIL: ${what}`);
    tally.failures = 1;
  }
}

// A service client over gen/<stem>.proto, the loader options the emitted
// wrappers spell out: snake_case fields, 64 bit integers as decimal
// strings, enums by name, and oneofs naming their set arm.
export function makeClient(stem, pkg, service, addr) {
  const def = protoLoader.loadSync(
    fileURLToPath(new URL(`./gen/${stem}.proto`, import.meta.url)),
    { keepCase: true, longs: String, enums: String, oneofs: true, defaults: false },
  );
  const loaded = grpc.loadPackageDefinition(def);
  return new loaded.geas[pkg][service](addr, grpc.credentials.createInsecure());
}

// The unary surface as promises. A pledge's Err never rejects: only a call
// that did not run does.
export function call(client, method, req) {
  return new Promise((resolve, reject) => {
    client[method](req, (err, reply) => {
      if (err) {
        reject(err);
      } else {
        resolve(reply);
      }
    });
  });
}

export async function expectCode(promise, code, name, what) {
  try {
    await promise;
    check(false, `${what} did not fail`);
  } catch (err) {
    check(err.code === code, `${what} is ${name}, got code ${err.code}`);
  }
}

export function sliceEq(got, want) {
  const have = got ?? [];
  return have.length === want.length && have.every((v, i) => v === want[i]);
}

export function waitReady(client) {
  return new Promise((resolve, reject) => {
    client.waitForReady(Date.now() + 10000, (err) => (err ? reject(err) : resolve()));
  });
}

// Polls read() until done holds on what it answered, or timeoutMs passes,
// and answers the last reading.
export async function waitUntil(read, done, timeoutMs) {
  const t0 = Date.now();
  let reading = await read();
  while (!done(reading) && Date.now() - t0 < timeoutMs) {
    await sleep(10);
    reading = await read();
  }
  return reading;
}
