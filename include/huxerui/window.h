#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/color.h>
#include <huxerui/geometry.h>
#include <huxerui/lifecycle.h>
#include <huxerui/resource.h>
#include <huxerui/view.h>

namespace huxerui {

namespace detail {
struct ModifierDescriptor;
class WindowService;
} // namespace detail

/// Controls how the application root consumes the current window safe area.
enum class WindowContentMode {
  SafeArea,  ///< Insets ordinary application content from platform obstructions automatically.
  EdgeToEdge, ///< Lays out the application across the complete viewport and leaves inset consumption to its Views.
};

/// Selects platform-owned or application-defined top-level window chrome.
enum class WindowChromeMode {
  System, ///< Retains the platform's ordinary title bar and decorations.
  Custom, ///< Lets application content occupy title-bar space while HuxerUI preserves native window behavior.
};

/// A top-level window operation understood by PlatformAdapter and WindowHandle.
enum class WindowCommand {
  Minimize,      ///< Requests the platform's minimized state.
  Maximize,      ///< Requests the platform's maximized or equivalent zoomed state.
  Restore,       ///< Restores the window from its minimized or maximized state.
  ToggleMaximize, ///< Toggles between the maximized and restored states.
  Close,         ///< Requests the platform's normal close path.
  Show,          ///< Makes the window visible without otherwise changing placement.
  Hide,          ///< Hides the window without closing the application.
  Activate,      ///< Shows and brings the window to the foreground when the platform permits it.
};

/// Selects the foreground brightness of native system-bar text and icons.
enum class SystemBarContentBrightness {
  Automatic, ///< Derives light or dark foreground content from the declared background color.
  Light,     ///< Requests light foreground content.
  Dark,      ///< Requests dark foreground content.
};

/// Platform-resolved title-bar geometry in logical units for one committed frame.
///
/// The value is available only when application content occupies native title-bar space. Insets already account for
/// control placement and layout direction, so application layout treats them as physical left and right reservations.
struct WindowTitleBarMetrics {
  /// The minimum logical height reserved for the title bar and its standard controls.
  float height = 0.0F;

  /// Logical width occupied by standard controls on the physical left edge.
  float left_inset = 0.0F;

  /// Logical width occupied by standard controls on the physical right edge.
  float right_inset = 0.0F;

  /// Whether the native top-level window is currently maximized or in its platform-equivalent state.
  bool maximized = false;

  /// Compares all resolved geometry and placement fields exactly.
  bool operator==(const WindowTitleBarMetrics&) const = default;
};

/// Resource-aware accessibility labels for framework-rendered caption controls.
///
/// Empty fields use HuxerUI's localized minimize, maximize or restore, and close strings. A non-empty
/// toggle_maximize label is used for both maximize and restore states.
struct WindowCaptionLabels {
  /// Label for the minimize control.
  StringVariant minimize{};

  /// Label for the maximize or restore control.
  StringVariant toggle_maximize{};

  /// Label for the close control.
  StringVariant close{};

  /// Compares the three label declarations exactly.
  bool operator==(const WindowCaptionLabels&) const = default;
};

/// Startup configuration for the current HuxerUI window or host surface.
///
/// Geometry uses logical device-independent units. Framework-owned desktop windows honor initial_size and
/// minimum_size as client-content dimensions. Android and iOS host views, and externally sized Web surfaces, remain
/// authoritative over their viewport and may ignore top-level window fields that they do not own.
///
/// initial_size, title_bar_height, and both dimensions of a present minimum_size must be finite and positive. When a
/// desktop initial size is below minimum_size, HuxerUI raises each dimension independently before creating the window.
///
/// Example:
/// @code
/// WindowOptions options{
///     .title = "Document Editor",
///     .initial_size = {960.0F, 640.0F},
///     .minimum_size = Size{600.0F, 400.0F},
///     .content_mode = WindowContentMode::EdgeToEdge,
///     .chrome_mode = WindowChromeMode::Custom,
///     .title_bar_height = 48.0F,
/// };
/// @endcode
struct WindowOptions {
  /// The top-level window or surface title. Platforms without an application-owned title may ignore it.
  std::string title = "HuxerUI";

  /// The preferred initial logical client size.
  Size initial_size = {520.0F, 360.0F};

