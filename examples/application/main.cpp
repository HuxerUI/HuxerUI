#include <huxerui/huxerui.h>

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#include "application_windows.h"
#endif

using namespace huxerui;

namespace {

struct TextFilePreview {
  std::string file_name;
  std::string content{"Open a UTF-8 text document with this application to preview its contents."};

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
#elif defined(__APPLE__)
  return "Open huxerui-example://documents/42, or open a UTF-8 text document with HuxerUI Application.";
#else
  return "Platform application shells may map external URLs and files into this activation model.";
#endif
}

std::string PlatformActivationSummary() {
#if defined(_WIN32)
  return "Windows accepts one URL or a command line containing only existing files.";
#elif defined(__ANDROID__)
  return "Android maps ACTION_VIEW and ACTION_EDIT URLs or document URIs into the current Activity Runtime.";
#elif defined(__APPLE__)
  return "Apple application callbacks map registered URL schemes and text documents into the current Runtime.";
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
          return "URL: " + value.url.ToString();
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

std::string DescribePermissionStatus(PermissionStatus status) {
  switch (status) {
  case PermissionStatus::NotDetermined:
    return "not determined";
  case PermissionStatus::Granted:
    return "granted";
  case PermissionStatus::Denied:
    return "denied";
  case PermissionStatus::PermanentlyDenied:
    return "permanently denied";
  case PermissionStatus::Restricted:
    return "restricted";
  case PermissionStatus::Unavailable:
    return "unavailable";
  }
  return "unknown";
}

std::optional<FileReference> ActivatedFile(const ApplicationActivation& activation) {
  const auto* files = std::get_if<FileActivation>(&activation);
  if (files == nullptr || files->files.empty()) {
    return std::nullopt;
  }
  return files->files.front();
}

void UpdateTextFilePreview(
    const ApplicationActivation& activation,
    TaskScope tasks,
    State<TextFilePreview> preview,
    State<std::uint64_t> generation
) {
  const std::uint64_t current_generation = generation.Get() + 1;
  generation = current_generation;

  std::optional<FileReference> file = ActivatedFile(activation);
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

[[huxerui::composable]]
View TextFilePreviewCard(const TextFilePreview& preview) {
  const ThemeSpec& theme = UseTheme();
  return Column {
    Text("Text file preview", TextRole::Title),
    Text(preview.file_name.empty() ? "No text document activated" : preview.file_name, TextRole::Label)
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

[[huxerui::composable]]
View PermissionCard(const ApplicationHandle& application, TaskScope tasks) {
  auto status = UseState(std::string{"not checked"});
  const ThemeSpec& theme = UseTheme();
  return Column {
    Text("Runtime permission", TextRole::Title),
    Text("Camera: " + status.Get(), TextRole::Label).With(Foreground(theme.colors.primary)),
    Text("The native application shell owns privacy declarations and final permission policy."),
    Button("Check camera").OnClick([=] {
      tasks.Launch([=]() -> Task<void> {
        status = DescribePermissionStatus(co_await application.CheckPermissionAsync(Permission::Camera));
      });
    }),
    Button("Request camera").OnClick([=] {
      tasks.Launch([=]() -> Task<void> {
        status = DescribePermissionStatus(co_await application.RequestPermissionAsync(Permission::Camera));
      });
    }),
    Button("Open permission settings").OnClick([=] {
      tasks.Launch([=]() -> Task<void> {
        const bool opened = co_await application.OpenPermissionSettingsAsync(Permission::Camera);
        if (!opened) {
          status = "settings unavailable";
        }
      });
    }),
  }.With(
      Padding(theme.spacing.large),
      Spacing(theme.spacing.small),
      CrossAlign(CrossAxisAlignment::Stretch),
      Background(theme.colors.surface_container_low),
      CornerRadius(theme.shapes.medium)
  );
}

[[huxerui::composable]] View ApplicationContent() {
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
      PermissionCard(application, tasks),
    }.With(
        Padding(theme.spacing.extra_large),
        Spacing(theme.spacing.medium),
        CrossAlign(CrossAxisAlignment::Stretch)
    ),
  }.With(Background(theme.colors.background));
}

View App() {
  return MaterialTheme {
    ApplicationContent(),
  };
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
