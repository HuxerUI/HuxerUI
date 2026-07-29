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
#include "text_input_internal.h"

namespace huxerui::detail {

namespace {

constexpr float kRadiansToDegrees = 57.2957795130823208768F;

enum class AndroidTextInputOperation : jint {
  CommitText,
  SetComposingText,
  FinishComposing,
  SetSelection,
  DeleteSurrounding,
  DeleteSurroundingCodePoints,
  SetComposingRegion,
};

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
  case AKEYCODE_A:
    return Key::A;
  case AKEYCODE_C:
    return Key::C;
  case AKEYCODE_V:
    return Key::V;
  case AKEYCODE_X:
    return Key::X;
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

class AndroidTextLayout final : public TextLayout {
public:
  AndroidTextLayout(JavaVM* virtual_machine, JNIEnv* environment, jobject layout)
      : virtual_machine_(virtual_machine), layout_(environment->NewGlobalRef(layout)) {
    if (layout_ == nullptr) {
      throw std::runtime_error("HuxerUI could not retain its Android text layout");
    }
    jclass layout_class = environment->GetObjectClass(layout);
    measure_ = environment->GetMethodID(layout_class, "measure", "()[F");
    hit_test_ = environment->GetMethodID(layout_class, "hitTest", "(FF)J");
    caret_ = environment->GetMethodID(layout_class, "caret", "(JZ)[F");
    range_ = environment->GetMethodID(layout_class, "range", "(JJ)[F");
    previous_ = environment->GetMethodID(layout_class, "previous", "(J)J");
    next_ = environment->GetMethodID(layout_class, "next", "(J)J");
    environment->DeleteLocalRef(layout_class);
    if (measure_ == nullptr || hit_test_ == nullptr || caret_ == nullptr || range_ == nullptr || previous_ == nullptr ||
        next_ == nullptr) {
      environment->DeleteGlobalRef(layout_);
      layout_ = nullptr;
      throw std::runtime_error("HuxerUI Android text layout methods do not match the native backend");
    }
  }

  ~AndroidTextLayout() override {
    if (JNIEnv* environment = Environment(); environment != nullptr && layout_ != nullptr) {
      environment->DeleteGlobalRef(layout_);
    }
  }

  Size Measure() const override {
    const std::vector<float> values = FloatArray(measure_);
    return values.size() >= 2 ? Size{values[0], values[1]} : Size{};
  }

  TextHit HitTest(Point point) const override {
    JNIEnv* environment = Environment();
    if (environment == nullptr) {
      return {};
    }
    return {
        static_cast<TextOffset>(environment->CallLongMethod(layout_, hit_test_, point.x, point.y)),
        TextAffinity::Downstream,
    };
  }

  Rect CaretRect(TextOffset offset, TextAffinity affinity) const override {
    const std::vector<float> values =
        FloatArray(caret_, static_cast<jlong>(offset), affinity == TextAffinity::Upstream ? JNI_TRUE : JNI_FALSE);
    return values.size() >= 4 ? Rect{values[0], values[1], values[2], values[3]} : Rect{};
  }

  std::vector<Rect> RangeRects(TextRange range) const override {
    const std::vector<float> values =
        FloatArray(range_, static_cast<jlong>(range.start), static_cast<jlong>(range.end));
    std::vector<Rect> rects;
    rects.reserve(values.size() / 4);
    for (std::size_t index = 0; index + 3 < values.size(); index += 4) {
      rects.push_back({
          values[index],
          values[index + 1],
          values[index + 2],
          values[index + 3],
      });
    }
    return rects;
  }

  TextOffset PreviousCaretOffset(TextOffset offset) const override {
    JNIEnv* environment = Environment();
    return environment == nullptr
               ? offset
               : static_cast<TextOffset>(environment->CallLongMethod(layout_, previous_, static_cast<jlong>(offset)));
  }

  TextOffset NextCaretOffset(TextOffset offset) const override {
    JNIEnv* environment = Environment();
    return environment == nullptr
               ? offset
               : static_cast<TextOffset>(environment->CallLongMethod(layout_, next_, static_cast<jlong>(offset)));
  }

private:
  JNIEnv* Environment() const noexcept {
    if (virtual_machine_ == nullptr) {
      return nullptr;
    }
    JNIEnv* environment = nullptr;
    return virtual_machine_->GetEnv(reinterpret_cast<void**>(&environment), JNI_VERSION_1_6) == JNI_OK ? environment
                                                                                                       : nullptr;
  }

