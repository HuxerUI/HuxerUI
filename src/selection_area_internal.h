#pragma once

#include "internal.h"

namespace huxerui::detail {

struct SelectionAreaModifier {
  static const ModifierDescriptor& Descriptor();
};

Size MeasureSelectionArea(MountedNode& node, PlatformHost& platform, Runtime& runtime, const Constraints& constraints);
bool CanPerformSelectionAreaAction(const MountedNode& node, TextEditingAction action, PlatformClipboard* clipboard);
bool PerformSelectionAreaAction(MountedNode& node, TextEditingAction action, PlatformClipboard* clipboard);
bool SelectSelectionAreaWord(MountedNode& node, Point position);
bool ExtendSelectionArea(MountedNode& node, Point position, bool start_handle);
bool QuerySelectionAreaGeometry(const MountedNode& node, Rect& start, Rect& end);
Color SelectionAreaHandleColor(const MountedNode& node);

} // namespace huxerui::detail
