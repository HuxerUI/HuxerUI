#include "runtime_test_support.h"

#include <type_traits>

namespace huxerui::test {

namespace {

int semantic_button_clicks = 0;
int semantic_icon_button_clicks = 0;
int semantic_lifecycle_clicks = 0;
int virtual_semantic_activations = 0;
State<float> semantic_slider_value;
State<bool> semantic_alternate_content;
State<bool> semantic_alternate_label;
State<bool> semantic_virtual_visible;

static_assert(!std::is_copy_constructible_v<SemanticBuilder>);
static_assert(!std::is_move_constructible_v<SemanticBuilder>);

struct VirtualSemantics;

class VirtualSemanticsExtension final : public NodeExtension {
public:
  VirtualSemanticsExtension(MountedNode& node, const VirtualSemantics& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const VirtualSemantics& modifier) {
    static_cast<void>(node);
    static_cast<void>(modifier);
  }

  void BuildSemantics(SemanticBuilder& builder) const override {
    builder.SetOwner(Semantics{.label = "Chart"});
    builder.AddChild(
        7,
        {8.0F, 6.0F, 24.0F, 12.0F},
        Semantics{
            .role = SemanticRole::Button,
            .label = "April",
        }
    );
    builder.AddAction(7, SemanticActionKind::Activate);
  }

  bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) override {
    if (local_id != 7 || action.kind != SemanticActionKind::Activate) {
      return false;
    }
    ++virtual_semantic_activations;
    return true;
  }
};

struct VirtualSemantics {
  using Extension = VirtualSemanticsExtension;

