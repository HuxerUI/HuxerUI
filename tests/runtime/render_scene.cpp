#include "runtime_test_support.h"

#include <limits>

#include "external_texture_test_support.h"

namespace huxerui::test {

View RenderSceneApp() {
  return Column{
      Text("content").With(Frame{80.0F, 20.0F}),
      Checkbox(true),
  }
      .With(Spacing{6.0F});
}

State<bool> paint_reuse_changed;
State<bool> presentation_reuse_moved;
State<bool> removal_damage_visible;
State<bool> clip_damage_expanded;
State<bool> retained_opacity_faded;
State<bool> child_order_reversed;
State<bool> overflowing_paint_changed;
State<bool> shadow_changed;
State<bool> canvas_changed;
State<bool> clip_children_enabled;
State<bool> overflow_clip_enabled;
ExternalTexture render_scene_external_texture;
bool render_scene_external_texture_visible = true;
int canvas_paint_count = 0;
int clipped_child_clicks = 0;
int overflowing_child_clicks = 0;

struct OverflowPaint;
struct FramePaintInvalidation;

class OverflowPaintExtension final : public NodeExtension {
public:
  OverflowPaintExtension(MountedNode& node, const OverflowPaint& modifier);

  void Update(MountedNode& node, const OverflowPaint& modifier);
  void Paint(const MountedNode& node, PaintContext& context) const override;

private:
  Color color_;
};

struct OverflowPaint {
  using Extension = OverflowPaintExtension;

  Color color;

  bool operator==(const OverflowPaint&) const = default;
};

class FramePaintInvalidationExtension final : public NodeExtension {
public:
  FramePaintInvalidationExtension(MountedNode& node, const FramePaintInvalidation& modifier);

  void Update(MountedNode& node, const FramePaintInvalidation& modifier);

  FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    static_cast<void>(node);
    static_cast<void>(frame);
    InvalidatePaint();
    return {};
  }
};

struct FramePaintInvalidation {
  using Extension = FramePaintInvalidationExtension;

  bool operator==(const FramePaintInvalidation&) const = default;
};

FramePaintInvalidationExtension::FramePaintInvalidationExtension(
    MountedNode& node, const FramePaintInvalidation& modifier
) {
  static_cast<void>(node);
  static_cast<void>(modifier);
}

void FramePaintInvalidationExtension::Update(MountedNode& node, const FramePaintInvalidation& modifier) {
  static_cast<void>(node);
  static_cast<void>(modifier);
}

OverflowPaintExtension::OverflowPaintExtension(MountedNode& node, const OverflowPaint& modifier)
    : color_(modifier.color) {
  static_cast<void>(node);
}

void OverflowPaintExtension::Update(MountedNode& node, const OverflowPaint& modifier) {
  static_cast<void>(node);
  color_ = modifier.color;
}

void OverflowPaintExtension::Paint(const MountedNode& node, PaintContext& context) const {
  static_cast<void>(node);
  context.DrawRect({-200.0F, 0.0F, 80.0F, 20.0F}, color_);
}

View PaintReuseApp() {
  auto changed = UseState(false);
  paint_reuse_changed = changed;
  return Column{
      Text(changed.Get() ? "changed" : "initial"),
      Text("stable"),
  };
}

View ExternalTextureRenderApp() {
  if (!render_scene_external_texture_visible) {
    return Spacer().With(Frame{80.0F, 40.0F});
  }
  return Image(render_scene_external_texture).Fit(ImageFit::Fill).With(Frame{80.0F, 40.0F});
}

View FramePaintInvalidationApp() {
  return Spacer().With(Frame{40.0F, 20.0F}, FramePaintInvalidation{});
}

View ShadowApp() {
  auto changed = UseState(false);
  shadow_changed = changed;
  return Row {
      Spacer().With(
          Frame{40.0F, 20.0F},
          Background{Color::White()},
          CornerRadius{6.0F},
          Shadow{
              .color = Color::Rgb(0, 0, 0, changed.Get() ? 0.4F : 0.2F),
              .offset = {4.0F, 5.0F},
              .blur_radius = 6.0F,
              .spread = 2.0F,
          }
      ),
  }.With(CrossAlign{CrossAxisAlignment::Start});
}

View CanvasApp() {
  auto changed = UseState(false);
  canvas_changed = changed;
  const Color color = changed.Get() ? Color::White() : Color::Black();
  return Row {
      Canvas([color](PaintContext& paint, Size size) {
        ++canvas_paint_count;
        REQUIRE(size == Size{60.0F, 40.0F});
        REQUIRE(paint.Bounds() == Rect{0.0F, 0.0F, 60.0F, 40.0F});
        paint.DrawRect({0.0F, 0.0F, size.width, size.height}, color);
      }).With(Frame{80.0F, 60.0F}, Padding(10.0F)),
  };
}

View ClipChildrenApp() {
  auto enabled = UseState(true);
  clip_children_enabled = enabled;
  View result = Stack {
    Button("clipped child").With(Frame{80.0F, 80.0F}).OnClick([] { ++clipped_child_clicks; }),
  }.With(Frame{80.0F, 80.0F}, Background{Color::White()}, CornerRadius{20.0F});
  if (enabled.Get()) {
    result = std::move(result).With(ClipChildren{});
  }
  return result;
}