  template <class... Arguments> std::vector<float> FloatArray(jmethodID method, Arguments... arguments) const {
    JNIEnv* environment = Environment();
    if (environment == nullptr) {
      return {};
    }
    auto* result = static_cast<jfloatArray>(environment->CallObjectMethod(layout_, method, arguments...));
    if (result == nullptr) {
      return {};
    }
    const jsize size = environment->GetArrayLength(result);
    std::vector<float> values(static_cast<std::size_t>(size));
    if (size > 0) {
      environment->GetFloatArrayRegion(result, 0, size, values.data());
    }
    environment->DeleteLocalRef(result);
    return values;
  }

  JavaVM* virtual_machine_ = nullptr;
  jobject layout_ = nullptr;
  jmethodID measure_ = nullptr;
  jmethodID hit_test_ = nullptr;
  jmethodID caret_ = nullptr;
  jmethodID range_ = nullptr;
  jmethodID previous_ = nullptr;
  jmethodID next_ = nullptr;
};

class AndroidViewPlatformHost final : public PlatformHost, public PlatformTextInput, public PlatformClipboard {
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
    create_text_layout_ = environment->GetMethodID(view_class, "createTextLayout", "([BFF)Ljava/lang/Object;");
    start_text_input_ = environment->GetMethodID(view_class, "startTextInput", "(JIIIZZZJJJIJJ)V");
    update_text_input_ = environment->GetMethodID(view_class, "updateTextInput", "(JJJJIJJIFFFF)V");
    restart_text_input_ = environment->GetMethodID(view_class, "restartTextInput", "(JIIIZZZJJJIJJ)V");
    stop_text_input_ = environment->GetMethodID(view_class, "stopTextInput", "(J)V");
    read_clipboard_text_ = environment->GetMethodID(view_class, "readClipboardText", "()[B");
    write_clipboard_text_ = environment->GetMethodID(view_class, "writeClipboardText", "([B)Z");
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

