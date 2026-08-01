#include <huxerui/huxerui.h>

using namespace huxerui;

View App() {
  return Column {
    Text("Canvas and Path", TextRole::Title),
    Canvas([](PaintContext& paint, Size size) {
      const float inset = 24.0F;
      Path shape;
      shape.MoveTo({inset, size.height * 0.65F})
          .CubicTo(
              {size.width * 0.20F, -8.0F},
              {size.width * 0.72F, size.height + 8.0F},
              {size.width - inset, size.height * 0.35F}
          )
          .LineTo({size.width - inset, size.height - inset})
          .LineTo({inset, size.height - inset})
          .Close();

      paint.DrawPathShadow(shape, Color::Rgb(0, 0, 0, 0.24F), {0.0F, 8.0F}, 18.0F);
      paint.FillPath(shape, Color::Rgb(103, 80, 164));
      paint.StrokePath(shape, Color::Rgb(255, 255, 255, 0.86F), 3.0F, StrokeCap::Round, StrokeJoin::Round);

      Path highlight;
      highlight.MoveTo({size.width * 0.22F, size.height * 0.52F})
          .QuadraticTo(
              {size.width * 0.50F, size.height * 0.18F},
              {size.width * 0.78F, size.height * 0.48F}
          );
      paint.PushPathClip(shape);
      paint.StrokePath(
          highlight,
          Color::Rgb(255, 255, 255, 0.60F),
          8.0F,
          StrokeCap::Round,
          StrokeJoin::Round
      );
      paint.PopClip();
    }).With(Frame{.height = 240.0F}),
  }.With(Padding(32.0F), Spacing(16.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI Canvas",
        .width = 560.0F,
        .height = 420.0F,
    }
)
