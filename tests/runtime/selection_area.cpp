#include "runtime_test_support.h"

namespace huxerui::test {
namespace {

std::function<View()> selection_test_root;
View DynamicSelectionApp() { return selection_test_root(); }

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

class SelectionSource final : public TextSelectionSource {
public:
  explicit SelectionSource(std::size_t count, std::string tail = "line")
      : count_(count), tail_(std::move(tail)) {}

  std::size_t Count() const noexcept override { return count_; }
  TextBlockId IdAt(std::size_t index) const override { return 100 + index; }
  std::optional<std::size_t> IndexOf(TextBlockId id) const override {
    return id >= 100 && id - 100 < count_ ? std::optional<std::size_t>(id - 100) : std::nullopt;
  }
  TextSelectionBlock BlockAt(std::size_t index) const override {
    ++reads;
    return {index + 1 == count_ ? tail_ : line_, "\n"};
  }

  mutable std::size_t reads = 0;

private:
  std::size_t count_;
  AttributedText line_{"line"};
  AttributedText tail_;
};

TextSelectionClient* SelectionClient(const detail::MountedNode* node) {
  if (!node) {
    return nullptr;
  }
  for (const auto& extension : node->extensions) {
    if (extension.extension) {
      if (auto* client = extension.extension->GetTextSelectionClient()) {
        return client;
      }
    }
  }
  for (const auto& child : node->children) {
    if (auto* client = SelectionClient(child.get())) {
      return client;
    }
  }
  return nullptr;
}

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
  return Theme {ThemeDefinition{spec}, SelectionAreaApp()};
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
    Extension(ViewNode&, const CustomSelection&) {}

    void Update(ViewNode&, const CustomSelection&) {}

    bool HitTest(ViewNode& node, Point position) const override {
      return node.Bounds().Contains(position);
    }

    PointerResult OnPointer(ViewNode&, const PointerEvent& event) override {
      return event.type == PointerEventType::Down ? PointerResult::Observe : PointerResult::Ignored;
    }

    TextSelectionClient* GetTextSelectionClient() noexcept override {
      return this;
    }

    bool ClearSelection() override {
      return std::exchange(custom_selection_requested, false);
    }

    bool SelectWord(Point) override {
      custom_selection_requested = true;
      return true;
    }

    bool ExtendSelection(Point, bool) override {
      return false;
    }

    std::optional<TextSelectionGeometry> QuerySelectionGeometry() const override {
      if (!custom_selection_requested) {
        return std::nullopt;
      }
      const Rect start{0.0F, 0.0F, 1.0F, 16.0F};
      const Rect end{20.0F, 0.0F, 1.0F, 16.0F};
      return TextSelectionGeometry{start, end, start};
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

void Touch(Runtime& runtime, PointerEventType type, Point position) {
  runtime.HandlePointerEvent({type, 850, position, PointerDeviceKind::Touch});
}

void SelectByTouch(Runtime& runtime, TestPlatform& platform, Point position) {
  Touch(runtime, PointerEventType::Down, position);
  Touch(runtime, PointerEventType::Up, position);
  platform.AdvanceTime(0.1);
  Touch(runtime, PointerEventType::Down, position);
  Touch(runtime, PointerEventType::Up, position);
  REQUIRE(FindText(runtime.BuildFrame(), "Copy") != nullptr);
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
    const Color* color = rect != nullptr ? SolidBrushColor(rect->brush) : nullptr;
    return color != nullptr && color->alpha > 0.0F;
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

  ClickAt(runtime, {20.0F, 10.0F}, 810);
  platform.AdvanceTime(0.2);
  runtime.HandlePointerEvent({
      PointerEventType::Down,
      811,
      {20.0F, 10.0F},
      PointerDeviceKind::Mouse,
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

  ClickAt(runtime, {20.0F, 10.0F}, 811);
  platform.AdvanceTime(0.2);
  runtime.HandlePointerEvent({
      PointerEventType::Down,
      812,
      {20.0F, 10.0F},
      PointerDeviceKind::Mouse,
  });
  REQUIRE(runtime.PerformTextEditingAction(TextEditingAction::Copy));
  REQUIRE(clipboard.text == "Alpha");

  const FlattenedScene& scene = runtime.BuildFrame();
  const auto selection = std::ranges::find_if(scene.Commands(), [](const PaintCommand& command) {
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    const Color* color = rect ? SolidBrushColor(rect->brush) : nullptr;
    return color && std::abs(rect->rect.width - 50.0F) < 0.01F && std::abs(color->alpha - 0.16F) < 0.001F;
  });
  REQUIRE(selection != scene.Commands().end());
  const Color* selection_color = SolidBrushColor(std::get<DrawRectCommand>(*selection).brush);
  REQUIRE(selection_color != nullptr);
  REQUIRE(std::abs(selection_color->alpha - 0.16F) < 0.001F);
  const auto bounds = FindPresentedRectWithColor(scene, *selection_color);
  REQUIRE(bounds.has_value());
  REQUIRE(std::abs(bounds->x - 10.0F) < 0.01F);
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
    return border != nullptr && border->color == Color::Rgb(40, 180, 90) && border->style.width == 3.0F;
  }));
}

TEST_CASE("TestTextSelectionCapabilityDoesNotDependOnBuiltInNodeKinds") {
  custom_selection_requested = false;
  TestPlatform platform;
  Runtime runtime{CustomSelectionApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 40.0F}});
  runtime.BuildFrame();