View AsymmetricClipChildrenApp() {
  return Stack {
    Spacer().With(Frame{80.0F, 80.0F}, Background{Color::White()}),
  }.With(Frame{80.0F, 80.0F}, CornerRadius{CornerRadii::Top(20.0F)}, ClipChildren{});
}

View OverflowChildHitTestApp() {
  auto clipped = UseState(true);
  overflow_clip_enabled = clipped;
  View result = Stack {
    Button("overflowing child")
        .With(Frame{80.0F, 40.0F}, Offset{Point{40.0F, 20.0F}})
        .OnClick([] { ++overflowing_child_clicks; }),
  }.With(Frame{80.0F, 80.0F});
  if (clipped.Get()) {
    result = std::move(result).With(ClipChildren{});
  }
  return Row {
    std::move(result),
  };
}

View ClippedScrollViewApp() {
  return Row {
    ScrollView {
      Spacer().With(Frame{160.0F, 160.0F}),
    }.With(Frame{80.0F, 80.0F}, Padding{10.0F}, CornerRadius{CornerRadii::Top(16.0F)}, ClipChildren{}),
  };
}

View PresentationReuseApp() {
  auto moved = UseState(false);
  presentation_reuse_moved = moved;
  return Text("moving").With(
      Offset{AnimateTo(Point{moved.Get() ? 80.0F : 0.0F, 0.0F}, TweenSpec{1.0, Easing::Linear})}
  );
}

View RemovalDamageApp() {
  auto visible = UseState(true);
  removal_damage_visible = visible;
  if (!visible.Get()) {
    return {};
  }
  return Text("removed").With(Frame{70.0F, 20.0F});
}

View ClipDamageApp() {
  auto expanded = UseState(false);
  clip_damage_expanded = expanded;
  return Column{
      ScrollView{
          Text("clipped").With(Frame{100.0F, 100.0F}),
      }
          .With(Frame{100.0F, expanded.Get() ? 60.0F : 40.0F}),
  };
}

View RetainedOpacityApp() {
  auto faded = UseState(false);
  retained_opacity_faded = faded;
  return Column{
      Text("faded child"),
      Checkbox(true),
  }
      .With(Opacity{AnimateTo(faded.Get() ? 0.0F : 1.0F, TweenSpec{1.0, Easing::Linear})});
}

View RetainedScrollApp() {
  return ScrollView{
      Column{
          Text("first").With(Frame{100.0F, 40.0F}),
          Text("second").With(Frame{100.0F, 40.0F}),
          Text("third").With(Frame{100.0F, 40.0F}),
      },
  };
}

View ChildOrderDamageApp() {
  auto reversed = UseState(false);
  child_order_reversed = reversed;
  View first = Text("first").With(Frame{80.0F, 20.0F}).Key("first");
  View second = Text("second").With(Frame{80.0F, 20.0F}).Key("second");
  if (reversed.Get()) {
    return Stack{
        std::move(second),
        std::move(first),
    };
  }
  return Stack{
      std::move(first),
      std::move(second),
  };
}

View OverflowingChildApp() {
  return Stack{
      Column{
          Text("visible child").With(Frame{80.0F, 20.0F}, Offset{Point{-200.0F, 0.0F}}),
      }
          .With(Frame{80.0F, 20.0F}, Offset{Point{200.0F, 0.0F}}),
  };
}

View ClippedOverflowingChildApp() {
  return Stack{
      ScrollView{
          Text("clipped child").With(Frame{80.0F, 20.0F}, Offset{Point{-200.0F, 0.0F}}),
      }
          .With(Frame{80.0F, 20.0F}, Offset{Point{200.0F, 0.0F}}),
  };
}

View OverflowingPaintApp() {
  auto changed = UseState(false);
  overflowing_paint_changed = changed;
  return Spacer().With(
      Frame{80.0F, 20.0F},
      Offset{Point{200.0F, 0.0F}},
      OverflowPaint{changed.Get() ? Color::White() : Color::Black()}
  );
}

View ClippedOverflowingPaintApp() {
  return ScrollView{
      Spacer().With(Frame{80.0F, 20.0F}, OverflowPaint{Color::White()}),
  }
      .With(Frame{80.0F, 20.0F}, Offset{Point{200.0F, 0.0F}});
}

const RenderNode* FindRenderNode(const RenderNode& node, std::uint64_t id) {
  if (node.id == id) {
    return &node;
  }
  for (const RenderNode* child : node.children) {
    if (child != nullptr) {
      if (const RenderNode* found = FindRenderNode(*child, id)) {
        return found;
      }
    }
  }
  return nullptr;
}

bool DamageContains(const DamageRegion& damage, Rect bounds) {
  return std::any_of(damage.rects.begin(), damage.rects.end(), [bounds](Rect rect) {
    return rect.x <= bounds.x && rect.y <= bounds.y && rect.x + rect.width >= bounds.x + bounds.width &&
           rect.y + rect.height >= bounds.y + bounds.height;
  });
}

