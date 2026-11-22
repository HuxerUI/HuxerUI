import { common } from '@kit.AbilityKit';
import { Content, UIContext, window } from '@kit.ArkUI';

/** Exact kinds used by a HuxerUI platform payload. */
export enum HuxerUIPlatformPayloadKind {
  Null,
  Boolean,
  Integer,
  Double,
  String,
  Bytes,
  List,
  Object,
  ExternalTexture,
  FileReference,
  BufferReference,
}

/** A retained external texture. `close` releases this handle's share. */
export interface HuxerUIExternalTexture {
  close(): void;
}

/** A retained file capability. Keep the reference alive while using its provider URI. */
export interface HuxerUIFileReference {
  /** Returns the provider URI if it can be opened by an ArkTS system API. */
  uri(): string | undefined;
  close(): void;
}

/** Retains an address-stable byte range. Coordinate producer writes with all readers. */
export interface HuxerUIBufferReference {
  byteLength(): number;
  /** Returns a snapshot of the current bytes. */
  copyBytes(): ArrayBuffer;
  /** Writes the range beginning at sourceOffset into the supplied destination. */
  readInto(destination: ArrayBuffer, sourceOffset: number): void;
  /** Retains a subrange of the same storage. */
  slice(offset: number, size: number): HuxerUIBufferReference;
  close(): void;
}

/** A key and its immutable payload value. */
export interface HuxerUIPlatformPayloadField {
  readonly name: string;
  readonly value: HuxerUIPlatformPayload;
}

/** An immutable value shared with C++. Accessors require an exact kind and report errors at its field path. */
export interface HuxerUIPlatformPayload {
  kind(): HuxerUIPlatformPayloadKind;
  isNull(): boolean;
  requireNull(): void;
  requireBoolean(): boolean;
  requireInt64(): bigint;
  requireDouble(): number;
  requireString(): string;
  requireBytes(): ArrayBuffer;
  requireExternalTexture(): HuxerUIExternalTexture;
  requireFileReference(): HuxerUIFileReference;
  requireBufferReference(): HuxerUIBufferReference;
  field(name: string): HuxerUIPlatformPayload | undefined;
  requireField(name: string): HuxerUIPlatformPayload;
  fields(): HuxerUIPlatformPayloadField[];
  rejectUnknownFields(names: string[]): void;
  element(index: number): HuxerUIPlatformPayload;
  elements(): HuxerUIPlatformPayload[];
}

/** Creates immutable payload values for requests, replies, and events. Byte and collection inputs are copied. */
export interface HuxerUIPlatformPayloadFactory {
  nullValue(): HuxerUIPlatformPayload;
  booleanValue(value: boolean): HuxerUIPlatformPayload;
  int64(value: bigint): HuxerUIPlatformPayload;
  doubleValue(value: number): HuxerUIPlatformPayload;
  string(value: string): HuxerUIPlatformPayload;
  bytes(value: ArrayBuffer): HuxerUIPlatformPayload;
  list(values: HuxerUIPlatformPayload[]): HuxerUIPlatformPayload;
  object(fields: HuxerUIPlatformPayloadField[]): HuxerUIPlatformPayload;
  externalTexture(value: HuxerUIExternalTexture): HuxerUIPlatformPayload;
  fileReference(value: HuxerUIFileReference): HuxerUIPlatformPayload;
  bufferReference(value: HuxerUIBufferReference): HuxerUIPlatformPayload;
  /** Retains a file whose URI is already accessible to the application. Returns undefined if it cannot be opened. */
  fileFromUri(uri: string, writable?: boolean): HuxerUIFileReference | undefined;
  /** Copies bytes into address-stable storage owned by the resulting reference. */
  bufferFromBytes(value: ArrayBuffer): HuxerUIBufferReference;
}

/** A result endpoint for one request. The first completion is delivered asynchronously. */
export interface HuxerUIPlatformResult {
  complete(value: HuxerUIPlatformPayload): void;
  fail(code: string, message: string, details?: HuxerUIPlatformPayload): void;
}

/** Emits a named event. A mounted PlatformView may return a synchronous response. */
export interface HuxerUIPlatformEvents {
  /** Creation-time view notifications wait until the view commits. Only committed views return a decision. */
  emit(name: string, value: HuxerUIPlatformPayload): HuxerUIPlatformPayload | undefined;
}

/** Implements one channel instance on the application thread. Return a cancellation function for pending work. */
export interface HuxerUIPlatformModule {
  invoke(method: string, args: HuxerUIPlatformPayload, result: HuxerUIPlatformResult): (() => void) | void;
  /** Releases instance resources after pending requests have been cancelled. */
  dispose(): void;
}

/** Creates an application-scoped module, including when the ability has no attached window. */
export interface HuxerUIPlatformModuleFactory {
  create(context: common.UIAbilityContext, options: HuxerUIPlatformPayload, events: HuxerUIPlatformEvents,
    payloads: HuxerUIPlatformPayloadFactory): HuxerUIPlatformModule;
}

/** Creates a module bound to the lifetime of the originating window attachment. */
export interface HuxerUIWindowPlatformModuleFactory {
  create(owner: window.Window, context: UIContext, options: HuxerUIPlatformPayload, events: HuxerUIPlatformEvents,
    payloads: HuxerUIPlatformPayloadFactory): HuxerUIPlatformModule;
}

/** Owns the detached ComponentContent and receives complete property revisions after mounting. */
export interface HuxerUIPlatformView {
  readonly content: Content;
  update?(properties: HuxerUIPlatformPayload): void;
  invoke?(method: string, args: HuxerUIPlatformPayload, result: HuxerUIPlatformResult): (() => void) | void;
  /** Releases ComponentContent after HuxerUI has removed it from the ArkUI hierarchy. */
  dispose(): void;
}

/** Creates a view for the original ArkUI attachment. HuxerUI parents and removes the returned content. */
export interface HuxerUIPlatformViewFactory {
  create(context: UIContext, properties: HuxerUIPlatformPayload, events: HuxerUIPlatformEvents,
    payloads: HuxerUIPlatformPayloadFactory): HuxerUIPlatformView;
}

/** Resolves factory objects selected by the application's C++ factory catalog. */
export interface HuxerUIPlatformExtensions {
  module?(name: string): HuxerUIPlatformModuleFactory | undefined;
  windowModule?(name: string): HuxerUIWindowPlatformModuleFactory | undefined;
  view?(name: string): HuxerUIPlatformViewFactory | undefined;
}
