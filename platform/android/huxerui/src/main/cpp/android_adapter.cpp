#include <huxerui/app.h>

#include <android/input.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/keycodes.h>
#include <jni.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <climits>
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
#include "application/platform_frame_internal.h"
#include "application/application_internal.h"
#include "resources/resource_internal.h"
#include "io/stream_internal.h"
#include "text/text_input_internal.h"
#include "text/text_internal.h"

namespace huxerui::detail {

namespace {

class AndroidAssetManager final {
public:
  AndroidAssetManager(JNIEnv* environment, jobject context) {
    if (environment->GetJavaVM(&vm_) != JNI_OK) {
      throw std::runtime_error("HuxerUI asset manager could not retain its Java VM");
    }
    android::LocalRef<jclass> type(environment, environment->GetObjectClass(context));
    const jmethodID get_assets =
        type ? environment->GetMethodID(type.Get(), "getAssets", "()Landroid/content/res/AssetManager;") : nullptr;
    if (!get_assets) {
      throw std::runtime_error("HuxerUI Android asset manager method is unavailable");
    }
    android::LocalRef<jobject> manager(environment, environment->CallObjectMethod(context, get_assets));
    if (environment->ExceptionCheck() || !manager) {
      throw std::runtime_error("HuxerUI Android asset manager is unavailable");
    }
    native_ = AAssetManager_fromJava(environment, manager.Get());
    if (!native_) {
      throw std::runtime_error("HuxerUI Android native asset manager is unavailable");
    }
    owner_ = environment->NewGlobalRef(manager.Get());
    if (!owner_) {
      throw std::runtime_error("HuxerUI Android asset manager could not be retained");
    }
  }

  ~AndroidAssetManager() {
    JNIEnv* environment = nullptr;
    bool attached = false;
    if (vm_->GetEnv(reinterpret_cast<void**>(&environment), JNI_VERSION_1_6) != JNI_OK) {
      if (vm_->AttachCurrentThread(&environment, nullptr) != JNI_OK) {
        return;
      }
      attached = true;
    }
    environment->DeleteGlobalRef(owner_);
    if (attached) {
      vm_->DetachCurrentThread();
    }
  }

  AAssetManager* Get() const noexcept { return native_; }

private:
  JavaVM* vm_ = nullptr;
  jobject owner_ = nullptr;
  AAssetManager* native_ = nullptr;
};

class AndroidAssetInputStreamState final : public InputStreamState {
public:
  AndroidAssetInputStreamState(std::shared_ptr<AndroidAssetManager> owner, std::string_view path)
      : owner_(std::move(owner)),
        asset_(AAssetManager_open(owner_->Get(), std::string(path).c_str(), AASSET_MODE_STREAMING)) {}
  ~AndroidAssetInputStreamState() override { Release(); }

  bool IsOpen() const noexcept { return asset_ != nullptr; }

  IoResult<std::size_t> Read(std::span<std::byte> buffer) override {
    const int count = AAsset_read(asset_, buffer.data(), std::min(buffer.size(), static_cast<std::size_t>(INT_MAX)));
    if (count < 0) {
      return IoResult<std::size_t>(IoError{IoErrorCode::Io, "HuxerUI Android package resource read failed"});
    }
    return IoResult<std::size_t>(static_cast<std::size_t>(count));
  }

