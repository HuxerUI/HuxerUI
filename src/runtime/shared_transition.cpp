#include <huxerui/animation.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <thread>
#include <utility>

#include <huxerui/state.h>

#include "transition_internal.h"
#include "mounted_node_internal.h"
#include "internal_access.h"

namespace huxerui::detail {

struct SharedVisual {
  SharedMarkerData marker;
  bool crossfade = false;
  std::uint64_t node_id = 0;
  std::vector<std::uint64_t> ancestors;
  std::vector<std::uint64_t> captured_nodes;
  Rect bounds;
  float opacity = 1.0F;
  bool eligible = false;
  std::shared_ptr<FrozenScene> scene;
};

struct SharedTransitionSession::Pair {
  SharedVisual from;
  SharedVisual to;
  std::shared_ptr<const SharedBoundsEvaluator> evaluator;
  Rect current;
  RenderNode outgoing;
  RenderNode incoming;
  RenderNode composite;
};

class SharedTransitionState {
public:
  void Mount(MountedNode& owner);
  void Unmount() noexcept;
  void Run(AnimationSpec animation, std::function<void()> mutation);
  NodeExtension::FrameResult Advance(MountedNode& owner, const FrameInfo& frame);
  SharedTransitionSession session;
private:
  MotionController progress_;
  std::thread::id thread_;
  bool mounted_ = false;
  bool started_ = false;
  bool mutating_ = false;
  AnimationSpec animation_ = SnapSpec{};
};

namespace {

std::uint64_t NextIdentity() {
  static std::atomic<std::uint64_t> next{0xB000000000000000ULL};
  return next.fetch_add(1, std::memory_order_relaxed);
}

SharedTransitionSession* Session(const NodeExtensionEntry& entry) {
  if (entry.descriptor == &SharedTransitionScope::Descriptor()) {
    return &InternalAccess::SharedScope(*static_cast<const SharedTransitionScope*>(entry.value.get()))->session;
  }
  return dynamic_cast<SharedTransitionSession*>(entry.extension.get());
}

bool NavigationBoundary(MountedNode& node) {
  return std::any_of(node.extensions.begin(), node.extensions.end(), [](const auto& entry) {
    auto* session = Session(entry);
    return session && session->IsNavigation();
  });
}

bool ContainsBoundary(MountedNode& node) {
  return NavigationBoundary(node) || std::any_of(node.children.begin(), node.children.end(),
      [](const auto& child) { return ContainsBoundary(*child); });
}

void StopDescendants(MountedNode& owner) {
  for (auto& child : owner.children) {
    if (NavigationBoundary(*child)) { continue; }
    for (auto& entry : child->extensions) {
      if (auto* session = Session(entry)) { session->Clear(); }
    }
    child->exclude_input = false;
    StopDescendants(*child);
  }
}

void CaptureNodeIdentities(const MountedNode& node, std::vector<std::uint64_t>& identities) {
  if (!node.participates_in_layout) { return; }
  identities.push_back(node.identity);
  for (std::size_t index = 0; index < node.children.size(); ++index) {
    CaptureNodeIdentities(ChildInPaintOrder(node, index), identities);
  }
}

bool MatchesCapturedPaint(const MountedNode& node, const RenderNode& captured,
                          const std::vector<std::uint64_t>& identities, std::size_t& identity_index,
                          bool root = true) {
  // Paint revisions belong to individual nodes, so equal revisions cannot identify reordered children.
  if (identity_index == identities.size() || identities[identity_index++] != node.identity ||
      !node.participates_in_layout || node.content_paint_dirty || node.foreground_paint_dirty ||
      !node.presentation.children_fragments.empty() ||
      captured.content.Revision() != node.render_node.content.Revision() ||
      captured.foreground.Revision() != node.render_node.foreground.Revision() ||
      captured.child_clips != ResolveChildClips(node) ||
      captured.children_transform != ResolveChildrenTransform(node)) { return false; }
  if (!root && (captured.offset != node.layout_offset ||
      captured.transform != node.presentation.local_transform ||
      captured.opacity != node.presentation.render_opacity)) { return false; }
  std::size_t child_index = 0;
  for (std::size_t index = 0; index < node.children.size(); ++index) {
    const auto& child = ChildInPaintOrder(node, index);
    if (!child.participates_in_layout) { continue; }
    if (child_index == captured.children.size() ||
        !MatchesCapturedPaint(child, *captured.children[child_index++], identities, identity_index, false)) { return false; }
  }
  return child_index == captured.children.size();
}

bool ValidBounds(Rect value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.width) &&
      std::isfinite(value.height) && value.width >= 0.0F && value.height >= 0.0F;
}

bool AxisAligned(const Transform2D& transform) {
  return transform.m12 == 0.0F && transform.m21 == 0.0F && transform.m11 > 0.0F && transform.m22 > 0.0F;
}

bool Contains(Rect outer, Rect inner) {
  return inner.x >= outer.x && inner.y >= outer.y &&
      inner.x + inner.width <= outer.x + outer.width && inner.y + inner.height <= outer.y + outer.height;
}

Rect SafeClip(const PushClipCommand& clip) {
  const float radius = clip.corner_radius;
  return {clip.rect.x + radius, clip.rect.y + radius,
          std::max(0.0F, clip.rect.width - radius * 2.0F),
          std::max(0.0F, clip.rect.height - radius * 2.0F)};
}

std::optional<Rect> OwnerClip(MountedNode& owner) {
  auto* root = InternalAccess::MountedRoot(*owner.runtime);
  const auto inverse = InverseTransform(owner.presentation.resolved_transform);
  if (!root || !inverse || !AxisAligned(*inverse)) { return std::nullopt; }
  std::optional<Rect> result;
  const auto visit = [&](auto&& self, const MountedNode& node, Rect clip, bool eligible) -> bool {
    if (&node == &owner) {
      if (eligible) { result = TransformBounds(*inverse, clip).Intersection(owner.bounds); }
      return true;
    }
    eligible = eligible && AxisAligned(node.presentation.resolved_transform);
    for (const auto& constraint : ResolveChildClips(node)) {
      if (const auto* rectangle = std::get_if<PushClipCommand>(&constraint)) {
        clip = clip.Intersection(TransformBounds(node.presentation.resolved_transform, SafeClip(*rectangle)));
      } else { eligible = false; }
    }
    for (const auto& child : node.children) {
      if (self(self, *child, clip, eligible)) { return true; }
    }
    return false;
  };
  visit(visit, *root, root->bounds, true);
  return result;
}

Rect Interpolate(Rect from, Rect to, float progress) {
  return {from.x + (to.x - from.x) * progress, from.y + (to.y - from.y) * progress,
          from.width + (to.width - from.width) * progress, from.height + (to.height - from.height) * progress};
}

Transform2D MapTo(Rect source, Rect target) {
  const float scale_x = target.width / source.width;
  const float scale_y = target.height / source.height;
  if (!std::isfinite(scale_x) || !std::isfinite(scale_y)) {
    throw std::invalid_argument("HuxerUI shared bounds cannot be represented by a finite transform");
  }
  return {scale_x, 0.0F, 0.0F, scale_y, target.x, target.y};
}

const SharedMarkerData* Marker(const NodeExtensionEntry& entry, bool& crossfade) {
  if (entry.descriptor == &SharedElement::Descriptor()) {
    crossfade = false;
    return &InternalAccess::SharedMarker(*static_cast<const SharedElement*>(entry.value.get()));
  }
  if (entry.descriptor == &SharedBounds::Descriptor()) {
    crossfade = true;
    return &InternalAccess::SharedMarker(*static_cast<const SharedBounds*>(entry.value.get()));
  }
  return nullptr;
}

template <class T> class SharedMarkerExtension final : public NodeExtension {
public:
  SharedMarkerExtension(ViewNode&, const T&) {}
  void Update(ViewNode&, const T&) {}
};

class SharedScopeExtension final : public NodeExtension {
public:
  SharedScopeExtension(ViewNode& node, const SharedTransitionScope& scope) { Update(node, scope); }
  ~SharedScopeExtension() override { if (state_) { state_->Unmount(); } }
  void Update(ViewNode& node, const SharedTransitionScope& scope) {
    const auto& next = InternalAccess::SharedScope(scope);
    if (next == state_) { return; }
    next->Mount(static_cast<MountedNode&>(node));
    if (state_) { state_->Unmount(); }
    state_ = next;
  }
  FrameResult OnFrame(ViewNode& node, const FrameInfo& frame) override {
    return state_->Advance(static_cast<MountedNode&>(node), frame);
  }
private:
  std::shared_ptr<SharedTransitionState> state_;
};

} // namespace

