#include "runtime_test_support.h"
#include "win32_platform_view.h"

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/windows/platform_registry.h>

#include "win32_accessibility.h"

namespace huxerui::test {
namespace {

constexpr wchar_t kPlatformViewTestClass[] = L"HuxerUI.Tests.PlatformView";
constexpr wchar_t kPlatformViewRootClass[] = L"HuxerUI.Tests.PlatformViewRoot";

State<int> windows_platform_view_value;
State<int> windows_platform_view_controller;
State<bool> windows_platform_view_controller_attached;
State<bool> windows_platform_view_mounted;
State<bool> windows_platform_view_visible;
int windows_platform_view_creates = 0;
int windows_platform_view_updates = 0;
int windows_platform_view_disposals = 0;
int windows_platform_view_event_value = 0;
std::optional<int> windows_platform_view_create_result;
std::vector<std::string> windows_platform_view_controller_operations;
HWND windows_platform_view_root = nullptr;
HWND windows_platform_view_edit = nullptr;
PlatformEventEmitter windows_platform_view_events;

struct WindowsPlatformViewEvents {
  struct Changed : Event<void(int)> {
    static constexpr std::string_view Name = "changed";
  };

  struct DecisionRequested : Event<bool(int)> {
    static constexpr std::string_view Name = "decisionRequested";
  };

  struct ReadyRequested : Event<int()> {
    static constexpr std::string_view Name = "readyRequested";
  };
};

struct WindowsPlatformViewProperties {
  int value = 0;

