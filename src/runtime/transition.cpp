#include <huxerui/animation.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <limits>
#include <utility>

#include <huxerui/root.h>
#include <huxerui/state.h>
#include <huxerui/theme.h>

#include "runtime_internal.h"
#include "internal_access.h"
#include "application/window_internal.h"
#include "graphics/geometry_internal.h"

namespace huxerui {
namespace {

thread_local unsigned evaluation_depth = 0;

void ValidateBounds(Rect bounds) {
  if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) || !std::isfinite(bounds.width) ||
      !std::isfinite(bounds.height) || bounds.width < 0.0F || bounds.height < 0.0F) {
    throw std::invalid_argument("HuxerUI transition bounds must be finite with non-negative dimensions");
  }
}

void ValidatePoint(Point point) {
  if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
    throw std::invalid_argument("HuxerUI transition point must be finite");
  }
}

void ValidateVisual(const Transform2D& t, float& opacity) {
  if (!std::isfinite(t.m11) || !std::isfinite(t.m12) || !std::isfinite(t.m21) || !std::isfinite(t.m22) ||
      !std::isfinite(t.translate_x) || !std::isfinite(t.translate_y) || !std::isfinite(opacity)) {
    throw std::invalid_argument("HuxerUI transition output must be finite");
  }
  opacity = std::clamp(opacity, 0.0F, 1.0F);
}

void ValidateSample(TransitionSample& sample) {
  ValidateVisual(sample.transform, sample.opacity);
  for (TransitionFragment& fragment : sample.fragments) {
    ValidateVisual(fragment.transform, fragment.opacity);
  }
}

void ValidateContext(const TransitionContext& context) {
  ValidateBounds(context.bounds);
  if (!std::isfinite(context.progress)) {
    throw std::invalid_argument("HuxerUI transition progress must be finite");
  }
  if (context.origin) { ValidatePoint(*context.origin); }
}

struct EvaluationScope {
  EvaluationScope() { ++evaluation_depth; }
  ~EvaluationScope() { --evaluation_depth; }
};

} // namespace

TransitionFrame FadeTransition::Evaluate(const TransitionContext& context) const {
  const float progress = std::clamp(context.progress, 0.0F, 1.0F);
  TransitionFrame frame;
  frame.outgoing.opacity = 1.0F - progress;
  frame.incoming.opacity = progress;
  return frame;
}

TransitionFrame SlideTransition::Evaluate(const TransitionContext& context) const {
  ValidatePoint(incoming_offset);
  ValidatePoint(outgoing_offset);
  TransitionFrame frame;
  frame.outgoing.transform.translate_x = outgoing_offset.x * context.bounds.width * context.progress;
  frame.outgoing.transform.translate_y = outgoing_offset.y * context.bounds.height * context.progress;
  frame.incoming.transform.translate_x = incoming_offset.x * context.bounds.width * (1.0F - context.progress);
  frame.incoming.transform.translate_y = incoming_offset.y * context.bounds.height * (1.0F - context.progress);
  return frame;
}

TransitionFrame ScaleFadeTransition::Evaluate(const TransitionContext& context) const {
  ValidatePoint({origin.x, origin.y});
  if (!std::isfinite(incoming_scale) || !std::isfinite(outgoing_scale) || incoming_scale < 0.0F ||
      outgoing_scale < 0.0F) {
    throw std::invalid_argument("HuxerUI transition scale must be finite and non-negative");
  }
  TransitionFrame frame = FadeTransition{}.Evaluate(context);
  const Point pivot{context.bounds.x + context.bounds.width * origin.x,
                    context.bounds.y + context.bounds.height * origin.y};
  const auto scaled = [pivot](float scale) {
    return detail::AroundOriginTransform(Transform2D{scale, 0.0F, 0.0F, scale}, pivot);
  };
  frame.incoming.transform = scaled(incoming_scale + (1.0F - incoming_scale) * context.progress);
  frame.outgoing.transform = scaled(1.0F + (outgoing_scale - 1.0F) * context.progress);
  return frame;
}

TransitionFrame CircularRevealTransition::Evaluate(const TransitionContext& context) const {
  if (!context.origin) {
    throw std::logic_error("HuxerUI circular transition requires an origin");
  }
  ValidatePoint(*context.origin);
  const float horizontal = std::max(std::abs(context.origin->x - context.bounds.x),
      std::abs(context.bounds.x + context.bounds.width - context.origin->x));
  const float vertical = std::max(std::abs(context.origin->y - context.bounds.y),
      std::abs(context.bounds.y + context.bounds.height - context.origin->y));
  TransitionFrame frame;
  frame.incoming.clip = ClipShape::Circle(*context.origin,
      std::hypot(horizontal, vertical) * std::clamp(context.progress, 0.0F, 1.0F));
  return frame;
}

void TransitionSpec::ValidateTiming() const {
  if (const auto* spring = std::get_if<SpringSpec>(&animation_); spring && spring->damping_ratio <= 0.0F) {
    throw std::invalid_argument("HuxerUI transition spring damping ratio must be positive");
  }
  MotionController validator;
  validator.AnimateTo(1.0F, animation_, AnimationPlayback{.delay = delay_});
}