  /// The minimum logical client size for framework-owned resizable desktop windows. std::nullopt leaves it
  /// unconstrained by HuxerUI; the platform may still enforce a larger native minimum.
  std::optional<Size> minimum_size{};

  /// The stable root safe-area policy for this Runtime. Individual pages may still override SystemBarsAppearance.
  WindowContentMode content_mode = WindowContentMode::SafeArea;

  /// The stable title-bar ownership policy for this Runtime.
  WindowChromeMode chrome_mode = WindowChromeMode::System;

  /// The preferred logical height in Custom mode. HuxerUI preserves any larger platform control minimum.
  float title_bar_height = 40.0F;

  /// Accessibility labels for framework-rendered desktop caption controls.
  WindowCaptionLabels caption_labels{};

  /// Compares all startup configuration fields exactly.
  bool operator==(const WindowOptions&) const = default;
};

/// Current platform-submitted window geometry in logical units.
///
/// PlatformAdapter implementations update all fields atomically through Runtime::SetWindowMetrics(). These metrics
/// describe the actual host viewport and are not clamped to WindowOptions::minimum_size.
struct WindowMetrics {
  /// The complete logical drawing surface after any platform-owned IME viewport adjustment.
  Size viewport;

  /// Remaining physical obstructions relative to viewport. Software-keyboard occlusion is not included.
  EdgeInsets safe_area{};

  /// Resolved title-bar geometry, present only when application content occupies native title-bar space.
  std::optional<WindowTitleBarMetrics> title_bar{};

  /// Compares all submitted geometry fields exactly.
  bool operator==(const WindowMetrics&) const = default;
};

/// Declares the painted backgrounds and native foreground brightness for status and navigation system bars.
///
/// This value may be supplied by Theme or applied as a View modifier. HuxerUI paints the backgrounds; platform
/// adapters receive only the resolved foreground brightness supported by the host.
struct SystemBarsAppearance {
  /// Background painted beneath the status-bar region.
  Color status_bar_background = Color::White();

  /// Background painted beneath the navigation-bar or equivalent bottom system region.
  Color navigation_bar_background = Color::White();

  /// Foreground brightness requested for status-bar text and icons.
  SystemBarContentBrightness status_bar_content = SystemBarContentBrightness::Automatic;

  /// Foreground brightness requested for navigation-bar text and icons.
  SystemBarContentBrightness navigation_bar_content = SystemBarContentBrightness::Automatic;

  /// Returns HuxerUI's neutral white, automatically contrasted fallback appearance.
  static SystemBarsAppearance Default();

  /// Returns the property-modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Compares all background and brightness fields exactly.
  bool operator==(const SystemBarsAppearance&) const = default;
};

/// Adds the unconsumed safe-area inset on selected edges to a View's ordinary padding.
///
/// Consumed edges become zero for descendants; unselected edges remain available to a nested SafeAreaPadding.
/// Ordinary Padding and safe-area padding are additive.
///
/// Example:
/// @code
/// Content().With(
///     Padding(16.0F),
///     SafeAreaPadding{.bottom = false}
/// );
/// @endcode
struct SafeAreaPadding {
  /// Whether to consume the remaining top inset.
  bool top = true;

  /// Whether to consume the remaining right inset.
  bool right = true;

  /// Whether to consume the remaining bottom inset.
  bool bottom = true;

  /// Whether to consume the remaining left inset.
  bool left = true;

  /// Returns the property-modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Compares the selected edges exactly.
  bool operator==(const SafeAreaPadding&) const = default;
};

/// Marks non-interactive client content as a native window drag region in Custom chrome mode.
///
/// Interactive descendants retain pointer input and take precedence over a marked ancestor. The marker has no effect
/// when the platform does not submit custom title-bar metrics. WindowTitleBar applies it automatically.
struct WindowDragRegion {
  /// Returns the property-modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// WindowDragRegion has no configuration, so all values compare equal.
  bool operator==(const WindowDragRegion&) const = default;
};

/// A horizontal application title bar that reserves platform- or framework-managed standard controls.
///
/// WindowTitleBar behaves like a Row, fills bounded horizontal space, and measures to at least the resolved custom
/// title-bar height. Children are laid out between the resolved left and right control insets. When custom title-bar
/// metrics are absent, it remains an ordinary horizontal application bar with its natural child height.
///
/// Example:
/// @code
/// WindowTitleBar {
///   Text("Document Editor"),
///   Spacer().With(Grow(1.0F)),
///   Button("Save"),
/// }.With(
///     Padding(8.0F),
///     Spacing(6.0F),
///     Background(theme.colors.surface)
/// );
/// @endcode
class WindowTitleBar final : public Layout<WindowTitleBar> {
public:
  /// Constructs a title bar from an existing child collection.
  explicit WindowTitleBar(std::vector<View> children) : Layout(std::move(children)) {
    this->ApplyModifiers(WindowDragRegion{}, CrossAlign(CrossAxisAlignment::Center));
  }