TEST_CASE("RuntimePublishesStableRenderSceneNodes") {
  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime{RenderSceneApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});

  const RenderFrame& first_frame = runtime.BuildRenderFrame();
  REQUIRE(first_frame.scene.root != nullptr);
  REQUIRE(first_frame.damage.full);
  REQUIRE(first_frame.damage.rects.size() == 1);
  REQUIRE(first_frame.damage.rects[0].width == 160.0F);
  REQUIRE(first_frame.damage.rects[0].height == 100.0F);

  const auto* mounted_root = runtime.RootNode();
  REQUIRE(mounted_root != nullptr);
  REQUIRE(mounted_root->children.size() == 2);
  const std::uint64_t text_id = mounted_root->children[0]->identity;
  const std::uint64_t checkbox_id = mounted_root->children[1]->identity;

  const RenderNode* first_text = FindRenderNode(*first_frame.scene.root, text_id);
  const RenderNode* first_checkbox = FindRenderNode(*first_frame.scene.root, checkbox_id);
  REQUIRE(first_text != nullptr);
  REQUIRE(first_checkbox != nullptr);
  REQUIRE(first_text->content.Commands().size() == 1);
  REQUIRE(first_text->foreground.Commands().empty());
  REQUIRE(first_checkbox->content.Commands().empty());
  REQUIRE(!first_checkbox->foreground.Commands().empty());
  REQUIRE(first_checkbox->offset.y == 26.0F);

  const std::uint64_t first_revision = first_frame.revision;
  const std::uint64_t first_root_revision = first_frame.scene.root->revision;
  const std::uint64_t first_text_revision = first_text->revision;
  const std::uint64_t first_checkbox_revision = first_checkbox->revision;
  const PaintCommand* first_text_commands = first_text->content.Commands().data();
  const RenderFrame& second_frame = runtime.BuildRenderFrame();
  const RenderNode* second_text = FindRenderNode(*second_frame.scene.root, text_id);
  const RenderNode* second_checkbox = FindRenderNode(*second_frame.scene.root, checkbox_id);
  REQUIRE(second_text == first_text);
  REQUIRE(second_checkbox == first_checkbox);
  REQUIRE(second_frame.revision > first_revision);
  REQUIRE(second_frame.scene.root->revision == first_root_revision);
  REQUIRE(second_text->revision == first_text_revision);
  REQUIRE(second_checkbox->revision == first_checkbox_revision);
  REQUIRE(second_text->content.Commands().data() == first_text_commands);
  REQUIRE_FALSE(second_frame.damage.full);
  REQUIRE(second_frame.damage.rects.empty());
}

TEST_CASE("ClipChildrenPublishesARoundedClipAndRestrictsDescendantHitTesting") {
  clipped_child_clicks = 0;
  TestPlatform platform;
  Runtime runtime{ClipChildrenApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});

  const RenderFrame& frame = runtime.BuildRenderFrame();
  REQUIRE(frame.scene.root != nullptr);
  const detail::MountedNode* mounted = runtime.RootNode();
  REQUIRE(mounted != nullptr);
  const RenderNode* render_node = FindRenderNode(*frame.scene.root, mounted->identity);
  REQUIRE(render_node != nullptr);
  REQUIRE(render_node->child_clips.size() == 1);
  const auto* child_clip = std::get_if<PushClipCommand>(&render_node->child_clips.front());
  REQUIRE(child_clip != nullptr);
  REQUIRE(child_clip->rect == mounted->bounds);
  REQUIRE(child_clip->corner_radius == 20.0F);

  ClickAt(runtime, {1.0F, 1.0F}, 1);
  REQUIRE(clipped_child_clicks == 0);

  clip_children_enabled = false;
  const RenderFrame& unclipped_frame = runtime.BuildRenderFrame();
  render_node = FindRenderNode(*unclipped_frame.scene.root, mounted->identity);
  REQUIRE(render_node != nullptr);
  REQUIRE(render_node->child_clips.empty());
  ClickAt(runtime, {1.0F, 1.0F}, 2);
  REQUIRE(clipped_child_clicks == 1);
  ClickAt(runtime, {40.0F, 40.0F}, 3);
  REQUIRE(clipped_child_clicks == 2);
}

TEST_CASE("ClipChildrenPublishesAPathClipForAsymmetricCornerRadii") {
  TestPlatform platform;
  Runtime runtime{AsymmetricClipChildrenApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});

  const RenderFrame& frame = runtime.BuildRenderFrame();
  REQUIRE(frame.scene.root != nullptr);
  const detail::MountedNode* mounted = runtime.RootNode();
  REQUIRE(mounted != nullptr);
  const RenderNode* render_node = FindRenderNode(*frame.scene.root, mounted->identity);
  REQUIRE(render_node != nullptr);
  REQUIRE(render_node->child_clips.size() == 1);
  const auto* child_clip = std::get_if<PushPathClipCommand>(&render_node->child_clips.front());
  REQUIRE(child_clip != nullptr);
  REQUIRE(child_clip->path.Bounds() == mounted->bounds);
}

