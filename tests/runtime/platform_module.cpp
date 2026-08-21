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

struct PlatformModuleCall {
  std::string method;
  PlatformPayload arguments;
  PlatformResultSink result;
  bool cancelled = false;
};

struct PlatformModuleState {
  PlatformPayload options;
  PlatformEventSink events;
  std::vector<std::shared_ptr<PlatformModuleCall>> calls;
  int creates = 0;
  int cancellations = 0;
  int disposals = 0;
};

PlatformModuleFactory TestModuleFactory(const std::shared_ptr<PlatformModuleState>& module_state) {
  PlatformModuleFactory factory;
  factory.create = [module_state](const PlatformPayload& options, PlatformEventSink events) {
    ++module_state->creates;
    module_state->options = options;
    module_state->events = std::move(events);
    PlatformModuleFactory::Instance instance;
    instance.call = [module_state](std::string method, PlatformPayload arguments, PlatformResultSink result) {
      auto call = std::make_shared<PlatformModuleCall>(PlatformModuleCall{
          std::move(method),
          std::move(arguments),
          std::move(result),
          false,
      });
      module_state->calls.push_back(call);
      return [module_state, call] {
        if (!call->cancelled) {
          call->cancelled = true;
          ++module_state->cancellations;
        }
      };
    };
    instance.dispose = [module_state] { ++module_state->disposals; };
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

AppOptions InstallTestModule(
    const std::shared_ptr<PlatformModuleState>& module_state,
    std::shared_ptr<TestService>& service
) {
  AppOptions options{.show_debug_overlay = false};
  options.root_hooks.push_back([module_state, &service](RootContext& root) {
    root.Modules().Register("test/Module", TestModuleFactory(module_state));
    PlatformInstance instance = root.Modules().Open("test/Module", PlatformPayload::Object{{"enabled", true}});
    service = std::make_shared<TestService>(std::move(instance));
    service->BindEvents();
    root.Provide(service);
  });
  return options;
}

AppOptions InstallTextureOption(const std::shared_ptr<PlatformModuleState>& module_state, ExternalTexture texture) {
  AppOptions options{.show_debug_overlay = false};
  options.root_hooks.push_back([module_state, texture = std::move(texture)](RootContext& root) {
    root.Modules().Register("test/TextureModule", TestModuleFactory(module_state));
    static_cast<void>(root.Modules().Open("test/TextureModule", PlatformPayload(texture)));
  });
  return options;
}

void OpenTextureOnAnotherSurface(const ExternalTexture& texture) {
  TestPlatform platform;
  const auto module_state = std::make_shared<PlatformModuleState>();
  Runtime runtime(PlatformModuleApp, platform, InstallTextureOption(module_state, texture));
  static_cast<void>(runtime);
}

TEST_CASE("PlatformInstanceDeliversTypedCallsAndEventsAsynchronously") {
  TestPlatform platform;
  auto module_state = std::make_shared<PlatformModuleState>();
  std::shared_ptr<TestService> service;
  Runtime runtime(PlatformModuleApp, platform, InstallTestModule(module_state, service));

  REQUIRE(service != nullptr);
  REQUIRE(module_state->creates == 1);
  REQUIRE(module_state->options.AsObject().at("enabled").AsBoolean());
  REQUIRE_THROWS_AS(service->BindEvents(), std::invalid_argument);

  std::vector<PlatformResult<int>> results;
  const PlatformRequestId request =
      service->Double(7, [&](PlatformResult<int> result) { results.push_back(std::move(result)); });
  REQUIRE(request != 0);
  REQUIRE(module_state->calls.size() == 1);
  REQUIRE(module_state->calls.front()->method == "double");
  REQUIRE(module_state->calls.front()->arguments.AsInteger() == 7);

  module_state->calls.front()->result(PlatformPayload(14));
  REQUIRE(results.empty());
  platform.RunPlatformModuleTasks();
  REQUIRE(results == std::vector<PlatformResult<int>>{14});

  std::string event_name = "changed";
  module_state->events(std::string_view(event_name), PlatformPayload(1));
  event_name = "replaced";
  module_state->events("changed", PlatformPayload(2));
  REQUIRE(service->events.empty());
  platform.RunPlatformModuleTasks();
  REQUIRE(service->events == std::vector<int>{1, 2});

  module_state->events("changed", PlatformPayload("invalid"));
  module_state->events("", PlatformPayload(3));
  module_state->events(std::string(1, static_cast<char>(0xFF)), PlatformPayload(4));
  platform.RunPlatformModuleTasks();
  REQUIRE(service->events == std::vector<int>{1, 2});

  int throwing_completions = 0;
  static_cast<void>(service->Double(8, [&](PlatformResult<int>) {
    ++throwing_completions;
    throw std::runtime_error("test completion failure");
  }));
  module_state->calls.back()->result(PlatformPayload(16));
  platform.RunPlatformModuleTasks();
  REQUIRE(throwing_completions == 1);
}

TEST_CASE("PlatformInstanceOrdersResultsAndRejectsCancelledOrDuplicateDelivery") {
  TestPlatform platform;
  auto module_state = std::make_shared<PlatformModuleState>();
  std::shared_ptr<TestService> service;
  Runtime runtime(PlatformModuleApp, platform, InstallTestModule(module_state, service));

  std::vector<int> completion_order;
  const PlatformRequestId first =
      service->Double(1, [&](PlatformResult<int> result) { completion_order.push_back(std::get<int>(result)); });
  const PlatformRequestId second =
      service->Double(2, [&](PlatformResult<int> result) { completion_order.push_back(std::get<int>(result)); });
  const PlatformRequestId cancelled =
      service->Double(3, [&](PlatformResult<int> result) { completion_order.push_back(std::get<int>(result)); });
  REQUIRE(first != second);
  REQUIRE(second != cancelled);

  module_state->calls[1]->result(PlatformPayload(20));
  module_state->calls[0]->result(PlatformPayload(10));
  module_state->calls[0]->result(PlatformPayload(11));
  REQUIRE(service->Cancel(cancelled));
  REQUIRE_FALSE(service->Cancel(cancelled));
  REQUIRE(module_state->cancellations == 1);
  module_state->calls[2]->result(PlatformPayload(30));

  platform.RunPlatformModuleTasks();
  REQUIRE(completion_order == std::vector<int>{20, 10});
  REQUIRE_FALSE(service->Cancel(first));
}

TEST_CASE("PlatformInstanceConvertsErrorsAndInvalidTypedResults") {
  TestPlatform platform;
  auto module_state = std::make_shared<PlatformModuleState>();
  std::shared_ptr<TestService> service;
  Runtime runtime(PlatformModuleApp, platform, InstallTestModule(module_state, service));

  std::vector<PlatformResult<int>> results;
  service->Double(1, [&](PlatformResult<int> result) { results.push_back(std::move(result)); });
  service->Double(2, [&](PlatformResult<int> result) { results.push_back(std::move(result)); });
  module_state->calls[0]->result(PlatformError{"test/rejected", "Rejected by test module", {}});
  module_state->calls[1]->result(PlatformPayload("invalid"));
  platform.RunPlatformModuleTasks();

  REQUIRE(results.size() == 2);
  REQUIRE(std::get<PlatformError>(results[0]).code == "test/rejected");
  REQUIRE(std::get<PlatformError>(results[1]).code == "huxerui/invalid-result");
}

TEST_CASE("PlatformInstanceMoveAndRootTeardownClosePlatformModuleState") {
  TestPlatform platform;
  auto module_state = std::make_shared<PlatformModuleState>();
  std::shared_ptr<TestService> service;
  {
    Runtime runtime(PlatformModuleApp, platform, InstallTestModule(module_state, service));
    REQUIRE(service->MoveRoundTrip());
    static_cast<void>(service->Double(1, [](PlatformResult<int>) {}));
    REQUIRE(module_state->disposals == 0);
    service.reset();
  }

  REQUIRE(module_state->cancellations == 1);
  REQUIRE(module_state->disposals == 1);
  module_state->calls.front()->result(PlatformPayload(2));
  module_state->events("changed", PlatformPayload(3));
  platform.RunPlatformModuleTasks();
  REQUIRE(module_state->disposals == 1);
}

TEST_CASE("PlatformAdapterCreatesPlatformModuleRegistration") {
  ProjectingTestPlatform platform;
  auto module_state = std::make_shared<PlatformModuleState>();
  std::shared_ptr<TestService> service;
  AppOptions options{.show_debug_overlay = false};
  options.root_hooks.push_back([module_state, &service](RootContext& root) {
    root.Modules().Register("test/PlatformModule", TestPlatformModuleFactory{TestModuleFactory(module_state)});
    service = std::make_shared<TestService>(
        root.Modules().Open("test/PlatformModule", PlatformPayload::Object{{"platform", true}})
    );
    root.Provide(service);
  });
  Runtime runtime(PlatformModuleApp, platform, std::move(options));

  REQUIRE(module_state->creates == 1);
  REQUIRE(module_state->options.AsObject().at("platform").AsBoolean());
  REQUIRE(service != nullptr);
}

TEST_CASE("PlatformModulesBindNestedExternalTexturePayloadsToOneSurface") {
  const ExternalTexture texture = MakeTestExternalTexture({32.0F, 18.0F});
  const auto install = [texture](const std::shared_ptr<PlatformModuleState>& module_state) {
    AppOptions options{.show_debug_overlay = false};
    options.root_hooks.push_back([texture, module_state](RootContext& root) {
      root.Modules().Register("test/TextureModule", TestModuleFactory(module_state));
      static_cast<void>(root.Modules().Open(
          "test/TextureModule",
          PlatformPayload::Object{{"textures", PlatformPayload::List{texture}}}
      ));
    });
    return options;
  };

  TestPlatform first_platform;
  const auto first_module_state = std::make_shared<PlatformModuleState>();
  Runtime first_runtime(PlatformModuleApp, first_platform, install(first_module_state));
  REQUIRE(first_module_state->creates == 1);
  REQUIRE(
      first_module_state->options.AsObject().at("textures").AsList().front().AsExternalTexture() == texture
  );

  TestPlatform other_platform;
  const auto other_module_state = std::make_shared<PlatformModuleState>();
  REQUIRE_THROWS_AS(
      Runtime(PlatformModuleApp, other_platform, install(other_module_state)),
      std::logic_error
  );
  REQUIRE(other_module_state->creates == 0);
}

TEST_CASE("PlatformInstanceBindsExternalTexturesAcrossCallsResultsAndEvents") {
  TestPlatform platform;
  auto module_state = std::make_shared<PlatformModuleState>();
  std::shared_ptr<TestService> service;
  Runtime runtime(PlatformModuleApp, platform, InstallTestModule(module_state, service));

  const ExternalTexture argument = MakeTestExternalTexture({32.0F, 18.0F});
  std::vector<PlatformResult<ExternalTexture>> results;
  static_cast<void>(service->Texture(argument, [&](PlatformResult<ExternalTexture> result) {
    results.push_back(std::move(result));
  }));
  REQUIRE(module_state->calls.back()->arguments.AsExternalTexture() == argument);

  REQUIRE_THROWS_AS(OpenTextureOnAnotherSurface(argument), std::logic_error);

  const ExternalTexture result = MakeTestExternalTexture({32.0F, 18.0F});
  module_state->calls.back()->result(PlatformPayload(result));
  REQUIRE_THROWS_AS(OpenTextureOnAnotherSurface(result), std::logic_error);
  platform.RunPlatformModuleTasks();
  REQUIRE(results.size() == 1);
  REQUIRE(std::get<ExternalTexture>(results.front()) == result);

  const ExternalTexture event = MakeTestExternalTexture({32.0F, 18.0F});
  module_state->events("textureChanged", PlatformPayload(event));
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
