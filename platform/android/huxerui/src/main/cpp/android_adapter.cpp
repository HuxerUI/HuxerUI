#include <huxerui/app.h>

#include <android/input.h>
#include <android/keycodes.h>
#include <jni.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <huxerui/android/jni.h>
#include <huxerui/android/platform_registry.h>

#include "android_accessibility.h"
#include "android_application_internal.h"
#include "android_file_internal.h"
#include "android_http_internal.h"
#include "android_platform_view.h"
#include "android_renderer.h"
#include "android_text_layout.h"
#include "android_text_input_internal.h"
#include "platform_frame_internal.h"
#include "resource_internal.h"
#include "text_input_internal.h"
#include "text_layout_internal.h"

namespace huxerui::detail {

namespace {

double TimevalSeconds(const timeval& value) noexcept {
  return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1'000'000.0;
}

enum class AndroidEditorAction : jint {
  Unspecified,
  None,
  Go,
  Search,
  Send,
  Next,
  Done,
  Previous,
};

std::optional<TextInputAction> ToTextInputAction(jint action) {
  switch (static_cast<AndroidEditorAction>(action)) {
  case AndroidEditorAction::Unspecified:
    return TextInputAction::Default;
  case AndroidEditorAction::None:
    return TextInputAction::Newline;
  case AndroidEditorAction::Go:
    return TextInputAction::Go;
  case AndroidEditorAction::Search:
    return TextInputAction::Search;
  case AndroidEditorAction::Send:
    return TextInputAction::Send;
  case AndroidEditorAction::Next:
    return TextInputAction::Next;
  case AndroidEditorAction::Done:
    return TextInputAction::Done;
  case AndroidEditorAction::Previous:
  default:
    return std::nullopt;
  }
}

enum class AndroidTextInputOperation : jint {
  CommitText,
  SetComposingText,
  FinishComposing,
  SetSelection,
  DeleteSurrounding,
  DeleteSurroundingCodePoints,
  SetComposingRegion,
};

std::optional<SemanticActionKind> ToSemanticAction(jint action) {
  switch (static_cast<AndroidSemanticAction>(action)) {
  case AndroidSemanticAction::Activate:
    return SemanticActionKind::Activate;
  case AndroidSemanticAction::Focus:
    return SemanticActionKind::Focus;
  case AndroidSemanticAction::SetText:
    return SemanticActionKind::SetText;
  case AndroidSemanticAction::SetSelection:
    return SemanticActionKind::SetSelection;
  case AndroidSemanticAction::SetValue:
    return SemanticActionKind::SetValue;
  case AndroidSemanticAction::Increment:
    return SemanticActionKind::Increment;
  case AndroidSemanticAction::Decrement:
    return SemanticActionKind::Decrement;
  case AndroidSemanticAction::Scroll:
    return SemanticActionKind::Scroll;
  case AndroidSemanticAction::ShowOnScreen:
    return SemanticActionKind::ShowOnScreen;
  case AndroidSemanticAction::Expand:
    return SemanticActionKind::Expand;
  case AndroidSemanticAction::Collapse:
    return SemanticActionKind::Collapse;
  case AndroidSemanticAction::Dismiss:
    return SemanticActionKind::Dismiss;
  case AndroidSemanticAction::Custom:
    return SemanticActionKind::Custom;
  }
  return std::nullopt;
}

jbyteArray ToByteArray(JNIEnv* environment, std::string_view text) {
  const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(text.data()), text.size());
  return android::BytesToJavaByteArray(environment, bytes).Release();
}

jbyteArray ToByteArray(JNIEnv* environment, const std::vector<std::uint8_t>& bytes) {
  const std::span<const std::byte> values(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
  return android::BytesToJavaByteArray(environment, values).Release();
}

std::string FromByteArray(JNIEnv* environment, jbyteArray bytes) {
  if (bytes == nullptr) {
    return {};
  }
  const std::vector<std::byte> values = android::JavaByteArrayToBytes(environment, bytes);
  if (values.empty()) {
    return {};
  }
  return {reinterpret_cast<const char*>(values.data()), values.size()};
}

Key TranslateKey(jint key_code) {
  switch (key_code) {
  case AKEYCODE_SHIFT_LEFT:
  case AKEYCODE_SHIFT_RIGHT:
    return Key::Shift;
  case AKEYCODE_CTRL_LEFT:
  case AKEYCODE_CTRL_RIGHT:
    return Key::Control;
  case AKEYCODE_ALT_LEFT:
  case AKEYCODE_ALT_RIGHT:
    return Key::Alt;
  case AKEYCODE_META_LEFT:
  case AKEYCODE_META_RIGHT:
    return Key::Meta;
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
  case AKEYCODE_Y:
    return Key::Y;
  case AKEYCODE_Z:
    return Key::Z;
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

class AndroidUIThreadDispatcherState final {
public:
  ~AndroidUIThreadDispatcherState() {
    bool attached = false;
    JNIEnv* environment = Environment(attached);
    Shutdown(environment);
    if (attached) {
      virtual_machine_->DetachCurrentThread();
    }
  }

  void Initialize(JNIEnv* environment, jobject view) {
    if (environment->GetJavaVM(&virtual_machine_) != JNI_OK) {
      throw std::runtime_error("HuxerUI could not access the Android Java VM for UI dispatch");
    }
    view_ = environment->NewGlobalRef(view);
    jclass view_class = environment->GetObjectClass(view);
    if (view_ == nullptr || view_class == nullptr) {
      if (view_ != nullptr) {
        environment->DeleteGlobalRef(view_);
        view_ = nullptr;
      }
      throw std::runtime_error("HuxerUI could not initialize Android UI dispatch");
    }
    schedule_tasks_ = environment->GetMethodID(view_class, "schedulePlatformTasks", "()V");
    environment->DeleteLocalRef(view_class);
    if (schedule_tasks_ == nullptr) {
      environment->ExceptionClear();
      environment->DeleteGlobalRef(view_);
      view_ = nullptr;
      throw std::runtime_error("HuxerUI Android UI dispatcher method does not match the platform backend");
    }
  }

  void Dispatch(std::function<void()> task) {
    bool schedule = false;
    {
      std::lock_guard lock(mutex_);
      if (closed_) {
        return;
      }
      tasks_.push_back(std::move(task));
      if (!scheduled_) {
        scheduled_ = true;
        schedule = true;
      }
    }
    if (!schedule) {
      return;
    }
    bool attached = false;
    JNIEnv* environment = Environment(attached);
    if (environment == nullptr) {
      std::lock_guard lock(mutex_);
      scheduled_ = false;
      return;
    }
    {
      std::lock_guard lock(mutex_);
      if (!closed_ && view_ != nullptr) {
        environment->CallVoidMethod(view_, schedule_tasks_);
        if (environment->ExceptionCheck()) {
          environment->ExceptionClear();
          scheduled_ = false;
        }
      } else {
        scheduled_ = false;
      }
    }
    if (attached) {
      virtual_machine_->DetachCurrentThread();
    }
  }

  void Drain() {
    std::vector<std::function<void()>> tasks;
    {
      std::lock_guard lock(mutex_);
      if (closed_) {
        return;
      }
      scheduled_ = false;
      tasks.swap(tasks_);
    }
    for (auto& task : tasks) {
      try {
        task();
      } catch (...) {
      }
    }
  }

  void Shutdown(JNIEnv* environment) {
    std::lock_guard lock(mutex_);
    closed_ = true;
    scheduled_ = false;
    tasks_.clear();
    if (environment != nullptr && view_ != nullptr) {
      environment->DeleteGlobalRef(view_);
      view_ = nullptr;
    }
  }

private:
  JNIEnv* Environment(bool& attached) const {
    attached = false;
    if (virtual_machine_ == nullptr) {
      return nullptr;
    }
    JNIEnv* environment = nullptr;
    const jint result = virtual_machine_->GetEnv(reinterpret_cast<void**>(&environment), JNI_VERSION_1_6);
    if (result == JNI_OK) {
      return environment;
    }
    if (result == JNI_EDETACHED && virtual_machine_->AttachCurrentThread(&environment, nullptr) == JNI_OK) {
      attached = true;
      return environment;
    }
    return nullptr;
  }

  JavaVM* virtual_machine_ = nullptr;
  jobject view_ = nullptr;
  jmethodID schedule_tasks_ = nullptr;
  std::mutex mutex_;
  std::vector<std::function<void()>> tasks_;
  bool scheduled_ = false;
  bool closed_ = false;
};

UIThreadDispatcher MakeUIThreadDispatcher(const std::shared_ptr<AndroidUIThreadDispatcherState>& state) {
  const std::weak_ptr<AndroidUIThreadDispatcherState> weak_state = state;
  return [weak_state](std::function<void()> task) mutable {
    if (const std::shared_ptr locked_state = weak_state.lock()) {
      locked_state->Dispatch(std::move(task));
    }
  };
}

} // namespace

class AndroidViewPlatformAdapter final : public PlatformAdapter,
                                         public PlatformTextInput,
                                         public PlatformClipboard,
                                         public PlatformResources {
public:
  AndroidViewPlatformAdapter(JNIEnv* environment, jobject view)
      : AndroidViewPlatformAdapter(environment, view, std::make_shared<AndroidUIThreadDispatcherState>()) {}

private:
  AndroidViewPlatformAdapter(
      JNIEnv* environment, jobject view, std::shared_ptr<AndroidUIThreadDispatcherState> dispatch_state
  )
      : PlatformAdapter(MakeUIThreadDispatcher(dispatch_state)), dispatch_state_(std::move(dispatch_state)) {
    dispatch_state_->Initialize(environment, view);
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

    const jmethodID get_context = environment->GetMethodID(view_class, "getContext", "()Landroid/content/Context;");
    schedule_frame_ = environment->GetMethodID(view_class, "scheduleFrame", "(J)V");
    invalidate_full_frame_ = environment->GetMethodID(view_class, "invalidateFullFrame", "()V");
    font_metrics_ = environment->GetMethodID(view_class, "fontMetrics", "(FI[BII)[F");
    measure_text_ = environment->GetMethodID(view_class, "measureText", "([BFFI[BIIIII[B)[F");
    measure_text_run_ = environment->GetMethodID(view_class, "measureTextRun", "([BFI[BIIII[B)[F");
    create_text_layout_ =
        environment->GetMethodID(view_class, "createTextLayout", "([BFFI[BIIIII[B)Ljava/lang/Object;");
    start_text_input_ = environment->GetMethodID(view_class, "startTextInput", "(JIIIZZZJJJIJJIFFFF)V");
    update_text_input_ = environment->GetMethodID(view_class, "updateTextInput", "(JJJJIJJIFFFF)V");
    restart_text_input_ = environment->GetMethodID(view_class, "restartTextInput", "(JIIIZZZJJJIJJIFFFF)V");
    stop_text_input_ = environment->GetMethodID(view_class, "stopTextInput", "(J)V");
    request_show_text_input_ = environment->GetMethodID(view_class, "requestShowTextInput", "(J)V");
    read_clipboard_text_ = environment->GetMethodID(view_class, "readClipboardText", "()[B");
    write_clipboard_text_ = environment->GetMethodID(view_class, "writeClipboardText", "([B)Z");
    resource_locale_ = environment->GetMethodID(view_class, "resourceLocale", "()[B");
    resource_scale_ = environment->GetMethodID(view_class, "resourceScale", "()F");
    process_pss_bytes_ = environment->GetMethodID(view_class, "processPssBytes", "()J");
    read_resource_ = environment->GetMethodID(view_class, "readResource", "([B)[B");
    set_system_bars_content_brightness_ =
        environment->GetMethodID(view_class, "setSystemBarsContentBrightness", "(II)V");

    if (get_context == nullptr || schedule_frame_ == nullptr || invalidate_full_frame_ == nullptr ||
        font_metrics_ == nullptr || measure_text_ == nullptr || measure_text_run_ == nullptr ||
        create_text_layout_ == nullptr || start_text_input_ == nullptr || update_text_input_ == nullptr ||
        restart_text_input_ == nullptr || stop_text_input_ == nullptr || request_show_text_input_ == nullptr ||
        read_clipboard_text_ == nullptr || write_clipboard_text_ == nullptr || resource_locale_ == nullptr ||
        resource_scale_ == nullptr || process_pss_bytes_ == nullptr || read_resource_ == nullptr ||
        set_system_bars_content_brightness_ == nullptr) {
      if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
      }
      environment->DeleteLocalRef(view_class);
      environment->DeleteGlobalRef(view_);
      view_ = nullptr;
      throw std::runtime_error("HuxerUI Android view methods do not match the platform backend");
    }

    try {
      renderer_.Initialize(environment, view_class);
    } catch (...) {
      environment->DeleteLocalRef(view_class);
      environment->DeleteGlobalRef(view_);
      view_ = nullptr;
      throw;
    }
    environment->DeleteLocalRef(view_class);

    jobject local_context = environment->CallObjectMethod(view, get_context);
    if (environment->ExceptionCheck() || local_context == nullptr) {
      if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
      }
      environment->DeleteGlobalRef(view_);
      view_ = nullptr;
      throw std::runtime_error("HuxerUI Android view could not provide its Context");
    }
    context_ = environment->NewGlobalRef(local_context);
    environment->DeleteLocalRef(local_context);
    if (context_ == nullptr) {
      if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
      }
      environment->DeleteGlobalRef(view_);
      view_ = nullptr;
      throw std::runtime_error("HuxerUI could not retain the Android platform Context");
    }
  }

public:
  ~AndroidViewPlatformAdapter() override {
    JNIEnv* environment = Environment();
    if (dispatch_state_) {
      dispatch_state_->Shutdown(environment);
    }
    if (environment != nullptr && context_ != nullptr) {
      environment->DeleteGlobalRef(context_);
    }
    if (environment != nullptr && view_ != nullptr) {
      environment->DeleteGlobalRef(view_);
    }
  }

