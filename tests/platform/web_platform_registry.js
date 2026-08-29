"use strict";

const assert = require("node:assert/strict");
const path = require("node:path");

const runtimePath = process.argv[2];
if (!runtimePath) {
  throw new Error("HuxerUI Web PlatformRegistry test requires the bridge runtime path");
}

const calls = [];
global.Module = {
  huxeruiWebPlatformEmit(handle, event, payload) {
    calls.push({ operation: "emit", handle, event, payload });
    return true;
  },
  huxeruiWebPlatformReleaseEvent(handle) {
    calls.push({ operation: "releaseEvent", handle });
  },
  huxeruiWebPlatformComplete(handle, payload) {
    calls.push({ operation: "complete", handle, payload });
    return true;
  },
  huxeruiWebPlatformFail(handle, code, message, details) {
    calls.push({ operation: "fail", handle, code, message, details });
    return true;
  },
  huxeruiWebPlatformReleaseResult(handle) {
    calls.push({ operation: "releaseResult", handle });
  },
};

require(path.resolve(runtimePath));

const Payload = Module.HuxerUI.PlatformPayload;

const payload = Payload.object({
  boolean: Payload.booleanValue(true),
  bytes: Payload.bytes(new Uint8Array([1, 2, 3])),
  double: Payload.doubleValue(-12.5),
  integer: Payload.int64(-(1n << 63n)),
  list: Payload.list([Payload.nullValue(), Payload.string("value")]),
});
const decoded = Payload.decode(payload.encode());

assert.equal(decoded.kind, Payload.Kind.OBJECT);
assert.equal(decoded.requireField("boolean").requireBoolean(), true);
assert.deepEqual(decoded.requireField("bytes").requireBytes(), new Uint8Array([1, 2, 3]));
assert.equal(decoded.requireField("double").requireDouble(), -12.5);
assert.equal(decoded.requireField("integer").requireInt64(), -(1n << 63n));
assert.equal(decoded.requireField("list").element(1).requireString(), "value");
assert.deepEqual(
  Payload.object({ second: Payload.int64(2n), first: Payload.int64(1n) }).encode(),
  Payload.object({ first: Payload.int64(1n), second: Payload.int64(2n) }).encode(),
);

const copiedBytes = decoded.requireField("bytes").requireBytes();
copiedBytes[0] = 9;
assert.deepEqual(decoded.requireField("bytes").requireBytes(), new Uint8Array([1, 2, 3]));
const copiedFields = decoded.fields();
copiedFields.clear();
assert.equal(decoded.requireField("boolean").requireBoolean(), true);
assert(Object.isFrozen(decoded));
assert(Object.isFrozen(Payload.prototype));
assert.throws(() => Payload.int64(1), /signed 64-bit bigint/);
assert.throws(() => Payload.doubleValue(Number.POSITIVE_INFINITY), /finite number/);
assert.throws(() => Payload.object(new Date()), /Map or plain object/);
assert.throws(() => Payload.list([Object.create(Payload.prototype)]), /requires a PlatformPayload value/);
assert.throws(() => Payload.decode(payload.encode().subarray(0, 7)), /truncated input/);
assert.throws(() => decoded.requireField("missing"), /\$\.missing/);

const events = Module.huxerUIWebPlatformBridge.createEvents(41);
events.emit("changed", Payload.string("next"));
events.close();
events.close();
assert.equal(calls[0].operation, "emit");
assert.equal(calls[0].payload.requireString(), "next");
assert.deepEqual(calls[1], { operation: "releaseEvent", handle: 41 });

const completion = Module.huxerUIWebPlatformBridge.createResult(42);
completion.complete(Payload.int64(7n));
completion.complete(Payload.int64(8n));
completion.close();
assert.equal(calls[2].operation, "complete");
assert.equal(calls[2].payload.requireInt64(), 7n);

const failure = Module.huxerUIWebPlatformBridge.createResult(43);
failure.fail("example/error", "failed", Payload.string("details"));
failure.close();
assert.equal(calls[3].operation, "fail");
assert.equal(calls[3].details.requireString(), "details");

const abandoned = Module.huxerUIWebPlatformBridge.createResult(44);
abandoned.close();
abandoned.close();
assert.deepEqual(calls[4], { operation: "releaseResult", handle: 44 });

console.log("HuxerUI Web PlatformRegistry tests passed");
