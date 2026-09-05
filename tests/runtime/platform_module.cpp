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
  detail::PlatformViewFactoryRegistration Erase(PlatformAdapter& adapter) && {
    static_cast<void>(adapter);
    return detail::MakePlatformViewFactoryRegistration(std::make_shared<int>(1));
  }

  friend class detail::PlatformRegistry;
};

struct AdapterPreparedPlatformViewFactory {
  PlatformAdapter** prepared = nullptr;

private:
  detail::PlatformViewFactoryRegistration Erase(PlatformAdapter& adapter) && {
    *prepared = &adapter;
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
  std::unique_ptr<TestModule> module;
  AppOptions options{.show_debug_overlay = false};
  options.root_hooks.push_back([&](RootContext& root) {
    root.RegisterPlatformModule<TestModule, TestModuleOptions>(
        "test/Module", [disposals](PlatformAdapter&, const TestModuleOptions& options) {
          return TestModule(options.value, disposals);
        });
    module =
        std::make_unique<TestModule>(root.OpenPlatformModule<TestModule>("test/Module", TestModuleOptions{.value = 7}));
  });

  {
    Runtime runtime(PlatformModuleApp, platform, std::move(options));
    REQUIRE(module != nullptr);
    REQUIRE(module->Value() == 7);
    REQUIRE(*disposals == 0);
  }
  module.reset();
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
  options.root_hooks.push_back([](RootContext& root) {
    root.RegisterPlatformModule<std::shared_ptr<int>, TestModuleOptions>(
        "test/LifecycleModule",
        [](PlatformAdapter&, const TestModuleOptions& options) { return std::make_shared<int>(options.value); });
  });

  {
    Runtime runtime(LifecyclePlatformModuleApp, platform, std::move(options));
    runtime.BuildFrame();
    REQUIRE(lifecycle_module_value != nullptr);
    REQUIRE(*lifecycle_module_value == 11);
    REQUIRE(*lifecycle_cleanups == 0);
  }
  REQUIRE(lifecycle_module_value == nullptr);
  REQUIRE(*lifecycle_cleanups == 1);
}

TEST_CASE("PlatformRegistryPassesOwningAdapterToModuleFactory") {
  TestPlatform platform;
  PlatformAdapter* received = nullptr;
  AppOptions options{.show_debug_overlay = false};
  options.root_hooks.push_back([&](RootContext& root) {
    root.RegisterPlatformModule<std::shared_ptr<int>>("test/Adapter", [&received](PlatformAdapter& adapter) {
      received = &adapter;
      return std::make_shared<int>(3);
    });
    REQUIRE(*root.OpenPlatformModule<std::shared_ptr<int>>("test/Adapter") == 3);
  });
  Runtime runtime(PlatformModuleApp, platform, std::move(options));
  REQUIRE(received == &platform);
}

TEST_CASE("PlatformRegistryPassesOwningAdapterWhileErasingPlatformViewFactories") {
  TestPlatform platform;
  PlatformAdapter* view_adapter = nullptr;
  AppOptions options{.show_debug_overlay = false};
  options.root_hooks.push_back([&](RootContext& root) {
    root.RegisterPlatformView<int>("test/PreparedView", AdapterPreparedPlatformViewFactory{&view_adapter});
  });
  Runtime runtime(PlatformModuleApp, platform, std::move(options));
  REQUIRE(view_adapter == &platform);
}

TEST_CASE("PlatformRegistryRejectsDuplicateNamesAcrossRegistrationKinds") {
  TestPlatform platform;
  AppOptions options{.show_debug_overlay = false};
  options.root_hooks.push_back([](RootContext& root) {
    root.RegisterPlatformModule<std::shared_ptr<int>>("test/Duplicate",
                                                      [](PlatformAdapter&) { return std::make_shared<int>(1); });
    root.RegisterPlatformView<int>("test/Duplicate", DummyPlatformViewFactory{});
  });
  REQUIRE_THROWS_AS(Runtime(PlatformModuleApp, platform, std::move(options)), std::logic_error);
}

TEST_CASE("PlatformRegistryRejectsWrongModuleAndOptionsTypes") {
  TestPlatform platform;
  AppOptions options{.show_debug_overlay = false};
  options.root_hooks.push_back([](RootContext& root) {
    root.RegisterPlatformModule<std::shared_ptr<int>, TestModuleOptions>(
        "test/Typed",
        [](PlatformAdapter&, const TestModuleOptions& options) { return std::make_shared<int>(options.value); });
    REQUIRE_THROWS_AS(root.OpenPlatformModule<std::shared_ptr<std::string>>("test/Typed"), std::logic_error);
    REQUIRE_THROWS_AS(root.OpenPlatformModule<std::shared_ptr<int>>("test/Typed", std::string("wrong")),
                      std::logic_error);
  });
  Runtime runtime(PlatformModuleApp, platform, std::move(options));
}

TEST_CASE("PlatformRegistryRejectsEmptyMissingAndInvalidUtf8Names") {
  TestPlatform platform;
  AppOptions options{.show_debug_overlay = false};
  options.root_hooks.push_back([](RootContext& root) {
    REQUIRE_THROWS_AS(root.RegisterPlatformModule<std::shared_ptr<int>>(
                          "", [](PlatformAdapter&) { return std::make_shared<int>(1); }),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(
        root.RegisterPlatformModule<std::shared_ptr<int>>(std::string(1, static_cast<char>(0xFF)),
                                                          [](PlatformAdapter&) { return std::make_shared<int>(1); }),
        std::invalid_argument);
    REQUIRE_THROWS_AS(root.OpenPlatformModule<std::shared_ptr<int>>("test/Missing"), std::logic_error);
  });
  Runtime runtime(PlatformModuleApp, platform, std::move(options));
}

TEST_CASE("PlatformChannelOwnsInvocationEventAndDisposalDelivery") {
  TestPlatform platform;
  detail::PlatformChannelEndpoint endpoint = detail::MakePlatformChannelEndpoint(platform);
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
  detail::PlatformChannelEndpoint endpoint = detail::MakePlatformChannelEndpoint(platform);
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
  detail::PlatformChannelEndpoint endpoint = detail::MakePlatformChannelEndpoint(platform);
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
  detail::PlatformChannelEndpoint endpoint = detail::MakePlatformChannelEndpoint(platform);
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
  detail::PlatformChannelEndpoint endpoint = detail::MakePlatformChannelEndpoint(platform);
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

} // namespace
} // namespace huxerui::test
