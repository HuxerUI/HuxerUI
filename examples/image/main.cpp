#include <huxerui/huxerui.h>

#include <image_example_resources.h>

using namespace huxerui;

View App() {
  const ImageAsset logo = UseImage(image_example::images::logo);
  const VectorAsset mark = UseVectorImage(image_example::images::mark);
  const RawAsset about = UseRawResource(image_example::raw::about_txt);
  return Column {
    Text(image_example::strings::title, TextRole::Title),
    Row {
      Image(logo).Fit(ImageFit::Contain).With(Frame{.width = 180.0F, .height = 140.0F}),
      Image(mark).Tint(Color::Rgb(132, 78, 255)).With(Frame{.width = 96.0F, .height = 96.0F}),
    }.With(Spacing(24.0F), CrossAlign(CrossAxisAlignment::Center)),
    Text::Format(
        image_example::strings::selected_variant,
        logo.Scale(),
        logo.PixelWidth(),
        logo.PixelHeight()
    ),
    Text(about.AsStringView()),
  }.With(Padding(24.0F), Spacing(16.0F), CrossAlign(CrossAxisAlignment::Center));
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI Image",
            .initial_size = {520.0F, 440.0F},
        },
    }
};
