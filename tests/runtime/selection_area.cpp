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

View PlainTextApp() {
  return Text("Alpha");
}

View PresentedSelectionAreaApp() {
  return SelectionArea{
      Text("Alpha").With(Offset{Point{10.0F, 0.0F}}, Opacity{0.5F}),
  };
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
  runtime.SetViewport({160.0F, 80.0F});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 10.0F, 10.0F);
  Pointer(runtime, PointerEventType::Move, 40.0F, 30.0F);
  Pointer(runtime, PointerEventType::Up, 40.0F, 30.0F);

  REQUIRE(runtime.CanPerformTextEditingAction(TextEditingAction::Copy));
  REQUIRE(runtime.PerformTextEditingAction(TextEditingAction::Copy));
  REQUIRE(clipboard.text == "lpha\nBeta");

  const DisplayList& display_list = runtime.BuildFrame();
  REQUIRE(std::ranges::any_of(display_list.Commands(), [](const DisplayCommand& command) {
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    return rect != nullptr && rect->color.alpha > 0.0F;
  }));
}

TEST_CASE("TestSelectionAreaHandlesSelectAllShortcut") {
  TestPlatform platform;
  SelectionClipboard clipboard;
  platform.platform_clipboard = &clipboard;
  Runtime runtime{SelectionAreaApp, platform};
  runtime.SetViewport({160.0F, 80.0F});
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
  runtime.SetViewport({160.0F, 80.0F});
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
  runtime.SetViewport({160.0F, 40.0F});
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

  const DisplayList& display_list = runtime.BuildFrame();
  const auto selection = std::ranges::find_if(display_list.Commands(), [](const DisplayCommand& command) {
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    return rect != nullptr && std::abs(rect->rect.x - 10.0F) < 0.01F && std::abs(rect->rect.width - 50.0F) < 0.01F;
  });
  REQUIRE(selection != display_list.Commands().end());
  REQUIRE(std::abs(std::get<DrawRectCommand>(*selection).color.alpha - 0.16F) < 0.001F);
}

TEST_CASE("TestTextRemainsNonSelectableOutsideSelectionArea") {
  TestPlatform platform;
  SelectionClipboard clipboard;
  platform.platform_clipboard = &clipboard;
  Runtime runtime{PlainTextApp, platform};
  runtime.SetViewport({160.0F, 40.0F});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 10.0F, 10.0F);
  Pointer(runtime, PointerEventType::Up, 40.0F, 10.0F);

  REQUIRE_FALSE(runtime.CanPerformTextEditingAction(TextEditingAction::Copy));
  REQUIRE_FALSE(runtime.PerformTextEditingAction(TextEditingAction::Copy));
}

} // namespace huxerui::test