  void RequestFrameAt(double deadline) override {
    if (const std::optional<double> scheduled = frame_state_.Request(deadline, Now(), view_ != nullptr)) {
      ScheduleFrame(*scheduled);
    }
  }

  bool BeginFrameCommit() {
    return frame_state_.BeginCommit();
  }

  void CommitFrame(const FrameCommit& commit) {
    if (platform_views_ == nullptr) {
      throw std::logic_error("HuxerUI Android PlatformView host is not attached to Runtime");
    }
    platform_views_->Commit(Environment(), commit.render_frame);
    static_cast<void>(InvalidateDamage(commit.render_frame.damage));
    if (commit.next_frame_deadline.has_value()) {
      RequestFrameAt(*commit.next_frame_deadline);
    }
    FlushDeferredFrame();
  }

  void BeginDraw() {
    frame_state_.BeginPaint();
    renderer_.BeginDraw();
  }

  void DrawBase(JNIEnv* environment, jobject canvas) {
    if (platform_views_ != nullptr) {
      platform_views_->DrawBase(environment, canvas);
    }
  }

  void DrawSlice(JNIEnv* environment, jobject canvas, std::size_t first_command, std::size_t command_count) {
    if (platform_views_ != nullptr) {
      platform_views_->DrawSlice(environment, canvas, first_command, command_count);
    }
  }

