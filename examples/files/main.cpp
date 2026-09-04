#include <huxerui/huxerui.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

using namespace huxerui;

struct FileOperationState {
  bool busy {false};
  std::string status {"Ready"};
  std::string detail {"Write, append, read, or delete the example file."};
};

struct FileReceptionState {
  std::vector<FileReference> references;
  std::uint64_t batch {0};
  std::string status {"No files received yet."};
  bool failed {false};
};

[[huxerui::composable]]
View DirectoryPath(std::string label, const File& directory) {
  const ThemeSpec& theme = UseTheme();
  return Column {
    Text(label, TextRole::Label).With(Foreground(theme.colors.primary)),
    SelectionArea {
      Text(directory.Path()),
    },
  }.With(Spacing(theme.spacing.extra_small));
}

[[huxerui::composable]] View FilesContent() {
  auto files = UseService<FileSystem>();
  auto picker = UseService<FilePicker>();
  auto tasks = UseTaskScope();
  const FilePickerFilter text_filter{
      .name = "Text files",
      .extensions = {"txt", "md", "json"},
      .content_types = {"text/*", "application/json"},
  };
  auto content = UseState(TextEditingValue::FromText("Hello from HuxerUI."));
  auto operation = UseState(FileOperationState{});
  auto hovering = UseState(false);
  auto reception = UseState(FileReceptionState{});
  const auto receive_files = [reception](std::vector<FileReference> selected) {
    const auto count = selected.size();
    reception = {
      std::move(selected),
      reception->batch + 1,
      "Received " + std::to_string(count) + (count == 1 ? " file." : " files.") +
          " Nothing has been read or imported.",
      false
    };
  };
  const ThemeSpec& theme = UseTheme();
  const AppDirectories& directories = files->Directories();
  File example_file(directories.data_directory, "example.txt");

  return ScrollView {
    Column {
      Text("Files and application storage", TextRole::Title),
      Text("Drop text files to retain their references, then choose which one to read into the editor."),
      Column {
        Text("Receive external files", TextRole::Title),
        Column {
          Text(hovering.Get() ? "Release to receive files" : "Drop text files here", TextRole::Title),
          Text("TXT, Markdown, JSON and other text files"),
          Text("References only. No automatic reading or import.", TextRole::Label),
        }.With(
            Frame{.min_height = 160.0F},
            Padding(theme.spacing.large),
            Spacing(theme.spacing.small),
            MainAlign(MainAxisAlignment::Center),
            CrossAlign(CrossAxisAlignment::Center),
            Background(hovering.Get() ? theme.colors.primary : theme.colors.primary_container),
            Foreground(hovering.Get() ? theme.colors.on_primary : theme.colors.on_primary_container),
            Border{theme.colors.primary, hovering.Get() ? 3.0F : 1.0F},
            CornerRadius(theme.shapes.medium),
            FileDropTarget::Accepts({
                .extensions = text_filter.extensions,
                .content_types = text_filter.content_types,
            })
        )
            .On<FileDropEvents::Entered>([hovering](const auto&, const auto&) { hovering = true; })
            .On<FileDropEvents::Exited>([hovering](const auto&, const auto&) { hovering = false; })
            .On<FileDropEvents::Dropped>([receive_files](const auto& selected, const auto&) {
              receive_files(selected);
            })
            .On<FileDropEvents::Failed>([reception](const FileError& error, const auto&) {
              reception.Update([&](FileReceptionState& value) {
                value.status = "Could not receive files: " + error.message;
                value.failed = true;
              });
            }),
        Flow {
          Button("Choose one file").With(Enabled(!operation->busy && picker->CanOpenFiles())).OnClick([=] {
            operation = {true, "Choosing a file", "Waiting for a text file selection..."};
            tasks.Launch([=]() -> Task<void> {
              std::optional<FileReference> selected = co_await picker->OpenFileAsync(text_filter);
              if (!selected) {
                operation = {false, "Selection canceled", "No external file was selected."};
                co_return;
              }
              receive_files({*selected});
              operation = {false, "File received", "Use Read into editor to open the retained reference."};
            });
          }),
          Button("Choose several files").With(Enabled(!operation->busy && picker->CanOpenFiles())).OnClick([=] {
            operation = {true, "Choosing files", "Waiting for multiple text file selections..."};
            tasks.Launch([=]() -> Task<void> {
              std::vector<FileReference> selected = co_await picker->OpenFilesAsync(text_filter);
              if (selected.empty()) {
                operation = {false, "Selection canceled", "No external files were selected."};
                co_return;
              }
              receive_files(std::move(selected));
              operation = {false, "Files received", "Use Read into editor to open a retained reference."};
            });
          }),
        }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
        Text(reception->status, TextRole::Label)
            .With(Foreground(reception->failed ? theme.colors.error : theme.colors.primary)),
        Column {
          ForEach(std::views::iota(std::size_t{0}, reception->references.size()), [=](std::size_t index) {
            const FileReference file = reception->references[index];
            const auto size = file.Size();
            return Column {
              Text(file.Name(), TextRole::Label),
              Flow {
                Text(size ? std::to_string(*size) + " bytes" : "Size unavailable")
                    .With(Foreground(theme.colors.on_surface_variant)),
                Button("Read into editor").With(Enabled(!operation->busy)).OnClick([=] {
                  operation = {true, "Reading external file", file.Name()};
                  tasks.Launch([=]() -> Task<void> {
                    FileResult<std::string> result = co_await file.ReadStringAsync();
                    if (!result.Succeeded()) {
                      operation = {false, "Read failed", result.Error().message};
                      co_return;
                    }
                    content = TextEditingValue::FromText(std::move(result).Value());
                    operation = {false, "Editor updated", "Loaded " + file.Name() + ". The source was not changed."};
                  });
                }),
              }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Center)),
            }.With(
                Padding(theme.spacing.medium),
                Spacing(theme.spacing.small),
                Background(theme.colors.surface),
                CornerRadius(theme.shapes.small),
                CrossAlign(CrossAxisAlignment::Stretch)
            ).Key(index);
          }),
        }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Stretch))
            .Key(reception->batch),
      }.With(
          Padding(theme.spacing.large),
          Spacing(theme.spacing.medium),
          Background(theme.colors.surface_container_low),
          CornerRadius(theme.shapes.medium),
          CrossAlign(CrossAxisAlignment::Stretch)
      ),
      Column {
        Text("Text editor and local file", TextRole::Title),
        SelectionArea {
          Text(example_file.Path()),
        },
        TextField(content)
            .Label("Content")
            .Placeholder("Enter UTF-8 text")
            .LineLimits(TextFieldLineLimits::MultiLine(3, 6))
            .OnChanged([content](const TextEditingValue& value) { content = value; }),
        Flow {
          Button("Write").With(Enabled(!operation->busy)).OnClick([=] {
            operation = {true, "Writing", "Replacing the file content..."};
            tasks.Launch([=, value = content->text]() -> Task<void> {
              const bool succeeded = co_await example_file.WriteStringAsync(value);
              operation = {
                false,
                succeeded ? "Write complete" : "Write failed",
                succeeded ? "The file now contains the editor text." : "The local write could not be completed."
              };
            });
          }),
          Button("Append").With(Enabled(!operation->busy)).OnClick([=] {
            operation = {true, "Appending", "Appending the editor text..."};
            tasks.Launch([=, value = content->text]() -> Task<void> {
              const bool succeeded = co_await example_file.AppendStringAsync(value);
              operation = {
                false,
                succeeded ? "Append complete" : "Append failed",
                succeeded ? "The editor text was appended without implicit separators."
                          : "The local append could not be completed."
              };
            });
          }),
          Button("Read").With(Enabled(!operation->busy)).OnClick([=] {
            operation = {true, "Reading", "Reading the complete file as UTF-8..."};
            tasks.Launch([=]() -> Task<void> {
              FileResult<std::string> result = co_await example_file.ReadStringAsync();
              if (result.Succeeded()) {
                std::string value = std::move(result).Value();
                content = TextEditingValue::FromText(value);
                operation = {
                  false,
                  "Read complete",
                  value.empty() ? "The file is empty." : "Editor content updated."
                };
              } else {
                operation = {false, "Read failed", result.Error().message};
              }
            });
          }),
          Button("Delete").With(Enabled(!operation->busy)).OnClick([=] {
            operation = {true, "Deleting", "Removing the example file..."};
            tasks.Launch([=]() -> Task<void> {
              const bool succeeded = co_await example_file.DeleteAsync();
              operation = {
                false,
                succeeded ? "Delete complete" : "Delete failed",
                succeeded ? "The file is absent." : "The local delete could not be completed."
              };
            });
          }),
          Button("Save editor text").With(Enabled(!operation->busy && picker->CanSaveFiles())).OnClick([=] {
            operation = {true, "Preparing export", "Writing the editor text to the local example file..."};
            tasks.Launch([=, value = content->text]() -> Task<void> {
              if (!co_await example_file.WriteStringAsync(value)) {
                operation = {false, "Export failed", "The local source file could not be prepared."};
                co_return;
              }
              const bool saved = co_await picker->SaveFileAsync(
                  example_file,
                  {
                      .suggested_name = "example.txt",
                      .filter = text_filter,
                  }
              );
              operation = {
                false,
                saved ? "Export complete" : "Export canceled or failed",
                saved ? "The editor text was saved to the selected location."
                      : "The save was canceled or could not be completed."
              };
            });
          }),
        }.With(
            Spacing(theme.spacing.small),
            CrossAlign(CrossAxisAlignment::Center)
        ),
        Text(operation->status, TextRole::Label).With(Foreground(theme.colors.primary)),
        Text(operation->detail),
      }.With(
          Padding(theme.spacing.large),
          Spacing(theme.spacing.medium),
          Background(theme.colors.surface_container_low),
          CornerRadius(theme.shapes.medium)
      ),
      Column {
        Text("Application directories", TextRole::Title),
        DirectoryPath("Data", directories.data_directory),
        DirectoryPath("Cache", directories.cache_directory),
        DirectoryPath("Temporary", directories.temporary_directory),
        DirectoryPath("Current working directory", files->CurrentDirectory()),
      }.With(
          Padding(theme.spacing.large),
          Spacing(theme.spacing.medium),
          Background(theme.colors.surface_container_low),
          CornerRadius(theme.shapes.medium)
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
  return MaterialTheme {FilesContent()};
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI Files",
            .initial_size = {820.0F, 720.0F},
        },
    }
};
