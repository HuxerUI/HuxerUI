#pragma once

#include "mounted_node_internal.h"

namespace huxerui::detail {

struct SelectionAreaModifier {
  static const ModifierDescriptor& Descriptor();

  bool operator==(const SelectionAreaModifier&) const = default;
};

Size MeasureSelectionArea(
    MountedNode& node,
    PlatformAdapter& platform,
    Runtime& runtime,
    const Constraints& constraints,
    EdgeInsets safe_area,
    const WindowTitleBarMetrics* title_bar_metrics
);

} // namespace huxerui::detail