  void EndDraw() {
    if (const std::optional<double> deadline = frame_state_.EndPaint(view_ != nullptr)) {
      ScheduleFrame(*deadline);
    }
  }

  void AttachRuntime(JNIEnv* environment, Runtime& runtime) {
    platform_views_ = std::make_unique<AndroidPlatformViews>(
        environment, view_, context_, renderer_, PlatformRegistry(), runtime, MakeUIThreadDispatcher(dispatch_state_));
  }

  ApplicationActivation DecodeApplicationActivation(
      JNIEnv* environment, const AndroidApplicationActivationInput& input
  ) const {
    return DecodeAndroidApplicationActivation(virtual_machine_, environment, context_, input);
  }

  void ShutdownPlatformViews() {
    if (platform_views_ != nullptr) {
      platform_views_->Shutdown(Environment());
      platform_views_.reset();
    }
  }

  void DrainPlatformTasks() {
    dispatch_state_->Drain();
  }

  std::optional<std::uint64_t> HitTestPlatformView(Point point) const {
    return platform_views_ == nullptr ? std::nullopt : platform_views_->HitTest(point);
  }

  void SynchronizePlatformViewFocus(std::optional<std::uint64_t> identity, bool focus_visible) {
    if (platform_views_ != nullptr) {
      platform_views_->SynchronizeFocus(identity, focus_visible);
    }
  }

  bool MoveFocusFromPlatformView(std::uint64_t identity, bool reverse) {
    return platform_views_ != nullptr && platform_views_->MoveFocus(identity, reverse);
  }

  double Now() const noexcept override {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
  }

  void SetSystemBarsContentBrightness(
      SystemBarContentBrightness status_bar, SystemBarContentBrightness navigation_bar
  ) override {
    JNIEnv* environment = Environment();
    if (environment == nullptr || view_ == nullptr) {
      return;
    }
    environment->CallVoidMethod(view_, set_system_bars_content_brightness_, static_cast<jint>(status_bar),
                                static_cast<jint>(navigation_bar));
  }

private:
  friend android::PlatformEnv android::GetPlatformEnv(PlatformAdapter& adapter);