    if (schedule_frame_ == nullptr || measure_text_ == nullptr || create_text_layout_ == nullptr ||
        start_text_input_ == nullptr || update_text_input_ == nullptr || restart_text_input_ == nullptr ||
        stop_text_input_ == nullptr || read_clipboard_text_ == nullptr || write_clipboard_text_ == nullptr ||
        draw_rect_ == nullptr || draw_text_ == nullptr || draw_circle_ == nullptr || draw_arc_ == nullptr ||
        draw_border_ == nullptr || push_clip_ == nullptr || pop_clip_ == nullptr || push_transform_ == nullptr ||
        pop_transform_ == nullptr) {
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

  std::unique_ptr<TextLayout> CreateTextLayout(std::string_view text, float font_size, float max_width) override {
    JNIEnv* environment = Environment();
    if (environment == nullptr || view_ == nullptr) {
      return {};
    }
    jbyteArray bytes = ToByteArray(environment, text);
    if (bytes == nullptr) {
      return {};
    }
    jobject layout = environment->CallObjectMethod(view_, create_text_layout_, bytes, font_size, max_width);
    environment->DeleteLocalRef(bytes);
    if (layout == nullptr) {
      return {};
    }
    auto result = std::make_unique<AndroidTextLayout>(virtual_machine_, environment, layout);
    environment->DeleteLocalRef(layout);
    return result;
  }

  PlatformTextInput* TextInput() noexcept override {
    return this;
  }

  PlatformClipboard* Clipboard() noexcept override {
    return this;
  }

  std::optional<std::string> ReadText() override {
    JNIEnv* environment = Environment();
    if (environment == nullptr || view_ == nullptr) {
      return std::nullopt;
    }
    auto* bytes = static_cast<jbyteArray>(environment->CallObjectMethod(view_, read_clipboard_text_));
    if (bytes == nullptr || environment->ExceptionCheck()) {
      return std::nullopt;
    }
    std::string text = FromByteArray(environment, bytes);
    environment->DeleteLocalRef(bytes);
    return text;
  }

  bool WriteText(std::string_view text) override {
    JNIEnv* environment = Environment();
    if (environment == nullptr || view_ == nullptr) {
      return false;
    }
    jbyteArray bytes = ToByteArray(environment, text);
    if (bytes == nullptr) {
      return false;
    }
    const bool result = environment->CallBooleanMethod(view_, write_clipboard_text_, bytes) == JNI_TRUE;
    environment->DeleteLocalRef(bytes);
    return result && !environment->ExceptionCheck();
  }

  void Start(
      TextInputSessionId session_id, const TextInputConfiguration& configuration, const TextInputState& state
  ) override {
    CallTextInput(start_text_input_, session_id, configuration, state);
  }

  void Update(TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry) override {
    JNIEnv* environment = Environment();
    if (environment == nullptr || view_ == nullptr) {
      return;
    }
    const TextRange composition = state.composition.value_or(TextRange{-1, -1});
    environment->CallVoidMethod(
        view_,
        update_text_input_,
        static_cast<jlong>(session_id),
        static_cast<jlong>(state.revision),
        static_cast<jlong>(state.selection.anchor),
        static_cast<jlong>(state.selection.active),
        static_cast<jint>(state.selection.affinity),
        static_cast<jlong>(composition.start),
        static_cast<jlong>(composition.end),
        static_cast<jint>(geometry.result_code),
        geometry.caret.x,
        geometry.caret.y,
        geometry.caret.width,
        geometry.caret.height
    );
  }

  void Restart(
      TextInputSessionId session_id, const TextInputConfiguration& configuration, const TextInputState& state
  ) override {
    CallTextInput(restart_text_input_, session_id, configuration, state);
  }

  void Stop(TextInputSessionId session_id) override {
    JNIEnv* environment = Environment();
    if (environment != nullptr && view_ != nullptr) {
      environment->CallVoidMethod(view_, stop_text_input_, static_cast<jlong>(session_id));
    }
  }

  void Render(JNIEnv* environment, jobject canvas, const DisplayList& display_list) {
    for (const DisplayCommand& command : display_list.Commands()) {
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
  void CallTextInput(
      jmethodID method,
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state
  ) {
    JNIEnv* environment = Environment();
    if (environment == nullptr || view_ == nullptr) {
      return;
    }
    const TextRange composition = state.composition.value_or(TextRange{-1, -1});
    environment->CallVoidMethod(
        view_,
        method,
        static_cast<jlong>(session_id),
        static_cast<jint>(configuration.type),
        static_cast<jint>(configuration.capitalization),
        static_cast<jint>(configuration.action),
        configuration.multiline ? JNI_TRUE : JNI_FALSE,
        configuration.secure ? JNI_TRUE : JNI_FALSE,
        configuration.autocorrect ? JNI_TRUE : JNI_FALSE,
        static_cast<jlong>(state.revision),
        static_cast<jlong>(state.selection.anchor),
        static_cast<jlong>(state.selection.active),
        static_cast<jint>(state.selection.affinity),
        static_cast<jlong>(composition.start),
        static_cast<jlong>(composition.end)
    );
  }

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
  jmethodID create_text_layout_ = nullptr;
  jmethodID start_text_input_ = nullptr;
  jmethodID update_text_input_ = nullptr;
  jmethodID restart_text_input_ = nullptr;
  jmethodID stop_text_input_ = nullptr;
  jmethodID read_clipboard_text_ = nullptr;
  jmethodID write_clipboard_text_ = nullptr;
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

std::optional<TextSelection> AndroidCursorSelection(
    const TextInputContext& context, TextRange target, TextOffset inserted_length, TextOffset new_cursor_position
) {
  if (target.end > context.total_length || inserted_length < 0) {
    return std::nullopt;
  }
  const TextOffset retained_length = context.total_length - target.Length();
  if (inserted_length > std::numeric_limits<TextOffset>::max() - retained_length) {
    return std::nullopt;
  }
  const TextOffset result_length = retained_length + inserted_length;

  TextOffset cursor = target.start;
  if (new_cursor_position > 0) {
    const TextOffset insertion_end = target.start + inserted_length;
    const TextOffset delta = new_cursor_position - 1;
    cursor = delta > result_length - insertion_end ? result_length : insertion_end + delta;
  } else if (new_cursor_position < 0) {
    const TextOffset magnitude = new_cursor_position == std::numeric_limits<TextOffset>::min()
                                     ? std::numeric_limits<TextOffset>::max()
                                     : -new_cursor_position;
    cursor = magnitude > target.start ? 0 : target.start - magnitude;
  }
  return TextSelection{cursor, cursor};
}

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

  bool ApplyTextInputCommand(
      TextInputSessionId session_id,
      AndroidTextInputOperation operation,
      std::string text,
      TextOffset argument0,
      TextOffset argument1,
      TextOffset argument2
  ) {
    static_cast<void>(argument2);
    TextInputCommandBatch batch;
    batch.session_id = session_id;

    switch (operation) {
    case AndroidTextInputOperation::CommitText:
    case AndroidTextInputOperation::SetComposingText: {
      const TextInputContext context = runtime_.QueryTextInputContext(session_id, 0, 0);
      const std::optional<TextOffset> inserted_length = Utf16Length(text);
      if (context.result_code != TextInputResultCode::Ok || !inserted_length.has_value()) {
        return false;
      }
      const TextRange target = context.composition.value_or(context.selection.Range());
      const std::optional<TextSelection> selection =
          AndroidCursorSelection(context, target, *inserted_length, argument0);
      if (!selection.has_value()) {
        return false;
      }
      TextInputCommand command;
      command.kind = operation == AndroidTextInputOperation::CommitText ? TextInputCommandKind::CommitText
                                                                        : TextInputCommandKind::UpdateComposition;
      command.selection_after = selection;
      command.text = std::move(text);
      batch.commands.push_back(std::move(command));
      break;
    }
    case AndroidTextInputOperation::FinishComposing: {
      TextInputCommand command;
      command.kind = TextInputCommandKind::FinishComposition;
      batch.commands.push_back(command);
      break;
    }
    case AndroidTextInputOperation::SetSelection: {
      TextInputCommand command;
      command.kind = TextInputCommandKind::SetSelection;
      command.selection_after = TextSelection{argument0, argument1};
      batch.commands.push_back(command);
      break;
    }
    case AndroidTextInputOperation::DeleteSurrounding:
    case AndroidTextInputOperation::DeleteSurroundingCodePoints: {
      TextInputCommand command;
      command.kind = TextInputCommandKind::DeleteSurrounding;
      command.delete_before = argument0;
      command.delete_after = argument1;
      command.delete_unit = operation == AndroidTextInputOperation::DeleteSurrounding ? TextInputUnit::Utf16CodeUnit
                                                                                      : TextInputUnit::UnicodeCodePoint;
      batch.commands.push_back(command);
      break;
    }
    case AndroidTextInputOperation::SetComposingRegion: {
      const TextInputContext context = runtime_.QueryTextInputContext(session_id, 0, 0);
      if (context.result_code != TextInputResultCode::Ok) {
        return false;
      }
      const TextRange target{std::min(argument0, argument1), std::max(argument0, argument1)};
      if (context.composition == target) {
        return true;
      }
      if (context.composition.has_value()) {
        TextInputCommand finish;
        finish.kind = TextInputCommandKind::FinishComposition;
        batch.commands.push_back(finish);
      }
      TextInputCommand begin;
      begin.kind = TextInputCommandKind::BeginComposition;
      begin.target = target;
      batch.commands.push_back(begin);
      break;
    }
    }

    const TextInputApplyResult result = runtime_.HandleTextInputCommands(batch);
    return result.result_code == TextInputResultCode::Ok;
  }

  TextInputContext QueryTextInputContext(TextInputSessionId session_id, TextOffset start, TextOffset length) const {
    return runtime_.QueryTextInputContext(session_id, start, length);
  }

  TextInputGeometry QueryTextInputGeometry(TextInputSessionId session_id, TextRange range) const {
    return runtime_.QueryTextInputGeometry(session_id, range);
  }

  bool PerformTextInputAction(TextInputSessionId session_id) {
    if (runtime_.QueryTextInputContext(session_id, 0, 0).result_code != TextInputResultCode::Ok) {
      return false;
    }
    runtime_.HandleKeyEvent({
        KeyEventType::Down,
        Key::Enter,
        {},
        {},
        false,
    });
    runtime_.HandleKeyEvent({
        KeyEventType::Up,
        Key::Enter,
        {},
        {},
        false,
    });
    return true;
  }

  bool PerformTextEditingAction(TextInputSessionId session_id, TextEditingAction action) {
    return (session_id == 0 ||
            runtime_.QueryTextInputContext(session_id, 0, 0).result_code == TextInputResultCode::Ok) &&
           runtime_.PerformTextEditingAction(action);
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

extern "C" JNIEXPORT jboolean JNICALL Java_org_huxerui_HuxerUIInputConnection_nativeApplyTextInputCommand(
    JNIEnv* environment,
    jclass,
    jlong handle,
    jlong session_id,
    jint operation,
    jbyteArray text,
    jlong argument0,
    jlong argument1,
    jlong argument2
) {
  try {
    auto* session = huxerui::detail::Session(handle);
    if (session == nullptr) {
      return JNI_FALSE;
    }
    return session->ApplyTextInputCommand(
               static_cast<huxerui::TextInputSessionId>(session_id),
               static_cast<huxerui::detail::AndroidTextInputOperation>(operation),
               huxerui::detail::FromByteArray(environment, text),
               static_cast<huxerui::TextOffset>(argument0),
               static_cast<huxerui::TextOffset>(argument1),
               static_cast<huxerui::TextOffset>(argument2)
           )
               ? JNI_TRUE
               : JNI_FALSE;
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
    return JNI_FALSE;
  }
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_org_huxerui_HuxerUIInputConnection_nativeQueryTextInputContext(
    JNIEnv* environment, jclass, jlong handle, jlong session_id, jlong start, jlong length, jlongArray metadata
) {
  try {
    auto* session = huxerui::detail::Session(handle);
    if (session == nullptr || metadata == nullptr || environment->GetArrayLength(metadata) < 8) {
      return nullptr;
    }
    const huxerui::TextInputContext context = session->QueryTextInputContext(
        static_cast<huxerui::TextInputSessionId>(session_id),
        static_cast<huxerui::TextOffset>(start),
        static_cast<huxerui::TextOffset>(length)
    );
    const huxerui::TextRange composition = context.composition.value_or(huxerui::TextRange{-1, -1});
    const jlong values[] = {
        static_cast<jlong>(context.result_code),
        static_cast<jlong>(context.slice_start),
        static_cast<jlong>(context.total_length),
        static_cast<jlong>(context.selection.anchor),
        static_cast<jlong>(context.selection.active),
        static_cast<jlong>(context.selection.affinity),
        static_cast<jlong>(composition.start),
        static_cast<jlong>(composition.end),
    };
    environment->SetLongArrayRegion(metadata, 0, static_cast<jsize>(std::size(values)), values);
    return huxerui::detail::ToByteArray(environment, context.text);
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
    return nullptr;
  }
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_huxerui_HuxerUIInputConnection_nativeQueryTextInputGeometry(
    JNIEnv* environment, jclass, jlong handle, jlong session_id, jlong start, jlong end, jfloatArray geometry
) {
  try {
    auto* session = huxerui::detail::Session(handle);
    if (session == nullptr || geometry == nullptr || environment->GetArrayLength(geometry) < 4) {
      return JNI_FALSE;
    }
    const huxerui::TextInputGeometry result = session->QueryTextInputGeometry(
        static_cast<huxerui::TextInputSessionId>(session_id),
        {
            static_cast<huxerui::TextOffset>(start),
            static_cast<huxerui::TextOffset>(end),
        }
    );
    if (result.result_code != huxerui::TextInputResultCode::Ok) {
      return JNI_FALSE;
    }
    const jfloat values[] = {
        result.caret.x,
        result.caret.y,
        result.caret.width,
        result.caret.height,
    };
    environment->SetFloatArrayRegion(geometry, 0, static_cast<jsize>(std::size(values)), values);
    return JNI_TRUE;
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
    return JNI_FALSE;
  }
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_huxerui_HuxerUIInputConnection_nativePerformTextInputAction(
    JNIEnv* environment, jclass, jlong handle, jlong session_id, jint editor_action
) {
  try {
    static_cast<void>(editor_action);
    auto* session = huxerui::detail::Session(handle);
    return session != nullptr && session->PerformTextInputAction(static_cast<huxerui::TextInputSessionId>(session_id))
               ? JNI_TRUE
               : JNI_FALSE;
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
    return JNI_FALSE;
  }
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_huxerui_HuxerUIInputConnection_nativePerformTextEditingAction(
    JNIEnv* environment, jclass, jlong handle, jlong session_id, jint action
) {
  try {
    auto* session = huxerui::detail::Session(handle);
    return session != nullptr && session->PerformTextEditingAction(
                                     static_cast<huxerui::TextInputSessionId>(session_id),
                                     static_cast<huxerui::TextEditingAction>(action)
                                 )
               ? JNI_TRUE
               : JNI_FALSE;
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
    return JNI_FALSE;
  }
}
