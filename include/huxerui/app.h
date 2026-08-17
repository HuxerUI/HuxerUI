#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <huxerui/clipboard.h>
#include <huxerui/environment.h>
#include <huxerui/event.h>
#include <huxerui/layer.h>
#include <huxerui/platform_module.h>
#include <huxerui/render_scene.h>
#include <huxerui/root.h>
#include <huxerui/semantics.h>
#include <huxerui/text.h>
#include <huxerui/text_input.h>
#include <huxerui/view.h>
#include <huxerui/window.h>

namespace huxerui {

class PlatformResources;
struct ResourceConfiguration;

namespace detail {
class ExternalTextureSurface;
class TextLayout;
} // namespace detail

struct AppOptions {
  WindowOptions window;
  ViewportBreakpoints viewport_breakpoints;
#if defined(NDEBUG)
  bool show_debug_overlay = false;
#else
  bool show_debug_overlay = true;
#endif
  std::vector<RootHook> root_hooks;
};

using RootFactory = View (*)();

class Application final {
public:
  explicit Application(RootFactory root_factory, AppOptions options = {});
  ~Application();

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;
  Application(Application&&) = delete;
  Application& operator=(Application&&) = delete;

  const RootFactory root_factory;
  const AppOptions options;
};

struct ProcessMetrics {
  // CPU time is cumulative; consumers derive utilization from two samples and the logical processor count.
  double cpu_time_seconds = 0.0;
  // Memory usage is the platform's preferred current process-footprint estimate, expressed in bytes.
  std::uint64_t memory_usage_bytes = 0;
  std::uint32_t processor_count = 1;

  bool operator==(const ProcessMetrics&) const = default;
};

class PlatformAdapter : public TextMeasurer {
public:
  // The dispatcher must enqueue work onto this adapter's UI thread without invoking it inline.
  explicit PlatformAdapter(UIThreadDispatcher dispatch_to_ui_thread = {});
  virtual ~PlatformAdapter();

  PlatformAdapter(const PlatformAdapter&) = delete;
  PlatformAdapter& operator=(const PlatformAdapter&) = delete;
  PlatformAdapter(PlatformAdapter&&) = delete;
  PlatformAdapter& operator=(PlatformAdapter&&) = delete;

  virtual void RequestFrameAt(double deadline) = 0;
  virtual double Now() const noexcept = 0;
  virtual std::unique_ptr<detail::TextLayout> CreateTextLayout(
      std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options = {}
  );
  virtual PlatformTextInput* TextInput() noexcept {
    return nullptr;
  }
  virtual PlatformClipboard* Clipboard() noexcept {
    return nullptr;
  }
  virtual PlatformResources* Resources() noexcept {
    return nullptr;
  }
  virtual std::optional<ProcessMetrics> QueryProcessMetrics() noexcept {
    return std::nullopt;
  }
  // Embedded adapters without native-window authority may ignore desktop window commands.
  virtual void RequestWindowCommand(WindowCommand command) {
    static_cast<void>(command);
  }
  // Embedded adapters without native-window authority may leave this optional capability as a no-op.
  virtual void SetSystemBarsContentBrightness(
      SystemBarContentBrightness status_bar, SystemBarContentBrightness navigation_bar
  ) {
    static_cast<void>(status_bar);
    static_cast<void>(navigation_bar);
  }

protected:
  template <class Registration>
  [[nodiscard]] const Registration* FindPlatformModuleRegistration(std::string_view type) const {
    return platform_modules_->FindCompatible<Registration>(type);
  }

  virtual PlatformModuleFactory::Instance CreatePlatformModule(
      std::string_view type, const PlatformPayload& options, PlatformEventSink events
  );

  PlatformModules& Modules() noexcept {
    return *platform_modules_;
  }

private:
  std::shared_ptr<detail::ExternalTextureSurface> external_texture_surface_;
  std::unique_ptr<PlatformModules> platform_modules_;

  friend class PlatformModules;
  friend class Runtime;
};

namespace detail {

const Application& CurrentApplication();
int RunPlatformApplication(const Application& application);

struct NodeExtensionHandle;
struct MountedNode;
struct PointerSession;
struct RuntimeAccess;
struct ViewSpec;
class RecomposeScope;
class ScrollConnection;
class VirtualMeasureSession;

} // namespace detail

class Runtime final {
public:
  Runtime(const Application& application, PlatformAdapter& platform);
  ~Runtime();

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) = delete;
  Runtime& operator=(Runtime&&) = delete;

  void SetWindowMetrics(WindowMetrics metrics);
  // Native non-client hit testing queries the geometry represented by the currently mounted tree.
  [[nodiscard]] bool IsWindowDragRegion(Point position) const;
  void UpdateResourceConfiguration(ResourceConfiguration configuration);
  const FrameCommit& BuildFrame();
  void HandlePointerEvent(const PointerEvent& event);
  void HandleScrollEvent(const ScrollEvent& event);
  void HandleKeyEvent(const KeyEvent& event);
  bool HandleBack();
  bool HandleBack(const BackEvent& event);
  bool PerformTextInputAction(TextInputSessionId session_id, TextInputAction action);
  [[nodiscard]] bool CanPerformTextEditingAction(TextEditingAction action) const;
  bool PerformTextEditingAction(TextEditingAction action);
  TextInputApplyResult HandleTextInputCommands(const TextInputCommandBatch& batch);
  [[nodiscard]] TextInputContext
  QueryTextInputContext(TextInputSessionId session_id, TextOffset start, TextOffset length) const;
  // Geometry is returned in logical coordinates relative to the HuxerUI host view.
  [[nodiscard]] TextInputGeometry QueryTextInputGeometry(TextInputSessionId session_id, TextRange range) const;
  // The point is expressed in logical coordinates relative to the HuxerUI host view.
  [[nodiscard]] TextInputPositionResult QueryTextInputPosition(TextInputSessionId session_id, Point point) const;
  bool PerformSemanticAction(SemanticNodeId node_id, const SemanticAction& action);

private:
  struct State;

