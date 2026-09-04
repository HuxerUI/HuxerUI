#include <huxerui/huxerui.h>

#include <memory>
#include <optional>
#include <string>

using namespace huxerui;

[[huxerui::composable]] View DirectoryCopyContent() {
  auto picker = UseService<FilePicker>();
  auto files = UseService<FileSystem>();
  auto tasks = UseTaskScope();
  auto source = UseState(std::optional<FileReference>{});
  auto destination = UseState(std::optional<FileReference>{});
  auto use_app_directory = UseState(true);
  auto overwrite = UseState(false);
  auto busy = UseState(false);
  auto active = UseState(std::shared_ptr<TaskHandle>{});
  auto status = UseState(std::string("Choose a source directory, choose a destination, then copy."));
  const File local = files->Directories().data_directory.Child("copied-directory");
  const ThemeSpec& theme = UseTheme();

  return ScrollView {
    Column {
      Text("Copy directory contents", TextRole::Title),
      Text("Names, file bytes, hidden items, and empty directories are preserved. Existing directories merge."),
      Button(source.Get() ? "Source: " + source.Get()->Name() : "Choose source")
          .With(Enabled(!busy.Get() && picker->CanOpenDirectories()))
          .OnClick([=] {
            tasks.Launch([=]() -> Task<void> {
              auto selected = co_await picker->OpenDirectoryAsync();
              if (selected) { source = std::move(selected); }
            });
          }),
      Button(destination.Get() ? "Destination: " + destination.Get()->Name() : "Choose destination")
          .With(Enabled(!busy.Get() && picker->CanOpenDirectories(true)))
          .OnClick([=] {
            tasks.Launch([=]() -> Task<void> {
              auto selected = co_await picker->OpenDirectoryAsync(true);
              if (selected) { destination = std::move(selected); use_app_directory = false; }
            });
          }),
      Button(use_app_directory.Get() ? "Destination: application storage" : "Use application storage")
          .With(Enabled(!busy.Get()))
          .OnClick([=] { use_app_directory = true; }),
      Text(use_app_directory.Get() ? local.Path() : "Using the separately selected writable directory."),
      Button(overwrite.Get() ? "Overwrite existing files: on" : "Overwrite existing files: off")
          .With(Enabled(!busy.Get()))
          .OnClick([=] { overwrite = !overwrite.Get(); }),
      Flow {
        Button("Copy contents")
            .With(Enabled(!busy.Get() && source.Get().has_value() &&
                          (use_app_directory.Get() || destination.Get().has_value())))
            .OnClick([=] {
              busy = true;
              status = "Copying...";
              auto task = tasks.Launch([=, input = *source.Get(), output = destination.Get(),
                  app_directory = use_app_directory.Get(), replace = overwrite.Get()]() -> Task<void> {
                if (app_directory && !(co_await local.CreateDirectoriesAsync())) {
                  status = "The application destination could not be created.";
                  busy = false;
                  co_return;
                }
                auto result = app_directory ? co_await input.CopyDirectoryContentsToAsync(local, replace)
                                            : co_await input.CopyDirectoryContentsToAsync(*output, replace);
                if (result.Succeeded()) {
                  const auto& summary = result.Value();
                  status = "Copied " + std::to_string(summary.files_copied) + " files, created " +
                      std::to_string(summary.directories_created) + " directories, transferred " +
                      std::to_string(summary.bytes_copied) + " bytes.";
                } else {
                  status = result.Error().message;
                }
                busy = false;
                active = {};
              });
              active = std::make_shared<TaskHandle>(std::move(task));
            }),
        Button("Cancel").With(Enabled(busy.Get())).OnClick([=] {
          if (auto task = active.Get()) { task->Cancel(); }
          active = {};
          busy = false;
          status = "Cancellation requested. Completed output remains; an in-flight provider write may still finish.";
        }),
      }.With(Spacing(theme.spacing.small)),
      Text(status),
      Text("Failure or cancellation does not roll back completed output. Unsupported containment checks fail before copying."),
    }.With(Padding(theme.spacing.large), Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch)),
  }.With(ScrollBar(), Background(theme.colors.background));
}

View App() { return MaterialTheme {DirectoryCopyContent()}; }

const Application application{App, {.window = {.title = "HuxerUI Directory Copy", .initial_size = {720.0F, 640.0F}}}};
