#include "internal.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_set>
#include <utility>

namespace huxerui::detail {

namespace {

float AlignOffset(float available, float extent, HorizontalAlignment alignment) noexcept {
  if (alignment == HorizontalAlignment::End) {
    return available - extent;
  }
  return alignment == HorizontalAlignment::Center ? (available - extent) * 0.5F : 0.0F;
}

float AlignOffset(float available, float extent, VerticalAlignment alignment) noexcept {
  if (alignment == VerticalAlignment::End) {
    return available - extent;
  }
  return alignment == VerticalAlignment::Center ? (available - extent) * 0.5F : 0.0F;
}

void PaintImage(
    const MountedNode& node,
    PaintContext& context,
    Rect content,
    std::optional<Color> vector_tint
) {
  const Size intrinsic = node.image_properties.IntrinsicSize();
  if (intrinsic.width <= 0.0F || intrinsic.height <= 0.0F || content.IsEmpty()) {
    return;
  }
  const auto draw = [&](Rect source, Rect destination) {
    std::visit(
        [&](const auto& asset) {
          using Asset = std::decay_t<decltype(asset)>;
          if constexpr (std::same_as<Asset, ImageAsset>) {
            context.DrawImageRect(asset, source, destination, node.image_properties.sampling);
          } else {
            context.DrawImageRect(asset, source, destination, vector_tint);
          }
        },
        node.image_properties.asset
    );
  };
  const Rect full_source{0.0F, 0.0F, intrinsic.width, intrinsic.height};
  if (node.image_properties.fit == ImageFit::Fill) {
    draw(full_source, content);
    return;
  }
  if (node.image_properties.fit == ImageFit::Cover) {
    const float scale = std::max(content.width / intrinsic.width, content.height / intrinsic.height);
    const Size source_size{content.width / scale, content.height / scale};
    const Rect source{
        AlignOffset(intrinsic.width, source_size.width, node.image_properties.horizontal_alignment),
        AlignOffset(intrinsic.height, source_size.height, node.image_properties.vertical_alignment),
        source_size.width,
        source_size.height,
    };
    draw(source, content);
    return;
  }
  float scale = 1.0F;
  if (node.image_properties.fit == ImageFit::Contain || node.image_properties.fit == ImageFit::ScaleDown) {
    scale = std::min(content.width / intrinsic.width, content.height / intrinsic.height);
    if (node.image_properties.fit == ImageFit::ScaleDown) {
      scale = std::min(1.0F, scale);
    }
  }
  const Size destination_size{intrinsic.width * scale, intrinsic.height * scale};
  const Rect destination{
      content.x + AlignOffset(content.width, destination_size.width, node.image_properties.horizontal_alignment),
      content.y + AlignOffset(content.height, destination_size.height, node.image_properties.vertical_alignment),
      destination_size.width,
      destination_size.height,
  };
  draw(full_source, destination);
}

void PaintImage(const MountedNode& node, PaintContext& context) {
  PaintImage(node, context, node.ContentBounds(), node.image_properties.tint);
}

void PaintLabelContent(
    const MountedNode& node,
    PaintContext& context,
    const TextStyle& text_style
) {
  const LabelContentMetrics metrics = node.LayoutValueOr<LabelContentMetrics>({});
  const Rect content = node.ContentBounds();
  const float icon_width = std::min(std::max(0.0F, metrics.icon_size.width), content.width);
  const float icon_height = std::min(std::max(0.0F, metrics.icon_size.height), content.height);
  const bool show_label = metrics.show_label && !node.text.empty();
  const auto cached = node.layout_cache.find(typeid(LabelLayoutCache));
  const auto* layout = cached == node.layout_cache.end() ? nullptr : std::any_cast<LabelLayoutCache>(&cached->second);
  const Size measured_text = show_label && layout != nullptr ? layout->text.size : Size{};
  const float spacing = show_label && icon_width > 0.0F ? std::max(0.0F, metrics.icon_spacing) : 0.0F;
  const float available_text_width = std::max(0.0F, content.width - icon_width - spacing);
  const float text_width = std::min(measured_text.width, available_text_width);
  const float group_width = icon_width + spacing + text_width;
  const float leading = content.x + std::max(0.0F, (content.width - group_width) * 0.5F);
  const Rect icon_bounds{
      leading,
      content.y + std::max(0.0F, (content.height - icon_height) * 0.5F),
      icon_width,
      icon_height,
  };
  PaintImage(node, context, icon_bounds, text_style.foreground);
  if (!show_label || text_width <= 0.0F) {
    return;
  }
  TextLayoutOptions options = node.properties.text_layout_options;
  options.align = TextAlign::Leading;
  options.wrap = TextWrap::NoWrap;
  context.DrawText(
      {leading + icon_width + spacing, content.y, text_width, content.height},
      node.text,
      text_style,
      options
  );
}

Rect RenderClipBounds(const RenderClip& clip) {
  return std::visit(
      [](const auto& command) -> Rect {
        using Command = std::decay_t<decltype(command)>;
        if constexpr (std::is_same_v<Command, PushClipCommand>) {
          return command.rect;
        } else {
          return command.path.Bounds();
        }
      },
      clip
  );
}

std::vector<RenderClip> ResolveChildClips(const MountedNode& node) {
  std::vector<RenderClip> clips;
  if (node.properties.clip_children) {
    if (node.properties.corner_radii.IsUniform()) {
      clips.emplace_back(PushClipCommand{node.bounds, std::max(0.0F, node.properties.corner_radii.top_left)});
    } else {
      clips.emplace_back(PushPathClipCommand{Path::RoundedRect(node.bounds, node.properties.corner_radii)});
    }
  }
  if (IsScrollContainer(node)) {
    const Rect bounds = node.ContentBounds();
    clips.emplace_back(PushClipCommand{bounds, 0.0F});
  }
  return clips;
}

Transform2D ResolveChildrenTransform(const MountedNode& node) {
  if (node.kind != NodeKind::ScrollView || node.scroll_state == nullptr) {
    return {};
  }
  return ScrollAxis(node) == Axis::Vertical ? TranslationTransform({0.0F, -node.scroll_state->offset_y})
                                            : TranslationTransform({-node.scroll_state->offset_x, 0.0F});
}

std::optional<Rect> UnionBounds(std::optional<Rect> left, Rect right) {
  if (right.IsEmpty()) {
    return left;
  }
  if (!left.has_value() || left->IsEmpty()) {
    return right;
  }
  const float min_x = std::min(left->x, right.x);
  const float min_y = std::min(left->y, right.y);
  const float max_x = std::max(left->x + left->width, right.x + right.width);
  const float max_y = std::max(left->y + left->height, right.y + right.height);
  return Rect{
      min_x,
      min_y,
      max_x - min_x,
      max_y - min_y,
  };
}

std::optional<Rect> ClipBounds(std::optional<Rect> bounds, const std::optional<Rect>& clip) {
  if (!bounds.has_value() || !clip.has_value()) {
    return bounds;
  }
  const Rect intersection = bounds->Intersection(*clip);
  return intersection.IsEmpty() ? std::nullopt : std::optional<Rect>{intersection};
}

std::optional<Rect> ResolveClip(
    const std::optional<Rect>& inherited_clip,
    const Transform2D& world_transform,
    const std::vector<RenderClip>& local_clips
) {
  std::optional<Rect> resolved = inherited_clip;
  for (const RenderClip& local_clip : local_clips) {
    const Rect world_clip = TransformBounds(world_transform, RenderClipBounds(local_clip));
    resolved = resolved.has_value() ? resolved->Intersection(world_clip) : world_clip;
  }
  return resolved;
}

std::optional<Rect> SnapshotRenderNode(
    const RenderNode& node,
    const Transform2D& inherited_transform,
    const std::optional<Rect>& inherited_clip,
    RenderSceneSnapshot& snapshot
) {
  RenderNodeSnapshot node_snapshot;
  node_snapshot.content_revision = node.content.Revision();
  node_snapshot.foreground_revision = node.foreground.Revision();
  node_snapshot.visible = node.visible;
  node_snapshot.opacity = node.opacity;
  node_snapshot.world_clip = inherited_clip;
  node_snapshot.child_clips = node.child_clips;

  node_snapshot.children.reserve(node.children.size());
  for (const RenderNode* child : node.children) {
    if (child != nullptr) {
      node_snapshot.children.push_back(child->id);
    }
  }

  const Transform2D local_transform = ComposeTransform(TranslationTransform(node.offset), node.transform);
  node_snapshot.world_transform = ComposeTransform(inherited_transform, local_transform);
  node_snapshot.world_children_transform = ComposeTransform(node_snapshot.world_transform, node.children_transform);
  if (!node.visible) {
    snapshot.insert_or_assign(node.id, std::move(node_snapshot));
    return std::nullopt;
  }

  std::optional<Rect> own_bounds;
  own_bounds = UnionBounds(std::move(own_bounds), node.content.Bounds());
  own_bounds = UnionBounds(std::move(own_bounds), node.foreground.Bounds());
  if (own_bounds.has_value()) {
    own_bounds = TransformBounds(node_snapshot.world_transform, *own_bounds);
    own_bounds = ClipBounds(std::move(own_bounds), inherited_clip);
  }
  if (own_bounds.has_value()) {
    node_snapshot.own_bounds = *own_bounds;
    node_snapshot.has_own_bounds = true;
  }

  node_snapshot.world_child_clip = ResolveClip(inherited_clip, node_snapshot.world_transform, node.child_clips);
  std::optional<Rect> subtree_bounds = own_bounds;
  for (const RenderNode* child : node.children) {
    if (child == nullptr) {
      continue;
    }
    const std::optional<Rect> child_bounds =
        SnapshotRenderNode(*child, node_snapshot.world_children_transform, node_snapshot.world_child_clip, snapshot);
    if (child_bounds.has_value()) {
      subtree_bounds = UnionBounds(std::move(subtree_bounds), *child_bounds);
    }
  }
  if (subtree_bounds.has_value()) {
    node_snapshot.subtree_bounds = *subtree_bounds;
    node_snapshot.has_subtree_bounds = true;
  }
  snapshot.insert_or_assign(node.id, std::move(node_snapshot));
  return subtree_bounds;
}

bool TouchesOrIntersects(Rect left, Rect right) {
  return left.x <= right.x + right.width && left.x + left.width >= right.x && left.y <= right.y + right.height &&
         left.y + left.height >= right.y;
}

void AddDamageRect(DamageRegion& damage, Rect rect, Rect viewport) {
  if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) || !std::isfinite(rect.height)) {
    damage.full = true;
    damage.rects = {viewport};
    return;
  }
  rect = rect.Intersection(viewport);
  if (rect.IsEmpty()) {
    return;
  }