SharedTransitionSession::SharedTransitionSession(bool navigation) : navigation_(navigation) {
  overlay_.id = NextIdentity();
}
SharedTransitionSession::~SharedTransitionSession() = default;

void SharedTransitionSession::Bind(MountedNode& owner) {
  runtime_ = owner.runtime;
  owner_id_ = owner.identity;
}

void SharedTransitionSession::Unbind() noexcept {
  Clear();
  runtime_ = nullptr;
  owner_id_ = 0;
}

MountedNode* SharedTransitionSession::Owner() const {
  if (!runtime_) { return nullptr; }
  auto* root = const_cast<MountedNode*>(InternalAccess::MountedRoot(*runtime_));
  return root ? FindNode(*root, owner_id_) : nullptr;
}

bool SharedTransitionSession::Blocked() const {
  if (!runtime_) { return false; }
  auto* root = const_cast<MountedNode*>(InternalAccess::MountedRoot(*runtime_));
  bool blocked = false;
  const auto visit = [&](auto&& self, MountedNode& node, bool inherited) -> bool {
    if (NavigationBoundary(node)) { inherited = false; }
    for (auto entry = node.extensions.rbegin(); entry != node.extensions.rend(); ++entry) {
      auto* session = Session(*entry);
      if (session == this) { blocked = inherited; return true; }
      if (session && session->Active()) { inherited = true; }
    }
    for (auto& child : node.children) { if (self(self, *child, inherited)) { return true; } }
    return false;
  };
  if (root) { visit(visit, *root, false); }
  return blocked;
}

