#include "runtime_test_support.h"

#include <deque>
#include <filesystem>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

#include "application/platform_registry_internal.h"

namespace huxerui::test {
namespace {

View EmptyApplication() { return {}; }

struct DemoViewModel {
  State<int> value{0};
};

std::vector<int> observed_values;
View SharedStateApplication() {
  observed_values.push_back(UseService<DemoViewModel>()->value.Get());
  return Text(std::to_string(observed_values.back()));
}

std::function<View()> event_context_root;
View EventContextApplication() { return event_context_root(); }

TEST_CASE("Window events retain their local environment when handlers are removed and replaced") {
  REQUIRE_THROWS_AS(UseEnvironment<Locale>(), std::logic_error);
  REQUIRE_THROWS_AS(UseTheme(), std::logic_error);
  TestPlatform platform;
  std::shared_ptr<DemoViewModel> window_model;
  State<int> phase;
  std::vector<std::string> locales;
  event_context_root = [&] {
    phase = UseState(0);
    View target = Button("Run").With(Frame{120.0F, 48.0F});
    if (phase.Get() != 1) {
      target = std::move(target).OnClick([&] {
        REQUIRE_THROWS_AS(UseEnvironment<Locale>(), std::logic_error);
        REQUIRE_THROWS_AS(UseTheme(), std::logic_error);
        REQUIRE(UseService<DemoViewModel>() == window_model);
        locales.emplace_back(UseService<Resources>()->Configuration().locale.LanguageTag());
      });
    }
    return ProvideEnvironment(Locale::FromLanguageTag(phase.Get() == 0 ? "fr-FR" : "de-DE"), std::move(target));
  };
  UiWindow window(EventContextApplication, platform, {
      .show_debug_overlay = false,
      .window_hooks = {[&](WindowContext& context) {
        window_model = std::make_shared<DemoViewModel>();
        context.Provide(window_model);
      }},
  });
  window.SetWindowMetrics({.viewport = {120.0F, 48.0F}});
  window.BuildFrame();
  const auto click = [&] {
    window.HandlePointerEvent({PointerEventType::Down, 801, {60.0F, 24.0F}});
    window.HandlePointerEvent({PointerEventType::Up, 801, {60.0F, 24.0F}});
  };
  click();
  REQUIRE(locales == std::vector<std::string>{"fr-FR"});
  phase = 1;
  window.BuildFrame();
  click();
  REQUIRE(locales.size() == 1);
  phase = 2;
  window.BuildFrame();
  click();
  REQUIRE(locales == std::vector<std::string>{"fr-FR", "de-DE"});
  REQUIRE_THROWS_AS(UseEnvironment<Locale>(), std::logic_error);
  REQUIRE_THROWS_AS(UseTheme(), std::logic_error);
  REQUIRE_THROWS_AS(UseService<DemoViewModel>(), std::logic_error);
}

TEST_CASE("UiWindow admits native callbacks only after successful shared initialization") {
  class WindowPlatform final : public TestPlatform {
  public:
    using huxerui::UiWindow::IsInitialized;
  } platform;
  huxerui::UiWindow& ui_window = platform;
  bool fail_hook = false;
  int hooks = 0;
  Application application(EmptyApplication, {
      .show_debug_overlay = false,
      .window_hooks = {[&](WindowContext&) {
        ++hooks;
        REQUIRE_FALSE(platform.IsInitialized());
        REQUIRE(UseWindow().LifecycleState() == WindowLifecycleState::Background);
        REQUIRE_NOTHROW(detail::MakePlatformChannelEndpoint(ui_window));
        if (fail_hook) throw std::runtime_error("window hook failure");
      }},
  });
  RuntimeLifetime runtime(application, platform);
  REQUIRE_FALSE(platform.IsInitialized());
  SECTION("Completed window is ready until retirement") {
    UiWindow window(runtime, platform);
    REQUIRE(platform.IsInitialized());
  }
  SECTION("Failed window never admits native callbacks") {
    fail_hook = true;
    REQUIRE_THROWS_AS(UiWindow(runtime, platform), std::runtime_error);
  }
  REQUIRE(hooks == 1);
  REQUIRE_FALSE(platform.IsInitialized());
  REQUIRE_THROWS_AS(UiWindow(runtime, platform), std::logic_error);
  REQUIRE_THROWS_AS(detail::MakePlatformChannelEndpoint(ui_window), std::logic_error);
}

TEST_CASE("Application hooks initialize once in order before windows and queued work") {
  TestPlatform platform;
  std::vector<int> starts;
  int window_installs = 0;
  std::shared_ptr<DemoViewModel> model;
  TaskScope tasks;
  int executed = 0;
  Application application(EmptyApplication, {
      .show_debug_overlay = false,
      .application_hooks = {
          [&](ApplicationContext& context) {
            starts.push_back(1);
            model = std::make_shared<DemoViewModel>();
            context.Provide(model);
            REQUIRE(UseService<DemoViewModel>() == model);
            REQUIRE_THROWS_AS(context.Provide(model), std::logic_error);
            tasks = UseApplicationTaskScope();
            tasks.Post([&] {
              REQUIRE(*UseService<int>() == 7);
              ++executed;
            });
            platform.RunPlatformModuleTasks();
            REQUIRE(executed == 0);
          },
          [&](ApplicationContext& context) {
            starts.push_back(2);
            REQUIRE(UseService<DemoViewModel>() == model);
            context.Provide(std::make_shared<int>(7));
            context.RegisterPlatformModule<int>("test/Startup", [](huxerui::Runtime&) { return 42; });
            platform.RunPlatformModuleTasks();
            REQUIRE(executed == 0);
          },
      },
      .window_hooks = {[&](WindowContext&) {
        ++window_installs;
        REQUIRE(starts == std::vector<int>{1, 2});
        REQUIRE(*UseService<int>() == 7);
        REQUIRE(OpenPlatformModule<int>("test/Startup") == 42);
      }},
  });
  RuntimeLifetime runtime(application, platform, std::nullopt);
  REQUIRE(starts == std::vector<int>{1, 2});
  REQUIRE(window_installs == 0);
  REQUIRE_FALSE(UseApplication().StartupActivation());
  REQUIRE(UseApplication().LifecycleState() == ApplicationLifecycleState::Background);
  REQUIRE(UseService<DemoViewModel>() == model);
  REQUIRE(executed == 0);
  platform.RunPlatformModuleTasks();
  REQUIRE(executed == 1);
  REQUIRE_THROWS_AS(RuntimeLifetime(application, platform), std::logic_error);
  TestPlatform first_platform;
  TestPlatform second_platform;
  UiWindow first(runtime, first_platform);
  UiWindow second(runtime, second_platform);
  REQUIRE(window_installs == 2);
  REQUIRE(starts == std::vector<int>{1, 2});
}

TEST_CASE("Application services may be acquired from a worker but State remains on its owner thread") {
  TestPlatform platform;
  Application application(EmptyApplication, {
      .show_debug_overlay = false,
      .application_hooks = {[](ApplicationContext& context) { context.Provide(std::make_shared<DemoViewModel>()); }},
  });
  RuntimeLifetime runtime(application, platform);
  const auto model = UseService<DemoViewModel>();
  std::shared_ptr<DemoViewModel> acquired;
  bool rejected_read = false;
  bool rejected_write = false;
  std::thread worker([&] {
    acquired = UseService<DemoViewModel>();
    try { static_cast<void>(acquired->value.Get()); } catch (const std::logic_error&) { rejected_read = true; }
    try { acquired->value = 1; } catch (const std::logic_error&) { rejected_write = true; }
  });
  worker.join();
  REQUIRE(acquired == model);
  REQUIRE(rejected_read);
  REQUIRE(rejected_write);
  REQUIRE(model->value.Get() == 0);
}

TEST_CASE("Application State observes multiple UIs and survives UI replacement") {
  TestPlatform platform;
  TestPlatform first_platform;
  TestPlatform second_platform;
  std::shared_ptr<DemoViewModel> model;
  Application application(SharedStateApplication, {
      .show_debug_overlay = false,
      .application_hooks = {
          [&](ApplicationContext& context) {
            model = std::make_shared<DemoViewModel>();
            context.Provide(model);
          },
      },
  });
  RuntimeLifetime runtime(application, platform);
  observed_values.clear();
  auto first = std::make_unique<UiWindow>(runtime, first_platform);
  auto second = std::make_unique<UiWindow>(runtime, second_platform);
  first->SetWindowMetrics({.viewport = {320, 240}});
  second->SetWindowMetrics({.viewport = {320, 240}});
  first->BuildFrame();
  second->BuildFrame();
  REQUIRE(observed_values == std::vector<int>{0, 0});
  model->value = 7;
  first->BuildFrame();
  second->BuildFrame();
  REQUIRE(observed_values == std::vector<int>{0, 0, 7, 7});
  first.reset();
  model->value = 9;
  second->BuildFrame();
  second.reset();
  model->value = 11;
  TestPlatform replacement_platform;
  first = std::make_unique<UiWindow>(runtime, replacement_platform);
  first->SetWindowMetrics({.viewport = {320, 240}});
  first->BuildFrame();
  REQUIRE(observed_values == std::vector<int>{0, 0, 7, 7, 9, 11});
}

TEST_CASE("Application tasks and delays run without frames and retire on shutdown") {
  TestPlatform platform;
  Application application(EmptyApplication, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, platform);
  const TaskScope tasks = UseApplicationTaskScope();
  int completed = 0;
  tasks.Launch([&]() -> Task<void> {
    co_await Delay(std::chrono::seconds(2));
    ++completed;
  });
  platform.RunPlatformModuleTasks();
  platform.AdvanceTime(1);
  platform.RunPlatformModuleTasks();
  REQUIRE(completed == 0);
  platform.AdvanceTime(1);
  platform.RunPlatformModuleTasks();
  REQUIRE(completed == 1);
  REQUIRE(platform.requested_frames == 0);
  tasks.Launch([&]() -> Task<void> {
    co_await Delay(std::chrono::seconds(1));
    ++completed;
  });
  platform.RunPlatformModuleTasks();
  runtime.RequestShutdown();
  runtime.RequestShutdown();
  REQUIRE(platform.application_quit_requests == 0);
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.application_quit_requests == 1);
  platform.AdvanceTime(5);
  platform.RunPlatformModuleTasks();
  REQUIRE(completed == 1);
  REQUIRE_THROWS_AS(tasks.Launch([]() -> Task<void> { co_return; }), std::logic_error);
}

TEST_CASE("Closed task scopes release captured Environment values") {
  struct TaskEnvironment {
    std::shared_ptr<TaskScope> tasks;
    static TaskEnvironment Default() { return {}; }
    bool operator==(const TaskEnvironment&) const = default;
  };
  TestPlatform platform;
  TestPlatform window_platform;
  State<bool> visible;
  TaskScope retained;
  std::weak_ptr<TaskScope> environment_tasks;
  event_context_root = [&]() -> View {
    visible = UseState(true);
    if (!visible.Get()) return Text("removed");
    auto tasks = std::make_shared<TaskScope>();
    environment_tasks = tasks;
    return ProvideEnvironment(TaskEnvironment{tasks}, Scope([&] {
      retained = UseTaskScope();
      // Model an Environment-owned object that retains the same scope as an external completion callback.
      *UseEnvironment<TaskEnvironment>().tasks = retained;
      return Text("child");
    }));
  };
  Application application(EventContextApplication, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, platform);
  auto window = std::make_unique<UiWindow>(runtime, window_platform);
  window->BuildFrame();
  REQUIRE_FALSE(environment_tasks.expired());

  SECTION("Unmount releases the captured Environment while the window remains alive") {
    visible = false;
    window->BuildFrame();
  }
  SECTION("Window retirement releases the captured Environment") {
    window.reset();
  }
  SECTION("A running Post retains its context until the callback returns") {
    retained.Post([&] {
      window.reset();
      REQUIRE_FALSE(environment_tasks.expired());
    });
    platform.RunPlatformModuleTasks();
  }
  SECTION("A running Task retains its context until deferred cancellation") {
    retained.Launch([&]() -> Task<void> {
      window.reset();
      REQUIRE_FALSE(environment_tasks.expired());
      co_await Delay(std::chrono::seconds(1));
      FAIL("A retired Task must not resume");
    });
    platform.RunPlatformModuleTasks();
  }

  REQUIRE(environment_tasks.expired());
  retained.Post([] { FAIL("A closed TaskScope must not deliver posts"); });
  REQUIRE_THROWS_AS(retained.Launch([]() -> Task<void> { co_return; }), std::logic_error);
  platform.AdvanceTime(2);
  platform.RunPlatformModuleTasks();
}

TEST_CASE("Application shutdown consistently retires windows before application tasks and services") {
  TestPlatform platform;
  TestPlatform window_platform;
  std::vector<std::string> cleanup_order;
  event_context_root = [&] {
    Lifecycle([&] {
      return [&] {
        cleanup_order.push_back("window:" + std::to_string(UseService<DemoViewModel>()->value.Get()));
      };
    });
    return Text("window");
  };
  Application application(EventContextApplication, {
      .show_debug_overlay = false,
      .application_hooks = {
          [&](ApplicationContext& context) {
            auto model = std::shared_ptr<DemoViewModel>(new DemoViewModel, [&](DemoViewModel* value) {
              cleanup_order.push_back("service");
              delete value;
            });
            model->value = 42;
            context.Provide(std::move(model));
          },
      },
  });
  const std::vector<std::string> expected{"window:42", "task:42", "service"};
  int expected_quit_requests = 0;
  {
    RuntimeLifetime runtime(application, platform);
    UiWindow window(runtime, window_platform);
    window.BuildFrame();
    UseApplicationTaskScope().Launch([&]() -> Task<void> {
      struct Cleanup {
        std::vector<std::string>& order;
        ~Cleanup() { order.push_back("task:" + std::to_string(UseService<DemoViewModel>()->value.Get())); }
      } cleanup{cleanup_order};
      co_await Delay(std::chrono::seconds(1));
      FAIL("Application shutdown must cancel the suspended Task");
    });
    platform.RunPlatformModuleTasks();
    REQUIRE(cleanup_order.empty());

    SECTION("Queued shutdown retires shared state before notifying the native host") {
      runtime.RequestShutdown();
      runtime.RequestShutdown();
      REQUIRE(cleanup_order.empty());
      REQUIRE(platform.application_quit_requests == 0);
      platform.RunPlatformModuleTasks();
      expected_quit_requests = 1;
    }
    SECTION("Direct retirement uses the same cleanup order") {
      detail::InternalAccess::RetireRuntime(runtime.Get());
    }
    SECTION("Direct retirement before queued shutdown suppresses the pending native stop") {
      runtime.RequestShutdown();
      detail::InternalAccess::RetireRuntime(runtime.Get());
    }
    REQUIRE(cleanup_order == expected);
    platform.AdvanceTime(2);
    platform.RunPlatformModuleTasks();
  }
  REQUIRE(cleanup_order == expected);
  REQUIRE(platform.application_quit_requests == expected_quit_requests);
}

TEST_CASE("Application activation and lifecycle delivery do not require a UI") {
  TestPlatform platform;
  std::vector<ApplicationActivation> activations;
  std::vector<ApplicationLifecycleState> states;
  std::vector<ApplicationLifecycleState> second_observer;
  Application application(EmptyApplication, {
      .show_debug_overlay = false,
      .application_hooks = {
          [&](ApplicationContext& context) {
            context.OnActivation([&](ApplicationActivation activation) {
              activations.push_back(std::move(activation));
            });
            context.OnLifecycleChanged([&](ApplicationLifecycleState state) { states.push_back(state); });
            context.OnLifecycleChanged([&](ApplicationLifecycleState state) { second_observer.push_back(state); });
          },
      },
  });
  RuntimeLifetime runtime(application, platform, UrlActivation{Uri("test:startup")});
  const auto handle = UseApplication();
  REQUIRE(std::get<UrlActivation>(*handle.StartupActivation()).url.ToString() == "test:startup");
  runtime.HandleApplicationActivation(UrlActivation{Uri("test:same")});
  runtime.HandleApplicationActivation(UrlActivation{Uri("test:same")});
  runtime.HandleApplicationActivation(UrlActivation{Uri("test:last")});
  runtime.UpdateApplicationLifecycleState(ApplicationLifecycleState::Inactive);
  runtime.UpdateApplicationLifecycleState(ApplicationLifecycleState::Active);
  runtime.UpdateApplicationLifecycleState(ApplicationLifecycleState::Active);
  REQUIRE(handle.LifecycleState() == ApplicationLifecycleState::Active);
  REQUIRE(activations.empty());
  REQUIRE(states.empty());
  platform.RunPlatformModuleTasks();
  REQUIRE(activations.size() == 3);
  REQUIRE(std::get<UrlActivation>(activations[0]).url.ToString() == "test:same");
  REQUIRE(std::get<UrlActivation>(activations[1]).url.ToString() == "test:same");
  REQUIRE(std::get<UrlActivation>(activations[2]).url.ToString() == "test:last");
  REQUIRE(states == std::vector{ApplicationLifecycleState::Inactive, ApplicationLifecycleState::Active});
  REQUIRE(second_observer == states);
  REQUIRE(platform.requested_frames == 0);
  REQUIRE_THROWS_AS(runtime.HandleApplicationActivation(FileActivation{}), std::invalid_argument);
  REQUIRE_THROWS_AS(runtime.HandleApplicationActivation(NotificationActivation{}), std::invalid_argument);
  REQUIRE_THROWS_AS(runtime.UpdateApplicationLifecycleState(static_cast<ApplicationLifecycleState>(-1)), std::invalid_argument);
}

TEST_CASE("Application directory queries report an unsupported host without blocking initialization") {
  TestPlatform platform;
  Application application(EmptyApplication, {.show_debug_overlay = false});
  std::optional<ApplicationHandle> handle;
  const File current_directory(std::filesystem::current_path().generic_u8string());
  {
    RuntimeLifetime runtime(application, platform);
    handle = UseApplication();
    REQUIRE_THROWS_AS(handle->Directories(), std::logic_error);
    REQUIRE(handle->CurrentDirectory() == current_directory);
  }
  REQUIRE_THROWS_AS(handle->Directories(), std::logic_error);
  REQUIRE(handle->CurrentDirectory() == current_directory);
}

TEST_CASE("Application preserves notification startup and subsequent activation payloads without a UI") {
  TestPlatform platform;
  std::vector<ApplicationActivation> activations;
  Application application(EmptyApplication, {
      .show_debug_overlay = false,
      .application_hooks = {
          [&](ApplicationContext& context) {
            context.OnActivation([&](ApplicationActivation activation) {
              activations.push_back(std::move(activation));
            });
          },
      },
  });
  RuntimeLifetime runtime(application, platform,
      NotificationActivation{"startup-notification", PlatformPayload::Object{{"id", 42}}});
  const auto handle = UseApplication();
  const auto& startup = std::get<NotificationActivation>(*handle.StartupActivation());
  REQUIRE(startup.identifier == "startup-notification");
  REQUIRE(startup.data.AsObject().at("id").AsInteger() == 42);
  platform.RunPlatformModuleTasks();
  REQUIRE(activations.empty());

  runtime.HandleApplicationActivation(NotificationActivation{"later-notification", PlatformPayload::Object{{"id", 43}}});
  REQUIRE(activations.empty());
  platform.RunPlatformModuleTasks();
  REQUIRE(activations.size() == 1);
  const auto& subsequent = std::get<NotificationActivation>(activations.front());
  REQUIRE(subsequent.identifier == "later-notification");
  REQUIRE(subsequent.data.AsObject().at("id").AsInteger() == 43);
  REQUIRE(startup.identifier == "startup-notification");
  REQUIRE(startup.data.AsObject().at("id").AsInteger() == 42);
  REQUIRE(platform.requested_frames == 0);

  REQUIRE_THROWS_AS(runtime.HandleApplicationActivation(NotificationActivation{"oversized", Bytes(65536)}),
                   std::invalid_argument);
  REQUIRE_THROWS_AS(runtime.HandleApplicationActivation(NotificationActivation{std::string("id\0x", 4)}),
                   std::invalid_argument);
  REQUIRE_THROWS_AS(
      runtime.HandleApplicationActivation(NotificationActivation{std::string(1, static_cast<char>(0xC3))}),
      std::invalid_argument);
  platform.RunPlatformModuleTasks();
  REQUIRE(activations.size() == 1);
}

TEST_CASE("Application rejects invalid startup activations") {
  TestPlatform platform;
  Application application(EmptyApplication, {.show_debug_overlay = false});
  SECTION("Empty notification identifier") {
    REQUIRE_THROWS_AS(RuntimeLifetime(application, platform, NotificationActivation{}), std::invalid_argument);
  }
  SECTION("Empty file payload") {
    REQUIRE_THROWS_AS(RuntimeLifetime(application, platform, FileActivation{}), std::invalid_argument);
  }
}

TEST_CASE("Application lifecycle changes recompose only subscribed scopes and ignore repeated values") {
  TestPlatform platform;
  int root_compositions = 0;
  int unrelated_compositions = 0;
  std::vector<ApplicationLifecycleState> observed;
  std::vector<ApplicationLifecycleState> transitions;
  event_context_root = [&] {
    ++root_compositions;
    return Column {
      Scope([&] {
        observed.push_back(UseApplication().LifecycleState());
        return Text("Lifecycle");
      }),
      Scope([&] {
        ++unrelated_compositions;
        return Text("Unrelated");
      }),
    };
  };
  Application application(EventContextApplication, {
      .show_debug_overlay = false,
      .application_hooks = {
          [&](ApplicationContext& context) {
            context.OnLifecycleChanged([&](ApplicationLifecycleState state) { transitions.push_back(state); });
          },
      },
  });
  RuntimeLifetime runtime(application, platform);
  UiWindow window(runtime, platform);
  window.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  window.BuildFrame();
  REQUIRE(observed == std::vector{ApplicationLifecycleState::Background});
  window.UpdateApplicationLifecycleState(ApplicationLifecycleState::Inactive);
  window.BuildFrame();
  REQUIRE(observed == std::vector{ApplicationLifecycleState::Background, ApplicationLifecycleState::Inactive});
  window.UpdateApplicationLifecycleState(ApplicationLifecycleState::Inactive);
  window.BuildFrame();
  REQUIRE(observed.size() == 2);

  window.UpdateApplicationLifecycleState(ApplicationLifecycleState::Background);
  window.UpdateApplicationLifecycleState(ApplicationLifecycleState::Active);
  window.UpdateApplicationLifecycleState(ApplicationLifecycleState::Active);
  window.BuildFrame();
  platform.RunPlatformModuleTasks();
  REQUIRE(observed == std::vector{ApplicationLifecycleState::Background, ApplicationLifecycleState::Inactive,
                                 ApplicationLifecycleState::Active});
  REQUIRE(transitions == std::vector{ApplicationLifecycleState::Inactive, ApplicationLifecycleState::Background,
                                    ApplicationLifecycleState::Active});
  REQUIRE(root_compositions == 1);
  REQUIRE(unrelated_compositions == 1);
}

TEST_CASE("Application defers reentrant events to the next dispatch turn") {
  std::deque<std::function<void()>> callbacks;
  TestPlatform platform([&](std::function<void()> callback) { callbacks.push_back(std::move(callback)); });
  huxerui::Runtime* active = nullptr;
  std::vector<std::string> received;
  Application application(EmptyApplication, {
      .show_debug_overlay = false,
      .application_hooks = {
          [&](ApplicationContext& context) {
            context.OnActivation([&](ApplicationActivation activation) {
              received.push_back(std::get<UrlActivation>(activation).url.ToString());
              if (received.size() == 1) {
                active->HandleApplicationActivation(UrlActivation{Uri("test:second")});
              }
            });
          },
      },
  });
  RuntimeLifetime runtime(application, platform);
  active = &runtime.Get();
  runtime.HandleApplicationActivation(UrlActivation{Uri("test:first")});
  auto callback = std::move(callbacks.front());
  callbacks.pop_front();
  callback();
  REQUIRE(received == std::vector<std::string>{"test:first"});
  REQUIRE(callbacks.size() == 1);
  callback = std::move(callbacks.front());
  callbacks.pop_front();
  callback();
  REQUIRE(received == std::vector<std::string>{"test:first", "test:second"});
}

TEST_CASE("Application dispatch preserves unrelated task resumes after a callback throws") {
  TestPlatform platform;
  int completed = 0;
  Application application(EmptyApplication, {
      .show_debug_overlay = false,
      .application_hooks = {
          [](ApplicationContext& context) {
            context.OnActivation([](ApplicationActivation) { throw std::runtime_error("test activation failure"); });
          },
      },
  });
  RuntimeLifetime runtime(application, platform);
  runtime.HandleApplicationActivation(LaunchActivation{});
  UseApplicationTaskScope().Launch([&]() -> Task<void> {
    ++completed;
    co_return;
  });
  REQUIRE_THROWS_AS(platform.RunPlatformModuleTasks(), std::runtime_error);
  REQUIRE(completed == 1);
  runtime.RequestShutdown();
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.application_quit_requests == 1);
}

