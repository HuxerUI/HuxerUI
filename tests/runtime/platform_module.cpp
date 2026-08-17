#include "runtime_test_support.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "external_texture_test_support.h"

namespace huxerui::test {
namespace {

struct TestPlatformMethods {
  struct Double {
    using Request = int;
    using Result = int;
    static constexpr std::string_view Name = "double";

    static PlatformPayload Encode(int value) {
      return value;
    }

    static int Decode(const PlatformPayload& payload) {
      return static_cast<int>(payload.AsInteger());
    }
  };

  struct Widen {
    using Request = int;
    using Result = std::int64_t;
    static constexpr std::string_view Name = "widen";

    static PlatformPayload Encode(int value) {
      return value;
    }

    static int Decode(const PlatformPayload& payload) {
      return static_cast<int>(payload.AsInteger());
    }
  };

  struct Texture {
    using Request = ExternalTexture;
    using Result = ExternalTexture;
    static constexpr std::string_view Name = "texture";

    static PlatformPayload Encode(const ExternalTexture& value) {
      return PlatformPayload(value);
    }

    static ExternalTexture Decode(const PlatformPayload& payload) {
      return payload.AsExternalTexture();
    }
  };
};

struct InvalidPlatformMethods {
  struct VoidRequest {
    using Request = void;
    using Result = int;
    static constexpr std::string_view Name = "voidRequest";

    static PlatformPayload Encode();
    static int Decode(const PlatformPayload&);
  };

  struct ImmovableResult {
    ImmovableResult() = default;
    ImmovableResult(const ImmovableResult&) = delete;
    ImmovableResult& operator=(const ImmovableResult&) = delete;
    ImmovableResult(ImmovableResult&&) = delete;
    ImmovableResult& operator=(ImmovableResult&&) = delete;
  };

  struct ReturnsImmovable {
    using Request = int;
    using Result = ImmovableResult;
    static constexpr std::string_view Name = "returnsImmovable";

    static PlatformPayload Encode(int);
    static ImmovableResult Decode(const PlatformPayload&);
  };

  struct ReturnsPlatformError {
    using Request = int;
    using Result = PlatformError;
    static constexpr std::string_view Name = "returnsPlatformError";

    static PlatformPayload Encode(int);
    static PlatformError Decode(const PlatformPayload&);
  };
};

struct TestPlatformModuleEvents {
  struct Changed : Event<int> {
    static constexpr std::string_view Name = "changed";

    static int Decode(const PlatformPayload& payload) {
      return static_cast<int>(payload.AsInteger());
    }
  };

  struct TextureChanged : Event<ExternalTexture> {
    static constexpr std::string_view Name = "textureChanged";

    static ExternalTexture Decode(const PlatformPayload& payload) {
      return payload.AsExternalTexture();
    }
  };

  struct MissingDecoder : Event<int> {
    static constexpr std::string_view Name = "missingDecoder";
  };

  struct InvalidDecoder : Event<int> {
    static constexpr std::string_view Name = "invalidDecoder";

