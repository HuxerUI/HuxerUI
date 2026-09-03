#include <windows.h>
#include <msiquery.h>
#include <objbase.h>
#include <msxml.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <dutil.h>
#include <dictutil.h>
#include <BootstrapperApplicationBase.h>
#include <BootstrapperApplication.h>
#include <balutil.h>
#include <strutil.h>
#include <huxerui/app.h>
#include <huxerui/state.h>
#include <huxerui/windows/installer.h>

#include "win32_application_runner.h"

namespace huxerui::detail {

namespace {

using Microsoft::WRL::ComPtr;

std::string WideToUtf8(const wchar_t* value) {
  if (value == nullptr || *value == L'\0') {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) {
    return {};
  }
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
  result.pop_back();
  return result;
}

std::string ErrorMessage(HRESULT result) {
  wchar_t* message = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
      static_cast<DWORD>(result), 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
  std::string text = length == 0 ? "Windows installation engine operation failed" : WideToUtf8(message);
  if (message != nullptr) {
    LocalFree(message);
  }
  while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
    text.pop_back();
  }
  return text;
}

DWORD InstallerExitCode(HRESULT result) {
  if (HRESULT_SEVERITY(result) == SEVERITY_ERROR && HRESULT_FACILITY(result) == FACILITY_WIN32) {
    return HRESULT_CODE(result);
  }
  return static_cast<DWORD>(result);
}

std::optional<std::filesystem::path> ChooseInstallerDestination(HWND owner, const std::filesystem::path& initial) {
  ComPtr<IFileOpenDialog> dialog;
  HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
  if (FAILED(result)) {
    throw std::runtime_error("HuxerUI Windows installer could not create the destination picker: " +
                             ErrorMessage(result));
  }
  FILEOPENDIALOGOPTIONS options = 0;
  result = dialog->GetOptions(&options);
  if (SUCCEEDED(result)) {
    result = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  }
  if (FAILED(result)) {
    throw std::runtime_error("HuxerUI Windows installer could not configure the destination picker: " +
                             ErrorMessage(result));
  }
  if (!initial.empty()) {
    ComPtr<IShellItem> initial_folder;
    if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&initial_folder)))) {
      static_cast<void>(dialog->SetFolder(initial_folder.Get()));
    }
  }
  result = dialog->Show(owner);
  if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    return std::nullopt;
  }
  if (FAILED(result)) {
    throw std::runtime_error("HuxerUI Windows installer could not show the destination picker: " +
                             ErrorMessage(result));
  }
  ComPtr<IShellItem> selected_folder;
  result = dialog->GetResult(&selected_folder);
  PWSTR selected_path = nullptr;
  if (SUCCEEDED(result)) {
    result = selected_folder->GetDisplayName(SIGDN_FILESYSPATH, &selected_path);
  }
  std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> released_path(selected_path, &CoTaskMemFree);
  if (FAILED(result) || selected_path == nullptr || *selected_path == L'\0') {
    throw std::runtime_error("HuxerUI Windows installer could not read the selected destination: " +
                             ErrorMessage(FAILED(result) ? result : E_UNEXPECTED));
  }
  return std::filesystem::path(selected_path);
}

std::optional<windows::InstallerPromptChoice> PromptChoice(int value) {
  switch (value) {
  case IDOK:
    return windows::InstallerPromptChoice::Ok;
  case IDCANCEL:
    return windows::InstallerPromptChoice::Cancel;
  case IDABORT:
    return windows::InstallerPromptChoice::Abort;
  case IDRETRY:
    return windows::InstallerPromptChoice::Retry;
  case IDTRYAGAIN:
    return windows::InstallerPromptChoice::TryAgain;
  case IDIGNORE:
    return windows::InstallerPromptChoice::Ignore;
  case IDYES:
    return windows::InstallerPromptChoice::Yes;
  case IDNO:
    return windows::InstallerPromptChoice::No;
  case IDCONTINUE:
    return windows::InstallerPromptChoice::Continue;
  default:
    return std::nullopt;
  }
}