  void Release() noexcept override {
    if (asset_) {
      AAsset_close(std::exchange(asset_, nullptr));
    }
  }

private:
  // The Java AssetManager must outlive all native assets, even after the ui_window is destroyed.
  std::shared_ptr<AndroidAssetManager> owner_;
  AAsset* asset_ = nullptr;
};

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
  case AndroidSemanticAction::SetSelected:
    return SemanticActionKind::SetSelected;
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
  if (key_code >= AKEYCODE_A && key_code <= AKEYCODE_Z) {
    return static_cast<Key>(static_cast<int>(Key::A) + key_code - AKEYCODE_A);
  }
  if (key_code >= AKEYCODE_0 && key_code <= AKEYCODE_9) {
    return static_cast<Key>(static_cast<int>(Key::Digit0) + key_code - AKEYCODE_0);
  }
  if (key_code >= AKEYCODE_F1 && key_code <= AKEYCODE_F12) {
    return static_cast<Key>(static_cast<int>(Key::F1) + key_code - AKEYCODE_F1);
  }
  if (key_code >= AKEYCODE_NUMPAD_0 && key_code <= AKEYCODE_NUMPAD_9) {
    return static_cast<Key>(static_cast<int>(Key::Numpad0) + key_code - AKEYCODE_NUMPAD_0);
  }
  switch (key_code) {
  case AKEYCODE_SHIFT_LEFT:
    return Key::ShiftLeft;
  case AKEYCODE_SHIFT_RIGHT:
    return Key::ShiftRight;
  case AKEYCODE_CTRL_LEFT:
    return Key::ControlLeft;
  case AKEYCODE_CTRL_RIGHT:
    return Key::ControlRight;
  case AKEYCODE_ALT_LEFT:
    return Key::AltLeft;
  case AKEYCODE_ALT_RIGHT:
    return Key::AltRight;
  case AKEYCODE_META_LEFT:
    return Key::MetaLeft;
  case AKEYCODE_META_RIGHT:
    return Key::MetaRight;
  case AKEYCODE_DEL:
    return Key::Backspace;
  case AKEYCODE_TAB:
    return Key::Tab;
  case AKEYCODE_ENTER:
    return Key::Enter;
  case AKEYCODE_NUMPAD_ENTER:
    return Key::NumpadEnter;
  case AKEYCODE_ESCAPE:
    return Key::Escape;
  case AKEYCODE_SPACE:
    return Key::Space;
  case AKEYCODE_INSERT:
    return Key::Insert;
  case AKEYCODE_FORWARD_DEL:
    return Key::Delete;
  case AKEYCODE_MOVE_HOME:
    return Key::Home;
  case AKEYCODE_MOVE_END:
    return Key::End;
  case AKEYCODE_PAGE_UP:
    return Key::PageUp;
  case AKEYCODE_PAGE_DOWN:
    return Key::PageDown;
  case AKEYCODE_DPAD_LEFT:
    return Key::ArrowLeft;
  case AKEYCODE_DPAD_RIGHT:
    return Key::ArrowRight;
  case AKEYCODE_DPAD_UP:
    return Key::ArrowUp;
  case AKEYCODE_DPAD_DOWN:
    return Key::ArrowDown;
  case AKEYCODE_GRAVE:
    return Key::Backquote;
  case AKEYCODE_MINUS:
    return Key::Minus;
  case AKEYCODE_EQUALS:
    return Key::Equal;
  case AKEYCODE_LEFT_BRACKET:
    return Key::BracketLeft;
  case AKEYCODE_RIGHT_BRACKET:
    return Key::BracketRight;
  case AKEYCODE_BACKSLASH:
    return Key::Backslash;
  case AKEYCODE_SEMICOLON:
    return Key::Semicolon;
  case AKEYCODE_APOSTROPHE:
    return Key::Quote;
  case AKEYCODE_COMMA:
    return Key::Comma;
  case AKEYCODE_PERIOD:
    return Key::Period;
  case AKEYCODE_SLASH:
    return Key::Slash;
  case AKEYCODE_RO:
    return Key::IntlRo;
  case AKEYCODE_YEN:
    return Key::IntlYen;
  case AKEYCODE_CAPS_LOCK:
    return Key::CapsLock;
  case AKEYCODE_NUM_LOCK:
    return Key::NumLock;
  case AKEYCODE_SCROLL_LOCK:
    return Key::ScrollLock;
  case AKEYCODE_SYSRQ:
    return Key::PrintScreen;
  case AKEYCODE_BREAK:
    return Key::Pause;
  case AKEYCODE_MENU:
    return Key::ContextMenu;
  case AKEYCODE_HELP:
    return Key::Help;
  case AKEYCODE_NUMPAD_DOT:
    return Key::NumpadDecimal;
  case AKEYCODE_NUMPAD_DIVIDE:
    return Key::NumpadDivide;
  case AKEYCODE_NUMPAD_MULTIPLY:
    return Key::NumpadMultiply;
  case AKEYCODE_NUMPAD_SUBTRACT:
    return Key::NumpadSubtract;
  case AKEYCODE_NUMPAD_ADD:
    return Key::NumpadAdd;
  case AKEYCODE_NUMPAD_EQUALS:
    return Key::NumpadEqual;
  case AKEYCODE_NUMPAD_COMMA:
    return Key::NumpadComma;
  case AKEYCODE_CLEAR:
    return Key::NumpadClear;
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

/// Independently retained JNI dispatch queue shared by the Runtime and outstanding dispatch handles.
/// Worker threads may enqueue work; main-thread Drain invokes callbacks after releasing the queue mutex.
/// Shutdown closes the queue before releasing the Java application host, so stale dispatchers become inert.
class AndroidUiThreadDispatcherState final {
public:
  ~AndroidUiThreadDispatcherState() {
    bool attached = false;
    JNIEnv* environment = Environment(attached);
    Shutdown(environment);
    if (attached) {
      virtual_machine_->DetachCurrentThread();
    }
  }

  /// Prepares JNI wakeup facilities before shared application initialization.
  /// @param environment JNI environment on the Android main thread.
  /// @param view HuxerUIApplication host providing schedulePlatformTasks; retained through a global reference.
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

  /// Enqueues a callback from any thread and coalesces Java main-Handler wakeups.
  /// @param task Callback retained until drain or shutdown; ignored once this queue is closed.
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

  /// Invokes the queued batch on the Android main thread without holding the queue mutex.
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

  /// Closes dispatch and releases pending captures outside the mutex.
  /// @param environment Valid JNI environment for releasing the host reference, or null if VM access failed.
  void Shutdown(JNIEnv* environment) {
    std::vector<std::function<void()>> retired;
    jobject host = nullptr;
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
      scheduled_ = false;
      retired.swap(tasks_);
      host = std::exchange(view_, nullptr);
    }
    if (environment != nullptr && host != nullptr) environment->DeleteGlobalRef(host);
  }
private:
  /// Obtains JNI access for a dispatcher call originating on any thread.
  /// @param attached Set to true only when this call attached the thread; the caller must then detach it.
  /// @return The thread-local JNI environment, or null if the VM is unavailable or attachment fails.
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

/// Creates a dispatch handle that does not extend the Java host's execution lifetime.
/// @param state Original queue retained by the Runtime.
/// @return An any-thread enqueue callback using a weak reference; expired queues silently drop work.
UiThreadDispatcher MakeUiThreadDispatcher(const std::shared_ptr<AndroidUiThreadDispatcherState>& state) {
  const std::weak_ptr<AndroidUiThreadDispatcherState> weak_state = state;
  return [weak_state](std::function<void()> task) mutable {
    if (const std::shared_ptr locked_state = weak_state.lock()) {
      locked_state->Dispatch(std::move(task));
    }
  };
}

} // namespace

/// Owns Java application services, global references, and the dispatcher independently of attached Android Views.
/// Native facilities are prepared before shared startup and remain alive until Runtime::Retire completes.
class AndroidRuntime final : public Runtime, public PlatformClipboard, public PlatformResources {
  friend android::PlatformEnv android::GetPlatformEnv(Runtime& runtime);

public:
  AndroidRuntime(JNIEnv* environment, jobject host,
                 std::shared_ptr<AndroidUiThreadDispatcherState> dispatcher)
      : Runtime(MakeUiThreadDispatcher(dispatcher)), dispatch_state_(std::move(dispatcher)) {
    dispatch_state_->Initialize(environment, host);
    if (environment->GetJavaVM(&virtual_machine_) != JNI_OK) {
      throw std::runtime_error("HuxerUI could not access the Android Java VM");
    }
    try {
      host_ = environment->NewGlobalRef(host);
      android::LocalRef<jclass> type(environment, environment->GetObjectClass(host));
      if (!host_ || !type) throw std::runtime_error("HuxerUI Android application host is unavailable");
      const jmethodID get_context = environment->GetMethodID(type.Get(), "getContext", "()Landroid/content/Context;");
      read_clipboard_text_ = environment->GetMethodID(type.Get(), "readClipboardText", "()[B");
      write_clipboard_text_ = environment->GetMethodID(type.Get(), "writeClipboardText", "([B)Z");
      resource_locale_ = environment->GetMethodID(type.Get(), "resourceLocale", "()[B");
      resource_scale_ = environment->GetMethodID(type.Get(), "resourceScale", "()F");
      process_pss_bytes_ = environment->GetMethodID(type.Get(), "processPssBytes", "()J");
      stopped_ = environment->GetMethodID(type.Get(), "onRuntimeStopped", "()V");
      if (!get_context || !read_clipboard_text_ || !write_clipboard_text_ || !resource_locale_ ||
          !resource_scale_ || !process_pss_bytes_ || !stopped_ || environment->ExceptionCheck()) {
        throw std::runtime_error("HuxerUI Android application methods do not match the platform backend");
      }
      android::LocalRef<jobject> context(environment, environment->CallObjectMethod(host, get_context));
      if (!context || environment->ExceptionCheck()) {
        throw std::runtime_error("HuxerUI Android application Context is unavailable");
      }
      context_ = environment->NewGlobalRef(context.Get());
      if (!context_) throw std::runtime_error("HuxerUI could not retain its Android application Context");
      asset_manager_ = std::make_shared<AndroidAssetManager>(environment, context_);
    } catch (...) {
      Release(environment);
      throw;
    }
  }

