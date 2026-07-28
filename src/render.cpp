#include "internal.h"

#include <algorithm>
#include <optional>

#include <huxerui/theme.h>

namespace huxerui::detail {

namespace {

Color ResolveForeground(const MountedNode &node) {
  return node.style.foreground.value_or(
      node.kind == NodeKind::Button
          ? ButtonStyleKey::Default().foreground
          : TextStyleKey::Default().foreground);
}

float ResolveFontSize(const MountedNode &node) {
  return node.style.font_size.value_or(
      node.kind == NodeKind::Button
          ? ButtonStyleKey::Default().font_size
          : TextStyleKey::Default().font_size);
}

Rect ContentRect(const MountedNode &node) {
  return {
      node.frame.x + node.style.padding.left,
      node.frame.y + node.style.padding.top,
      std::max(0.0F, node.frame.width - node.style.padding.Horizontal()),
      std::max(0.0F, node.frame.height - node.style.padding.Vertical()),
  };
}

Rect Translate(Rect rect, Point offset) {
  rect.x += offset.x;
  rect.y += offset.y;
  return rect;
}

Color ApplyOpacity(Color color, float opacity) {
  color.alpha *= std::clamp(opacity, 0.0F, 1.0F);
  return color;
}

bool ClipsChildren(const MountedNode &node) {
  return node.kind == NodeKind::ScrollView || IsVirtualLayoutNode(node);
}

void PaintModifiers(MountedNode &node, DisplayList &display_list) {
  for (const auto &entry : node.modifiers) {
    if (entry.mounted) {
      entry.mounted->Paint(node, display_list);
    }
  }
}

void PaintFocusRing(
    const MountedNode &node, const Rect &frame,
    float opacity, DisplayList &display_list) {
  if (!node.focused || !node.enabled ||
      node.style.focus_ring_width <= 0.0F) {
    return;
  }
  display_list.DrawBorder(
      frame,
      ApplyOpacity(node.style.focus_ring, opacity),
      node.style.focus_ring_width,
      node.style.corner_radius);
}

void PaintNodeWithinClip(MountedNode &node, DisplayList &display_list,
                         const Rect &clip, Point inherited_offset,
                         float inherited_opacity,
                         bool inherited_enabled) {
  const Point offset{
      inherited_offset.x + node.presentation_offset.x,
      inherited_offset.y + node.presentation_offset.y,
  };
  float opacity =
      inherited_opacity * node.presentation_opacity;
  if (inherited_enabled && !node.enabled) {
    opacity *= node.style.disabled_opacity;
  }
  node.resolved_presentation_offset = offset;
  node.resolved_presentation_opacity = opacity;
  const Rect frame = Translate(node.frame, offset);
  if (!frame.Intersects(clip) || opacity <= 0.0F) {
    return;
  }

  if (node.style.background.has_value() &&
      node.style.background->alpha > 0.0F) {
    display_list.DrawRect(frame,
                          ApplyOpacity(*node.style.background, opacity),
                          node.style.corner_radius);
  }

  if (node.kind == NodeKind::Text) {
    display_list.DrawText(Translate(ContentRect(node), offset), node.text,
                          ApplyOpacity(ResolveForeground(node), opacity),
                          ResolveFontSize(node));
  } else if (node.kind == NodeKind::Button) {
    display_list.DrawText(frame, node.text,
                          ApplyOpacity(ResolveForeground(node), opacity),
                          ResolveFontSize(node), TextAlign::Center);
  }

  if (ClipsChildren(node)) {
    const Rect viewport = Translate(ContentRect(node), offset);
    const Rect child_clip = clip.Intersection(viewport);
    if (child_clip.IsEmpty()) {
      return;
    }

    display_list.PushClip(viewport);
    for (const auto &child : node.children) {
      PaintNodeWithinClip(
          *child, display_list, child_clip, offset, opacity,
          node.enabled);
    }
    display_list.PopClip();
    PaintModifiers(node, display_list);
    PaintFocusRing(node, frame, opacity, display_list);
    return;
  }

  for (const auto &child : node.children) {
    PaintNodeWithinClip(
        *child, display_list, clip, offset, opacity,
        node.enabled);
  }
  PaintModifiers(node, display_list);
  PaintFocusRing(node, frame, opacity, display_list);
}

} // namespace

void PaintNode(MountedNode &node, DisplayList &display_list) {
  if (node.frame.IsEmpty()) {
    return;
  }
  PaintNodeWithinClip(
      node, display_list, node.frame, {}, 1.0F, true);
}

} // namespace huxerui::detail
