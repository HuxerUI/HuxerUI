// CMake before 4.1 can mistake the Windows SDK GUID equality operator for a DLL export named "==".
#define _NO_SYS_GUID_OPERATOR_EQ_

#include "win32_accessibility.h"

#include <oleauto.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <huxerui/app.h>

#include "win32_internal.h"
#include "win32_platform_view.h"

namespace huxerui::detail {

namespace {

constexpr float kDefaultDpiScale = 1.0F;

bool HasAction(const SemanticNode& node, SemanticActionKind action) noexcept {
  return (node.actions & SemanticActionMask(action)) != 0;
}

const SemanticNode* FindNode(const SemanticFrame& frame, SemanticNodeId id) noexcept {
  const auto found = std::ranges::find(frame.nodes, id, &SemanticNode::id);
  return found == frame.nodes.end() ? nullptr : &*found;
}

struct SemanticNodeSnapshot {
  // The owning frame keeps the node pointer valid while UI Automation answers an off-thread query.
  std::shared_ptr<const SemanticFrame> frame;
  const SemanticNode* node = nullptr;

  explicit operator bool() const noexcept {
    return node != nullptr;
  }

  const SemanticNode* operator->() const noexcept {
    return node;
  }

  const SemanticNode& operator*() const noexcept {
    return *node;
  }
};

CONTROLTYPEID ControlType(SemanticRole role) noexcept {
  switch (role) {
  case SemanticRole::Text:
  case SemanticRole::Heading:
    return UIA_TextControlTypeId;
  case SemanticRole::Image:
    return UIA_ImageControlTypeId;
  case SemanticRole::Button:
    return UIA_ButtonControlTypeId;
  case SemanticRole::Link:
    return UIA_HyperlinkControlTypeId;
  case SemanticRole::Checkbox:
  case SemanticRole::Switch:
    return UIA_CheckBoxControlTypeId;
  case SemanticRole::RadioButton:
    return UIA_RadioButtonControlTypeId;
  case SemanticRole::Slider:
    return UIA_SliderControlTypeId;
  case SemanticRole::ProgressIndicator:
    return UIA_ProgressBarControlTypeId;
  case SemanticRole::ComboBox:
    return UIA_ComboBoxControlTypeId;
  case SemanticRole::TextField:
  case SemanticRole::SearchField:
    return UIA_EditControlTypeId;
  case SemanticRole::Tab:
    return UIA_TabItemControlTypeId;
  case SemanticRole::TabList:
    return UIA_TabControlTypeId;
  case SemanticRole::Menu:
    return UIA_MenuControlTypeId;
  case SemanticRole::MenuItem:
    return UIA_MenuItemControlTypeId;
  case SemanticRole::Dialog:
    return UIA_PaneControlTypeId;
  case SemanticRole::List:
    return UIA_ListControlTypeId;
  case SemanticRole::ListItem:
    return UIA_ListItemControlTypeId;
  case SemanticRole::Grid:
    return UIA_DataGridControlTypeId;
  case SemanticRole::GridCell:
    return UIA_DataItemControlTypeId;
  case SemanticRole::Generic:
  case SemanticRole::Navigation:
  case SemanticRole::ScrollView:
    return UIA_GroupControlTypeId;
  }
  return UIA_GroupControlTypeId;
}

bool IsSelectionContainer(SemanticRole role) noexcept {
  return role == SemanticRole::TabList || role == SemanticRole::Navigation || role == SemanticRole::List;
}

bool SupportsInvoke(const SemanticNode& node) noexcept {
  return HasAction(node, SemanticActionKind::Activate) && !node.checked.has_value() && !node.selected.has_value() &&
         !node.expanded.has_value();
}

bool SupportsToggle(const SemanticNode& node) noexcept {
  return node.checked.has_value() && HasAction(node, SemanticActionKind::Activate);
}

bool SupportsValue(const SemanticNode& node) noexcept {
  return node.role == SemanticRole::ComboBox || node.role == SemanticRole::TextField ||
         node.role == SemanticRole::SearchField;
}

bool SupportsScroll(const SemanticNode& node) noexcept {
  return node.scroll.has_value() && HasAction(node, SemanticActionKind::Scroll);
}

bool SupportsSelection(const SemanticFrame& frame, const SemanticNode& node) noexcept {
  return IsSelectionContainer(node.role) && std::ranges::any_of(node.children, [&frame](SemanticNodeId child) {
           const SemanticNode* item = FindNode(frame, child);
           return item != nullptr && item->selected.has_value();
         });
}

constexpr std::uint16_t kFragmentRootInterface = 1U << 0U;
constexpr std::uint16_t kInvokeInterface = 1U << 1U;
constexpr std::uint16_t kToggleInterface = 1U << 2U;
constexpr std::uint16_t kValueInterface = 1U << 3U;
constexpr std::uint16_t kRangeValueInterface = 1U << 4U;
constexpr std::uint16_t kSelectionItemInterface = 1U << 5U;
constexpr std::uint16_t kSelectionInterface = 1U << 6U;
constexpr std::uint16_t kExpandCollapseInterface = 1U << 7U;
constexpr std::uint16_t kScrollItemInterface = 1U << 8U;
constexpr std::uint16_t kScrollInterface = 1U << 9U;

bool HasInterface(std::uint16_t interfaces, std::uint16_t interface_bit) noexcept {
  return (interfaces & interface_bit) != 0;
}

std::uint16_t ProviderInterfaces(const SemanticFrame& frame, const SemanticNode& node) noexcept {
  std::uint16_t interfaces = node.id == frame.root ? kFragmentRootInterface : 0;
  interfaces |= SupportsInvoke(node) ? kInvokeInterface : 0;
  interfaces |= SupportsToggle(node) ? kToggleInterface : 0;
  interfaces |= SupportsValue(node) ? kValueInterface : 0;
  interfaces |= node.range.has_value() ? kRangeValueInterface : 0;
  interfaces |= node.selected.has_value() ? kSelectionItemInterface : 0;
  interfaces |= SupportsSelection(frame, node) ? kSelectionInterface : 0;
  interfaces |= node.expanded.has_value() ? kExpandCollapseInterface : 0;
  interfaces |= HasAction(node, SemanticActionKind::ShowOnScreen) ? kScrollItemInterface : 0;
  interfaces |= SupportsScroll(node) ? kScrollInterface : 0;
  return interfaces;
}

ToggleState CheckedState(SemanticCheckedState state) noexcept {
  switch (state) {
  case SemanticCheckedState::Unchecked:
    return ToggleState_Off;
  case SemanticCheckedState::Checked:
    return ToggleState_On;
  case SemanticCheckedState::Mixed:
    return ToggleState_Indeterminate;
  }
  return ToggleState_Off;
}

ExpandCollapseState ExpandedState(const SemanticNode& node) noexcept {
  if (!node.expanded.has_value()) {
    return ExpandCollapseState_LeafNode;
  }
  return *node.expanded ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
}

LiveSetting LiveRegionSetting(SemanticLiveRegion region) noexcept {
  switch (region) {
  case SemanticLiveRegion::None:
    return Off;
  case SemanticLiveRegion::Polite:
    return Polite;
  case SemanticLiveRegion::Assertive:
    return Assertive;
  }
  return Off;
}

LONG HeadingLevel(unsigned int level) noexcept {
  if (level == 0 || level > 9) {
    return HeadingLevel_None;
  }
  return HeadingLevel1 + static_cast<LONG>(level - 1);
}

HRESULT SetStringVariant(VARIANT* value, std::string_view text) {
  const std::wstring wide = Utf8ToWide(text);
  value->vt = VT_BSTR;
  value->bstrVal = SysAllocStringLen(wide.data(), static_cast<UINT>(wide.size()));
  return value->bstrVal != nullptr || wide.empty() ? S_OK : E_OUTOFMEMORY;
}

HRESULT SetBoolVariant(VARIANT* value, bool flag) noexcept {
  value->vt = VT_BOOL;
  value->boolVal = flag ? VARIANT_TRUE : VARIANT_FALSE;
  return S_OK;
}

HRESULT SetIntVariant(VARIANT* value, LONG number) noexcept {
  value->vt = VT_I4;
  value->lVal = number;
  return S_OK;
}

HRESULT SetDoubleVariant(VARIANT* value, double number) noexcept {
  value->vt = VT_R8;
  value->dblVal = number;
  return S_OK;
}

bool StructureChanged(const SemanticFrame* previous, const SemanticFrame* current) noexcept {
  if (previous == nullptr || current == nullptr) {
    return previous != current;
  }
  if (previous->root != current->root || previous->nodes.size() != current->nodes.size()) {
    return true;
  }
  return std::ranges::any_of(current->nodes, [previous, current](const SemanticNode& node) {
    const SemanticNode* old = FindNode(*previous, node.id);
    return old == nullptr || old->parent != node.parent || old->children != node.children || old->role != node.role ||
           ProviderInterfaces(*previous, *old) != ProviderInterfaces(*current, node);
  });
}

bool LayoutChanged(const SemanticFrame* previous, const SemanticFrame* current) noexcept {
  if (previous == nullptr || current == nullptr || previous->root != current->root ||
      previous->nodes.size() != current->nodes.size()) {
    return false;
  }
  return std::ranges::any_of(current->nodes, [previous](const SemanticNode& node) {
    const SemanticNode* old = FindNode(*previous, node.id);
    return old != nullptr && old->bounds != node.bounds;
  });
}

double ScrollPercent(const ScrollMetrics& metrics) noexcept {
  if (metrics.maximum_offset <= 0.0F) {
    return UIA_ScrollPatternNoScroll;
  }
  return std::clamp(static_cast<double>(metrics.offset / metrics.maximum_offset) * 100.0, 0.0, 100.0);
}

double ViewSizePercent(const ScrollMetrics& metrics) noexcept {
  if (metrics.content_extent <= 0.0F) {
    return 100.0;
  }
  return std::clamp(static_cast<double>(metrics.viewport_extent / metrics.content_extent) * 100.0, 0.0, 100.0);
}

float ScrollDelta(ScrollAmount amount, const ScrollMetrics& metrics) noexcept {
  switch (amount) {
  case ScrollAmount_LargeDecrement:
    return -metrics.viewport_extent;
  case ScrollAmount_SmallDecrement:
    return -metrics.viewport_extent * 0.1F;
  case ScrollAmount_LargeIncrement:
    return metrics.viewport_extent;
  case ScrollAmount_SmallIncrement:
    return metrics.viewport_extent * 0.1F;
  case ScrollAmount_NoAmount:
    return 0.0F;
  }
  return 0.0F;
}

struct ActionRequest {
  SemanticNodeId id = 0;
  SemanticAction action;
  bool result = false;
};

} // namespace

class Win32SemanticProvider final : public IRawElementProviderSimple,
                                    public IRawElementProviderFragment,
                                    public IRawElementProviderFragmentRoot,
                                    public IInvokeProvider,
                                    public IToggleProvider,
                                    public IValueProvider,
                                    public IRangeValueProvider,
                                    public ISelectionItemProvider,
                                    public ISelectionProvider,
                                    public IExpandCollapseProvider,
                                    public IScrollItemProvider,
                                    public IScrollProvider {
public:
  Win32SemanticProvider(
      std::weak_ptr<Win32Accessibility::State> state, SemanticNodeId id, std::uint16_t interfaces
  ) noexcept
      : state_(std::move(state)), id_(id), interfaces_(interfaces) {}

  [[nodiscard]] std::uint16_t Interfaces() const noexcept {
    return interfaces_;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* options) override;
  HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern_id, IUnknown** provider) override;
  HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property_id, VARIANT* value) override;
  HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** provider) override;

  HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** provider) override;
  HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** runtime_id) override;
  HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* rectangle) override;
  HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** roots) override;
  HRESULT STDMETHODCALLTYPE SetFocus() override;
  HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** root) override;

  HRESULT STDMETHODCALLTYPE
  ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** provider) override;
  HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** provider) override;

  HRESULT STDMETHODCALLTYPE Invoke() override;

  HRESULT STDMETHODCALLTYPE Toggle() override;
  HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* state) override;

  HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override;
  HRESULT STDMETHODCALLTYPE get_Value(BSTR* value) override;
  HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* read_only) override;

  HRESULT STDMETHODCALLTYPE SetValue(double value) override;
  HRESULT STDMETHODCALLTYPE get_Value(double* value) override;
  HRESULT STDMETHODCALLTYPE get_Maximum(double* value) override;
  HRESULT STDMETHODCALLTYPE get_Minimum(double* value) override;
  HRESULT STDMETHODCALLTYPE get_LargeChange(double* value) override;
  HRESULT STDMETHODCALLTYPE get_SmallChange(double* value) override;

  HRESULT STDMETHODCALLTYPE Select() override;
  HRESULT STDMETHODCALLTYPE AddToSelection() override;
  HRESULT STDMETHODCALLTYPE RemoveFromSelection() override;
  HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* selected) override;
  HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** provider) override;

  HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** selection) override;
  HRESULT STDMETHODCALLTYPE get_CanSelectMultiple(BOOL* multiple) override;
  HRESULT STDMETHODCALLTYPE get_IsSelectionRequired(BOOL* required) override;

  HRESULT STDMETHODCALLTYPE Expand() override;
  HRESULT STDMETHODCALLTYPE Collapse() override;
  HRESULT STDMETHODCALLTYPE get_ExpandCollapseState(ExpandCollapseState* state) override;

  HRESULT STDMETHODCALLTYPE ScrollIntoView() override;

  HRESULT STDMETHODCALLTYPE Scroll(ScrollAmount horizontal, ScrollAmount vertical) override;
  HRESULT STDMETHODCALLTYPE SetScrollPercent(double horizontal, double vertical) override;
  HRESULT STDMETHODCALLTYPE get_HorizontalScrollPercent(double* percent) override;
  HRESULT STDMETHODCALLTYPE get_VerticalScrollPercent(double* percent) override;
  HRESULT STDMETHODCALLTYPE get_HorizontalViewSize(double* percent) override;
  HRESULT STDMETHODCALLTYPE get_VerticalViewSize(double* percent) override;
  HRESULT STDMETHODCALLTYPE get_HorizontallyScrollable(BOOL* scrollable) override;
  HRESULT STDMETHODCALLTYPE get_VerticallyScrollable(BOOL* scrollable) override;

