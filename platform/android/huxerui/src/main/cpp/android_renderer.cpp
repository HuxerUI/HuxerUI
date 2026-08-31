#include "android_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/android/jni.h>

#include "path_internal.h"
#include "resource_internal.h"
#include "shadow_internal.h"

namespace huxerui::detail {

namespace {

constexpr float kRadiansToDegrees = 57.2957795130823208768F;

jint PackColor(Color color) {
  const auto channel = [](float value) {
    return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
  };
  return static_cast<jint>(
      channel(color.alpha) << 24U | channel(color.red) << 16U | channel(color.green) << 8U | channel(color.blue)
  );
}

std::span<const std::byte> ByteSpan(std::string_view value) {
  return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

std::span<const std::byte> ByteSpan(const std::vector<jbyte>& value) {
  return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

jintArray ToIntArray(JNIEnv* environment, const std::vector<jint>& values) {
  jintArray result = environment->NewIntArray(static_cast<jsize>(values.size()));
  if (result != nullptr && !values.empty()) {
    environment->SetIntArrayRegion(result, 0, static_cast<jsize>(values.size()), values.data());
  }
  return result;
}

jfloatArray ToFloatArray(JNIEnv* environment, const std::vector<jfloat>& values) {
  jfloatArray result = environment->NewFloatArray(static_cast<jsize>(values.size()));
  if (result != nullptr && !values.empty()) {
    environment->SetFloatArrayRegion(result, 0, static_cast<jsize>(values.size()), values.data());
  }
  return result;
}

jfloatArray ToPathArray(JNIEnv* environment, const Path& path) {
  std::vector<jfloat> data;
  const std::span<const PathElement> elements = PathAccess::Elements(path);
  data.reserve(elements.size() * 7);
  for (const PathElement& element : elements) {
    data.push_back(static_cast<jfloat>(element.verb));
    const auto append = [&data](Point point) {
      data.push_back(point.x);
      data.push_back(point.y);
    };
    switch (element.verb) {
    case PathVerb::MoveTo:
    case PathVerb::LineTo:
      append(element.points[0]);
      break;
    case PathVerb::QuadraticTo:
      append(element.points[0]);
      append(element.points[1]);
      break;
    case PathVerb::CubicTo:
      append(element.points[0]);
      append(element.points[1]);
      append(element.points[2]);
      break;
    case PathVerb::Close:
      break;
    }
  }

  jfloatArray result = environment->NewFloatArray(static_cast<jsize>(data.size()));
  if (result != nullptr && !data.empty()) {
    environment->SetFloatArrayRegion(result, 0, static_cast<jsize>(data.size()), data.data());
  }
  return result;
}

} // namespace

void AndroidRenderer::Initialize(JNIEnv* environment, jclass view_class) {
  draw_rect_ = environment->GetMethodID(view_class, "drawRect", "(Landroid/graphics/Canvas;FFFFIF)V");
  draw_linear_gradient_ = environment->GetMethodID(
      view_class, "drawLinearGradient", "(Landroid/graphics/Canvas;FFFFFFFF[F[IF)V"
  );
  draw_radial_gradient_ = environment->GetMethodID(
      view_class, "drawRadialGradient", "(Landroid/graphics/Canvas;FFFFFFFF[F[IF)V"
  );
  draw_text_ =
      environment->GetMethodID(view_class, "drawText", "(Landroid/graphics/Canvas;[BFFFFFFIFI[BIIIIIII[B)V");
  draw_text_runs_ =
      environment->GetMethodID(view_class, "drawTextRuns", "(Landroid/graphics/Canvas;[B[I[F[I[F[I[B[I)V");
  draw_image_ = environment->GetMethodID(view_class, "drawImage", "(Landroid/graphics/Canvas;J[BFFFFFFFFFI)Z");
  draw_external_texture_ = environment->GetMethodID(
      view_class,
      "drawExternalTexture",
      "(Landroid/graphics/Canvas;Landroid/graphics/Bitmap;FFFFFFFFFII)V"
  );
  draw_circle_ = environment->GetMethodID(view_class, "drawCircle", "(Landroid/graphics/Canvas;FFFI)V");
  draw_line_ = environment->GetMethodID(view_class, "drawLine", "(Landroid/graphics/Canvas;FFFFIFIIF[FF)V");
  draw_arc_ = environment->GetMethodID(view_class, "drawArc", "(Landroid/graphics/Canvas;FFFFFIFIIF[FF)V");
  draw_border_ = environment->GetMethodID(view_class, "drawBorder", "(Landroid/graphics/Canvas;FFFFIFF)V");
  draw_shadow_ = environment->GetMethodID(view_class, "drawShadow", "(Landroid/graphics/Canvas;FFFFIFF)V");
  fill_path_ = environment->GetMethodID(view_class, "fillPath", "(Landroid/graphics/Canvas;[FII)V");
  fill_linear_gradient_path_ = environment->GetMethodID(
      view_class, "fillLinearGradientPath", "(Landroid/graphics/Canvas;[FFFFFFFFF[F[II)V"
  );
  fill_radial_gradient_path_ = environment->GetMethodID(
      view_class, "fillRadialGradientPath", "(Landroid/graphics/Canvas;[FFFFFFFFF[F[II)V"
  );
  stroke_path_ = environment->GetMethodID(view_class, "strokePath", "(Landroid/graphics/Canvas;[FIFIIF[FF)V");
  draw_path_shadow_ = environment->GetMethodID(view_class, "drawPathShadow", "(Landroid/graphics/Canvas;[FFFFFIFFFI)V");
  push_clip_ = environment->GetMethodID(view_class, "pushClip", "(Landroid/graphics/Canvas;FFFFF)V");
  push_path_clip_ = environment->GetMethodID(view_class, "pushPathClip", "(Landroid/graphics/Canvas;[FI)V");
  pop_clip_ = environment->GetMethodID(view_class, "popClip", "(Landroid/graphics/Canvas;)V");
  push_opacity_ = environment->GetMethodID(view_class, "pushOpacity", "(Landroid/graphics/Canvas;F)V");
  pop_opacity_ = environment->GetMethodID(view_class, "popOpacity", "(Landroid/graphics/Canvas;)V");
  push_transform_ = environment->GetMethodID(view_class, "pushTransform", "(Landroid/graphics/Canvas;FFFFFF)V");
  pop_transform_ = environment->GetMethodID(view_class, "popTransform", "(Landroid/graphics/Canvas;)V");

  if (draw_rect_ == nullptr || draw_linear_gradient_ == nullptr || draw_radial_gradient_ == nullptr ||
      draw_text_ == nullptr || draw_text_runs_ == nullptr || draw_image_ == nullptr ||
      draw_external_texture_ == nullptr || draw_circle_ == nullptr || draw_line_ == nullptr || draw_arc_ == nullptr ||
      draw_border_ == nullptr || draw_shadow_ == nullptr || fill_path_ == nullptr ||
      fill_linear_gradient_path_ == nullptr || fill_radial_gradient_path_ == nullptr || stroke_path_ == nullptr ||
      draw_path_shadow_ == nullptr || push_clip_ == nullptr || push_path_clip_ == nullptr || pop_clip_ == nullptr ||
      push_opacity_ == nullptr || pop_opacity_ == nullptr || push_transform_ == nullptr || pop_transform_ == nullptr) {
    throw std::runtime_error("HuxerUI Android renderer methods do not match the platform backend");
  }
}

void AndroidRenderer::BeginDraw() {
  if (draw_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
    draw_epoch_ = 0;
    for (CachedExternalTexture& cached : external_textures_) {
      cached.draw_epoch = std::numeric_limits<std::uint64_t>::max();
    }
  }
  ++draw_epoch_;
  std::erase_if(external_textures_, [](const CachedExternalTexture& cached) {
    const std::shared_ptr<AndroidExternalTextureState> source = cached.source.lock();
    return !source || !source->IsActive();
  });
}

struct AndroidRenderer::CommandRange {
  std::size_t first = 0;
  std::size_t end = 0;
  std::size_t cursor = 0;
};

bool AndroidRenderer::RenderSequence(
    JNIEnv* environment, jobject view, jobject canvas, const PaintSequence& sequence, CommandRange* range
) {
  for (const PaintCommand& command : sequence.Commands()) {
    if (std::holds_alternative<PlacePlatformViewCommand>(command)) {
      continue;
    }
    const bool selected = range == nullptr || (range->cursor >= range->first && range->cursor < range->end);
    if (range != nullptr) {
      ++range->cursor;
    }
    if (!selected) {
      continue;
    }
    std::visit(
        [this, environment, view, canvas](const auto& value) { RenderCommand(environment, view, canvas, value); },
        command
    );
    if (environment->ExceptionCheck()) {
      return false;
    }
  }
  return true;
}

bool AndroidRenderer::RenderSceneNode(
    JNIEnv* environment, jobject view, jobject canvas, const RenderNode& node, CommandRange* range
) {
  const float opacity = std::clamp(node.opacity, 0.0F, 1.0F);
  if (!node.visible || opacity <= 0.0F || environment->ExceptionCheck()) {
    return !environment->ExceptionCheck();
  }

  Transform2D transform = node.transform;
  transform.translate_x += node.offset.x;
  transform.translate_y += node.offset.y;
  const bool transformed = !transform.IsIdentity();
  if (transformed) {
    RenderCommand(environment, view, canvas, PushTransformCommand{transform});
    if (environment->ExceptionCheck()) {
      return false;
    }
  }

  const bool translucent = opacity < 1.0F;
  if (translucent) {
    environment->CallVoidMethod(view, push_opacity_, canvas, opacity);
    if (environment->ExceptionCheck()) {
      return false;
    }
  }

  if (!RenderSequence(environment, view, canvas, node.content, range)) {
    return false;
  }
  for (const RenderClip& clip : node.child_clips) {
    std::visit([&](const auto& command) { RenderCommand(environment, view, canvas, command); }, clip);
    if (environment->ExceptionCheck()) {
      return false;
    }
  }
  const bool children_transformed = !node.children_transform.IsIdentity();
  if (children_transformed) {
    RenderCommand(environment, view, canvas, PushTransformCommand{node.children_transform});
    if (environment->ExceptionCheck()) {
      return false;
    }
  }
  for (const RenderNode* child : node.children) {
    if (child != nullptr && !RenderSceneNode(environment, view, canvas, *child, range)) {
      return false;
    }
  }
  if (children_transformed) {
    RenderCommand(environment, view, canvas, PopTransformCommand{});
    if (environment->ExceptionCheck()) {
      return false;
    }
  }
  for (std::size_t index = 0; index < node.child_clips.size(); ++index) {
    RenderCommand(environment, view, canvas, PopClipCommand{});
    if (environment->ExceptionCheck()) {
      return false;
    }
  }
  if (!RenderSequence(environment, view, canvas, node.foreground, range)) {
    return false;
  }
  if (translucent) {
    environment->CallVoidMethod(view, pop_opacity_, canvas);
    if (environment->ExceptionCheck()) {
      return false;
    }
  }
  if (transformed) {
    RenderCommand(environment, view, canvas, PopTransformCommand{});
    if (environment->ExceptionCheck()) {
      return false;
    }
  }
  return true;
}

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawRectCommand& command) {
  environment->CallVoidMethod(
      view,
      draw_rect_,
      canvas,
      command.rect.x,
      command.rect.y,
      command.rect.width,
      command.rect.height,
      PackColor(command.color),
      command.corner_radius
  );
}

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas,
                                    const DrawLinearGradientCommand& command) {
  std::vector<jfloat> offsets;
  std::vector<jint> colors;
  offsets.reserve(command.gradient.stops.size());
  colors.reserve(command.gradient.stops.size());
  for (const GradientStop& stop : command.gradient.stops) {
    offsets.push_back(stop.offset);
    colors.push_back(PackColor(stop.color));
  }
  android::LocalRef<jfloatArray> java_offsets(environment, ToFloatArray(environment, offsets));
  android::LocalRef<jintArray> java_colors(environment, ToIntArray(environment, colors));
  if (!java_offsets || !java_colors) {
    return;
  }
  environment->CallVoidMethod(
      view,
      draw_linear_gradient_,
      canvas,
      command.rect.x,
      command.rect.y,
      command.rect.width,
      command.rect.height,
      command.gradient.start.x,
      command.gradient.start.y,
      command.gradient.end.x,
      command.gradient.end.y,
      java_offsets.Get(),
      java_colors.Get(),
      command.corner_radius
  );
}

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas,
                                    const DrawRadialGradientCommand& command) {
  std::vector<jfloat> offsets;
  std::vector<jint> colors;
  offsets.reserve(command.gradient.stops.size());
  colors.reserve(command.gradient.stops.size());
  for (const GradientStop& stop : command.gradient.stops) {
    offsets.push_back(stop.offset);
    colors.push_back(PackColor(stop.color));
  }
  android::LocalRef<jfloatArray> java_offsets(environment, ToFloatArray(environment, offsets));
  android::LocalRef<jintArray> java_colors(environment, ToIntArray(environment, colors));
  if (!java_offsets || !java_colors) {
    return;
  }
  environment->CallVoidMethod(
      view,
      draw_radial_gradient_,
      canvas,
      command.rect.x,
      command.rect.y,
      command.rect.width,
      command.rect.height,
      command.gradient.center.x,
      command.gradient.center.y,
      command.gradient.radius.width,
      command.gradient.radius.height,
      java_offsets.Get(),
      java_colors.Get(),
      command.corner_radius
  );
}

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawTextCommand& command) {
  auto bytes = android::BytesToJavaByteArray(environment, ByteSpan(command.text));
  auto family = android::BytesToJavaByteArray(environment, ByteSpan(command.style.font.FamilyName()));
  auto locale = android::BytesToJavaByteArray(environment, ByteSpan(command.options.shaping.locale));
  if (!bytes || !family || !locale) {
    return;
  }
  environment->CallVoidMethod(
      view,
      draw_text_,
      canvas,
      bytes.Get(),
      command.rect.x,
      command.rect.y,
      command.rect.width,
      command.rect.height,
      command.paragraph_offset.x,
      command.paragraph_offset.y,
      PackColor(command.style.foreground),
      command.style.font.Size(),
      static_cast<jint>(command.style.font.FamilyKind()),
      family.Get(),
      static_cast<jint>(command.style.font.Weight()),
      static_cast<jint>(command.style.font.Slant()),
      static_cast<jint>(command.style.decoration),
      static_cast<jint>(command.options.align),
      static_cast<jint>(command.options.vertical_align),
      static_cast<jint>(command.options.wrap),
      static_cast<jint>(command.options.shaping.direction),
      locale.Get()
  );
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const DrawTextRunsCommand& command
) {
  std::vector<jbyte> text_data;
  std::vector<jint> text_ranges;
  std::vector<jfloat> baselines;
  std::vector<jint> colors;
  std::vector<jfloat> font_sizes;
  std::vector<jint> styles;
  std::vector<jbyte> metadata;
  std::vector<jint> metadata_ranges;
  text_ranges.reserve(command.runs.size() * 2);
  baselines.reserve(command.runs.size() * 2);
  colors.reserve(command.runs.size());
  font_sizes.reserve(command.runs.size());
  styles.reserve(command.runs.size() * 5);
  metadata_ranges.reserve(command.runs.size() * 4);

  const auto append = [](std::vector<jbyte>& destination, std::string_view value) {
    const jint offset = static_cast<jint>(destination.size());
    destination.insert(destination.end(), value.begin(), value.end());
    return std::pair{offset, static_cast<jint>(value.size())};
  };
  for (const TextRun& run : command.runs) {
    const auto text_range = append(text_data, run.text);
    const auto family_range = append(metadata, run.style.font.FamilyName());
    const auto locale_range = append(metadata, run.shaping.locale);
    text_ranges.insert(text_ranges.end(), {text_range.first, text_range.second});
    baselines.insert(baselines.end(), {run.baseline_origin.x, run.baseline_origin.y});
    colors.push_back(PackColor(run.style.foreground));
    font_sizes.push_back(run.style.font.Size());
    styles.insert(
        styles.end(),
        {
            static_cast<jint>(run.style.font.FamilyKind()),
            static_cast<jint>(run.style.font.Weight()),
            static_cast<jint>(run.style.font.Slant()),
            static_cast<jint>(run.style.decoration),
            static_cast<jint>(run.shaping.direction),
        }
    );
    metadata_ranges.insert(
        metadata_ranges.end(),
        {family_range.first, family_range.second, locale_range.first, locale_range.second}
    );
  }

  auto text_array = android::BytesToJavaByteArray(environment, ByteSpan(text_data));
  jintArray text_range_array = ToIntArray(environment, text_ranges);
  jfloatArray baseline_array = ToFloatArray(environment, baselines);
  jintArray color_array = ToIntArray(environment, colors);
  jfloatArray font_size_array = ToFloatArray(environment, font_sizes);
  jintArray style_array = ToIntArray(environment, styles);
  auto metadata_array = android::BytesToJavaByteArray(environment, ByteSpan(metadata));
  jintArray metadata_range_array = ToIntArray(environment, metadata_ranges);
  if (text_array && text_range_array != nullptr && baseline_array != nullptr && color_array != nullptr &&
      font_size_array != nullptr && style_array != nullptr && metadata_array && metadata_range_array != nullptr) {
    environment->CallVoidMethod(
        view,
        draw_text_runs_,
        canvas,
        text_array.Get(),
        text_range_array,
        baseline_array,
        color_array,
        font_size_array,
        style_array,
        metadata_array.Get(),
        metadata_range_array
    );
  }
  const jobject references[] = {
      text_range_array,
      baseline_array,
      color_array,
      font_size_array,
      style_array,
      metadata_range_array,
  };
  for (jobject reference : references) {
    if (reference != nullptr) {
      environment->DeleteLocalRef(reference);
    }
  }
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const DrawImageCommand& command
) {
  const std::uint64_t identity = ResourceAccess::ImageIdentity(command.image);
  const float scale = command.image.Scale();
  const auto draw = [&](jbyteArray encoded) {
    return environment->CallBooleanMethod(
               view,
               draw_image_,
               canvas,
               static_cast<jlong>(identity),
               encoded,
               command.source.x * scale,
               command.source.y * scale,
               command.source.width * scale,
               command.source.height * scale,
               command.destination.x,
               command.destination.y,
               command.destination.width,
               command.destination.height,
               command.opacity,
               static_cast<jint>(command.sampling)
           ) == JNI_TRUE;
  };
  // Probe the Java Bitmap cache without allocating or copying encoded bytes; transfer the payload only on a miss.
  if (draw(nullptr) || environment->ExceptionCheck()) {
    return;
  }
  auto encoded = android::BytesToJavaByteArray(environment, command.image.EncodedBytes());
  if (encoded) {
    static_cast<void>(draw(encoded.Get()));
  }
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const DrawExternalTextureCommand& command
) {
  const AndroidExternalTextureFrame* frame = FrameFor(command.texture);
  if (frame == nullptr) {
    return;
  }
  const Size intrinsic_size = command.texture.IntrinsicSize();
  const float scale_x = static_cast<float>(frame->PixelWidth()) / intrinsic_size.width;
  const float scale_y = static_cast<float>(frame->PixelHeight()) / intrinsic_size.height;
  environment->CallVoidMethod(
      view,
      draw_external_texture_,
      canvas,
      frame->Bitmap(),
      command.source.x * scale_x,
      command.source.y * scale_y,
      command.source.width * scale_x,
      command.source.height * scale_y,
      command.destination.x,
      command.destination.y,
      command.destination.width,
      command.destination.height,
      command.opacity,
      frame->Generation(),
      static_cast<jint>(command.sampling)
  );
}