  for (std::size_t index = 0; index < damage.rects.size();) {
    if (!TouchesOrIntersects(damage.rects[index], rect)) {
      ++index;
      continue;
    }
    rect = *UnionBounds(damage.rects[index], rect);
    damage.rects.erase(damage.rects.begin() + static_cast<std::ptrdiff_t>(index));
    index = 0;
  }
  damage.rects.push_back(rect);
}

void AddSnapshotBounds(DamageRegion& damage, const RenderNodeSnapshot& snapshot, bool subtree, Rect viewport) {
  if (subtree ? snapshot.has_subtree_bounds : snapshot.has_own_bounds) {
    AddDamageRect(damage, subtree ? snapshot.subtree_bounds : snapshot.own_bounds, viewport);
  }
}

bool CommonChildOrderChanged(const std::vector<std::uint64_t>& previous, const std::vector<std::uint64_t>& current) {
  if (previous == current) {
    return false;
  }
  const std::unordered_set<std::uint64_t> previous_ids(previous.begin(), previous.end());
  const std::unordered_set<std::uint64_t> current_ids(current.begin(), current.end());
  std::vector<std::uint64_t> previous_common;
  std::vector<std::uint64_t> current_common;
  previous_common.reserve(previous.size());
  current_common.reserve(current.size());
  for (std::uint64_t id : previous) {
    if (current_ids.contains(id)) {
      previous_common.push_back(id);
    }
  }
  for (std::uint64_t id : current) {
    if (previous_ids.contains(id)) {
      current_common.push_back(id);
    }
  }
  return previous_common != current_common;
}

