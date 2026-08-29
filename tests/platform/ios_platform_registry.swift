import HuxerUIPlatform
import UIKit

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

final class TestPlatformModuleFactory: NSObject, UIKitPlatformModuleFactory {
  func create(with viewController: UIViewController, options: PlatformPayload,
              events: PlatformEventEmitter) -> PlatformModule {
    events.emit("created", payload: options)
    return TestPlatformModule()
  }
}

final class TestPlatformView: NSObject, UIKitPlatformView {
  let view = UIView()

  func dispose() {}
}

final class TestPlatformViewFactory: NSObject, UIKitPlatformViewFactory {
  func create(with viewController: UIViewController, properties: PlatformPayload,
              events: PlatformEventEmitter) -> UIKitPlatformView {
    events.emit("created", payload: properties)
    return TestPlatformView()
  }
}

let payload: PlatformPayload = .object([
  "enabled": .boolean(true),
  "count": .integer(3),
])
payload.validate(fields: ["enabled", "count"])

let textureSource = ExternalTextureSource(intrinsicSize: CGSize(width: 16, height: 9))
_ = textureSource.texture
textureSource.finish()