TEST_CASE("OverflowingChildrenRemainInteractiveUntilClipChildrenIsApplied") {
  overflowing_child_clicks = 0;
  TestPlatform platform;
  Runtime runtime{OverflowChildHitTestApp, platform};
  runtime.SetWindowMetrics({.viewport = {140.0F, 100.0F}});

  runtime.BuildRenderFrame();
  ClickAt(runtime, {100.0F, 40.0F}, 1);
  REQUIRE(overflowing_child_clicks == 0);

  overflow_clip_enabled = false;
  runtime.BuildRenderFrame();
  ClickAt(runtime, {100.0F, 40.0F}, 2);
  REQUIRE(overflowing_child_clicks == 1);
}

TEST_CASE("ScrollViewRetainsContainerAndContentClips") {
  TestPlatform platform;
  Runtime runtime{ClippedScrollViewApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});

  const RenderFrame& frame = runtime.BuildRenderFrame();
  REQUIRE(frame.scene.root != nullptr);
  const detail::MountedNode* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 1);
  const detail::MountedNode* scroll_view = root->children.front().get();
  REQUIRE(scroll_view->kind == detail::NodeKind::ScrollView);
  const RenderNode* render_node = FindRenderNode(*frame.scene.root, scroll_view->identity);
  REQUIRE(render_node != nullptr);
  REQUIRE(render_node->child_clips.size() == 2);
  REQUIRE(std::holds_alternative<PushPathClipCommand>(render_node->child_clips[0]));
  const auto* content_clip = std::get_if<PushClipCommand>(&render_node->child_clips[1]);
  REQUIRE(content_clip != nullptr);
  REQUIRE(content_clip->rect == Rect{10.0F, 10.0F, 60.0F, 60.0F});
}

TEST_CASE("RenderSceneRerecordsOnlyChangedDeclarativePaint") {
  TestPlatform platform;
  Runtime runtime{PaintReuseApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});

  const RenderFrame& first_frame = runtime.BuildRenderFrame();
  REQUIRE(first_frame.scene.root != nullptr);
  const auto* mounted_root = runtime.RootNode();
  REQUIRE(mounted_root != nullptr);
  REQUIRE(mounted_root->children.size() == 2);
  const std::uint64_t changed_id = mounted_root->children[0]->identity;
  const std::uint64_t stable_id = mounted_root->children[1]->identity;
  const RenderNode* changed_before = FindRenderNode(*first_frame.scene.root, changed_id);
  const RenderNode* stable_before = FindRenderNode(*first_frame.scene.root, stable_id);
  REQUIRE(changed_before != nullptr);
  REQUIRE(stable_before != nullptr);
  const std::uint64_t changed_revision = changed_before->revision;
  const std::uint64_t stable_revision = stable_before->revision;
  const PaintCommand* stable_commands = stable_before->content.Commands().data();
  const Rect changed_bounds = mounted_root->children[0]->PresentationBounds();
  const Rect stable_bounds = mounted_root->children[1]->PresentationBounds();

  paint_reuse_changed = true;
  const RenderFrame& second_frame = runtime.BuildRenderFrame();
  const RenderNode* changed_after = FindRenderNode(*second_frame.scene.root, changed_id);
  const RenderNode* stable_after = FindRenderNode(*second_frame.scene.root, stable_id);
  REQUIRE(changed_after == changed_before);
  REQUIRE(stable_after == stable_before);
  REQUIRE(changed_after->revision > changed_revision);
  REQUIRE(stable_after->revision == stable_revision);
  REQUIRE(stable_after->content.Commands().data() == stable_commands);
  REQUIRE_FALSE(second_frame.damage.full);
  REQUIRE(DamageContains(second_frame.damage, changed_bounds));
  REQUIRE(std::none_of(second_frame.damage.rects.begin(), second_frame.damage.rects.end(), [stable_bounds](Rect rect) {
    return rect.Intersects(stable_bounds);
  }));
}

TEST_CASE("PresentationAnimationReusesPaintSequences") {
  TestPlatform platform;
  Runtime runtime{PresentationReuseApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});
  runtime.BuildRenderFrame();

  presentation_reuse_moved = true;
  const RenderFrame& animation_start = runtime.BuildRenderFrame();
  const auto* mounted_root = runtime.RootNode();
  REQUIRE(mounted_root != nullptr);
  const std::uint64_t node_id = mounted_root->identity;
  const RenderNode* before = FindRenderNode(*animation_start.scene.root, node_id);
  REQUIRE(before != nullptr);
  REQUIRE(!before->content.Commands().empty());
  const std::uint64_t before_revision = before->revision;
  const PaintCommand* before_commands = before->content.Commands().data();
  const Rect before_bounds = mounted_root->PresentationBounds();

  platform.AdvanceTime(0.5);
  const RenderFrame& animation_frame = runtime.BuildRenderFrame();
  const RenderNode* after = FindRenderNode(*animation_frame.scene.root, node_id);
  REQUIRE(after == before);
  REQUIRE(after->revision > before_revision);
  REQUIRE(after->transform.translate_x > 0.0F);
  REQUIRE(after->content.Commands().data() == before_commands);
  REQUIRE_FALSE(animation_frame.damage.full);
  const Rect after_bounds = mounted_root->PresentationBounds();
  REQUIRE(DamageContains(animation_frame.damage, before_bounds));
  REQUIRE(DamageContains(animation_frame.damage, after_bounds.Intersection({0.0F, 0.0F, 160.0F, 100.0F})));
}