TEST_CASE("A rejected application wake rolls back its callback without losing concurrent posts") {
  std::deque<std::function<void()>> callbacks;
  std::function<void()> reject_wake;
  UiThreadDispatcher dispatcher;
  std::vector<int> delivered;
  TestPlatform platform([&](std::function<void()> callback) {
    if (auto reject = std::exchange(reject_wake, {})) {
      reject();
      throw std::runtime_error("test native wake rejection");
    }
    callbacks.push_back(std::move(callback));
  });
  Application application(EmptyApplication, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, platform, std::nullopt);
  dispatcher = detail::CurrentApplicationRuntime()->Dispatcher();
  reject_wake = [&] {
    std::thread worker([&] { dispatcher([&] { delivered.push_back(1); }); });
    worker.join();
  };
  auto capture = std::shared_ptr<int>(new int(0), [&](int* value) {
    delete value;
    dispatcher([&] { delivered.push_back(2); });
  });
  REQUIRE_THROWS_AS(dispatcher([capture = std::move(capture), &delivered] { delivered.push_back(-1); }),
                    std::runtime_error);
  dispatcher([&] { delivered.push_back(3); });
  REQUIRE(delivered.empty());
  REQUIRE(callbacks.size() == 1);
  auto callback = std::move(callbacks.front());
  callbacks.pop_front();
  callback();
  REQUIRE(delivered == std::vector<int>{1, 2, 3});
  REQUIRE(callbacks.empty());
}

