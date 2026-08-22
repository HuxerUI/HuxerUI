#include "runtime_test_support.h"

namespace huxerui::test {
namespace {

class SelectionClipboard final : public PlatformClipboard {
public:
  std::optional<std::string> ReadText() override {
    return text;
  }

  bool WriteText(std::string_view value) override {
    text = std::string(value);
    return true;
  }

  std::optional<std::string> text;
};

View SelectionAreaApp() {
  return SelectionArea{
      Column{
          Text("Alpha"),
          Text("Beta"),
      },
  };
}

View FocusedSelectionAreaApp() {
  ThemeSpec spec = FlatLightThemeSpec();
  spec.interactions.focus_ring = FocusRing{Color::Rgb(40, 180, 90), 3.0F, 4.0F};
  return Theme(ThemeDefinition{spec}, SelectionAreaApp);
}

View PlainTextApp() {
  return Text("Alpha");
}

View PresentedSelectionAreaApp() {
  return SelectionArea{
      Text("Alpha").With(Offset{Point{10.0F, 0.0F}}, Opacity{0.5F}),
  };
}

bool custom_selection_requested = false;

struct CustomSelection {
  class Extension final : public NodeExtension, public TextSelectionClient {
  public:
    Extension(MountedNode&, const CustomSelection&) {}

    void Update(MountedNode&, const CustomSelection&) {}

    bool HitTest(MountedNode& node, Point position) const override {
      return node.Bounds().Contains(position);
    }

    PointerResult OnPointer(MountedNode&, const PointerEvent& event) override {
      return event.type == PointerEventType::Down ? PointerResult::Observe : PointerResult::Ignored;
    }

    TextSelectionClient* GetTextSelectionClient() noexcept override {
      return this;
    }

    bool SelectWord(Point) override {
      custom_selection_requested = true;
      return true;
    }

    bool ExtendSelection(Point, bool) override {
      return false;
    }

    bool QuerySelectionGeometry(Rect& start, Rect& end) const override {
      start = {0.0F, 0.0F, 1.0F, 16.0F};
      end = {20.0F, 0.0F, 1.0F, 16.0F};
      return custom_selection_requested;
    }

    Color SelectionHandleColor() const noexcept override {
      return Color::Black();
    }
  };

  bool operator==(const CustomSelection&) const = default;
};

View CustomSelectionApp() {
  return Text("Selectable").With(Focusable{}, CustomSelection{});
}

const RenderNode* FindSelectionPaintNode(const RenderNode* node) {
  if (!node) {
    return nullptr;
  }
  if (std::ranges::any_of(node->foreground.Commands(), [](const PaintCommand& command) {
        return std::holds_alternative<DrawRectCommand>(command);
      })) {
    return node;
  }
  for (const RenderNode* child : node->children) {
    if (const RenderNode* result = FindSelectionPaintNode(child)) {
      return result;
    }
  }
  return nullptr;
}

void Pointer(Runtime& runtime, PointerEventType type, float x, float y) {
  runtime.HandlePointerEvent({
      type,
      810,
      {x, y},
  });
}

} // namespace

TEST_CASE("TestSelectionAreaSelectsAndCopiesAcrossTextNodes") {
  TestPlatform platform;
  SelectionClipboard clipboard;
  platform.platform_clipboard = &clipboard;
  Runtime runtime{SelectionAreaApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 10.0F, 10.0F);
  Pointer(runtime, PointerEventType::Move, 40.0F, 30.0F);
  Pointer(runtime, PointerEventType::Up, 40.0F, 30.0F);

  REQUIRE(runtime.CanPerformTextEditingAction(TextEditingAction::Copy));
  REQUIRE(runtime.PerformTextEditingAction(TextEditingAction::Copy));
  REQUIRE(clipboard.text == "lpha\nBeta");

  const FlattenedScene& scene = runtime.BuildFrame();
  REQUIRE(std::ranges::any_of(scene.Commands(), [](const PaintCommand& command) {
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    return rect != nullptr && rect->color.alpha > 0.0F;
  }));
}

TEST_CASE("TestSelectionAreaHandlesSelectAllShortcut") {
  TestPlatform platform;
  SelectionClipboard clipboard;
  platform.platform_clipboard = &clipboard;
  Runtime runtime{SelectionAreaApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 10.0F, 10.0F);
  Pointer(runtime, PointerEventType::Up, 10.0F, 10.0F);
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::A,
      {},
      {.control = true},
  });
  runtime.HandleKeyEvent({
      KeyEventType::Down,
      Key::C,
      {},
      {.control = true},
  });

  REQUIRE(clipboard.text == "Alpha\nBeta");
  REQUIRE_FALSE(runtime.CanPerformTextEditingAction(TextEditingAction::SelectAll));
  REQUIRE_FALSE(runtime.CanPerformTextEditingAction(TextEditingAction::Cut));
  REQUIRE_FALSE(runtime.CanPerformTextEditingAction(TextEditingAction::Paste));
}