TEST_CASE("FrameCommitSeparatesRuntimeWorkFromPlatformScheduling") {
  TestPlatform platform;
  platform.current_time = 12.5;
  Runtime runtime{PresentationReuseApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});
  runtime.BuildCommit();

  const int requests_before_invalidation = platform.requested_frames;
  presentation_reuse_moved = true;
  REQUIRE(platform.requested_frames == requests_before_invalidation + 1);
  REQUIRE(platform.requested_deadlines.back() == platform.current_time);

  const int requests_before_build = platform.requested_frames;
  const FrameCommit& commit = runtime.BuildCommit();
  REQUIRE(platform.requested_frames == requests_before_build);
  REQUIRE(commit.next_frame_deadline == platform.current_time);
}

TEST_CASE("ExternalTextureFramesReusePaintAndDamageOnlyTheVisibleTexture") {
  const auto texture_state = std::make_shared<ExternalTextureTestState>(Size{40.0F, 20.0F});
  render_scene_external_texture = texture_state->Texture();
  render_scene_external_texture_visible = true;
  TestPlatform platform;
  Runtime runtime{ExternalTextureRenderApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});

  const RenderFrame& first = runtime.BuildRenderFrame();
  const detail::MountedNode* mounted = runtime.RootNode();
  REQUIRE(mounted != nullptr);
  const RenderNode* before = FindRenderNode(*first.scene.root, mounted->identity);
  REQUIRE(before != nullptr);
  REQUIRE(before->content.Commands().size() == 1);
  REQUIRE(std::holds_alternative<DrawExternalTextureCommand>(before->content.Commands().front()));
  const std::uint64_t render_revision = before->revision;
  const std::uint64_t paint_revision = before->content.Revision();
  const PaintCommand* commands = before->content.Commands().data();

  const int requests_before_frame = platform.requested_frames;
  texture_state->PublishFrame();
  texture_state->PublishFrame();
  REQUIRE(platform.requested_frames == requests_before_frame);
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.requested_frames == requests_before_frame + 1);

  const RenderFrame& updated = runtime.BuildRenderFrame();
  const RenderNode* after = FindRenderNode(*updated.scene.root, mounted->identity);
  REQUIRE(after == before);
  REQUIRE(after->revision == render_revision);
  REQUIRE(after->content.Revision() == paint_revision);
  REQUIRE(after->content.Commands().data() == commands);
  REQUIRE_FALSE(updated.damage.full);
  REQUIRE(DamageContains(updated.damage, Rect{0.0F, 0.0F, 80.0F, 40.0F}));

  render_scene_external_texture_visible = false;
  runtime.InvalidateRoot();
  runtime.BuildRenderFrame();
  const int requests_after_hiding = platform.requested_frames;
  texture_state->PublishFrame();
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.requested_frames == requests_after_hiding);

  TestPlatform other_platform;
  Runtime other_runtime{ExternalTextureRenderApp, other_platform};
  render_scene_external_texture_visible = true;
  other_runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});
  REQUIRE_THROWS_AS(other_runtime.BuildRenderFrame(), std::logic_error);
}

TEST_CASE("InFramePaintInvalidationDoesNotScheduleRedundantWork") {
  TestPlatform platform;
  Runtime runtime{FramePaintInvalidationApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});

  const FrameCommit& commit = runtime.BuildCommit();

  REQUIRE_FALSE(commit.next_frame_deadline.has_value());
}