TEST_CASE("An in-flight native wake remains safe while its application Runtime is destroyed") {
  struct NativeQueue {
    std::mutex mutex;
    std::deque<std::function<void()>> callbacks;
    std::promise<void> entered;
    std::shared_future<void> release;
  };
  auto queue = std::make_shared<NativeQueue>();
  std::promise<void> release;
  queue->release = release.get_future().share();
  auto entered = queue->entered.get_future();
  const auto application_thread = std::this_thread::get_id();
  auto platform = std::make_unique<TestPlatform>([queue, application_thread](std::function<void()> callback) {
    if (std::this_thread::get_id() != application_thread) {
      queue->entered.set_value();
      queue->release.wait();
    }
    std::lock_guard lock(queue->mutex);
    queue->callbacks.push_back(std::move(callback));
  });
  Application application(EmptyApplication, {.show_debug_overlay = false});
  auto runtime = std::make_unique<RuntimeLifetime>(application, *platform);
  auto dispatcher = detail::CurrentApplicationRuntime()->Dispatcher();
  int delivered = 0;
  std::thread worker([&] { dispatcher([&] { ++delivered; }); });
  const bool posted = entered.wait_for(5s) == std::future_status::ready;
  if (posted) {
    runtime.reset();
    platform.reset();
  }
  release.set_value();
  worker.join();
  REQUIRE(posted);
  TestPlatform replacement_platform;
  RuntimeLifetime replacement(application, replacement_platform);
  for (auto& callback : queue->callbacks) callback();
  dispatcher([&] { ++delivered; });
  REQUIRE(delivered == 0);
}