  bool operator==(const VirtualSemantics&) const = default;
};

VectorAsset SemanticActionIcon() {
  static const VectorAsset icon = VectorAsset::Create({12.0F, 12.0F}, [](VectorBuilder& builder) {
    builder.FillPath(
        Path{}.MoveTo({1.0F, 1.0F}).LineTo({11.0F, 6.0F}).LineTo({1.0F, 11.0F}).Close(),
        Color::Black()
    );
  });
  return icon;
}

View SemanticBasicsApp() {
  auto slider = UseState(0.25F);
  semantic_slider_value = slider;
  return Column {
    Text("Status"),
    Button("Continue").OnClick([] { ++semantic_button_clicks; }),
    IconButton(SemanticActionIcon(), "Play").OnClick([] { ++semantic_icon_button_clicks; }),
    Checkbox("Remember", true),
    Slider(slider.Get()).Range(0.0F, 10.0F).Step(0.5F).OnChanged([slider](float value) mutable {
      slider = value;
    }),
  };
}

View SemanticOverrideApp() {
  return Canvas([](PaintContext&, Size) {}).With(
      Frame{40.0F, 20.0F},
      Semantics{
          .role = SemanticRole::Image,
          .label = "Revenue chart",
      }
  );
}

View SecureSemanticTextFieldApp() {
  return TextField(TextEditingValue::FromText("secret"))
      .Label("Password")
      .InputConfiguration({.secure = true});
}

View VirtualSemanticApp() {
  return Canvas([](PaintContext&, Size) {}).With(VirtualSemantics{});
}

View VirtualSemanticLifecycleApp() {
  auto visible = UseState(true);
  semantic_virtual_visible = visible;
  View canvas = Canvas([](PaintContext&, Size) {});
  if (visible.Get()) {
    return std::move(canvas).With(VirtualSemantics{});
  }
  return std::move(canvas).With(Semantics{.label = "Fallback"});
}

View EmptySemanticApp() {
  return Canvas([](PaintContext&, Size) {}).With(Semantics{});
}

View SemanticPrecedenceApp() {
  return Button("Component").OnClick([] {}).With(VirtualSemantics{}, Semantics{.label = "Author"});
}

View SemanticLifecycleApp() {
  auto alternate = UseState(false);
  semantic_alternate_content = alternate;
  View active = alternate.Get()
                    ? Button("Replacement").OnClick([] { ++semantic_lifecycle_clicks; }).Key("replacement")
                    : Button("Primary").OnClick([] { ++semantic_lifecycle_clicks; }).Key("primary");
  return Column {
    std::move(active),
    Button("Disabled").OnClick([] { ++semantic_lifecycle_clicks; }).With(Enabled{false}),
    Column {
      Text("Excluded"),
    }.With(Semantics{
        .label = "Owner",
        .descendants = SemanticDescendantPolicy::Exclude,
    }),
    Text("Hidden").With(Semantics{.hidden = true}),
  };
}

View SemanticCompatibleUpdateApp() {
  auto alternate = UseState(false);
  semantic_alternate_label = alternate;
  return Canvas([](PaintContext&, Size) {}).With(Semantics{.label = alternate.Get() ? "After" : "Before"});
}

const SemanticNode& FindSemanticNode(const SemanticFrame& frame, std::string_view label) {
  const auto found = std::ranges::find(frame.nodes, label, &SemanticNode::label);
  REQUIRE(found != frame.nodes.end());
  return *found;
}

const SemanticNode* FindSemanticNodeOrNull(const SemanticFrame& frame, std::string_view label) {
  const auto found = std::ranges::find(frame.nodes, label, &SemanticNode::label);
  return found == frame.nodes.end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("SemanticFramePublishesBuiltInComponentMeaningAndReusesUnchangedData") {
  semantic_button_clicks = 0;
  semantic_icon_button_clicks = 0;
  TestPlatform platform;
  Runtime runtime(SemanticBasicsApp, platform);
  runtime.SetViewport({320.0F, 240.0F});

  const FrameCommit& first = runtime.BuildCommit();
  REQUIRE(first.semantic_frame);
  REQUIRE(first.semantic_frame->revision != 0);
  REQUIRE(first.semantic_frame->root != 0);
  const std::shared_ptr<const SemanticFrame> first_frame = first.semantic_frame;

  const SemanticNode& text = FindSemanticNode(*first_frame, "Status");
  REQUIRE(text.role == SemanticRole::Text);
  const SemanticNode& button = FindSemanticNode(*first_frame, "Continue");
  REQUIRE(button.role == SemanticRole::Button);
  REQUIRE((button.actions & SemanticActionMask(SemanticActionKind::Activate)) != 0);
  const SemanticNode& icon_button = FindSemanticNode(*first_frame, "Play");
  REQUIRE(icon_button.role == SemanticRole::Button);
  REQUIRE((icon_button.actions & SemanticActionMask(SemanticActionKind::Activate)) != 0);
  const SemanticNode& checkbox = FindSemanticNode(*first_frame, "Remember");
  REQUIRE(checkbox.checked == SemanticCheckedState::Checked);

  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(button.id, {SemanticActionKind::Activate, std::monostate{}}));
  REQUIRE(semantic_button_clicks == 1);
  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(
      icon_button.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(semantic_icon_button_clicks == 1);
  REQUIRE_FALSE(runtime.NativeRuntime().PerformSemanticAction(button.id, {SemanticActionKind::Activate, 1.0}));

  const FrameCommit& unchanged = runtime.BuildCommit();
  REQUIRE(unchanged.semantic_frame == first_frame);
}

TEST_CASE("SemanticActionsRouteToRetainedControlBehavior") {
  TestPlatform platform;
  Runtime runtime(SemanticBasicsApp, platform);
  runtime.SetViewport({320.0F, 240.0F});

  runtime.BuildCommit();
  const auto slider_node = std::ranges::find_if(runtime.LastCommit().semantic_frame->nodes, [](const SemanticNode& node) {
    return node.role == SemanticRole::Slider;
  });
  REQUIRE(slider_node != runtime.LastCommit().semantic_frame->nodes.end());
  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(
      slider_node->id,
      {SemanticActionKind::SetValue, 7.5}
  ));
  REQUIRE(semantic_slider_value.Get() == 7.5F);
  REQUIRE_FALSE(runtime.NativeRuntime().PerformSemanticAction(
      slider_node->id,
      {SemanticActionKind::SetValue, std::string("7.5")}
  ));

  const SemanticNodeId slider_id = slider_node->id;
  const std::shared_ptr<const SemanticFrame> updated = runtime.BuildCommit().semantic_frame;
  const auto updated_slider = std::ranges::find(updated->nodes, slider_id, &SemanticNode::id);
  REQUIRE(updated_slider != updated->nodes.end());
  REQUIRE(updated_slider->range.has_value());
  REQUIRE(updated_slider->range->current == 7.5);
}

TEST_CASE("SemanticsModifierPublishesCustomMeaning") {
  TestPlatform platform;
  Runtime runtime(SemanticOverrideApp, platform);
  runtime.SetViewport({120.0F, 80.0F});

  const SemanticNode& image = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "Revenue chart");
  REQUIRE(image.role == SemanticRole::Image);
  REQUIRE(image.bounds == Rect{0.0F, 0.0F, 120.0F, 80.0F});
}

TEST_CASE("ExplicitEmptySemanticsPublishesGenericOwner") {
  TestPlatform platform;
  Runtime runtime(EmptySemanticApp, platform);
  runtime.SetViewport({120.0F, 80.0F});

  const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
  REQUIRE(frame->nodes.size() == 2);
  REQUIRE(frame->nodes.back().role == SemanticRole::Generic);
}

TEST_CASE("AuthorSemanticsOverrideExtensionAndPreserveComponentMeaning") {
  TestPlatform platform;
  Runtime runtime(SemanticPrecedenceApp, platform);
  runtime.SetViewport({120.0F, 80.0F});

  const SemanticNode& button = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "Author");
  REQUIRE(button.role == SemanticRole::Button);
  REQUIRE((button.actions & SemanticActionMask(SemanticActionKind::Activate)) != 0);
}

TEST_CASE("SecureTextFieldDoesNotPublishItsValue") {
  TestPlatform platform;
  Runtime runtime(SecureSemanticTextFieldApp, platform);
  runtime.SetViewport({320.0F, 120.0F});

  const SemanticNode& field = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "Password");
  REQUIRE(field.role == SemanticRole::TextField);
  REQUIRE(field.secure);
  REQUIRE(field.value.empty());
}