TEST_CASE("OpacityAnimationUpdatesOnlyTheOwningRenderNode") {
  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime{RetainedOpacityApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});
  runtime.BuildRenderFrame();

  retained_opacity_faded = true;
  runtime.BuildRenderFrame();
  const auto* mounted_root = runtime.RootNode();
  REQUIRE(mounted_root != nullptr);
  REQUIRE(mounted_root->children.size() == 2);
  const auto* text = mounted_root->children[0].get();
  const auto* checkbox = mounted_root->children[1].get();
  const std::uint64_t root_revision = mounted_root->render_node.revision;
  const std::uint64_t root_foreground_revision = mounted_root->render_node.foreground.Revision();
  const std::uint64_t text_content_revision = text->render_node.content.Revision();
  const std::uint64_t checkbox_foreground_revision = checkbox->render_node.foreground.Revision();
  const PaintCommand* text_commands = text->render_node.content.Commands().data();
  const PaintCommand* checkbox_commands = checkbox->render_node.foreground.Commands().data();

  platform.AdvanceTime(0.5);
  const RenderFrame& middle = runtime.BuildRenderFrame();
  REQUIRE(mounted_root->render_node.revision > root_revision);
  REQUIRE(std::abs(mounted_root->render_node.opacity - 0.5F) < 0.01F);
  REQUIRE(text->render_node.opacity == 1.0F);
  REQUIRE(checkbox->render_node.opacity == 1.0F);
  REQUIRE(std::abs(text->PresentationOpacity() - 0.5F) < 0.01F);
  REQUIRE(mounted_root->render_node.foreground.Revision() == root_foreground_revision);
  REQUIRE(text->render_node.content.Revision() == text_content_revision);
  REQUIRE(checkbox->render_node.foreground.Revision() == checkbox_foreground_revision);
  REQUIRE(text->render_node.content.Commands().data() == text_commands);
  REQUIRE(checkbox->render_node.foreground.Commands().data() == checkbox_commands);
  REQUIRE(std::get<DrawTextCommand>(text->render_node.content.Commands().front()).style.foreground.alpha == 1.0F);
  REQUIRE_FALSE(middle.damage.full);
  REQUIRE_FALSE(middle.damage.rects.empty());
}

TEST_CASE("ScrollViewUpdatesOnlyItsRetainedChildrenTransform") {
  TestPlatform platform;
  Runtime runtime{RetainedScrollApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 60.0F}});
  runtime.BuildRenderFrame();

  const auto* scroll_view = runtime.RootNode();
  REQUIRE(scroll_view != nullptr);
  REQUIRE(scroll_view->children.size() == 1);
  const auto* content = scroll_view->children[0].get();
  REQUIRE(content->children.size() == 3);
  const auto* first = content->children[0].get();
  const Point content_offset = content->layout_offset;
  const std::uint64_t scroll_revision = scroll_view->render_node.revision;
  const std::uint64_t content_revision = content->render_node.revision;
  const std::uint64_t first_revision = first->render_node.revision;
  const std::uint64_t first_content_revision = first->render_node.content.Revision();
  const PaintCommand* first_commands = first->render_node.content.Commands().data();
  const Rect first_bounds = first->PresentationBounds();

  runtime.HandleScrollEvent(ScrollEvent{{50.0F, 30.0F}, 0.0F, 20.0F});
  const RenderFrame& scrolled = runtime.BuildRenderFrame();

  REQUIRE(scroll_view->scroll_state->offset_y == 20.0F);
  REQUIRE(content->layout_offset.x == content_offset.x);
  REQUIRE(content->layout_offset.y == content_offset.y);
  REQUIRE(scroll_view->render_node.children_transform.translate_x == 0.0F);
  REQUIRE(scroll_view->render_node.children_transform.translate_y == -20.0F);
  REQUIRE(scroll_view->render_node.revision > scroll_revision);
  REQUIRE(content->render_node.revision == content_revision);
  REQUIRE(first->render_node.revision == first_revision);
  REQUIRE(first->render_node.content.Revision() == first_content_revision);
  REQUIRE(first->render_node.content.Commands().data() == first_commands);
  REQUIRE(first->PresentationBounds().y == first_bounds.y - 20.0F);
  REQUIRE_FALSE(scrolled.damage.full);
  REQUIRE(DamageContains(scrolled.damage, {0.0F, 0.0F, 100.0F, 60.0F}));
}

TEST_CASE("ViewportChangesProduceFullDamage") {
  TestPlatform platform;
  platform.platform_resources = BuiltinTestResources();
  Runtime runtime{RenderSceneApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});
  runtime.BuildRenderFrame();

  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  const RenderFrame& resized = runtime.BuildRenderFrame();
  REQUIRE(resized.damage.full);
  REQUIRE(resized.damage.rects.size() == 1);
  REQUIRE(resized.damage.rects[0].x == 0.0F);
  REQUIRE(resized.damage.rects[0].y == 0.0F);
  REQUIRE(resized.damage.rects[0].width == 200.0F);
  REQUIRE(resized.damage.rects[0].height == 120.0F);

  runtime.SetWindowMetrics({.viewport = {}});
  REQUIRE(runtime.BuildRenderFrame().scene.root == nullptr);
  runtime.SetWindowMetrics({.viewport = {200.0F, 120.0F}});
  const RenderFrame& restored = runtime.BuildRenderFrame();
  REQUIRE(restored.damage.full);
  REQUIRE(restored.damage.rects.size() == 1);
}

TEST_CASE("RemovedAndInsertedNodesDamageTheirCommittedBounds") {
  TestPlatform platform;
  Runtime runtime{RemovalDamageApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});

  const RenderFrame& first_frame = runtime.BuildRenderFrame();
  REQUIRE(first_frame.scene.root != nullptr);
  const auto* mounted_root = runtime.RootNode();
  REQUIRE(mounted_root != nullptr);
  const Rect committed_bounds = mounted_root->PresentationBounds();

  removal_damage_visible = false;
  const RenderFrame& removed = runtime.BuildRenderFrame();
  REQUIRE_FALSE(removed.damage.full);
  REQUIRE(DamageContains(removed.damage, committed_bounds));

  removal_damage_visible = true;
  const RenderFrame& inserted = runtime.BuildRenderFrame();
  REQUIRE_FALSE(inserted.damage.full);
  REQUIRE(DamageContains(inserted.damage, committed_bounds));
}