private:
  std::shared_ptr<Win32Accessibility::State> LockState() const noexcept;
  SemanticNodeSnapshot Node() const;
  HRESULT Perform(SemanticAction action) const;
  HRESULT Provider(SemanticNodeId id, IRawElementProviderFragment** provider) const;

  std::atomic<ULONG> reference_count_{1};
  std::weak_ptr<Win32Accessibility::State> state_;
  SemanticNodeId id_ = 0;
  // COM requires QueryInterface to expose a static interface set for the lifetime of an object.
  std::uint16_t interfaces_ = 0;
};

struct Win32Accessibility::State final : public std::enable_shared_from_this<Win32Accessibility::State> {
  ~State() {
    ReleaseProviders();
  }

  SemanticNodeSnapshot Node(SemanticNodeId id) const {
    const std::shared_ptr<const SemanticFrame> current = Frame();
    if (!current) {
      return {};
    }
    const SemanticNode* node = FindNode(*current, id);
    return {current, node};
  }

  std::shared_ptr<const SemanticFrame> Frame() const {
    std::scoped_lock lock(mutex);
    return frame;
  }

  HWND PlatformViewHandle(std::uint64_t identity) const {
    std::scoped_lock lock(mutex);
    const auto found = platform_view_handles.find(identity);
    return found == platform_view_handles.end() ? nullptr : found->second;
  }

  static HRESULT QueryWindowProvider(HWND view, REFIID interface_id, void** provider) {
    if (provider == nullptr) {
      return E_INVALIDARG;
    }
    *provider = nullptr;
    if (view == nullptr || !IsWindow(view)) {
      return UIA_E_ELEMENTNOTAVAILABLE;
    }
    IRawElementProviderSimple* host = nullptr;
    const HRESULT host_result = UiaHostProviderFromHwnd(view, &host);
    if (FAILED(host_result) || host == nullptr) {
      return host_result;
    }
    const HRESULT result = host->QueryInterface(interface_id, provider);
    host->Release();
    return result;
  }

