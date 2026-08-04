#include <huxerui/huxerui.h>

#include <image_example_resources.h>

using namespace huxerui;

View App() {
  const ImageAsset logo = UseImage(image_example_resources::images::logo);
  const VectorAsset mark = UseVectorImage(image_example_resources::images::mark);
  const RawAsset about = UseRawResource(image_example_resources::raw::about_txt);
  return Column {
    Text(image_example_resources::strings::title, TextRole::Title),
    Row {
      Image(logo).Fit(ImageFit::Contain).With(Frame{.width = 180.0F, .height = 140.0F}),
      Image(mark).Tint(Color::Rgb(132, 78, 255)).With(Frame{.width = 96.0F, .height = 96.0F}),
    }.With(Spacing(24.0F), CrossAlign(CrossAxisAlignment::Center)),
    Text::Format(
        image_example_resources::strings::selected_variant,
        logo.Scale(),
        logo.PixelWidth(),
        logo.PixelHeight()
    ),
    Text(about.AsStringView()),
  }.With(Padding(24.0F), Spacing(16.0F), CrossAlign(CrossAxisAlignment::Center));
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI Image",
        .width = 520.0F,
        .height = 440.0F,
    }
)
