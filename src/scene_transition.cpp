#include <huxerui/animation.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include <huxerui/root.h>
#include <huxerui/state.h>
#include <huxerui/theme.h>

#include "runtime_internal.h"
#include "window_internal.h"

namespace huxerui::detail {

namespace {

Path CircularClip(Point center, float radius) {
  constexpr float cubic_circle = 0.5522847498F;
  const float control = radius * cubic_circle;
  Path path;
  path.MoveTo({center.x + radius, center.y})
      .CubicTo(
          {center.x + radius, center.y + control},
          {center.x + control, center.y + radius},
          {center.x, center.y + radius}
      )
      .CubicTo(
          {center.x - control, center.y + radius},
          {center.x - radius, center.y + control},
          {center.x - radius, center.y}
      )
      .CubicTo(
          {center.x - radius, center.y - control},
          {center.x - control, center.y - radius},
          {center.x, center.y - radius}
      )
      .CubicTo(
          {center.x + control, center.y - radius},
          {center.x + radius, center.y - control},
          {center.x + radius, center.y}
      )
      .Close();
  return path;
}

float CircularRevealRadius(Point origin, Size viewport) noexcept {
  const float horizontal = std::max(std::abs(origin.x), std::abs(viewport.width - origin.x));
  const float vertical = std::max(std::abs(origin.y), std::abs(viewport.height - origin.y));
  return std::hypot(horizontal, vertical);
}

void ResetSceneTransitionNode(RenderNode& node, std::uint64_t identity) {
  node.id = identity;
  node.offset = {};
  node.transform = {};
  node.opacity = 1.0F;
  node.child_clips.clear();
  node.children_transform = {};
  node.content = {};
  node.children.clear();
  node.foreground = {};
  node.visible = true;
  ++node.revision;
}

} // namespace

const RenderNode* SceneTransitionService::Compose(const RenderNode* live_root) {
  if (!active_) {
    return live_root;
  }
  ActiveTransition& transition = *active_;
  const float progress = std::clamp(transition.progress.Value(), 0.0F, 1.0F);
  ResetSceneTransitionNode(transition.composite, std::numeric_limits<std::uint64_t>::max());
  ResetSceneTransitionNode(transition.old_wrapper, std::numeric_limits<std::uint64_t>::max() - 1);
  ResetSceneTransitionNode(transition.new_wrapper, std::numeric_limits<std::uint64_t>::max() - 2);
  transition.old_wrapper.children = {transition.frozen->root};

  const bool has_platform_views = RenderSceneHasPlatformViews(live_root);
  if (has_platform_views) {
    // PlatformViews stay live and outside group opacity or path clips; only the frozen render scene fades over them.
    transition.old_wrapper.opacity = 1.0F - progress;
    transition.composite.children = {live_root, &transition.old_wrapper};
    return &transition.composite;
  }

  transition.new_wrapper.children = {live_root};
  if (transition.request.kind == SceneTransitionKind::Fade) {
    transition.old_wrapper.opacity = 1.0F - progress;
    transition.new_wrapper.opacity = progress;
    transition.composite.children = {&transition.old_wrapper, &transition.new_wrapper};
    return &transition.composite;
  }

  const float radius = CircularRevealRadius(transition.request.origin, transition.viewport) * progress;
  transition.new_wrapper.child_clips.push_back(PushPathClipCommand{
      CircularClip(transition.request.origin, radius),
      PathFillRule::NonZero,
  });
  transition.composite.children = {transition.frozen->root, &transition.new_wrapper};
  return &transition.composite;
}

void SceneTransitionAnchorState::Mount() {
  if (mounted) {
    throw std::logic_error("HuxerUI scene transition anchor must be mounted on only one View");
  }
  mounted = true;
}

void SceneTransitionAnchorState::Unmount() noexcept {
  mounted = false;
  bounds.reset();
}

void SceneTransitionAnchorState::UpdateBounds(Rect next_bounds) noexcept {
  bounds = next_bounds;
}

std::optional<Point> SceneTransitionAnchorState::Center() const noexcept {
  if (!mounted || !bounds.has_value()) {
    return std::nullopt;
  }
  return Point{
      bounds->x + bounds->width * 0.5F,
      bounds->y + bounds->height * 0.5F,
  };
}

std::shared_ptr<SceneTransitionAnchorState> SceneTransitionService::CreateAnchor() const {
  return std::make_shared<SceneTransitionAnchorState>();
}

std::optional<Point> SceneTransitionService::CurrentInteractionOrigin() const noexcept {
  return runtime_state_ == nullptr ? std::nullopt : runtime_state_->current_interaction_origin_;
}

void SceneTransitionService::Run(
    SceneTransitionRequest request, std::function<void()> mutation, bool reduced_motion
) {
  if (runtime_state_ == nullptr) {
    throw std::logic_error("HuxerUI scene transition service is disconnected");
  }
  if (!mutation) {
    throw std::invalid_argument("HuxerUI scene transition mutation must not be empty");
  }
  if (const auto* spring = std::get_if<SpringSpec>(&request.animation); spring && spring->damping_ratio == 0.0F) {
    throw std::invalid_argument("HuxerUI scene transition spring damping ratio must be positive");
  }

  MotionController progress{0.0F};
  progress.AnimateTo(1.0F, request.animation, AnimationPlayback{.delay = request.delay});
  const RenderNode* const committed_root = runtime_state_->frame_commit_.render_frame.scene.root;
  if (reduced_motion || committed_root == nullptr) {
    mutation();
    return;
  }

  std::shared_ptr<FrozenScene> frozen = FreezeRenderScene(committed_root);
  mutation();
  ActiveTransition transition;
  transition.request = std::move(request);
  transition.frozen = std::move(frozen);
  transition.progress = std::move(progress);
  transition.viewport = runtime_state_->window_->metrics.viewport;
  active_ = std::move(transition);
  runtime_state_->owner_.RequestFrame();
}

bool SceneTransitionService::IsActive() const noexcept {
  return active_.has_value();
}

MotionAdvanceResult SceneTransitionService::Advance(const FrameInfo& frame) {
  if (!active_) {
    return {};
  }
  ActiveTransition& transition = *active_;
  if (transition.viewport != runtime_state_->window_->metrics.viewport) {
    active_.reset();
    return {};
  }
  const MotionAdvanceResult result = transition.progress.Advance(frame);
  if (!transition.progress.IsRunning()) {
    active_.reset();
  }
  return result;
}

void SceneTransitionService::Disconnect() noexcept {
  runtime_state_ = nullptr;
  active_.reset();
}

class SceneTransitionAnchorExtension final : public NodeExtension {
public:
  SceneTransitionAnchorExtension(huxerui::MountedNode& node, const SceneTransitionAnchor& modifier) {
    Update(node, modifier);
  }