  bool operator==(const WindowsPlatformViewProperties&) const = default;
};

struct PlatformViewState {
  HWND root = nullptr;
  HWND edit = nullptr;
};

LRESULT CALLBACK PlatformViewTestProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
  static_cast<void>(w_param);
  auto* state = reinterpret_cast<PlatformViewState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
    state = static_cast<PlatformViewState*>(create->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  switch (message) {
  case WM_CREATE:
    state->edit = CreateWindowExW(
        0,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0,
        0,
        0,
        0,
        window,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );
    windows_platform_view_edit = state->edit;
    return state->edit == nullptr ? -1 : 0;
  case WM_SIZE:
    if (state != nullptr && state->edit != nullptr) {
      MoveWindow(
          state->edit,
          5,
          3,
          std::max(0, static_cast<int>(LOWORD(l_param)) - 10),
          std::max(0, static_cast<int>(HIWORD(l_param)) - 6),
          TRUE
      );
    }
    return 0;
  case WM_SETFOCUS:
    if (state != nullptr && state->edit != nullptr) {
      SetFocus(state->edit);
    }
    return 0;
  default:
    return DefWindowProcW(window, message, w_param, l_param);
  }
}

LRESULT CALLBACK PlatformViewRootProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
  return DefWindowProcW(window, message, w_param, l_param);
}

void RegisterPlatformViewTestClasses() {
  WNDCLASSEXW platform_view_class{
      sizeof(WNDCLASSEXW),
      0,
      PlatformViewTestProcedure,
      0,
      0,
      GetModuleHandleW(nullptr),
      nullptr,
      LoadCursor(nullptr, IDC_ARROW),
      nullptr,
      nullptr,
      kPlatformViewTestClass,
      nullptr,
  };
  REQUIRE((RegisterClassExW(&platform_view_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS));

  WNDCLASSEXW root_class{
      sizeof(WNDCLASSEXW),
      0,
      PlatformViewRootProcedure,
      0,
      0,
      GetModuleHandleW(nullptr),
      nullptr,
      LoadCursor(nullptr, IDC_ARROW),
      nullptr,
      nullptr,
      kPlatformViewRootClass,
      nullptr,
  };
  REQUIRE((RegisterClassExW(&root_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS));
}

class PlatformViewTestWindow final {
public:
  PlatformViewTestWindow() {
    RegisterPlatformViewTestClasses();
    window_ = CreateWindowExW(
        0,
        kPlatformViewRootClass,
        L"",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        240,
        160,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );
    REQUIRE(window_ != nullptr);
  }

  ~PlatformViewTestWindow() {
    if (window_ != nullptr) {
      DestroyWindow(window_);
    }
  }

  PlatformViewTestWindow(const PlatformViewTestWindow&) = delete;
  PlatformViewTestWindow& operator=(const PlatformViewTestWindow&) = delete;

  HWND Handle() const noexcept {
    return window_;
  }

private:
  HWND window_ = nullptr;
};

class ComApartment final {
public:
  ComApartment() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}

  ~ComApartment() {
    if (SUCCEEDED(result_)) {
      CoUninitialize();
    }
  }

  [[nodiscard]] HRESULT Result() const noexcept {
    return result_;
  }

private:
  HRESULT result_ = E_FAIL;
};

View WindowsPlatformViewApp() {
  auto value = UseState(1);
  auto controller = UseState(1);
  auto controller_attached = UseState(true);
  auto mounted = UseState(true);
  auto visible = UseState(true);
  windows_platform_view_value = value;
  windows_platform_view_controller = controller;
  windows_platform_view_controller_attached = controller_attached;
  windows_platform_view_mounted = mounted;
  windows_platform_view_visible = visible;
  if (!mounted.Get()) {
    return Text("without platform view");
  }
  if (controller_attached.Get()) {
    return Stack{
        PlatformView("test/WindowsView", WindowsPlatformViewProperties{value.Get()})
            .Controller(controller.Get())
            .On<WindowsPlatformViewEvents::Changed>([](int next) { windows_platform_view_event_value = next; })
            .On<WindowsPlatformViewEvents::DecisionRequested>([](int next) { return next == 42; })
            .On<WindowsPlatformViewEvents::ReadyRequested>([] { return 7; })
            .With(Frame{80.0F, 40.0F}, Opacity{visible.Get() ? 1.0F : 0.0F}),
    };
  }
  return Stack{
      PlatformView("test/WindowsView", WindowsPlatformViewProperties{value.Get()})
          .On<WindowsPlatformViewEvents::Changed>([](int next) { windows_platform_view_event_value = next; })
          .On<WindowsPlatformViewEvents::DecisionRequested>([](int next) { return next == 42; })
          .On<WindowsPlatformViewEvents::ReadyRequested>([] { return 7; })
          .With(Frame{80.0F, 40.0F}, Opacity{visible.Get() ? 1.0F : 0.0F}),
  };
}

View FailingWindowsPlatformViewApp() {
  return PlatformView("test/FailingWindowsView", WindowsPlatformViewProperties{1}).With(Frame{80.0F, 40.0F});
}

std::shared_ptr<PlatformViewState>
CreateWindowsPlatformView(HWND parent, const WindowsPlatformViewProperties& properties, PlatformEventEmitter events) {
  ++windows_platform_view_creates;
  windows_platform_view_create_result = events.Emit<WindowsPlatformViewEvents::ReadyRequested>();
  windows_platform_view_events = std::move(events);
  auto state = std::make_shared<PlatformViewState>();
  HWND view = CreateWindowExW(
      WS_EX_CONTROLPARENT,
      kPlatformViewTestClass,
      L"",
      WS_CHILD | WS_TABSTOP | WS_CLIPCHILDREN,
      0,
      0,
      0,
      0,
      parent,
      nullptr,
      GetModuleHandleW(nullptr),
      state.get()
  );
  if (view == nullptr) {
    return {};
  }
  state->root = view;
  windows_platform_view_root = view;
  const std::wstring text = std::to_wstring(properties.value);
  SetWindowTextW(windows_platform_view_edit, text.c_str());
  return state;
}

void UpdateWindowsPlatformView(PlatformViewState& state, const WindowsPlatformViewProperties& properties) {
  static_cast<void>(state);
  ++windows_platform_view_updates;
  const std::wstring text = std::to_wstring(properties.value);
  SetWindowTextW(windows_platform_view_edit, text.c_str());
}

void DisposeWindowsPlatformView(PlatformViewState& state) {
  ++windows_platform_view_disposals;
  SetWindowLongPtrW(state.root, GWLP_USERDATA, 0);
  windows_platform_view_root = nullptr;
  windows_platform_view_edit = nullptr;
}

windows::PlatformViewFactory<WindowsPlatformViewProperties, PlatformViewState> WindowsPlatformViewFactory() {
  return {
      .create = CreateWindowsPlatformView,
      .view = [](const std::shared_ptr<PlatformViewState>& state) { return state->root; },
      .update = UpdateWindowsPlatformView,
      .dispose = DisposeWindowsPlatformView,
  };
}

windows::PlatformViewFactory<WindowsPlatformViewProperties, PlatformViewState, int>
ControlledWindowsPlatformViewFactory() {
  return {
      .create = CreateWindowsPlatformView,
      .view = [](const std::shared_ptr<PlatformViewState>& state) { return state->root; },
      .update = UpdateWindowsPlatformView,
      .dispose = DisposeWindowsPlatformView,
      .connect =
          [](PlatformViewState&, const int& controller) {
            windows_platform_view_controller_operations.push_back("connect:" + std::to_string(controller));
          },
      .disconnect =
          [](PlatformViewState&, const int& controller) {
            windows_platform_view_controller_operations.push_back("disconnect:" + std::to_string(controller));
          },
  };
}

windows::PlatformViewFactory<void, PlatformViewState> WindowsPropertylessPlatformViewFactory() {
  return {
      .create =
          [](HWND parent, PlatformEventEmitter events) {
            return CreateWindowsPlatformView(parent, WindowsPlatformViewProperties{}, std::move(events));
          },
      .view = [](const std::shared_ptr<PlatformViewState>& state) { return state->root; },
      .dispose = DisposeWindowsPlatformView,
  };
}

class WindowsPlatformViewTestPlatform final : public TestPlatform {
public:
  detail::PlatformRegistry& Registry() noexcept {
    return PlatformRegistry();
  }
};

TEST_CASE("WindowsPlatformViewsRetainUpdateHideRetireAndRemount") {
  ComApartment com;
  REQUIRE(SUCCEEDED(com.Result()));
  windows_platform_view_creates = 0;
  windows_platform_view_updates = 0;
  windows_platform_view_disposals = 0;
  windows_platform_view_event_value = 0;
  windows_platform_view_create_result.reset();
  windows_platform_view_controller_operations.clear();
  windows_platform_view_root = nullptr;
  windows_platform_view_edit = nullptr;
  windows_platform_view_events = {};

  WindowsPlatformViewTestPlatform platform;
  AppOptions options{.show_debug_overlay = false};
  options.root_hooks.push_back([](RootContext& root) {
    root.RegisterPlatformView<WindowsPlatformViewProperties, int>("test/WindowsView",
                                                                  ControlledWindowsPlatformViewFactory());
    root.RegisterPlatformView<void>("test/PropertylessWindowsView", WindowsPropertylessPlatformViewFactory());
  });
  Runtime runtime(WindowsPlatformViewApp, platform, std::move(options));
  runtime.SetWindowMetrics({{240.0F, 160.0F}});

  PlatformViewTestWindow window;
  detail::Win32PlatformViews platform_views(
      GetModuleHandleW(nullptr), window.Handle(), platform.Registry(), runtime.CoreRuntime(),
      [](HWND source, UINT message, WPARAM w_param, LPARAM l_param) {
        return DefWindowProcW(source, message, w_param, l_param);
      });

  const FrameCommit& initial = runtime.BuildCommit();
  REQUIRE(platform_views.Commit(initial.render_frame, 1.0F));
  platform_views.DidPresent();
  REQUIRE(windows_platform_view_creates == 1);
  REQUIRE(windows_platform_view_updates == 0);
  REQUIRE(windows_platform_view_root != nullptr);
  REQUIRE_FALSE(windows_platform_view_create_result.has_value());
  REQUIRE((windows_platform_view_controller_operations == std::vector<std::string>{"connect:1"}));
  REQUIRE(GetParent(GetParent(windows_platform_view_root)) == window.Handle());
  RECT platform_view_bounds{};
  RECT platform_view_edit_bounds{};
  REQUIRE(GetClientRect(windows_platform_view_root, &platform_view_bounds));
  REQUIRE(GetClientRect(windows_platform_view_edit, &platform_view_edit_bounds));
  REQUIRE(platform_view_bounds.right - platform_view_bounds.left == 80);
  REQUIRE(platform_view_bounds.bottom - platform_view_bounds.top == 40);
  REQUIRE(platform_view_edit_bounds.right - platform_view_edit_bounds.left == 70);
  REQUIRE(platform_view_edit_bounds.bottom - platform_view_edit_bounds.top == 34);
  REQUIRE((GetWindowLongPtrW(windows_platform_view_root, GWL_STYLE) & WS_VISIBLE) != 0);
  REQUIRE((GetWindowLongPtrW(windows_platform_view_edit, GWL_STYLE) & WS_VISIBLE) != 0);

  const auto anchor = std::ranges::find_if(initial.semantic_frame->nodes, [](const SemanticNode& node) {
    return node.platform_view_identity.has_value();
  });
  REQUIRE(anchor != initial.semantic_frame->nodes.end());
  const SemanticNodeId anchor_id = anchor->id;

  windows_platform_view_value = 2;
  REQUIRE(platform_views.Commit(runtime.BuildRenderFrame(), 1.0F));
  REQUIRE(windows_platform_view_creates == 1);
  REQUIRE(windows_platform_view_updates == 1);
  REQUIRE((windows_platform_view_controller_operations == std::vector<std::string>{"connect:1"}));

  windows_platform_view_controller = 2;
  static_cast<void>(platform_views.Commit(runtime.BuildRenderFrame(), 1.0F));
  REQUIRE((windows_platform_view_controller_operations ==
           std::vector<std::string>{"connect:1", "disconnect:1", "connect:2"}));

  windows_platform_view_controller_attached = false;
  static_cast<void>(platform_views.Commit(runtime.BuildRenderFrame(), 1.0F));
  REQUIRE((windows_platform_view_controller_operations ==
           std::vector<std::string>{"connect:1", "disconnect:1", "connect:2", "disconnect:2"}));
  windows_platform_view_controller_attached = true;
  static_cast<void>(platform_views.Commit(runtime.BuildRenderFrame(), 1.0F));
  REQUIRE((windows_platform_view_controller_operations ==
           std::vector<std::string>{"connect:1", "disconnect:1", "connect:2", "disconnect:2", "connect:2"}));
  windows_platform_view_events.Emit<WindowsPlatformViewEvents::Changed>(7);
  REQUIRE(windows_platform_view_event_value == 7);
  REQUIRE(windows_platform_view_events.Emit<WindowsPlatformViewEvents::DecisionRequested>(42) ==
          std::optional{true});
  REQUIRE(windows_platform_view_events.Emit<WindowsPlatformViewEvents::DecisionRequested>(7) ==
          std::optional{false});
  REQUIRE(windows_platform_view_events.Emit<WindowsPlatformViewEvents::ReadyRequested>() == std::optional{7});
  std::optional<bool> off_thread_result;
  std::thread off_thread([&] {
    off_thread_result = windows_platform_view_events.Emit<WindowsPlatformViewEvents::DecisionRequested>(42);
  });
  off_thread.join();
  REQUIRE_FALSE(off_thread_result.has_value());

  const HWND retained_root = windows_platform_view_root;
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(anchor_id, {SemanticActionKind::Focus, std::monostate{}}));
  const FrameCommit& focused = runtime.BuildCommit();
  REQUIRE(platform_views.Commit(focused.render_frame, 1.0F));
  REQUIRE(GetFocus() == windows_platform_view_edit);

  detail::Win32Accessibility accessibility;
  accessibility.SetRuntime(&runtime.CoreRuntime());
  accessibility.SetWindow(window.Handle());
  accessibility.Commit(focused.semantic_frame, &platform_views);
  Microsoft::WRL::ComPtr<IRawElementProviderSimple> semantic_root;
  REQUIRE(accessibility.ProviderForNode(focused.semantic_frame->root, &semantic_root) == S_OK);
  Microsoft::WRL::ComPtr<IRawElementProviderFragmentRoot> semantic_fragment_root;
  REQUIRE(semantic_root.As(&semantic_fragment_root) == S_OK);
  Microsoft::WRL::ComPtr<IRawElementProviderFragment> focused_provider;
  REQUIRE(semantic_fragment_root->GetFocus(&focused_provider) == S_OK);
  REQUIRE(focused_provider != nullptr);
  UiaRect focused_bounds{};
  REQUIRE(focused_provider->get_BoundingRectangle(&focused_bounds) == S_OK);
  RECT platform_view_window_bounds{};
  REQUIRE(GetWindowRect(windows_platform_view_root, &platform_view_window_bounds));
  REQUIRE(focused_bounds.left == Catch::Approx(platform_view_window_bounds.left));
  REQUIRE(focused_bounds.top == Catch::Approx(platform_view_window_bounds.top));
  REQUIRE(
      focused_bounds.width == Catch::Approx(platform_view_window_bounds.right - platform_view_window_bounds.left)
  );
  REQUIRE(
      focused_bounds.height == Catch::Approx(platform_view_window_bounds.bottom - platform_view_window_bounds.top)
  );
  RECT edit_bounds{};
  REQUIRE(GetWindowRect(windows_platform_view_edit, &edit_bounds));
  Microsoft::WRL::ComPtr<IRawElementProviderFragment> hit_provider;
  REQUIRE(
      semantic_fragment_root->ElementProviderFromPoint(
          static_cast<double>(edit_bounds.left + edit_bounds.right) * 0.5,
          static_cast<double>(edit_bounds.top + edit_bounds.bottom) * 0.5,
          &hit_provider
      ) == S_OK
  );
  REQUIRE(hit_provider != nullptr);
  UiaRect hit_bounds{};
  REQUIRE(hit_provider->get_BoundingRectangle(&hit_bounds) == S_OK);
  REQUIRE(hit_bounds.left == Catch::Approx(focused_bounds.left));
  REQUIRE(hit_bounds.top == Catch::Approx(focused_bounds.top));
  REQUIRE(hit_bounds.width == Catch::Approx(focused_bounds.width));
  REQUIRE(hit_bounds.height == Catch::Approx(focused_bounds.height));

  windows_platform_view_visible = false;
  REQUIRE(platform_views.Commit(runtime.BuildRenderFrame(), 1.0F));
  REQUIRE(windows_platform_view_root == retained_root);
  REQUIRE(windows_platform_view_disposals == 0);
  REQUIRE(GetFocus() == window.Handle());

  windows_platform_view_visible = true;
  REQUIRE(platform_views.Commit(runtime.BuildRenderFrame(), 1.0F));
  REQUIRE(windows_platform_view_root == retained_root);
  REQUIRE(windows_platform_view_creates == 1);

  const PlatformEventEmitter retired_events = windows_platform_view_events;
  windows_platform_view_mounted = false;
  REQUIRE(platform_views.Commit(runtime.BuildRenderFrame(), 1.0F));
  accessibility.Commit(runtime.BuildCommit().semantic_frame, &platform_views);
  REQUIRE(windows_platform_view_disposals == 0);
  REQUIRE(IsWindow(retained_root));
  retired_events.Emit<WindowsPlatformViewEvents::Changed>(9);
  REQUIRE_FALSE(retired_events.Emit<WindowsPlatformViewEvents::DecisionRequested>(42).has_value());
  REQUIRE(windows_platform_view_event_value == 7);

  platform_views.DidPresent();
  REQUIRE(windows_platform_view_disposals == 1);
  REQUIRE(windows_platform_view_controller_operations.back() == "disconnect:2");
  REQUIRE_FALSE(IsWindow(retained_root));
  REQUIRE_FALSE(platform_views.Commit(runtime.BuildRenderFrame(), 1.0F));

  windows_platform_view_mounted = true;
  REQUIRE(platform_views.Commit(runtime.BuildRenderFrame(), 1.0F));
  platform_views.DidPresent();
  REQUIRE(windows_platform_view_creates == 2);
  REQUIRE(windows_platform_view_root != nullptr);
  REQUIRE(windows_platform_view_controller_operations.back() == "connect:2");

  windows_platform_view_visible = false;
  REQUIRE(platform_views.Commit(runtime.BuildRenderFrame(), 1.0F));
  windows_platform_view_mounted = false;
  REQUIRE_FALSE(platform_views.Commit(runtime.BuildRenderFrame(), 1.0F));
  REQUIRE(windows_platform_view_disposals == 2);
  REQUIRE(windows_platform_view_controller_operations.back() == "disconnect:2");

  accessibility.Reset();
  platform_views.Shutdown();
  REQUIRE(windows_platform_view_disposals == 2);
  windows_platform_view_events = {};
}

TEST_CASE("WindowsPlatformViewsDisposeInstancesWhenViewCreationFails") {
  ComApartment com;
  REQUIRE(SUCCEEDED(com.Result()));
  windows_platform_view_creates = 0;
  windows_platform_view_disposals = 0;
  windows_platform_view_root = nullptr;
  windows_platform_view_edit = nullptr;

  WindowsPlatformViewTestPlatform platform;
  AppOptions options{.show_debug_overlay = false};
  options.root_hooks.push_back([](RootContext& root) {
    auto factory = WindowsPlatformViewFactory();
    factory.view = [](const std::shared_ptr<PlatformViewState>&) -> HWND {
      throw std::runtime_error("test view failure");
    };
    root.RegisterPlatformView<WindowsPlatformViewProperties>("test/FailingWindowsView", std::move(factory));
  });
  Runtime runtime(FailingWindowsPlatformViewApp, platform, std::move(options));
  runtime.SetWindowMetrics({{240.0F, 160.0F}});

  PlatformViewTestWindow window;
  detail::Win32PlatformViews platform_views(
      GetModuleHandleW(nullptr), window.Handle(), platform.Registry(), runtime.CoreRuntime(),
      [](HWND source, UINT message, WPARAM w_param, LPARAM l_param) {
        return DefWindowProcW(source, message, w_param, l_param);
      });

  REQUIRE_THROWS_AS(platform_views.Commit(runtime.BuildRenderFrame(), 1.0F), std::runtime_error);
  REQUIRE(windows_platform_view_creates == 1);
  REQUIRE(windows_platform_view_disposals == 1);
  REQUIRE(windows_platform_view_root == nullptr);
  REQUIRE(windows_platform_view_edit == nullptr);
  platform_views.Shutdown();
}

} // namespace
} // namespace huxerui::test