TransitionFrame TransitionSpec::Evaluate(const TransitionContext& context) const {
  ValidateContext(context);
  EvaluationScope evaluation_scope;
  TransitionContext input = context;
  if (reversed_) {
    input.progress = 1.0F - input.progress;
  }
  TransitionFrame frame = effect_ ? effect_->Evaluate(input) : FadeTransition{}.Evaluate(input);
  ValidateSample(frame.outgoing);
  ValidateSample(frame.incoming);
  if (frame.order != TransitionOrder::IncomingAbove && frame.order != TransitionOrder::OutgoingAbove) {
    throw std::invalid_argument("HuxerUI transition drawing order is invalid");
  }
  if (reversed_) {
    std::swap(frame.outgoing, frame.incoming);
    frame.order = frame.order == TransitionOrder::IncomingAbove ? TransitionOrder::OutgoingAbove
                                                               : TransitionOrder::IncomingAbove;
  }
  return frame;
}

void TransitionSpec::Paint(PaintContext& paint, const TransitionContext& context) const {
  ValidateContext(context);
  EvaluationScope evaluation_scope;
  TransitionContext input = context;
  if (reversed_) { input.progress = 1.0F - input.progress; }
  if (effect_) { effect_->Paint(paint, input); }
}

bool detail::InternalAccess::HasTransitionPaint(const TransitionSpec& transition) noexcept {
  return transition.effect_ && transition.effect_->HasPaint();
}

TransitionSpec TransitionSpec::Reversed() const {
  TransitionSpec reversed = *this;
  reversed.reversed_ = !reversed.reversed_;
  return reversed;
}

TransitionSpec TransitionSpec::Reversed(AnimationSpec animation) const {
  TransitionSpec reversed = Reversed();
  reversed.animation_ = std::move(animation);
  reversed.ValidateTiming();
  return reversed;
}

bool TransitionSpec::operator==(const TransitionSpec& other) const {
  return animation_ == other.animation_ && delay_ == other.delay_ && reversed_ == other.reversed_ &&
      (effect_ == other.effect_ || (effect_ && other.effect_ && effect_->Equals(*other.effect_)));
}

bool TransitionSpec::IsImmediate() const noexcept {
  if (delay_ != 0.0) {
    return false;
  }
  const auto* tween = std::get_if<TweenSpec>(&animation_);
  return std::holds_alternative<SnapSpec>(animation_) || (tween && tween->duration == 0.0);
}

} // namespace huxerui