  HRESULT QueryPlatformViewProvider(std::uint64_t identity, REFIID interface_id, void** provider) const {
    return QueryWindowProvider(PlatformViewHandle(identity), interface_id, provider);
  }

  HRESULT PlatformViewElementFromPoint(
      std::uint64_t identity,
      double x,
      double y,
      IRawElementProviderFragment** provider
  ) const {
    HWND target = PlatformViewHandle(identity);
    if (target == nullptr || !IsWindow(target)) {
      return UIA_E_ELEMENTNOTAVAILABLE;
    }
    const POINT screen_point{static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y))};
    while (true) {
      POINT local = screen_point;
      if (!ScreenToClient(target, &local)) {
        break;
      }
      const HWND child = ChildWindowFromPointEx(target, local, CWP_SKIPINVISIBLE);
      if (child == nullptr || child == target) {
        break;
      }
      target = child;
    }
    return QueryWindowProvider(target, __uuidof(IRawElementProviderFragment), reinterpret_cast<void**>(provider));
  }

  HRESULT FocusedPlatformViewElement(std::uint64_t identity, IRawElementProviderFragment** provider) const {
    const HWND root = PlatformViewHandle(identity);
    if (root == nullptr || !IsWindow(root)) {
      return UIA_E_ELEMENTNOTAVAILABLE;
    }
    GUITHREADINFO thread_info{sizeof(GUITHREADINFO)};
    const DWORD thread = GetWindowThreadProcessId(root, nullptr);
    HWND focused = GetGUIThreadInfo(thread, &thread_info) ? thread_info.hwndFocus : nullptr;
    if (focused == nullptr || (focused != root && !IsChild(root, focused))) {
      focused = root;
    }
    return QueryWindowProvider(focused, __uuidof(IRawElementProviderFragment), reinterpret_cast<void**>(provider));
  }

  HRESULT Provider(SemanticNodeId id, IRawElementProviderSimple** result) {
    if (result == nullptr) {
      return E_INVALIDARG;
    }
    *result = nullptr;
    Win32SemanticProvider* replaced = nullptr;
    {
      std::scoped_lock lock(mutex);
      const SemanticNode* node = frame ? FindNode(*frame, id) : nullptr;
      if (node == nullptr) {
        return UIA_E_ELEMENTNOTAVAILABLE;
      }
      const std::uint16_t interfaces = ProviderInterfaces(*frame, *node);
      Win32SemanticProvider*& provider = providers[id];
      if (provider == nullptr || provider->Interfaces() != interfaces) {
        Win32SemanticProvider* replacement = new (std::nothrow) Win32SemanticProvider(weak_from_this(), id, interfaces);
        if (replacement == nullptr) {
          if (provider == nullptr) {
            providers.erase(id);
          }
          return E_OUTOFMEMORY;
        }
        replaced = provider;
        provider = replacement;
      }
      provider->AddRef();
      *result = static_cast<IRawElementProviderSimple*>(provider);
    }
    if (replaced != nullptr) {
      replaced->Release();
    }
    return S_OK;
  }

  bool Perform(SemanticNodeId id, SemanticAction action) {
    Runtime* current_runtime = nullptr;
    HWND current_window = nullptr;
    DWORD current_thread = 0;
    {
      std::scoped_lock lock(mutex);
      current_runtime = runtime;
      current_window = window;
      current_thread = ui_thread;
    }
    if (current_runtime == nullptr) {
      return false;
    }
    if (GetCurrentThreadId() == current_thread) {
      return current_runtime->PerformSemanticAction(id, action);
    }
    if (current_window == nullptr || !IsWindow(current_window)) {
      return false;
    }
    // Runtime actions stay on the window thread even when UI Automation invokes a provider from another thread.
    ActionRequest request{id, std::move(action), false};
    SendMessageW(current_window, Win32Accessibility::action_message, 0, reinterpret_cast<LPARAM>(&request));
    return request.result;
  }

  void ReleaseProviders() noexcept {
    std::vector<Win32SemanticProvider*> retained;
    {
      std::scoped_lock lock(mutex);
      for (const auto& [id, provider] : providers) {
        static_cast<void>(id);
        retained.push_back(provider);
      }
      providers.clear();
    }
    for (Win32SemanticProvider* provider : retained) {
      provider->Release();
    }
  }

  void RetainProviders(const SemanticFrame* retained_frame) noexcept {
    {
      std::scoped_lock lock(mutex);
      if (providers.empty()) {
        return;
      }
    }
    std::unordered_set<SemanticNodeId> retained_ids;
    if (retained_frame != nullptr) {
      for (const SemanticNode& node : retained_frame->nodes) {
        retained_ids.insert(node.id);
      }
    }
    std::vector<Win32SemanticProvider*> removed;
    {
      std::scoped_lock lock(mutex);
      for (auto provider = providers.begin(); provider != providers.end();) {
        if (retained_ids.contains(provider->first)) {
          ++provider;
          continue;
        }
        removed.push_back(provider->second);
        provider = providers.erase(provider);
      }
    }
    for (Win32SemanticProvider* provider : removed) {
      provider->Release();
    }
  }

  mutable std::mutex mutex;
  Runtime* runtime = nullptr;
  HWND window = nullptr;
  DWORD ui_thread = GetCurrentThreadId();
  float dpi_scale = kDefaultDpiScale;
  // HWND values are copied with the SemanticFrame so off-thread UI Automation queries never reach their owner.
  std::unordered_map<std::uint64_t, HWND> platform_view_handles;
  std::shared_ptr<const SemanticFrame> frame;
  // The cache owns one COM reference; clients may retain additional references after a node is removed.
  std::unordered_map<SemanticNodeId, Win32SemanticProvider*> providers;
};

std::shared_ptr<Win32Accessibility::State> Win32SemanticProvider::LockState() const noexcept {
  return state_.lock();
}

SemanticNodeSnapshot Win32SemanticProvider::Node() const {
  const std::shared_ptr<Win32Accessibility::State> state = LockState();
  SemanticNodeSnapshot node = state ? state->Node(id_) : SemanticNodeSnapshot{};
  if (node && ProviderInterfaces(*node.frame, *node) != interfaces_) {
    return {};
  }
  return node;
}

HRESULT Win32SemanticProvider::Perform(SemanticAction action) const {
  const SemanticNodeSnapshot node = Node();
  if (!node) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  if (!node->enabled) {
    return UIA_E_ELEMENTNOTENABLED;
  }
  const std::shared_ptr<Win32Accessibility::State> state = LockState();
  return state && state->Perform(id_, std::move(action)) ? S_OK : UIA_E_INVALIDOPERATION;
}

