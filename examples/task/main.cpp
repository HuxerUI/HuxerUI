#include <huxerui/huxerui.h>

#include <memory>
#include <stdexcept>
#include <string>

using namespace huxerui;

using UserId = int;

int CountPrimes(int limit) {
  int count = 0;
  for (int candidate = 2; candidate <= limit; ++candidate) {
    bool prime = true;
    for (int divisor = 2; divisor <= candidate / divisor; ++divisor) {
      if (candidate % divisor == 0) {
        prime = false;
        break;
      }
    }
    if (prime) {
      ++count;
    }
  }
  return count;
}

class UserService {
public:
  Task<std::string> LoadName(UserId user_id) {
    co_await Delay(500ms);
    co_return user_id == 1 ? "Ada Lovelace" : "Grace Hopper";
  }
};

[[huxerui::composable]]
View UserName(UserId user_id, std::shared_ptr<UserService> service) {
  auto tasks = UseTaskScope();
  auto name = UseState(std::string{"Loading..."});

  Lifecycle([=] {
    name = "Loading...";
    TaskHandle request = tasks.Launch([=]() -> Task<void> {
      name = co_await service->LoadName(user_id);
    });

    return [request] {
      request.Cancel();
    };
  }, user_id);

  const ThemeSpec& theme = UseTheme();
  return Column {
    Text(name, TextRole::Title),
    Text("The loaded value is written directly to State on the owning UI thread."),
    Button("Reload").OnClick([=] {
      tasks.Launch([=]() -> Task<void> {
        name = co_await service->LoadName(user_id);
      });
    }),
  }.With(
      Padding(theme.spacing.large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.surface_container_low),
      CornerRadius(theme.shapes.medium)
  );
}

[[huxerui::composable]]
View TaskContent(std::shared_ptr<UserService> service) {
  auto tasks = UseTaskScope();
  auto user_id = UseState<UserId>(1);
  auto worker_status = UseState(std::string{"Worker has not run."});
  auto posted_updates = UseState(0);
  const ThemeSpec& theme = UseTheme();

  return ScrollView {
    Column {
      Text("Task and structured concurrency", TextRole::Title),
      Text(
          "TaskScope owns launched work for the component scope. Lifecycle explicitly cancels the request when its "
          "user dependency changes."
      ),
      Row {
        Button("Ada").OnClick([user_id] { user_id = 1; }),
        Button("Grace").OnClick([user_id] { user_id = 2; }),
      }.With(
          Spacing(theme.spacing.medium),
          CrossAlign(CrossAxisAlignment::Center)
      ),
      UserName(user_id, service),
      Text("Worker and UI dispatch", TextRole::Title),
      Text(
          "RunWorker uses the shared bounded worker executor. Post schedules an owned callback on this component's "
          "UI scope."
      ),
      Text(worker_status),
      Text::Format("Posted UI updates: {}", posted_updates),
      Row {
        Button("Run worker").OnClick([=] {
          worker_status = "Running...";
          tasks.Launch([=]() -> Task<void> {
            try {
              const int count = co_await RunWorker(CountPrimes, 25000);
              worker_status = std::to_string(count) + " primes found.";
            } catch (const std::runtime_error&) {
              worker_status = "Worker execution is unavailable in this build.";
            }
          });
        }),
        Button("Post UI update").OnClick([=] {
          tasks.Post([posted_updates] { posted_updates += 1; });
        }),
      }.With(
          Spacing(theme.spacing.medium),
          CrossAlign(CrossAxisAlignment::Center)
      ),
    }.With(
        Padding(theme.spacing.extra_large),
        Spacing(theme.spacing.large),
        CrossAlign(CrossAxisAlignment::Stretch)
    ),
  }.With(
      ScrollBar(),
      Background(theme.colors.background)
  );
}

View App() {
  static const auto service = std::make_shared<UserService>();
  return MaterialTheme {TaskContent(service)};
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI Task",
            .initial_size = {720.0F, 520.0F},
        },
    }
};
