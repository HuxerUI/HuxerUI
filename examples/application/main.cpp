#include <huxerui/huxerui.h>

#include <string>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#include "application_windows.h"
#endif

using namespace huxerui;

namespace {

#if defined(_WIN32)
const bool browser_scheme_registered = example::RegisterApplicationExampleUrlScheme();
#endif

std::string BrowserActivationHint() {
#if defined(_WIN32)
  if (browser_scheme_registered) {
    return "Open huxerui-example://documents/42 in a browser to deliver a URL activation.";
  }
  return "The huxerui-example URL protocol could not be registered for the current Windows user.";
#else
  return "Platform application shells may map external URLs and files into this activation model.";
#endif
}

std::string DescribeActivation(const ApplicationActivation& activation) {
  return std::visit(
      [](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, LaunchActivation>) {
          return std::string{"ordinary launch"};
        } else if constexpr (std::is_same_v<Value, UrlActivation>) {
          return "URL: " + value.url;
        } else {
          std::string result = "files:";
          for (const FileReference& file : value.files) {
            result += " " + file.Name();
          }
          return result;
        }
      },
      activation
  );
}

[[huxerui::scope]] View ApplicationContent() {
  const ApplicationHandle application = UseApplication();
  auto activations = UseStateList<std::string>({"Startup: " + DescribeActivation(application.StartupActivation())});
  application.OnActivation([activations](ApplicationActivation activation) {
    activations.PushBack("Subsequent: " + DescribeActivation(activation));
  });

  const ThemeSpec& theme = UseTheme();
  return ScrollView {
    Column {
      Text("Application activation", TextRole::Title),
      Text(
          "StartupActivation is immutable for this Runtime. Later platform activations enter the same application "
          "policy through OnActivation."
      ),
      Text("Windows accepts one URL or a command line containing only existing files."),
      Text(BrowserActivationHint()),
      Column {
        ForEach(activations, [](const std::string& activation) { return Text(activation); }),
      }.With(
          Padding(theme.spacing.large),
          Spacing(theme.spacing.small),
          Background(theme.colors.surface_container_low),
          CornerRadius(theme.shapes.medium)
      ),
    }.With(
        Padding(theme.spacing.extra_large),
        Spacing(theme.spacing.medium),
        CrossAlign(CrossAxisAlignment::Stretch)
    ),
  }.With(Background(theme.colors.background));
}

View App() {
  return MaterialTheme(ApplicationContent);
}

AppOptions Options() {
  AppOptions options;
  options.window = {
      .title = "HuxerUI Application",
      .initial_size = {720.0F, 520.0F},
  };
  return options;
}

} // namespace

const Application application{App, Options()};