namespace huxerui::detail {

namespace {

void ResetSceneTransitionNode(RenderNode& node, std::uint64_t identity) {
  node.id = identity;
  node.offset = {};
  node.transform = {};
  node.opacity = 1.0F;
  node.child_clips.clear();
  node.children_transform = {};
  node.content = {};
  node.children.clear();
  node.visible = true;
  ++node.revision;
}

} // namespace

const RenderNode* SceneTransitionService::Compose(const RenderNode* live_root) {
  if (!active_) {
    return live_root;
  }
  ActiveTransition& transition = *active_;
  const TransitionContext context{
      transition.progress.Value(), {0.0F, 0.0F, transition.viewport.width, transition.viewport.height},
      transition.request.origin,
  };
  TransitionFrame frame;
  try {
    frame = transition.request.transition.Evaluate(context);
  } catch (...) {
    ClearActive();
    runtime_state_->owner_.RequestFrame();
    throw;
  }
  ResetSceneTransitionNode(transition.composite, std::numeric_limits<std::uint64_t>::max());
  ResetSceneTransitionNode(transition.old_wrapper, std::numeric_limits<std::uint64_t>::max() - 1);
  ResetSceneTransitionNode(transition.new_wrapper, std::numeric_limits<std::uint64_t>::max() - 2);
  transition.old_wrapper.children = {transition.frozen->root};
  if (RenderSceneHasPlatformViews(live_root)) {
    // Native Views remain live; unsupported group effects fade the frozen visual above the live tree.
    transition.old_wrapper.opacity = 1.0F - std::clamp(transition.progress.Value(), 0.0F, 1.0F);
    transition.composite.foreground = {};
    transition.painted_context.reset();
    transition.old_fragments.Clear();
    transition.new_fragments.Clear();
    transition.composite.children = {live_root, &transition.old_wrapper};
    return &transition.composite;
  }
  transition.new_wrapper.children = {live_root};
  const auto apply = [](RenderNode& node, const TransitionSample& sample) {
    node.children_transform = sample.transform;
    node.opacity = sample.opacity;
    if (sample.clip) {
      node.child_clips.push_back(InternalAccess::RenderClipShape(*sample.clip));
    }
  };
  apply(transition.old_wrapper, frame.outgoing);
  apply(transition.new_wrapper, frame.incoming);
  try {
    transition.old_fragments.Update(transition.old_wrapper.children, frame.outgoing.fragments);
    transition.new_fragments.Update(transition.new_wrapper.children, frame.incoming.fragments);
    transition.old_wrapper.children = transition.old_fragments.roots;
    transition.new_wrapper.children = transition.new_fragments.roots;
    if (InternalAccess::HasTransitionPaint(transition.request.transition) &&
        (!transition.painted_context || transition.painted_context->progress != context.progress ||
         transition.painted_context->bounds != context.bounds || transition.painted_context->origin != context.origin)) {
      PaintContext paint{transition.composite.foreground, context.bounds};
      paint.PushClip(context.bounds);
      transition.request.transition.Paint(paint, context);
      paint.PopClip();
      paint.Finish();
      transition.painted_context = context;
    }
  } catch (...) {
    ClearActive();
    runtime_state_->owner_.RequestFrame();
    throw;
  }
  transition.composite.children = frame.order == TransitionOrder::IncomingAbove
      ? std::vector<const RenderNode*>{&transition.old_wrapper, &transition.new_wrapper}
      : std::vector<const RenderNode*>{&transition.new_wrapper, &transition.old_wrapper};
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
  if (mutating_ || evaluation_depth != 0) {
    throw std::logic_error("HuxerUI scene transitions cannot start recursively from mutation or effect evaluation");
  }
  const Size viewport = runtime_state_->window_->metrics.viewport;
  static_cast<void>(request.transition.Evaluate({0.0F, {0.0F, 0.0F, viewport.width, viewport.height}, request.origin}));
  MotionController progress{0.0F};
  progress.AnimateTo(1.0F, request.transition.Animation(), AnimationPlayback{.delay = request.transition.Delay()});
  const RenderNode* const committed_root = runtime_state_->frame_commit_.render_frame.scene.root;
  if (!reduced_motion && !request.transition.IsImmediate() && committed_root &&
      InternalAccess::HasTransitionPaint(request.transition)) {
    PaintSequence preflight;
    PaintContext paint{preflight, {0.0F, 0.0F, viewport.width, viewport.height}};
    request.transition.Paint(paint, {0.0F, paint.Bounds(), request.origin});
    paint.Finish();
  }
  std::shared_ptr<FrozenScene> frozen;
  if (!reduced_motion && !request.transition.IsImmediate() && committed_root != nullptr) {
    frozen = FreezeRenderScene(committed_root);
  }
  mutating_ = true;
  try {
    mutation();
  } catch (...) {
    mutating_ = false;
    ClearActive();
    runtime_state_->owner_.RequestFrame();
    throw;
  }
  mutating_ = false;
  if (!frozen) {
    ClearActive();
    runtime_state_->owner_.RequestFrame();
    return;
  }
  ActiveTransition transition;
  transition.request = std::move(request);
  transition.frozen = std::move(frozen);
  transition.progress = std::move(progress);
  transition.viewport = viewport;
  active_ = std::move(transition);
  // Until the next composition, this immutable copy is the last committed visual, including rapid same-frame runs.
  runtime_state_->frame_commit_.render_frame.scene.root = active_->frozen->root;
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
    ClearActive();
    return {};
  }
  const MotionAdvanceResult result = transition.progress.Advance(frame);
  if (!transition.progress.IsRunning()) {
    ClearActive();
  }
  return result;
}

void SceneTransitionService::ClearActive() noexcept {
  if (active_ && runtime_state_) {
    runtime_state_->frame_commit_.render_frame.scene.root = nullptr;
  }
  active_.reset();
}

void SceneTransitionService::Disconnect() noexcept {
  ClearActive();
  runtime_state_ = nullptr;
}

class SceneTransitionAnchorExtension final : public NodeExtension {
public:
  SceneTransitionAnchorExtension(huxerui::ViewNode& node, const SceneTransitionAnchor& modifier) {
    Update(node, modifier);
  }

  ~SceneTransitionAnchorExtension() override {
    if (state_) {
      state_->Unmount();
    }
  }

  void Update(huxerui::ViewNode& node, const SceneTransitionAnchor& modifier) {
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

  PaintInvalidation PrepareGeometry(huxerui::ViewNode& node, huxerui::TextMeasurer&) override {
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

void SceneTransitionHandle::Run(TransitionSpec transition, std::function<void()> mutation) const {
  const std::optional<Point> origin = anchor_ ? anchor_->Center() : std::nullopt;
  service_->Run({std::move(transition), origin}, std::move(mutation), reduced_motion_);
}

void SceneTransitionHandle::RunAt(Point origin, TransitionSpec transition, std::function<void()> mutation) const {
  if (!std::isfinite(origin.x) || !std::isfinite(origin.y)) {
    throw std::invalid_argument("HuxerUI scene transition origin must be finite");
  }
  service_->Run({std::move(transition), origin}, std::move(mutation), reduced_motion_);
}

void SceneTransitionHandle::RunFromCurrentInteraction(
    TransitionSpec transition, std::function<void()> mutation
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