TEST_CASE("Application shutdown stops reentrant activation and lifecycle delivery") {
  TestPlatform platform;
  int activations = 0;
  int lifecycle_events = 0;
  Application application(EmptyApplication, {
      .show_debug_overlay = false,
      .application_hooks = {
          [&](ApplicationContext& context) {
            context.OnActivation([&](ApplicationActivation) {
              ++activations;
              UseApplication().Quit();
            });
            context.OnLifecycleChanged([&](ApplicationLifecycleState) { ++lifecycle_events; });
          },
      },
  });
  RuntimeLifetime runtime(application, platform);
  runtime.HandleApplicationActivation(LaunchActivation{});
  runtime.HandleApplicationActivation(LaunchActivation{});
  runtime.UpdateApplicationLifecycleState(ApplicationLifecycleState::Active);
  platform.RunPlatformModuleTasks();
  REQUIRE(activations == 1);
  REQUIRE(lifecycle_events == 0);
  REQUIRE(platform.application_quit_requests == 1);
}

TEST_CASE("Application hook failure stops installation and releases earlier services and queued work") {
  TestPlatform platform;
  std::weak_ptr<DemoViewModel> model;
  int callbacks = 0;
  int later_hooks = 0;
  ApplicationHook failing_hook;
  SECTION("An installer throws") {
    failing_hook = [](ApplicationContext&) { throw std::runtime_error("test initialization failure"); };
  }
  SECTION("An installer is empty") {}
  Application failing(EmptyApplication, {
      .show_debug_overlay = false,
      .application_hooks = {
          [&](ApplicationContext& context) {
            auto service = std::make_shared<DemoViewModel>();
            model = service;
            context.Provide(service);
            UseApplicationTaskScope().Post([&] { ++callbacks; });
          },
          failing_hook,
          [&](ApplicationContext&) { ++later_hooks; },
      },
  });
  if (failing_hook) {
    REQUIRE_THROWS_WITH(RuntimeLifetime(failing, platform), "test initialization failure");
  } else {
    REQUIRE_THROWS_AS(RuntimeLifetime(failing, platform), std::invalid_argument);
  }
  platform.RunPlatformModuleTasks();
  REQUIRE(callbacks == 0);
  REQUIRE(later_hooks == 0);
  REQUIRE(model.expired());
  REQUIRE_THROWS_AS(UseService<DemoViewModel>(), std::logic_error);
  Application valid(EmptyApplication, {.show_debug_overlay = false});
  TestPlatform valid_platform;
  RuntimeLifetime runtime(valid, valid_platform);
}