  ~AndroidRuntime() override {
    Retire();
    dispatch_state_->Shutdown(Environment());
    Release(Environment());
  }

  /// Borrows the queue shared by native attachments under this Runtime.
  /// @return Retained dispatcher state whose gate is closed before native application references are released.
  const std::shared_ptr<AndroidUiThreadDispatcherState>& Dispatcher() const { return dispatch_state_; }
  /// Borrows the process application Context on the application thread.
  /// @return JNI global reference owned by this Runtime; callers must not delete it.
  jobject Context() const noexcept { return context_; }
  /// Returns the VM used to acquire thread-local JNI environments.
  /// @return Process VM pointer; no JNI attachment or reference ownership is transferred.
  JavaVM* VirtualMachine() const noexcept { return virtual_machine_; }
  std::function<void()> on_stopped;
  std::uint64_t identity = 0;
  /// Initializes shared services after Java application facilities and dispatch are ready.
  /// @param environment JNI environment on the application thread.
  /// @param foreground Whether to deliver startup activation; false starts services without a launch event.
  /// @param lifecycle Initial aggregate Activity state exposed during application hooks.
  /// @param input Borrowed activation envelope; missing foreground data becomes LaunchActivation.
  void Start(JNIEnv* environment, bool foreground, ApplicationLifecycleState lifecycle,
      const AndroidApplicationActivationInput& input) {
    InitializeApplication(CurrentApplication(), foreground
        ? std::optional<ApplicationActivation>(DecodeAndroidApplicationActivation(
              virtual_machine_, environment, context_, input).value_or(LaunchActivation{}))
        : std::nullopt, lifecycle);
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
    if (environment == nullptr || host_ == nullptr) {
      return std::nullopt;
    }
    const jlong pss_bytes = environment->CallLongMethod(host_, process_pss_bytes_);
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
    if (environment == nullptr || host_ == nullptr) {
      return {};
    }
    auto* locale_bytes = static_cast<jbyteArray>(environment->CallObjectMethod(host_, resource_locale_));
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI Android resource locale could not be read");
    }
    const std::string language_tag =
        locale_bytes == nullptr ? std::string{"en"} : FromByteArray(environment, locale_bytes);
    if (locale_bytes != nullptr) {
      environment->DeleteLocalRef(locale_bytes);
    }
    const float scale = environment->CallFloatMethod(host_, resource_scale_);
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI Android resource scale could not be read");
    }
    return {Locale::FromLanguageTag(language_tag), scale};
  }

