(() => {
  const maxEnvelopeBytes = 64 * 1024 * 1024;
  const maxScalarBytes = 16 * 1024 * 1024;
  const maxContainerEntries = 1024 * 1024;
  const maxCapabilitySlots = 1024 * 1024;
  const maxNestingDepth = 64;
  const internalConstruction = Symbol("HuxerUI PlatformPayload construction");
  const payloadData = new WeakMap();
  // Private state makes public wrappers unforgeable while allowing garbage collection to release native capabilities.
  const fileReferenceData = new WeakMap();
  const textEncoder = new TextEncoder();
  const textDecoder = new TextDecoder("utf-8", { fatal: true });

  const tags = Object.freeze({
    null: 0,
    boolean: 1,
    int64: 2,
    double: 3,
    string: 4,
    bytes: 5,
    list: 6,
    object: 7,
    externalTexture: 8,
    fileReference: 9,
    bufferReference: 10,
  });
  const kinds = Object.freeze(Object.keys(tags));

  function requireValidUnicode(value, description) {
    for (let index = 0; index < value.length; ++index) {
      const unit = value.charCodeAt(index);
      if (unit >= 0xd800 && unit <= 0xdbff) {
        if (++index >= value.length) {
          throw new TypeError(`HuxerUI PlatformPayload ${description} is not valid Unicode`);
        }
        const next = value.charCodeAt(index);
        if (next < 0xdc00 || next > 0xdfff) {
          throw new TypeError(`HuxerUI PlatformPayload ${description} is not valid Unicode`);
        }
      } else if (unit >= 0xdc00 && unit <= 0xdfff) {
        throw new TypeError(`HuxerUI PlatformPayload ${description} is not valid Unicode`);
      }
    }
  }

  function compareBytes(left, right) {
    const count = Math.min(left.length, right.length);
    for (let index = 0; index < count; ++index) {
      if (left[index] !== right[index]) {
        return left[index] - right[index];
      }
    }
    return left.length - right.length;
  }

  function childPath(path, name) {
    return `${path}.${name}`;
  }

  class EnvelopeWriter {
    constructor(fileReferences = null) {
      this.buffer = new Uint8Array(256);
      this.length = 0;
      this.fileReferences = fileReferences;
      this.fileReferenceSlots = new Map();
    }

    ensure(additional) {
      const required = this.length + additional;
      if (!Number.isSafeInteger(additional) || additional < 0 || required > maxEnvelopeBytes) {
        throw new RangeError("HuxerUI PlatformPayload envelope is too large");
      }
      if (required <= this.buffer.length) {
        return;
      }
      let capacity = this.buffer.length;
      while (capacity < required) {
        capacity = Math.min(maxEnvelopeBytes, Math.max(capacity * 2, required));
      }
      const grown = new Uint8Array(capacity);
      grown.set(this.buffer.subarray(0, this.length));
      this.buffer = grown;
    }

    byte(value) {
      this.ensure(1);
      this.buffer[this.length++] = value;
    }

    uint16(value) {
      this.ensure(2);
      new DataView(this.buffer.buffer).setUint16(this.length, value, true);
      this.length += 2;
    }

    uint32(value) {
      this.ensure(4);
      new DataView(this.buffer.buffer).setUint32(this.length, value, true);
      this.length += 4;
    }

    int64(value) {
      this.ensure(8);
      new DataView(this.buffer.buffer).setBigInt64(this.length, value, true);
      this.length += 8;
    }

    double(value) {
      this.ensure(8);
      new DataView(this.buffer.buffer).setFloat64(this.length, value, true);
      this.length += 8;
    }

    bytes(value, maximum, description) {
      if (value.length > maximum) {
        throw new RangeError(`HuxerUI PlatformPayload ${description} is too large`);
      }
      this.uint32(value.length);
      this.ensure(value.length);
      this.buffer.set(value, this.length);
      this.length += value.length;
    }

    string(value) {
      this.bytes(textEncoder.encode(value), maxScalarBytes, "string");
    }

    value(payload, depth) {
      if (depth > maxNestingDepth) {
        throw new RangeError("HuxerUI PlatformPayload exceeds the maximum envelope nesting depth");
      }
      const stored = payloadData.get(payload);
      this.byte(stored.tag);
      switch (stored.tag) {
        case tags.null:
          return;
        case tags.boolean:
          this.byte(stored.value ? 1 : 0);
          return;
        case tags.int64:
          this.int64(stored.value);
          return;
        case tags.double:
          this.double(stored.value);
          return;
        case tags.string:
          this.string(stored.value);
          return;
        case tags.bytes:
          this.bytes(stored.value, maxScalarBytes, "byte value");
          return;
        case tags.list:
          this.uint32(stored.value.length);
          for (const element of stored.value) {
            this.value(element, depth + 1);
          }
          return;
        case tags.object:
          this.uint32(stored.value.size);
          for (const [name, value] of stored.value) {
            this.string(name);
            this.value(value, depth + 1);
          }
          return;
        case tags.bufferReference:
          throw new TypeError("HuxerUI Web bridge does not support BufferReference");
        case tags.externalTexture:
          throw new TypeError("HuxerUI Web PlatformPayload does not support ExternalTexture capabilities");
        case tags.fileReference: {
          if (this.fileReferences === null) {
            throw new TypeError("HuxerUI PlatformPayload capabilities require the platform bridge envelope");
          }
          this.byte(2);
          const reference = stored.value;
          const capabilityKey = requireFileReference(reference).capabilityKey;
          let slot = this.fileReferenceSlots.get(capabilityKey);
          if (slot === undefined) {
            if (this.fileReferences.length >= maxCapabilitySlots) {
              throw new RangeError("HuxerUI PlatformPayload contains too many retained capabilities");
            }
            slot = this.fileReferences.length;
            this.fileReferenceSlots.set(capabilityKey, slot);
            this.fileReferences.push(reference);
          }
          this.uint32(slot);
          return;
        }
        default:
          throw new TypeError("HuxerUI PlatformPayload contains an unknown kind");
      }
    }

    finish() {
      return this.buffer.slice(0, this.length);
    }
  }

  class EnvelopeReader {
    constructor(bytes, fileReferences = []) {
      if (!(bytes instanceof Uint8Array)) {
        throw new TypeError("HuxerUI PlatformPayload envelope must be a Uint8Array");
      }
      if (bytes.byteLength > maxEnvelopeBytes) {
        throw new RangeError("HuxerUI PlatformPayload envelope is too large");
      }
      if (!Array.isArray(fileReferences) || fileReferences.length > maxCapabilitySlots) {
        throw new TypeError("HuxerUI PlatformPayload file reference table is invalid");
      }
      const capabilityKeys = new Set();
      for (const reference of fileReferences) {
        const capabilityKey = requireFileReference(reference).capabilityKey;
        if (capabilityKeys.has(capabilityKey)) {
          throw new TypeError("HuxerUI PlatformPayload file reference table is invalid");
        }
        capabilityKeys.add(capabilityKey);
      }
      this.bytes = bytes.slice();
      this.view = new DataView(this.bytes.buffer, this.bytes.byteOffset, this.bytes.byteLength);
      this.offset = 0;
      this.fileReferences = fileReferences;
      this.usedFileReferences = new Array(fileReferences.length).fill(false);
    }

    read() {
      if (this.byte() !== 0x48 || this.byte() !== 0x55 || this.byte() !== 0x58 || this.byte() !== 0x50) {
        throw this.malformed("invalid header");
      }
      if (this.uint16() !== 1) {
        throw this.malformed("unsupported version");
      }
      if (this.uint16() !== 0) {
        throw this.malformed("unsupported flags");
      }
      const payload = this.value(0, "$");
      if (this.offset !== this.bytes.length) {
        throw this.malformed("trailing bytes");
      }
      if (this.usedFileReferences.includes(false)) {
        throw this.malformed("unreferenced capability");
      }
      return payload;
    }

    require(length) {
      if (!Number.isSafeInteger(length) || length < 0 || this.offset + length > this.bytes.length) {
        throw this.malformed("truncated input");
      }
    }

    byte() {
      this.require(1);
      return this.bytes[this.offset++];
    }

    uint16() {
      this.require(2);
      const value = this.view.getUint16(this.offset, true);
      this.offset += 2;
      return value;
    }

    uint32() {
      this.require(4);
      const value = this.view.getUint32(this.offset, true);
      this.offset += 4;
      return value;
    }

    int64() {
      this.require(8);
      const value = this.view.getBigInt64(this.offset, true);
      this.offset += 8;
      return value;
    }

    double() {
      this.require(8);
      const value = this.view.getFloat64(this.offset, true);
      this.offset += 8;
      if (!Number.isFinite(value)) {
        throw this.malformed("non-finite double");
      }
      return Object.is(value, -0) ? 0 : value;
    }

    length(maximum) {
      const value = this.uint32();
      if (value > maximum) {
        throw this.malformed("excessive length");
      }
      return value;
    }

    byteValue(maximum) {
      const length = this.length(maximum);
      this.require(length);
      const result = this.bytes.slice(this.offset, this.offset + length);
      this.offset += length;
      return result;
    }

    string() {
      try {
        return textDecoder.decode(this.byteValue(maxScalarBytes));
      } catch (_) {
        throw this.malformed("invalid UTF-8 string");
      }
    }

    value(depth, path) {
      if (depth > maxNestingDepth) {
        throw this.malformed("excessive nesting");
      }
      const tag = this.byte();
      switch (tag) {
        case tags.null:
          return new PlatformPayload(internalConstruction, tag, null, path);
        case tags.boolean: {
          const value = this.byte();
          if (value > 1) {
            throw this.malformed("invalid boolean");
          }
          return new PlatformPayload(internalConstruction, tag, value !== 0, path);
        }
        case tags.int64:
          return new PlatformPayload(internalConstruction, tag, this.int64(), path);
        case tags.double:
          return new PlatformPayload(internalConstruction, tag, this.double(), path);
        case tags.string:
          return new PlatformPayload(internalConstruction, tag, this.string(), path);
        case tags.bytes:
          return new PlatformPayload(internalConstruction, tag, this.byteValue(maxScalarBytes), path);
        case tags.list: {
          const count = this.length(maxContainerEntries);
          const values = [];
          for (let index = 0; index < count; ++index) {
            values.push(this.value(depth + 1, `${path}[${index}]`));
          }
          return new PlatformPayload(internalConstruction, tag, Object.freeze(values), path);
        }
        case tags.object: {
          const count = this.length(maxContainerEntries);
          const fields = new Map();
          for (let index = 0; index < count; ++index) {
            const name = this.string();
            if (fields.has(name)) {
              throw this.malformed("duplicate object key");
            }
            fields.set(name, this.value(depth + 1, childPath(path, name)));
          }
          return new PlatformPayload(internalConstruction, tag, fields, path);
        }
        case tags.bufferReference:
          throw new TypeError("HuxerUI Web bridge does not support BufferReference");
        case tags.externalTexture:
          throw new TypeError("HuxerUI Web PlatformPayload does not support ExternalTexture capabilities");
        case tags.fileReference: {
          if (this.byte() !== 2) {
            throw this.malformed("unknown capability kind");
          }
          const slot = this.uint32();
          if (slot >= this.fileReferences.length) {
            throw this.malformed("missing file reference");
          }
          this.usedFileReferences[slot] = true;
          return new PlatformPayload(internalConstruction, tag, this.fileReferences[slot], path);
        }
        default:
          throw this.malformed("unknown value tag");
      }
    }

    malformed(reason) {
      return new TypeError(`HuxerUI PlatformPayload envelope has ${reason}`);
    }
  }

  const fileReferenceFinalizer =
    typeof FinalizationRegistry === "function"
      ? new FinalizationRegistry((handle) => Module.huxeruiWebFileReferenceRelease(handle))
      : null;

  class FileReference {
    constructor(token, nativeHandle, capabilityKey, source, isFile) {
      const invalidHandle = !Number.isSafeInteger(nativeHandle) || nativeHandle <= 0;
      const invalidCapabilityKey = !Number.isSafeInteger(capabilityKey) || capabilityKey <= 0;
      const invalidSource = source === null || typeof source !== "object";
      if (token !== internalConstruction || invalidHandle || invalidCapabilityKey ||
          invalidSource || typeof isFile !== "boolean") {
        throw new TypeError("HuxerUI FileReference must be created by the platform bridge");
      }
      fileReferenceData.set(this, { nativeHandle, capabilityKey, source, isFile });
      fileReferenceFinalizer?.register(this, nativeHandle, this);
      Object.freeze(this);
    }

    async getFile() {
      const state = requireFileReference(this);
      if (!state.isFile) {
        throw new TypeError("HuxerUI FileReference does not identify a file");
      }
      if (state.source.handle !== null && state.source.handle !== undefined) {
        if (typeof state.source.handle.getFile !== "function") {
          throw new TypeError("HuxerUI FileReference provider cannot resolve a File");
        }
        return await state.source.handle.getFile();
      }
      if (state.source.file !== null && state.source.file !== undefined) {
        return state.source.file;
      }
      throw new TypeError("HuxerUI FileReference provider cannot resolve a File");
    }

    close() {
      const state = fileReferenceData.get(this);
      if (state?.nativeHandle) {
        Module.huxeruiWebFileReferenceRelease(state.nativeHandle);
        state.nativeHandle = 0;
        state.source = null;
        fileReferenceFinalizer?.unregister(this);
      }
    }
  }

  function requireFileReference(reference) {
    if (!(reference instanceof FileReference) || !fileReferenceData.has(reference)) {
      throw new TypeError("HuxerUI platform boundary requires a FileReference value");
    }
    const state = fileReferenceData.get(reference);
    if (state.nativeHandle === 0) {
      throw new TypeError("HuxerUI FileReference is closed");
    }
    return state;
  }

  function createFileReference(nativeHandle, capabilityKey, source, isFile) {
    return new FileReference(internalConstruction, nativeHandle, capabilityKey, source, isFile);
  }

  function retainFileReference(reference) {
    const state = requireFileReference(reference);
    const handle = Module.huxeruiWebFileReferenceRetain(state.nativeHandle);
    if (!Number.isSafeInteger(handle) || handle <= 0) {
      throw new TypeError("HuxerUI FileReference could not be retained");
    }
    return handle;
  }

  class PlatformPayload {
    constructor(token, tag, value, path) {
      if (token !== internalConstruction) {
        throw new TypeError("HuxerUI PlatformPayload values must be created with a factory method");
      }
      payloadData.set(this, { tag, value, path });
      Object.freeze(this);
    }

    static nullValue() {
      return nullPayload;
    }

    static booleanValue(value) {
      if (typeof value !== "boolean") {
        throw new TypeError("HuxerUI PlatformPayload Boolean value must be a boolean");
      }
      return new PlatformPayload(internalConstruction, tags.boolean, value, "$");
    }

    static int64(value) {
      if (typeof value !== "bigint" || value < -(1n << 63n) || value > (1n << 63n) - 1n) {
        throw new RangeError("HuxerUI PlatformPayload Integer value must be a signed 64-bit bigint");
      }
      return new PlatformPayload(internalConstruction, tags.int64, value, "$");
    }

    static doubleValue(value) {
      if (typeof value !== "number" || !Number.isFinite(value)) {
        throw new TypeError("HuxerUI PlatformPayload Double value must be a finite number");
      }
      return new PlatformPayload(internalConstruction, tags.double, Object.is(value, -0) ? 0 : value, "$");
    }

    static string(value) {
      if (typeof value !== "string") {
        throw new TypeError("HuxerUI PlatformPayload String value must be a string");
      }
      requireValidUnicode(value, "string");
      if (textEncoder.encode(value).length > maxScalarBytes) {
        throw new RangeError("HuxerUI PlatformPayload string is too large");
      }
      return new PlatformPayload(internalConstruction, tags.string, value, "$");
    }

    static bytes(value) {
      if (!(value instanceof Uint8Array)) {
        throw new TypeError("HuxerUI PlatformPayload Bytes value must be a Uint8Array");
      }
      if (value.byteLength > maxScalarBytes) {
        throw new RangeError("HuxerUI PlatformPayload byte value is too large");
      }
      return new PlatformPayload(internalConstruction, tags.bytes, value.slice(), "$");
    }

    static list(values) {
      const copy = Array.from(values ?? []);
      if (copy.length > maxContainerEntries) {
        throw new RangeError("HuxerUI PlatformPayload list is too large");
      }
      for (const value of copy) {
        requirePayload(value);
      }
      return new PlatformPayload(internalConstruction, tags.list, Object.freeze(copy), "$");
    }

    static object(fields) {
      let entries;
      if (fields instanceof Map) {
        entries = Array.from(fields.entries());
      } else if (
        fields !== null &&
        typeof fields === "object" &&
        !Array.isArray(fields) &&
        (Object.getPrototypeOf(fields) === Object.prototype || Object.getPrototypeOf(fields) === null)
      ) {
        entries = Object.entries(fields);
      } else {
        throw new TypeError("HuxerUI PlatformPayload Object fields must be a Map or plain object");
      }
      if (entries.length > maxContainerEntries) {
        throw new RangeError("HuxerUI PlatformPayload object is too large");
      }
      const encoded = entries.map(([name, value]) => {
        if (typeof name !== "string") {
          throw new TypeError("HuxerUI PlatformPayload Object key must be a string");
        }
        requireValidUnicode(name, "object key");
        requirePayload(value);
        return { name, value, bytes: textEncoder.encode(name) };
      });
      encoded.sort((left, right) => compareBytes(left.bytes, right.bytes));
      const result = new Map();
      for (const field of encoded) {
        if (result.has(field.name)) {
          throw new TypeError(`HuxerUI PlatformPayload contains duplicate field ${field.name}`);
        }
        result.set(field.name, field.value);
      }
      return new PlatformPayload(internalConstruction, tags.object, result, "$");
    }

    static fileReference(value) {
      requireFileReference(value);
      return new PlatformPayload(internalConstruction, tags.fileReference, value, "$");
    }

    static decode(bytes) {
      return new EnvelopeReader(bytes).read();
    }

    get kind() {
      return kinds[payloadData.get(this).tag];
    }

    isNull() {
      return payloadData.get(this).tag === tags.null;
    }

    requireNull() {
      this._requireKind(tags.null);
    }

    requireBoolean() {
      this._requireKind(tags.boolean);
      return payloadData.get(this).value;
    }

    requireInt64() {
      this._requireKind(tags.int64);
      return payloadData.get(this).value;
    }

    requireDouble() {
      this._requireKind(tags.double);
      return payloadData.get(this).value;
    }

    requireString() {
      this._requireKind(tags.string);
      return payloadData.get(this).value;
    }

    requireBytes() {
      this._requireKind(tags.bytes);
      return payloadData.get(this).value.slice();
    }

    requireFileReference() {
      this._requireKind(tags.fileReference);
      return payloadData.get(this).value;
    }

    requireField(name) {
      const value = this.field(name);
      if (value === undefined) {
        throw new TypeError(`HuxerUI PlatformPayload is missing ${childPath(payloadData.get(this).path, name)}`);
      }
      return value;
    }

    field(name) {
      this._requireKind(tags.object);
      if (typeof name !== "string") {
        throw new TypeError("HuxerUI PlatformPayload field name must be a string");
      }
      const stored = payloadData.get(this);
      const value = stored.value.get(name);
      return value === undefined ? undefined : value._atPath(childPath(stored.path, name));
    }

    fields() {
      this._requireKind(tags.object);
      const stored = payloadData.get(this);
      const result = new Map();
      for (const [name, value] of stored.value) {
        result.set(name, value._atPath(childPath(stored.path, name)));
      }
      return result;
    }

    rejectUnknownFields(names) {
      this._requireKind(tags.object);
      const stored = payloadData.get(this);
      const accepted = new Set(names);
      for (const name of stored.value.keys()) {
        if (!accepted.has(name)) {
          throw new TypeError(`HuxerUI PlatformPayload contains unknown field ${childPath(stored.path, name)}`);
        }
      }
    }

    elements() {
      this._requireKind(tags.list);
      const stored = payloadData.get(this);
      return Object.freeze(stored.value.map((value, index) => value._atPath(`${stored.path}[${index}]`)));
    }

    element(index) {
      this._requireKind(tags.list);
      const stored = payloadData.get(this);
      if (!Number.isSafeInteger(index) || index < 0 || index >= stored.value.length) {
        throw new RangeError(`HuxerUI PlatformPayload element is outside ${stored.path}`);
      }
      return stored.value[index]._atPath(`${stored.path}[${index}]`);
    }

    encode() {
      const writer = new EnvelopeWriter();
      writer.byte(0x48);
      writer.byte(0x55);
      writer.byte(0x58);
      writer.byte(0x50);
      writer.uint16(1);
      writer.uint16(0);
      writer.value(this, 0);
      return writer.finish();
    }

    _atPath(path) {
      const stored = payloadData.get(this);
      return new PlatformPayload(internalConstruction, stored.tag, stored.value, path);
    }

    _requireKind(expected) {
      const stored = payloadData.get(this);
      if (stored.tag !== expected) {
        throw new TypeError(
          `HuxerUI PlatformPayload at ${stored.path} is ${this.kind}, expected ${kinds[expected]}`,
        );
      }
    }
  }

  const nullPayload = new PlatformPayload(internalConstruction, tags.null, null, "$");
  PlatformPayload.Kind = Object.freeze({
    NULL: kinds[tags.null],
    BOOLEAN: kinds[tags.boolean],
    INT64: kinds[tags.int64],
    DOUBLE: kinds[tags.double],
    STRING: kinds[tags.string],
    BYTES: kinds[tags.bytes],
    LIST: kinds[tags.list],
    OBJECT: kinds[tags.object],
    EXTERNAL_TEXTURE: kinds[tags.externalTexture],
    FILE_REFERENCE: kinds[tags.fileReference],
  });
  Object.freeze(FileReference.prototype);
  Object.freeze(FileReference);
  Object.freeze(PlatformPayload.prototype);
  Object.freeze(PlatformPayload);

  function requirePayload(value) {
    if (!(value instanceof PlatformPayload) || !payloadData.has(value)) {
      throw new TypeError("HuxerUI platform boundary requires a PlatformPayload value");
    }
    return value;
  }

  function encodeEnvelope(payload) {
    requirePayload(payload);
    const fileReferences = [];
    const writer = new EnvelopeWriter(fileReferences);
    writer.byte(0x48);
    writer.byte(0x55);
    writer.byte(0x58);
    writer.byte(0x50);
    writer.uint16(1);
    writer.uint16(0);
    writer.value(payload, 0);
    return { bytes: writer.finish(), fileReferences };
  }

  function decodeEnvelope(bytes, fileReferences) {
    return new EnvelopeReader(bytes, fileReferences).read();
  }

  function createEvents(bridgeHandle) {
    const state = { bridgeHandle };
    return Object.freeze({
      emit(event, payload = nullPayload) {
        if (state.bridgeHandle === 0) {
          return;
        }
        if (typeof event !== "string" || event.length === 0) {
          throw new TypeError("HuxerUI platform event name must be a non-empty string");
        }
        requireValidUnicode(event, "event name");
        const result = Module.huxeruiWebPlatformEmit(state.bridgeHandle, event, requirePayload(payload));
        if (result === null) {
          throw new TypeError("HuxerUI platform event contains an invalid payload");
        }
        return result;
      },
      close() {
        if (state.bridgeHandle !== 0) {
          Module.huxeruiWebPlatformReleaseEvent(state.bridgeHandle);
          state.bridgeHandle = 0;
        }
      },
    });
  }

  function createResult(bridgeHandle) {
    const state = { bridgeHandle };
    return Object.freeze({
      complete(payload) {
        if (state.bridgeHandle === 0) {
          return;
        }
        requirePayload(payload);
        if (!Module.huxeruiWebPlatformComplete(state.bridgeHandle, payload)) {
          throw new TypeError("HuxerUI platform result contains an invalid payload");
        }
        state.bridgeHandle = 0;
      },
      fail(code, message, details = nullPayload) {
        if (state.bridgeHandle === 0) {
          return;
        }
        if (typeof code !== "string" || code.length === 0 || typeof message !== "string") {
          throw new TypeError("HuxerUI platform failure requires a non-empty code and a message");
        }
        requireValidUnicode(code, "error code");
        requireValidUnicode(message, "error message");
        requirePayload(details);
        if (!Module.huxeruiWebPlatformFail(state.bridgeHandle, code, message, details)) {
          throw new TypeError("HuxerUI platform failure contains an invalid payload");
        }
        state.bridgeHandle = 0;
      },
      close() {
        if (state.bridgeHandle !== 0) {
          Module.huxeruiWebPlatformReleaseResult(state.bridgeHandle);
          state.bridgeHandle = 0;
        }
      },
    });
  }

  Module.HuxerUI ??= {};
  Module.HuxerUI.FileReference = FileReference;
  Module.HuxerUI.PlatformPayload = PlatformPayload;
  Module.huxerUIWebPlatformBridge = Object.freeze({
    createEvents,
    createFileReference,
    createResult,
    decodeEnvelope,
    encodeEnvelope,
    retainFileReference,
  });
})();