void SharedTransitionSession::Clear() noexcept {
  overlay_.children.clear();
  pairs_.clear();
  previous_.clear();
  active_ = false;
  ready_ = false;
}

void SharedTransitionSession::Navigation(
    std::uint64_t source, std::uint64_t destination, float progress, bool reversed, bool animate
) {
  if (!animate) { Clear(); return; }
  if (!active_ || source_id_ != source || destination_id_ != destination) {
    Clear();
    active_ = true;
    source_id_ = source;
    destination_id_ = destination;
    reversed_ = reversed;
  }
  progress_ = progress;
}

void SharedTransitionSession::BeginLocal(std::vector<SharedVisual> previous) {
  Clear();
  previous_ = std::move(previous);
  active_ = true;
  progress_ = 0.0F;
}

std::vector<SharedVisual> SharedTransitionSession::Collect(
    MountedNode& owner, MountedNode& root, bool capture, bool committed
) {
  std::vector<SharedVisual> result;
  std::vector<std::uint64_t> ancestors;
  const auto visit = [&](auto&& self, MountedNode& node, Transform2D transform, float opacity, Rect clip,
                         bool eligible, bool first) -> void {
    if (!node.participates_in_layout || (!first && NavigationBoundary(node))) { return; }
    const bool page_wrapper = navigation_ && first;
    if (!first) {
      transform = ComposeTransform(transform,
          ComposeTransform(TranslationTransform(node.layout_offset), node.presentation.local_transform));
    }
    eligible = eligible && AxisAligned(transform);
    // The overlay inherits its owner's opacity; navigation page effects are excluded as well.
    if (!first) { opacity *= node.presentation.render_opacity; }
    bool has_marker = false;
    for (const auto& entry : node.extensions) {
      bool crossfade = false;
      const auto* marker = Marker(entry, crossfade);
      if (!marker) { continue; }
      if (has_marker || std::any_of(result.begin(), result.end(), [&](const SharedVisual& item) {
            return item.marker.key == marker->key;
          })) {
        throw std::invalid_argument("HuxerUI shared keys must be unique on each participating side");
      }
      has_marker = true;
      SharedVisual visual;
      visual.marker = *marker;
      visual.crossfade = crossfade;
      visual.node_id = node.identity;
      visual.ancestors = ancestors;
      visual.bounds = TransformBounds(transform, node.bounds);
      visual.opacity = opacity;
      const bool visible = &node != &owner && eligible && ValidBounds(visual.bounds) && !visual.bounds.IsEmpty() && opacity > 0.0F &&
          Contains(clip, visual.bounds) && !ContainsBoundary(node);
      if (visible && capture) {
        visual.scene = CaptureRenderSubtree(node, !committed);
        if (visual.scene) { CaptureNodeIdentities(node, visual.captured_nodes); }
      }
      visual.eligible = visible;
      result.push_back(std::move(visual));
    }
    ancestors.push_back(node.identity);
    if (!page_wrapper) {
      for (const auto& constraint : ResolveChildClips(node)) {
        if (const auto* rectangle = std::get_if<PushClipCommand>(&constraint)) {
          clip = clip.Intersection(TransformBounds(transform, SafeClip(*rectangle)));
        } else { eligible = false; }
      }
      transform = ComposeTransform(transform, ResolveChildrenTransform(node));
    }
    for (std::size_t index = 0; index < node.children.size(); ++index) {
      self(self, ChildInPaintOrder(node, index), transform, opacity, clip, eligible, false);
    }
    ancestors.pop_back();
  };
  const auto clip = OwnerClip(owner);
  visit(visit, root, Transform2D{}, 1.0F, clip.value_or(Rect{}), clip.has_value(), true);
  return result;
}

