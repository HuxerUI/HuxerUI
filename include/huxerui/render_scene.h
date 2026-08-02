#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <huxerui/geometry.h>
#include <huxerui/paint.h>

namespace huxerui {

struct RenderClip {
  // The clip is expressed in the owning render node's local logical coordinates.
  Rect rect;
  float corner_radius = 0.0F;

  bool operator==(const RenderClip&) const = default;
};

struct RenderNode {
  // Identity remains stable while the corresponding mounted node reconciles compatibly.
  std::uint64_t id = 0;
  // Parent-local layout translation applied before transform.
  Point offset;
  // Node-local presentation transform shared by content, descendants, and foreground.
  Transform2D transform;
  // Group opacity applied to content, descendants, and foreground.
  float opacity = 1.0F;
  // Clips descendants only; content and foreground remain outside this clip.
  std::optional<RenderClip> child_clip;
  // Applied after child_clip to child render nodes without affecting this node's content or foreground.
  Transform2D children_transform;
  // Content and foreground commands use this node's local logical coordinates.
  PaintSequence content;
  // Child pointers are non-owning and remain valid until the next Runtime frame construction or Runtime destruction.
  std::vector<const RenderNode*> children;
  PaintSequence foreground;
  // True when a recorded paint sequence or at least one descendant contributes visible output.
  bool visible = true;
  // Changes when a paint sequence is rerecorded or retained scene properties change.
  std::uint64_t revision = 0;
};

struct RenderScene {
  // The root pointer is non-owning and remains valid until the next Runtime frame construction or Runtime destruction.
  const RenderNode* root = nullptr;
};

struct DamageRegion {
  // A full region supersedes any rectangles stored in rects.
  bool full = false;
  std::vector<Rect> rects;

  bool operator==(const DamageRegion&) const = default;
};

struct RenderFrame {
  // The scene and all referenced records remain valid until the next Runtime frame construction or destruction.
  RenderScene scene;
  DamageRegion damage;
  std::uint64_t revision = 0;
};

struct FrameCommit {
  // Couples the frame to present with the earliest follow-up build requested while producing it. The platform commits
  // the render frame before scheduling this absolute deadline, avoiding frame construction re-entry during a build.
  RenderFrame render_frame;
  // Absolute deadline in the platform adapter's monotonic clock.
  std::optional<double> next_frame_deadline;
};

} // namespace huxerui