HRESULT Win32SemanticProvider::Provider(SemanticNodeId id, IRawElementProviderFragment** provider) const {
  if (provider == nullptr) {
    return E_INVALIDARG;
  }
  *provider = nullptr;
  const std::shared_ptr<Win32Accessibility::State> state = LockState();
  if (!state) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  IRawElementProviderSimple* simple = nullptr;
  const HRESULT result = state->Provider(id, &simple);
  if (FAILED(result)) {
    return result;
  }
  const HRESULT query = simple->QueryInterface(IID_PPV_ARGS(provider));
  simple->Release();
  return query;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::QueryInterface(REFIID iid, void** object) {
  if (object == nullptr) {
    return E_INVALIDARG;
  }
  *object = nullptr;
  if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(IRawElementProviderSimple))) {
    *object = static_cast<IRawElementProviderSimple*>(this);
  } else if (IsEqualIID(iid, __uuidof(IRawElementProviderFragment))) {
    *object = static_cast<IRawElementProviderFragment*>(this);
  } else if (IsEqualIID(iid, __uuidof(IRawElementProviderFragmentRoot)) &&
             HasInterface(interfaces_, kFragmentRootInterface)) {
    *object = static_cast<IRawElementProviderFragmentRoot*>(this);
  } else if (IsEqualIID(iid, __uuidof(IInvokeProvider)) && HasInterface(interfaces_, kInvokeInterface)) {
    *object = static_cast<IInvokeProvider*>(this);
  } else if (IsEqualIID(iid, __uuidof(IToggleProvider)) && HasInterface(interfaces_, kToggleInterface)) {
    *object = static_cast<IToggleProvider*>(this);
  } else if (IsEqualIID(iid, __uuidof(IValueProvider)) && HasInterface(interfaces_, kValueInterface)) {
    *object = static_cast<IValueProvider*>(this);
  } else if (IsEqualIID(iid, __uuidof(IRangeValueProvider)) && HasInterface(interfaces_, kRangeValueInterface)) {
    *object = static_cast<IRangeValueProvider*>(this);
  } else if (IsEqualIID(iid, __uuidof(ISelectionItemProvider)) && HasInterface(interfaces_, kSelectionItemInterface)) {
    *object = static_cast<ISelectionItemProvider*>(this);
  } else if (IsEqualIID(iid, __uuidof(ISelectionProvider)) && HasInterface(interfaces_, kSelectionInterface)) {
    *object = static_cast<ISelectionProvider*>(this);
  } else if (IsEqualIID(iid, __uuidof(IExpandCollapseProvider)) && HasInterface(interfaces_, kExpandCollapseInterface)) {
    *object = static_cast<IExpandCollapseProvider*>(this);
  } else if (IsEqualIID(iid, __uuidof(IScrollItemProvider)) && HasInterface(interfaces_, kScrollItemInterface)) {
    *object = static_cast<IScrollItemProvider*>(this);
  } else if (IsEqualIID(iid, __uuidof(IScrollProvider)) && HasInterface(interfaces_, kScrollInterface)) {
    *object = static_cast<IScrollProvider*>(this);
  }
  if (*object == nullptr) {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

ULONG STDMETHODCALLTYPE Win32SemanticProvider::AddRef() {
  return ++reference_count_;
}

ULONG STDMETHODCALLTYPE Win32SemanticProvider::Release() {
  const ULONG remaining = --reference_count_;
  if (remaining == 0) {
    delete this;
  }
  return remaining;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_ProviderOptions(ProviderOptions* options) {
  if (options == nullptr) {
    return E_INVALIDARG;
  }
  *options = ProviderOptions_ServerSideProvider;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::GetPatternProvider(PATTERNID pattern_id, IUnknown** provider) {
  if (provider == nullptr) {
    return E_INVALIDARG;
  }
  *provider = nullptr;
  if (!Node()) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  HRESULT result = E_NOINTERFACE;
  if (pattern_id == UIA_InvokePatternId) {
    result = QueryInterface(__uuidof(IInvokeProvider), reinterpret_cast<void**>(provider));
  } else if (pattern_id == UIA_TogglePatternId) {
    result = QueryInterface(__uuidof(IToggleProvider), reinterpret_cast<void**>(provider));
  } else if (pattern_id == UIA_ValuePatternId) {
    result = QueryInterface(__uuidof(IValueProvider), reinterpret_cast<void**>(provider));
  } else if (pattern_id == UIA_RangeValuePatternId) {
    result = QueryInterface(__uuidof(IRangeValueProvider), reinterpret_cast<void**>(provider));
  } else if (pattern_id == UIA_SelectionItemPatternId) {
    result = QueryInterface(__uuidof(ISelectionItemProvider), reinterpret_cast<void**>(provider));
  } else if (pattern_id == UIA_SelectionPatternId) {
    result = QueryInterface(__uuidof(ISelectionProvider), reinterpret_cast<void**>(provider));
  } else if (pattern_id == UIA_ExpandCollapsePatternId) {
    result = QueryInterface(__uuidof(IExpandCollapseProvider), reinterpret_cast<void**>(provider));
  } else if (pattern_id == UIA_ScrollItemPatternId) {
    result = QueryInterface(__uuidof(IScrollItemProvider), reinterpret_cast<void**>(provider));
  } else if (pattern_id == UIA_ScrollPatternId) {
    result = QueryInterface(__uuidof(IScrollProvider), reinterpret_cast<void**>(provider));
  }
  return result == E_NOINTERFACE ? S_OK : result;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::GetPropertyValue(PROPERTYID property_id, VARIANT* value) {
  if (value == nullptr) {
    return E_INVALIDARG;
  }
  VariantInit(value);
  const SemanticNodeSnapshot node = Node();
  if (!node) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  switch (property_id) {
  case UIA_ControlTypePropertyId:
    return SetIntVariant(value, ControlType(node->role));
  case UIA_NamePropertyId:
    return SetStringVariant(value, node->label.empty() ? node->placeholder : node->label);
  case UIA_AutomationIdPropertyId:
    return SetStringVariant(value, node->identifier);
  case UIA_ClassNamePropertyId:
    return SetStringVariant(value, "HuxerUIElement");
  case UIA_FrameworkIdPropertyId:
    return SetStringVariant(value, "HuxerUI");
  case UIA_HelpTextPropertyId:
    return SetStringVariant(value, node->hint);
  case UIA_ItemStatusPropertyId:
    return SetStringVariant(value, !node->error.empty() ? node->error : node->state_description);
  case UIA_IsEnabledPropertyId:
    return SetBoolVariant(value, node->enabled);
  case UIA_HasKeyboardFocusPropertyId:
    return SetBoolVariant(value, node->focused);
  case UIA_IsKeyboardFocusablePropertyId:
    return SetBoolVariant(value, HasAction(*node, SemanticActionKind::Focus));
  case UIA_IsOffscreenPropertyId:
    return SetBoolVariant(value, node->offscreen);
  case UIA_IsPasswordPropertyId:
    return SetBoolVariant(value, node->secure);
  case UIA_IsControlElementPropertyId:
  case UIA_IsContentElementPropertyId:
    return SetBoolVariant(value, true);
  case UIA_IsDialogPropertyId:
    return SetBoolVariant(value, node->role == SemanticRole::Dialog);
  case UIA_IsRequiredForFormPropertyId:
    return SetBoolVariant(value, node->required.value_or(false));
  case UIA_IsDataValidForFormPropertyId:
    return SetBoolVariant(value, !node->invalid.value_or(false));
  case UIA_LiveSettingPropertyId:
    return SetIntVariant(value, LiveRegionSetting(node->live_region));
  case UIA_HeadingLevelPropertyId:
    return SetIntVariant(value, HeadingLevel(node->heading_level.value_or(0)));
  case UIA_PositionInSetPropertyId:
    if (node->collection_item && node->collection_item->index &&
        *node->collection_item->index < static_cast<std::size_t>(std::numeric_limits<LONG>::max())) {
      return SetIntVariant(value, static_cast<LONG>(*node->collection_item->index + 1));
    }
    return S_OK;
  case UIA_SizeOfSetPropertyId: {
    const SemanticNode* parent = node->parent ? FindNode(*node.frame, *node->parent) : nullptr;
    if (parent && parent->collection && parent->collection->item_count &&
        *parent->collection->item_count <= static_cast<std::size_t>(std::numeric_limits<LONG>::max())) {
      return SetIntVariant(value, static_cast<LONG>(*parent->collection->item_count));
    }
    return S_OK;
  }
  case UIA_OrientationPropertyId:
    if (node->scroll) {
      return SetIntVariant(
          value,
          node->scroll->axis == Axis::Horizontal ? OrientationType_Horizontal : OrientationType_Vertical
      );
    }
    return S_OK;
  default:
    return S_OK;
  }
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_HostRawElementProvider(IRawElementProviderSimple** provider) {
  if (provider == nullptr) {
    return E_INVALIDARG;
  }
  *provider = nullptr;
  const SemanticNodeSnapshot node = Node();
  const std::shared_ptr<Win32Accessibility::State> state = LockState();
  if (!node || !state) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  HWND window = nullptr;
  {
    std::scoped_lock lock(state->mutex);
    window = state->window;
  }
  if (window != nullptr && HasInterface(interfaces_, kFragmentRootInterface)) {
    return UiaHostProviderFromHwnd(window, provider);
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
Win32SemanticProvider::Navigate(NavigateDirection direction, IRawElementProviderFragment** provider) {
  if (provider == nullptr) {
    return E_INVALIDARG;
  }
  *provider = nullptr;
  const SemanticNodeSnapshot node = Node();
  if (!node) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  const std::shared_ptr<Win32Accessibility::State> state = LockState();
  if (!state) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  const std::shared_ptr<const SemanticFrame>& frame = node.frame;
  std::optional<SemanticNodeId> destination;
  if (direction == NavigateDirection_Parent) {
    destination = node->parent;
  } else if (direction == NavigateDirection_FirstChild && !node->children.empty()) {
    destination = node->children.front();
  } else if (direction == NavigateDirection_LastChild && !node->children.empty()) {
    destination = node->children.back();
  } else if (
      (direction == NavigateDirection_NextSibling || direction == NavigateDirection_PreviousSibling) && node->parent
  ) {
    const SemanticNode* parent = FindNode(*frame, *node->parent);
    if (parent != nullptr) {
      const auto found = std::ranges::find(parent->children, id_);
      if (found != parent->children.end()) {
        if (direction == NavigateDirection_NextSibling && std::next(found) != parent->children.end()) {
          destination = *std::next(found);
        } else if (direction == NavigateDirection_PreviousSibling && found != parent->children.begin()) {
          destination = *std::prev(found);
        }
      }
    }
  }
  return destination ? Provider(*destination, provider) : S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::GetRuntimeId(SAFEARRAY** runtime_id) {
  if (runtime_id == nullptr) {
    return E_INVALIDARG;
  }
  *runtime_id = nullptr;
  if (!Node()) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  if (HasInterface(interfaces_, kFragmentRootInterface)) {
    return S_OK;
  }
  *runtime_id = SafeArrayCreateVector(VT_I4, 0, 3);
  if (*runtime_id == nullptr) {
    return E_OUTOFMEMORY;
  }
  LONG* values = nullptr;
  HRESULT result = SafeArrayAccessData(*runtime_id, reinterpret_cast<void**>(&values));
  if (SUCCEEDED(result)) {
    values[0] = UiaAppendRuntimeId;
    values[1] = static_cast<LONG>(id_ & 0xFFFFFFFFULL);
    values[2] = static_cast<LONG>(id_ >> 32U);
    result = SafeArrayUnaccessData(*runtime_id);
  }
  if (FAILED(result)) {
    SafeArrayDestroy(*runtime_id);
    *runtime_id = nullptr;
  }
  return result;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_BoundingRectangle(UiaRect* rectangle) {
  if (rectangle == nullptr) {
    return E_INVALIDARG;
  }
  *rectangle = {};
  const SemanticNodeSnapshot node = Node();
  const std::shared_ptr<Win32Accessibility::State> state = LockState();
  if (!node || !state) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  HWND window = nullptr;
  float scale = kDefaultDpiScale;
  {
    std::scoped_lock lock(state->mutex);
    window = state->window;
    scale = state->dpi_scale;
  }
  if (window == nullptr) {
    return S_OK;
  }
  POINT origin{};
  ClientToScreen(window, &origin);
  rectangle->left = static_cast<double>(origin.x) + node->bounds.x * scale;
  rectangle->top = static_cast<double>(origin.y) + node->bounds.y * scale;
  rectangle->width = node->bounds.width * scale;
  rectangle->height = node->bounds.height * scale;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::GetEmbeddedFragmentRoots(SAFEARRAY** roots) {
  if (roots == nullptr) {
    return E_INVALIDARG;
  }
  *roots = nullptr;
  const SemanticNodeSnapshot node = Node();
  if (!node) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  if (!node->platform_view_identity.has_value()) {
    return S_OK;
  }
  const std::shared_ptr<Win32Accessibility::State> state = LockState();
  if (!state) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  IRawElementProviderSimple* embedded = nullptr;
  const HRESULT provider_result = state->QueryPlatformViewProvider(
      *node->platform_view_identity,
      __uuidof(IRawElementProviderSimple),
      reinterpret_cast<void**>(&embedded)
  );
  if (provider_result == UIA_E_ELEMENTNOTAVAILABLE) {
    return S_OK;
  }
  if (FAILED(provider_result)) {
    return provider_result;
  }
  SAFEARRAY* result = SafeArrayCreateVector(VT_UNKNOWN, 0, 1);
  if (result == nullptr) {
    embedded->Release();
    return E_OUTOFMEMORY;
  }
  LONG index = 0;
  IUnknown* unknown = embedded;
  const HRESULT array_result = SafeArrayPutElement(result, &index, unknown);
  embedded->Release();
  if (FAILED(array_result)) {
    SafeArrayDestroy(result);
    return array_result;
  }
  *roots = result;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::SetFocus() {
  const SemanticNodeSnapshot node = Node();
  if (!node) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  if (!HasAction(*node, SemanticActionKind::Focus)) {
    return UIA_E_NOTSUPPORTED;
  }
  return Perform({SemanticActionKind::Focus, std::monostate{}});
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_FragmentRoot(IRawElementProviderFragmentRoot** root) {
  if (root == nullptr) {
    return E_INVALIDARG;
  }
  *root = nullptr;
  const SemanticNodeSnapshot node = Node();
  if (!node) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  const std::shared_ptr<Win32Accessibility::State> state = LockState();
  if (!state) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  IRawElementProviderSimple* simple = nullptr;
  const HRESULT result = state->Provider(node.frame->root, &simple);
  if (FAILED(result)) {
    return result;
  }
  const HRESULT query = simple->QueryInterface(IID_PPV_ARGS(root));
  simple->Release();
  return query;
}

HRESULT STDMETHODCALLTYPE
Win32SemanticProvider::ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** provider) {
  if (provider == nullptr) {
    return E_INVALIDARG;
  }
  *provider = nullptr;
  const SemanticNodeSnapshot root = Node();
  if (!root || !HasInterface(interfaces_, kFragmentRootInterface)) {
    return root ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  const std::shared_ptr<Win32Accessibility::State> state = LockState();
  if (!state) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  const std::shared_ptr<const SemanticFrame>& frame = root.frame;
  HWND window = nullptr;
  float scale = kDefaultDpiScale;
  {
    std::scoped_lock lock(state->mutex);
    window = state->window;
    scale = state->dpi_scale;
  }
  POINT point{static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y))};
  if (window != nullptr) {
    ScreenToClient(window, &point);
  }
  const Point local{static_cast<float>(point.x) / scale, static_cast<float>(point.y) / scale};
  std::optional<SemanticNodeId> found;
  const auto hit_test = [&](const auto& self, SemanticNodeId candidate) -> bool {
    const SemanticNode* node = FindNode(*frame, candidate);
    if (node == nullptr || node->offscreen || !node->bounds.Contains(local)) {
      return false;
    }
    found = candidate;
    for (auto child = node->children.rbegin(); child != node->children.rend(); ++child) {
      if (self(self, *child)) {
        break;
      }
    }
    return true;
  };
  if (!hit_test(hit_test, frame->root)) {
    return S_OK;
  }
  const SemanticNode* found_node = FindNode(*frame, *found);
  if (found_node != nullptr && found_node->platform_view_identity.has_value()) {
    const HRESULT result = state->PlatformViewElementFromPoint(*found_node->platform_view_identity, x, y, provider);
    if (SUCCEEDED(result)) {
      return result;
    }
  }
  return Provider(*found, provider);
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::GetFocus(IRawElementProviderFragment** provider) {
  if (provider == nullptr) {
    return E_INVALIDARG;
  }
  *provider = nullptr;
  const SemanticNodeSnapshot root = Node();
  if (!root || !HasInterface(interfaces_, kFragmentRootInterface)) {
    return root ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  const std::shared_ptr<Win32Accessibility::State> state = LockState();
  if (!state) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  const std::shared_ptr<const SemanticFrame>& frame = root.frame;
  const auto focused = std::ranges::find(frame->nodes, true, &SemanticNode::focused);
  if (focused == frame->nodes.end()) {
    return S_OK;
  }
  if (focused->platform_view_identity.has_value()) {
    const HRESULT result = state->FocusedPlatformViewElement(*focused->platform_view_identity, provider);
    if (SUCCEEDED(result)) {
      return result;
    }
  }
  return Provider(focused->id, provider);
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::Invoke() {
  const SemanticNodeSnapshot node = Node();
  if (!node || !SupportsInvoke(*node)) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  return Perform({SemanticActionKind::Activate, std::monostate{}});
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::Toggle() {
  const SemanticNodeSnapshot node = Node();
  if (!node || !SupportsToggle(*node)) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  return Perform({SemanticActionKind::Activate, std::monostate{}});
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_ToggleState(ToggleState* state) {
  if (state == nullptr) {
    return E_INVALIDARG;
  }
  const SemanticNodeSnapshot node = Node();
  if (!node) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  if (!node->checked) {
    return UIA_E_NOTSUPPORTED;
  }
  *state = CheckedState(*node->checked);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::SetValue(LPCWSTR value) {
  const SemanticNodeSnapshot node = Node();
  if (!node) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  if (!SupportsValue(*node)) {
    return UIA_E_NOTSUPPORTED;
  }
  if (node->read_only.value_or(false) || !HasAction(*node, SemanticActionKind::SetText)) {
    return UIA_E_NOTSUPPORTED;
  }
  return Perform({SemanticActionKind::SetText, WideToUtf8(value == nullptr ? L"" : value)});
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_Value(BSTR* value) {
  if (value == nullptr) {
    return E_INVALIDARG;
  }
  *value = nullptr;
  const SemanticNodeSnapshot node = Node();
  if (!node) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  if (!SupportsValue(*node)) {
    return UIA_E_NOTSUPPORTED;
  }
  if (node->secure) {
    return UIA_E_INVALIDOPERATION;
  }
  const std::wstring wide = Utf8ToWide(node->value);
  *value = SysAllocStringLen(wide.data(), static_cast<UINT>(wide.size()));
  return *value != nullptr || wide.empty() ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_IsReadOnly(BOOL* read_only) {
  if (read_only == nullptr) {
    return E_INVALIDARG;
  }
  const SemanticNodeSnapshot node = Node();
  if (!node) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  if (!SupportsValue(*node) && !node->range.has_value()) {
    return UIA_E_NOTSUPPORTED;
  }
  const bool editable = node->range.has_value() ? HasAction(*node, SemanticActionKind::SetValue)
                                                : HasAction(*node, SemanticActionKind::SetText);
  *read_only = node->read_only.value_or(!editable) ? TRUE : FALSE;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::SetValue(double value) {
  const SemanticNodeSnapshot node = Node();
  if (!node) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  if (!node->range || !HasAction(*node, SemanticActionKind::SetValue)) {
    return UIA_E_NOTSUPPORTED;
  }
  if (!std::isfinite(value) || value < node->range->minimum || value > node->range->maximum) {
    return E_INVALIDARG;
  }
  return Perform({SemanticActionKind::SetValue, value});
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_Value(double* value) {
  if (value == nullptr) {
    return E_INVALIDARG;
  }
  const SemanticNodeSnapshot node = Node();
  if (!node || !node->range) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  *value = node->range->current;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_Maximum(double* value) {
  if (value == nullptr) {
    return E_INVALIDARG;
  }
  const SemanticNodeSnapshot node = Node();
  if (!node || !node->range) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  *value = node->range->maximum;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_Minimum(double* value) {
  if (value == nullptr) {
    return E_INVALIDARG;
  }
  const SemanticNodeSnapshot node = Node();
  if (!node || !node->range) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  *value = node->range->minimum;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_LargeChange(double* value) {
  if (value == nullptr) {
    return E_INVALIDARG;
  }
  const SemanticNodeSnapshot node = Node();
  if (!node || !node->range) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  *value = node->range->step.value_or((node->range->maximum - node->range->minimum) * 0.1);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_SmallChange(double* value) {
  if (value == nullptr) {
    return E_INVALIDARG;
  }
  const SemanticNodeSnapshot node = Node();
  if (!node || !node->range) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  *value = node->range->step.value_or((node->range->maximum - node->range->minimum) * 0.01);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::Select() {
  const SemanticNodeSnapshot node = Node();
  if (!node || !node->selected.has_value() || !HasAction(*node, SemanticActionKind::Activate)) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  if (*node->selected) {
    return S_OK;
  }
  return Perform({SemanticActionKind::Activate, std::monostate{}});
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::AddToSelection() {
  const SemanticNodeSnapshot node = Node();
  if (!node || !node->selected.has_value() || !HasAction(*node, SemanticActionKind::Activate)) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  if (*node->selected) {
    return S_OK;
  }
  const std::optional<SemanticNodeId> parent = node->parent;
  const SemanticNode* container = parent ? FindNode(*node.frame, *parent) : nullptr;
  if (container == nullptr || !SupportsSelection(*node.frame, *container)) {
    return UIA_E_INVALIDOPERATION;
  }
  const bool has_selection = std::ranges::any_of(container->children, [&node](SemanticNodeId child) {
    const SemanticNode* item = FindNode(*node.frame, child);
    return item != nullptr && item->selected.value_or(false);
  });
  return has_selection ? UIA_E_INVALIDOPERATION : Perform({SemanticActionKind::Activate, std::monostate{}});
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::RemoveFromSelection() {
  return UIA_E_INVALIDOPERATION;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_IsSelected(BOOL* selected) {
  if (selected == nullptr) {
    return E_INVALIDARG;
  }
  const SemanticNodeSnapshot node = Node();
  if (!node || !node->selected.has_value()) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  *selected = *node->selected ? TRUE : FALSE;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_SelectionContainer(IRawElementProviderSimple** provider) {
  if (provider == nullptr) {
    return E_INVALIDARG;
  }
  *provider = nullptr;
  const SemanticNodeSnapshot node = Node();
  if (!node || !node->selected.has_value()) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  const std::shared_ptr<Win32Accessibility::State> state = LockState();
  if (!state) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  const std::shared_ptr<const SemanticFrame>& frame = node.frame;
  std::optional<SemanticNodeId> parent = node->parent;
  while (parent) {
    const SemanticNode* candidate = FindNode(*frame, *parent);
    if (candidate == nullptr) {
      break;
    }
    if (IsSelectionContainer(candidate->role) &&
        std::ranges::any_of(candidate->children, [frame](SemanticNodeId child) {
          const SemanticNode* item = FindNode(*frame, child);
          return item != nullptr && item->selected.has_value();
        })) {
      return state->Provider(candidate->id, provider);
    }
    parent = candidate->parent;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::GetSelection(SAFEARRAY** selection) {
  if (selection == nullptr) {
    return E_INVALIDARG;
  }
  *selection = nullptr;
  const SemanticNodeSnapshot node = Node();
  if (!node) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  const std::shared_ptr<Win32Accessibility::State> state = LockState();
  if (!state) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  const std::shared_ptr<const SemanticFrame>& frame = node.frame;
  if (!SupportsSelection(*frame, *node)) {
    return UIA_E_NOTSUPPORTED;
  }
  std::vector<IUnknown*> selected;
  for (SemanticNodeId child : node->children) {
    const SemanticNode* item = FindNode(*frame, child);
    if (item == nullptr || !item->selected.value_or(false)) {
      continue;
    }
    IRawElementProviderSimple* provider = nullptr;
    if (SUCCEEDED(state->Provider(child, &provider))) {
      selected.push_back(provider);
    }
  }
  if (selected.size() > static_cast<std::size_t>(std::numeric_limits<LONG>::max())) {
    for (IUnknown* provider : selected) {
      provider->Release();
    }
    return E_OUTOFMEMORY;
  }
  *selection = SafeArrayCreateVector(VT_UNKNOWN, 0, static_cast<ULONG>(selected.size()));
  if (*selection == nullptr) {
    for (IUnknown* provider : selected) {
      provider->Release();
    }
    return E_OUTOFMEMORY;
  }
  for (std::size_t item = 0; item < selected.size(); ++item) {
    LONG index = static_cast<LONG>(item);
    IUnknown* provider = selected[item];
    const HRESULT result = SafeArrayPutElement(*selection, &index, provider);
    provider->Release();
    if (FAILED(result)) {
      for (std::size_t remaining = item + 1; remaining < selected.size(); ++remaining) {
        selected[remaining]->Release();
      }
      SafeArrayDestroy(*selection);
      *selection = nullptr;
      return result;
    }
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_CanSelectMultiple(BOOL* multiple) {
  if (multiple == nullptr) {
    return E_INVALIDARG;
  }
  const SemanticNodeSnapshot node = Node();
  if (!node) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  if (!SupportsSelection(*node.frame, *node)) {
    return UIA_E_NOTSUPPORTED;
  }
  *multiple = FALSE;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_IsSelectionRequired(BOOL* required) {
  if (required == nullptr) {
    return E_INVALIDARG;
  }
  const SemanticNodeSnapshot node = Node();
  if (!node) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  if (!SupportsSelection(*node.frame, *node)) {
    return UIA_E_NOTSUPPORTED;
  }
  *required = TRUE;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::Expand() {
  const SemanticNodeSnapshot node = Node();
  if (!node || !HasAction(*node, SemanticActionKind::Expand)) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  return Perform({SemanticActionKind::Expand, std::monostate{}});
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::Collapse() {
  const SemanticNodeSnapshot node = Node();
  if (!node || !HasAction(*node, SemanticActionKind::Collapse)) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  return Perform({SemanticActionKind::Collapse, std::monostate{}});
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_ExpandCollapseState(ExpandCollapseState* state) {
  if (state == nullptr) {
    return E_INVALIDARG;
  }
  const SemanticNodeSnapshot node = Node();
  if (!node) {
    return UIA_E_ELEMENTNOTAVAILABLE;
  }
  if (!node->expanded.has_value()) {
    return UIA_E_NOTSUPPORTED;
  }
  *state = ExpandedState(*node);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::ScrollIntoView() {
  const SemanticNodeSnapshot node = Node();
  if (!node || !HasAction(*node, SemanticActionKind::ShowOnScreen)) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  return Perform({SemanticActionKind::ShowOnScreen, std::monostate{}});
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::Scroll(ScrollAmount horizontal, ScrollAmount vertical) {
  const SemanticNodeSnapshot node = Node();
  if (!node || !SupportsScroll(*node)) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  if (node->scroll->maximum_offset <= 0.0F ||
      (node->scroll->axis == Axis::Horizontal && vertical != ScrollAmount_NoAmount) ||
      (node->scroll->axis == Axis::Vertical && horizontal != ScrollAmount_NoAmount)) {
    return UIA_E_INVALIDOPERATION;
  }
  const float delta = node->scroll->axis == Axis::Horizontal ? ScrollDelta(horizontal, *node->scroll)
                                                             : ScrollDelta(vertical, *node->scroll);
  const Point offset = node->scroll->axis == Axis::Horizontal ? Point{delta, 0.0F} : Point{0.0F, delta};
  return Perform({SemanticActionKind::Scroll, offset});
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::SetScrollPercent(double horizontal, double vertical) {
  const SemanticNodeSnapshot node = Node();
  if (!node || !SupportsScroll(*node)) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  if ((node->scroll->axis == Axis::Horizontal && vertical != UIA_ScrollPatternNoScroll) ||
      (node->scroll->axis == Axis::Vertical && horizontal != UIA_ScrollPatternNoScroll)) {
    return UIA_E_INVALIDOPERATION;
  }
  const double percent = node->scroll->axis == Axis::Horizontal ? horizontal : vertical;
  if (percent == UIA_ScrollPatternNoScroll) {
    return S_OK;
  }
  if (node->scroll->maximum_offset <= 0.0F) {
    return UIA_E_INVALIDOPERATION;
  }
  if (!std::isfinite(percent) || percent < 0.0 || percent > 100.0) {
    return E_INVALIDARG;
  }
  const float target = static_cast<float>(percent / 100.0 * node->scroll->maximum_offset);
  const float delta = target - node->scroll->offset;
  const Point offset = node->scroll->axis == Axis::Horizontal ? Point{delta, 0.0F} : Point{0.0F, delta};
  return Perform({SemanticActionKind::Scroll, offset});
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_HorizontalScrollPercent(double* percent) {
  if (percent == nullptr) {
    return E_INVALIDARG;
  }
  const SemanticNodeSnapshot node = Node();
  if (!node || !SupportsScroll(*node)) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  *percent = node->scroll->axis == Axis::Horizontal ? ScrollPercent(*node->scroll) : UIA_ScrollPatternNoScroll;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_VerticalScrollPercent(double* percent) {
  if (percent == nullptr) {
    return E_INVALIDARG;
  }
  const SemanticNodeSnapshot node = Node();
  if (!node || !SupportsScroll(*node)) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  *percent = node->scroll->axis == Axis::Vertical ? ScrollPercent(*node->scroll) : UIA_ScrollPatternNoScroll;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_HorizontalViewSize(double* percent) {
  if (percent == nullptr) {
    return E_INVALIDARG;
  }
  const SemanticNodeSnapshot node = Node();
  if (!node || !SupportsScroll(*node)) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  *percent = node->scroll->axis == Axis::Horizontal ? ViewSizePercent(*node->scroll) : 100.0;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_VerticalViewSize(double* percent) {
  if (percent == nullptr) {
    return E_INVALIDARG;
  }
  const SemanticNodeSnapshot node = Node();
  if (!node || !SupportsScroll(*node)) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  *percent = node->scroll->axis == Axis::Vertical ? ViewSizePercent(*node->scroll) : 100.0;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_HorizontallyScrollable(BOOL* scrollable) {
  if (scrollable == nullptr) {
    return E_INVALIDARG;
  }
  const SemanticNodeSnapshot node = Node();
  if (!node || !SupportsScroll(*node)) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  *scrollable = node->scroll->axis == Axis::Horizontal && node->scroll->maximum_offset > 0.0F ? TRUE : FALSE;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE Win32SemanticProvider::get_VerticallyScrollable(BOOL* scrollable) {
  if (scrollable == nullptr) {
    return E_INVALIDARG;
  }
  const SemanticNodeSnapshot node = Node();
  if (!node || !SupportsScroll(*node)) {
    return node ? UIA_E_NOTSUPPORTED : UIA_E_ELEMENTNOTAVAILABLE;
  }
  *scrollable = node->scroll->axis == Axis::Vertical && node->scroll->maximum_offset > 0.0F ? TRUE : FALSE;
  return S_OK;
}

Win32Accessibility::Win32Accessibility() : state_(std::make_shared<State>()) {}

Win32Accessibility::~Win32Accessibility() {
  Reset();
}

void Win32Accessibility::SetRuntime(Runtime* runtime) noexcept {
  std::scoped_lock lock(state_->mutex);
  state_->runtime = runtime;
  state_->ui_thread = GetCurrentThreadId();
}

void Win32Accessibility::SetWindow(HWND window) noexcept {
  std::scoped_lock lock(state_->mutex);
  state_->window = window;
  state_->ui_thread = GetCurrentThreadId();
}

void Win32Accessibility::SetDpiScale(float scale) noexcept {
  std::scoped_lock lock(state_->mutex);
  state_->dpi_scale = std::isfinite(scale) && scale > 0.0F ? scale : kDefaultDpiScale;
}

void Win32Accessibility::Commit(std::shared_ptr<const SemanticFrame> frame, const Win32PlatformViews* platform_views) {
  std::unordered_map<std::uint64_t, HWND> platform_view_handles;
  if (frame && platform_views != nullptr) {
    for (const SemanticNode& node : frame->nodes) {
      if (!node.platform_view_identity.has_value()) {
        continue;
      }
      if (HWND view = platform_views->AccessibilityView(*node.platform_view_identity)) {
        platform_view_handles.emplace(*node.platform_view_identity, view);
      }
    }
  }

  std::shared_ptr<const SemanticFrame> previous;
  HWND window = nullptr;
  {
    std::scoped_lock lock(state_->mutex);
    if (state_->frame == frame && state_->platform_view_handles == platform_view_handles) {
      return;
    }
    previous = state_->frame;
    state_->frame = frame;
    state_->platform_view_handles = std::move(platform_view_handles);
    window = state_->window;
  }
  state_->RetainProviders(frame.get());
  if (!UiaClientsAreListening() || window == nullptr || !frame) {
    return;
  }

  IRawElementProviderSimple* root = nullptr;
  if (FAILED(state_->Provider(frame->root, &root))) {
    return;
  }
  if (StructureChanged(previous.get(), frame.get())) {
    static_cast<void>(UiaRaiseStructureChangedEvent(root, StructureChangeType_ChildrenInvalidated, nullptr, 0));
  } else if (LayoutChanged(previous.get(), frame.get())) {
    static_cast<void>(UiaRaiseAutomationEvent(root, UIA_LayoutInvalidatedEventId));
  }
  root->Release();

  if (!previous) {
    return;
  }
  for (const SemanticNode& node : frame->nodes) {
    const SemanticNode* old = FindNode(*previous, node.id);
    if (old == nullptr) {
      continue;
    }
    IRawElementProviderSimple* provider = nullptr;
    if (FAILED(state_->Provider(node.id, &provider))) {
      continue;
    }
    const auto raise_string = [provider](PROPERTYID property, const std::string& before, const std::string& after) {
      if (before == after) {
        return;
      }
      VARIANT old_value{};
      VARIANT new_value{};
      if (SUCCEEDED(SetStringVariant(&old_value, before)) && SUCCEEDED(SetStringVariant(&new_value, after))) {
        static_cast<void>(UiaRaiseAutomationPropertyChangedEvent(provider, property, old_value, new_value));
      }
      VariantClear(&old_value);
      VariantClear(&new_value);
    };
    const auto raise_bool = [provider](PROPERTYID property, bool before, bool after) {
      if (before == after) {
        return;
      }
      VARIANT old_value{};
      VARIANT new_value{};
      SetBoolVariant(&old_value, before);
      SetBoolVariant(&new_value, after);
      static_cast<void>(UiaRaiseAutomationPropertyChangedEvent(provider, property, old_value, new_value));
    };
    const auto raise_int = [provider](PROPERTYID property, LONG before, LONG after) {
      if (before == after) {
        return;
      }
      VARIANT old_value{};
      VARIANT new_value{};
      SetIntVariant(&old_value, before);
      SetIntVariant(&new_value, after);
      static_cast<void>(UiaRaiseAutomationPropertyChangedEvent(provider, property, old_value, new_value));
    };
    const auto raise_double = [provider](PROPERTYID property, double before, double after) {
      if (before == after) {
        return;
      }
      VARIANT old_value{};
      VARIANT new_value{};
      SetDoubleVariant(&old_value, before);
      SetDoubleVariant(&new_value, after);
      static_cast<void>(UiaRaiseAutomationPropertyChangedEvent(provider, property, old_value, new_value));
    };
    raise_string(
        UIA_NamePropertyId,
        old->label.empty() ? old->placeholder : old->label,
        node.label.empty() ? node.placeholder : node.label
    );
    raise_string(UIA_HelpTextPropertyId, old->hint, node.hint);
    raise_string(UIA_AutomationIdPropertyId, old->identifier, node.identifier);
    raise_string(
        UIA_ItemStatusPropertyId,
        !old->error.empty() ? old->error : old->state_description,
        !node.error.empty() ? node.error : node.state_description
    );
    if (!old->secure && !node.secure) {
      raise_string(UIA_ValueValuePropertyId, old->value, node.value);
    }
    raise_bool(UIA_IsEnabledPropertyId, old->enabled, node.enabled);
    raise_bool(UIA_HasKeyboardFocusPropertyId, old->focused, node.focused);
    raise_bool(UIA_IsOffscreenPropertyId, old->offscreen, node.offscreen);
    raise_bool(UIA_IsPasswordPropertyId, old->secure, node.secure);
    if (old->checked && node.checked) {
      raise_int(UIA_ToggleToggleStatePropertyId, CheckedState(*old->checked), CheckedState(*node.checked));
    }
    if (old->selected && node.selected) {
      raise_bool(UIA_SelectionItemIsSelectedPropertyId, *old->selected, *node.selected);
      if (!*old->selected && *node.selected) {
        static_cast<void>(UiaRaiseAutomationEvent(provider, UIA_SelectionItem_ElementSelectedEventId));
      }
    }
    if (old->expanded && node.expanded) {
      raise_int(UIA_ExpandCollapseExpandCollapseStatePropertyId, ExpandedState(*old), ExpandedState(node));
    }
    if (old->range && node.range) {
      raise_double(UIA_RangeValueValuePropertyId, old->range->current, node.range->current);
      raise_double(UIA_RangeValueMinimumPropertyId, old->range->minimum, node.range->minimum);
      raise_double(UIA_RangeValueMaximumPropertyId, old->range->maximum, node.range->maximum);
      const double old_small = old->range->step.value_or((old->range->maximum - old->range->minimum) * 0.01);
      const double new_small = node.range->step.value_or((node.range->maximum - node.range->minimum) * 0.01);
      const double old_large = old->range->step.value_or((old->range->maximum - old->range->minimum) * 0.1);
      const double new_large = node.range->step.value_or((node.range->maximum - node.range->minimum) * 0.1);
      raise_double(UIA_RangeValueSmallChangePropertyId, old_small, new_small);
      raise_double(UIA_RangeValueLargeChangePropertyId, old_large, new_large);
    }
    if (old->scroll && node.scroll && old->scroll->axis == node.scroll->axis) {
      const bool horizontal = node.scroll->axis == Axis::Horizontal;
      raise_double(
          horizontal ? UIA_ScrollHorizontalScrollPercentPropertyId : UIA_ScrollVerticalScrollPercentPropertyId,
          ScrollPercent(*old->scroll),
          ScrollPercent(*node.scroll)
      );
      raise_double(
          horizontal ? UIA_ScrollHorizontalViewSizePropertyId : UIA_ScrollVerticalViewSizePropertyId,
          ViewSizePercent(*old->scroll),
          ViewSizePercent(*node.scroll)
      );
      raise_bool(
          horizontal ? UIA_ScrollHorizontallyScrollablePropertyId : UIA_ScrollVerticallyScrollablePropertyId,
          old->scroll->maximum_offset > 0.0F,
          node.scroll->maximum_offset > 0.0F
      );
    }
    if (old->focused != node.focused && node.focused) {
      static_cast<void>(UiaRaiseAutomationEvent(provider, UIA_AutomationFocusChangedEventId));
    }
    raise_int(UIA_LiveSettingPropertyId, LiveRegionSetting(old->live_region), LiveRegionSetting(node.live_region));
    if (node.live_region != SemanticLiveRegion::None &&
        (old->live_region != node.live_region || old->label != node.label)) {
      static_cast<void>(UiaRaiseAutomationEvent(provider, UIA_LiveRegionChangedEventId));
    }
    provider->Release();
  }
}

void Win32Accessibility::Reset() noexcept {
  HWND window = nullptr;
  {
    std::scoped_lock lock(state_->mutex);
    window = state_->window;
    state_->runtime = nullptr;
    state_->window = nullptr;
    state_->frame.reset();
    state_->platform_view_handles.clear();
  }
  if (window != nullptr) {
    // UI Automation keeps a per-HWND provider map that must be disconnected before the HWND becomes invalid.
    static_cast<void>(UiaReturnRawElementProvider(window, 0, 0, nullptr));
  }
  state_->ReleaseProviders();
}

LRESULT Win32Accessibility::HandleGetObject(WPARAM w_param, LPARAM l_param) {
  std::shared_ptr<const SemanticFrame> frame;
  HWND window = nullptr;
  {
    std::scoped_lock lock(state_->mutex);
    frame = state_->frame;
    window = state_->window;
  }
  if (!frame || window == nullptr) {
    return 0;
  }
  IRawElementProviderSimple* provider = nullptr;
  if (FAILED(ProviderForNode(frame->root, &provider))) {
    return 0;
  }
  const LRESULT result = UiaReturnRawElementProvider(window, w_param, l_param, provider);
  provider->Release();
  return result;
}

LRESULT Win32Accessibility::HandleActionMessage(LPARAM l_param) {
  auto* request = reinterpret_cast<ActionRequest*>(l_param);
  if (request == nullptr) {
    return FALSE;
  }
  Runtime* runtime = nullptr;
  {
    std::scoped_lock lock(state_->mutex);
    runtime = state_->runtime;
  }
  request->result = runtime != nullptr && runtime->PerformSemanticAction(request->id, request->action);
  return request->result ? TRUE : FALSE;
}

HRESULT Win32Accessibility::ProviderForNode(SemanticNodeId id, IRawElementProviderSimple** provider) {
  return state_->Provider(id, provider);
}

} // namespace huxerui::detail
