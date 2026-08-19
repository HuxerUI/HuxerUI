#include <huxerui/huxerui.h>

#include <memory>
#include <string>

using namespace huxerui;

using UserId = int;

class UserService {
public:
  Task<std::string> LoadName(UserId user_id) {
    co_await Delay(500ms);
    co_return user_id == 1 ? "Ada Lovelace" : "Grace Hopper";
  }
};

[[huxerui::scope]]
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

[[huxerui::scope]]
View TaskContent(std::shared_ptr<UserService> service) {
  auto user_id = UseState<UserId>(1);
  const ThemeSpec& theme = UseTheme();

  return Column {
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
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.large),
      CrossAlign(CrossAxisAlignment::Stretch),
      Background(theme.colors.background)
  );
}

View App() {
  static const auto service = std::make_shared<UserService>();
  return MaterialTheme(TaskContent, service);
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
