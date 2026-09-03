import AppKit
import HuxerUIPlatform

final class TestPlatformModule: NSObject, PlatformModule {
  func invoke(_ method: String, arguments: PlatformPayload, result: PlatformResult) -> PlatformCancellation? {
    result.complete(.object([
      "method": .string(method),
      "arguments": arguments,
    ]))
    return nil
  }

  func dispose() {}
}

final class TestPlatformModuleFactory: NSObject, AppKitPlatformModuleFactory {
  func create(with window: NSWindow, options: PlatformPayload,
              events: PlatformEventEmitter) -> PlatformModule {
    _ = events.emit("created", payload: options)
    return TestPlatformModule()
  }
}

final class TestPlatformView: NSObject, AppKitPlatformView {
  let view = NSView()

  func dispose() {}
}

final class TestPlatformViewFactory: NSObject, AppKitPlatformViewFactory {
  func create(with window: NSWindow, properties: PlatformPayload,
              events: PlatformEventEmitter) -> AppKitPlatformView {
    _ = events.emit("created", payload: properties)
    return TestPlatformView()
  }
}

let payload: PlatformPayload = .object([
  "enabled": .boolean(true),
  "count": .integer(3),
])
payload.validate(fields: ["enabled", "count"])

let texture = PixelBufferTexture(intrinsicSize: CGSize(width: 16, height: 9))
texture.finish()

let metalTexture = MetalTexture(intrinsicSize: CGSize(width: 16, height: 9))
let metalOrigin: MetalTexture.Origin = .topLeft
let metalAlpha: MetalTexture.Alpha = .premultiplied
_ = (metalOrigin, metalAlpha)
metalTexture.finish()
