#include <huxerui/app.h>

#include <android/input.h>
#include <android/keycodes.h>
#include <jni.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "internal.h"

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

std::string FromByteArray(JNIEnv* environment, jbyteArray bytes) {
  if (bytes == nullptr) {
    return {};
  }
  const jsize size = environment->GetArrayLength(bytes);
  std::string text(static_cast<std::size_t>(size), '\0');
  if (size > 0) {
    environment->GetByteArrayRegion(bytes, 0, size, reinterpret_cast<jbyte*>(text.data()));
  }
  return text;
}

Key TranslateKey(jint key_code) {
  switch (key_code) {
  case AKEYCODE_TAB:
    return Key::Tab;
  case AKEYCODE_ENTER:
  case AKEYCODE_NUMPAD_ENTER:
    return Key::Enter;
  case AKEYCODE_SPACE:
    return Key::Space;
  case AKEYCODE_ESCAPE:
    return Key::Escape;
  case AKEYCODE_DEL:
    return Key::Backspace;
  case AKEYCODE_FORWARD_DEL:
    return Key::Delete;
  case AKEYCODE_DPAD_LEFT:
    return Key::ArrowLeft;
  case AKEYCODE_DPAD_RIGHT:
    return Key::ArrowRight;
  case AKEYCODE_DPAD_UP:
    return Key::ArrowUp;
  case AKEYCODE_DPAD_DOWN:
    return Key::ArrowDown;
  case AKEYCODE_MOVE_HOME:
    return Key::Home;
  case AKEYCODE_MOVE_END:
    return Key::End;
  case AKEYCODE_PAGE_UP:
    return Key::PageUp;
  case AKEYCODE_PAGE_DOWN:
    return Key::PageDown;
  default:
    return Key::Unknown;
  }
}

void ThrowJavaException(JNIEnv* environment, const char* message) noexcept {
  if (environment->ExceptionCheck()) {
    return;
  }
  jclass exception_class = environment->FindClass("java/lang/RuntimeException");
  if (exception_class != nullptr) {
    environment->ThrowNew(exception_class, message);
    environment->DeleteLocalRef(exception_class);
  }
}

class AndroidViewPlatformHost final : public PlatformHost {
public:
  AndroidViewPlatformHost(JNIEnv* environment, jobject view) {
    if (environment->GetJavaVM(&virtual_machine_) != JNI_OK) {
      throw std::runtime_error("HuxerUI could not access the Android Java VM");
    }
    view_ = environment->NewGlobalRef(view);
    if (view_ == nullptr) {
      throw std::runtime_error("HuxerUI could not retain its Android view");
    }

    jclass view_class = environment->GetObjectClass(view);
    if (view_class == nullptr) {
      environment->DeleteGlobalRef(view_);
      view_ = nullptr;
      throw std::runtime_error("HuxerUI could not inspect its Android view");
    }
    schedule_frame_ = environment->GetMethodID(view_class, "scheduleFrame", "(J)V");
    measure_text_ = environment->GetMethodID(view_class, "measureText", "([BFF)[F");
    draw_rect_ = environment->GetMethodID(view_class, "drawRect", "(Landroid/graphics/Canvas;FFFFIF)V");
    draw_text_ = environment->GetMethodID(view_class, "drawText", "(Landroid/graphics/Canvas;[BFFFFIFI)V");
    draw_circle_ = environment->GetMethodID(view_class, "drawCircle", "(Landroid/graphics/Canvas;FFFI)V");
    draw_arc_ = environment->GetMethodID(view_class, "drawArc", "(Landroid/graphics/Canvas;FFFFFIFI)V");
    draw_border_ = environment->GetMethodID(view_class, "drawBorder", "(Landroid/graphics/Canvas;FFFFIFF)V");
    push_clip_ = environment->GetMethodID(view_class, "pushClip", "(Landroid/graphics/Canvas;FFFFF)V");
    pop_clip_ = environment->GetMethodID(view_class, "popClip", "(Landroid/graphics/Canvas;)V");
    push_transform_ = environment->GetMethodID(view_class, "pushTransform", "(Landroid/graphics/Canvas;FFFFFF)V");
    pop_transform_ = environment->GetMethodID(view_class, "popTransform", "(Landroid/graphics/Canvas;)V");
    environment->DeleteLocalRef(view_class);

    if (schedule_frame_ == nullptr || measure_text_ == nullptr || draw_rect_ == nullptr || draw_text_ == nullptr ||
        draw_circle_ == nullptr || draw_arc_ == nullptr || draw_border_ == nullptr || push_clip_ == nullptr ||
        pop_clip_ == nullptr || push_transform_ == nullptr || pop_transform_ == nullptr) {
      environment->DeleteGlobalRef(view_);
      view_ = nullptr;
      throw std::runtime_error("HuxerUI Android view methods do not match the native backend");
    }
  }

