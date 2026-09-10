#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <huxerui/animation.h>
#include <huxerui/geometry.h>
#include <huxerui/paint.h>
#include <huxerui/render_scene.h>

namespace huxerui {
class Runtime;
}

namespace huxerui::detail {

void ValidateTransitionTiming(const AnimationSpec& animation, double delay);
bool IsImmediateTransitionTiming(const AnimationSpec& animation, double delay) noexcept;

class TransitionEvaluationScope {
public:
  TransitionEvaluationScope();
  ~TransitionEvaluationScope();
  static bool Active() noexcept;
};

struct SharedVisual;
struct MountedNode;

struct FrozenScene {
  static PaintSequence CopyPaintSequence(const PaintSequence& source);

  const RenderNode* root = nullptr;
  std::vector<std::unique_ptr<RenderNode>> nodes;
};

std::shared_ptr<FrozenScene> FreezeRenderScene(const RenderNode* root);
std::shared_ptr<FrozenScene> CaptureRenderSubtree(MountedNode& node, bool record = true);

// One operation's matching and render data. Navigation or the local owner supplies progress.
class SharedTransitionSession {
public:
  explicit SharedTransitionSession(bool navigation = false);
  virtual ~SharedTransitionSession();
  void Bind(MountedNode& owner);
  void Unbind() noexcept;
  void Navigation(std::uint64_t source, std::uint64_t destination, float progress, bool reversed, bool animate);
  void BeginLocal(std::vector<SharedVisual> previous);
  std::vector<SharedVisual> CaptureCommitted(MountedNode& owner);
  void SetProgress(float progress) noexcept { progress_ = progress; }
  bool Active() const noexcept { return active_; }
  bool IsNavigation() const noexcept { return navigation_; }
  bool Blocked() const;
  MountedNode* Owner() const;
  void Clear() noexcept;
  virtual void PrepareRender(MountedNode& owner);

private:
  struct Pair;
  std::vector<SharedVisual> Collect(MountedNode& owner, MountedNode& root, bool capture, bool committed);
  void Match(MountedNode& owner);
  void Sample(MountedNode& owner);
  Runtime* runtime_ = nullptr;
  std::uint64_t owner_id_ = 0;
  std::uint64_t source_id_ = 0;
  std::uint64_t destination_id_ = 0;
  bool navigation_ = false;
  bool active_ = false;
  bool ready_ = false;
  bool reversed_ = false;
  float progress_ = 0.0F;
  Rect bounds_;
  Size viewport_;
  Transform2D owner_transform_;
  std::vector<SharedVisual> previous_;
  std::vector<std::unique_ptr<Pair>> pairs_;
  RenderNode overlay_;
};

// Resolves shared visuals before page fragments are copied; reports changes to input exclusion.
bool PrepareSharedTransitions(MountedNode& node, bool participating = true);

} // namespace huxerui::detail
