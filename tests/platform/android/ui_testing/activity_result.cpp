#include <huxerui/android/platform_registry.h>
#include <huxerui/huxerui.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {
using namespace huxerui;

struct WindowFixture {
  PlatformChannel channel;
  PlatformChannel configured;
  std::shared_ptr<FilePicker> picker;
  TaskHandle pick;
  PlatformRequestId request;
  int replies = 0;
  int picks = 0;
  std::int64_t value = 0;
  std::string error;
};

std::unique_ptr<Application> application;
TaskScope tasks;
PlatformChannel application_channel;
bool application_replied = false;
std::vector<std::unique_ptr<WindowFixture>> windows;

template <class Action> void Check(JNIEnv* environment, Action action) {
  try {
    action();
  } catch (const std::exception& error) {
    android::LocalRef<jclass> type(environment, environment->FindClass("java/lang/AssertionError"));
    environment->ThrowNew(type.Get(), error.what());
  }
}
} // namespace

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIActivityResultTest_prepareNative(JNIEnv* environment, jclass) {
  Check(environment, [] {
    application = std::make_unique<Application>([] { return View{}; }, AppOptions{
        .application_hooks = {[](ApplicationContext& context) {
          tasks = UseApplicationTaskScope();
          android::JavaPlatformModuleFactory<PlatformChannel> factory;
          factory.class_name = "org.huxerui.HuxerUIActivityResultTest$Factory";
          factory.create = [](PlatformChannel channel) { return channel; };
          context.RegisterPlatformModule<PlatformChannel>("test/application", std::move(factory));
          context.RegisterPlatformModule<PlatformChannel>("test/window",
              android::JavaWindowPlatformModuleFactory<PlatformChannel>{
                  .class_name = "org.huxerui.HuxerUIActivityResultTest$Factory",
                  .create = [](PlatformChannel channel) { return channel; },
              });
          context.RegisterPlatformModule<PlatformChannel, std::int64_t>("test/configured-window",
              android::JavaWindowPlatformModuleFactory<PlatformChannel, std::int64_t>{
                  .class_name = "org.huxerui.HuxerUIActivityResultTest$Factory",
                  .create = [](PlatformChannel channel) { return channel; },
              });
        }},
        .window_hooks = {[](WindowContext&) {
          auto window = std::make_unique<WindowFixture>();
          window->channel = OpenPlatformModule<PlatformChannel>("test/window");
          window->configured = OpenPlatformModule<PlatformChannel>("test/configured-window", std::int64_t{7});
          window->picker = UseService<FilePicker>();
          windows.push_back(std::move(window));
        }},
    });
  });
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIActivityResultTest_verifyApplicationNative(
    JNIEnv* environment, jclass) {
  Check(environment, [] {
    bool rejected = false;
    try { OpenPlatformModule<PlatformChannel>("test/window"); }
    catch (const std::logic_error&) { rejected = true; }
    if (!rejected) throw std::runtime_error("A window factory must reject windowless access");
    application_channel = OpenPlatformModule<PlatformChannel>("test/application");
    application_channel.Invoke<std::int64_t>("identity", std::monostate{}, [](PlatformResult<std::int64_t> result) {
      application_replied = std::get_if<std::int64_t>(&result) && std::get<std::int64_t>(result) == 5;
      application_channel.Close();
    });
  });
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIActivityResultTest_invokeNative(
    JNIEnv* environment, jclass, jint index, jboolean configured, jstring method) {
  Check(environment, [&] {
    auto& window = *windows.at(index);
    const auto& channel = configured ? window.configured : window.channel;
    window.request = channel.Invoke<std::int64_t>(android::JavaStringToUtf8(environment, method), std::monostate{},
        [&window](PlatformResult<std::int64_t> result) {
          ++window.replies;
          if (const auto* error = std::get_if<PlatformError>(&result)) {
            window.error = error->message;
          } else {
            window.value = std::get<std::int64_t>(result);
          }
        });
  });
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIActivityResultTest_pickNative(
    JNIEnv* environment, jclass, jint index) {
  Check(environment, [&] {
    auto& window = *windows.at(index);
    window.pick = tasks.Launch([&window]() -> Task<void> {
      const auto result = co_await window.picker->OpenFileAsync();
      if (result) window.error = "A canceled picker must not return a file";
      ++window.picks;
    });
  });
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIActivityResultTest_cancelNative(
    JNIEnv* environment, jclass, jint index, jboolean picker) {
  Check(environment, [&] {
    auto& window = *windows.at(index);
    if (picker) window.pick.Cancel();
    else window.channel.Cancel(window.request);
  });
}

extern "C" JNIEXPORT jlong JNICALL Java_org_huxerui_HuxerUIActivityResultTest_statusNative(
    JNIEnv* environment, jclass, jint index, jint field) {
  jlong result = 0;
  Check(environment, [&] {
    if (index == -1) { result = application_replied; return; }
    const auto& window = *windows.at(index);
    if (!window.error.empty()) throw std::runtime_error(window.error);
    result = field == 0 ? window.replies : field == 1 ? window.picks : window.value;
  });
  return result;
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIActivityResultTest_closeNative(JNIEnv*, jclass) {
  application_channel.Close();
  for (const auto& window : windows) {
    window->pick.Cancel();
    window->channel.Close();
    window->configured.Close();
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIActivityResultTest_finishNative(JNIEnv*, jclass) {
  windows.clear();
  application_channel = {};
  application_replied = false;
  tasks = {};
  application.reset();
}