TEST_CASE("ReorderedRenderChildrenDamageTheirSharedBounds") {
  TestPlatform platform;
  Runtime runtime{ChildOrderDamageApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});
  runtime.BuildRenderFrame();

  const auto* mounted_root = runtime.RootNode();
  REQUIRE(mounted_root != nullptr);
  REQUIRE(mounted_root->children.size() == 2);
  const std::uint64_t first_id = mounted_root->children[0]->identity;
  const std::uint64_t second_id = mounted_root->children[1]->identity;
  const Rect bounds = mounted_root->children[0]->PresentationBounds();

  child_order_reversed = true;
  const RenderFrame& reordered = runtime.BuildRenderFrame();
  mounted_root = runtime.RootNode();
  REQUIRE(mounted_root->children[0]->identity == second_id);
  REQUIRE(mounted_root->children[1]->identity == first_id);
  REQUIRE_FALSE(reordered.damage.full);
  REQUIRE(DamageContains(reordered.damage, bounds));
}

TEST_CASE("ClipChangesDamageOldAndNewClippedSubtreeBounds") {
  TestPlatform platform;
  Runtime runtime{ClipDamageApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});
  runtime.BuildRenderFrame();

  clip_damage_expanded = true;
  const RenderFrame& expanded = runtime.BuildRenderFrame();
  REQUIRE_FALSE(expanded.damage.full);
  REQUIRE(DamageContains(expanded.damage, Rect{0.0F, 0.0F, 100.0F, 60.0F}));
}

TEST_CASE("OffscreenParentsRemainVisibleWhenAnOverflowingChildIsVisible") {
  TestPlatform platform;
  Runtime runtime{OverflowingChildApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});

  const RenderFrame& frame = runtime.BuildRenderFrame();
  REQUIRE(frame.scene.root != nullptr);
  const auto* mounted_root = runtime.RootNode();
  REQUIRE(mounted_root != nullptr);
  REQUIRE(mounted_root->children.size() == 1);
  const auto* parent = mounted_root->children[0].get();
  REQUIRE(parent->children.size() == 1);
  const RenderNode* parent_render_node = FindRenderNode(*frame.scene.root, parent->identity);
  const RenderNode* child = FindRenderNode(*frame.scene.root, parent->children[0]->identity);
  REQUIRE(parent_render_node != nullptr);
  REQUIRE(parent_render_node->visible);
  REQUIRE(child != nullptr);
  REQUIRE(child->visible);
}

TEST_CASE("OffscreenClipsKeepOverflowingChildrenInvisible") {
  TestPlatform platform;
  Runtime runtime{ClippedOverflowingChildApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});

  const RenderFrame& frame = runtime.BuildRenderFrame();
  REQUIRE(frame.scene.root != nullptr);
  const auto* mounted_root = runtime.RootNode();
  REQUIRE(mounted_root != nullptr);
  REQUIRE(mounted_root->children.size() == 1);
  const auto* parent = mounted_root->children[0].get();
  REQUIRE(parent->children.size() == 1);
  const RenderNode* parent_render_node = FindRenderNode(*frame.scene.root, parent->identity);
  const RenderNode* child = FindRenderNode(*frame.scene.root, parent->children[0]->identity);
  REQUIRE(parent_render_node != nullptr);
  REQUIRE_FALSE(parent_render_node->visible);
  REQUIRE(child != nullptr);
  REQUIRE_FALSE(child->visible);
}

TEST_CASE("PaintBoundsKeepOffscreenNodesVisibleWhenTheirCommandsOverflowIntoTheViewport") {
  TestPlatform platform;
  Runtime runtime{OverflowingPaintApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});

  const RenderFrame& first = runtime.BuildRenderFrame();
  const auto* mounted = runtime.RootNode();
  REQUIRE(mounted != nullptr);
  const RenderNode* render_node = FindRenderNode(*first.scene.root, mounted->identity);
  REQUIRE(render_node != nullptr);
  REQUIRE(render_node->visible);
  REQUIRE(render_node->foreground.Bounds() == Rect{-200.0F, 0.0F, 80.0F, 20.0F});
  REQUIRE(std::get<DrawRectCommand>(render_node->foreground.Commands().front()).color == Color::Black());

  overflowing_paint_changed = true;
  const RenderFrame& changed = runtime.BuildRenderFrame();
  render_node = FindRenderNode(*changed.scene.root, mounted->identity);
  REQUIRE(render_node != nullptr);
  REQUIRE(render_node->visible);
  REQUIRE(std::get<DrawRectCommand>(render_node->foreground.Commands().front()).color == Color::White());
  REQUIRE_FALSE(changed.damage.full);
  REQUIRE(DamageContains(changed.damage, {0.0F, 0.0F, 80.0F, 20.0F}));
}

