# Platform Modules

Use `PlatformModule` for a non-visual platform capability. Do not use it for ordinary portable C++ services, embedded controls, or rendered media.

## Application-side boundary

`PlatformPayload` supports null, boolean, signed 64-bit integer, double, string, bytes, list, object, and `ExternalTexture`. Construct its owned byte value with `Bytes`; `AsBytes()` returns a borrowed `std::span<const std::byte>` into the payload. Keep raw payload conversion at a typed service boundary instead of spreading type strings and map keys through UI code.

Define typed method keys with:

- `Request` and `Result` C++ object types, including scalar types such as `bool`;
- static `Name` convertible to `std::string_view`;
- `Encode(const Request&) -> PlatformPayload`;
- `Decode(const PlatformPayload&) -> Result`.

Define typed event keys with an Event `Signature`, static `Name`, and `Decode` compatible with that signature.

`PlatformModules::Open(type, options)` returns a move-only `PlatformInstance`. `Call<Method>` returns a request ID and completes with `PlatformResult<Result>`. Register event handlers with `On<Key>`, cancel outstanding calls with `Cancel`, and release the instance with `Close` or destruction.

Wrap the instance in an app-side service installed by a `RootHook`, then expose the service through `RootContext::Provide` and `UseService<Service>()`. UI components should depend on that typed service, not on `PlatformPayload`.

## Registration

`RootContext::Modules()` owns registrations for the window runtime. A generic `huxerui::PlatformModuleFactory` contains:

- `create(options, event_sink) -> Instance`;
- `Instance::call(method, arguments, result_sink) -> cancellation callback`;
- `Instance::dispose()`.

Android additionally exposes `huxerui::android::PlatformModuleFactory`, whose create callback receives `JNIEnv*` and the host Java object. Use installed platform-specific public headers when they exist. Do not assume an uninstalled platform has the same factory signature.

Platform completions/events must return safely to the HuxerUI UI thread according to the platform factory contract. Complete each request at most once. Cancellation and dispose must be idempotent and must not invoke callbacks into destroyed UI.

## Timer service pattern

Keep the public component layer typed:

```cpp
struct StartTimer {
  struct Request {
    std::int64_t milliseconds;
  };
  using Result = bool;
  static constexpr std::string_view Name = "start";

  static PlatformPayload Encode(const Request& request) {
    return PlatformPayload::Object{{"milliseconds", request.milliseconds}};
  }

  static bool Decode(const PlatformPayload& payload) {
    return payload.AsBoolean();
  }
};
```

The platform implementation owns the actual scheduler and cancellation handle. The app-side service owns `PlatformInstance`, calls `Call<StartTimer>`, translates `PlatformError`, and closes on teardown. Avoid implementing a timer module when `TaskScope` plus `Delay` already satisfies a portable app need.

## External texture pattern

A capture, camera, or decoder module can return an `ExternalTexture` inside `PlatformPayload`. Decode it with `AsExternalTexture()` and display it with `Image(texture)`. The platform producer owns its `ExternalTextureSource` and calls `Finish` on shutdown. This separates non-visual control calls from frame rendering.

## Review points

- stable type/method/event names contained in one service;
- complete payload validation and error mapping;
- request cancellation and instance disposal;
- UI-thread delivery and component-lifetime safety;
- no `PlatformView` used for a non-visual service;
- no `PlatformModule` used where portable C++ is sufficient.