  ClickAt(runtime, {20.0F, 10.0F}, 812);
  platform.AdvanceTime(0.2);
  runtime.HandlePointerEvent({
      PointerEventType::Down,
      813,
      {20.0F, 10.0F},
      PointerDeviceKind::Mouse,
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

TEST_CASE("TestSelectionAreaCopiesLogicalBlocksWithoutRealizingOffscreenViews") {
  TestPlatform platform;
  SelectionClipboard clipboard;
  platform.platform_clipboard = &clipboard;
  auto source = std::make_shared<const SelectionSource>(1000);
  ScrollController scroll;
  int factory_calls = 0;
  selection_test_root = [&]() -> View {
    const auto snapshot = source;
    return SelectionArea(
        VirtualList(snapshot->Count(), [snapshot, &factory_calls](std::size_t index) {
          ++factory_calls;
          return Text(snapshot->BlockAt(index).text.PlainText()).SelectionBlock(snapshot->IdAt(index)).Key(index);
        }).ItemExtent(20.0F).CacheExtent(0.0F).Controller(scroll)
    ).Source(snapshot);
  };
  Runtime runtime{DynamicSelectionApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 60.0F}});
  runtime.BuildFrame();
  auto* client = SelectionClient(runtime.RootNode());
  REQUIRE(client != nullptr);
  const int initial_factories = factory_calls;
  REQUIRE(initial_factories < 10);
  source->reads = 0;
  REQUIRE(client->PerformTextEditingAction(TextEditingAction::SelectAll, &clipboard));
  REQUIRE(source->reads < 10);
  REQUIRE(client->PerformTextEditingAction(TextEditingAction::Copy, &clipboard));
  REQUIRE(factory_calls == initial_factories);
  REQUIRE(clipboard.text->size() == 4999);
  REQUIRE(clipboard.text->starts_with("line\nline"));
  REQUIRE(clipboard.text->ends_with("\nline"));
  const auto initial_geometry = client->QuerySelectionGeometry();
  REQUIRE(initial_geometry.has_value());
  REQUIRE(initial_geometry->start.has_value());
  REQUIRE_FALSE(initial_geometry->end.has_value());

  REQUIRE(scroll.ScrollToItem(500));
  runtime.BuildFrame();
  client = SelectionClient(runtime.RootNode());
  const auto middle = client->QuerySelectionGeometry();
  REQUIRE(middle.has_value());
  REQUIRE_FALSE(middle->start.has_value());
  REQUIRE_FALSE(middle->end.has_value());
  REQUIRE(middle->toolbar_anchor.has_value());
  const int middle_factories = factory_calls;
  REQUIRE(client->PerformTextEditingAction(TextEditingAction::Copy, &clipboard));
  REQUIRE(clipboard.text->size() == 4999);
  REQUIRE(factory_calls == middle_factories);

  source = std::make_shared<const SelectionSource>(1000, "line appended");
  runtime.InvalidateRoot();
  REQUIRE(client->PerformTextEditingAction(TextEditingAction::Copy, &clipboard));
  REQUIRE(clipboard.text->ends_with("\nline"));
  runtime.BuildFrame();
  client = SelectionClient(runtime.RootNode());
  REQUIRE(client->PerformTextEditingAction(TextEditingAction::Copy, &clipboard));
  REQUIRE(clipboard.text->ends_with("\nline"));
}

TEST_CASE("TestSelectionAreaRetainsLayoutsWhenOnlyTheTailOrPresentationChanges") {
  class CountingPlatform final : public TestPlatform {
  public:
    std::unique_ptr<detail::TextLayout> CreateTextLayout(const huxerui::AttributedText& text, const TextStyle& style,
        float width, const TextLayoutOptions& options) override {
      ++layouts;
      return TestPlatform::CreateTextLayout(text, style, width, options);
    }
    int layouts = 0;
  } platform;
  std::string tail = "tail";
  float offset = 0.0F;
  selection_test_root = [&]() -> View {
    return SelectionArea(Column {
      Text("history").Key(1),
      Text(tail).With(Offset{Point{offset, 0.0F}}).Key(2),
    });
  };
  Runtime runtime{DynamicSelectionApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
  runtime.BuildFrame();
  REQUIRE(platform.layouts == 2);
  runtime.BuildFrame();
  REQUIRE(platform.layouts == 2);
  offset = 5.0F;
  runtime.InvalidateRoot();
  runtime.BuildFrame();
  REQUIRE(platform.layouts == 2);
  tail += " updated";
  runtime.InvalidateRoot();
  runtime.BuildFrame();
  REQUIRE(platform.layouts == 3);
  for (int update = 0; update < 256; ++update) {
    tail = "tail version " + std::to_string(update);
    runtime.InvalidateRoot();
    runtime.BuildFrame();
    REQUIRE(platform.layouts == 4 + update);
  }
}

TEST_CASE("TestSelectionAreaRejectsMismatchedSourceBodiesAndDuplicateBindings") {
  TestPlatform platform;
  const auto source = std::make_shared<const SelectionSource>(2);
  SECTION("Mismatched body") {
    selection_test_root = [&]() -> View { return SelectionArea(Text("wrong").SelectionBlock(100)).Source(source); };
    Runtime runtime{DynamicSelectionApp, platform};
    runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
    REQUIRE_THROWS_AS(runtime.BuildFrame(), std::invalid_argument);
  }
  SECTION("Duplicate binding") {
    selection_test_root = [&]() -> View {
      return SelectionArea(Column {Text("line").SelectionBlock(100), Text("line").SelectionBlock(100)}).Source(source);
    };
    Runtime runtime{DynamicSelectionApp, platform};
    runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
    REQUIRE_THROWS_AS(runtime.BuildFrame(), std::invalid_argument);
  }
  SECTION("Decorative text is excluded") {
    SelectionClipboard clipboard;
    selection_test_root = [&]() -> View {
      return SelectionArea(Column {Text("decoration"), Text("line").SelectionBlock(100)}).Source(source);
    };
    Runtime runtime{DynamicSelectionApp, platform};
    runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
    runtime.BuildFrame();
    auto* client = SelectionClient(runtime.RootNode());
    REQUIRE(client->PerformTextEditingAction(TextEditingAction::SelectAll, &clipboard));
    REQUIRE(client->PerformTextEditingAction(TextEditingAction::Copy, &clipboard));
    REQUIRE(clipboard.text == "line\nline");
  }
}

TEST_CASE("TestSelectionAreaEdgeDragAdvancesVirtualBlocksAndStopsWhenUnavailable") {
  TestPlatform platform;
  SelectionClipboard clipboard;
  platform.platform_clipboard = &clipboard;
  const auto source = std::make_shared<const SelectionSource>(40);
  ScrollController scroll;
  bool enabled = true;
  bool mounted = true;
  selection_test_root = [&]() -> View {
    if (!mounted) {
      return Text("Removed");
    }
    return SelectionArea(
        VirtualList(source->Count(), [source](std::size_t index) {
          return Text(source->BlockAt(index).text).SelectionBlock(source->IdAt(index)).Key(index);
        }).ItemExtent(20.0F).CacheExtent(0.0F).Controller(scroll)
    ).Source(source).With(Enabled{enabled});
  };
  Runtime runtime{DynamicSelectionApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 60.0F}});
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Down, 10.0F, 10.0F);
  Pointer(runtime, PointerEventType::Move, 35.0F, 59.0F);
  for (int frame = 0; frame < 20; ++frame) {
    platform.current_time += 0.016;
    runtime.BuildFrame();
  }
  REQUIRE(scroll.Offset() > 100.0F);
  REQUIRE(runtime.PerformTextEditingAction(TextEditingAction::Copy));
  REQUIRE(clipboard.text->size() > 25);
  SECTION("Release") { Pointer(runtime, PointerEventType::Up, 35.0F, 59.0F); }
  SECTION("Cancel") { Pointer(runtime, PointerEventType::Cancel, 35.0F, 59.0F); }
  SECTION("Disabled") { enabled = false; }
  SECTION("Unmounted") { mounted = false; }
  SECTION("Exhausted") { REQUIRE(scroll.ScrollTo(scroll.MaxOffset())); }
  runtime.InvalidateRoot();
  runtime.BuildFrame();
  const float released = scroll.Offset();
  for (int frame = 0; frame < 3; ++frame) {
    platform.current_time += 0.016;
    runtime.BuildFrame();
  }
  REQUIRE(scroll.Offset() == released);
  const int requests = platform.requested_frames;
  platform.current_time += 0.016;
  runtime.BuildFrame();
  REQUIRE(platform.requested_frames == requests);
}

