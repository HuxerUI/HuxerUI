#include "android_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <variant>

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

jbyteArray ToByteArray(JNIEnv* environment, std::string_view text) {
  auto* bytes = environment->NewByteArray(static_cast<jsize>(text.size()));
  if (bytes == nullptr || text.empty()) {
    return bytes;
  }
  environment
      ->SetByteArrayRegion(bytes, 0, static_cast<jsize>(text.size()), reinterpret_cast<const jbyte*>(text.data()));
  return bytes;
}

} // namespace

void AndroidRenderer::Initialize(JNIEnv* environment, jclass view_class) {
  draw_rect_ = environment->GetMethodID(view_class, "drawRect", "(Landroid/graphics/Canvas;FFFFIF)V");
  draw_text_ = environment->GetMethodID(view_class, "drawText", "(Landroid/graphics/Canvas;[BFFFFIFI)V");
  draw_circle_ = environment->GetMethodID(view_class, "drawCircle", "(Landroid/graphics/Canvas;FFFI)V");
  draw_arc_ = environment->GetMethodID(view_class, "drawArc", "(Landroid/graphics/Canvas;FFFFFIFI)V");
  draw_border_ = environment->GetMethodID(view_class, "drawBorder", "(Landroid/graphics/Canvas;FFFFIFF)V");
  draw_shadow_ = environment->GetMethodID(view_class, "drawShadow", "(Landroid/graphics/Canvas;FFFFIFF)V");
  push_clip_ = environment->GetMethodID(view_class, "pushClip", "(Landroid/graphics/Canvas;FFFFF)V");
  pop_clip_ = environment->GetMethodID(view_class, "popClip", "(Landroid/graphics/Canvas;)V");
  push_opacity_ = environment->GetMethodID(view_class, "pushOpacity", "(Landroid/graphics/Canvas;F)V");
  pop_opacity_ = environment->GetMethodID(view_class, "popOpacity", "(Landroid/graphics/Canvas;)V");
  push_transform_ = environment->GetMethodID(view_class, "pushTransform", "(Landroid/graphics/Canvas;FFFFFF)V");
  pop_transform_ = environment->GetMethodID(view_class, "popTransform", "(Landroid/graphics/Canvas;)V");

  if (draw_rect_ == nullptr || draw_text_ == nullptr || draw_circle_ == nullptr || draw_arc_ == nullptr ||
      draw_border_ == nullptr || draw_shadow_ == nullptr || push_clip_ == nullptr || pop_clip_ == nullptr ||
      push_opacity_ == nullptr || pop_opacity_ == nullptr || push_transform_ == nullptr || pop_transform_ == nullptr) {
    throw std::runtime_error("HuxerUI Android renderer methods do not match the native backend");
  }
}

void AndroidRenderer::Render(JNIEnv* environment, jobject view, jobject canvas, const RenderFrame& frame) {
  if (frame.scene.root != nullptr) {
    RenderSceneNode(environment, view, canvas, *frame.scene.root);
  }
}

bool AndroidRenderer::RenderSequence(JNIEnv* environment, jobject view, jobject canvas, const PaintSequence& sequence) {
  for (const PaintCommand& command : sequence.Commands()) {
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

bool AndroidRenderer::RenderSceneNode(JNIEnv* environment, jobject view, jobject canvas, const RenderNode& node) {
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

  if (!RenderSequence(environment, view, canvas, node.content)) {
    return false;
  }
  if (node.child_clip.has_value()) {
    RenderCommand(
        environment,
        view,
        canvas,
        PushClipCommand{
            node.child_clip->rect,
            node.child_clip->corner_radius,
        }
    );
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
    if (child != nullptr && !RenderSceneNode(environment, view, canvas, *child)) {
      return false;
    }
  }
  if (children_transformed) {
    RenderCommand(environment, view, canvas, PopTransformCommand{});
    if (environment->ExceptionCheck()) {
      return false;
    }
  }
  if (node.child_clip.has_value()) {
    RenderCommand(environment, view, canvas, PopClipCommand{});
    if (environment->ExceptionCheck()) {
      return false;
    }
  }
  if (!RenderSequence(environment, view, canvas, node.foreground)) {
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

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawTextCommand& command) {
  jbyteArray bytes = ToByteArray(environment, command.text);
  if (bytes == nullptr) {
    return;
  }
  environment->CallVoidMethod(
      view,
      draw_text_,
      canvas,
      bytes,
      command.rect.x,
      command.rect.y,
      command.rect.width,
      command.rect.height,
      PackColor(command.color),
      command.font_size,
      static_cast<jint>(command.align)
  );
  environment->DeleteLocalRef(bytes);
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

void AndroidRenderer::RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawArcCommand& command) {
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
      command.width,
      static_cast<jint>(command.cap)
  );
}

void AndroidRenderer::RenderCommand(
    JNIEnv* environment, jobject view, jobject canvas, const DrawBorderCommand& command
) {
  environment->CallVoidMethod(
      view,
      draw_border_,
      canvas,
      command.rect.x,
      command.rect.y,
      command.rect.width,
      command.rect.height,
      PackColor(command.color),
      command.width,
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

} // namespace huxerui::detail