  void ScheduleFrame(double deadline) {
    JNIEnv* environment = Environment();
    if (environment == nullptr || view_ == nullptr) {
      return;
    }
    const double delay_seconds = std::max(0.0, deadline - Now());
    double delay_milliseconds = std::ceil(delay_seconds * 1000.0);
    if (!std::isfinite(delay_milliseconds) || delay_milliseconds <= 0.0) {
      delay_milliseconds = 0.0;
    }
    const double bounded = std::min(delay_milliseconds, static_cast<double>(std::numeric_limits<jlong>::max()));
    environment->CallVoidMethod(view_, schedule_frame_, static_cast<jlong>(bounded));
  }

  void FlushDeferredFrame() {
    if (const std::optional<double> deadline = frame_state_.TakeDeferred(view_ != nullptr)) {
      ScheduleFrame(*deadline);
    }
  }

  bool InvalidateDamage(const DamageRegion& damage) {
    JNIEnv* environment = Environment();
    if (environment == nullptr || view_ == nullptr || (!damage.full && damage.rects.empty())) {
      return false;
    }
    environment->CallVoidMethod(view_, invalidate_full_frame_);
    const bool invalidated = !environment->ExceptionCheck();
    if (invalidated) {
      frame_state_.MarkPaintPending();
    }
    return invalidated;
  }

public:
  FontMetrics Metrics(const Font& font) override {
    JNIEnv* environment = Environment();
    if (environment == nullptr || view_ == nullptr) {
      return {};
    }
    jbyteArray family = ToByteArray(environment, font.FamilyName());
    if (family == nullptr) {
      return {};
    }
    auto* result = static_cast<jfloatArray>(environment->CallObjectMethod(
        view_,
        font_metrics_,
        font.Size(),
        static_cast<jint>(font.FamilyKind()),
        family,
        static_cast<jint>(font.Weight()),
        static_cast<jint>(font.Slant())
    ));
    environment->DeleteLocalRef(family);
    if (environment->ExceptionCheck()) {
      if (result != nullptr) {
        environment->DeleteLocalRef(result);
      }
      return {};
    }
    if (result == nullptr || environment->GetArrayLength(result) < 7) {
      if (result != nullptr) {
        environment->DeleteLocalRef(result);
      }
      return {};
    }
    jfloat values[7]{};
    environment->GetFloatArrayRegion(result, 0, 7, values);
    environment->DeleteLocalRef(result);
    if (environment->ExceptionCheck()) {
      return {};
    }
    return {values[0], values[1], values[2], values[3], values[4], values[5], values[6]};
  }