int NativePromptChoice(windows::InstallerPromptChoice choice) {
  switch (choice) {
  case windows::InstallerPromptChoice::Ok:
    return IDOK;
  case windows::InstallerPromptChoice::Cancel:
    return IDCANCEL;
  case windows::InstallerPromptChoice::Abort:
    return IDABORT;
  case windows::InstallerPromptChoice::Retry:
    return IDRETRY;
  case windows::InstallerPromptChoice::TryAgain:
    return IDTRYAGAIN;
  case windows::InstallerPromptChoice::Ignore:
    return IDIGNORE;
  case windows::InstallerPromptChoice::Yes:
    return IDYES;
  case windows::InstallerPromptChoice::No:
    return IDNO;
  case windows::InstallerPromptChoice::Continue:
    return IDCONTINUE;
  }
  return IDCANCEL;
}

std::vector<windows::InstallerPromptChoice> PromptChoices(DWORD hint) {
  switch (hint & MB_TYPEMASK) {
  case MB_OK:
    return {windows::InstallerPromptChoice::Ok};
  case MB_OKCANCEL:
    return {windows::InstallerPromptChoice::Ok, windows::InstallerPromptChoice::Cancel};
  case MB_ABORTRETRYIGNORE:
    return {windows::InstallerPromptChoice::Abort, windows::InstallerPromptChoice::Retry,
            windows::InstallerPromptChoice::Ignore};
  case MB_YESNOCANCEL:
    return {windows::InstallerPromptChoice::Yes, windows::InstallerPromptChoice::No,
            windows::InstallerPromptChoice::Cancel};
  case MB_YESNO:
    return {windows::InstallerPromptChoice::Yes, windows::InstallerPromptChoice::No};
  case MB_RETRYCANCEL:
    return {windows::InstallerPromptChoice::Retry, windows::InstallerPromptChoice::Cancel};
  case MB_CANCELTRYCONTINUE:
    return {windows::InstallerPromptChoice::Cancel, windows::InstallerPromptChoice::TryAgain,
            windows::InstallerPromptChoice::Continue};
  default:
    return {windows::InstallerPromptChoice::Cancel};
  }
}

class HuxerUIBootstrapperApplication;

std::mutex& CurrentInstallerMutex() {
  static std::mutex mutex;
  return mutex;
}

std::weak_ptr<WindowsInstallerSession>& CurrentInstaller() {
  static std::weak_ptr<WindowsInstallerSession> installer;
  return installer;
}

} // namespace

class WindowsInstallerSession final : public std::enable_shared_from_this<WindowsInstallerSession> {
public:
  WindowsInstallerSession()
      : status_cell_(std::make_shared<StateCell<windows::InstallerStatus>>(windows::InstallerStatus{})) {}

  [[nodiscard]] windows::InstallerStatus Status() const {
    ObserveState(status_cell_);
    return status_cell_->value;
  }

  void Bind(HuxerUIBootstrapperApplication& application, UIThreadDispatcher dispatcher, HWND window) {
    if (!dispatcher || window == nullptr) {
      throw std::logic_error("HuxerUI Windows installer requires an attached UI dispatcher and window");
    }
    {
      std::lock_guard lock(mutex_);
      application_ = &application;
      dispatcher_ = std::move(dispatcher);
      window_ = window;
    }
    SchedulePublish();
  }

  void Install(windows::InstallerInstallOptions options);
  [[nodiscard]] std::optional<std::filesystem::path> ChooseDestination(const std::filesystem::path& initial) const;
  void Repair();
  void Uninstall();
  void Cancel();

  void SetDefaults(std::filesystem::path destination, bool create_desktop_shortcut) {
    Mutate([destination = std::move(destination), create_desktop_shortcut](windows::InstallerStatus& status) mutable {
      status.default_destination = std::move(destination);
      status.default_create_desktop_shortcut = create_desktop_shortcut;
    });
  }

  void Respond(std::uint64_t prompt_id, windows::InstallerPromptChoice choice) {
    std::lock_guard lock(mutex_);
    if (!status_.prompt || status_.prompt->id != prompt_id ||
        std::find(status_.prompt->choices.begin(), status_.prompt->choices.end(), choice) ==
            status_.prompt->choices.end()) {
      return;
    }
    prompt_response_ = choice;
    prompt_condition_.notify_all();
  }