  enum class ScrollActivitySource {
    External,
    TextInputReveal,
  };

  void RequestFrame();
  void RequestFrameAfter(double delay_seconds);
  void NotifyScrollActivity(detail::MountedNode& node, ScrollActivitySource source);
  [[nodiscard]] std::optional<std::uint64_t> HitTestPlatformView(Point position) const;
  [[nodiscard]] std::optional<std::uint64_t> FocusedPlatformView() const;
  void SynchronizePlatformViewFocus(std::optional<std::uint64_t> identity, bool focus_visible);
  void MoveFocusFromPlatformView(std::uint64_t identity, bool reverse);
  bool DispatchPlatformViewEvent(std::uint64_t identity, std::string_view name, const PlatformPayload& payload);
  static detail::MountedNode* FindNode(detail::MountedNode& node, std::uint64_t identity);
  static NodeExtension* FindExtension(detail::MountedNode& root, const detail::NodeExtensionHandle& handle);
  static void ActivateNode(detail::MountedNode& node);
  void CancelPointerTarget(detail::PointerSession& session, const PointerEvent& event);
  void ReleaseScrollGesture(detail::PointerSession& session);
  bool DispatchExtensionObservers(detail::PointerSession& session, const PointerEvent& event, bool clear);
  [[nodiscard]] std::optional<std::size_t>
  FindScrollCandidate(const detail::PointerSession& session, Axis axis, float delta);
  std::vector<detail::MountedNode*> ApplyDragScroll(detail::PointerSession& session, float delta);
  void HandlePointerDown(const PointerEvent& event);
  void HandlePointerMove(const PointerEvent& event);
  void HandlePointerCancel(const PointerEvent& event);
  void HandlePointerUp(const PointerEvent& event);
  bool CommitPendingTouchFocus(detail::PointerSession& session, Point position, bool record_tap = false);
  [[nodiscard]] std::optional<std::uint64_t> ResolvePointerFocusTarget(const std::vector<detail::MountedNode*>& route);
  void UpdateHoveredExtensions(Point position);
  void RefreshInteractionTree();
  bool HandleFocusedTextInputKey(const KeyEvent& event);
  detail::MountedNode* ActiveFocusTrapRoot();
  void SetFocusedNode(std::optional<std::uint64_t> identity, std::optional<bool> focus_visible = std::nullopt);
  void MoveFocus(bool reverse, bool wrap = true);
  bool BringTextInputIntoView();
  bool SelectFocusedTextWord(Point position, bool show_overlay = true);
  bool ExtendFocusedTextSelection(Point position, bool start_handle);
  bool QueryFocusedTextSelectionGeometry(Rect& start, Rect& end) const;
  bool HandleTextSelectionOverlayPointer(const PointerEvent& event);
  void HandleTextSelectionClick(const PointerEvent& event);
  void TrackTouchTextSelectionGesture(const PointerEvent& event);
  void AdvanceTextSelectionLongPress(double timestamp);
  void AdvanceTextSelectionOverlay(const FrameInfo& frame);
  void PaintTextSelectionOverlay();
  void ShowTextSelectionOverlay(bool show_handles);
  void HideTextSelectionOverlay();
  void RefreshTextInputSession();
  void StopTextInputSession(TextInputEndReason reason);
  void InvalidateTextInputStateChange(
      std::uint64_t node_identity, const TextInputState& previous, const TextInputState& current
  );
  bool UpdateNodeExtensions(
      detail::MountedNode& node,
      const FrameInfo& frame,
      bool& needs_frame,
      std::optional<double>& next_wakeup,
      bool rebuild_cache
  );
  void BindExtensionInvalidation(detail::MountedNode& node);
  void BuildSemantics();
  const FrameCommit& BuildFrame(FrameInfo frame);
  void InvalidateRoot();
  void InvalidateLayers();
  void DeactivateLayerInput(LayerId id);
  void InvalidateLayerPlacement(LayerId id);
  void InvalidateScope(std::uint64_t scope_id);
  void InvalidateLayout(detail::MountedNode& mounted);
  void EnsureRootStructure();
  void ReconcileWindowControls();
  void CommitWindowAppearance();
  void ComposeApplication();
  void ComposeLayers();
  bool ComposeScope(detail::MountedNode& mounted);
  bool RecomposeDirtyScopes(detail::MountedNode& mounted);
  bool Reconcile(std::unique_ptr<detail::MountedNode>& mounted, const std::shared_ptr<detail::ViewSpec>& incoming);
  std::unique_ptr<detail::MountedNode> Mount(const std::shared_ptr<detail::ViewSpec>& incoming);
  bool ReconcileChildren(
      std::vector<std::unique_ptr<detail::MountedNode>>& mounted_children, const std::vector<View>& incoming_children
  );
  bool ReconcileLayerChildren(
      std::vector<std::unique_ptr<detail::MountedNode>>& mounted_children, const std::vector<View>& incoming_children
  );
  [[nodiscard]] const detail::MountedNode* RootNode() const noexcept;

  std::unique_ptr<State> state_;

  friend class LayerController;
  friend class detail::RecomposeScope;
  friend class detail::ScrollConnection;
  friend class detail::VirtualMeasureSession;
  friend struct detail::RuntimeAccess;
};

int RunApplication();

} // namespace huxerui