  TextRunMetrics
  MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options = {}) override {
    if (text.find_first_of("\r\n") != std::string_view::npos) {
      throw std::invalid_argument("HuxerUI text runs must not contain line breaks");
    }
    JNIEnv* environment = Environment();
    if (environment == nullptr || view_ == nullptr) {
      return {};
    }
    jbyteArray bytes = ToByteArray(environment, text);
    jbyteArray family = ToByteArray(environment, style.font.FamilyName());
    jbyteArray locale = ToByteArray(environment, options.locale);
    if (bytes == nullptr || family == nullptr || locale == nullptr) {
      if (bytes != nullptr) {
        environment->DeleteLocalRef(bytes);
      }
      if (family != nullptr) {
        environment->DeleteLocalRef(family);
      }
      if (locale != nullptr) {
        environment->DeleteLocalRef(locale);
      }
      return {};
    }
    auto* result = static_cast<jfloatArray>(environment->CallObjectMethod(
        view_,
        measure_text_run_,
        bytes,
        style.font.Size(),
        static_cast<jint>(style.font.FamilyKind()),
        family,
        static_cast<jint>(style.font.Weight()),
        static_cast<jint>(style.font.Slant()),
        static_cast<jint>(style.decoration),
        static_cast<jint>(options.direction),
        locale
    ));
    environment->DeleteLocalRef(locale);
    environment->DeleteLocalRef(family);
    environment->DeleteLocalRef(bytes);
    if (environment->ExceptionCheck()) {
      if (result != nullptr) {
        environment->DeleteLocalRef(result);
      }
      return {};
    }
    if (result == nullptr || environment->GetArrayLength(result) < 12) {
      if (result != nullptr) {
        environment->DeleteLocalRef(result);
      }
      return {};
    }
    jfloat values[12]{};
    environment->GetFloatArrayRegion(result, 0, 12, values);
    environment->DeleteLocalRef(result);
    if (environment->ExceptionCheck()) {
      return {};
    }
    const FontMetrics metrics{
        values[5],
        values[6],
        values[7],
        values[8],
        values[9],
        values[10],
        values[11],
    };
    return {values[0], {values[1], values[2], values[3], values[4]}, metrics};
  }

  TextLayoutMetrics MeasureText(
      std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options = {}
  ) override {
    JNIEnv* environment = Environment();
    if (environment == nullptr || view_ == nullptr) {
      return {};
    }
    jbyteArray bytes = ToByteArray(environment, text);
    jbyteArray family = ToByteArray(environment, style.font.FamilyName());
    jbyteArray locale = ToByteArray(environment, options.shaping.locale);
    if (bytes == nullptr || family == nullptr || locale == nullptr) {
      if (bytes != nullptr) {
        environment->DeleteLocalRef(bytes);
      }
      if (family != nullptr) {
        environment->DeleteLocalRef(family);
      }
      if (locale != nullptr) {
        environment->DeleteLocalRef(locale);
      }
      return {};
    }
    auto* result = static_cast<jfloatArray>(environment->CallObjectMethod(
        view_,
        measure_text_,
        bytes,
        style.font.Size(),
        max_width,
        static_cast<jint>(style.font.FamilyKind()),
        family,
        static_cast<jint>(style.font.Weight()),
        static_cast<jint>(style.font.Slant()),
        static_cast<jint>(options.align),
        static_cast<jint>(options.wrap),
        static_cast<jint>(options.shaping.direction),
        locale
    ));
    environment->DeleteLocalRef(locale);
    environment->DeleteLocalRef(family);
    environment->DeleteLocalRef(bytes);
    if (environment->ExceptionCheck()) {
      if (result != nullptr) {
        environment->DeleteLocalRef(result);
      }
      return {};
    }
    if (result == nullptr || environment->GetArrayLength(result) < 5) {
      if (result != nullptr) {
        environment->DeleteLocalRef(result);
      }
      return {};
    }
    jfloat values[5]{};
    environment->GetFloatArrayRegion(result, 0, 5, values);
    environment->DeleteLocalRef(result);
    if (environment->ExceptionCheck()) {
      return {};
    }
    return {
        {values[0], values[1]},
        values[2],
        values[3],
        static_cast<std::size_t>(std::max(0.0F, values[4])),
    };
  }

  std::unique_ptr<TextLayout> CreateTextLayout(
      std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options = {}
  ) override {
    JNIEnv* environment = Environment();
    if (environment == nullptr || view_ == nullptr) {
      return {};
    }
    jbyteArray bytes = ToByteArray(environment, text);
    jbyteArray family = ToByteArray(environment, style.font.FamilyName());
    jbyteArray locale = ToByteArray(environment, options.shaping.locale);
    if (bytes == nullptr || family == nullptr || locale == nullptr) {
      if (bytes != nullptr) {
        environment->DeleteLocalRef(bytes);
      }
      if (family != nullptr) {
        environment->DeleteLocalRef(family);
      }
      if (locale != nullptr) {
        environment->DeleteLocalRef(locale);
      }
      return {};
    }
    jobject layout = environment->CallObjectMethod(
        view_,
        create_text_layout_,
        bytes,
        style.font.Size(),
        max_width,
        static_cast<jint>(style.font.FamilyKind()),
        family,
        static_cast<jint>(style.font.Weight()),
        static_cast<jint>(style.font.Slant()),
        static_cast<jint>(options.align),
        static_cast<jint>(options.wrap),
        static_cast<jint>(options.shaping.direction),
        locale
    );
    environment->DeleteLocalRef(locale);
    environment->DeleteLocalRef(family);
    environment->DeleteLocalRef(bytes);
    if (environment->ExceptionCheck()) {
      if (layout != nullptr) {
        environment->DeleteLocalRef(layout);
      }
      return {};
    }
    if (layout == nullptr) {
      return {};
    }
    std::unique_ptr<TextLayout> result = CreateAndroidTextLayout(virtual_machine_, environment, layout);
    environment->DeleteLocalRef(layout);
    return result;
  }

  PlatformTextInput* TextInput() noexcept override {
    return this;
  }

  PlatformClipboard* Clipboard() noexcept override {
    return this;
  }

  PlatformResources* Resources() noexcept override {
    return this;
  }

  std::optional<ProcessMetrics> QueryProcessMetrics() noexcept override {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
      return std::nullopt;
    }
    JNIEnv* environment = Environment();
    if (environment == nullptr || view_ == nullptr) {
      return std::nullopt;
    }
    const jlong pss_bytes = environment->CallLongMethod(view_, process_pss_bytes_);
    if (environment->ExceptionCheck() || pss_bytes < 0) {
      return std::nullopt;
    }
    const long processor_count = sysconf(_SC_NPROCESSORS_ONLN);
    return ProcessMetrics{
        .cpu_time_seconds = TimevalSeconds(usage.ru_utime) + TimevalSeconds(usage.ru_stime),
        .memory_usage_bytes = static_cast<std::uint64_t>(pss_bytes),
        .processor_count = static_cast<std::uint32_t>(std::max(1L, processor_count)),
    };
  }

  ResourceConfiguration Configuration() const override {
    JNIEnv* environment = Environment();
    if (environment == nullptr || view_ == nullptr) {
      return {};
    }
    auto* locale_bytes = static_cast<jbyteArray>(environment->CallObjectMethod(view_, resource_locale_));
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI Android resource locale could not be read");
    }
    const std::string language_tag =
        locale_bytes == nullptr ? std::string{"en"} : FromByteArray(environment, locale_bytes);
    if (locale_bytes != nullptr) {
      environment->DeleteLocalRef(locale_bytes);
    }
    const float scale = environment->CallFloatMethod(view_, resource_scale_);
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI Android resource scale could not be read");
    }
    return {Locale::FromLanguageTag(language_tag), scale};
  }

  RawAsset Read(std::string_view package_path) override {
    if (!IsValidResourcePackagePath(package_path)) {
      throw std::logic_error("HuxerUI Android resource path is invalid");
    }
    JNIEnv* environment = Environment();
    if (environment == nullptr || view_ == nullptr) {
      return {};
    }
    jbyteArray path = ToByteArray(environment, package_path);
    if (path == nullptr) {
      return {};
    }
    auto* payload = static_cast<jbyteArray>(environment->CallObjectMethod(view_, read_resource_, path));
    environment->DeleteLocalRef(path);
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI Android packaged resource could not be read");
    }
    if (payload == nullptr) {
      return {};
    }
    const jsize length = environment->GetArrayLength(payload);
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    if (length > 0) {
      environment->GetByteArrayRegion(payload, 0, length, reinterpret_cast<jbyte*>(bytes.data()));
    }
    environment->DeleteLocalRef(payload);
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI Android packaged resource bytes could not be copied");
    }
    return RawAsset::FromBytes(std::move(bytes));
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
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) override {
    CallTextInput(start_text_input_, session_id, configuration, state, geometry);
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
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) override {
    CallTextInput(restart_text_input_, session_id, configuration, state, geometry);
  }

  void Stop(TextInputSessionId session_id) override {
    JNIEnv* environment = Environment();
    if (environment != nullptr && view_ != nullptr) {
      environment->CallVoidMethod(view_, stop_text_input_, static_cast<jlong>(session_id));
    }
  }

  void RequestShow(TextInputSessionId session_id) override {
    JNIEnv* environment = Environment();
    if (environment != nullptr && view_ != nullptr) {
      environment->CallVoidMethod(view_, request_show_text_input_, static_cast<jlong>(session_id));
    }
  }

