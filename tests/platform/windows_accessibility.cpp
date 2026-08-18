#include "runtime_test_support.h"

#include <wrl/client.h>

#include <memory>
#include <ranges>
#include <string>

#include "win32_accessibility.h"

namespace huxerui::test {

namespace {

int accessibility_clicks = 0;
State<float> accessibility_slider_value;
State<TextEditingValue> accessibility_text_value;

View WindowsAccessibilityApp() {
  auto slider = UseState(2.0F);
  auto text = UseState(TextEditingValue::FromText("initial"));
  accessibility_slider_value = slider;
  accessibility_text_value = text;
  return Column{
      Button("Run").OnClick([] { ++accessibility_clicks; }),
      Slider(slider.Get()).Range(0.0F, 10.0F).Step(0.5F).OnChanged([slider](float value) mutable { slider = value; }),
      TextField(text).Label("Editor").OnChanged([text](const TextEditingValue& value) mutable { text = value; }),
  };
}

const SemanticNode& FindNode(const SemanticFrame& frame, SemanticRole role) {
  const auto found = std::ranges::find(frame.nodes, role, &SemanticNode::role);
  REQUIRE(found != frame.nodes.end());
  return *found;
}

std::wstring PropertyString(IRawElementProviderSimple& provider, PROPERTYID property) {
  VARIANT value{};
  REQUIRE(provider.GetPropertyValue(property, &value) == S_OK);
  REQUIRE(value.vt == VT_BSTR);
  const std::wstring result(value.bstrVal, SysStringLen(value.bstrVal));
  VariantClear(&value);
  return result;
}

} // namespace

TEST_CASE("Windows accessibility maps semantic properties and stable fragments") {
  TestPlatform platform;
  Runtime runtime(WindowsAccessibilityApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
  REQUIRE(frame);

  detail::Win32Accessibility accessibility;
  accessibility.SetRuntime(&runtime.NativeRuntime());
  accessibility.Commit(frame, nullptr);

  const SemanticNode& button_node = FindNode(*frame, SemanticRole::Button);
  Microsoft::WRL::ComPtr<IRawElementProviderSimple> button;
  REQUIRE(accessibility.ProviderForNode(button_node.id, &button) == S_OK);
  REQUIRE(PropertyString(*button.Get(), UIA_NamePropertyId) == L"Run");
  REQUIRE(PropertyString(*button.Get(), UIA_FrameworkIdPropertyId) == L"HuxerUI");

  VARIANT control_type{};
  REQUIRE(button->GetPropertyValue(UIA_ControlTypePropertyId, &control_type) == S_OK);
  REQUIRE(control_type.vt == VT_I4);
  REQUIRE(control_type.lVal == UIA_ButtonControlTypeId);

  Microsoft::WRL::ComPtr<IRawElementProviderSimple> same_button;
  REQUIRE(accessibility.ProviderForNode(button_node.id, &same_button) == S_OK);
  REQUIRE(button.Get() == same_button.Get());

  Microsoft::WRL::ComPtr<IRawElementProviderFragment> root;
  Microsoft::WRL::ComPtr<IRawElementProviderSimple> root_simple;
  REQUIRE(accessibility.ProviderForNode(frame->root, &root_simple) == S_OK);
  REQUIRE(root_simple.As(&root) == S_OK);
  SAFEARRAY* root_runtime_id = nullptr;
  REQUIRE(root->GetRuntimeId(&root_runtime_id) == S_OK);
  REQUIRE(root_runtime_id == nullptr);
  Microsoft::WRL::ComPtr<IRawElementProviderFragment> first_child;
  REQUIRE(root->Navigate(NavigateDirection_FirstChild, &first_child) == S_OK);
  REQUIRE(first_child);

  SAFEARRAY* runtime_id = nullptr;
  REQUIRE(first_child->GetRuntimeId(&runtime_id) == S_OK);
  REQUIRE(runtime_id != nullptr);
  REQUIRE(runtime_id->rgsabound[0].cElements == 3);
  SafeArrayDestroy(runtime_id);
}

TEST_CASE("Windows accessibility patterns route actions through Runtime") {
  accessibility_clicks = 0;
  TestPlatform platform;
  Runtime runtime(WindowsAccessibilityApp, platform);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
  REQUIRE(frame);

  detail::Win32Accessibility accessibility;
  accessibility.SetRuntime(&runtime.NativeRuntime());
  accessibility.Commit(frame, nullptr);

  const SemanticNode& button_node = FindNode(*frame, SemanticRole::Button);
  Microsoft::WRL::ComPtr<IRawElementProviderSimple> button;
  REQUIRE(accessibility.ProviderForNode(button_node.id, &button) == S_OK);
  Microsoft::WRL::ComPtr<IInvokeProvider> invoke;
  REQUIRE(button.As(&invoke) == S_OK);
  REQUIRE(invoke->Invoke() == S_OK);
  REQUIRE(accessibility_clicks == 1);

  const SemanticNode& slider_node = FindNode(*frame, SemanticRole::Slider);
  Microsoft::WRL::ComPtr<IRawElementProviderSimple> slider;
  REQUIRE(accessibility.ProviderForNode(slider_node.id, &slider) == S_OK);
  Microsoft::WRL::ComPtr<IRangeValueProvider> range;
  REQUIRE(slider.As(&range) == S_OK);
  double minimum = 0.0;
  double maximum = 0.0;
  double step = 0.0;
  REQUIRE(range->get_Minimum(&minimum) == S_OK);
  REQUIRE(range->get_Maximum(&maximum) == S_OK);
  REQUIRE(range->get_SmallChange(&step) == S_OK);
  REQUIRE(minimum == 0.0);
  REQUIRE(maximum == 10.0);
  REQUIRE(step == 0.5);
  REQUIRE(range->SetValue(7.5) == S_OK);
  REQUIRE(accessibility_slider_value.Get() == 7.5F);

  const SemanticNode& field_node = FindNode(*frame, SemanticRole::TextField);
  Microsoft::WRL::ComPtr<IRawElementProviderSimple> field;
  REQUIRE(accessibility.ProviderForNode(field_node.id, &field) == S_OK);
  Microsoft::WRL::ComPtr<IValueProvider> value;
  REQUIRE(field.As(&value) == S_OK);
  REQUIRE(value->SetValue(L"updated") == S_OK);
  REQUIRE(accessibility_text_value.Get().text == "updated");

  const std::shared_ptr<const SemanticFrame> updated = runtime.BuildCommit().semantic_frame;
  accessibility.Commit(updated, nullptr);
  double current = 0.0;
  REQUIRE(range->get_Value(&current) == S_OK);
  REQUIRE(current == 7.5);
  BSTR text = nullptr;
  REQUIRE(value->get_Value(&text) == S_OK);
  REQUIRE(std::wstring(text, SysStringLen(text)) == L"updated");
  SysFreeString(text);

  accessibility.Reset();
  VARIANT unavailable{};
  REQUIRE(button->GetPropertyValue(UIA_NamePropertyId, &unavailable) == UIA_E_ELEMENTNOTAVAILABLE);
}

TEST_CASE("Windows accessibility advertises only committed semantic patterns") {
  auto frame = std::make_shared<SemanticFrame>();
  frame->revision = 1;
  frame->root = 1;
  frame->nodes = {
      SemanticNode{.id = 1, .children = {2, 3, 4, 5, 6, 7, 8}, .bounds = {0.0F, 0.0F, 320.0F, 240.0F}},
      SemanticNode{
          .id = 2,
          .parent = 1,
          .role = SemanticRole::Checkbox,
          .label = "Checked",
          .checked = SemanticCheckedState::Mixed,
          .actions = SemanticActionMask(SemanticActionKind::Activate),
      },
      SemanticNode{
          .id = 3,
          .parent = 1,
          .role = SemanticRole::TextField,
          .label = "Password",
          .value = "secret",
          .read_only = false,
          .secure = true,
          .actions = SemanticActionMask(SemanticActionKind::SetText),
      },
      SemanticNode{
          .id = 4,
          .parent = 1,
          .role = SemanticRole::Tab,
          .label = "Library",
          .selected = true,
          .actions = SemanticActionMask(SemanticActionKind::Activate),
      },
      SemanticNode{
          .id = 5,
          .parent = 1,
          .role = SemanticRole::MenuItem,
          .label = "Move to",
          .expanded = false,
          .actions = SemanticActionMask(SemanticActionKind::Expand),
      },
      SemanticNode{
          .id = 6,
          .parent = 1,
          .role = SemanticRole::ScrollView,
          .label = "Results",
          .scroll = ScrollMetrics{Axis::Vertical, 25.0F, 100.0F, 50.0F, 150.0F},
          .actions = SemanticActionMask(SemanticActionKind::Scroll),
      },
      SemanticNode{
          .id = 7,
          .parent = 1,
          .role = SemanticRole::ScrollView,
          .label = "Static results",
          .scroll = ScrollMetrics{Axis::Vertical, 0.0F, 0.0F, 50.0F, 50.0F},
          .actions = SemanticActionMask(SemanticActionKind::Scroll),
      },
      SemanticNode{
          .id = 8,
          .parent = 1,
          .role = SemanticRole::Tab,
          .label = "Settings",
          .selected = false,
          .actions = SemanticActionMask(SemanticActionKind::Activate),
      },
  };

  detail::Win32Accessibility accessibility;
  accessibility.Commit(frame, nullptr);

  Microsoft::WRL::ComPtr<IRawElementProviderSimple> checkbox;
  REQUIRE(accessibility.ProviderForNode(2, &checkbox) == S_OK);
  Microsoft::WRL::ComPtr<IToggleProvider> toggle;
  REQUIRE(checkbox.As(&toggle) == S_OK);
  ToggleState toggle_state = ToggleState_Off;
  REQUIRE(toggle->get_ToggleState(&toggle_state) == S_OK);
  REQUIRE(toggle_state == ToggleState_Indeterminate);

  Microsoft::WRL::ComPtr<IRawElementProviderSimple> field;
  REQUIRE(accessibility.ProviderForNode(3, &field) == S_OK);
  Microsoft::WRL::ComPtr<IValueProvider> value;
  REQUIRE(field.As(&value) == S_OK);
  BSTR field_value = nullptr;
  REQUIRE(value->get_Value(&field_value) == UIA_E_INVALIDOPERATION);
  REQUIRE(field_value == nullptr);

  Microsoft::WRL::ComPtr<IRawElementProviderSimple> tab;
  REQUIRE(accessibility.ProviderForNode(4, &tab) == S_OK);
  Microsoft::WRL::ComPtr<ISelectionItemProvider> selection_item;
  REQUIRE(tab.As(&selection_item) == S_OK);
  BOOL selected = FALSE;
  REQUIRE(selection_item->get_IsSelected(&selected) == S_OK);
  REQUIRE(selected == TRUE);
  REQUIRE(selection_item->Select() == S_OK);

  Microsoft::WRL::ComPtr<IRawElementProviderSimple> unselected_tab;
  REQUIRE(accessibility.ProviderForNode(8, &unselected_tab) == S_OK);
  Microsoft::WRL::ComPtr<ISelectionItemProvider> unselected_item;
  REQUIRE(unselected_tab.As(&unselected_item) == S_OK);
  REQUIRE(unselected_item->Select() == UIA_E_INVALIDOPERATION);
  REQUIRE(unselected_item->AddToSelection() == UIA_E_INVALIDOPERATION);

  Microsoft::WRL::ComPtr<IRawElementProviderSimple> menu_item;
  REQUIRE(accessibility.ProviderForNode(5, &menu_item) == S_OK);
  Microsoft::WRL::ComPtr<IExpandCollapseProvider> expand;
  REQUIRE(menu_item.As(&expand) == S_OK);
  ExpandCollapseState expanded = ExpandCollapseState_LeafNode;
  REQUIRE(expand->get_ExpandCollapseState(&expanded) == S_OK);
  REQUIRE(expanded == ExpandCollapseState_Collapsed);

  Microsoft::WRL::ComPtr<IRawElementProviderSimple> scroll_view;
  REQUIRE(accessibility.ProviderForNode(6, &scroll_view) == S_OK);
  Microsoft::WRL::ComPtr<IScrollProvider> scroll;
  REQUIRE(scroll_view.As(&scroll) == S_OK);
  double vertical_percent = 0.0;
  double vertical_view_size = 0.0;
  REQUIRE(scroll->get_VerticalScrollPercent(&vertical_percent) == S_OK);
  REQUIRE(scroll->get_VerticalViewSize(&vertical_view_size) == S_OK);
  REQUIRE(vertical_percent == 25.0);
  REQUIRE(vertical_view_size == Catch::Approx(100.0 / 3.0));

  Microsoft::WRL::ComPtr<IRawElementProviderSimple> static_scroll_view;
  REQUIRE(accessibility.ProviderForNode(7, &static_scroll_view) == S_OK);
  Microsoft::WRL::ComPtr<IScrollProvider> static_scroll;
  REQUIRE(static_scroll_view.As(&static_scroll) == S_OK);
  REQUIRE(static_scroll->SetScrollPercent(UIA_ScrollPatternNoScroll, 50.0) == UIA_E_INVALIDOPERATION);

  Microsoft::WRL::ComPtr<IRawElementProviderSimple> root_simple;
  REQUIRE(accessibility.ProviderForNode(1, &root_simple) == S_OK);
  Microsoft::WRL::ComPtr<IRawElementProviderFragmentRoot> root;
  REQUIRE(root_simple.As(&root) == S_OK);
  Microsoft::WRL::ComPtr<IRawElementProviderFragment> outside;
  REQUIRE(root->ElementProviderFromPoint(400.0, 300.0, &outside) == S_OK);
  REQUIRE_FALSE(outside);
}

TEST_CASE("Windows accessibility keeps COM interfaces static while replacing changed provider shapes") {
  auto invoke_frame = std::make_shared<SemanticFrame>();
  invoke_frame->revision = 1;
  invoke_frame->root = 1;
  invoke_frame->nodes = {
      SemanticNode{.id = 1, .children = {2}},
      SemanticNode{
          .id = 2,
          .parent = 1,
          .role = SemanticRole::Button,
          .label = "Action",
          .actions = SemanticActionMask(SemanticActionKind::Activate),
      },
  };

  detail::Win32Accessibility accessibility;
  accessibility.Commit(invoke_frame, nullptr);

  Microsoft::WRL::ComPtr<IRawElementProviderSimple> original;
  REQUIRE(accessibility.ProviderForNode(2, &original) == S_OK);
  Microsoft::WRL::ComPtr<IInvokeProvider> invoke;
  REQUIRE(original.As(&invoke) == S_OK);
  Microsoft::WRL::ComPtr<IToggleProvider> original_toggle;
  REQUIRE(original.As(&original_toggle) == E_NOINTERFACE);

  auto toggle_frame = std::make_shared<SemanticFrame>(*invoke_frame);
  toggle_frame->revision = 2;
  toggle_frame->nodes[1].role = SemanticRole::Checkbox;
  toggle_frame->nodes[1].checked = SemanticCheckedState::Unchecked;
  accessibility.Commit(toggle_frame, nullptr);

  Microsoft::WRL::ComPtr<IInvokeProvider> retained_invoke;
  REQUIRE(original.As(&retained_invoke) == S_OK);
  Microsoft::WRL::ComPtr<IToggleProvider> retained_toggle;
  REQUIRE(original.As(&retained_toggle) == E_NOINTERFACE);

  Microsoft::WRL::ComPtr<IRawElementProviderSimple> replacement;
  REQUIRE(accessibility.ProviderForNode(2, &replacement) == S_OK);
  REQUIRE(replacement.Get() != original.Get());
  Microsoft::WRL::ComPtr<IInvokeProvider> replacement_invoke;
  REQUIRE(replacement.As(&replacement_invoke) == E_NOINTERFACE);
  Microsoft::WRL::ComPtr<IToggleProvider> replacement_toggle;
  REQUIRE(replacement.As(&replacement_toggle) == S_OK);

  auto removed_frame = std::make_shared<SemanticFrame>();
  removed_frame->revision = 3;
  removed_frame->root = 1;
  removed_frame->nodes = {SemanticNode{.id = 1}};
  accessibility.Commit(removed_frame, nullptr);

  Microsoft::WRL::ComPtr<IUnknown> retained_unknown;
  REQUIRE(original.As(&retained_unknown) == S_OK);
  VARIANT unavailable{};
  REQUIRE(original->GetPropertyValue(UIA_NamePropertyId, &unavailable) == UIA_E_ELEMENTNOTAVAILABLE);
  Microsoft::WRL::ComPtr<IRawElementProviderSimple> removed;
  REQUIRE(accessibility.ProviderForNode(2, &removed) == UIA_E_ELEMENTNOTAVAILABLE);
}

} // namespace huxerui::test
