#include <huxerui/huxerui.h>

#include <algorithm>
#include <exception>
#include <string>
#include <utility>

using namespace huxerui;

[[huxerui::composable]] View BitmapEditor() {
  const auto picker = UseService<FilePicker>();
  const auto tasks = UseTaskScope();
  const auto destination = UseApplication().Directories().temporary_directory.Child("bitmap-crop.png");
  auto bitmap = UseState(Bitmap{});
  auto busy = UseState(false);
  auto status = UseState(std::string{"Open a PNG or JPEG."});
  const auto& theme = UseTheme();

  return ScrollView {
    Column {
      Text("Bitmap editor", TextRole::Title),
      Flow {
        Button("Open image").With(Enabled(!busy && picker->CanOpenFiles())).OnClick([=] {
          busy = true;
          tasks.Launch([=]() -> Task<void> {
            try {
              FilePickerFilter filter{
                  .name = "Images",
                  .extensions = {"png", "jpg", "jpeg"},
                  .content_types = {"image/png", "image/jpeg"},
              };
              if (auto picked = co_await picker->OpenFileAsync(std::move(filter))) {
                auto result = co_await picked->ReadBytesAsync();
                if (result.Succeeded()) {
                  bitmap = co_await ImageAsset::FromEncoded(std::move(result).Value()).DecodeAsync();
                  status = picked->Name();
                } else {
                  status = result.Error().message;
                }
              }
            } catch (const std::exception& error) {
              status = std::string(error.what());
            }
            busy = false;
          });
        }),
        Button("Center crop").With(Enabled(!busy && bitmap->HasValue())).OnClick([=] {
          const auto width = bitmap->PixelWidth();
          const auto height = bitmap->PixelHeight();
          const auto side = std::min(width, height);
          bitmap = bitmap->Crop((width - side) / 2, (height - side) / 2, side, side);
          status = "Center crop";
        }),
        Button("Save PNG").With(Enabled(!busy && bitmap->HasValue())).OnClick([=] {
          busy = true;
          tasks.Launch([=]() -> Task<void> {
            try {
              const bool saved = co_await bitmap->SaveAsync(destination, {.format = ImageFormat::Png});
              status = saved ? "Saved to " + destination.Path() : std::string{"Could not save the image."};
            } catch (const std::exception& error) {
              status = std::string(error.what());
            }
            busy = false;
          });
        }),
      }.With(Spacing(theme.spacing.small)),
      bitmap->HasValue() ? View(Column {
        Image(bitmap.Get()).Fit(ImageFit::Contain).With(Frame{.height = 300.0F}),
        Text(std::to_string(bitmap->PixelWidth()) + " × " + std::to_string(bitmap->PixelHeight()) + " px"),
      }.With(
          Padding(theme.spacing.medium),
          Spacing(theme.spacing.small),
          CrossAlign(CrossAxisAlignment::Stretch),
          Background(theme.colors.surface_container_low),
          CornerRadius(theme.shapes.medium)
      )) : View{},
      Text(status.Get()),
    }.With(
        Padding(theme.spacing.large),
        Spacing(theme.spacing.medium),
        CrossAlign(CrossAxisAlignment::Stretch)
    ),
  };
}

View App() {
  return MaterialTheme {BitmapEditor()};
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI Bitmap",
            .initial_size = {680.0F, 520.0F},
        },
    }
};