std::vector<SharedVisual> SharedTransitionSession::CaptureCommitted(MountedNode& owner) {
  if (active_ && !ready_) { return previous_; }
  auto result = Collect(owner, owner, true, true);
  const auto inverse = InverseTransform(owner.presentation.resolved_transform);
  if (!inverse) { return result; }
  for (auto& visual : result) {
    const auto capture = [&](auto&& self, MountedNode& node) -> bool {
      if (&node != &owner && NavigationBoundary(node)) { return false; }
      for (const auto& entry : node.extensions) {
        auto* session = Session(entry);
        if (!session || !session->ready_) { continue; }
        const auto pair = std::find_if(session->pairs_.begin(), session->pairs_.end(), [&](const auto& item) {
          return item->to.node_id == visual.node_id && item->to.marker.key == visual.marker.key;
        });
        if (pair == session->pairs_.end()) { continue; }
        const auto transform = ComposeTransform(*inverse, node.presentation.resolved_transform);
        if ((*pair)->current.IsEmpty() || !AxisAligned(transform)) { visual.scene.reset(); return true; }
        visual.scene = FreezeRenderScene(&(*pair)->composite);
        visual.bounds = TransformBounds(transform, (*pair)->current);
        auto& root = *const_cast<RenderNode*>(visual.scene->root);
        root.transform = ComposeTransform(TranslationTransform({-visual.bounds.x, -visual.bounds.y}), transform);
        visual.opacity = owner.presentation.resolved_opacity > 0.0F
            ? node.presentation.resolved_opacity / owner.presentation.resolved_opacity : 0.0F;
        return true;
      }
      for (auto& child : node.children) { if (self(self, *child)) { return true; } }
      return false;
    };
    capture(capture, owner);
  }
  return result;
}