  void Detecting() {
    Mutate([](windows::InstallerStatus& status) {
      status.phase = windows::InstallerPhase::Detecting;
      status.product = windows::InstallerProductState::Unknown;
      status.failure.reset();
    });
  }

  void PackageDetected(HRESULT result, BOOTSTRAPPER_PACKAGE_STATE package_state) {
    if (FAILED(result)) {
      Fail(result);
      return;
    }
    Mutate([package_state](windows::InstallerStatus& status) {
      if (status.product == windows::InstallerProductState::NewerVersion) {
        return;
      }
      if (package_state == BOOTSTRAPPER_PACKAGE_STATE_PRESENT) {
        status.product = windows::InstallerProductState::Present;
      } else if (package_state == BOOTSTRAPPER_PACKAGE_STATE_ABSENT) {
        status.product = windows::InstallerProductState::Absent;
      } else if (
          package_state == BOOTSTRAPPER_PACKAGE_STATE_OBSOLETE || package_state == BOOTSTRAPPER_PACKAGE_STATE_SUPERSEDED
      ) {
        status.product = windows::InstallerProductState::NewerVersion;
      }
    });
  }

  void DetectionComplete(HRESULT result) {
    if (FAILED(result)) {
      Fail(result);
      return;
    }
    Mutate([](windows::InstallerStatus& status) {
      status.phase = windows::InstallerPhase::Ready;
      status.progress = 0.0F;
    });
  }

  void NewerVersionDetected() {
    Mutate([](windows::InstallerStatus& status) { status.product = windows::InstallerProductState::NewerVersion; });
  }

  void ApplyStarted() {
    Mutate([this](windows::InstallerStatus& status) {
      status.phase = cancel_requested_ ? windows::InstallerPhase::Canceling : windows::InstallerPhase::Applying;
      status.progress = 0.0F;
      status.failure.reset();
    });
  }

  void Progress(DWORD percentage) {
    Mutate([percentage](windows::InstallerStatus& status) {
      status.progress = std::clamp(static_cast<float>(percentage) / 100.0F, 0.0F, 1.0F);
    });
  }

  void PackageStarted(const wchar_t* package) {
    Mutate([name = WideToUtf8(package)](windows::InstallerStatus& status) { status.current_package = name; });
  }

  void ApplyComplete(HRESULT result, BOOTSTRAPPER_APPLY_RESTART restart) {
    bool should_quit = false;
    {
      std::lock_guard lock(mutex_);
      status_.prompt.reset();
      status_.current_package.clear();
      status_.restart = restart == BOOTSTRAPPER_APPLY_RESTART_NONE ? windows::InstallerRestart::None
                                                                   : windows::InstallerRestart::Required;
      if (result == HRESULT_FROM_WIN32(ERROR_INSTALL_USEREXIT)) {
        status_.phase = windows::InstallerPhase::Canceled;
        status_.failure.reset();
      } else if (FAILED(result)) {
        SetFailureLocked(result, ErrorMessage(result));
      } else {
        status_.phase = windows::InstallerPhase::Completed;
        status_.progress = 1.0F;
        status_.failure.reset();
      }
      prompt_response_ = windows::InstallerPromptChoice::Cancel;
      prompt_condition_.notify_all();
      should_quit = ui_closed_;
    }
    SchedulePublish();
    if (should_quit) {
      Quit(InstallerExitCode(result));
    }
  }

  int Ask(windows::InstallerPromptKind kind, std::string message,
          std::vector<windows::InstallerPromptChoice> choices,
          std::optional<windows::InstallerPromptChoice> recommended) {
    std::unique_lock lock(mutex_);
    const std::uint64_t id = next_prompt_id_++;
    status_.prompt = windows::InstallerPrompt{id, kind, std::move(message), std::move(choices), recommended};
    prompt_response_.reset();
    SchedulePublishLocked();
    prompt_condition_.wait(lock, [this] { return prompt_response_.has_value() || cancel_requested_ || ui_closed_; });
    const windows::InstallerPromptChoice response = prompt_response_.value_or(windows::InstallerPromptChoice::Cancel);
    prompt_response_.reset();
    status_.prompt.reset();
    SchedulePublishLocked();
    return NativePromptChoice(response);
  }

