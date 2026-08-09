#pragma once

#include "internal.h"

namespace huxerui::detail {

struct SelectionAreaModifier {
  static const ModifierDescriptor& Descriptor();

  bool operator==(const SelectionAreaModifier&) const = default;
};

Size MeasureSelectionArea(
    MountedNode& node, PlatformAdapter& platform, Runtime& runtime, const Constraints& constraints, EdgeInsets safe_area
);

} // namespace huxerui::detail