private:
  std::shared_ptr<FilePickerTransport> CreateFilePickerTransport() override {
    return CreateAndroidFilePickerTransport(virtual_machine_, Environment(), view_, context_);
  }

  std::shared_ptr<FileSystem> CreateFileSystem() override {
    return CreateAndroidFileSystem(Environment(), context_);
  }

  std::shared_ptr<HttpTransport> CreateHttpTransport() override {
    return CreateAndroidHttpTransport(virtual_machine_, Environment());
  }

  void CallTextInput(
      jmethodID method,
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
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
        static_cast<jlong>(composition.end),
        static_cast<jint>(geometry.result_code),
        geometry.caret.x,
        geometry.caret.y,
        geometry.caret.width,
        geometry.caret.height
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

  AndroidRenderer renderer_;
  std::shared_ptr<AndroidUIThreadDispatcherState> dispatch_state_;
  std::unique_ptr<AndroidPlatformViews> platform_views_;
  JavaVM* virtual_machine_ = nullptr;
  jobject view_ = nullptr;
  jobject context_ = nullptr;
  jmethodID schedule_frame_ = nullptr;
  jmethodID invalidate_full_frame_ = nullptr;
  jmethodID font_metrics_ = nullptr;
  jmethodID measure_text_ = nullptr;
  jmethodID measure_text_run_ = nullptr;
  jmethodID create_text_layout_ = nullptr;
  jmethodID start_text_input_ = nullptr;
  jmethodID update_text_input_ = nullptr;
  jmethodID restart_text_input_ = nullptr;
  jmethodID stop_text_input_ = nullptr;
  jmethodID request_show_text_input_ = nullptr;
  jmethodID read_clipboard_text_ = nullptr;
  jmethodID write_clipboard_text_ = nullptr;
  jmethodID resource_locale_ = nullptr;
  jmethodID resource_scale_ = nullptr;
  jmethodID process_pss_bytes_ = nullptr;
  jmethodID read_resource_ = nullptr;
  jmethodID set_system_bars_content_brightness_ = nullptr;
  PlatformFrameState frame_state_;
};

class AndroidSession final {
public:
  AndroidSession(
      JNIEnv* environment,
      jobject view,
      const Application& application,
      const AndroidApplicationActivationInput& startup_activation
  )
      : platform_(environment, view),
        runtime_(application, platform_, platform_.DecodeApplicationActivation(environment, startup_activation)) {
    platform_.AttachRuntime(environment, runtime_);
  }

  ~AndroidSession() {
    platform_.ShutdownPlatformViews();
  }

  void Resize(float width, float height, float safe_left, float safe_top, float safe_right, float safe_bottom) {
    runtime_.SetWindowMetrics({
        .viewport = {std::max(0.0F, width), std::max(0.0F, height)},
        .safe_area = {
            .top = std::max(0.0F, safe_top),
            .right = std::max(0.0F, safe_right),
            .bottom = std::max(0.0F, safe_bottom),
            .left = std::max(0.0F, safe_left),
        },
    });
  }

  void UpdateResourceConfiguration(std::string language_tag, float display_scale) {
    runtime_.UpdateResourceConfiguration({Locale::FromLanguageTag(language_tag), display_scale});
  }

  void BeginDraw() {
    platform_.BeginDraw();
  }

  void DrawBase(JNIEnv* environment, jobject canvas) {
    platform_.DrawBase(environment, canvas);
  }

  void DrawSlice(JNIEnv* environment, jobject canvas, std::size_t first_command, std::size_t command_count) {
    platform_.DrawSlice(environment, canvas, first_command, command_count);
  }

  void EndDraw() {
    platform_.EndDraw();
  }

  void DrainPlatformTasks() {
    platform_.DrainPlatformTasks();
  }

  std::optional<std::uint64_t> HitTestPlatformView(Point point) const {
    return platform_.HitTestPlatformView(point);
  }

  void SynchronizePlatformViewFocus(std::optional<std::uint64_t> identity, bool focus_visible) {
    platform_.SynchronizePlatformViewFocus(identity, focus_visible);
  }

  bool MoveFocusFromPlatformView(std::uint64_t identity, bool reverse) {
    return platform_.MoveFocusFromPlatformView(identity, reverse);
  }

