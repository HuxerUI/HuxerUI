#include "runtime_test_support.h"
#include "win32_platform_view.h"

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/windows/platform_view.h>

#include "win32_accessibility.h"

namespace huxerui::test {
namespace {

constexpr wchar_t kPlatformViewTestClass[] = L"HuxerUI.Tests.PlatformView";
constexpr wchar_t kPlatformViewRootClass[] = L"HuxerUI.Tests.PlatformViewRoot";

State<int> windows_platform_view_value;
State<bool> windows_platform_view_mounted;
State<bool> windows_platform_view_visible;
int windows_platform_view_creates = 0;
int windows_platform_view_updates = 0;
int windows_platform_view_disposals = 0;
int windows_platform_view_event_value = 0;
HWND windows_platform_view_root = nullptr;
HWND windows_platform_view_edit = nullptr;
PlatformEventSink windows_platform_view_event_sink;

struct WindowsPlatformViewEvents {
  struct Changed : Event<int> {
    static constexpr std::string_view Name = "changed";

    static int Decode(const PlatformPayload& payload) {
      return static_cast<int>(payload.AsInteger());
    }
  };
};

struct PlatformViewState {
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
  REQUIRE(RegisterClassExW(&platform_view_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);

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
  REQUIRE(RegisterClassExW(&root_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
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

PlatformPayload WindowsPlatformViewProperties(int value) {
  return PlatformPayload::Object{{"value", value}};
}

View WindowsPlatformViewApp() {
  auto value = UseState(1);
  auto mounted = UseState(true);
  auto visible = UseState(true);
  windows_platform_view_value = value;
  windows_platform_view_mounted = mounted;
  windows_platform_view_visible = visible;
  if (!mounted.Get()) {
    return Text("without platform view");
  }
  return PlatformView("test/WindowsView", WindowsPlatformViewProperties(value.Get()))
      .Events<WindowsPlatformViewEvents::Changed>()
      .On<WindowsPlatformViewEvents::Changed>([](int next) { windows_platform_view_event_value = next; })
      .With(Frame{80.0F, 40.0F}, Opacity{visible.Get() ? 1.0F : 0.0F});
}

HWND CreateWindowsPlatformView(HWND parent, const PlatformPayload& properties, PlatformEventSink event_sink) {
  ++windows_platform_view_creates;
  windows_platform_view_event_sink = std::move(event_sink);
  auto state = std::make_unique<PlatformViewState>();
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
    return nullptr;
  }
  state.release();
  windows_platform_view_root = view;
  const std::wstring text = std::to_wstring(properties.AsObject().at("value").AsInteger());
  SetWindowTextW(windows_platform_view_edit, text.c_str());
  return view;
}

void UpdateWindowsPlatformView(HWND view, const PlatformPayload& properties) {
  static_cast<void>(view);
  ++windows_platform_view_updates;
  const std::wstring text = std::to_wstring(properties.AsObject().at("value").AsInteger());
  SetWindowTextW(windows_platform_view_edit, text.c_str());
}

void DisposeWindowsPlatformView(HWND view) {
  ++windows_platform_view_disposals;
  auto* state = reinterpret_cast<PlatformViewState*>(GetWindowLongPtrW(view, GWLP_USERDATA));
  SetWindowLongPtrW(view, GWLP_USERDATA, 0);
  delete state;
  windows_platform_view_root = nullptr;
  windows_platform_view_edit = nullptr;
}

windows::PlatformViewFactory WindowsPlatformViewFactory() {
  return {
      .create = CreateWindowsPlatformView,
      .update = UpdateWindowsPlatformView,
      .dispose = DisposeWindowsPlatformView,
  };
}

TEST_CASE("WindowsPlatformViewsRetainUpdateHideRetireAndRemount") {
  windows_platform_view_creates = 0;
  windows_platform_view_updates = 0;
  windows_platform_view_disposals = 0;
  windows_platform_view_event_value = 0;
  windows_platform_view_root = nullptr;
  windows_platform_view_edit = nullptr;
  windows_platform_view_event_sink = {};

  TestPlatform platform;
  PlatformModules* modules = nullptr;
  AppOptions options{.show_debug_overlay = false};
  options.root_hooks.push_back([&](RootContext& root) {
    modules = &root.Modules();
    modules->Register("test/WindowsView", WindowsPlatformViewFactory());
  });
  Runtime runtime(WindowsPlatformViewApp, platform, std::move(options));
  runtime.SetWindowMetrics({{240.0F, 160.0F}});
  REQUIRE(modules != nullptr);

  PlatformViewTestWindow window;
  std::vector<std::function<void()>> pending_tasks;
  detail::Win32PlatformViews platform_views(
      GetModuleHandleW(nullptr),
      window.Handle(),
      *modules,
      runtime.CoreRuntime(),
      [&pending_tasks](std::function<void()> task) { pending_tasks.push_back(std::move(task)); },
      [](HWND source, UINT message, WPARAM w_param, LPARAM l_param) {
        return DefWindowProcW(source, message, w_param, l_param);
      }
  );

  const FrameCommit& initial = runtime.BuildCommit();
  REQUIRE(platform_views.Commit(initial.render_frame, 1.0F));
  platform_views.DidPresent();
  REQUIRE(windows_platform_view_creates == 1);
  REQUIRE(windows_platform_view_updates == 0);
  REQUIRE(windows_platform_view_root != nullptr);
  REQUIRE(GetParent(GetParent(windows_platform_view_root)) == window.Handle());
  RECT platform_view_bounds{};
  RECT platform_view_edit_bounds{};
  REQUIRE(GetClientRect(windows_platform_view_root, &platform_view_bounds));
  REQUIRE(GetClientRect(windows_platform_view_edit, &platform_view_edit_bounds));
  REQUIRE(platform_view_bounds.right - platform_view_bounds.left == 80);
  REQUIRE(platform_view_bounds.bottom - platform_view_bounds.top == 40);
  REQUIRE(platform_view_edit_bounds.right - platform_view_edit_bounds.left == 80);
  REQUIRE(platform_view_edit_bounds.bottom - platform_view_edit_bounds.top == 40);
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
  windows_platform_view_event_sink(WindowsPlatformViewEvents::Changed::Name, PlatformPayload(7));
  for (const auto& task : std::exchange(pending_tasks, {})) {
    task();
  }
  REQUIRE(windows_platform_view_event_value == 7);

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
  RECT edit_bounds{};
  REQUIRE(GetWindowRect(windows_platform_view_edit, &edit_bounds));
  REQUIRE(focused_bounds.left == Catch::Approx(edit_bounds.left));
  REQUIRE(focused_bounds.top == Catch::Approx(edit_bounds.top));
  REQUIRE(focused_bounds.width == Catch::Approx(edit_bounds.right - edit_bounds.left));
  REQUIRE(focused_bounds.height == Catch::Approx(edit_bounds.bottom - edit_bounds.top));
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

  const PlatformEventSink retired_event_sink = windows_platform_view_event_sink;
  windows_platform_view_mounted = false;
  REQUIRE(platform_views.Commit(runtime.BuildRenderFrame(), 1.0F));
  accessibility.Commit(runtime.BuildCommit().semantic_frame, &platform_views);
  REQUIRE(windows_platform_view_disposals == 0);
  REQUIRE(IsWindow(retained_root));
  retired_event_sink(WindowsPlatformViewEvents::Changed::Name, PlatformPayload(9));
  for (const auto& task : std::exchange(pending_tasks, {})) {
    task();
  }
  REQUIRE(windows_platform_view_event_value == 7);

  platform_views.DidPresent();
  REQUIRE(windows_platform_view_disposals == 1);
  REQUIRE_FALSE(IsWindow(retained_root));
  REQUIRE_FALSE(platform_views.Commit(runtime.BuildRenderFrame(), 1.0F));

  windows_platform_view_mounted = true;
  REQUIRE(platform_views.Commit(runtime.BuildRenderFrame(), 1.0F));
  platform_views.DidPresent();
  REQUIRE(windows_platform_view_creates == 2);
  REQUIRE(windows_platform_view_root != nullptr);

  windows_platform_view_visible = false;
  REQUIRE(platform_views.Commit(runtime.BuildRenderFrame(), 1.0F));
  windows_platform_view_mounted = false;
  REQUIRE_FALSE(platform_views.Commit(runtime.BuildRenderFrame(), 1.0F));
  REQUIRE(windows_platform_view_disposals == 2);

  accessibility.Reset();
  platform_views.Shutdown();
  REQUIRE(windows_platform_view_disposals == 2);
  windows_platform_view_event_sink = {};
}

} // namespace
} // namespace huxerui::test
