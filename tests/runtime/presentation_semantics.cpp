#include "runtime_test_support.h"

#include <algorithm>
#include <optional>
#include <string_view>

namespace huxerui::test {

namespace {

std::optional<DialogHandle> semantic_dialog;
std::optional<BottomSheetHandle> semantic_bottom_sheet;
std::optional<ToastHandle> semantic_toast;
std::optional<MenuHandle> semantic_menu;
int semantic_dismiss_requests = 0;

View PresentationSemanticsApp() {
  semantic_dialog = UseDialog();
  semantic_bottom_sheet = UseBottomSheet();
  semantic_toast = UseToast();
  semantic_menu = UseMenu();
  return Column{
      Button("menu anchor").With(huxerui::Frame{120.0F, 36.0F}, semantic_menu->Anchor()),
      Text("application semantics"),
  };
}

const SemanticNode* FindSemanticNode(const SemanticFrame& frame, std::string_view label) {
  const auto found =
      std::ranges::find_if(frame.nodes, [label](const SemanticNode& node) { return node.label == label; });
  return found == frame.nodes.end() ? nullptr : &*found;
}

const SemanticNode* FindSemanticNode(const SemanticFrame& frame, SemanticRole role) {
  const auto found = std::ranges::find(frame.nodes, role, &SemanticNode::role);
  return found == frame.nodes.end() ? nullptr : &*found;
}

std::size_t CountSemanticNodes(const SemanticFrame& frame, SemanticRole role) {
  return static_cast<std::size_t>(std::ranges::count(frame.nodes, role, &SemanticNode::role));
}

bool HasAction(const SemanticNode& node, SemanticActionKind action) {
  return (node.actions & SemanticActionMask(action)) != 0;
}

const SemanticFrame& SemanticFrameFrom(const FrameCommit& commit) {
  REQUIRE(commit.semantic_frame != nullptr);
  return *commit.semantic_frame;
}

void ResetPresentationSemantics() {
  semantic_dialog.reset();
  semantic_bottom_sheet.reset();
  semantic_toast.reset();
  semantic_menu.reset();
  semantic_dismiss_requests = 0;
}

} // namespace

TEST_CASE("Dialog semantics isolate the application and honor dismiss requests") {
  ResetPresentationSemantics();
  TestPlatform platform;
  Runtime runtime{PresentationSemanticsApp, platform};
  runtime.SetViewport({360.0F, 240.0F});
  runtime.BuildFrame();

  semantic_dialog->Show(
      [] { return Text("dialog body"); },
      DialogOptions{
          .on_dismiss_request = [] { ++semantic_dismiss_requests; },
      }
  );
  const SemanticFrame& shown = SemanticFrameFrom(runtime.BuildCommit());
  const SemanticNode* dialog = FindSemanticNode(shown, SemanticRole::Dialog);
  REQUIRE(dialog != nullptr);
  REQUIRE(HasAction(*dialog, SemanticActionKind::Dismiss));
  REQUIRE(FindSemanticNode(shown, "dialog body") != nullptr);
  REQUIRE(FindSemanticNode(shown, "application semantics") == nullptr);

  semantic_toast->Show("dialog announcement", ToastOptions{10.0});
  const SemanticFrame& announced = SemanticFrameFrom(runtime.BuildCommit());
  dialog = FindSemanticNode(announced, SemanticRole::Dialog);
  REQUIRE(dialog != nullptr);
  REQUIRE(FindSemanticNode(announced, "dialog announcement") != nullptr);
  REQUIRE(FindSemanticNode(announced, "application semantics") == nullptr);

  REQUIRE(
      runtime.NativeRuntime().PerformSemanticAction(dialog->id, SemanticAction{.kind = SemanticActionKind::Dismiss})
  );
  REQUIRE(semantic_dismiss_requests == 1);
  const SemanticFrame& requested = SemanticFrameFrom(runtime.BuildCommit());
  REQUIRE(FindSemanticNode(requested, SemanticRole::Dialog) != nullptr);
  REQUIRE(FindSemanticNode(requested, "application semantics") == nullptr);
}

TEST_CASE("Exiting dialogs immediately leave input and semantic participation") {
  ResetPresentationSemantics();
  TestPlatform platform;
  Runtime runtime{PresentationSemanticsApp, platform};
  runtime.SetViewport({360.0F, 240.0F});
  runtime.BuildFrame();

  semantic_dialog->Show([] { return Text("exiting dialog"); });
  const SemanticFrame& shown = SemanticFrameFrom(runtime.BuildCommit());
  const SemanticNode* dialog = FindSemanticNode(shown, SemanticRole::Dialog);
  REQUIRE(dialog != nullptr);
  REQUIRE(HasAction(*dialog, SemanticActionKind::Dismiss));
  REQUIRE(
      runtime.NativeRuntime().PerformSemanticAction(dialog->id, SemanticAction{.kind = SemanticActionKind::Dismiss})
  );

  const SemanticFrame& exiting = SemanticFrameFrom(runtime.BuildCommit());
  REQUIRE(FindSemanticNode(exiting, SemanticRole::Dialog) == nullptr);
  REQUIRE(FindSemanticNode(exiting, "exiting dialog") == nullptr);
  REQUIRE(FindSemanticNode(exiting, "application semantics") != nullptr);

  platform.AdvanceTime(0.5);
  // Exit completion removes the controller entry during this commit, while the mounted snapshot remains until the
  // next commit. Its semantics must continue to reflect the snapshot that produced the retained visual node.
  const SemanticFrame& completed = SemanticFrameFrom(runtime.BuildCommit());
  REQUIRE(FindSemanticNode(completed, SemanticRole::Dialog) == nullptr);
  REQUIRE(FindSemanticNode(completed, "exiting dialog") == nullptr);
  REQUIRE(FindSemanticNode(completed, "application semantics") != nullptr);
}

TEST_CASE("Dialog semantics follow the existing cancellation policy") {
  ResetPresentationSemantics();
  TestPlatform platform;
  Runtime runtime{PresentationSemanticsApp, platform};
  runtime.SetViewport({360.0F, 240.0F});
  runtime.BuildFrame();

  semantic_dialog->Show(
      [] { return Text("non-dismissible dialog"); },
      DialogOptions{
          .dismiss_on_outside_press = false,
          .dismiss_on_cancel = false,
      }
  );
  const SemanticFrame& frame = SemanticFrameFrom(runtime.BuildCommit());
  const SemanticNode* dialog = FindSemanticNode(frame, SemanticRole::Dialog);
  REQUIRE(dialog != nullptr);
  REQUIRE_FALSE(HasAction(*dialog, SemanticActionKind::Dismiss));
}

TEST_CASE("Standard dialog actions remain real semantic buttons") {
  ResetPresentationSemantics();
  TestPlatform platform;
  Runtime runtime{PresentationSemanticsApp, platform};
  runtime.SetViewport({360.0F, 240.0F});
  runtime.BuildFrame();

  semantic_dialog->Show("Save changes?", "The document has unsaved changes.", "Save");
  const SemanticFrame& frame = SemanticFrameFrom(runtime.BuildCommit());
  const SemanticNode* action = FindSemanticNode(frame, "Save");
  REQUIRE(action != nullptr);
  REQUIRE(action->role == SemanticRole::Button);
  REQUIRE(HasAction(*action, SemanticActionKind::Activate));
}

TEST_CASE("Bottom sheets and toasts publish presentation semantics") {
  ResetPresentationSemantics();
  TestPlatform platform;
  Runtime runtime{PresentationSemanticsApp, platform};
  runtime.SetViewport({360.0F, 240.0F});
  runtime.BuildFrame();

  const LayerId sheet_id = semantic_bottom_sheet->Show([] { return Text("sheet body"); });
  const SemanticFrame& sheet_frame = SemanticFrameFrom(runtime.BuildCommit());
  const SemanticNode* sheet = FindSemanticNode(sheet_frame, SemanticRole::Dialog);
  REQUIRE(sheet != nullptr);
  REQUIRE(HasAction(*sheet, SemanticActionKind::Dismiss));
  REQUIRE(FindSemanticNode(sheet_frame, "sheet body") != nullptr);
  REQUIRE(FindSemanticNode(sheet_frame, "application semantics") == nullptr);

  REQUIRE(semantic_bottom_sheet->Dismiss(sheet_id));
  runtime.BuildCommit();
  semantic_toast->Show("changes saved", ToastOptions{10.0});
  const SemanticFrame& toast_frame = SemanticFrameFrom(runtime.BuildCommit());
  const SemanticNode* toast = FindSemanticNode(toast_frame, "changes saved");
  REQUIRE(toast != nullptr);
  REQUIRE(toast->live_region == SemanticLiveRegion::Polite);
  REQUIRE(toast->actions == 0);
  REQUIRE(FindSemanticNode(toast_frame, "application semantics") != nullptr);
}

TEST_CASE("Menu semantics describe items and keep an expanded submenu in one modal group") {
  ResetPresentationSemantics();
  TestPlatform platform;
  Runtime runtime{PresentationSemanticsApp, platform};
  runtime.SetViewport({480.0F, 320.0F});
  runtime.BuildFrame();

  semantic_menu->Show({
      MenuItem("Checked", [] {}).Checked(true),
      MenuItem("Unchecked", [] {}).Checked(false),
      MenuSection{},
      MenuItem("Disabled", [] {}).Enabled(false),
      MenuItem(
          "More",
          {
              MenuItem("Child action", [] {}),
          }
      ),
  });
  const SemanticFrame& root_menu_frame = SemanticFrameFrom(runtime.BuildCommit());
  const SemanticNode* menu = FindSemanticNode(root_menu_frame, SemanticRole::Menu);
  REQUIRE(menu != nullptr);
  REQUIRE(menu->collection.has_value());
  REQUIRE(menu->collection->item_count == 4);
  REQUIRE(HasAction(*menu, SemanticActionKind::Dismiss));
  REQUIRE(FindSemanticNode(root_menu_frame, "application semantics") == nullptr);

  const SemanticNode* checked = FindSemanticNode(root_menu_frame, "Checked");
  const SemanticNode* unchecked = FindSemanticNode(root_menu_frame, "Unchecked");
  const SemanticNode* disabled = FindSemanticNode(root_menu_frame, "Disabled");
  const SemanticNode* more = FindSemanticNode(root_menu_frame, "More");
  REQUIRE(checked != nullptr);
  REQUIRE(unchecked != nullptr);
  REQUIRE(disabled != nullptr);
  REQUIRE(more != nullptr);
  REQUIRE(checked->role == SemanticRole::MenuItem);
  REQUIRE(checked->checked == SemanticCheckedState::Checked);
  REQUIRE(checked->collection_item.has_value());
  REQUIRE(checked->collection_item->index == 0);
  REQUIRE(unchecked->checked == SemanticCheckedState::Unchecked);
  REQUIRE(unchecked->collection_item.has_value());
  REQUIRE(unchecked->collection_item->index == 1);
  REQUIRE_FALSE(HasAction(*disabled, SemanticActionKind::Activate));
  REQUIRE(more->expanded == false);
  REQUIRE(HasAction(*more, SemanticActionKind::Expand));

  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(more->id, SemanticAction{.kind = SemanticActionKind::Expand}));
  const SemanticFrame& submenu_frame = SemanticFrameFrom(runtime.BuildCommit());
  REQUIRE(CountSemanticNodes(submenu_frame, SemanticRole::Menu) == 2);
  REQUIRE(FindSemanticNode(submenu_frame, "Checked") != nullptr);
  REQUIRE(FindSemanticNode(submenu_frame, "Child action") != nullptr);
  more = FindSemanticNode(submenu_frame, "More");
  REQUIRE(more != nullptr);
  REQUIRE(more->expanded == true);
  REQUIRE(HasAction(*more, SemanticActionKind::Collapse));

  REQUIRE(
      runtime.NativeRuntime().PerformSemanticAction(more->id, SemanticAction{.kind = SemanticActionKind::Collapse})
  );
  const SemanticFrame& collapsed_frame = SemanticFrameFrom(runtime.BuildCommit());
  REQUIRE(CountSemanticNodes(collapsed_frame, SemanticRole::Menu) == 1);
  REQUIRE(FindSemanticNode(collapsed_frame, "Child action") == nullptr);
  more = FindSemanticNode(collapsed_frame, "More");
  REQUIRE(more != nullptr);
  REQUIRE(more->expanded == false);
}

} // namespace huxerui::test