  ~AndroidViewPlatformHost() override {
    JNIEnv* environment = Environment();
    if (environment != nullptr && view_ != nullptr) {
      environment->DeleteGlobalRef(view_);
    }
  }

  void RequestFrame(double delay_seconds) override {
    JNIEnv* environment = Environment();
    if (environment == nullptr || view_ == nullptr) {
      return;
    }
    double delay_milliseconds = std::ceil(delay_seconds * 1000.0);
    if (!std::isfinite(delay_milliseconds) || delay_milliseconds <= 0.0) {
      delay_milliseconds = 0.0;
    }
    const double bounded = std::min(delay_milliseconds, static_cast<double>(std::numeric_limits<jlong>::max()));
    environment->CallVoidMethod(view_, schedule_frame_, static_cast<jlong>(bounded));
  }

  double Now() const noexcept override {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
  }

  Size MeasureText(std::string_view text, float font_size, float max_width) override {
    JNIEnv* environment = Environment();
    if (environment == nullptr || view_ == nullptr) {
      return {};
    }
    jbyteArray bytes = ToByteArray(environment, text);
    if (bytes == nullptr) {
      return {};
    }
    auto* result =
        static_cast<jfloatArray>(environment->CallObjectMethod(view_, measure_text_, bytes, font_size, max_width));
    environment->DeleteLocalRef(bytes);
    if (result == nullptr || environment->GetArrayLength(result) < 2) {
      if (result != nullptr) {
        environment->DeleteLocalRef(result);
      }
      return {};
    }
    jfloat values[2]{};
    environment->GetFloatArrayRegion(result, 0, 2, values);
    environment->DeleteLocalRef(result);
    return {values[0], values[1]};
  }

  void Render(JNIEnv* environment, jobject canvas, const DisplayList& display_list) {
    for (const DrawCommand& command : display_list.Commands()) {
      std::visit(
          [this, environment, canvas](const auto& value) { RenderCommand(environment, canvas, value); },
          command
      );
      if (environment->ExceptionCheck()) {
        return;
      }
    }
  }

private:
  JNIEnv* Environment() const noexcept {
    if (virtual_machine_ == nullptr) {
      return nullptr;
    }
    JNIEnv* environment = nullptr;
    if (virtual_machine_->GetEnv(reinterpret_cast<void**>(&environment), JNI_VERSION_1_6) != JNI_OK) {
      return nullptr;
    }
    return environment;
  }