TEST_CASE("TestSelectionAreaDoubleClickSelectsWord") {
  TestPlatform platform;
  SelectionClipboard clipboard;
  platform.platform_clipboard = &clipboard;
  Runtime runtime{SelectionAreaApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({
      PointerEventType::Down,
      811,
      {20.0F, 10.0F},
      PointerDeviceKind::Mouse,
      2,
  });
  REQUIRE(runtime.PerformTextEditingAction(TextEditingAction::Copy));
  REQUIRE(clipboard.text == "Alpha");
}

TEST_CASE("TestSelectionAreaUsesPresentedTextGeometryAndOpacity") {
  TestPlatform platform;
  SelectionClipboard clipboard;
  platform.platform_clipboard = &clipboard;
  Runtime runtime{PresentedSelectionAreaApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 40.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({
      PointerEventType::Down,
      812,
      {20.0F, 10.0F},
      PointerDeviceKind::Mouse,
      2,
  });
  REQUIRE(runtime.PerformTextEditingAction(TextEditingAction::Copy));
  REQUIRE(clipboard.text == "Alpha");

  const FlattenedScene& scene = runtime.BuildFrame();
  const auto selection = std::ranges::find_if(scene.Commands(), [](const PaintCommand& command) {
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    return rect != nullptr && std::abs(rect->rect.x - 10.0F) < 0.01F && std::abs(rect->rect.width - 50.0F) < 0.01F;
  });
  REQUIRE(selection != scene.Commands().end());
  REQUIRE(std::abs(std::get<DrawRectCommand>(*selection).color.alpha - 0.16F) < 0.001F);
}

TEST_CASE("TestSelectionAreaRetainsForegroundPaintAcrossCleanFrames") {
  TestPlatform platform;
  Runtime runtime{SelectionAreaApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 10.0F, 10.0F);
  Pointer(runtime, PointerEventType::Move, 40.0F, 30.0F);
  Pointer(runtime, PointerEventType::Up, 40.0F, 30.0F);

  const RenderNode* selected = FindSelectionPaintNode(runtime.BuildRenderFrame().scene.root);
  REQUIRE(selected != nullptr);
  const std::uint64_t foreground_revision = selected->foreground.Revision();

  const RenderNode* retained = FindSelectionPaintNode(runtime.BuildRenderFrame().scene.root);
  REQUIRE(retained != nullptr);
  REQUIRE(retained->foreground.Revision() == foreground_revision);
}

TEST_CASE("TestSelectionAreaPaintsTheThemeFocusRingForKeyboardFocus") {
  TestPlatform platform;
  Runtime runtime{FocusedSelectionAreaApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
  runtime.BuildFrame();

  runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Tab});
  const FlattenedScene& focused = runtime.BuildFrame();
  REQUIRE(std::ranges::any_of(focused.Commands(), [](const PaintCommand& command) {
    const auto* border = std::get_if<DrawBorderCommand>(&command);
    return border != nullptr && border->color == Color::Rgb(40, 180, 90) && border->width == 3.0F;
  }));
}

TEST_CASE("TestTextSelectionCapabilityDoesNotDependOnBuiltInNodeKinds") {
  custom_selection_requested = false;
  TestPlatform platform;
  Runtime runtime{CustomSelectionApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 40.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({
      PointerEventType::Down,
      813,
      {20.0F, 10.0F},
      PointerDeviceKind::Mouse,
      2,
  });

  REQUIRE(custom_selection_requested);
}

TEST_CASE("TestTextRemainsNonSelectableOutsideSelectionArea") {
  TestPlatform platform;
  SelectionClipboard clipboard;
  platform.platform_clipboard = &clipboard;
  Runtime runtime{PlainTextApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 40.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 10.0F, 10.0F);
  Pointer(runtime, PointerEventType::Up, 40.0F, 10.0F);

  REQUIRE_FALSE(runtime.CanPerformTextEditingAction(TextEditingAction::Copy));
  REQUIRE_FALSE(runtime.PerformTextEditingAction(TextEditingAction::Copy));
}

} // namespace huxerui::test
