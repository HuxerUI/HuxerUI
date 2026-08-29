# Platform Modules

Use a PlatformModule for a non-visual capability whose implementation depends on the current platform.
Do not use one for portable C++ services, embedded controls, or frame production that fits `ExternalTexture`.

## Typed C++ contract

The library defines the Module and optional Options types.
Register one exact factory from a RootHook:

```cpp
root.RegisterPlatformModule<AudioPlayer, AudioPlayerOptions>(
    "audio/Player",
    [](PlatformAdapter& adapter, const AudioPlayerOptions& options) {
      return CreateAudioPlayer(adapter, options);
    }
);
```

Every direct C++ factory receives the owning surface's non-owning `PlatformAdapter&`, followed by the exact Options type when present, and returns the exact Module type.
The adapter provides the existing host capabilities without introducing a second factory context; it remains owned by the surface and must not be retained beyond that surface's lifetime.
Direct C++ factories do not encode values into `PlatformPayload`.
Registration names are nonempty case-sensitive UTF-8 identities and do not require `/`.

Open a root-owned Module with `RootContext::OpenPlatformModule<Module>()`, then optionally expose it through `root.Provide()`.
For component lifetime, call the typed free `OpenPlatformModule<Module>()` only from committed `Lifecycle` setup and release the returned Module from cleanup.
There is no generic `UsePlatformModule`, public registry accessor, mandatory service base, or dynamic method list.

## Cross-language implementations

`PlatformPayload` and `PlatformChannel` belong only at a C++/platform-language boundary.
Keep them behind the library's typed Module facade.
A structured boundary type owns its static `Encode(const T&)` or `Decode(const PlatformPayload&)` operation; direct C++ implementations do not call those operations.

Android currently provides `android::JavaPlatformModuleFactory<Module, Options>` for Java or Kotlin implementations.
Its `connect` callback receives one framework-owned `PlatformChannel` and returns the library's exact Module type.
The Java implementation receives one `HuxerUIPlatformChannel.Events` emitter and uses the SDK `PlatformPayload` value; it does not declare one JNI callback per event.

Web provides `web::JavaScriptPlatformModuleFactory<Module, Options>` for a linked JavaScript structural factory.
The RootHook supplies its actual `emscripten::val`, and `connect` wraps the framework-owned `PlatformChannel` in the library's exact Module type.
JavaScript receives immutable `Module.HuxerUI.PlatformPayload` values and one framework-owned events endpoint; it does not require inheritance or a second name registry.

iOS and macOS expose their Objective-C/Swift contracts through the pure Objective-C Clang module `HuxerUIPlatform`.
An Objective-C or Swift implementation conforms to `UIKitPlatformModuleFactory` or `AppKitPlatformModuleFactory` and returns a `PlatformModule` instance.
The library's Objective-C++ RootHook passes the actual factory object to the matching typed adapter:

```cpp
ios::ObjectiveCPlatformModuleFactory<std::shared_ptr<AudioPlayer>, AudioPlayerOptions> factory{
    .factory = actual_factory,
    .connect = [](PlatformChannel channel) {
      return std::make_shared<ChannelAudioPlayer>(std::move(channel));
    },
};
root.RegisterPlatformModule<std::shared_ptr<AudioPlayer>, AudioPlayerOptions>(
    "audio/Player",
    std::move(factory)
);
```

Use `macos::ObjectiveCPlatformModuleFactory` for AppKit.
The framework does not look up a class name, generate a registrant, or require the application delegate to register the factory again.
Direct Objective-C++ implementations remain strongly typed through `ios::PlatformModuleFactory` or `macos::PlatformModuleFactory` and do not use payloads.

`PlatformChannel::Invoke` returns a request identity before scheduling the platform invocation on the owning UI thread.
Use `Invoke<Result>(method, completion)` when both the argument and result are Null; the typed C++ completion receives `PlatformResult<std::monostate>`.
Results and events return asynchronously through that dispatcher.
`Cancel` and `Close` invalidate C++ delivery immediately; queued invocations are skipped, in-flight cancellation runs before disposal, and late results or events are ignored.
The channel is a reusable transport convenience, not a PlatformModule base class or the Module API exposed to application UI.
Apple factory creation, invocation, cancellation, and disposal run on the UIKit or AppKit main thread; results and events may originate on any queue and resume through the owning surface dispatcher.

## Review points

- one explicit RootHook registration and no platform-host registration path;
- exact Module and Options types on the direct C++ path;
- payload conversion confined to an actual language boundary;
- deterministic ownership, cancellation, and disposal;
- no string methods, payload maps, or channels exposed through application components;
- no PlatformModule where portable C++ already owns the capability.