  std::optional<std::vector<std::uint8_t>> CommitFrame() {
    if (platform_.BeginFrameCommit()) {
      const FrameCommit& commit = runtime_.BuildFrame();
      platform_.CommitFrame(commit);
      if (commit.semantic_frame && last_semantic_revision_ != commit.semantic_frame->revision) {
        std::vector<std::uint8_t> encoded = EncodeAndroidSemanticFrame(*commit.semantic_frame);
        last_semantic_revision_ = commit.semantic_frame->revision;
        return encoded;
      }
    }
    return std::nullopt;
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

  bool HandleBack(BackPhase phase, float progress) {
    return runtime_.HandleBack({phase, progress});
  }

  void HandleApplicationActivation(JNIEnv* environment, const AndroidApplicationActivationInput& input) {
    runtime_.HandleApplicationActivation(platform_.DecodeApplicationActivation(environment, input));
  }

  void UpdateApplicationLifecycleState(jint state) {
    switch (state) {
    case 0:
      runtime_.UpdateApplicationLifecycleState(ApplicationLifecycleState::Active);
      return;
    case 1:
      runtime_.UpdateApplicationLifecycleState(ApplicationLifecycleState::Inactive);
      return;
    case 2:
      runtime_.UpdateApplicationLifecycleState(ApplicationLifecycleState::Background);
      return;
    default:
      throw std::invalid_argument("HuxerUI Android application lifecycle state is invalid");
    }
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

  bool PerformTextInputAction(TextInputSessionId session_id, TextInputAction action) {
    return runtime_.PerformTextInputAction(session_id, action);
  }

  bool PerformTextEditingAction(TextInputSessionId session_id, TextEditingAction action) {
    return (session_id == 0 ||
            runtime_.QueryTextInputContext(session_id, 0, 0).result_code == TextInputResultCode::Ok) &&
           runtime_.PerformTextEditingAction(action);
  }

  bool PerformSemanticAction(jint node_id, jint action_kind, std::string text, jlong argument0, jlong argument1,
                             jdouble number, jfloat x, jfloat y, jlong custom_id) {
    const std::optional<SemanticActionKind> semantic_action = ToSemanticAction(action_kind);
    if (node_id <= 0 || !semantic_action.has_value()) {
      return false;
    }
    SemanticAction action;
    action.kind = *semantic_action;
    switch (action.kind) {
    case SemanticActionKind::SetText:
      action.value = std::move(text);
      break;
    case SemanticActionKind::SetSelection:
      action.value = TextRange{static_cast<TextOffset>(argument0), static_cast<TextOffset>(argument1)};
      break;
    case SemanticActionKind::SetValue:
      action.value = static_cast<double>(number);
      break;
    case SemanticActionKind::Scroll:
      action.value = Point{x, y};
      break;
    case SemanticActionKind::Custom:
      action.value = static_cast<std::uint64_t>(custom_id);
      break;
    case SemanticActionKind::Activate:
    case SemanticActionKind::Focus:
    case SemanticActionKind::Increment:
    case SemanticActionKind::Decrement:
    case SemanticActionKind::ShowOnScreen:
    case SemanticActionKind::Expand:
    case SemanticActionKind::Collapse:
    case SemanticActionKind::Dismiss:
      action.value = std::monostate{};
      break;
    }
    return runtime_.PerformSemanticAction(static_cast<SemanticNodeId>(node_id), action);
  }

private:
  AndroidViewPlatformAdapter platform_;
  Runtime runtime_;
  std::optional<std::uint64_t> last_semantic_revision_;
};

AndroidSession* Session(jlong handle) {
  return reinterpret_cast<AndroidSession*>(static_cast<std::uintptr_t>(handle));
}

} // namespace huxerui::detail

namespace huxerui::android {

PlatformEnv GetPlatformEnv(PlatformAdapter& adapter) {
  auto* platform = dynamic_cast<huxerui::detail::AndroidViewPlatformAdapter*>(&adapter);
  if (platform == nullptr) {
    throw std::logic_error("HuxerUI Android platform factory requires an Android host");
  }
  JNIEnv* environment = platform->Environment();
  if (environment == nullptr || platform->context_ == nullptr) {
    throw std::logic_error("HuxerUI Android platform host is unavailable");
  }
  return {environment, platform->context_};
}

} // namespace huxerui::android

extern "C" JNIEXPORT jlong JNICALL Java_org_huxerui_HuxerUIView_nativeCreate(
    JNIEnv* environment, jclass, jobject view, jint kind, jstring value, jstring name, jlong size,
    jstring content_type, jboolean writable
) {
  try {
    const huxerui::detail::AndroidApplicationActivationInput startup_activation{
        .kind = kind,
        .value = value,
        .file_name = name,
        .file_size = size,
        .content_type = content_type,
        .writable = writable,
    };
    auto session = std::make_unique<huxerui::detail::AndroidSession>(
        environment, view, huxerui::detail::CurrentApplication(), startup_activation
    );
    return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(session.release()));
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
    return 0;
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIView_nativeHandleApplicationActivation(
    JNIEnv* environment, jclass, jlong handle, jint kind, jstring value, jstring name, jlong size,
    jstring content_type, jboolean writable
) {
  try {
    if (auto* session = huxerui::detail::Session(handle)) {
      const huxerui::detail::AndroidApplicationActivationInput activation{
          .kind = kind,
          .value = value,
          .file_name = name,
          .file_size = size,
          .content_type = content_type,
          .writable = writable,
      };
      session->HandleApplicationActivation(environment, activation);
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIView_nativeUpdateApplicationLifecycleState(
    JNIEnv* environment, jclass, jlong handle, jint lifecycle_state
) {
  try {
    if (auto* session = huxerui::detail::Session(handle)) {
      session->UpdateApplicationLifecycleState(lifecycle_state);
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIView_nativeDestroy(JNIEnv*, jclass, jlong handle) {
  delete huxerui::detail::Session(handle);
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIView_nativeResize(
    JNIEnv* environment, jclass, jlong handle, jfloat width, jfloat height, jfloat safe_left, jfloat safe_top,
    jfloat safe_right, jfloat safe_bottom
) {
  try {
    if (auto* session = huxerui::detail::Session(handle)) {
      session->Resize(width, height, safe_left, safe_top, safe_right, safe_bottom);
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIView_nativeUpdateResourceConfiguration(
    JNIEnv* environment, jclass, jlong handle, jbyteArray language_tag, jfloat display_scale
) {
  try {
    if (auto* session = huxerui::detail::Session(handle)) {
      session->UpdateResourceConfiguration(huxerui::detail::FromByteArray(environment, language_tag), display_scale);
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_huxerui_HuxerUIView_nativeCommitFrame(JNIEnv* environment, jclass, jlong handle) {
  try {
    if (auto* session = huxerui::detail::Session(handle)) {
      const std::optional<std::vector<std::uint8_t>> semantics = session->CommitFrame();
      return semantics.has_value() ? huxerui::detail::ToByteArray(environment, *semantics) : nullptr;
    }
    return nullptr;
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
    return nullptr;
  }
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_huxerui_HuxerUIView_nativePerformSemanticAction(
    JNIEnv* environment, jclass, jlong handle, jint node_id, jint action_kind, jbyteArray text, jlong argument0,
    jlong argument1, jdouble number, jfloat x, jfloat y, jlong custom_id
) {
  try {
    auto* session = huxerui::detail::Session(handle);
    if (session == nullptr) {
      return JNI_FALSE;
    }
    const bool handled = session->PerformSemanticAction(
        node_id, action_kind, huxerui::detail::FromByteArray(environment, text), argument0, argument1, number, x, y,
        custom_id
    );
    return handled ? JNI_TRUE : JNI_FALSE;
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
    return JNI_FALSE;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_org_huxerui_HuxerUIView_nativeBeginDraw(JNIEnv* environment, jclass, jlong handle) {
  try {
    if (auto* session = huxerui::detail::Session(handle)) {
      session->BeginDraw();
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}

extern "C" JNIEXPORT void JNICALL
Java_org_huxerui_HuxerUIView_nativeDrawBase(JNIEnv* environment, jclass, jlong handle, jobject canvas) {
  try {
    if (auto* session = huxerui::detail::Session(handle)) {
      session->DrawBase(environment, canvas);
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIView_nativeDrawSlice(
    JNIEnv* environment, jclass, jlong handle, jobject canvas, jlong first_command, jlong command_count
) {
  try {
    if (first_command < 0 || command_count < 0) {
      throw std::invalid_argument("HuxerUI Android RenderComposition slice indices must not be negative");
    }
    if (auto* session = huxerui::detail::Session(handle)) {
      session->DrawSlice(
          environment,
          canvas,
          static_cast<std::size_t>(first_command),
          static_cast<std::size_t>(command_count)
      );
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}

extern "C" JNIEXPORT void JNICALL
Java_org_huxerui_HuxerUIView_nativeEndDraw(JNIEnv* environment, jclass, jlong handle) {
  try {
    if (auto* session = huxerui::detail::Session(handle)) {
      session->EndDraw();
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}

extern "C" JNIEXPORT void JNICALL
Java_org_huxerui_HuxerUIView_nativeDrainPlatformTasks(JNIEnv* environment, jclass, jlong handle) {
  try {
    if (auto* session = huxerui::detail::Session(handle)) {
      session->DrainPlatformTasks();
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_huxerui_HuxerUIView_nativeHitTestPlatformView(JNIEnv* environment, jclass, jlong handle, jfloat x, jfloat y) {
  try {
    auto* session = huxerui::detail::Session(handle);
    if (session == nullptr) {
      return 0;
    }
    const std::optional<std::uint64_t> identity = session->HitTestPlatformView({x, y});
    if (!identity.has_value()) {
      return 0;
    }
    if (*identity > static_cast<std::uint64_t>(std::numeric_limits<jlong>::max())) {
      throw std::overflow_error("HuxerUI Android PlatformView identity exceeds the JNI range");
    }
    return static_cast<jlong>(*identity);
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
    return 0;
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIView_nativeSynchronizePlatformViewFocus(
    JNIEnv* environment, jclass, jlong handle, jlong identity, jboolean focus_visible
) {
  try {
    if (identity < 0) {
      throw std::invalid_argument("HuxerUI Android PlatformView focus identity must not be negative");
    }
    if (auto* session = huxerui::detail::Session(handle)) {
      const std::optional<std::uint64_t> focused =
          identity == 0 ? std::nullopt : std::optional{static_cast<std::uint64_t>(identity)};
      session->SynchronizePlatformViewFocus(focused, focus_visible == JNI_TRUE);
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_huxerui_HuxerUIView_nativeMoveFocusFromPlatformView(
    JNIEnv* environment, jclass, jlong handle, jlong identity, jboolean reverse
) {
  try {
    if (identity <= 0) {
      return JNI_FALSE;
    }
    auto* session = huxerui::detail::Session(handle);
    return session != nullptr &&
                   session->MoveFocusFromPlatformView(static_cast<std::uint64_t>(identity), reverse == JNI_TRUE)
               ? JNI_TRUE
               : JNI_FALSE;
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
    return JNI_FALSE;
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

extern "C" JNIEXPORT jboolean JNICALL
Java_org_huxerui_HuxerUIView_nativeHandleBack(JNIEnv* environment, jclass, jlong handle, jint phase, jfloat progress) {
  try {
    if (auto* session = huxerui::detail::Session(handle)) {
      huxerui::BackPhase back_phase;
      switch (phase) {
      case 0:
        back_phase = huxerui::BackPhase::Begin;
        break;
      case 1:
        back_phase = huxerui::BackPhase::Update;
        break;
      case 2:
        back_phase = huxerui::BackPhase::Cancel;
        break;
      case 3:
        back_phase = huxerui::BackPhase::Commit;
        break;
      default:
        throw std::invalid_argument("HuxerUI Android back phase is invalid");
      }
      return session->HandleBack(back_phase, progress) ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
    return JNI_FALSE;
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
    auto* session = huxerui::detail::Session(handle);
    const std::optional<huxerui::TextInputAction> action = huxerui::detail::ToTextInputAction(editor_action);
    return session != nullptr && action.has_value() &&
                   session->PerformTextInputAction(static_cast<huxerui::TextInputSessionId>(session_id), *action)
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