    static void Decode(const PlatformPayload&);
  };
};

static_assert(detail::PlatformMethodKey<TestPlatformMethods::Double>);
static_assert(detail::PlatformMethodKey<TestPlatformMethods::Widen>);
static_assert(detail::PlatformMethodKey<TestPlatformMethods::Texture>);
static_assert(!detail::PlatformMethodKey<InvalidPlatformMethods::VoidRequest>);
static_assert(!detail::PlatformMethodKey<InvalidPlatformMethods::ReturnsImmovable>);
static_assert(!detail::PlatformMethodKey<InvalidPlatformMethods::ReturnsPlatformError>);
static_assert(detail::PlatformEventKey<TestPlatformModuleEvents::Changed>);
static_assert(detail::PlatformEventKey<TestPlatformModuleEvents::TextureChanged>);
static_assert(!detail::PlatformEventKey<TestPlatformModuleEvents::MissingDecoder>);
static_assert(!detail::PlatformEventKey<TestPlatformModuleEvents::InvalidDecoder>);

struct NativeCall {
  std::string method;
  PlatformPayload arguments;
  PlatformResultSink result;
  bool cancelled = false;
};

struct NativeState {
  PlatformPayload options;
  PlatformEventSink events;
  std::vector<std::shared_ptr<NativeCall>> calls;
  int creates = 0;
  int cancellations = 0;
  int disposals = 0;
};

PlatformModuleFactory TestModuleFactory(const std::shared_ptr<NativeState>& native) {
  PlatformModuleFactory factory;
  factory.create = [native](const PlatformPayload& options, PlatformEventSink events) {
    ++native->creates;
    native->options = options;
    native->events = std::move(events);
    PlatformModuleFactory::Instance instance;
    instance.call = [native](std::string method, PlatformPayload arguments, PlatformResultSink result) {
      auto call = std::make_shared<NativeCall>(NativeCall{
          std::move(method),
          std::move(arguments),
          std::move(result),
          false,
      });
      native->calls.push_back(call);
      return [native, call] {
        if (!call->cancelled) {
          call->cancelled = true;
          ++native->cancellations;
        }
      };
    };
    instance.dispose = [native] { ++native->disposals; };
    return instance;
  };
  return factory;
}

struct TestPlatformModuleFactory {
  PlatformModuleFactory factory;
};

class ProjectingTestPlatform final : public TestPlatform {
protected:
  PlatformModuleFactory::Instance CreatePlatformModule(
      std::string_view type, const PlatformPayload& options, PlatformEventSink events
  ) override {
    if (const auto* registration = FindPlatformModuleRegistration<TestPlatformModuleFactory>(type)) {
      return registration->factory.create(options, std::move(events));
    }
    return TestPlatform::CreatePlatformModule(type, options, std::move(events));
  }
};

class TestService final {
public:
  explicit TestService(PlatformInstance instance) : instance_(std::move(instance)) {}

  PlatformRequestId Double(int value, std::function<void(PlatformResult<int>)> completion) {
    return instance_.Call<TestPlatformMethods::Double>(value, std::move(completion));
  }

  PlatformRequestId Texture(
      const ExternalTexture& texture,
      std::function<void(PlatformResult<ExternalTexture>)> completion
  ) {
    return instance_.Call<TestPlatformMethods::Texture>(texture, std::move(completion));
  }

  void BindEvents() {
    instance_.On<TestPlatformModuleEvents::Changed>([this](int value) { events.push_back(value); });
    instance_.On<TestPlatformModuleEvents::TextureChanged>([this](ExternalTexture texture) {
      texture_events.push_back(std::move(texture));
    });
  }

  bool Cancel(PlatformRequestId request) {
    return instance_.Cancel(request);
  }

  bool MoveRoundTrip() {
    PlatformInstance moved = std::move(instance_);
    bool moved_from_rejected = false;
    try {
      static_cast<void>(instance_.Call<TestPlatformMethods::Double>(0, [](PlatformResult<int>) {}));
    } catch (const std::logic_error&) {
      moved_from_rejected = true;
    }
    instance_ = std::move(moved);
    return moved_from_rejected;
  }