TEST_CASE("SemanticBuilderPublishesStableVirtualChildrenAndRoutesActions") {
  virtual_semantic_activations = 0;
  TestPlatform platform;
  Runtime runtime(VirtualSemanticApp, platform);
  runtime.SetViewport({120.0F, 80.0F});

  const std::shared_ptr<const SemanticFrame> first = runtime.BuildCommit().semantic_frame;
  const SemanticNode& child = FindSemanticNode(*first, "April");
  REQUIRE(child.role == SemanticRole::Button);
  REQUIRE(child.bounds == Rect{8.0F, 6.0F, 24.0F, 12.0F});
  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(
      child.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(virtual_semantic_activations == 1);

  const SemanticNodeId child_id = child.id;
  const SemanticNode& stable_child = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "April");
  REQUIRE(stable_child.id == child_id);
}

TEST_CASE("ReplacingSemanticExtensionInvalidatesVirtualIdentityAndActionRoute") {
  virtual_semantic_activations = 0;
  TestPlatform platform;
  Runtime runtime(VirtualSemanticLifecycleApp, platform);
  runtime.SetViewport({120.0F, 80.0F});

  const SemanticNodeId first_id = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "April").id;
  semantic_virtual_visible = false;
  const std::shared_ptr<const SemanticFrame> fallback = runtime.BuildCommit().semantic_frame;
  REQUIRE(FindSemanticNodeOrNull(*fallback, "April") == nullptr);
  REQUIRE_FALSE(runtime.NativeRuntime().PerformSemanticAction(
      first_id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));

  semantic_virtual_visible = true;
  const SemanticNode& replacement = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "April");
  REQUIRE(replacement.id != first_id);
  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(
      replacement.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(virtual_semantic_activations == 1);
}

TEST_CASE("SemanticLifecycleHonorsVisibilityExclusionDisabledStateAndStaleActions") {
  semantic_lifecycle_clicks = 0;
  TestPlatform platform;
  Runtime runtime(SemanticLifecycleApp, platform);
  runtime.SetViewport({320.0F, 240.0F});

  const std::shared_ptr<const SemanticFrame> first = runtime.BuildCommit().semantic_frame;
  const SemanticNode& primary = FindSemanticNode(*first, "Primary");
  const SemanticNodeId primary_id = primary.id;
  const SemanticNode& disabled = FindSemanticNode(*first, "Disabled");
  REQUIRE(disabled.actions == 0);
  REQUIRE_FALSE(runtime.NativeRuntime().PerformSemanticAction(
      disabled.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(semantic_lifecycle_clicks == 0);

  const SemanticNode& owner = FindSemanticNode(*first, "Owner");
  REQUIRE(owner.children.empty());
  REQUIRE(FindSemanticNodeOrNull(*first, "Excluded") == nullptr);
  REQUIRE(FindSemanticNodeOrNull(*first, "Hidden") == nullptr);

  REQUIRE(runtime.NativeRuntime().PerformSemanticAction(primary_id, {SemanticActionKind::Focus, std::monostate{}}));
  const SemanticNode& focused = FindSemanticNode(*runtime.BuildCommit().semantic_frame, "Primary");
  REQUIRE(focused.focused);

  semantic_alternate_content = true;
  const std::shared_ptr<const SemanticFrame> replacement_frame = runtime.BuildCommit().semantic_frame;
  const SemanticNode& replacement = FindSemanticNode(*replacement_frame, "Replacement");
  REQUIRE(replacement.id != primary_id);
  REQUIRE(FindSemanticNodeOrNull(*replacement_frame, "Primary") == nullptr);
  REQUIRE_FALSE(runtime.NativeRuntime().PerformSemanticAction(
      primary_id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
}

TEST_CASE("CompatibleSemanticUpdatesPreserveIdentityAndAdvanceRevision") {
  TestPlatform platform;
  Runtime runtime(SemanticCompatibleUpdateApp, platform);
  runtime.SetViewport({120.0F, 80.0F});

  const std::shared_ptr<const SemanticFrame> before = runtime.BuildCommit().semantic_frame;
  const SemanticNodeId id = FindSemanticNode(*before, "Before").id;

  semantic_alternate_label = true;
  const std::shared_ptr<const SemanticFrame> after = runtime.BuildCommit().semantic_frame;
  REQUIRE(after->revision > before->revision);
  REQUIRE(FindSemanticNode(*after, "After").id == id);
}

TEST_CASE("SemanticsModifierRejectsInvalidSharedValues") {
  REQUIRE_THROWS_AS(
      Canvas([](PaintContext&, Size) {}).With(Semantics{.heading_level = 7}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      Canvas([](PaintContext&, Size) {}).With(Semantics{.range = SemanticRange{1.0, 0.0, 0.5}}),
      std::invalid_argument
  );
}

} // namespace huxerui::test
