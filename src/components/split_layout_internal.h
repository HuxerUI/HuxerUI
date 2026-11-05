#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <huxerui/view.h>

namespace huxerui::detail {

struct SplitPaneMetrics {
  Frame frame;
  Grow grow{0.0F};
  // SplitLayout uses mounted identity to validate drags. DockLayout checks tree topology when reusing a session.
  std::uint64_t identity = 0;
  float natural = 0.0F;
  float extent = 0.0F;
};

// Measurement produces split-local bounds. Geometry preparation supplies the split-to-window transform
// and enabled state.
struct SplitGeometry {
  Axis axis = Axis::Horizontal;
  bool bounded = false;
  bool enabled = false;
  // On a bounded main axis, this is the space left for panes after reserving all separators.
  float available = 0.0F;
  float separator = 0.0F;
  Size size;
  Transform2D transform;
  std::vector<SplitPaneMetrics> panes;

  // Converts a main-axis interval to split-local bounds spanning the cross axis.
  Rect Region(float offset, float extent) const;
};

// A split's separators share one session. Preserve it across accepted resize proposals to keep drag baselines.
struct SplitSession {
  SplitGeometry geometry;
  std::function<void(std::vector<SplitChange>)> propose;
};

// Panes are in layout order. The callback can measure the same pane at different constraints, including its
// allocated main extent to resolve an unbounded cross axis. The caller measures and places the final allocations.
SplitGeometry MeasureSplit(Axis axis, Constraints constraints, std::vector<SplitPaneMetrics> panes,
                           const std::function<Size(std::size_t, Constraints)>& measure_pane);

// Publish the final placement before the separators prepare their geometry. The origin is the split's top-left
// in owner-local coordinates, including the offset of a nested dock region.
void CommitSplitGeometry(SplitSession& session, SplitGeometry geometry, const ViewNode& owner, Point origin = {});

// Keep the clip on the allocated pane so a presentation transform on its content cannot move the clip.
View MakeClippedPane(View content);

// The index identifies the pane immediately before the separator in the session's pane order.
View MakeSplitSeparator(std::shared_ptr<SplitSession> session, std::size_t index, Axis axis, Color color);

} // namespace huxerui::detail
