#include <catch2/catch_amalgamated.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/state.h>

#include "runtime_test_support.h"

namespace huxerui::test {
namespace {

std::vector<ApplicationActivation> received_activations;
std::vector<ApplicationLifecycleState> received_lifecycle_states;
ApplicationActivation startup_activation;
State<bool> show_handler;
State<int> handler_version;
ApplicationLifecycleState lifecycle_state = ApplicationLifecycleState::Active;
std::size_t lifecycle_compositions = 0;
Runtime* active_runtime = nullptr;
std::optional<ApplicationHandle> application_handle;

View ActivationApp() {
  auto application = UseApplication();
  startup_activation = application.StartupActivation();
  application.OnActivation([](ApplicationActivation activation) {
    received_activations.push_back(std::move(activation));
  });
  return {};
}

View ConditionalActivationApp() {
  auto application = UseApplication();
  show_handler = UseState(false);
  handler_version = UseState(1);
  if (show_handler) {
    const int version = handler_version;
    application.OnActivation(
        [version](ApplicationActivation activation) {
          static_cast<void>(activation);
          received_activations.push_back(UrlActivation{std::to_string(version)});
        },
        version
    );
  }
  return {};
}

View ReentrantActivationApp() {
  auto application = UseApplication();
  application.OnActivation([](ApplicationActivation activation) {
    received_activations.push_back(std::move(activation));
    if (received_activations.size() == 1) {
      active_runtime->HandleApplicationActivation(UrlActivation{"second"});
    }
  });
  return {};
}

View LifecycleStateApp() {
  auto application = UseApplication();
  lifecycle_state = application.LifecycleState();
  application.OnLifecycleChange([](ApplicationLifecycleState state) { received_lifecycle_states.push_back(state); });
  ++lifecycle_compositions;
  return {};
}

View ApplicationCommandsApp() {
  application_handle = UseApplication();
  return {};
}

void ResetActivationState() {
  received_activations.clear();
  received_lifecycle_states.clear();
  startup_activation = LaunchActivation{};
  show_handler = State<bool>();
  handler_version = State<int>();
  lifecycle_state = ApplicationLifecycleState::Active;
  lifecycle_compositions = 0;
  active_runtime = nullptr;
  application_handle.reset();
}

TEST_CASE("Application quit requests orderly platform termination") {
  ResetActivationState();
  TestPlatform platform;
  Runtime runtime(ApplicationCommandsApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  REQUIRE(application_handle.has_value());
  application_handle->Quit();
  REQUIRE(platform.application_quit_requests == 1);
}

} // namespace

TEST_CASE("Application exposes its immutable startup activation") {
  ResetActivationState();
  TestPlatform platform;
  Runtime runtime(ActivationApp, platform, {.show_debug_overlay = false}, UrlActivation{"huxerui://documents/42"});
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  runtime.BuildFrame();

  REQUIRE(std::get<UrlActivation>(startup_activation).url == "huxerui://documents/42");
  REQUIRE(received_activations.empty());
}

TEST_CASE("Application activation validates startup and subsequent payloads") {
  ResetActivationState();
  TestPlatform platform;

  REQUIRE_THROWS_AS(
      Runtime(ActivationApp, platform, {.show_debug_overlay = false}, UrlActivation{}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      Runtime(ActivationApp, platform, {.show_debug_overlay = false}, FileActivation{}),
      std::invalid_argument
  );

  Runtime runtime(ActivationApp, platform);
  REQUIRE_THROWS_AS(runtime.HandleApplicationActivation(UrlActivation{}), std::invalid_argument);
  REQUIRE_THROWS_AS(runtime.HandleApplicationActivation(FileActivation{}), std::invalid_argument);
}

TEST_CASE("Application delivers subsequent activations in FIFO order without deduplication") {
  ResetActivationState();
  TestPlatform platform;
  Runtime runtime(ActivationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  runtime.HandleApplicationActivation(UrlActivation{"huxerui://same"});
  runtime.HandleApplicationActivation(UrlActivation{"huxerui://same"});
  runtime.HandleApplicationActivation(UrlActivation{"huxerui://last"});
  runtime.BuildFrame();

  REQUIRE(received_activations.size() == 3);
  REQUIRE(std::get<UrlActivation>(received_activations[0]).url == "huxerui://same");
  REQUIRE(std::get<UrlActivation>(received_activations[1]).url == "huxerui://same");
  REQUIRE(std::get<UrlActivation>(received_activations[2]).url == "huxerui://last");
}

TEST_CASE("Application retains activation until a composition handler is connected") {
  ResetActivationState();
  TestPlatform platform;
  Runtime runtime(ConditionalActivationApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  runtime.HandleApplicationActivation(UrlActivation{"huxerui://waiting"});
  runtime.BuildFrame();
  REQUIRE(received_activations.empty());

  show_handler = true;
  runtime.BuildFrame();
  REQUIRE(received_activations.empty());
  runtime.BuildFrame();
  REQUIRE(std::get<UrlActivation>(received_activations.front()).url == "1");

  handler_version = 2;
  runtime.BuildFrame();
  runtime.HandleApplicationActivation(UrlActivation{"huxerui://latest-handler"});
  runtime.BuildFrame();
  REQUIRE(std::get<UrlActivation>(received_activations.back()).url == "2");

  const std::size_t delivered_count = received_activations.size();
  show_handler = false;
  runtime.BuildFrame();
  runtime.HandleApplicationActivation(UrlActivation{"huxerui://while-disconnected"});
  runtime.BuildFrame();
  REQUIRE(received_activations.size() == delivered_count);

  show_handler = true;
  runtime.BuildFrame();
  REQUIRE(received_activations.size() == delivered_count);
  runtime.BuildFrame();
  REQUIRE(received_activations.size() == delivered_count + 1);
  REQUIRE(std::get<UrlActivation>(received_activations.back()).url == "2");
}

TEST_CASE("Application defers activations enqueued by a handler until the next frame") {
  ResetActivationState();
  TestPlatform platform;
  Runtime runtime(ReentrantActivationApp, platform);
  active_runtime = &runtime;
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  runtime.HandleApplicationActivation(UrlActivation{"first"});
  runtime.BuildFrame();
  REQUIRE(received_activations.size() == 1);
  REQUIRE(std::get<UrlActivation>(received_activations.front()).url == "first");

  runtime.BuildFrame();
  REQUIRE(received_activations.size() == 2);
  REQUIRE(std::get<UrlActivation>(received_activations.back()).url == "second");
}

TEST_CASE("Application lifecycle state invalidates only when its current value changes") {
  ResetActivationState();
  TestPlatform platform;
  Runtime runtime(LifecycleStateApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  REQUIRE(lifecycle_state == ApplicationLifecycleState::Active);
  REQUIRE(lifecycle_compositions == 1);

  runtime.UpdateApplicationLifecycleState(ApplicationLifecycleState::Inactive);
  runtime.BuildFrame();
  REQUIRE(lifecycle_state == ApplicationLifecycleState::Inactive);
  REQUIRE(lifecycle_compositions == 2);

  runtime.UpdateApplicationLifecycleState(ApplicationLifecycleState::Inactive);
  runtime.BuildFrame();
  REQUIRE(lifecycle_compositions == 2);

  runtime.UpdateApplicationLifecycleState(ApplicationLifecycleState::Background);
  runtime.BuildFrame();
  REQUIRE(lifecycle_state == ApplicationLifecycleState::Background);
  REQUIRE(lifecycle_compositions == 3);

  REQUIRE_THROWS_AS(
      runtime.UpdateApplicationLifecycleState(static_cast<ApplicationLifecycleState>(-1)),
      std::invalid_argument
  );
}

TEST_CASE("Application delivers mounted lifecycle transitions in FIFO order without losing coalesced states") {
  ResetActivationState();
  TestPlatform platform;
  Runtime runtime(LifecycleStateApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  runtime.UpdateApplicationLifecycleState(ApplicationLifecycleState::Inactive);
  runtime.UpdateApplicationLifecycleState(ApplicationLifecycleState::Background);
  runtime.UpdateApplicationLifecycleState(ApplicationLifecycleState::Active);
  runtime.UpdateApplicationLifecycleState(ApplicationLifecycleState::Active);
  runtime.BuildFrame();

  const std::vector expected_states{
      ApplicationLifecycleState::Inactive,
      ApplicationLifecycleState::Background,
      ApplicationLifecycleState::Active,
  };
  REQUIRE(received_lifecycle_states == expected_states);
  REQUIRE(lifecycle_state == ApplicationLifecycleState::Active);
  REQUIRE(lifecycle_compositions == 2);
}

} // namespace huxerui::test