  void PlanComplete(HRESULT result);

  void UiExited(DWORD exit_code) {
    bool active = false;
    DWORD engine_exit_code = exit_code;
    {
      std::lock_guard lock(mutex_);
      ui_closed_ = true;
      active = status_.phase == windows::InstallerPhase::Planning ||
               status_.phase == windows::InstallerPhase::Applying ||
               status_.phase == windows::InstallerPhase::Canceling;
      if (active) {
        cancel_requested_ = true;
        status_.phase = windows::InstallerPhase::Canceling;
      } else if (status_.phase == windows::InstallerPhase::Failed && status_.failure) {
        engine_exit_code = InstallerExitCode(static_cast<HRESULT>(status_.failure->code));
      } else if (status_.phase == windows::InstallerPhase::Canceled) {
        engine_exit_code = ERROR_INSTALL_USEREXIT;
      }
      prompt_response_ = windows::InstallerPromptChoice::Cancel;
      prompt_condition_.notify_all();
    }
    if (active) {
      Cancel();
    } else {
      Quit(engine_exit_code);
    }
  }

  void Fail(HRESULT result, std::string message = {}) {
    bool should_quit = false;
    {
      std::lock_guard lock(mutex_);
      if (message.empty()) {
        message = ErrorMessage(result);
      }
      SetFailureLocked(result, std::move(message));
      should_quit = ui_closed_;
    }
    SchedulePublish();
    if (should_quit) {
      Quit(InstallerExitCode(result));
    }
  }

private:
  void Start(windows::InstallerAction action, windows::InstallerInstallOptions options);
  void Quit(DWORD exit_code);

  template <class Function> void Mutate(Function&& mutation) {
    {
      std::lock_guard lock(mutex_);
      std::forward<Function>(mutation)(status_);
    }
    SchedulePublish();
  }

  void SetFailureLocked(HRESULT result, std::string message) {
    status_.phase = windows::InstallerPhase::Failed;
    status_.failure = windows::InstallerFailure{static_cast<std::uint32_t>(result), std::move(message)};
  }

  void SchedulePublish() {
    std::lock_guard lock(mutex_);
    SchedulePublishLocked();
  }

  void SchedulePublishLocked() {
    if (publish_scheduled_ || !dispatcher_) {
      return;
    }
    publish_scheduled_ = true;
    const std::weak_ptr<WindowsInstallerSession> weak = weak_from_this();
    dispatcher_([weak] {
      if (const std::shared_ptr<WindowsInstallerSession> session = weak.lock()) {
        session->PublishOnUIThread();
      }
    });
  }

  void PublishOnUIThread() {
    windows::InstallerStatus status;
    {
      std::lock_guard lock(mutex_);
      publish_scheduled_ = false;
      status = status_;
    }
    status_cell_->value = std::move(status);
    ++status_cell_->version;
    NotifyState(status_cell_);
  }

  mutable std::mutex mutex_;
  std::condition_variable prompt_condition_;
  windows::InstallerStatus status_;
  std::shared_ptr<StateCell<windows::InstallerStatus>> status_cell_;
  UIThreadDispatcher dispatcher_;
  HuxerUIBootstrapperApplication* application_ = nullptr;
  HWND window_ = nullptr;
  std::optional<windows::InstallerPromptChoice> prompt_response_;
  std::uint64_t next_prompt_id_ = 1;
  bool publish_scheduled_ = false;
  bool cancel_requested_ = false;
  bool ui_closed_ = false;
};

namespace {

class HuxerUIBootstrapperApplication final : public CBootstrapperApplicationBase {
public:
  explicit HuxerUIBootstrapperApplication(std::shared_ptr<WindowsInstallerSession> session)
      : session_(std::move(session)) {}

  STDMETHODIMP OnStartup() override {
    ui_thread_ = CreateThread(nullptr, 0, RunUI, this, 0, nullptr);
    return ui_thread_ == nullptr ? HRESULT_FROM_WIN32(GetLastError()) : S_OK;
  }