  ~SceneTransitionAnchorExtension() override {
    if (state_) {
      state_->Unmount();
    }
  }

  void Update(huxerui::MountedNode& node, const SceneTransitionAnchor& modifier) {
    static_cast<void>(node);
    if (state_ == modifier.state_) {
      return;
    }
    if (state_) {
      state_->Unmount();
    }
    state_ = modifier.state_;
    if (state_) {
      state_->Mount();
    }
  }

  PaintInvalidation PrepareGeometry(huxerui::MountedNode& node, huxerui::TextMeasurer&) override {
    if (state_) {
      state_->UpdateBounds(node.PresentationBounds());
    }
    return PaintInvalidation::None;
  }

private:
  std::shared_ptr<SceneTransitionAnchorState> state_;
};

} // namespace huxerui::detail

namespace huxerui {

SceneTransitionAnchor SceneTransitionHandle::Anchor() const {
  return SceneTransitionAnchor{anchor_};
}

void SceneTransitionHandle::Run(FadeSceneTransition transition, std::function<void()> mutation) const {
  service_->Run(
      detail::SceneTransitionRequest{
          .kind = detail::SceneTransitionKind::Fade,
          .animation = std::move(transition.animation),
          .delay = transition.delay,
          .origin = {},
      },
      std::move(mutation),
      reduced_motion_
  );
}

void SceneTransitionHandle::Run(CircularRevealSceneTransition transition, std::function<void()> mutation) const {
  const std::optional<Point> origin = anchor_ ? anchor_->Center() : std::nullopt;
  if (!origin.has_value()) {
    throw std::logic_error("HuxerUI circular scene transition requires a mounted anchor");
  }
  RunAt(*origin, std::move(transition), std::move(mutation));
}

void SceneTransitionHandle::RunAt(
    Point origin, CircularRevealSceneTransition transition, std::function<void()> mutation
) const {
  if (!std::isfinite(origin.x) || !std::isfinite(origin.y)) {
    throw std::invalid_argument("HuxerUI scene transition origin must be finite");
  }
  service_->Run(
      detail::SceneTransitionRequest{
          .kind = detail::SceneTransitionKind::CircularReveal,
          .animation = std::move(transition.animation),
          .delay = transition.delay,
          .origin = origin,
      },
      std::move(mutation),
      reduced_motion_
  );
}

void SceneTransitionHandle::RunFromCurrentInteraction(
    CircularRevealSceneTransition transition, std::function<void()> mutation
) const {
  const std::optional<Point> origin = service_->CurrentInteractionOrigin();
  if (!origin.has_value()) {
    throw std::logic_error("HuxerUI scene transition requires a current interaction origin");
  }
  RunAt(*origin, std::move(transition), std::move(mutation));
}

SceneTransitionHandle UseSceneTransition() {
  const std::shared_ptr<detail::SceneTransitionService> service = UseService<detail::SceneTransitionService>();
  auto anchor = UseState(service->CreateAnchor());
  return SceneTransitionHandle{service, anchor.Get(), UseTheme().motion.reduced_motion};
}

const detail::ModifierDescriptor& SceneTransitionAnchor::Descriptor() {
  return detail::ModifierDescriptorFor<SceneTransitionAnchor, detail::SceneTransitionAnchorExtension>();
}

} // namespace huxerui
