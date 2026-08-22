#include <huxerui/huxerui.h>

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#include "application_windows.h"
#endif

using namespace huxerui;

namespace {

struct TextFilePreview {
  std::string file_name;
  std::string content{"Open a .txt file with this application to preview its contents."};

  bool operator==(const TextFilePreview&) const = default;
};

#if defined(_WIN32)
const bool browser_scheme_registered = example::RegisterApplicationExampleUrlScheme();
#endif

std::string PlatformActivationHint() {
#if defined(_WIN32)
  if (browser_scheme_registered) {
    return "Open huxerui-example://documents/42 in a browser to deliver a URL activation.";
  }
  return "The huxerui-example URL protocol could not be registered for the current Windows user.";
#elif defined(__ANDROID__)
  return "Open huxerui-example://documents/42, or choose HuxerUI Application when another app opens a file.";
#else
  return "Platform application shells may map external URLs and files into this activation model.";
#endif
}

std::string PlatformActivationSummary() {
#if defined(_WIN32)
  return "Windows accepts one URL or a command line containing only existing files.";
#elif defined(__ANDROID__)
  return "Android maps ACTION_VIEW and ACTION_EDIT URLs or document URIs into the current Activity Runtime.";
#else
  return "The current platform uses the shared application activation boundary when its shell provides a mapping.";
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

std::string DescribeLifecycleState(ApplicationLifecycleState state) {
  switch (state) {
  case ApplicationLifecycleState::Active:
    return "active";
  case ApplicationLifecycleState::Inactive:
    return "inactive";
  case ApplicationLifecycleState::Background:
    return "background";
  }
  return "unknown";
}

bool IsTextFileName(std::string_view name) {
  constexpr std::string_view extension = ".txt";
  if (name.size() < extension.size()) {
    return false;
  }
  const std::string_view suffix = name.substr(name.size() - extension.size());
  for (std::size_t index = 0; index < extension.size(); ++index) {
    if (static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[index]))) != extension[index]) {
      return false;
    }
  }
  return true;
}

std::optional<FileReference> ActivatedTextFile(const ApplicationActivation& activation) {
  const auto* files = std::get_if<FileActivation>(&activation);
  if (files == nullptr) {
    return std::nullopt;
  }
  for (const FileReference& file : files->files) {
    if (IsTextFileName(file.Name())) {
      return file;
    }
  }
  return std::nullopt;
}

void UpdateTextFilePreview(
    const ApplicationActivation& activation,
    TaskScope tasks,
    State<TextFilePreview> preview,
    State<std::uint64_t> generation
) {
  const std::uint64_t current_generation = generation.Get() + 1;
  generation = current_generation;

  std::optional<FileReference> file = ActivatedTextFile(activation);
  if (!file) {
    preview = TextFilePreview{};
    return;
  }

  const std::string file_name = file->Name();
  preview = {file_name, "Reading text content..."};
  tasks.Launch([file = std::move(*file), file_name, preview, generation, current_generation]() -> Task<void> {
    FileResult<std::string> result = co_await file.ReadStringAsync();
    if (generation.Get() != current_generation) {
      co_return;
    }
    if (!result.Succeeded()) {
      preview = {file_name, "Read failed: " + result.Error().message};
      co_return;
    }
    std::string content = std::move(result).Value();
    preview = {file_name, content.empty() ? "The file is empty." : std::move(content)};
  });
}

View TextFilePreviewCard(const TextFilePreview& preview) {
  const ThemeSpec& theme = UseTheme();
  return Column {
    Text("Text file preview", TextRole::Title),
    Text(preview.file_name.empty() ? "No .txt file activated" : preview.file_name, TextRole::Label)
        .With(Foreground(theme.colors.primary)),
    SelectionArea {
      Text(preview.content),
    },
  }.With(
      Padding(theme.spacing.large),
      Spacing(theme.spacing.small),
      Background(theme.colors.surface_container_low),
      CornerRadius(theme.shapes.medium)
  );
}

[[huxerui::scope]] View ApplicationContent() {
  const ApplicationHandle application = UseApplication();
  const ApplicationActivation startup_activation = application.StartupActivation();
  const ApplicationLifecycleState lifecycle_state = application.LifecycleState();
  auto tasks = UseTaskScope();
  auto lifecycle_transitions =
      UseStateList<std::string>({"Initially observed: " + DescribeLifecycleState(lifecycle_state)});
  auto activations = UseStateList<std::string>({"Startup: " + DescribeActivation(startup_activation)});
  auto preview = UseState(TextFilePreview{});
  auto preview_generation = UseState<std::uint64_t>(0);

  Lifecycle([=] { UpdateTextFilePreview(startup_activation, tasks, preview, preview_generation); });
  application.OnLifecycleChange([=](ApplicationLifecycleState state) {
    lifecycle_transitions.PushBack("Transition: " + DescribeLifecycleState(state));
  });
  application.OnActivation([=](ApplicationActivation activation) {
    activations.PushBack("Subsequent: " + DescribeActivation(activation));
    UpdateTextFilePreview(activation, tasks, preview, preview_generation);
  });

  const ThemeSpec& theme = UseTheme();
  return ScrollView {
    Column {
      Text("Application lifecycle and activation", TextRole::Title),
      Text("Current lifecycle: " + DescribeLifecycleState(lifecycle_state), TextRole::Label)
          .With(Foreground(theme.colors.primary)),
      Text("Ordered lifecycle transitions remain available after the application returns to the foreground."),
      Column {
        ForEach(lifecycle_transitions, [](const std::string& transition) { return Text(transition); }),
      }.With(
          Padding(theme.spacing.large),
          Spacing(theme.spacing.small),
          Background(theme.colors.surface_container_low),
          CornerRadius(theme.shapes.medium)
      ),
      Text(
          "StartupActivation is immutable for this Runtime. Later platform activations enter the same application "
          "policy through OnActivation."
      ),
      Text(PlatformActivationSummary()),
      Text(PlatformActivationHint()),
      Column {
        ForEach(activations, [](const std::string& activation) { return Text(activation); }),
      }.With(
          Padding(theme.spacing.large),
          Spacing(theme.spacing.small),
          Background(theme.colors.surface_container_low),
          CornerRadius(theme.shapes.medium)
      ),
      TextFilePreviewCard(preview),
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
