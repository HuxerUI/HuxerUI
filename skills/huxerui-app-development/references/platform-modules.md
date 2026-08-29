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

Apple Objective-C/Swift and Web JavaScript common adapters are future work.
Current Apple and Web libraries use direct C++/Objective-C++/Emscripten factories or a library-owned bridge while retaining the same RootHook registration and typed public Module contract.

`PlatformChannel::Invoke` returns a request identity before scheduling the platform invocation on the owning UI thread.
Use `Invoke<Result>(method, completion)` when both the argument and result are Null; the typed C++ completion receives `PlatformResult<std::monostate>`.
Results and events return asynchronously through that dispatcher.
`Cancel` and `Close` invalidate C++ delivery immediately; queued invocations are skipped, in-flight cancellation runs before disposal, and late results or events are ignored.
The channel is a reusable transport convenience, not a PlatformModule base class or the Module API exposed to application UI.

## Review points

- one explicit RootHook registration and no platform-host registration path;
- exact Module and Options types on the direct C++ path;
- payload conversion confined to an actual language boundary;
- deterministic ownership, cancellation, and disposal;
- no string methods, payload maps, or channels exposed through application components;
- no PlatformModule where portable C++ already owns the capability.