  std::optional<InputStream> OpenRead(std::string_view package_path) override {
    if (!IsValidResourcePackagePath(package_path)) {
      throw std::logic_error("HuxerUI Android resource path is invalid");
    }
    auto state = std::make_shared<AndroidAssetInputStreamState>(asset_manager_, package_path);
    if (!state->IsOpen()) {
      return std::nullopt;
    }
    return StreamAccess::MakeInputStream(std::move(state));
  }

  std::optional<std::string> ReadText() override {
    JNIEnv* environment = Environment();
    if (environment == nullptr || host_ == nullptr) {
      return std::nullopt;
    }
    auto* bytes = static_cast<jbyteArray>(environment->CallObjectMethod(host_, read_clipboard_text_));
    if (bytes == nullptr || environment->ExceptionCheck()) {
      return std::nullopt;
    }
    std::string text = FromByteArray(environment, bytes);
    environment->DeleteLocalRef(bytes);
    return text;
  }

  bool WriteText(std::string_view text) override {
    JNIEnv* environment = Environment();
    if (environment == nullptr || host_ == nullptr) {
      return false;
    }
    jbyteArray bytes = ToByteArray(environment, text);
    if (bytes == nullptr) {
      return false;
    }
    const bool result = environment->CallBooleanMethod(host_, write_clipboard_text_, bytes) == JNI_TRUE;
    environment->DeleteLocalRef(bytes);
    return result && !environment->ExceptionCheck();
  }

private:
  std::optional<AppDirectories> CreateAppDirectories() override {
    return CreateAndroidAppDirectories(Environment(), context_);
  }

  std::shared_ptr<HttpTransport> CreateHttpTransport() override {
    return CreateAndroidHttpTransport(virtual_machine_, Environment());
  }

  std::shared_ptr<LocalNotificationTransport> CreateLocalNotificationTransport() override {
    return CreateAndroidLocalNotificationTransport(virtual_machine_, Environment(), host_);
  }

  std::shared_ptr<PermissionTransport> CreatePermissionTransport() override {
    return CreateAndroidPermissionTransport(virtual_machine_, Environment(), host_);
  }


  /// Borrows JNI access from an already attached calling thread without attaching a worker.
  /// @return Thread-local JNI environment, or null when this thread cannot access the VM.
  JNIEnv* Environment() const noexcept {
    JNIEnv* environment = nullptr;
    if (virtual_machine_) virtual_machine_->GetEnv(reinterpret_cast<void**>(&environment), JNI_VERSION_1_6);
    return environment;
  }

  void OnRuntimeStopped() override {
    JNIEnv* environment = Environment();
    if (environment && host_) environment->CallVoidMethod(host_, stopped_);
    if (on_stopped) on_stopped();
  }

  /// Releases application-owned Java global references after shared retirement or failed native preparation.
  /// @param environment JNI environment on the current attached thread; null leaves reference release unavailable.
  void Release(JNIEnv* environment) noexcept {
    if (!environment) return;
    if (context_) environment->DeleteGlobalRef(context_);
    if (host_) environment->DeleteGlobalRef(host_);
    context_ = nullptr;
    host_ = nullptr;
  }

  std::shared_ptr<AndroidUiThreadDispatcherState> dispatch_state_;
  JavaVM* virtual_machine_ = nullptr;
  jobject host_ = nullptr;
  jobject context_ = nullptr;
  std::shared_ptr<AndroidAssetManager> asset_manager_;
  jmethodID read_clipboard_text_ = nullptr;
  jmethodID write_clipboard_text_ = nullptr;
  jmethodID resource_locale_ = nullptr;
  jmethodID resource_scale_ = nullptr;
  jmethodID process_pss_bytes_ = nullptr;
  jmethodID stopped_ = nullptr;
};

std::shared_ptr<AndroidRuntime> active_application;
std::uint64_t next_application_identity = 1;

