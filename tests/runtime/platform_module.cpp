#include "runtime_test_support.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "application/platform_registry_internal.h"

namespace huxerui::test {
namespace {

struct TestModuleOptions {
  int value = 0;

  bool operator==(const TestModuleOptions&) const = default;
};

class TestModule final {
public:
  TestModule(int value, std::shared_ptr<int> disposals) : value_(value), disposals_(std::move(disposals)) {}

  TestModule(const TestModule&) = delete;
  TestModule& operator=(const TestModule&) = delete;
  TestModule(TestModule&&) noexcept = default;
  TestModule& operator=(TestModule&&) noexcept = default;

  ~TestModule() {
    if (disposals_) {
      ++*disposals_;
    }
  }

  [[nodiscard]] int Value() const noexcept {
    return value_;
  }

private:
  int value_ = 0;
  std::shared_ptr<int> disposals_;
};

struct DummyPlatformViewFactory {
private:
  detail::PlatformViewFactoryRegistration Erase(huxerui::UiWindow& ui_window) && {
    static_cast<void>(ui_window);
    return detail::MakePlatformViewFactoryRegistration(std::make_shared<int>(1));
  }

  friend class detail::PlatformRegistry;
};

struct WindowPreparedPlatformViewFactory {
  huxerui::UiWindow** prepared = nullptr;

private:
  detail::PlatformViewFactoryRegistration Erase(huxerui::UiWindow& ui_window) && {
    *prepared = &ui_window;
    return detail::MakePlatformViewFactoryRegistration(std::make_shared<int>(2));
  }