TEST_CASE("ShadowsPaintBehindContentAndInvalidateTheirOverflow") {
  TestPlatform platform;
  Runtime runtime{ShadowApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});

  const RenderFrame& first = runtime.BuildRenderFrame();
  const auto* mounted_root = runtime.RootNode();
  REQUIRE(mounted_root != nullptr);
  REQUIRE(mounted_root->children.size() == 1);
  const auto* mounted = mounted_root->children.front().get();
  const RenderNode* render_node = FindRenderNode(*first.scene.root, mounted->identity);
  REQUIRE(render_node != nullptr);
  REQUIRE(render_node->content.Commands().size() == 2);
  const auto* shadow = std::get_if<DrawShadowCommand>(&render_node->content.Commands()[0]);
  REQUIRE(shadow != nullptr);
  REQUIRE(shadow->corner_radius == 6.0F);
  REQUIRE(std::holds_alternative<DrawRectCommand>(render_node->content.Commands()[1]));
  REQUIRE(render_node->content.Bounds() == Rect{-4.0F, -3.0F, 176.0F, 36.0F});

  shadow_changed = true;
  const RenderFrame& changed = runtime.BuildRenderFrame();
  render_node = FindRenderNode(*changed.scene.root, mounted->identity);
  REQUIRE(render_node != nullptr);
  shadow = std::get_if<DrawShadowCommand>(&render_node->content.Commands()[0]);
  REQUIRE(shadow != nullptr);
  REQUIRE(shadow->color.alpha == 0.4F);
  REQUIRE_FALSE(changed.damage.full);
  REQUIRE(DamageContains(changed.damage, Rect{0.0F, 0.0F, 160.0F, 33.0F}));
}

TEST_CASE("CanvasRecordsInContentLocalCoordinatesAndReusesCleanPaint") {
  canvas_paint_count = 0;
  TestPlatform platform;
  Runtime runtime{CanvasApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});

  const RenderFrame& first = runtime.BuildRenderFrame();
  const auto* mounted_root = runtime.RootNode();
  REQUIRE(mounted_root != nullptr);
  REQUIRE(mounted_root->children.size() == 1);
  const auto* mounted = mounted_root->children.front().get();
  const RenderNode* render_node = FindRenderNode(*first.scene.root, mounted->identity);
  REQUIRE(render_node != nullptr);
  REQUIRE(canvas_paint_count == 1);
  REQUIRE(render_node->content.Commands().size() == 3);
  REQUIRE(std::holds_alternative<PushTransformCommand>(render_node->content.Commands()[0]));
  REQUIRE(std::get<PushTransformCommand>(render_node->content.Commands()[0]).transform.translate_x == 10.0F);
  REQUIRE(std::get<PushTransformCommand>(render_node->content.Commands()[0]).transform.translate_y == 10.0F);
  REQUIRE(std::get<DrawRectCommand>(render_node->content.Commands()[1]).color == Color::Black());
  REQUIRE(std::holds_alternative<PopTransformCommand>(render_node->content.Commands()[2]));
  REQUIRE(render_node->content.Bounds() == Rect{10.0F, 10.0F, 60.0F, 40.0F});
  const std::uint64_t first_revision = render_node->revision;

  runtime.BuildRenderFrame();
  REQUIRE(canvas_paint_count == 1);

  canvas_changed = true;
  const RenderFrame& changed = runtime.BuildRenderFrame();
  render_node = FindRenderNode(*changed.scene.root, mounted->identity);
  REQUIRE(render_node != nullptr);
  REQUIRE(canvas_paint_count == 2);
  REQUIRE(render_node->revision > first_revision);
  REQUIRE(std::get<DrawRectCommand>(render_node->content.Commands()[1]).color == Color::White());
  REQUIRE_FALSE(changed.damage.full);
  REQUIRE(DamageContains(changed.damage, Rect{10.0F, 10.0F, 60.0F, 40.0F}));
}

TEST_CASE("AncestorClipsHideOverflowingPaintOutsideTheirViewport") {
  TestPlatform platform;
  Runtime runtime{ClippedOverflowingPaintApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});

  const RenderFrame& frame = runtime.BuildRenderFrame();
  const auto* scroll_view = runtime.RootNode();
  REQUIRE(scroll_view != nullptr);
  REQUIRE(scroll_view->children.size() == 1);
  const RenderNode* child = FindRenderNode(*frame.scene.root, scroll_view->children.front()->identity);
  REQUIRE(child != nullptr);
  REQUIRE_FALSE(child->visible);
  REQUIRE_FALSE(scroll_view->render_node.visible);
}

TEST_CASE("ShadowModifierRejectsInvalidValues") {
  const float nan = std::numeric_limits<float>::quiet_NaN();

  REQUIRE_THROWS_AS(
      Spacer().With(Shadow{.color = Color::Black(), .blur_radius = -1.0F}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      Spacer().With(Shadow{.color = Color::Black(), .blur_radius = nan}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      Spacer().With(Shadow{.color = Color::Black(), .spread = nan}),
      std::invalid_argument
  );
}

} // namespace huxerui::test