const AndroidExternalTextureFrame* AndroidRenderer::FrameFor(const ExternalTexture& texture) {
  const std::shared_ptr<AndroidExternalTextureState> source =
      std::dynamic_pointer_cast<AndroidExternalTextureState>(ExternalTextureState::From(texture));
  if (!source) {
    throw std::logic_error("HuxerUI external texture does not contain an Android Bitmap source");
  }
  auto cached = std::ranges::find_if(external_textures_, [&source](const CachedExternalTexture& entry) {
    return entry.source.lock() == source;
  });
  if (cached == external_textures_.end()) {
    external_textures_.push_back(CachedExternalTexture{source, {}, std::numeric_limits<std::uint64_t>::max()});
    cached = external_textures_.end() - 1;
  }
  if (cached->draw_epoch != draw_epoch_) {
    AndroidExternalTextureFrame frame = source->AcquireLatestFrame();
    if (frame) {
      cached->frame = std::move(frame);
    }
    cached->draw_epoch = draw_epoch_;
  }
  return cached->frame ? &cached->frame : nullptr;
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const DrawCircleCommand& command
) {
  environment->CallVoidMethod(
      view,
      draw_circle_,
      canvas,
      command.center.x,
      command.center.y,
      command.radius,
      PackColor(command.color)
  );
}

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawLineCommand& command) {
  jfloatArray dashes =
      command.style.dash_pattern.empty() ? nullptr : ToFloatArray(environment, command.style.dash_pattern);
  if (!command.style.dash_pattern.empty() && dashes == nullptr) {
    return;
  }
  environment->CallVoidMethod(
      view,
      draw_line_,
      canvas,
      command.start.x,
      command.start.y,
      command.end.x,
      command.end.y,
      PackColor(command.color),
      command.style.width,
      static_cast<jint>(command.style.cap),
      static_cast<jint>(command.style.join),
      command.style.miter_limit,
      dashes,
      command.style.dash_offset
  );
  if (dashes != nullptr) {
    environment->DeleteLocalRef(dashes);
  }
}

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const FillPathCommand& command) {
  jfloatArray path = ToPathArray(environment, command.path);
  if (path == nullptr) {
    return;
  }
  environment
      ->CallVoidMethod(view, fill_path_, canvas, path, PackColor(command.color), static_cast<jint>(command.fill_rule));
  environment->DeleteLocalRef(path);
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const FillLinearGradientPathCommand& command
) {
  android::LocalRef<jfloatArray> path(environment, ToPathArray(environment, command.path));
  std::vector<jfloat> offsets;
  std::vector<jint> colors;
  offsets.reserve(command.gradient.stops.size());
  colors.reserve(command.gradient.stops.size());
  for (const GradientStop& stop : command.gradient.stops) {
    offsets.push_back(stop.offset);
    colors.push_back(PackColor(stop.color));
  }
  android::LocalRef<jfloatArray> java_offsets(environment, ToFloatArray(environment, offsets));
  android::LocalRef<jintArray> java_colors(environment, ToIntArray(environment, colors));
  if (!path || !java_offsets || !java_colors) {
    return;
  }
  environment->CallVoidMethod(view, fill_linear_gradient_path_, canvas, path.Get(), command.gradient_rect.x,
                              command.gradient_rect.y, command.gradient_rect.width, command.gradient_rect.height,
                              command.gradient.start.x, command.gradient.start.y, command.gradient.end.x,
                              command.gradient.end.y, java_offsets.Get(), java_colors.Get(),
                              static_cast<jint>(command.fill_rule));
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const FillRadialGradientPathCommand& command
) {
  android::LocalRef<jfloatArray> path(environment, ToPathArray(environment, command.path));
  std::vector<jfloat> offsets;
  std::vector<jint> colors;
  offsets.reserve(command.gradient.stops.size());
  colors.reserve(command.gradient.stops.size());
  for (const GradientStop& stop : command.gradient.stops) {
    offsets.push_back(stop.offset);
    colors.push_back(PackColor(stop.color));
  }
  android::LocalRef<jfloatArray> java_offsets(environment, ToFloatArray(environment, offsets));
  android::LocalRef<jintArray> java_colors(environment, ToIntArray(environment, colors));
  if (!path || !java_offsets || !java_colors) {
    return;
  }
  environment->CallVoidMethod(view, fill_radial_gradient_path_, canvas, path.Get(), command.gradient_rect.x,
                              command.gradient_rect.y, command.gradient_rect.width, command.gradient_rect.height,
                              command.gradient.center.x, command.gradient.center.y, command.gradient.radius.width,
                              command.gradient.radius.height, java_offsets.Get(), java_colors.Get(),
                              static_cast<jint>(command.fill_rule));
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const StrokePathCommand& command
) {
  jfloatArray path = ToPathArray(environment, command.path);
  if (path == nullptr) {
    return;
  }
  jfloatArray dashes =
      command.style.dash_pattern.empty() ? nullptr : ToFloatArray(environment, command.style.dash_pattern);
  if (!command.style.dash_pattern.empty() && dashes == nullptr) {
    environment->DeleteLocalRef(path);
    return;
  }
  environment->CallVoidMethod(
      view,
      stroke_path_,
      canvas,
      path,
      PackColor(command.color),
      command.style.width,
      static_cast<jint>(command.style.cap),
      static_cast<jint>(command.style.join),
      command.style.miter_limit,
      dashes,
      command.style.dash_offset
  );
  environment->DeleteLocalRef(path);
  if (dashes != nullptr) {
    environment->DeleteLocalRef(dashes);
  }
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const DrawPathShadowCommand& command
) {
  jfloatArray path = ToPathArray(environment, command.path);
  if (path == nullptr) {
    return;
  }
  const Rect bounds = command.path.Bounds();
  environment->CallVoidMethod(
      view,
      draw_path_shadow_,
      canvas,
      path,
      bounds.x,
      bounds.y,
      bounds.width,
      bounds.height,
      PackColor(command.color),
      command.offset.x,
      command.offset.y,
      command.blur_radius,
      static_cast<jint>(command.fill_rule)
  );
  environment->DeleteLocalRef(path);
}

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawArcCommand& command) {
  jfloatArray dashes =
      command.style.dash_pattern.empty() ? nullptr : ToFloatArray(environment, command.style.dash_pattern);
  if (!command.style.dash_pattern.empty() && dashes == nullptr) {
    return;
  }
  environment->CallVoidMethod(
      view,
      draw_arc_,
      canvas,
      command.center.x,
      command.center.y,
      command.radius,
      command.start_angle * kRadiansToDegrees,
      command.sweep_angle * kRadiansToDegrees,
      PackColor(command.color),
      command.style.width,
      static_cast<jint>(command.style.cap),
      static_cast<jint>(command.style.join),
      command.style.miter_limit,
      dashes,
      command.style.dash_offset
  );
  if (dashes != nullptr) {
    environment->DeleteLocalRef(dashes);
  }
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const DrawBorderCommand& command
) {
  if (!command.style.dash_pattern.empty() || command.style.join != StrokeJoin::Miter ||
      command.style.miter_limit != 4.0F) {
    RenderCommand(environment, view, canvas,
                  StrokePathCommand{
                      CreateBorderStrokePath(command.rect, CornerRadii{command.corner_radius}, command.style.width),
                      command.color,
                      command.style,
                  });
    return;
  }
  environment->CallVoidMethod(
      view,
      draw_border_,
      canvas,
      command.rect.x,
      command.rect.y,
      command.rect.width,
      command.rect.height,
      PackColor(command.color),
      command.style.width,
      command.corner_radius
  );
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const DrawShadowCommand& command
) {
  const ResolvedShadow resolved = ResolveShadow(command);
  if (resolved.IsEmpty() || command.color.alpha <= 0.0F) {
    return;
  }
  environment->CallVoidMethod(
      view,
      draw_shadow_,
      canvas,
      resolved.caster.x,
      resolved.caster.y,
      resolved.caster.width,
      resolved.caster.height,
      PackColor(command.color),
      command.blur_radius,
      resolved.corner_radius
  );
}

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const PushClipCommand& command) {
  environment->CallVoidMethod(
      view,
      push_clip_,
      canvas,
      command.rect.x,
      command.rect.y,
      command.rect.width,
      command.rect.height,
      command.corner_radius
  );
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const PushPathClipCommand& command
) {
  jfloatArray path = ToPathArray(environment, command.path);
  if (path == nullptr) {
    return;
  }
  environment->CallVoidMethod(view, push_path_clip_, canvas, path, static_cast<jint>(command.fill_rule));
  environment->DeleteLocalRef(path);
}

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const PopClipCommand&) {
  environment->CallVoidMethod(view, pop_clip_, canvas);
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const PushTransformCommand& command
) {
  environment->CallVoidMethod(
      view,
      push_transform_,
      canvas,
      command.transform.m11,
      command.transform.m12,
      command.transform.m21,
      command.transform.m22,
      command.transform.translate_x,
      command.transform.translate_y
  );
}

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const PopTransformCommand&) {
  environment->CallVoidMethod(view, pop_transform_, canvas);
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const PlacePlatformViewCommand& command
) {
  static_cast<void>(environment);
  static_cast<void>(view);
  static_cast<void>(canvas);
  static_cast<void>(command);
}

void AndroidRenderer::DrawSlice(
    JNIEnv* environment,
    jobject view,
    jobject canvas,
    const RenderFrame& frame,
    std::size_t first_command,
    std::size_t command_count
) {
  if (frame.scene.root != nullptr && command_count > 0) {
    const std::size_t end = command_count > std::numeric_limits<std::size_t>::max() - first_command
                                ? std::numeric_limits<std::size_t>::max()
                                : first_command + command_count;
    CommandRange range{first_command, end, 0};
    static_cast<void>(RenderSceneNode(environment, view, canvas, *frame.scene.root, &range));
  }
}

} // namespace huxerui::detail