void ResolvePresentationTreeImpl(MountedNode& node, const Transform2D& inherited_transform, float inherited_opacity) {
  const Transform2D node_transform =
      ComposeTransform(TranslationTransform(node.layout_offset), node.presentation.local_transform);
  node.presentation.resolved_transform = ComposeTransform(inherited_transform, node_transform);
  // render_opacity is emitted as this node's group opacity. resolved_opacity is the inherited product used for
  // visibility and descendant geometry without baking ancestor opacity into retained paint commands.
  float render_opacity = std::clamp(node.presentation.local_opacity, 0.0F, 1.0F);
  // Apply disabled opacity only when entering a disabled subtree; descendants inherit the result without multiplying
  // the same disabled state again.
  if (node.disabled_visual_state) {
    render_opacity *= node.properties.disabled_opacity;
  }
  node.presentation.render_opacity = render_opacity;
  node.presentation.resolved_opacity = inherited_opacity * render_opacity;
  const Transform2D children_transform =
      ComposeTransform(node.presentation.resolved_transform, ResolveChildrenTransform(node));
  for (auto& child : node.children) {
    ResolvePresentationTreeImpl(*child, children_transform, node.presentation.resolved_opacity);
  }
}

void PaintNodeExtensions(MountedNode& node, PaintContext& context) {
  for (const auto& entry : node.extensions) {
    if (entry.extension) {
      entry.extension->Paint(node, context);
    }
  }
}