  /// Constructs a title bar from ordinary View children.
  template <class... Children>
    requires(detail::ViewChild<Children> && ...)
  explicit WindowTitleBar(Children&&... children)
      : WindowTitleBar(detail::CollectChildren(std::forward<Children>(children)...)) {}

  /// Measures and places title-bar children inside the current platform control reservations.
  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints);
};

/// A lightweight handle for the current Runtime's top-level window operations.
///
/// Commands request native operations and do not mutate Runtime placement state directly. Minimize and close handlers
/// are tied to the calling composition scope through Lifecycle: returning true consumes the request, while returning
/// false continues the platform's default operation.
///
/// Example:
/// @code
/// [[huxerui::composable]]
/// View EditorShell() {
///   const WindowHandle window = UseWindow();
///   window.OnCloseRequest([] {
///     if (!HasUnsavedChanges()) {
///       return false;
///     }
///     ShowExitConfirmation();
///     return true;
///   });
///   return Button("Close").OnClick([window] { window.Close(); });
/// }
/// @endcode
class WindowHandle {
public:
  /// Makes the native window visible without otherwise changing placement.
  void Show() const;

  /// Hides the native window without closing the application.
  void Hide() const;

  /// Shows and brings the native window to the foreground when permitted by the platform.
  void Activate() const;

  /// Requests the platform's minimized state after consulting the active minimize handler.
  void Minimize() const;

  /// Requests the platform's maximized or equivalent zoomed state.
  void Maximize() const;

  /// Restores the native window from its minimized or maximized state.
  void Restore() const;

  /// Toggles between the platform's maximized and restored states.
  void ToggleMaximize() const;

  /// Requests the platform's normal close path after consulting the active close handler.
  void Close() const;

  /// Registers a lifecycle-bound minimize request handler. Return true to consume the request or false to continue the
  /// native minimize operation. Changing a dependency reconnects the handler.
  template <class... Dependencies>
  void OnMinimizeRequest(std::function<bool()> handler, Dependencies&&... dependencies) const {
    RegisterRequest(WindowCommand::Minimize, std::move(handler), std::forward<Dependencies>(dependencies)...);
  }

  /// Registers a lifecycle-bound close request handler. Return true to consume the request or false to continue the
  /// native close operation. Changing a dependency reconnects the handler.
  template <class... Dependencies>
  void OnCloseRequest(std::function<bool()> handler, Dependencies&&... dependencies) const {
    RegisterRequest(WindowCommand::Close, std::move(handler), std::forward<Dependencies>(dependencies)...);
  }

private:
  template <class... Dependencies>
  void RegisterRequest(
      WindowCommand command, std::function<bool()> handler, Dependencies&&... dependencies
  ) const {
    if (!handler) {
      throw std::invalid_argument("HuxerUI window request handler must not be empty");
    }
    Lifecycle(
        [window = *this, command, handler = std::move(handler)]() mutable {
          return window.ConnectRequest(command, std::move(handler));
        },
        std::forward<Dependencies>(dependencies)...
    );
  }

  [[nodiscard]] std::function<void()>
  ConnectRequest(WindowCommand command, std::function<bool()> handler) const;

  explicit WindowHandle(std::shared_ptr<detail::WindowService> service) : service_(std::move(service)) {}

  std::shared_ptr<detail::WindowService> service_;

  friend WindowHandle UseWindow();
};

/// Returns a lightweight handle for the current Runtime window.
///
/// Call UseWindow() only from an active composition scope. A reusable function that calls it directly should be marked
/// [[huxerui::composable]].
WindowHandle UseWindow();

} // namespace huxerui