TEST_CASE("State retains its original application lifetime after shutdown") {
  State<int> retained;
  TestPlatform platform;
  Application application(EmptyApplication, {.show_debug_overlay = false});
  {
    RuntimeLifetime runtime(application, platform);
    retained = State<int>(42);
    REQUIRE(retained.Get() == 42);
  }
  REQUIRE_THROWS_AS(retained.Get(), std::logic_error);
  REQUIRE_THROWS_AS(retained = 8, std::logic_error);
  TestPlatform replacement_platform;
  RuntimeLifetime replacement(application, replacement_platform);
  REQUIRE_THROWS_AS(retained.Get(), std::logic_error);
}

TEST_CASE("Repeated UI attachments bound stale registrations and preserve live window shutdown") {
  class WindowPlatform final : public TestPlatform {
  public:
    using huxerui::UiWindow::IsInitialized;
  };
  TestPlatform platform;
  WindowPlatform live_platform;
  WindowPlatform retired_platform;
  Application application(EmptyApplication, {.show_debug_overlay = false});
  RuntimeLifetime runtime(application, platform);
  const auto state = detail::CurrentApplicationRuntime();
  UiWindow live(runtime, live_platform);
  {
    UiWindow retired(runtime, retired_platform);
  }
  for (int index = 0; index < 32; ++index) {
    WindowPlatform temporary_platform;
    UiWindow temporary(runtime, temporary_platform);
    REQUIRE(state->uis.size() <= 2);
    REQUIRE(live_platform.IsInitialized());
    REQUIRE_FALSE(retired_platform.IsInitialized());
  }
  WindowPlatform replacement_platform;
  UiWindow replacement(runtime, replacement_platform);
  runtime.RequestShutdown();
  platform.RunPlatformModuleTasks();
  REQUIRE_FALSE(live_platform.IsInitialized());
  REQUIRE_FALSE(replacement_platform.IsInitialized());
  REQUIRE(platform.application_quit_requests == 1);
}

TEST_CASE("UI resource configuration and window lifecycle are available before root installation") {
  TestPlatform platform;
  TestPlatform first_platform;
  TestPlatform second_platform;
  std::vector<float> scales;
  std::vector<WindowLifecycleState> states;
  Application application(EmptyApplication, {
      .show_debug_overlay = false,
      .window_hooks = {
          [&](WindowContext&) {
            scales.push_back(UseService<Resources>()->Configuration().display_scale);
            states.push_back(UseWindow().LifecycleState());
          },
      },
  });
  RuntimeLifetime runtime(application, platform);
  UiWindow first(runtime, first_platform, ResourceConfiguration{.display_scale = 2.0F},
      WindowLifecycleState::Active);
  UiWindow second(runtime, second_platform, ResourceConfiguration{.display_scale = 3.0F},
      WindowLifecycleState::Inactive);
  REQUIRE(scales == std::vector<float>{2.0F, 3.0F});
  REQUIRE(states == std::vector{WindowLifecycleState::Active, WindowLifecycleState::Inactive});
  REQUIRE(UseService<Resources>()->Configuration().display_scale == 1.0F);
}

} // namespace
} // namespace huxerui::test
