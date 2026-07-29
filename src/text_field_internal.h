#pragma once

#include <string>

#include <huxerui/modifier.h>
#include <huxerui/text_input.h>
#include <huxerui/theme.h>

namespace huxerui {
class PlatformHost;
}

namespace huxerui::detail {

struct ResolvedTextFieldStyle {
  using Value = TextFieldStyle;
};

struct TextFieldModifier {
  static const ModifierDescriptor& Descriptor();

  TextEditingValue value;
  std::string placeholder;
  TextInputConfiguration configuration;
};

[[nodiscard]] Size MeasureTextField(MountedNode& node, PlatformHost& platform, Constraints constraints);
bool SelectTextFieldWord(MountedNode& node, Point position);
bool ExtendTextFieldSelection(MountedNode& node, Point position, bool start_handle);
bool QueryTextFieldSelectionGeometry(const MountedNode& node, Rect& start, Rect& end);
Color TextFieldSelectionHandleColor(const MountedNode& node);

} // namespace huxerui::detail
