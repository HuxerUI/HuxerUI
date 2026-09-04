"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const { File } = require("node:buffer");
const source = fs.readFileSync(process.argv[2], "utf8");
global.Emval = { toValue: (value) => value, toHandle: (value) => value };
global.File = File;

function embedded(name, parameters) {
  const declaration = source.indexOf(", " + name + ",");
  assert(declaration >= 0, name);
  const start = source.indexOf(", {", declaration) + 3;
  const end = source.indexOf("\n});", start);
  assert(end > start, name);
  return new Function(...parameters, source.slice(start, end));
}

const helpers = embedded("CreateWebFileHelpers", [])();
const create = embedded("CreateWebReferenceOperation", ["source_handle"]);
const start = embedded("StartWebReferenceOperation", ["operation_handle", "request_handle", "native_handle", "helper_handle"]);
const cancel = embedded("CancelWebReferenceOperation", ["operation_handle"]);
let complete;
global.Module = { _huxerui_web_file_reference_complete: (_, result) => complete(result) };

class FileHandle {
  constructor(name, bytes = new Uint8Array()) {
    this.name = name;
    this.kind = "file";
    this.bytes = bytes;
    this.writes = [];
  }
  async getFile() { return new File([this.bytes], this.name); }
  async isSameEntry(other) { return this === other; }
  async createWritable() {
    const chunks = [];
    return {
      write: async (chunk) => { chunks.push(chunk.slice()); this.writes.push(chunk.length); },
      close: async () => {
        if (this.failClose) { throw new Error("close failed"); }
        this.bytes = new Uint8Array(Buffer.concat(chunks));
        this.closed = true;
      },
      abort: async () => { this.aborted = true; },
    };
  }
}

class DirectoryHandle {
  constructor(name) { this.name = name; this.kind = "directory"; this.entries = new Map(); }
  async isSameEntry(other) { return this === other; }
  async resolve(target) {
    if (target === this) { return []; }
    for (const child of this.entries.values()) {
      if (child === target) { return [child.name]; }
      if (child.kind === "directory") {
        const nested = await child.resolve(target);
        if (nested !== null) { return [child.name, ...nested]; }
      }
    }
    return null;
  }
  async *values() { yield* this.entries.values(); }
  async getFileHandle(name, options) { return this.get(name, "file", options); }
  async getDirectoryHandle(name, options) { return this.get(name, "directory", options); }
  get(name, kind, options = {}) {
    const key = name.toLowerCase();
    let child = this.entries.get(key);
    if (child && child.kind !== kind) { throw new DOMException("wrong kind", "TypeMismatchError"); }
    if (!child && !options.create) { throw new DOMException("missing", "NotFoundError"); }
    if (!child) {
      child = kind === "file" ? new FileHandle(name) : new DirectoryHandle(name);
      this.entries.set(key, child);
    }
    return child;
  }
}

async function operation(handle, request, writable = true, canceled = false) {
  const state = create({ handle, writable, identity: "test-root" });
  if (canceled) { cancel(state); }
  const result = new Promise((resolve) => { complete = resolve; });
  start(state, request, 1, helpers);
  return result;
}

