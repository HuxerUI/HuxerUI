#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/root.h>
#include <huxerui/task.h>

namespace huxerui::detail {
class WindowsInstallerSession;
}

namespace huxerui::windows {

/// Identifies the current stage of the Windows installation session.
enum class InstallerPhase {
  Detecting,
  Ready,
  Planning,
  Applying,
  Canceling,
  Canceled,
  Completed,
  Failed,
};

/// Describes the product state detected by the Windows installation engine.
enum class InstallerProductState {
  Unknown,
  Absent,
  Present,
  NewerVersion,
};

/// Identifies the operation currently planned or applied by the installation engine.
enum class InstallerAction {
  Install,
  Repair,
  Uninstall,
};

/// Describes whether completing the installation requires a system restart.
enum class InstallerRestart {
  None,
  Required,
};

/// Identifies a question that must be answered before the installation engine can continue.
enum class InstallerPromptKind {
  Error,
  FilesInUse,
};

/// Identifies one answer accepted by an installer prompt.
enum class InstallerPromptChoice {
  Ok,
  Cancel,
  Abort,
  Retry,
  TryAgain,
  Ignore,
  Yes,
  No,
  Continue,
};

/// Describes a terminal installation failure without exposing WiX or HRESULT types.
struct InstallerFailure {
  /// Stable numeric error code reported by the Windows installation engine.
  std::uint32_t code = 0;
  /// Human-readable error message suitable for the installer interface.
  std::string message;
};

/// Describes the one outstanding question in an installation session.
struct InstallerPrompt {
  /// Monotonically increasing identity used to reject stale answers.
  std::uint64_t id = 0;
  /// Prompt category.
  InstallerPromptKind kind = InstallerPromptKind::Error;
  /// Engine-provided question or failure detail, or an empty value when `kind` fully identifies the prompt.
  std::string message;
  /// Answers accepted by this prompt.
  std::vector<InstallerPromptChoice> choices;
  /// Engine-recommended answer when it is represented by `choices`.
  std::optional<InstallerPromptChoice> recommended;
};

/// Overrides authored choices for one installation request.
///
/// Leave a field unset to retain the matching value authored by the bundle. Custom installer interfaces can start
/// from `InstallerStatus` defaults and replace only values controlled by the user.
/// @code
/// installer.Install({
///     .destination = selected_destination,
///     .create_desktop_shortcut = create_shortcut,
/// });
/// @endcode
struct InstallerInstallOptions {
  /// Absolute destination directory, or no override for the authored destination.
  std::optional<std::filesystem::path> destination;
  /// Whether to install the optional desktop shortcut, or no override for the authored choice.
  std::optional<bool> create_desktop_shortcut;
};

/// Contains the coalesced current state of one Windows installation session.
///
/// Read this value during composition to subscribe the current scope to later changes. Progress is normalized to the
/// inclusive range from `0.0F` to `1.0F`.
struct InstallerStatus {
  /// Current engine phase.
  InstallerPhase phase = InstallerPhase::Detecting;
  /// Currently detected product state.
  InstallerProductState product = InstallerProductState::Unknown;
  /// Authored destination after expanding Burn variables for display and editing.
  std::filesystem::path default_destination;
  /// Authored initial choice for the optional desktop shortcut.
  bool default_create_desktop_shortcut = false;
  /// Operation currently being planned or applied.
  std::optional<InstallerAction> action;
  /// Overall normalized progress.
  float progress = 0.0F;
  /// Engine package currently being processed, when known.
  std::string current_package;
  /// Outstanding synchronous engine question, when one exists.
  std::optional<InstallerPrompt> prompt;
  /// Terminal failure detail when `phase` is `InstallerPhase::Failed`.
  std::optional<InstallerFailure> failure;
  /// Restart requirement reported by the completed apply operation.
  InstallerRestart restart = InstallerRestart::None;
};

/// Controls the root-owned Windows installation session used by a custom HuxerUI bootstrapper interface.
///
/// The handle is available only inside an application launched by `RunInstallerApplication()`. The destination is
/// optional because applications that do not expose destination selection should let MSI resolve its authored
/// default.
/// @code
/// const InstallerHandle installer = UseInstaller();
/// const InstallerStatus status = installer.Status();
/// if (status.phase == InstallerPhase::Ready) {
///   return Button("Install").OnClick([installer] { installer.Install(); });
/// }
/// return ProgressBar(status.progress);
/// @endcode
class InstallerHandle final {
public:
  /// Returns the coalesced current status and observes it for composition invalidation.
  [[nodiscard]] InstallerStatus Status() const;
  /// Opens the Windows folder picker asynchronously with an optional initial destination.
  ///
  /// Launch the returned Task from a composition-owned TaskScope. The TaskScope queues its first resume after the
  /// current UI callback, so the native modal dialog cannot reenter the pointer event that requested it.
  /// @code
  /// Button("Browse").OnClick([installer, tasks, destination] {
  ///   tasks.Launch([installer, destination]() -> Task<void> {
  ///     if (const auto selected = co_await installer.ChooseDestinationAsync(destination)) {
  ///       destination = *selected;
  ///     }
  ///   });
  /// });
  /// @endcode
  /// @return A Task that produces the selected absolute directory, or `std::nullopt` when the user dismisses the
  /// picker.
  /// @throws std::invalid_argument if `initial` is nonempty and not absolute.
  /// @throws std::logic_error if the installer is not ready or has no attached window.
  /// @throws std::runtime_error if the Windows folder picker fails.
  [[nodiscard]] Task<std::optional<std::filesystem::path>>
  ChooseDestinationAsync(const std::filesystem::path& initial = {}) const;
  /// Plans and applies installation with optional overrides for the authored choices.
  /// @throws std::invalid_argument if `options.destination` is nonempty and not absolute.
  void Install(InstallerInstallOptions options = {}) const;
  /// Plans and applies repair of the detected product.
  void Repair() const;
  /// Plans and applies removal of the detected product.
  void Uninstall() const;
  /// Requests cooperative cancellation and rollback of an active operation.
  void Cancel() const;
  /// Answers the currently identified engine prompt; stale prompt identities are ignored.
  void Respond(std::uint64_t prompt_id, InstallerPromptChoice choice) const;

private:
  explicit InstallerHandle(std::shared_ptr<detail::WindowsInstallerSession> session) : session_(std::move(session)) {}

  std::shared_ptr<detail::WindowsInstallerSession> session_;

  friend InstallerHandle UseInstaller();
};

/// Returns the installer handle installed for the current composition.
///
/// A reusable function that calls this composition-bound facility should be marked with the composable attribute.
InstallerHandle UseInstaller();

/// Installs the current process installer session into a Runtime root.
///
/// Add this hook exactly once to the `Application` used by `RunInstallerApplication()`:
/// @code
/// const Application application{
///     InstallerApp,
///     {.root_hooks = {windows::InstallInstallerSession}},
/// };
/// @endcode
void InstallInstallerSession(RootContext& root);

/// Connects the process application to the WiX Burn engine and runs its HuxerUI interface.
///
/// Call this operation from the generated Windows bootstrapper application's `wWinMain` entry point. It is not
/// available to an ordinary HuxerUI application executable.
int RunInstallerApplication();

} // namespace huxerui::windows