/// Resolves a JNI application handle without accepting a stale generation after Runtime replacement.
/// @param identity Original nonzero Runtime identity supplied to Java.
/// @return A retained matching Runtime, or null; called on the Android application thread.
std::shared_ptr<AndroidRuntime> ApplicationHost(jlong identity) {
  return active_application && active_application->identity == static_cast<std::uint64_t>(identity)
      ? active_application : nullptr;
}
/// Owns one HuxerUIView attachment and its native input/rendering state under the application Runtime.
class AndroidUiWindow final : public UiWindow, public PlatformTextInput {
public:
  AndroidUiWindow(JNIEnv* environment, jobject view, AndroidRuntime& application, WindowLifecycleState lifecycle) {
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
    measure_text_ = environment->GetMethodID(view_class, "measureText", "([BFFI[BIIIII[B[B)[F");
    measure_text_run_ = environment->GetMethodID(view_class, "measureTextRun", "([BFI[BIIII[B)[F");
    create_text_layout_ =
        environment->GetMethodID(view_class, "createTextLayout", "([BFFI[BIIIII[B[B)Ljava/lang/Object;");
    start_text_input_ = environment->GetMethodID(view_class, "startTextInput", "(JIIIZZZJJJIJJIFFFF)V");
    update_text_input_ = environment->GetMethodID(view_class, "updateTextInput", "(JJJJIJJIFFFF)V");
    restart_text_input_ = environment->GetMethodID(view_class, "restartTextInput", "(JIIIZZZJJJIJJIFFFF)V");
    stop_text_input_ = environment->GetMethodID(view_class, "stopTextInput", "(J)V");
    request_show_text_input_ = environment->GetMethodID(view_class, "requestShowTextInput", "(J)V");
    resource_locale_ = environment->GetMethodID(view_class, "resourceLocale", "()[B");
    resource_scale_ = environment->GetMethodID(view_class, "resourceScale", "()F");
    set_system_bars_content_brightness_ =
        environment->GetMethodID(view_class, "setSystemBarsContentBrightness", "(II)V");
    set_pointer_cursor_ = environment->GetMethodID(view_class, "setHuxerUIPointerCursor", "(I)V");

    if (get_context == nullptr || schedule_frame_ == nullptr || invalidate_full_frame_ == nullptr ||
        font_metrics_ == nullptr || measure_text_ == nullptr || measure_text_run_ == nullptr ||
        create_text_layout_ == nullptr || start_text_input_ == nullptr || update_text_input_ == nullptr ||
        restart_text_input_ == nullptr || stop_text_input_ == nullptr || request_show_text_input_ == nullptr ||
        resource_locale_ == nullptr || resource_scale_ == nullptr ||
        set_system_bars_content_brightness_ == nullptr || set_pointer_cursor_ == nullptr) {
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
    try {
      InitializeWindow(application, Configuration(), lifecycle);
      platform_views_ = std::make_unique<AndroidPlatformViews>(
          environment, view_, context_, renderer_, PlatformRegistry(), *this);
    } catch (...) {
      Retire();
      environment->DeleteGlobalRef(context_);
      environment->DeleteGlobalRef(view_);
      context_ = nullptr;
      view_ = nullptr;
      throw;
    }
  }

public:
  ~AndroidUiWindow() override {
    Retire();
    ShutdownPlatformViews();
    JNIEnv* environment = Environment();
    if (environment != nullptr && context_ != nullptr) {
      environment->DeleteGlobalRef(context_);
    }
    if (environment != nullptr && view_ != nullptr) {
      environment->DeleteGlobalRef(view_);
    }
  }

  void Resize(float width, float height, float safe_left, float safe_top, float safe_right, float safe_bottom) {
    UiWindow::SetWindowMetrics({
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
    UiWindow::UpdateResourceConfiguration({Locale::FromLanguageTag(language_tag), display_scale});
  }

  std::optional<std::vector<std::uint8_t>> CommitFrame() {
    if (BeginFrameCommit()) {
      const FrameCommit& commit = UiWindow::BuildFrame();
      CommitFrame(commit);
      if (commit.semantic_frame && last_semantic_revision_ != commit.semantic_frame->revision) {
        std::vector<std::uint8_t> encoded = EncodeAndroidSemanticFrame(*commit.semantic_frame);
        last_semantic_revision_ = commit.semantic_frame->revision;
        return encoded;
      }
    }
    return std::nullopt;
  }

  void Pointer(PointerEventType type, PointerDeviceKind device_kind, std::int64_t pointer_id, float x, float y,
      PointerButton changed_button, PointerButton pressed_buttons, KeyModifiers modifiers) {
    UiWindow::HandlePointerEvent({
        type,
        pointer_id,
        {x, y},
        device_kind,
        changed_button,
        pressed_buttons,
        modifiers,
    });
  }

  bool Scroll(float x, float y, float delta_x, float delta_y, KeyModifiers modifiers) {
    const Point consumed = UiWindow::HandleScrollInput({{x, y}, delta_x, delta_y, modifiers});
    return consumed.x != 0.0F || consumed.y != 0.0F;
  }

  bool FileDrag(std::uint64_t session, int phase, Point position, FileDropOffer offer, FileDropPreparation source) {
    switch (phase) {
    case 0:
      return UiWindow::HandleFileDragEntered(session, std::move(offer), position);
    case 1:
      return UiWindow::HandleFileDragMoved(session, std::move(offer), position);
    case 2:
      UiWindow::HandleFileDragExited(session);
      return false;
    case 3:
      return UiWindow::HandleFileDrop(session, std::move(offer), position, std::move(source));
    default:
      return false;
    }
  }

  bool KeyEvent(KeyEventType type, jint key_code, std::string text, KeyModifiers modifiers, bool repeat) {
    return UiWindow::HandleKeyEvent({
        type,
        TranslateKey(key_code),
        std::move(text),
        modifiers,
        repeat,
    });
  }

  bool HandleBack(BackPhase phase, float progress) {
    return UiWindow::HandleBack({phase, progress});
  }

  /// Converts the Java window lifecycle value at the native boundary.
  /// @param state Active (0), inactive (1), or background (2); other values throw std::invalid_argument.
  void UpdateWindowLifecycleState(jint state) {
    switch (state) {
    case 0:
      UiWindow::UpdateWindowLifecycleState(WindowLifecycleState::Active);
      return;
    case 1:
      UiWindow::UpdateWindowLifecycleState(WindowLifecycleState::Inactive);
      return;
    case 2:
      UiWindow::UpdateWindowLifecycleState(WindowLifecycleState::Background);
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
      const TextInputContext context = UiWindow::QueryTextInputContext(session_id, 0, 0);
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
      const TextInputContext context = UiWindow::QueryTextInputContext(session_id, 0, 0);
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

    const TextInputApplyResult result = UiWindow::HandleTextInputCommands(batch);
    return result.result_code == TextInputResultCode::Ok;
  }

  bool PerformTextEditingAction(TextInputSessionId session_id, TextEditingAction action) {
    return (session_id == 0 ||
            UiWindow::QueryTextInputContext(session_id, 0, 0).result_code == TextInputResultCode::Ok) &&
           UiWindow::PerformTextEditingAction(action);
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
    case SemanticActionKind::SetSelected:
      if (argument0 != 0 && argument0 != 1) {
        return false;
      }
      action.value = argument0 != 0;
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
    return UiWindow::PerformSemanticAction(static_cast<SemanticNodeId>(node_id), action);
  }

  /// Borrows the original Java View for a presentation request on the application thread.
  /// @return UiWindow-owned JNI reference, valid only while this window attachment remains live.
  jobject NativeView() const noexcept { return view_; }

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
      throw std::logic_error("HuxerUI Android PlatformView host is not attached to UiWindow");
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

  void SetTextureLayerSurface(
      JNIEnv* environment, std::uint64_t identity, jobject surface, int pixel_width, int pixel_height
  ) {
    if (platform_views_ != nullptr) {
      platform_views_->SetTextureLayerSurface(environment, identity, surface, pixel_width, pixel_height);
    }
  }

  void ClearTextureLayerSurface(std::uint64_t identity) noexcept {
    if (platform_views_ != nullptr) {
      platform_views_->ClearTextureLayerSurface(identity);
    }
  }

  void EndDraw() {
    if (const std::optional<double> deadline = frame_state_.EndPaint(view_ != nullptr)) {
      ScheduleFrame(*deadline);
    }
  }

  void ShutdownPlatformViews() {
    if (platform_views_ != nullptr) {
      platform_views_->Shutdown(Environment());
      platform_views_.reset();
    }
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

  void SetPointerCursor(PointerCursorKind kind) override {
    JNIEnv* environment = Environment();
    if (environment != nullptr && view_ != nullptr) {
      environment->CallVoidMethod(view_, set_pointer_cursor_, static_cast<jint>(kind));
    }
  }

private:
  friend android::PlatformEnv android::GetPlatformEnv(UiWindow& ui_window);

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

  TextLayoutMetrics MeasureText(const AttributedText& text, const TextStyle& style, float max_width,
      const TextLayoutOptions& options = {}) override {
    JNIEnv* environment = Environment();
    if (environment == nullptr || view_ == nullptr) {
      return {};
    }
    auto attributes = AndroidTextAttributes(environment, text, style);
    if (!attributes) {
      return {};
    }
    jbyteArray bytes = ToByteArray(environment, text.PlainText());
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
    auto* result = static_cast<jfloatArray>(environment->CallObjectMethod(view_, measure_text_, bytes,
        style.font.Size(), max_width, static_cast<jint>(style.font.FamilyKind()), family,
        static_cast<jint>(style.font.Weight()), static_cast<jint>(style.font.Slant()),
        static_cast<jint>(options.align), static_cast<jint>(options.wrap), static_cast<jint>(options.shaping.direction),
        locale, attributes.Get()));
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

  std::unique_ptr<TextLayout> CreateTextLayout(const AttributedText& text, const TextStyle& style, float max_width,
      const TextLayoutOptions& options = {}) override {
    JNIEnv* environment = Environment();
    if (environment == nullptr || view_ == nullptr) {
      return {};
    }
    auto attributes = AndroidTextAttributes(environment, text, style);
    if (!attributes) {
      return {};
    }
    jbyteArray bytes = ToByteArray(environment, text.PlainText());
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
    jobject layout = environment->CallObjectMethod(view_, create_text_layout_, bytes, style.font.Size(), max_width,
        static_cast<jint>(style.font.FamilyKind()), family, static_cast<jint>(style.font.Weight()),
        static_cast<jint>(style.font.Slant()), static_cast<jint>(options.align), static_cast<jint>(options.wrap),
        static_cast<jint>(options.shaping.direction), locale, attributes.Get());
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

  ResourceConfiguration Configuration() const {
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
  jmethodID resource_locale_ = nullptr;
  jmethodID resource_scale_ = nullptr;
  jmethodID set_system_bars_content_brightness_ = nullptr;
  jmethodID set_pointer_cursor_ = nullptr;
  PlatformFrameState frame_state_;
  std::optional<std::uint64_t> last_semantic_revision_;
};


jobject CurrentAndroidPresentationView() {
  const auto source = CurrentExecutionContext();
  if (!source || !source->requires_ui) return nullptr;
  const auto ui = source->ui.lock();
  auto* ui_window = ui ? dynamic_cast<AndroidUiWindow*>(ui->ui_window) : nullptr;
  if (!ui_window) throw std::logic_error("HuxerUI Android presentation source is no longer attached");
  return ui_window->NativeView();
}

AndroidUiWindow* Session(jlong handle) {
  return reinterpret_cast<AndroidUiWindow*>(static_cast<std::uintptr_t>(handle));
}

} // namespace huxerui::detail

namespace huxerui::android {

PlatformEnv GetPlatformEnv(Runtime& runtime) {
  const auto application = huxerui::detail::CurrentApplicationRuntime();
  auto* platform = dynamic_cast<huxerui::detail::AndroidRuntime*>(&runtime);
  if (!platform || application->owner != platform) {
    throw std::logic_error("HuxerUI Android platform factory requires its original application host");
  }
  JNIEnv* environment = platform->Environment();
  if (!environment || !platform->Context()) {
    throw std::logic_error("HuxerUI Android application host is unavailable");
  }
  return {environment, platform->Context()};
}

PlatformEnv GetPlatformEnv(UiWindow& ui_window) {
  static_cast<void>(huxerui::detail::CurrentApplicationRuntime());
  auto* platform = dynamic_cast<huxerui::detail::AndroidUiWindow*>(&ui_window);
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

extern "C" JNIEXPORT jlong JNICALL Java_org_huxerui_HuxerUIApplication_nativeInitialize(
    JNIEnv* environment, jclass, jobject host, jboolean foreground, jint lifecycle,
    jint kind, jstring value, jstring name, jlong size, jstring content_type, jboolean writable, jbyteArray data) {
  try {
    if (huxerui::detail::active_application) throw std::logic_error("HuxerUI Android application is already running");
    if (lifecycle < 0 || lifecycle > 2) throw std::invalid_argument("HuxerUI Android application lifecycle is invalid");
    const huxerui::detail::AndroidApplicationActivationInput input{
        .kind = kind, .value = value, .file_name = name, .file_size = size,
        .content_type = content_type, .writable = writable, .data = data,
    };
    auto application = std::make_shared<huxerui::detail::AndroidRuntime>(
        environment, host, std::make_shared<huxerui::detail::AndroidUiThreadDispatcherState>());
    application->Start(environment, foreground == JNI_TRUE,
        static_cast<huxerui::ApplicationLifecycleState>(lifecycle), input);
    application->identity = huxerui::detail::next_application_identity++;
    const auto identity = application->identity;
    application->on_stopped = [identity] {
      if (huxerui::detail::active_application && huxerui::detail::active_application->identity == identity) {
        huxerui::detail::active_application.reset();
      }
    };
    huxerui::detail::active_application = std::move(application);
    return static_cast<jlong>(identity);
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
    return 0;
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIApplication_nativeRequestShutdown(
    JNIEnv* environment, jclass, jlong identity) {
  try {
    if (auto application = huxerui::detail::ApplicationHost(identity)) application->RequestShutdown();
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIApplication_nativeDrainTasks(
    JNIEnv* environment, jclass, jlong identity) {
  try {
    if (auto application = huxerui::detail::ApplicationHost(identity)) application->Dispatcher()->Drain();
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIApplication_nativeUpdateLifecycleState(
    JNIEnv* environment, jclass, jlong identity, jint state) {
  try {
    if (state < 0 || state > 2) throw std::invalid_argument("HuxerUI Android application lifecycle is invalid");
    if (auto application = huxerui::detail::ApplicationHost(identity)) {
      application->UpdateApplicationLifecycleState(static_cast<huxerui::ApplicationLifecycleState>(state));
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIApplication_nativeUpdateResources(
    JNIEnv* environment, jclass, jlong identity) {
  try {
    if (auto application = huxerui::detail::ApplicationHost(identity)) {
      application->UpdateResourceConfiguration(application->Configuration());
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIApplication_nativeHandleActivation(
    JNIEnv* environment, jclass, jlong identity, jint kind, jstring value, jstring name, jlong size,
    jstring content_type, jboolean writable, jbyteArray data) {
  try {
    if (auto application = huxerui::detail::ApplicationHost(identity)) {
      const huxerui::detail::AndroidApplicationActivationInput input{
          .kind = kind, .value = value, .file_name = name, .file_size = size,
          .content_type = content_type, .writable = writable, .data = data,
      };
      if (auto activation = huxerui::detail::DecodeAndroidApplicationActivation(
              application->VirtualMachine(), environment, application->Context(), input)) {
        application->HandleApplicationActivation(std::move(*activation));
      }
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}
extern "C" JNIEXPORT jlong JNICALL Java_org_huxerui_HuxerUIView_nativeCreate(
    JNIEnv* environment, jclass, jobject view, jint lifecycle) {
  try {
    auto application = huxerui::detail::active_application;
    if (!application) throw std::logic_error("HuxerUI Android application has not been initialized");
    auto session = std::make_unique<huxerui::detail::AndroidUiWindow>(
        environment, view, *application, static_cast<huxerui::WindowLifecycleState>(lifecycle));
    return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(session.release()));
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
    return 0;
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIView_nativeUpdateWindowLifecycleState(
    JNIEnv* environment, jclass, jlong handle, jint lifecycle_state
) {
  try {
    if (auto* session = huxerui::detail::Session(handle)) {
      session->UpdateWindowLifecycleState(lifecycle_state);
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

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIView_nativeSetTextureLayerSurface(
    JNIEnv* environment, jclass, jlong handle, jlong identity, jobject surface, jint pixel_width, jint pixel_height
) {
  try {
    if (identity <= 0 || surface == nullptr || pixel_width <= 0 || pixel_height <= 0) {
      throw std::invalid_argument("HuxerUI Android texture layer Surface arguments must be valid");
    }
    if (auto* session = huxerui::detail::Session(handle)) {
      session->SetTextureLayerSurface(
          environment, static_cast<std::uint64_t>(identity), surface, static_cast<int>(pixel_width),
          static_cast<int>(pixel_height)
      );
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}

extern "C" JNIEXPORT void JNICALL
Java_org_huxerui_HuxerUIView_nativeClearTextureLayerSurface(JNIEnv*, jclass, jlong handle, jlong identity) {
  if (identity > 0) {
    if (auto* session = huxerui::detail::Session(handle)) {
      session->ClearTextureLayerSurface(static_cast<std::uint64_t>(identity));
    }
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

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIView_nativePointer(JNIEnv* environment, jclass, jlong handle,
    jint type, jint device_kind, jlong pointer_id, jfloat x, jfloat y, jint changed_button, jint pressed_buttons,
    jboolean shift, jboolean control, jboolean alt, jboolean meta) {
  try {
    if (auto* session = huxerui::detail::Session(handle)) {
      const auto event_type = static_cast<huxerui::PointerEventType>(type);
      const auto device = static_cast<huxerui::PointerDeviceKind>(device_kind);
      const auto changed = static_cast<huxerui::PointerButton>(changed_button & 31);
      const auto pressed = static_cast<huxerui::PointerButton>(pressed_buttons & 31);
      session->Pointer(event_type, device, pointer_id, x, y, changed, pressed,
          {shift != JNI_FALSE, control != JNI_FALSE, alt != JNI_FALSE, meta != JNI_FALSE});
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_huxerui_HuxerUIView_nativeFileDrag(
    JNIEnv* environment, jclass, jlong handle, jlong drag, jint phase, jfloat x, jfloat y,
    jobjectArray content_types, jobject operation) {
  try {
    if (auto* session = huxerui::detail::Session(handle); session && drag > 0) {
      huxerui::FileDropOffer offer;
      const jsize count = content_types ? environment->GetArrayLength(content_types) : 0;
      for (jsize index = 0; index < count; ++index) {
        huxerui::android::LocalRef<jstring> type(
            environment, static_cast<jstring>(environment->GetObjectArrayElement(content_types, index))
        );
        if (type) {
          offer.content_types.push_back(huxerui::android::JavaStringToUtf8(environment, type.Get()));
        }
      }
      huxerui::detail::FileDropPreparation source;
      if (phase == 3 && operation) {
        source = huxerui::detail::CaptureAndroidFileDrop(environment, operation);
      }
      return session->FileDrag(static_cast<std::uint64_t>(drag), phase, {x, y}, std::move(offer), std::move(source))
                 ? JNI_TRUE : JNI_FALSE;
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
  return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_huxerui_HuxerUIView_nativeScroll(
    JNIEnv* environment, jclass, jlong handle, jfloat x, jfloat y, jfloat delta_x, jfloat delta_y, jboolean shift,
    jboolean control, jboolean alt, jboolean meta
) {
  try {
    if (auto* session = huxerui::detail::Session(handle)) {
      return session->Scroll(x, y, delta_x, delta_y, {shift != JNI_FALSE, control != JNI_FALSE, alt != JNI_FALSE,
                                                       meta != JNI_FALSE});
    }
  } catch (const std::exception& exception) {
    huxerui::detail::ThrowJavaException(environment, exception.what());
  }
  return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_huxerui_HuxerUIView_nativeKey(
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
      return session->KeyEvent(
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
  return false;
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