  STDMETHODIMP OnShutdown(BOOTSTRAPPER_SHUTDOWN_ACTION* action) override {
    if (ui_thread_ != nullptr) {
      WaitForSingleObject(ui_thread_, INFINITE);
      CloseHandle(ui_thread_);
      ui_thread_ = nullptr;
    }
    return CBootstrapperApplicationBase::OnShutdown(action);
  }

  STDMETHODIMP OnDetectBegin(BOOL cached, BOOTSTRAPPER_REGISTRATION_TYPE registration_type, DWORD packages,
                             BOOL* cancel) override {
    session_->Detecting();
    return CBootstrapperApplicationBase::OnDetectBegin(cached, registration_type, packages, cancel);
  }

  STDMETHODIMP OnDetectPackageComplete(const wchar_t* package, HRESULT result, BOOTSTRAPPER_PACKAGE_STATE state,
                                        BOOL cached) override {
    session_->PackageDetected(result, state);
    return CBootstrapperApplicationBase::OnDetectPackageComplete(package, result, state, cached);
  }

  STDMETHODIMP OnDetectComplete(HRESULT result, BOOL eligible_for_cleanup) override {
    session_->DetectionComplete(result);
    return CBootstrapperApplicationBase::OnDetectComplete(result, eligible_for_cleanup);
  }

  STDMETHODIMP OnDetectForwardCompatibleBundle(const wchar_t* bundle, BOOTSTRAPPER_RELATION_TYPE relation,
                                                const wchar_t* tag, BOOL per_machine, const wchar_t* version,
                                                BOOL missing_from_cache, BOOL* cancel) override {
    session_->NewerVersionDetected();
    return CBootstrapperApplicationBase::OnDetectForwardCompatibleBundle(
        bundle, relation, tag, per_machine, version, missing_from_cache, cancel);
  }

  STDMETHODIMP OnPlanComplete(HRESULT result) override {
    session_->PlanComplete(result);
    return CBootstrapperApplicationBase::OnPlanComplete(result);
  }

  STDMETHODIMP OnApplyBegin(DWORD phases, BOOL* cancel) override {
    session_->ApplyStarted();
    return CBootstrapperApplicationBase::OnApplyBegin(phases, cancel);
  }

  STDMETHODIMP OnProgress(DWORD progress, DWORD overall_progress, BOOL* cancel) override {
    session_->Progress(overall_progress);
    return CBootstrapperApplicationBase::OnProgress(progress, overall_progress, cancel);
  }

  STDMETHODIMP OnExecutePackageBegin(const wchar_t* package, BOOL execute, BOOTSTRAPPER_ACTION_STATE action,
                                      INSTALLUILEVEL ui_level, BOOL disable_external_ui, BOOL* cancel) override {
    session_->PackageStarted(package);
    return CBootstrapperApplicationBase::OnExecutePackageBegin(
        package, execute, action, ui_level, disable_external_ui, cancel);
  }

  STDMETHODIMP OnError(BOOTSTRAPPER_ERROR_TYPE type, const wchar_t* package, DWORD code,
                       const wchar_t* message, DWORD ui_hint, DWORD data_count, const wchar_t** data,
                       int recommendation, int* result) override {
    int engine_result = recommendation;
    const HRESULT engine_status = CBootstrapperApplicationBase::OnError(
        type, package, code, message, ui_hint, data_count, data, recommendation, &engine_result);
    if (FAILED(engine_status)) {
      return engine_status;
    }
    const std::vector<windows::InstallerPromptChoice> choices = PromptChoices(ui_hint);
    std::optional<windows::InstallerPromptChoice> recommended = PromptChoice(recommendation);
    if (recommended && std::find(choices.begin(), choices.end(), *recommended) == choices.end()) {
      recommended.reset();
    }
    *result = session_->Ask(windows::InstallerPromptKind::Error, WideToUtf8(message), choices, recommended);
    return S_OK;
  }