void SharedTransitionSession::Match(MountedNode& owner) {
  auto* source = navigation_ ? FindNode(owner, source_id_) : &owner;
  auto* target = navigation_ ? FindNode(owner, destination_id_) : &owner;
  if (!source || !target) { Clear(); return; }
  if (navigation_) { previous_ = Collect(owner, *source, true, false); }
  auto incoming = Collect(owner, *target, true, false);
  for (auto& to : incoming) {
    const auto from = std::find_if(previous_.begin(), previous_.end(), [&](const SharedVisual& item) {
      return item.marker.key == to.marker.key;
    });
    if (from == previous_.end()) { continue; }
    if (from->crossfade != to.crossfade) {
      throw std::invalid_argument("HuxerUI shared markers with the same key must have the same kind");
    }
    if (!from->scene || !to.scene) { continue; }
    for (const auto& existing : pairs_) {
      const auto overlaps = [](const SharedVisual& a, const SharedVisual& b) {
        return std::find(a.ancestors.begin(), a.ancestors.end(), b.node_id) != a.ancestors.end() ||
            std::find(b.ancestors.begin(), b.ancestors.end(), a.node_id) != b.ancestors.end();
      };
      if (overlaps(*from, existing->from) || overlaps(to, existing->to)) {
        throw std::invalid_argument("HuxerUI shared transitions cannot match both an ancestor and its descendant");
      }
    }
    auto pair = std::make_unique<Pair>();
    pair->from = *from;
    pair->to = std::move(to);
    const auto& preferred = reversed_ ? pair->from : pair->to;
    const auto& alternate = reversed_ ? pair->to : pair->from;
    pair->evaluator = preferred.marker.bounds_transform ? preferred.marker.bounds_transform : alternate.marker.bounds_transform;
    if (pair->evaluator) {
      const Rect begin = reversed_ ? pair->to.bounds : pair->from.bounds;
      const Rect end = reversed_ ? pair->from.bounds : pair->to.bounds;
      TransitionEvaluationScope evaluation;
      if (pair->evaluator->Evaluate(begin, end, 0.0F) != begin || pair->evaluator->Evaluate(begin, end, 1.0F) != end) {
        throw std::invalid_argument("HuxerUI shared bounds transforms must preserve their endpoints");
      }
    }
    pair->outgoing.id = NextIdentity();
    pair->incoming.id = NextIdentity();
    pair->composite.id = NextIdentity();
    pairs_.push_back(std::move(pair));
  }
  if (reversed_) {
    const auto position = [&](std::uint64_t identity) {
      return std::find_if(previous_.begin(), previous_.end(),
          [&](const SharedVisual& item) { return item.node_id == identity; });
    };
    std::stable_sort(pairs_.begin(), pairs_.end(), [&](const auto& a, const auto& b) {
      return position(a->from.node_id) < position(b->from.node_id);
    });
  }
  previous_.clear();
  ready_ = true;
  bounds_ = owner.bounds;
  owner_transform_ = owner.presentation.resolved_transform;
  const auto* root = InternalAccess::MountedRoot(*owner.runtime);
  viewport_ = root ? Size{root->bounds.width, root->bounds.height} : Size{};
}

void SharedTransitionSession::Sample(MountedNode& owner) {
  if (pairs_.empty()) { return; }
  auto* source = navigation_ ? FindNode(owner, source_id_) : &owner;
  auto* target = navigation_ ? FindNode(owner, destination_id_) : &owner;
  if (!source || !target) { Clear(); return; }
  const auto incoming = Collect(owner, *target, false, false);
  const auto outgoing = navigation_ ? Collect(owner, *source, false, false) : std::vector<SharedVisual>{};
  const auto valid = [&](const SharedVisual& captured, const std::vector<SharedVisual>& current) {
    auto* node = FindNode(owner, captured.node_id);
    std::size_t identity_index = 0;
    return node && MatchesCapturedPaint(*node, *captured.scene->root, captured.captured_nodes, identity_index) &&
        std::any_of(current.begin(), current.end(), [&](const SharedVisual& item) {
      return item.node_id == captured.node_id && item.marker.key == captured.marker.key &&
          item.crossfade == captured.crossfade && item.eligible &&
          item.bounds == captured.bounds && item.opacity == captured.opacity;
    });
  };
  overlay_.children.clear();
  std::erase_if(pairs_, [&](const auto& pair) {
    return !valid(pair->to, incoming) || (navigation_ && !valid(pair->from, outgoing));
  });
  const float opacity_progress = std::clamp(progress_, 0.0F, 1.0F);
  const auto inverse = InverseTransform(ResolveChildrenTransform(owner));
  if (!inverse) { Clear(); return; }
  // Validate all geometry before hiding any mounted drawing or publishing any overlay links.
  for (auto& pair : pairs_) {
    TransitionEvaluationScope evaluation;
    pair->current = pair->evaluator
        ? pair->evaluator->Evaluate(reversed_ ? pair->to.bounds : pair->from.bounds,
                                   reversed_ ? pair->from.bounds : pair->to.bounds,
                                   reversed_ ? 1.0F - progress_ : progress_)
        : Interpolate(pair->from.bounds, pair->to.bounds, progress_);
    if (!ValidBounds(pair->current)) { throw std::invalid_argument("HuxerUI shared bounds output must be finite and non-negative"); }
    const auto& content = !navigation_ || reversed_ ? pair->from : pair->to;
    pair->outgoing.transform = MapTo(pair->from.crossfade ? pair->from.bounds : content.bounds, pair->current);
    if (pair->from.crossfade) { pair->incoming.transform = MapTo(pair->to.bounds, pair->current); }
  }
  for (auto& pair : pairs_) {
    const bool single_from = !navigation_ || reversed_;
    const auto& content = single_from ? pair->from : pair->to;
    pair->outgoing.children = {pair->from.crossfade ? pair->from.scene->root : content.scene->root};
    pair->outgoing.opacity = pair->from.crossfade ? pair->from.opacity * (1.0F - opacity_progress) : content.opacity;
    ++pair->outgoing.revision;
    pair->composite.children = {&pair->outgoing};
    if (pair->from.crossfade) {
      pair->incoming.children = {pair->to.scene->root};
      pair->incoming.opacity = pair->to.opacity * opacity_progress;
      ++pair->incoming.revision;
      pair->composite.children.push_back(&pair->incoming);
    }
    ++pair->composite.revision;
    if (auto* node = FindNode(owner, pair->to.node_id)) { node->presentation.suppress_render = true; }
    if (navigation_) {
      if (auto* node = FindNode(owner, pair->from.node_id)) { node->presentation.suppress_render = true; }
    }
    overlay_.children.push_back(&pair->composite);
  }
  overlay_.transform = *inverse;
  overlay_.child_clips = {PushClipCommand{owner.bounds, 0.0F}};
  ++overlay_.revision;
  if (!pairs_.empty()) { owner.presentation.overlay = &overlay_; }
}

