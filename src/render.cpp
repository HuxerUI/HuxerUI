#include "internal.h"

#include <algorithm>
#include <optional>

#include <huxerui/theme.h>

namespace huxerui::detail {

namespace {

Color ResolveForeground(const MountedNode& node) {
  return node.style.foreground.value_or(
      node.kind == NodeKind::Button ? ButtonStyleKey::Default().foreground : TextStyleKey::Default().foreground
  );
}

float ResolveFontSize(const MountedNode& node) {
  return node.style.font_size.value_or(
      node.kind == NodeKind::Button ? ButtonStyleKey::Default().font_size : TextStyleKey::Default().font_size
  );
}

Rect ContentRect(const MountedNode& node) {
  return {
      node.frame.x + node.style.padding.left,
      node.frame.y + node.style.padding.top,
      std::max(0.0F, node.frame.width - node.style.padding.Horizontal()),
      std::max(0.0F, node.frame.height - node.style.padding.Vertical()),
  };
}

Color ApplyOpacity(Color color, float opacity) {
  color.alpha *= std::clamp(opacity, 0.0F, 1.0F);
  return color;
}

bool ClipsChildren(const MountedNode& node) {
  return IsScrollContainer(node);
}

void ResolvePresentationTree(
    MountedNode& node, const PresentationTransform& inherited_transform, float inherited_opacity, bool inherited_enabled
) {
  node.presentation.resolved_transform = ComposeTransform(inherited_transform, node.presentation.local_transform);
  float opacity = inherited_opacity * node.presentation.local_opacity;
  if (inherited_enabled && !node.enabled) {
    opacity *= node.style.disabled_opacity;
  }
  node.presentation.resolved_opacity = opacity;
  for (auto& child : node.children) {
    ResolvePresentationTree(*child, node.presentation.resolved_transform, opacity, node.enabled);
  }
}

void PaintNodeExtensions(MountedNode& node, DisplayList& display_list) {
  for (const auto& entry : node.extensions) {
    if (entry.extension) {
      entry.extension->Paint(node, display_list);
    }
  }
}

void PaintFocusRing(const MountedNode& node, const Rect& frame, float opacity, DisplayList& display_list) {
  if (!node.focus_visible || !node.enabled || node.style.focus_ring_width <= 0.0F) {
    return;
  }
  display_list.DrawBorder(
      frame,
      ApplyOpacity(node.style.focus_ring, opacity),
      node.style.focus_ring_width,
      node.style.corner_radius
  );
}

void PaintNodeWithinClip(MountedNode& node, DisplayList& display_list, const Rect& clip) {
  const PresentationTransform& transform = node.presentation.resolved_transform;
  const float opacity = node.presentation.resolved_opacity;
  const Rect presentation_frame = TransformBounds(transform, node.frame);
  if (!presentation_frame.Intersects(clip) || opacity <= 0.0F) {
    return;
  }

  const bool transformed = !node.presentation.local_transform.IsIdentity();
  if (transformed) {
    const auto& value = node.presentation.local_transform;
    display_list.PushTransform(value.m11, value.m12, value.m21, value.m22, value.translate_x, value.translate_y);
  }

  const Rect frame = node.frame;
  if (node.style.background.has_value() && node.style.background->alpha > 0.0F) {
    display_list.DrawRect(frame, ApplyOpacity(*node.style.background, opacity), node.style.corner_radius);
  }

  if (node.kind == NodeKind::Text) {
    display_list
        .DrawText(ContentRect(node), node.text, ApplyOpacity(ResolveForeground(node), opacity), ResolveFontSize(node));
  } else if (node.kind == NodeKind::Button) {
    display_list.DrawText(
        frame,
        node.text,
        ApplyOpacity(ResolveForeground(node), opacity),
        ResolveFontSize(node),
        TextAlign::Center
    );
  }

  if (ClipsChildren(node)) {
    const Rect viewport = ContentRect(node);
    const Rect child_clip = clip.Intersection(TransformBounds(transform, viewport));
    if (!child_clip.IsEmpty()) {
      display_list.PushClip(viewport);
      for (const auto& child : node.children) {
        PaintNodeWithinClip(*child, display_list, child_clip);
      }
      display_list.PopClip();
    }
    PaintNodeExtensions(node, display_list);
    PaintFocusRing(node, frame, opacity, display_list);
  } else {
    for (const auto& child : node.children) {
      PaintNodeWithinClip(*child, display_list, clip);
    }
    PaintNodeExtensions(node, display_list);
    PaintFocusRing(node, frame, opacity, display_list);
  }
  if (transformed) {
    display_list.PopTransform();
  }
}

} // namespace

void PaintNode(MountedNode& node, DisplayList& display_list) {
  if (node.frame.IsEmpty()) {
    return;
  }
  ResolvePresentationTree(node, PresentationTransform{}, 1.0F, true);
  PaintNodeWithinClip(node, display_list, node.frame);
}

} // namespace huxerui::detail