  STDMETHODIMP OnExecuteFilesInUse(const wchar_t* package, DWORD file_count, const wchar_t** files,
                                   int recommendation, BOOTSTRAPPER_FILES_IN_USE_TYPE source,
                                   int* result) override {
    const HRESULT engine_status =
        CBootstrapperApplicationBase::OnExecuteFilesInUse(package, file_count, files, recommendation, source, result);
    if (FAILED(engine_status)) {
      return engine_status;
    }
    const std::vector<windows::InstallerPromptChoice> choices{
        windows::InstallerPromptChoice::Retry,
        windows::InstallerPromptChoice::Ignore,
        windows::InstallerPromptChoice::Cancel,
    };
    std::optional<windows::InstallerPromptChoice> recommended = PromptChoice(recommendation);
    if (recommended && std::find(choices.begin(), choices.end(), *recommended) == choices.end()) {
      recommended.reset();
    }
    *result = session_->Ask(windows::InstallerPromptKind::FilesInUse, {}, choices, recommended);
    return S_OK;
  }

  STDMETHODIMP OnApplyComplete(HRESULT result, BOOTSTRAPPER_APPLY_RESTART restart,
                               BOOTSTRAPPER_APPLYCOMPLETE_ACTION recommendation,
                               BOOTSTRAPPER_APPLYCOMPLETE_ACTION* action) override {
    session_->ApplyComplete(result, restart);
    return CBootstrapperApplicationBase::OnApplyComplete(result, restart, recommendation, action);
  }

  HRESULT Detect(HWND window) {
    return m_pEngine->Detect(window);
  }

  HRESULT Plan(BOOTSTRAPPER_ACTION action) {
    return m_pEngine->Plan(action);
  }

  HRESULT Apply(HWND window) {
    return m_pEngine->Apply(window);
  }

  std::filesystem::path DefaultDestination() {
    LPWSTR formatted_path = nullptr;
    const HRESULT result = BalFormatStringFromEngine(m_pEngine, L"[InstallFolder]", &formatted_path);
    std::unique_ptr<wchar_t, decltype(&StrFree)> released_path(formatted_path, &StrFree);
    if (FAILED(result) || formatted_path == nullptr || *formatted_path == L'\0') {
      throw std::runtime_error("HuxerUI Windows installer could not resolve the default destination: " +
                               ErrorMessage(FAILED(result) ? result : E_UNEXPECTED));
    }
    std::filesystem::path destination(formatted_path);
    if (!destination.is_absolute()) {
      throw std::runtime_error("HuxerUI Windows installer default destination must resolve to an absolute path");
    }
    return destination;
  }

  bool DefaultCreateDesktopShortcut() {
    LONGLONG value = 0;
    const HRESULT result = m_pEngine->GetVariableNumeric(L"CreateDesktopShortcut", &value);
    if (FAILED(result)) {
      throw std::runtime_error("HuxerUI Windows installer could not resolve the desktop shortcut default: " +
                               ErrorMessage(result));
    }
    return value != 0;
  }

  HRESULT SetInstallFolder(const std::filesystem::path& destination) {
    return m_pEngine->SetVariableString(L"InstallFolder", destination.c_str(), FALSE);
  }

  HRESULT SetCreateDesktopShortcut(bool enabled) {
    return m_pEngine->SetVariableNumeric(L"CreateDesktopShortcut", enabled ? 1 : 0);
  }

  void Cancel() {
    PromptCancel(nullptr, TRUE, nullptr, nullptr);
  }

  void Quit(DWORD exit_code) {
    m_pEngine->Quit(exit_code);
  }

private:
  static DWORD WINAPI RunUI(void* context) {
    auto& application = *static_cast<HuxerUIBootstrapperApplication*>(context);
    DWORD exit_code = 1;
    try {
      exit_code = static_cast<DWORD>(RunWin32PlatformApplication(
          CurrentApplication(), [&application](UIThreadDispatcher dispatcher, HWND window) {
            application.session_->Bind(application, std::move(dispatcher), window);
            std::filesystem::path default_destination = application.DefaultDestination();
            const bool default_create_desktop_shortcut = application.DefaultCreateDesktopShortcut();
            application.session_->SetDefaults(std::move(default_destination), default_create_desktop_shortcut);
            const HRESULT result = application.Detect(window);
            if (FAILED(result)) {
              application.session_->Fail(result);
            }
          }));
    } catch (const std::exception& error) {
      application.session_->Fail(E_FAIL, error.what());
    } catch (...) {
      application.session_->Fail(E_FAIL, "HuxerUI Windows installer interface failed");
    }
    application.session_->UiExited(exit_code);
    return exit_code;
  }

