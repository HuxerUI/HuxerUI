#import <UIKit/UIKit.h>

#include <memory>
#include <utility>

#include <huxerui/ios/platform_registry.h>
#include <huxerui/root.h>

namespace {

struct DirectViewProperties {
  int value = 0;

  bool operator==(const DirectViewProperties&) const = default;
};

struct DirectViewInstance {
  __strong UIView* view = nil;
};

void RegisterDirectFactories(huxerui::RootContext& root) {
  huxerui::ios::PlatformModuleFactory<int> module_factory{
      .create = [](huxerui::PlatformAdapter&, UIViewController*) { return 1; },
  };
  root.RegisterPlatformModule<int>("test/DirectModule", std::move(module_factory));

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

void HuxerUITestIOSDirectPlatformFactories(huxerui::RootContext& root) {
  RegisterDirectFactories(root);
}