TEST_CASE("TestSelectionAreaRejectedSourceUpdateCanRecoverWithoutLosingOldSelection") {
  TestPlatform platform;
  SelectionClipboard clipboard;
  auto source = std::make_shared<const SelectionSource>(2);
  selection_test_root = [&]() -> View {
    return SelectionArea(Column {
      Text("line").SelectionBlock(100),
      Text("line").SelectionBlock(101),
    }).Source(source);
  };
  Runtime runtime{DynamicSelectionApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
  runtime.BuildFrame();
  auto* client = SelectionClient(runtime.RootNode());
  REQUIRE(client->PerformTextEditingAction(TextEditingAction::SelectAll, &clipboard));
  source = std::make_shared<const SelectionSource>(2, "mismatch");
  runtime.InvalidateRoot();
  REQUIRE_THROWS_AS(runtime.BuildFrame(), std::invalid_argument);
  REQUIRE(client->PerformTextEditingAction(TextEditingAction::Copy, &clipboard));
  REQUIRE(clipboard.text == "line\nline");
  source = std::make_shared<const SelectionSource>(2);
  runtime.InvalidateRoot();
  REQUIRE_NOTHROW(runtime.BuildFrame());
  REQUIRE(client->PerformTextEditingAction(TextEditingAction::Copy, &clipboard));
  REQUIRE(clipboard.text == "line\nline");
}

TEST_CASE("TestSelectionAreaTouchTapClearsSelectionWithoutConsumingTarget") {
  TestPlatform platform;
  SelectionClipboard clipboard;
  platform.platform_clipboard = &clipboard;
  int clicks = 0;
  const auto paragraph = AttributedText::FromRanges("Alpha link", {}, {{{6, 10}, Uri("https://example.com")}});
  selection_test_root = [&]() -> View {
    return ProvideEnvironment(TextSelectionMenuLabels{"Cut", "Copy", "Paste", "Select all"}, Column {
      SelectionArea(Text(paragraph).On<TextEvents::LinkActivated>([&](const TextLinkActivation&) { ++clicks; }))
          .With(Frame{300.0F, 120.0F}),
      Button("Outside").OnClick([&] { ++clicks; }).With(Frame{300.0F, 48.0F}),
    });
  };
  Runtime runtime{DynamicSelectionApp, platform};
  runtime.SetWindowMetrics({.viewport = {300.0F, 240.0F}});
  runtime.BuildFrame();
  SelectByTouch(runtime, platform, {20.0F, 10.0F});
  auto* client = SelectionClient(runtime.RootNode());
  REQUIRE(client->CanPerformTextEditingAction(TextEditingAction::Copy, &clipboard));

  Point target{260.0F, 90.0F};
  int expected_clicks = 0;
  SECTION("Inside the selection area") {}
  SECTION("Empty viewport outside the application bounds") { target = {220.0F, 210.0F}; }
  SECTION("Outside button") { target = {20.0F, 144.0F}; expected_clicks = 1; }
  SECTION("Descendant link") { target = {85.0F, 10.0F}; expected_clicks = 1; }
  Touch(runtime, PointerEventType::Down, target);
  REQUIRE(client->CanPerformTextEditingAction(TextEditingAction::Copy, &clipboard));
  Touch(runtime, PointerEventType::Up, target);
  REQUIRE_FALSE(client->CanPerformTextEditingAction(TextEditingAction::Copy, &clipboard));
  REQUIRE(FindText(runtime.BuildFrame(), "Copy") == nullptr);
  REQUIRE(clicks == expected_clicks);
}

TEST_CASE("TestSelectionAreaTouchScrollRestoresMenuAndRetainsOffscreenSelection") {
  TestPlatform platform;
  SelectionClipboard clipboard;
  platform.platform_clipboard = &clipboard;
  auto source = std::make_shared<const SelectionSource>(100);
  ScrollController scroll;
  selection_test_root = [&]() -> View {
    return ProvideEnvironment(TextSelectionMenuLabels{"Cut", "Copy", "Paste", "Select all"},
        SelectionArea(VirtualList(source->Count(), [source](std::size_t index) {
          return Text(source->BlockAt(index).text).SelectionBlock(source->IdAt(index)).Key(index);
        }).ItemExtent(20.0F).CacheExtent(0.0F).Controller(scroll)
            .With(ScrollPhysics{.overscroll_enabled = false})).Source(source));
  };
  Runtime runtime{DynamicSelectionApp, platform};
  runtime.SetWindowMetrics({.viewport = {300.0F, 160.0F}});
  runtime.BuildFrame();
  SelectByTouch(runtime, platform, {20.0F, 50.0F});
  std::string expected_copy = "line";

  SECTION("Drag then release") {
    Touch(runtime, PointerEventType::Down, {270.0F, 130.0F});
    Touch(runtime, PointerEventType::Move, {270.0F, 110.0F});
    REQUIRE(FindText(runtime.BuildFrame(), "Copy") == nullptr);
    REQUIRE(scroll.Offset() > 0.0F);
    Touch(runtime, PointerEventType::Up, {270.0F, 110.0F});
  }
  SECTION("Canceled drag") {
    Touch(runtime, PointerEventType::Down, {270.0F, 130.0F});
    Touch(runtime, PointerEventType::Move, {270.0F, 110.0F});
    runtime.BuildFrame();
    Touch(runtime, PointerEventType::Cancel, {270.0F, 110.0F});
  }
  SECTION("Offscreen and back") {
    REQUIRE(scroll.ScrollToItem(50));
    REQUIRE(FindText(runtime.BuildFrame(), "Copy") == nullptr);
    const auto geometry = SelectionClient(runtime.RootNode())->QuerySelectionGeometry();
    REQUIRE(geometry.has_value());
    REQUIRE_FALSE(geometry->start);
    REQUIRE_FALSE(geometry->end);
    REQUIRE_FALSE(geometry->toolbar_anchor);
    REQUIRE(runtime.PerformTextEditingAction(TextEditingAction::Copy));
    REQUIRE(clipboard.text == "line");
    REQUIRE(scroll.ScrollTo(0.0F));
  }
  SECTION("Removing the selected block ends menu restoration") {
    source = std::make_shared<const SelectionSource>(2);
    runtime.InvalidateRoot();
    REQUIRE(FindText(runtime.BuildFrame(), "Copy") == nullptr);
    REQUIRE_FALSE(runtime.CanPerformTextEditingAction(TextEditingAction::Copy));
    REQUIRE_FALSE(SelectionClient(runtime.RootNode())->QuerySelectionGeometry());
    SECTION("Back reaches the application") {
      REQUIRE_FALSE(runtime.HandleBack());
    }
    SECTION("A new selection does not inherit the old menu") {
      REQUIRE(runtime.PerformTextEditingAction(TextEditingAction::SelectAll));
      REQUIRE(FindText(runtime.BuildFrame(), "Copy") == nullptr);
    }
    return;
  }
  SECTION("Canceled tap") {
    Touch(runtime, PointerEventType::Down, {270.0F, 130.0F});
    Touch(runtime, PointerEventType::Cancel, {270.0F, 130.0F});
  }
  SECTION("Momentum completes before the menu returns") {
    REQUIRE(runtime.PerformTextEditingAction(TextEditingAction::SelectAll));
    REQUIRE(runtime.PerformTextEditingAction(TextEditingAction::Copy));
    expected_copy = *clipboard.text;
    runtime.BuildFrame();
    Touch(runtime, PointerEventType::Down, {270.0F, 130.0F});
    platform.AdvanceTime(0.05);
    Touch(runtime, PointerEventType::Move, {270.0F, 110.0F});
    platform.AdvanceTime(0.01);
    Touch(runtime, PointerEventType::Up, {270.0F, 110.0F});
    REQUIRE(FindText(runtime.BuildFrame(), "Copy") == nullptr);
    const float released_offset = scroll.Offset();
    bool restored = false;
    for (int frame = 0; frame < 300 && !restored; ++frame) {
      platform.AdvanceTime(0.016);
      restored = FindText(runtime.BuildFrame(), "Copy") != nullptr;
    }
    REQUIRE(restored);
    REQUIRE(scroll.Offset() > released_offset);
    const float stopped_offset = scroll.Offset();
    platform.AdvanceTime(0.1);
    runtime.BuildFrame();
    REQUIRE(scroll.Offset() == stopped_offset);
  }
  SECTION("Back prevents later scroll from reopening the menu") {
    REQUIRE(runtime.HandleBack());
    REQUIRE(scroll.ScrollTo(20.0F));
    REQUIRE(FindText(runtime.BuildFrame(), "Copy") == nullptr);
    REQUIRE(runtime.PerformTextEditingAction(TextEditingAction::Copy));
    return;
  }
  SECTION("Copy prevents later scroll from reopening the menu") {
    const auto* copy = FindText(runtime.BuildFrame(), "Copy");
    REQUIRE(copy != nullptr);
    const Point center{copy->rect.x + copy->rect.width * 0.5F, copy->rect.y + copy->rect.height * 0.5F};
    Touch(runtime, PointerEventType::Down, center);
    Touch(runtime, PointerEventType::Up, center);
    REQUIRE(clipboard.text == "line");
    runtime.BuildFrame();
    platform.AdvanceTime(0.3);
    REQUIRE(FindText(runtime.BuildFrame(), "Copy") == nullptr);
    REQUIRE(scroll.ScrollTo(20.0F));
    REQUIRE(FindText(runtime.BuildFrame(), "Copy") == nullptr);
    return;
  }
  REQUIRE(FindText(runtime.BuildFrame(), "Copy") != nullptr);
  REQUIRE(runtime.PerformTextEditingAction(TextEditingAction::Copy));
  REQUIRE(clipboard.text == expected_copy);
}

} // namespace huxerui::test