  std::vector<int> events;
  std::vector<ExternalTexture> texture_events;

private:
  PlatformInstance instance_;
};

View PlatformModuleApp() {
  return Text("module");
}

AppOptions InstallTestModule(const std::shared_ptr<NativeState>& native, std::shared_ptr<TestService>& service) {
  AppOptions options{.show_debug_overlay = false};
  options.root_hooks.push_back([native, &service](RootContext& root) {
    root.Modules().Register("test/Module", TestModuleFactory(native));
    PlatformInstance instance = root.Modules().Open("test/Module", PlatformPayload::Object{{"enabled", true}});
    service = std::make_shared<TestService>(std::move(instance));
    service->BindEvents();
    root.Provide(service);
  });
  return options;
}

AppOptions InstallTextureOption(const std::shared_ptr<NativeState>& native, ExternalTexture texture) {
  AppOptions options{.show_debug_overlay = false};
  options.root_hooks.push_back([native, texture = std::move(texture)](RootContext& root) {
    root.Modules().Register("test/TextureModule", TestModuleFactory(native));
    static_cast<void>(root.Modules().Open("test/TextureModule", PlatformPayload(texture)));
  });
  return options;
}

void OpenTextureOnAnotherSurface(const ExternalTexture& texture) {
  TestPlatform platform;
  const auto native = std::make_shared<NativeState>();
  Runtime runtime(PlatformModuleApp, platform, InstallTextureOption(native, texture));
  static_cast<void>(runtime);
}

TEST_CASE("PlatformInstanceDeliversTypedCallsAndEventsAsynchronously") {
  TestPlatform platform;
  auto native = std::make_shared<NativeState>();
  std::shared_ptr<TestService> service;
  Runtime runtime(PlatformModuleApp, platform, InstallTestModule(native, service));

  REQUIRE(service != nullptr);
  REQUIRE(native->creates == 1);
  REQUIRE(native->options.AsObject().at("enabled").AsBoolean());
  REQUIRE_THROWS_AS(service->BindEvents(), std::invalid_argument);

  std::vector<PlatformResult<int>> results;
  const PlatformRequestId request =
      service->Double(7, [&](PlatformResult<int> result) { results.push_back(std::move(result)); });
  REQUIRE(request != 0);
  REQUIRE(native->calls.size() == 1);
  REQUIRE(native->calls.front()->method == "double");
  REQUIRE(native->calls.front()->arguments.AsInteger() == 7);

  native->calls.front()->result(PlatformPayload(14));
  REQUIRE(results.empty());
  platform.RunPlatformModuleTasks();
  REQUIRE(results == std::vector<PlatformResult<int>>{14});

  native->events("changed", PlatformPayload(1));
  native->events("changed", PlatformPayload(2));
  REQUIRE(service->events.empty());
  platform.RunPlatformModuleTasks();
  REQUIRE(service->events == std::vector<int>{1, 2});

  native->events("changed", PlatformPayload("invalid"));
  native->events("", PlatformPayload(3));
  native->events(std::string(1, static_cast<char>(0xFF)), PlatformPayload(4));
  platform.RunPlatformModuleTasks();
  REQUIRE(service->events == std::vector<int>{1, 2});

  int throwing_completions = 0;
  static_cast<void>(service->Double(8, [&](PlatformResult<int>) {
    ++throwing_completions;
    throw std::runtime_error("test completion failure");
  }));
  native->calls.back()->result(PlatformPayload(16));
  platform.RunPlatformModuleTasks();
  REQUIRE(throwing_completions == 1);
}

TEST_CASE("PlatformInstanceOrdersResultsAndRejectsCancelledOrDuplicateDelivery") {
  TestPlatform platform;
  auto native = std::make_shared<NativeState>();
  std::shared_ptr<TestService> service;
  Runtime runtime(PlatformModuleApp, platform, InstallTestModule(native, service));

  std::vector<int> completion_order;
  const PlatformRequestId first =
      service->Double(1, [&](PlatformResult<int> result) { completion_order.push_back(std::get<int>(result)); });
  const PlatformRequestId second =
      service->Double(2, [&](PlatformResult<int> result) { completion_order.push_back(std::get<int>(result)); });
  const PlatformRequestId cancelled =
      service->Double(3, [&](PlatformResult<int> result) { completion_order.push_back(std::get<int>(result)); });
  REQUIRE(first != second);
  REQUIRE(second != cancelled);

  native->calls[1]->result(PlatformPayload(20));
  native->calls[0]->result(PlatformPayload(10));
  native->calls[0]->result(PlatformPayload(11));
  REQUIRE(service->Cancel(cancelled));
  REQUIRE_FALSE(service->Cancel(cancelled));
  REQUIRE(native->cancellations == 1);
  native->calls[2]->result(PlatformPayload(30));

  platform.RunPlatformModuleTasks();
  REQUIRE(completion_order == std::vector<int>{20, 10});
  REQUIRE_FALSE(service->Cancel(first));
}

TEST_CASE("PlatformInstanceConvertsErrorsAndInvalidTypedResults") {
  TestPlatform platform;
  auto native = std::make_shared<NativeState>();
  std::shared_ptr<TestService> service;
  Runtime runtime(PlatformModuleApp, platform, InstallTestModule(native, service));

  std::vector<PlatformResult<int>> results;
  service->Double(1, [&](PlatformResult<int> result) { results.push_back(std::move(result)); });
  service->Double(2, [&](PlatformResult<int> result) { results.push_back(std::move(result)); });
  native->calls[0]->result(PlatformError{"test/rejected", "Rejected by test module", {}});
  native->calls[1]->result(PlatformPayload("invalid"));
  platform.RunPlatformModuleTasks();

  REQUIRE(results.size() == 2);
  REQUIRE(std::get<PlatformError>(results[0]).code == "test/rejected");
  REQUIRE(std::get<PlatformError>(results[1]).code == "huxerui/invalid-result");
}

TEST_CASE("PlatformInstanceMoveAndRootTeardownCloseNativeState") {
  TestPlatform platform;
  auto native = std::make_shared<NativeState>();
  std::shared_ptr<TestService> service;
  {
    Runtime runtime(PlatformModuleApp, platform, InstallTestModule(native, service));
    REQUIRE(service->MoveRoundTrip());
    static_cast<void>(service->Double(1, [](PlatformResult<int>) {}));
    REQUIRE(native->disposals == 0);
    service.reset();
  }

  REQUIRE(native->cancellations == 1);
  REQUIRE(native->disposals == 1);
  native->calls.front()->result(PlatformPayload(2));
  native->events("changed", PlatformPayload(3));
  platform.RunPlatformModuleTasks();
  REQUIRE(native->disposals == 1);
}

TEST_CASE("PlatformAdapterCreatesNativePlatformModuleRegistration") {
  ProjectingTestPlatform platform;
  auto native = std::make_shared<NativeState>();
  std::shared_ptr<TestService> service;
  AppOptions options{.show_debug_overlay = false};
  options.root_hooks.push_back([native, &service](RootContext& root) {
    root.Modules().Register("test/NativeModule", TestPlatformModuleFactory{TestModuleFactory(native)});
    service = std::make_shared<TestService>(
        root.Modules().Open("test/NativeModule", PlatformPayload::Object{{"native", true}})
    );
    root.Provide(service);
  });
  Runtime runtime(PlatformModuleApp, platform, std::move(options));

  REQUIRE(native->creates == 1);
  REQUIRE(native->options.AsObject().at("native").AsBoolean());
  REQUIRE(service != nullptr);
}

TEST_CASE("PlatformModulesBindNestedExternalTexturePayloadsToOneSurface") {
  const ExternalTexture texture = MakeTestExternalTexture({32.0F, 18.0F});
  const auto install = [texture](const std::shared_ptr<NativeState>& native) {
    AppOptions options{.show_debug_overlay = false};
    options.root_hooks.push_back([texture, native](RootContext& root) {
      root.Modules().Register("test/TextureModule", TestModuleFactory(native));
      static_cast<void>(root.Modules().Open(
          "test/TextureModule",
          PlatformPayload::Object{{"textures", PlatformPayload::List{texture}}}
      ));
    });
    return options;
  };

  TestPlatform first_platform;
  const auto first_native = std::make_shared<NativeState>();
  Runtime first_runtime(PlatformModuleApp, first_platform, install(first_native));
  REQUIRE(first_native->creates == 1);
  REQUIRE(
      first_native->options.AsObject().at("textures").AsList().front().AsExternalTexture() == texture
  );

  TestPlatform other_platform;
  const auto other_native = std::make_shared<NativeState>();
  REQUIRE_THROWS_AS(
      Runtime(PlatformModuleApp, other_platform, install(other_native)),
      std::logic_error
  );
  REQUIRE(other_native->creates == 0);
}

TEST_CASE("PlatformInstanceBindsExternalTexturesAcrossCallsResultsAndEvents") {
  TestPlatform platform;
  auto native = std::make_shared<NativeState>();
  std::shared_ptr<TestService> service;
  Runtime runtime(PlatformModuleApp, platform, InstallTestModule(native, service));

  const ExternalTexture argument = MakeTestExternalTexture({32.0F, 18.0F});
  std::vector<PlatformResult<ExternalTexture>> results;
  static_cast<void>(service->Texture(argument, [&](PlatformResult<ExternalTexture> result) {
    results.push_back(std::move(result));
  }));
  REQUIRE(native->calls.back()->arguments.AsExternalTexture() == argument);

  REQUIRE_THROWS_AS(OpenTextureOnAnotherSurface(argument), std::logic_error);

  const ExternalTexture result = MakeTestExternalTexture({32.0F, 18.0F});
  native->calls.back()->result(PlatformPayload(result));
  REQUIRE_THROWS_AS(OpenTextureOnAnotherSurface(result), std::logic_error);
  platform.RunPlatformModuleTasks();
  REQUIRE(results.size() == 1);
  REQUIRE(std::get<ExternalTexture>(results.front()) == result);

  const ExternalTexture event = MakeTestExternalTexture({32.0F, 18.0F});
  native->events("textureChanged", PlatformPayload(event));
  REQUIRE_THROWS_AS(OpenTextureOnAnotherSurface(event), std::logic_error);
  platform.RunPlatformModuleTasks();
  REQUIRE(service->texture_events == std::vector<ExternalTexture>{event});
}

TEST_CASE("PlatformModulesValidateNonvisualFactoryRegistration") {
  TestPlatform undispatched_platform(UIThreadDispatcher{});
  AppOptions undispatched_options{.show_debug_overlay = false};
  undispatched_options.root_hooks.push_back(
      [](RootContext& root) { static_cast<void>(root.Modules().Open("test/Module")); }
  );
  REQUIRE_THROWS_WITH(
      Runtime(PlatformModuleApp, undispatched_platform, std::move(undispatched_options)),
      "HuxerUI UI thread dispatcher is not configured"
  );

  TestPlatform missing_platform;
  AppOptions missing_options{.show_debug_overlay = false};
  RootHook missing_hook = [](RootContext& root) { static_cast<void>(root.Modules().Open("test/Missing")); };
  missing_options.root_hooks.push_back(std::move(missing_hook));
  REQUIRE_THROWS_AS(Runtime(PlatformModuleApp, missing_platform, std::move(missing_options)), std::logic_error);

  TestPlatform incompatible_platform;
  AppOptions incompatible_options{.show_debug_overlay = false};
  incompatible_options.root_hooks.push_back([](RootContext& root) {
    root.Modules().Register("test/Module", 42);
    static_cast<void>(root.Modules().Open("test/Module"));
  });
  REQUIRE_THROWS_AS(
      Runtime(PlatformModuleApp, incompatible_platform, std::move(incompatible_options)),
      std::logic_error
  );

  TestPlatform invalid_platform;
  AppOptions invalid_options{.show_debug_overlay = false};
  invalid_options.root_hooks.push_back([](RootContext& root) {
    root.Modules().Register("test/Module", PlatformModuleFactory{});
    static_cast<void>(root.Modules().Open("test/Module"));
  });
  REQUIRE_THROWS_AS(Runtime(PlatformModuleApp, invalid_platform, std::move(invalid_options)), std::logic_error);

  TestPlatform missing_call_platform;
  AppOptions missing_call_options{.show_debug_overlay = false};
  missing_call_options.root_hooks.push_back([](RootContext& root) {
    PlatformModuleFactory factory;
    factory.create = [](const PlatformPayload&, PlatformEventSink) { return PlatformModuleFactory::Instance{}; };
    root.Modules().Register("test/Module", std::move(factory));
    static_cast<void>(root.Modules().Open("test/Module"));
  });
  REQUIRE_THROWS_AS(
      Runtime(PlatformModuleApp, missing_call_platform, std::move(missing_call_options)),
      std::logic_error
  );
}

} // namespace
} // namespace huxerui::test