(async () => {
  const root = new DirectoryHandle("root");
  const bytes = Uint8Array.from({ length: 1024 * 1024 + 3 }, (_, index) => index % 251);
  const input = new FileHandle("source.bin", bytes);
  let result = await operation(root, { kind: "create", name: "空目录" });
  assert.equal(result.kind, 1);
  assert.equal(result.value.created, true);
  assert.equal(result.value.reference.type, 1);
  assert.equal(result.value.reference.size, null);
  result = await operation(root, { kind: "create", name: "空目录" });
  assert.equal(result.value.created, false);
  result = await operation(root, { kind: "create", name: "denied" }, false);
  assert.equal(result.errorCode, 1);
  assert(!root.entries.has("denied"));
  result = await operation(root, { kind: "copy", name: ".Binary", input: { handle: input }, overwrite: false });
  assert.equal(result.kind, 1);
  assert.equal(result.value.bytes, bytes.length);
  const copied = await root.getFileHandle(".Binary");
  assert(copied.closed);
  assert.deepEqual(copied.bytes, bytes);
  assert(copied.writes.length > 1);
  assert(Math.max(...copied.writes) <= 64 * 1024);
  result = await operation(root, { kind: "copy", name: ".Binary", input: { handle: input }, overwrite: false });
  assert.equal(result.errorCode, 7);
  result = await operation(root, { kind: "copy", name: ".binary", input: { handle: input }, overwrite: true });
  assert.equal(result.errorCode, 6);
  result = await operation(root, { kind: "list" }, false);
  assert.equal(result.value.length, 2);
  assert(result.value.every((entry) => !entry.canWrite));
  const firstIdentity = result.value.find((entry) => entry.name === ".Binary").source.identity;
  result = await operation(root, { kind: "find", name: ".binary" });
  assert.equal(result.value.source.identity, firstIdentity);
  const nested = await root.getDirectoryHandle("空目录");
  assert.equal((await operation(root, { kind: "check", target: { handle: nested } })).value, false);
  assert.equal((await operation(nested, { kind: "check", target: { handle: root } })).value, false);
  assert.equal((await operation(root, { kind: "check", target: { handle: new DirectoryHandle("other") } })).value, true);
  copied.failClose = true;
  result = await operation(root, { kind: "copy", name: ".Binary", input: { handle: input }, overwrite: true });
  assert.equal(result.kind, 3);
  assert(copied.aborted);
  result = await operation(root, { kind: "create", name: "canceled" }, true, true);
  assert.equal(result.kind, 3);
  assert(!root.entries.has("canceled"));
  for (const name of ["..", "a/b", "a\\b", "a\0b"]) {
    assert.equal((await operation(root, { kind: "create", name })).errorCode, 6);
  }

  const localFiles = new Map([["/output.bin", new Uint8Array([99])]]);
  let failLocalClose = false;
  global.FS = {
    analyzePath: (path) => ({ exists: localFiles.has(path) }),
    isFile: (mode) => mode === 1,
    lstat: () => ({ mode: 1 }),
    open: (path, mode) => {
      if (mode === "w") { localFiles.set(path, new Uint8Array()); }
      return { path, mode };
    },
    read: (stream, buffer, offset, length, position) => {
      const chunk = localFiles.get(stream.path).subarray(position, position + length);
      buffer.set(chunk, offset);
      return chunk.length;
    },
    write: (stream, buffer, offset, length, position) => {
      assert(length <= 64 * 1024);
      const next = new Uint8Array(position + length);
      next.set(localFiles.get(stream.path));
      next.set(buffer.subarray(offset, offset + length), position);
      localFiles.set(stream.path, next);
      return length;
    },
    close: (stream) => {
      if (stream.mode === "w" && failLocalClose) { throw new Error("local close failed"); }
    },
    rename: (from, to) => { localFiles.set(to, localFiles.get(from)); localFiles.delete(from); },
    unlink: (path) => localFiles.delete(path),
  };
  result = await operation(input, { kind: "import", path: "/output.bin", overwrite: false });
  assert.equal(result.errorCode, 7);
  result = await operation(input, { kind: "import", path: "/output.bin", overwrite: true });
  assert.equal(result.value, bytes.length);
  assert.deepEqual(localFiles.get("/output.bin"), bytes);
  assert.equal(localFiles.size, 1);
  failLocalClose = true;
  result = await operation(input, { kind: "import", path: "/failed.bin", overwrite: false });
  assert.equal(result.kind, 3);
  assert.equal(localFiles.size, 1);
  failLocalClose = false;
  result = await operation(root, { kind: "copy", name: "export.bin", input: { path: "/output.bin" }, overwrite: false });
  assert.equal(result.value.bytes, bytes.length);
  assert.deepEqual((await root.getFileHandle("export.bin")).bytes, bytes);
  console.log("Web file-reference tests passed");
})().catch((error) => { console.error(error); process.exitCode = 1; });
