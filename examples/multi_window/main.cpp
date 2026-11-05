#include <huxerui/huxerui.h>

#include <memory>
#include <optional>
#include <string>

using namespace huxerui;

struct SharedCounter {
  State<int> value{0};
};

[[huxerui::composable]]
View CounterPanel(std::shared_ptr<SharedCounter> model) {
  auto local = UseState(0);
  auto window = UseWindow();
  return Column {
    Text("Shared: " + std::to_string(model->value)),
    Button("Increment shared").OnClick([model] { model->value += 1; }),
    Text("This window: " + std::to_string(local)),
    Button("Increment local").OnClick([local] { local += 1; }),
    Button("Close this window").OnClick([window] { window.Close(); }),
  }.With(Padding(24.0F), Spacing(12.0F));
}

View App() {
  auto application = UseApplication();
  auto model = UseService<SharedCounter>();
  auto secondary = UseState(std::optional<WindowHandle>{});
  const std::optional<WindowHandle>& secondary_window = secondary;
  const bool open = secondary_window && secondary_window->IsOpen();
  return Column {
    Text("Multiple windows"),
    CounterPanel(model),
    Text(open ? "Secondary window is open" : "Secondary window is closed"),
    Button(open ? "Activate secondary" : "Open secondary").OnClick([application, model, secondary] {
      const std::optional<WindowHandle>& current = secondary;
      if (current && current->IsOpen()) {
        current->Activate();
      } else {
        secondary = application.OpenWindow([model] { return CounterPanel(model); }, {
            .title = "Independent counter",
            .initial_size = {380.0F, 320.0F},
        });
      }
    }).With(Enabled(application.SupportsMultipleWindows())),
    Button("Quit application").OnClick([application] { application.Quit(); }),
  }.With(Padding(24.0F), Spacing(12.0F));
}

const Application application{
    App,
    {
        .window = {.title = "HuxerUI Multiple Windows", .initial_size = {480.0F, 520.0F}},
        .application_hooks = {[](ApplicationContext& context) {
          context.Provide(std::make_shared<SharedCounter>());
        }},
    },
};