void SharedTransitionSession::PrepareRender(MountedNode& owner) {
  if (!active_) { return; }
  if (Blocked()) { Clear(); return; }
  try {
    if (!ready_) {
      for (auto& entry : owner.extensions) {
        auto* session = Session(entry);
        if (session == this) { break; }
        if (session) { session->Clear(); }
      }
      StopDescendants(owner);
      Match(owner);
    }
    const auto* root = InternalAccess::MountedRoot(*owner.runtime);
    const Size viewport = root ? Size{root->bounds.width, root->bounds.height} : Size{};
    if (!active_ || owner.bounds != bounds_ || viewport != viewport_ ||
        owner.presentation.resolved_transform != owner_transform_) { Clear(); return; }
    Sample(owner);
    if (!navigation_ && pairs_.empty()) { Clear(); }
  } catch (...) {
    Clear();
    owner.presentation.overlay = nullptr;
    owner.exclude_input = false;
    InternalAccess::RequestFrame(*owner.runtime);
    throw;
  }
}

void SharedTransitionState::Mount(MountedNode& owner) {
  if (mounted_) { throw std::logic_error("HuxerUI a shared transition scope can be mounted on only one View"); }
  session.Bind(owner);
  thread_ = std::this_thread::get_id();
  mounted_ = true;
}

void SharedTransitionState::Unmount() noexcept {
  session.Unbind();
  mounted_ = false;
  started_ = false;
}

void SharedTransitionState::Run(AnimationSpec animation, std::function<void()> mutation) {
  if (!mounted_ || thread_ != std::this_thread::get_id()) {
    throw std::logic_error("HuxerUI shared transition requires a mounted scope on its UI thread");
  }
  auto* owner = session.Owner();
  if (!owner) { throw std::logic_error("HuxerUI shared transition requires a mounted scope on its UI thread"); }
  if (mutating_ || TransitionEvaluationScope::Active()) {
    throw std::logic_error("HuxerUI shared transitions cannot start recursively from mutation or evaluation");
  }
  if (!mutation) { throw std::invalid_argument("HuxerUI shared transition mutation must not be empty"); }
  ValidateTransitionTiming(animation, 0.0);
  Runtime* const runtime = owner->runtime;
  const bool animate = !IsImmediateTransitionTiming(animation, 0.0) && !owner->reduced_motion && !session.Blocked();
  auto previous = animate ? session.CaptureCommitted(*owner) : std::vector<SharedVisual>{};
  mutating_ = true;
  try {
    TransitionEvaluationScope mutation_scope;
    mutation();
  } catch (...) {
    mutating_ = false;
    session.Clear();
    InternalAccess::RequestFrame(*runtime);
    throw;
  }
  mutating_ = false;
  animation_ = std::move(animation);
  started_ = false;
  if (animate && mounted_) { session.BeginLocal(std::move(previous)); }
  else { session.Clear(); }
  InternalAccess::RequestFrame(*runtime);
}