  friend class detail::PlatformRegistry;
};

View PlatformModuleApp() {
  return Text("module");
}

TEST_CASE("PlatformRegistryOpensExactMoveOnlyModuleFromRoot") {
  TestPlatform platform;
  const auto disposals = std::make_shared<int>(0);
  std::unique_ptr<TestModule> opened;
  AppOptions options{.show_debug_overlay = false};
  options.application_hooks.push_back([&](ApplicationContext& root) {
    root.RegisterPlatformModule<TestModule, TestModuleOptions>(
        "test/Module", [disposals](Runtime&, const TestModuleOptions& options) {
          return TestModule(options.value, disposals);
        });
    opened =
        std::make_unique<TestModule>(OpenPlatformModule<TestModule>("test/Module", TestModuleOptions{.value = 7}));
  });

  {
    UiWindow runtime(PlatformModuleApp, platform, std::move(options));
    REQUIRE(opened != nullptr);
    REQUIRE(opened->Value() == 7);
    REQUIRE(*disposals == 0);
  }
  opened.reset();
  REQUIRE(*disposals == 1);
}

std::shared_ptr<int> lifecycle_module_value;
std::shared_ptr<int> lifecycle_cleanups;

View LifecyclePlatformModuleApp() {
  Lifecycle([] {
    lifecycle_module_value =
        OpenPlatformModule<std::shared_ptr<int>>("test/LifecycleModule", TestModuleOptions{.value = 11});
    return [] {
      lifecycle_module_value.reset();
      ++*lifecycle_cleanups;
    };
  });
  return Text("lifecycle module");
}

TEST_CASE("OpenPlatformModuleUsesOnlyCommittedLifecycleContext") {
  REQUIRE_THROWS_AS(OpenPlatformModule<std::shared_ptr<int>>("test/LifecycleModule", TestModuleOptions{}),
                    std::logic_error);

  TestPlatform platform;
  lifecycle_module_value.reset();
  lifecycle_cleanups = std::make_shared<int>(0);
  AppOptions options{.show_debug_overlay = false};
  options.application_hooks.push_back([](ApplicationContext& root) {
    root.RegisterPlatformModule<std::shared_ptr<int>, TestModuleOptions>(
        "test/LifecycleModule",
        [](huxerui::UiWindow&, const TestModuleOptions& options) { return std::make_shared<int>(options.value); });
  });

  {
    UiWindow runtime(LifecyclePlatformModuleApp, platform, std::move(options));
    runtime.BuildFrame();
    REQUIRE(lifecycle_module_value != nullptr);
    REQUIRE(*lifecycle_module_value == 11);
    REQUIRE(*lifecycle_cleanups == 0);
  }
  REQUIRE(lifecycle_module_value == nullptr);
  REQUIRE(*lifecycle_cleanups == 1);
}

TEST_CASE("PlatformRegistryPassesOwningWindowToModuleFactory") {
  TestPlatform platform;
  huxerui::UiWindow* received = nullptr;
  AppOptions options{.show_debug_overlay = false};
  options.application_hooks.push_back([&](ApplicationContext& root) {
    root.RegisterPlatformModule<std::shared_ptr<int>>("test/Window", [&received](huxerui::UiWindow& ui_window) {
      received = &ui_window;
      return std::make_shared<int>(3);
    });
  });
  options.window_hooks.push_back([](WindowContext& root) {
    REQUIRE(*OpenPlatformModule<std::shared_ptr<int>>("test/Window") == 3);
  });
  UiWindow runtime(PlatformModuleApp, platform, std::move(options));
  REQUIRE(received == &platform);
}

TEST_CASE("PlatformRegistryBindsViewFactoriesOnlyWhenCreatingAnInstance") {
  TestPlatform platform;
  TestPlatform other_ui;
  huxerui::UiWindow* view_window = nullptr;
  detail::PlatformRegistry registry(platform);
  registry.RegisterView<int>("test/PreparedView", WindowPreparedPlatformViewFactory{&view_window});
  registry.Freeze();
  REQUIRE(view_window == nullptr);
  REQUIRE(*registry.FindView<int>(other_ui, "test/PreparedView", typeid(int), typeid(void)) == 2);
  REQUIRE(view_window == &other_ui);
}
TEST_CASE("PlatformRegistryRejectsDuplicateNamesAcrossRegistrationKinds") {
  TestPlatform platform;
  AppOptions options{.show_debug_overlay = false};
  options.application_hooks.push_back([](ApplicationContext& root) {
    root.RegisterPlatformModule<std::shared_ptr<int>>("test/Duplicate",
                                                      [](huxerui::UiWindow&) { return std::make_shared<int>(1); });
    root.RegisterPlatformView<int>("test/Duplicate", DummyPlatformViewFactory{});
  });
  REQUIRE_THROWS_AS(UiWindow(PlatformModuleApp, platform, std::move(options)), std::logic_error);
}

TEST_CASE("PlatformRegistryRejectsWrongModuleAndOptionsTypes") {
  TestPlatform platform;
  AppOptions options{.show_debug_overlay = false};
  options.application_hooks.push_back([](ApplicationContext& root) {
    root.RegisterPlatformModule<std::shared_ptr<int>, TestModuleOptions>(
        "test/Typed",
        [](huxerui::UiWindow&, const TestModuleOptions& options) { return std::make_shared<int>(options.value); });
    REQUIRE_THROWS_AS(OpenPlatformModule<std::shared_ptr<std::string>>("test/Typed"), std::logic_error);
    REQUIRE_THROWS_AS(OpenPlatformModule<std::shared_ptr<int>>("test/Typed", std::string("wrong")),
                      std::logic_error);
  });
  UiWindow runtime(PlatformModuleApp, platform, std::move(options));
}

TEST_CASE("PlatformRegistryRejectsEmptyMissingAndInvalidUtf8Names") {
  TestPlatform platform;
  AppOptions options{.show_debug_overlay = false};
  options.application_hooks.push_back([](ApplicationContext& root) {
    REQUIRE_THROWS_AS(root.RegisterPlatformModule<std::shared_ptr<int>>(
                          "", [](huxerui::UiWindow&) { return std::make_shared<int>(1); }),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(
        root.RegisterPlatformModule<std::shared_ptr<int>>(std::string(1, static_cast<char>(0xFF)),
                                                          [](huxerui::UiWindow&) { return std::make_shared<int>(1); }),
        std::invalid_argument);
    REQUIRE_THROWS_AS(OpenPlatformModule<std::shared_ptr<int>>("test/Missing"), std::logic_error);
  });
  UiWindow runtime(PlatformModuleApp, platform, std::move(options));
}

TEST_CASE("PlatformChannelOwnsInvocationEventAndDisposalDelivery") {
  TestPlatform platform;
  Application application(PlatformModuleApp, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, platform);
  detail::PlatformChannelEndpoint endpoint = detail::MakePlatformChannelEndpoint(static_cast<huxerui::Runtime&>(platform));
  int invocations = 0;
  int cancellations = 0;
  int disposals = 0;
  endpoint.Connect({
      .invoke =
          [&](std::string method, PlatformPayload arguments,
              std::function<void(PlatformResult<PlatformPayload>)> completion) {
            REQUIRE(method == "read");
            REQUIRE(arguments.AsInteger() == 4);
            ++invocations;
            completion(PlatformPayload(8));
            return [&] { ++cancellations; };
          },
      .dispose = [&] { ++disposals; },
  });

  PlatformChannel channel = endpoint.Channel();
  std::optional<std::int64_t> result;
  const PlatformRequestId completed =
      channel.Invoke("read", PlatformPayload(4), [&](PlatformResult<PlatformPayload> value) {
        result = std::get<PlatformPayload>(value).AsInteger();
      });
  REQUIRE(invocations == 0);
  REQUIRE_FALSE(result.has_value());
  platform.RunPlatformModuleTasks();
  REQUIRE(invocations == 1);
  REQUIRE(result == 8);
  REQUIRE_FALSE(channel.Cancel(completed));

  std::optional<std::int64_t> event;
  channel.On("changed", [&](const PlatformPayload& value) { event = value.AsInteger(); });
  endpoint.Events().Emit("changed", PlatformPayload(9));
  REQUIRE_FALSE(event.has_value());
  platform.RunPlatformModuleTasks();
  REQUIRE(event == 9);

  channel.Close();
  REQUIRE(disposals == 0);
  REQUIRE(cancellations == 0);
  REQUIRE_FALSE(channel.IsOpen());
  platform.RunPlatformModuleTasks();
  REQUIRE(disposals == 1);
}

TEST_CASE("PlatformChannelMapsTypedNoValueCallsToNullPayloads") {
  TestPlatform platform;
  Application application(PlatformModuleApp, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, platform);
  detail::PlatformChannelEndpoint endpoint = detail::MakePlatformChannelEndpoint(static_cast<huxerui::Runtime&>(platform));
  endpoint.Connect({
      .invoke =
          [](std::string method, PlatformPayload arguments,
             std::function<void(PlatformResult<PlatformPayload>)> completion) {
            REQUIRE(arguments.IsNull());
            if (method == "stop") {
              completion(PlatformPayload{});
            } else {
              completion(PlatformPayload(1));
            }
            return std::function<void()>{};
          },
  });

  PlatformChannel channel = endpoint.Channel();
  bool stopped = false;
  static_cast<void>(channel.Invoke<std::monostate>("stop", [&](PlatformResult<std::monostate> result) {
    stopped = std::holds_alternative<std::monostate>(result);
  }));
  platform.RunPlatformModuleTasks();
  REQUIRE(stopped);

  std::optional<PlatformError> error;
  static_cast<void>(channel.Invoke<std::monostate>("invalid", [&](PlatformResult<std::monostate> result) {
    if (const auto* failure = std::get_if<PlatformError>(&result)) {
      error = *failure;
    }
  }));
  platform.RunPlatformModuleTasks();
  REQUIRE(error.has_value());
  REQUIRE(error->code == "huxerui/invalid-result");
}

TEST_CASE("PlatformChannelCancellationInvalidatesQueuedInvocation") {
  TestPlatform platform;
  Application application(PlatformModuleApp, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, platform);
  detail::PlatformChannelEndpoint endpoint = detail::MakePlatformChannelEndpoint(static_cast<huxerui::Runtime&>(platform));
  int invocations = 0;
  int cancellations = 0;
  endpoint.Connect({
      .invoke =
          [&](std::string, PlatformPayload, std::function<void(PlatformResult<PlatformPayload>)>) {
            ++invocations;
            return [&] { ++cancellations; };
          },
  });

  PlatformChannel channel = endpoint.Channel();
  const PlatformRequestId request = channel.Invoke("read", PlatformPayload(), [](PlatformResult<PlatformPayload>) {});
  REQUIRE(channel.Cancel(request));
  platform.RunPlatformModuleTasks();
  REQUIRE(invocations == 0);
  REQUIRE(cancellations == 0);
}

TEST_CASE("PlatformChannelCancelsInFlightInvocationBeforeDisposal") {
  TestPlatform platform;
  Application application(PlatformModuleApp, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, platform);
  detail::PlatformChannelEndpoint endpoint = detail::MakePlatformChannelEndpoint(static_cast<huxerui::Runtime&>(platform));
  PlatformChannel channel = endpoint.Channel();
  std::vector<std::string> operations;
  bool completed = false;
  endpoint.Connect({
      .invoke =
          [&](std::string, PlatformPayload, std::function<void(PlatformResult<PlatformPayload>)>) {
            channel.Close();
            return [&] { operations.emplace_back("cancel"); };
          },
      .dispose = [&] { operations.emplace_back("dispose"); },
  });

  static_cast<void>(
      channel.Invoke("read", PlatformPayload(), [&](PlatformResult<PlatformPayload>) { completed = true; }));
  REQUIRE(channel.IsOpen());
  platform.RunPlatformModuleTasks();
  REQUIRE_FALSE(channel.IsOpen());
  REQUIRE_FALSE(completed);
  const std::vector<std::string> expected{"cancel", "dispose"};
  REQUIRE(operations == expected);
}

TEST_CASE("PlatformChannelRemovesRequestWhenInvocationDispatchFails") {
  TestPlatform platform([](std::function<void()>) { throw std::runtime_error("test dispatch failure"); });
  Application application(PlatformModuleApp, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, platform);
  detail::PlatformChannelEndpoint endpoint = detail::MakePlatformChannelEndpoint(static_cast<huxerui::Runtime&>(platform));
  int invocations = 0;
  endpoint.Connect({
      .invoke =
          [&](std::string, PlatformPayload, std::function<void(PlatformResult<PlatformPayload>)>) {
            ++invocations;
            return std::function<void()>{};
          },
  });

  PlatformChannel channel = endpoint.Channel();
  REQUIRE_THROWS_AS(channel.Invoke("read", PlatformPayload(), [](PlatformResult<PlatformPayload>) {}),
                    std::runtime_error);
  REQUIRE(invocations == 0);
  REQUIRE_FALSE(channel.Cancel(1));
}

TEST_CASE("PlatformChannel restores its declared host for invocations results and events") {
  TestPlatform platform;
  TestPlatform first_platform;
  TestPlatform second_platform;
  std::vector<detail::PlatformChannelEndpoint> endpoints;
  detail::PlatformChannelEndpoint application_endpoint;
  Application application(PlatformModuleApp, {
      .show_debug_overlay = false,
      .window_hooks = {[&](WindowContext&) {
        auto& window = *detail::CurrentUiWindow();
        endpoints.push_back(detail::MakePlatformChannelEndpoint(window));
        if (endpoints.size() == 1) {
          application_endpoint = detail::MakePlatformChannelEndpoint(window.ApplicationRuntime());
        }
      }},
  });
  RuntimeLifetime runtime(application, platform);
  UiWindow first(runtime, first_platform, ResourceConfiguration{.display_scale = 2.0F});
  UiWindow second(runtime, second_platform, ResourceConfiguration{.display_scale = 3.0F});
  endpoints.push_back(application_endpoint);
  std::vector<float> invocations;
  std::vector<float> results;
  std::vector<float> events;
  std::vector<huxerui::UiWindow*> windows;
  std::vector<PlatformChannel> channels;
  for (auto& endpoint : endpoints) {
    endpoint.Connect({.invoke = [&](std::string, PlatformPayload, auto completion) {
      invocations.push_back(UseService<Resources>()->Configuration().display_scale);
      windows.push_back(detail::CurrentUiWindow());
      completion(PlatformPayload{});
      return std::function<void()>{};
    }});
    auto channel = endpoint.Channel();
    channel.On("changed", [&](const PlatformPayload&) {
      events.push_back(UseService<Resources>()->Configuration().display_scale);
    });
    static_cast<void>(channel.Invoke("read", PlatformPayload{}, [&](auto) {
      results.push_back(UseService<Resources>()->Configuration().display_scale);
    }));
    endpoint.Events().Emit("changed", PlatformPayload{});
    channels.push_back(std::move(channel));
  }
  platform.RunPlatformModuleTasks();
  REQUIRE(invocations == std::vector<float>{2.0F, 3.0F, 1.0F});
  REQUIRE(results == invocations);
  REQUIRE(events == invocations);
  REQUIRE(windows == std::vector<huxerui::UiWindow*>{&first_platform, &second_platform, nullptr});
}

TEST_CASE("PlatformChannel rejects retired host delivery and still cancels and disposes native work") {
  const bool window_owned = GENERATE(true, false);
  TestPlatform platform;
  TestPlatform window_platform;
  TestPlatform replacement_platform;
  Application application(PlatformModuleApp, {.show_debug_overlay = false});
  auto runtime = std::make_unique<RuntimeLifetime>(application, platform);
  auto window = std::make_unique<UiWindow>(*runtime, window_platform);
  auto endpoint = window_owned ? detail::MakePlatformChannelEndpoint(static_cast<huxerui::UiWindow&>(window_platform))
      : detail::MakePlatformChannelEndpoint(static_cast<huxerui::Runtime&>(platform));
  std::function<void(PlatformResult<PlatformPayload>)> completion;
  std::vector<std::string> native_cleanup;
  int delivered = 0;
  endpoint.Connect({
      .invoke = [&](std::string, PlatformPayload, auto callback) {
        completion = std::move(callback);
        return [&] { native_cleanup.push_back("cancel"); };
      },
      .dispose = [&] { native_cleanup.push_back("dispose"); },
  });
  auto channel = endpoint.Channel();
  channel.On("changed", [&](const PlatformPayload&) { ++delivered; });
  static_cast<void>(channel.Invoke("read", PlatformPayload{}, [&](auto) { ++delivered; }));
  SECTION("Retirement before native invocation") {}
  SECTION("Retirement after native completion was queued") {
    platform.RunPlatformModuleTasks();
    REQUIRE(completion);
    completion(PlatformPayload{});
  }
  endpoint.Events().Emit("changed", PlatformPayload{});
  window.reset();
  std::unique_ptr<UiWindow> replacement_window;
  std::unique_ptr<RuntimeLifetime> replacement_runtime;
  if (window_owned) {
    replacement_window = std::make_unique<UiWindow>(*runtime, replacement_platform);
  } else {
    runtime.reset();
    replacement_runtime = std::make_unique<RuntimeLifetime>(application, replacement_platform);
  }
  platform.RunPlatformModuleTasks();
  REQUIRE(delivered == 0);
  REQUIRE_FALSE(channel.IsOpen());
  const auto expected = completion ? std::vector<std::string>{"cancel", "dispose"}
                                   : std::vector<std::string>{"dispose"};
  REQUIRE(native_cleanup == expected);
}

} // namespace
} // namespace huxerui::test