  std::shared_ptr<WindowsInstallerSession> session_;
  HANDLE ui_thread_ = nullptr;
};

} // namespace

std::optional<std::filesystem::path>
WindowsInstallerSession::ChooseDestination(const std::filesystem::path& initial) const {
  HWND window = nullptr;
  {
    std::lock_guard lock(mutex_);
    if (status_.phase != windows::InstallerPhase::Ready) {
      throw std::logic_error("HuxerUI Windows installer destination picker requires a ready session");
    }
    window = window_;
  }
  if (window == nullptr) {
    throw std::logic_error("HuxerUI Windows installer destination picker requires an attached window");
  }
  return ChooseInstallerDestination(window, initial);
}

Task<std::optional<std::filesystem::path>>
ChooseInstallerDestinationTask(std::shared_ptr<WindowsInstallerSession> session, std::filesystem::path initial) {
  co_return session->ChooseDestination(initial);
}

void WindowsInstallerSession::Install(windows::InstallerInstallOptions options) {
  Start(windows::InstallerAction::Install, std::move(options));
}

void WindowsInstallerSession::Repair() {
  Start(windows::InstallerAction::Repair, {});
}

void WindowsInstallerSession::Uninstall() {
  Start(windows::InstallerAction::Uninstall, {});
}

void WindowsInstallerSession::Start(windows::InstallerAction action, windows::InstallerInstallOptions options) {
  HuxerUIBootstrapperApplication* application = nullptr;
  HWND window = nullptr;
  {
    std::lock_guard lock(mutex_);
    if (status_.phase != windows::InstallerPhase::Ready) {
      throw std::logic_error("HuxerUI Windows installer action requires a ready session");
    }
    if (action == windows::InstallerAction::Repair && status_.product != windows::InstallerProductState::Present) {
      throw std::logic_error("HuxerUI Windows installer cannot repair an absent product");
    }
    if (action == windows::InstallerAction::Uninstall && status_.product != windows::InstallerProductState::Present) {
      throw std::logic_error("HuxerUI Windows installer cannot uninstall an absent product");
    }
    application = application_;
    window = window_;
    status_.phase = windows::InstallerPhase::Planning;
    status_.action = action;
    status_.progress = 0.0F;
    status_.failure.reset();
    cancel_requested_ = false;
  }
  SchedulePublish();
  if (application == nullptr || window == nullptr) {
    Fail(E_UNEXPECTED, "HuxerUI Windows installer engine is not attached");
    return;
  }
  if (options.destination && action == windows::InstallerAction::Install) {
    const HRESULT result = application->SetInstallFolder(*options.destination);
    if (FAILED(result)) {
      Fail(result);
      return;
    }
  }
  if (options.create_desktop_shortcut && action == windows::InstallerAction::Install) {
    const HRESULT result = application->SetCreateDesktopShortcut(*options.create_desktop_shortcut);
    if (FAILED(result)) {
      Fail(result);
      return;
    }
  }
  BOOTSTRAPPER_ACTION native_action = BOOTSTRAPPER_ACTION_INSTALL;
  if (action == windows::InstallerAction::Repair) {
    native_action = BOOTSTRAPPER_ACTION_REPAIR;
  } else if (action == windows::InstallerAction::Uninstall) {
    native_action = BOOTSTRAPPER_ACTION_UNINSTALL;
  }
  const HRESULT result = application->Plan(native_action);
  if (FAILED(result)) {
    Fail(result);
  }
}

void WindowsInstallerSession::PlanComplete(HRESULT result) {
  HuxerUIBootstrapperApplication* application = nullptr;
  HWND window = nullptr;
  bool canceled = false;
  bool ui_closed = false;
  {
    std::lock_guard lock(mutex_);
    application = application_;
    window = window_;
    canceled = result == HRESULT_FROM_WIN32(ERROR_INSTALL_USEREXIT) || (cancel_requested_ && SUCCEEDED(result));
    ui_closed = ui_closed_;
    if (canceled) {
      status_.phase = windows::InstallerPhase::Canceled;
    }
  }
  if (canceled) {
    SchedulePublish();
    if (ui_closed) {
      Quit(ERROR_INSTALL_USEREXIT);
    }
    return;
  }
  if (FAILED(result)) {
    Fail(result);
    return;
  }
  if (application == nullptr || window == nullptr) {
    Fail(E_UNEXPECTED, "HuxerUI Windows installer engine is not attached");
    return;
  }
  const HRESULT apply_result = application->Apply(window);
  if (FAILED(apply_result)) {
    Fail(apply_result);
  }
}

