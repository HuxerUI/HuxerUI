#include <huxerui/huxerui.h>

#include <string>
#include <utility>

using namespace huxerui;

struct LoadingState {
  bool loading {false};
  std::string status {"Ready"};
  std::string body {"Press Send to perform an HTTPS GET request."};
};

[[huxerui::scope]]
View HttpContent() {
  auto http = UseService<HttpClient>();
  auto tasks = UseTaskScope();
  auto loading = UseState(LoadingState{});
  auto& theme = UseTheme();

  return ScrollView {
    Column {
      Text("HTTP", TextRole::Title),
      Text("HttpClient performs a platform request and resumes its Task on the owning UI thread."),
      Text("https://httpbingo.org/get", TextRole::Label).With(Foreground(theme.colors.primary)),
      Button(loading->loading ? "Loading..." : "Send").With(Enabled(!loading->loading)).OnClick([=] {
        tasks.Launch([=]() -> Task<void> {
          loading = {true, "Loading", "Waiting for the response..."};

          HttpResult result = co_await http->Send({
              .url = "https://httpbingo.org/get",
              .headers = {{"Accept", "application/json"}},
          });
          if (result.HasResponse()) {
            HttpResponse& response = result.Response();
            loading = {
              false, "HTTP " + std::to_string(response.status_code) + " · " + response.url,
              response.body.empty() ? "(empty response body)" : std::move(response.body)
            };
          } else {
            loading = {false, "Request failed", result.Error().message};
          }
        });
      }),
      Text(loading->status, TextRole::Label),
      SelectionArea {
        Text(loading->body),
      }.With(
          Padding(theme.spacing.large),
          Background(theme.colors.surface_container_low),
          CornerRadius(theme.shapes.medium)
      ),
    }.With(
        Padding(theme.spacing.extra_large),
        Spacing(theme.spacing.medium),
        CrossAlign(CrossAxisAlignment::Stretch)
    ),
  }.With(
      ScrollBar(),
      Background(theme.colors.background)
  );
}

View App() {
  return MaterialTheme {HttpContent()};
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI HTTP",
            .initial_size = {760.0F, 620.0F},
        },
    }
};