void PaintFocusRing(const MountedNode& node, const Rect& bounds, PaintContext& context) {
  if (!node.focus_visible || !node.enabled || node.properties.focus_ring_width <= 0.0F) {
    return;
  }
  context.DrawBorder(
      bounds,
      node.properties.focus_ring,
      node.properties.focus_ring_width,
      node.properties.corner_radii
  );
}

void PaintNodeWithinClip(MountedNode& node, const Rect& clip, const RenderNode* extra_child = nullptr) {
  RenderNode& render_node = node.render_node;
  const Transform2D& local_transform = node.presentation.local_transform;
  const Transform2D children_transform = ResolveChildrenTransform(node);
  const Transform2D& transform = node.presentation.resolved_transform;
  const float opacity = node.presentation.resolved_opacity;

  Rect child_clip = clip;
  std::vector<RenderClip> child_clips = ResolveChildClips(node);
  for (const RenderClip& render_clip : child_clips) {
    child_clip = child_clip.Intersection(TransformBounds(transform, RenderClipBounds(render_clip)));
  }

  bool changed = false;
  if (node.content_paint_dirty) {
    const Rect bounds = node.bounds;
    const Rect canvas_bounds = node.kind == NodeKind::Canvas
                                   ? Rect{
                                         0.0F,
                                         0.0F,
                                         std::max(0.0F, bounds.width - node.properties.padding.Horizontal()),
                                         std::max(0.0F, bounds.height - node.properties.padding.Vertical()),
                                     }
                                   : bounds;
    PaintContext content{render_node.content, canvas_bounds};
    std::optional<Color> background = node.properties.background;
    std::optional<Color> border = node.properties.border;
    TextStyle text_style = node.properties.text_style;
    if (node.disabled_visual_state) {
      if (node.properties.disabled_background.has_value()) {
        background = node.properties.disabled_background;
      }
      if (node.properties.disabled_foreground.has_value()) {
        text_style.foreground = *node.properties.disabled_foreground;
      }
      if (node.properties.disabled_border.has_value()) {
        border = node.properties.disabled_border;
      }
    }
    if (node.properties.shadow.has_value() && node.properties.shadow->color.alpha > 0.0F) {
      const Shadow& shadow = *node.properties.shadow;
      content.DrawShadow(
          bounds,
          shadow.color,
          shadow.offset,
          shadow.blur_radius,
          shadow.spread,
          node.properties.corner_radii
      );
    }
    if (background.has_value() && background->alpha > 0.0F) {
      content.DrawRect(
          node.kind == NodeKind::Divider ? node.ContentBounds() : bounds,
          *background,
          node.properties.corner_radii
      );
    }
    if (border.has_value() && border->alpha > 0.0F && node.properties.border_width > 0.0F) {
      content.DrawBorder(bounds, *border, node.properties.border_width, node.properties.corner_radii);
    }
    if (node.kind == NodeKind::Text) {
      if (node.image_properties.HasValue() || node.layout_values.contains(typeid(LabelContentMetrics))) {
        PaintLabelContent(node, content, text_style);
      } else {
        content.DrawText(
            node.ContentBounds(), node.text, node.properties.text_style, node.properties.text_layout_options
        );
      }
    } else if (node.kind == NodeKind::IconButton ||
               (node.kind == NodeKind::Chip && node.image_properties.HasValue())) {
      PaintLabelContent(node, content, text_style);
    } else if (node.kind == NodeKind::Button || node.kind == NodeKind::Chip) {
      content.DrawText(
          bounds,
          node.text,
          text_style,
          TextLayoutOptions{.align = TextAlign::Center, .wrap = TextWrap::NoWrap}
      );
    } else if ((node.kind == NodeKind::Checkbox || node.kind == NodeKind::RadioButton ||
                node.kind == NodeKind::Switch) &&
               !node.text.empty()) {
      content.DrawText(
          ResolveToggleLabelBounds(node),
          node.text,
          text_style,
          node.properties.text_layout_options
      );
    } else if (node.kind == NodeKind::Image) {
      PaintImage(node, content);
    } else if (node.kind == NodeKind::Canvas && node.canvas_painter) {
      const Point content_origin{node.properties.padding.left, node.properties.padding.top};
      if (content_origin != Point{}) {
        content.PushTransform(TranslationTransform(content_origin));
      }
      node.canvas_painter(content, {canvas_bounds.width, canvas_bounds.height});
      if (content_origin != Point{}) {
        content.PopTransform();
      }
    }
    content.Finish();
    node.content_paint_dirty = false;
    changed = true;
  }

  if (node.foreground_paint_dirty) {
    const Rect bounds = node.bounds;
    PaintContext foreground{render_node.foreground, node.bounds};
    PaintNodeExtensions(node, foreground);
    PaintFocusRing(node, bounds, foreground);
    foreground.Finish();
    node.foreground_paint_dirty = false;
    changed = true;
  }

  std::optional<Rect> own_paint_bounds;
  own_paint_bounds = UnionBounds(std::move(own_paint_bounds), render_node.content.Bounds());
  own_paint_bounds = UnionBounds(std::move(own_paint_bounds), render_node.foreground.Bounds());
  const bool own_visible =
      opacity > 0.0F && own_paint_bounds.has_value() && TransformBounds(transform, *own_paint_bounds).Intersects(clip);

  std::vector<const RenderNode*> children;
  children.reserve(node.children.size());
  for (const auto& child : node.children) {
    PaintNodeWithinClip(*child, child_clip);
    children.push_back(&child->render_node);
  }
  if (extra_child != nullptr) {
    children.push_back(extra_child);
  }
  const bool visible = own_visible || std::any_of(children.begin(), children.end(), [](const RenderNode* child) {
                         return child != nullptr && child->visible;
                       });

  if (render_node.id != node.identity) {
    render_node.id = node.identity;
    changed = true;
  }
  if (render_node.offset != node.layout_offset) {
    render_node.offset = node.layout_offset;
    changed = true;
  }
  if (render_node.transform != local_transform) {
    render_node.transform = local_transform;
    changed = true;
  }
  if (render_node.opacity != node.presentation.render_opacity) {
    render_node.opacity = node.presentation.render_opacity;
    changed = true;
  }
  if (render_node.child_clips != child_clips) {
    render_node.child_clips = std::move(child_clips);
    changed = true;
  }
  if (render_node.children_transform != children_transform) {
    render_node.children_transform = children_transform;
    changed = true;
  }
  if (render_node.children != children) {
    render_node.children = std::move(children);
    changed = true;
  }
  if (render_node.visible != visible) {
    render_node.visible = visible;
    changed = true;
  }
  if (changed) {
    ++render_node.revision;
  }
}

} // namespace