void WindowsInstallerSession::Cancel() {
  HuxerUIBootstrapperApplication* application = nullptr;
  {
    std::lock_guard lock(mutex_);
    if (status_.phase != windows::InstallerPhase::Planning && status_.phase != windows::InstallerPhase::Applying &&
        status_.phase != windows::InstallerPhase::Canceling) {
      return;
    }
    cancel_requested_ = true;
    status_.phase = windows::InstallerPhase::Canceling;
    prompt_response_ = windows::InstallerPromptChoice::Cancel;
    prompt_condition_.notify_all();
    application = application_;
  }
  SchedulePublish();
  if (application != nullptr) {
    application->Cancel();
  }
}

void WindowsInstallerSession::Quit(DWORD exit_code) {
  HuxerUIBootstrapperApplication* application = nullptr;
  {
    std::lock_guard lock(mutex_);
    application = application_;
  }
  if (application != nullptr) {
    application->Quit(exit_code);
  }
}

} // namespace huxerui::detail

namespace huxerui::windows {

InstallerStatus InstallerHandle::Status() const {
  return session_->Status();
}

Task<std::optional<std::filesystem::path>>
InstallerHandle::ChooseDestinationAsync(const std::filesystem::path& initial) const {
  if (!initial.empty() && !initial.is_absolute()) {
    throw std::invalid_argument("HuxerUI Windows installer destination picker requires an absolute initial path");
  }
  return detail::ChooseInstallerDestinationTask(session_, initial);
}

void InstallerHandle::Install(InstallerInstallOptions options) const {
  if (options.destination && (options.destination->empty() || !options.destination->is_absolute())) {
    throw std::invalid_argument("HuxerUI Windows installer destination must be an absolute path");
  }
  session_->Install(std::move(options));
}

void InstallerHandle::Repair() const {
  session_->Repair();
}

void InstallerHandle::Uninstall() const {
  session_->Uninstall();
}

void InstallerHandle::Cancel() const {
  session_->Cancel();
}

void InstallerHandle::Respond(std::uint64_t prompt_id, InstallerPromptChoice choice) const {
  session_->Respond(prompt_id, choice);
}

InstallerHandle UseInstaller() {
  return InstallerHandle{UseService<detail::WindowsInstallerSession>()};
}

void InstallInstallerSession(RootContext& root) {
  std::shared_ptr<detail::WindowsInstallerSession> session;
  {
    std::lock_guard lock(detail::CurrentInstallerMutex());
    session = detail::CurrentInstaller().lock();
  }
  if (!session) {
    throw std::logic_error("HuxerUI Windows installer session is not running");
  }
  root.Provide(std::move(session));
}

int RunInstallerApplication() {
  const std::shared_ptr<detail::WindowsInstallerSession> session = std::make_shared<detail::WindowsInstallerSession>();
  {
    std::lock_guard lock(detail::CurrentInstallerMutex());
    if (!detail::CurrentInstaller().expired()) {
      throw std::logic_error("HuxerUI Windows installer session is already running");
    }
    detail::CurrentInstaller() = session;
  }

  auto* application = new detail::HuxerUIBootstrapperApplication(session);
  IBootstrapperApplication* bootstrapper = nullptr;
  HRESULT result = application->QueryInterface(IID_PPV_ARGS(&bootstrapper));
  application->Release();
  if (SUCCEEDED(result)) {
    result = BootstrapperApplicationRun(bootstrapper);
  }
  if (bootstrapper != nullptr) {
    bootstrapper->Release();
  }
  {
    std::lock_guard lock(detail::CurrentInstallerMutex());
    detail::CurrentInstaller().reset();
  }
  return FAILED(result) ? 1 : 0;
}

} // namespace huxerui::windows
