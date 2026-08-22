#pragma once

#include <jni.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <huxerui/render_scene.h>

#include "android_external_texture_internal.h"

namespace huxerui::detail {

class AndroidRenderer final {
public:
  void Initialize(JNIEnv* environment, jclass view_class);
  void BeginDraw();
  void DrawSlice(
      JNIEnv* environment,
      jobject view,
      jobject canvas,
      const RenderFrame& frame,
      std::size_t first_command,
      std::size_t command_count
  );

private:
  struct CommandRange;
  struct CachedExternalTexture {
    std::weak_ptr<AndroidExternalTextureState> source;
    AndroidExternalTextureFrame frame;
    std::uint64_t draw_epoch = 0;
  };

  bool
  RenderSequence(JNIEnv* environment, jobject view, jobject canvas, const PaintSequence& sequence, CommandRange* range);
  bool RenderSceneNode(JNIEnv* environment, jobject view, jobject canvas, const RenderNode& node, CommandRange* range);
  [[nodiscard]] const AndroidExternalTextureFrame* FrameFor(const ExternalTexture& texture);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawRectCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawLinearGradientCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawRadialGradientCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawTextCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawTextRunsCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawImageCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawExternalTextureCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawCircleCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawArcCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawBorderCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawShadowCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const FillPathCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const StrokePathCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const DrawPathShadowCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const PushClipCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const PushPathClipCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const PopClipCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const PushTransformCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const PopTransformCommand& command);
  void RenderCommand(JNIEnv* environment, jobject view, jobject canvas, const PlacePlatformViewCommand& command);

  jmethodID draw_rect_ = nullptr;
  jmethodID draw_linear_gradient_ = nullptr;
  jmethodID draw_radial_gradient_ = nullptr;
  jmethodID draw_text_ = nullptr;
  jmethodID draw_text_runs_ = nullptr;
  jmethodID draw_image_ = nullptr;
  jmethodID draw_external_texture_ = nullptr;
  jmethodID draw_circle_ = nullptr;
  jmethodID draw_arc_ = nullptr;
  jmethodID draw_border_ = nullptr;
  jmethodID draw_shadow_ = nullptr;
  jmethodID fill_path_ = nullptr;
  jmethodID stroke_path_ = nullptr;
  jmethodID draw_path_shadow_ = nullptr;
  jmethodID push_clip_ = nullptr;
  jmethodID push_path_clip_ = nullptr;
  jmethodID pop_clip_ = nullptr;
  jmethodID push_opacity_ = nullptr;
  jmethodID pop_opacity_ = nullptr;
  jmethodID push_transform_ = nullptr;
  jmethodID pop_transform_ = nullptr;
  std::vector<CachedExternalTexture> external_textures_;
  std::uint64_t draw_epoch_ = 0;
};

} // namespace huxerui::detail