void ResolvePresentationTree(MountedNode& node) {
  ResolvePresentationTreeImpl(node, Transform2D{}, 1.0F);
}

void UpdateRenderScene(MountedNode& node, Rect clip, const RenderNode* overlay) {
  PaintNodeWithinClip(node, clip, overlay);
}

DamageRegion ComputeDamageRegion(
    const RenderNode* root,
    Size viewport,
    RenderSceneSnapshot& committed_scene,
    Size& committed_viewport,
    bool& has_committed_scene
) {
  const Rect viewport_bounds{
      0.0F,
      0.0F,
      viewport.width,
      viewport.height,
  };
  RenderSceneSnapshot current_scene;
  if (root != nullptr) {
    SnapshotRenderNode(*root, Transform2D{}, std::nullopt, current_scene);
  }

  DamageRegion damage;
  if (!has_committed_scene || committed_viewport != viewport) {
    damage.full = true;
    if (!viewport_bounds.IsEmpty()) {
      damage.rects.push_back(viewport_bounds);
    }
  } else {
    for (const auto& [id, current] : current_scene) {
      const auto previous_entry = committed_scene.find(id);
      if (previous_entry == committed_scene.end()) {
        AddSnapshotBounds(damage, current, true, viewport_bounds);
        continue;
      }

      const RenderNodeSnapshot& previous = previous_entry->second;
      const bool presentation_changed = current.visible != previous.visible || current.opacity != previous.opacity ||
                                        current.world_transform != previous.world_transform ||
                                        current.world_children_transform != previous.world_children_transform ||
                                        current.world_clip != previous.world_clip ||
                                        current.world_child_clip != previous.world_child_clip ||
                                        current.child_clips != previous.child_clips;
      if (presentation_changed || CommonChildOrderChanged(previous.children, current.children)) {
        // Presentation and ordering changes can move or recomposite descendant pixels, so both old and new subtree
        // bounds must be invalidated.
        AddSnapshotBounds(damage, previous, true, viewport_bounds);
        AddSnapshotBounds(damage, current, true, viewport_bounds);
      } else if (
          current.content_revision != previous.content_revision ||
          current.foreground_revision != previous.foreground_revision
      ) {
        // A rerecorded sequence affects this node's content or foreground only; unchanged descendants remain valid.
        AddSnapshotBounds(damage, previous, false, viewport_bounds);
        AddSnapshotBounds(damage, current, false, viewport_bounds);
      }
    }

    for (const auto& [id, previous] : committed_scene) {
      if (!current_scene.contains(id)) {
        AddSnapshotBounds(damage, previous, true, viewport_bounds);
      }
    }
  }

  committed_scene = std::move(current_scene);
  committed_viewport = viewport;
  has_committed_scene = true;
  return damage;
}

} // namespace huxerui::detail