  void RenderCommand(JNIEnv* environment, jobject canvas, const DrawRectCommand& command) {
    environment->CallVoidMethod(
        view_,
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

  void RenderCommand(JNIEnv* environment, jobject canvas, const DrawTextCommand& command) {
    jbyteArray bytes = ToByteArray(environment, command.text);
    if (bytes == nullptr) {
      return;
    }
    environment->CallVoidMethod(
        view_,
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

  void RenderCommand(JNIEnv* environment, jobject canvas, const DrawCircleCommand& command) {
    environment->CallVoidMethod(
        view_,
        draw_circle_,
        canvas,
        command.center.x,
        command.center.y,
        command.radius,
        PackColor(command.color)
    );
  }

  void RenderCommand(JNIEnv* environment, jobject canvas, const DrawArcCommand& command) {
    environment->CallVoidMethod(
        view_,
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

  void RenderCommand(JNIEnv* environment, jobject canvas, const DrawBorderCommand& command) {
    environment->CallVoidMethod(
        view_,
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

  void RenderCommand(JNIEnv* environment, jobject canvas, const PushClipCommand& command) {
    environment->CallVoidMethod(
        view_,
        push_clip_,
        canvas,
        command.rect.x,
        command.rect.y,
        command.rect.width,
        command.rect.height,
        command.corner_radius
    );
  }

  void RenderCommand(JNIEnv* environment, jobject canvas, const PopClipCommand&) {
    environment->CallVoidMethod(view_, pop_clip_, canvas);
  }

  void RenderCommand(JNIEnv* environment, jobject canvas, const PushTransformCommand& command) {
    environment->CallVoidMethod(
        view_,
        push_transform_,
        canvas,
        command.m11,
        command.m12,
        command.m21,
        command.m22,
        command.translate_x,
        command.translate_y
    );
  }

  void RenderCommand(JNIEnv* environment, jobject canvas, const PopTransformCommand&) {
    environment->CallVoidMethod(view_, pop_transform_, canvas);
  }

  JavaVM* virtual_machine_ = nullptr;
  jobject view_ = nullptr;
  jmethodID schedule_frame_ = nullptr;
  jmethodID measure_text_ = nullptr;
  jmethodID draw_rect_ = nullptr;
  jmethodID draw_text_ = nullptr;
  jmethodID draw_circle_ = nullptr;
  jmethodID draw_arc_ = nullptr;
  jmethodID draw_border_ = nullptr;
  jmethodID push_clip_ = nullptr;
  jmethodID pop_clip_ = nullptr;
  jmethodID push_transform_ = nullptr;
  jmethodID pop_transform_ = nullptr;
};

class AndroidSession final {
public:
  AndroidSession(JNIEnv* environment, jobject view, AppDefinition definition)
      : platform_(environment, view), runtime_(std::move(definition), platform_) {}

  void Resize(float width, float height) {
    runtime_.SetViewport({
        std::max(0.0F, width),
        std::max(0.0F, height),
    });
  }

  void Draw(JNIEnv* environment, jobject canvas) {
    platform_.Render(environment, canvas, runtime_.BuildFrame());
  }

  void Pointer(PointerEventType type, PointerDeviceKind device_kind, std::int64_t pointer_id, float x, float y) {
    runtime_.HandlePointerEvent({
        type,
        pointer_id,
        {x, y},
        device_kind,
    });
  }

  void Scroll(float x, float y, float delta_x, float delta_y) {
    runtime_.HandleScrollEvent({
        {x, y},
        delta_x,
        delta_y,
    });
  }

  void KeyEvent(KeyEventType type, jint key_code, std::string text, KeyModifiers modifiers, bool repeat) {
    runtime_.HandleKeyEvent({
        type,
        TranslateKey(key_code),
        std::move(text),
        modifiers,
        repeat,
    });
  }

private:
  AndroidViewPlatformHost platform_;
  Runtime runtime_;
};

AndroidSession* Session(jlong handle) {
  return reinterpret_cast<AndroidSession*>(static_cast<std::uintptr_t>(handle));
}

} // namespace

} // namespace huxerui::detail

extern "C" JNIEXPORT jlong JNICALL
Java_org_huxerui_HuxerUIView_nativeCreate(JNIEnv* environment, jclass, jobject view) {
  try {
    auto session = std::make_unique<huxerui::detail::AndroidSession>(
        environment,
        view,
        huxerui::detail::RegisteredAppDefinition()
    );
    return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(session.release()));
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
    return 0;
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIView_nativeDestroy(JNIEnv*, jclass, jlong handle) {
  delete huxerui::detail::Session(handle);
}

extern "C" JNIEXPORT void JNICALL
Java_org_huxerui_HuxerUIView_nativeResize(JNIEnv* environment, jclass, jlong handle, jfloat width, jfloat height) {
  try {
    if (auto* session = huxerui::detail::Session(handle)) {
      session->Resize(width, height);
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}

extern "C" JNIEXPORT void JNICALL
Java_org_huxerui_HuxerUIView_nativeDraw(JNIEnv* environment, jclass, jlong handle, jobject canvas) {
  try {
    if (auto* session = huxerui::detail::Session(handle)) {
      session->Draw(environment, canvas);
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIView_nativePointer(
    JNIEnv* environment, jclass, jlong handle, jint type, jint device_kind, jlong pointer_id, jfloat x, jfloat y
) {
  try {
    if (auto* session = huxerui::detail::Session(handle)) {
      session->Pointer(
          static_cast<huxerui::PointerEventType>(type),
          static_cast<huxerui::PointerDeviceKind>(device_kind),
          pointer_id,
          x,
          y
      );
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIView_nativeScroll(
    JNIEnv* environment, jclass, jlong handle, jfloat x, jfloat y, jfloat delta_x, jfloat delta_y
) {
  try {
    if (auto* session = huxerui::detail::Session(handle)) {
      session->Scroll(x, y, delta_x, delta_y);
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIView_nativeKey(
    JNIEnv* environment,
    jclass,
    jlong handle,
    jboolean down,
    jint key_code,
    jbyteArray text,
    jboolean shift,
    jboolean control,
    jboolean alt,
    jboolean meta,
    jboolean repeat
) {
  try {
    if (auto* session = huxerui::detail::Session(handle)) {
      session->KeyEvent(
          down ? huxerui::KeyEventType::Down : huxerui::KeyEventType::Up,
          key_code,
          huxerui::detail::FromByteArray(environment, text),
          {
              static_cast<bool>(shift),
              static_cast<bool>(control),
              static_cast<bool>(alt),
              static_cast<bool>(meta),
          },
          static_cast<bool>(repeat)
      );
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}
