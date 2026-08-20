#include <huxerui/huxerui.h>

#include <string>
#include <utility>

using namespace huxerui;

struct FileOperationState {
  bool busy {false};
  std::string status {"Ready"};
  std::string detail {"Write, append, read, or delete the example file."};
};

View DirectoryPath(std::string label, const File& directory) {
  const ThemeSpec& theme = UseTheme();
  return Column {
    Text(std::move(label), TextRole::Label).With(Foreground(theme.colors.primary)),
    SelectionArea {
      Text(directory.Path()),
    },
  }.With(Spacing(theme.spacing.extra_small));
}

[[huxerui::scope]] View FilesContent() {
  auto files = UseService<FileSystem>();
  auto tasks = UseTaskScope();
  auto content = UseState(TextEditingValue::FromText("Hello from HuxerUI."));
  auto operation = UseState(FileOperationState{});
  const ThemeSpec& theme = UseTheme();
  const AppDirectories& directories = files->Directories();
  File example_file(directories.data_directory, "example.txt");

  return ScrollView {
    Column {
      Text("Files and application storage", TextRole::Title),
      Text(
          "FileSystem provides application-owned directories. Asynchronous File operations resume on the owning "
          "TaskScope after platform storage completes."
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
      Column {
        Text("Local file", TextRole::Title),
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
        }.With(
            Spacing(theme.spacing.small),
            CrossAlign(CrossAxisAlignment::Center)
        ),
      }.With(
          Padding(theme.spacing.large),
          Spacing(theme.spacing.medium),
          Background(theme.colors.surface_container_low),
          CornerRadius(theme.shapes.medium)
      ),
      Column {
        Text(operation->status, TextRole::Label).With(Foreground(theme.colors.primary)),
        Text(operation->detail),
      }.With(
          Padding(theme.spacing.large),
          Spacing(theme.spacing.small),
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
  return MaterialTheme(FilesContent);
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
