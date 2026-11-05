#import <UIKit/UIKit.h>

#include <memory>
#include <utility>

#include <huxerui/ios/platform_registry.h>
#include <huxerui/app.h>

namespace {

struct DirectViewProperties {
  int value = 0;

  bool operator==(const DirectViewProperties&) const = default;
};

struct DirectViewInstance {
  __strong UIView* view = nil;
};

void RegisterDirectFactories(huxerui::ApplicationContext& root) {
  huxerui::ios::WindowPlatformModuleFactory<int> module_factory{
      .create = [](huxerui::UiWindow&, UIViewController*) { return 1; },
  };
  root.RegisterPlatformModule<int>("test/DirectModule", std::move(module_factory));

  root.RegisterPlatformModule<int>("test/ApplicationModule", [](huxerui::Runtime&) { return 2; });

  huxerui::ios::PlatformViewFactory<DirectViewProperties, DirectViewInstance> view_factory{
      .create = [](UIViewController*, const DirectViewProperties&, huxerui::PlatformEventEmitter) {
        return std::make_shared<DirectViewInstance>([UIView new]);
      },
      .view = [](const std::shared_ptr<DirectViewInstance>& instance) { return instance->view; },
      .update = [](DirectViewInstance&, const DirectViewProperties&) {},
      .dispose = [](DirectViewInstance&) {},
  };
  root.RegisterPlatformView<DirectViewProperties>("test/DirectView", std::move(view_factory));
}

} // namespace

void HuxerUITestIOSDirectPlatformFactories(huxerui::ApplicationContext& root) {
  RegisterDirectFactories(root);
}

void HuxerUITestIOSObjectiveCModuleFactories(huxerui::ApplicationContext& root,
                                            id<HUXPlatformModuleFactory> application_factory,
                                            id<HUXUIKitWindowPlatformModuleFactory> window_factory) {
  using namespace huxerui;
  root.RegisterPlatformModule<PlatformChannel>("test/ApplicationChannel",
      ios::ObjectiveCPlatformModuleFactory<PlatformChannel>{
          .factory = application_factory, .create = [](PlatformChannel channel) { return channel; }});
  root.RegisterPlatformModule<PlatformChannel, PlatformPayload>("test/ApplicationOptions",
      ios::ObjectiveCPlatformModuleFactory<PlatformChannel, PlatformPayload>{
          .factory = application_factory, .create = [](PlatformChannel channel) { return channel; }});
  root.RegisterPlatformModule<PlatformChannel>("test/WindowChannel",
      ios::ObjectiveCWindowPlatformModuleFactory<PlatformChannel>{
          .factory = window_factory, .create = [](PlatformChannel channel) { return channel; }});
  root.RegisterPlatformModule<PlatformChannel, PlatformPayload>("test/WindowOptions",
      ios::ObjectiveCWindowPlatformModuleFactory<PlatformChannel, PlatformPayload>{
          .factory = window_factory, .create = [](PlatformChannel channel) { return channel; }});
}