NodeExtension::FrameResult SharedTransitionState::Advance(MountedNode& owner, const FrameInfo& frame) {
  if (!session.Active()) { return {}; }
  if (frame.reduced_motion || session.Blocked()) { session.Clear(); return {}; }
  owner.exclude_input = true;
  if (!started_) {
    progress_.Set(0.0F);
    progress_.AnimateTo(1.0F, animation_);
    started_ = true;
  }
  const auto result = progress_.Advance(frame);
  session.SetProgress(progress_.Value());
  if (!progress_.IsRunning()) {
    session.Clear();
    owner.exclude_input = false;
  }
  return {result.needs_frame, result.wake_after};
}

bool PrepareSharedTransitions(MountedNode& node, bool participating) {
  if (!node.subtree_has_extensions) { return false; }
  participating = participating && node.participates_in_layout;
  const bool was_excluded = node.exclude_input;
  bool has_local_session = false;
  for (NodeExtensionEntry& entry : node.extensions) {
    auto* session = Session(entry);
    if (!session) { continue; }
    has_local_session = has_local_session || !session->IsNavigation();
    if (!participating) { session->Clear(); }
    else { session->PrepareRender(node); }
  }

  node.exclude_input = participating && has_local_session &&
      std::any_of(node.extensions.begin(), node.extensions.end(), [](const auto& entry) {
        auto* session = Session(entry);
        return session && !session->IsNavigation() && session->Active();
      });
  if (!participating) {
    node.presentation.overlay = nullptr;
    node.presentation.suppress_render = false;
  }
  bool changed = was_excluded != node.exclude_input;
  for (auto& child : node.children) { changed = PrepareSharedTransitions(*child, participating) || changed; }
  return changed;
}

const SharedMarkerData& InternalAccess::SharedMarker(const SharedElement& marker) noexcept { return marker.data_; }
const SharedMarkerData& InternalAccess::SharedMarker(const SharedBounds& marker) noexcept { return marker.data_; }
const std::shared_ptr<SharedTransitionState>& InternalAccess::SharedScope(const SharedTransitionScope& scope) noexcept {
  return scope.state_;
}

} // namespace huxerui::detail

namespace huxerui {

SharedElement::SharedElement(const char* key) : SharedElement(std::string(key ? key : "")) {
  if (!key) { throw std::invalid_argument("HuxerUI shared key must not be null"); }
}
SharedBounds::SharedBounds(const char* key) : SharedBounds(std::string(key ? key : "")) {
  if (!key) { throw std::invalid_argument("HuxerUI shared key must not be null"); }
}
const detail::ModifierDescriptor& SharedElement::Descriptor() {
  return detail::ModifierDescriptorFor<SharedElement, detail::SharedMarkerExtension<SharedElement>>();
}
const detail::ModifierDescriptor& SharedBounds::Descriptor() {
  return detail::ModifierDescriptorFor<SharedBounds, detail::SharedMarkerExtension<SharedBounds>>();
}
const detail::ModifierDescriptor& SharedTransitionScope::Descriptor() {
  return detail::ModifierDescriptorFor<SharedTransitionScope, detail::SharedScopeExtension>();
}
SharedTransitionScope SharedTransitionHandle::Scope() const { return SharedTransitionScope{state_}; }
void SharedTransitionHandle::Run(AnimationSpec animation, std::function<void()> mutation) const {
  state_->Run(std::move(animation), std::move(mutation));
}
SharedTransitionHandle UseSharedTransition() {
  auto state = UseState(std::make_shared<detail::SharedTransitionState>());
  return SharedTransitionHandle{state.Get()};
}

} // namespace huxerui
