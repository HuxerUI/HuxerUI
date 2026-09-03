#include "internal.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "external_texture_internal.h"
#include "paint_internal.h"

namespace huxerui::detail {

PaintSequence FrozenScene::CopyPaintSequence(const PaintSequence& source) {
  PaintSequence copy = source;
  std::erase_if(copy.commands_, [](const PaintCommand& command) {
    return std::holds_alternative<PlacePlatformViewCommand>(command);
  });
  return copy;
}

namespace {

RenderNode* FreezeRenderNode(const RenderNode& source, FrozenScene& scene, std::uint64_t& next_identity) {
  auto frozen = std::make_unique<RenderNode>();
  RenderNode* const result = frozen.get();
  frozen->id = next_identity--;
  frozen->offset = source.offset;
  frozen->transform = source.transform;
  frozen->opacity = source.opacity;
  frozen->child_clips = source.child_clips;
  frozen->children_transform = source.children_transform;
  frozen->content = FrozenScene::CopyPaintSequence(source.content);
  frozen->foreground = FrozenScene::CopyPaintSequence(source.foreground);
  frozen->visible = source.visible;
  frozen->revision = source.revision;
  scene.nodes.push_back(std::move(frozen));
  result->children.reserve(source.children.size());
  for (const RenderNode* child : source.children) {
    if (child != nullptr) {
      result->children.push_back(FreezeRenderNode(*child, scene, next_identity));
    }
  }
  return result;
}

bool RenderNodeHasPlatformViews(const RenderNode& node) {
  const auto sequence_has_platform_view = [](const PaintSequence& sequence) {
    return std::any_of(sequence.Commands().begin(), sequence.Commands().end(), [](const PaintCommand& command) {
      return std::holds_alternative<PlacePlatformViewCommand>(command);
    });
  };
  if (sequence_has_platform_view(node.content) || sequence_has_platform_view(node.foreground)) {
    return true;
  }
  return std::any_of(node.children.begin(), node.children.end(), [](const RenderNode* child) {
    return child != nullptr && RenderNodeHasPlatformViews(*child);
  });
}

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
    const ImageProperties& properties,
    PaintContext& context,
    Rect content,
    std::optional<Color> vector_tint,
    float opacity = 1.0F
) {
  const Size intrinsic = properties.IntrinsicSize();
  if (intrinsic.width <= 0.0F || intrinsic.height <= 0.0F || content.IsEmpty()) {
    return;
  }
  const auto draw = [&](Rect source, Rect destination) {
    std::visit(
        [&](const auto& asset) {
          using Asset = std::decay_t<decltype(asset)>;
          if constexpr (
              std::same_as<Asset, ImageAsset> || std::same_as<Asset, std::shared_ptr<ExternalTexture>>
          ) {
            context.DrawImageRect(asset, source, destination, properties.sampling, opacity);
          } else if constexpr (std::same_as<Asset, VectorAsset>) {
            context.DrawImageRect(asset, source, destination, vector_tint, opacity);
          } else {
            throw std::logic_error("HuxerUI unresolved image resource reached retained paint recording");
          }
        },
        properties.source
    );
  };
  const Rect full_source{0.0F, 0.0F, intrinsic.width, intrinsic.height};
  if (properties.fit == ImageFit::Fill) {
    draw(full_source, content);
    return;
  }
  if (properties.fit == ImageFit::Cover) {
    const float scale = std::max(content.width / intrinsic.width, content.height / intrinsic.height);
    const Size source_size{content.width / scale, content.height / scale};
    const Rect source{
        AlignOffset(intrinsic.width, source_size.width, properties.horizontal_alignment),
        AlignOffset(intrinsic.height, source_size.height, properties.vertical_alignment),
        source_size.width,
        source_size.height,
    };
    draw(source, content);
    return;
  }
  float scale = 1.0F;
  if (properties.fit == ImageFit::Contain || properties.fit == ImageFit::ScaleDown) {
    scale = std::min(content.width / intrinsic.width, content.height / intrinsic.height);
    if (properties.fit == ImageFit::ScaleDown) {
      scale = std::min(1.0F, scale);
    }
  }
  const Size destination_size{intrinsic.width * scale, intrinsic.height * scale};
  const Rect destination{
      content.x + AlignOffset(content.width, destination_size.width, properties.horizontal_alignment),
      content.y + AlignOffset(content.height, destination_size.height, properties.vertical_alignment),
      destination_size.width,
      destination_size.height,
  };
  draw(full_source, destination);
}

void PaintImage(const MountedNode& node, PaintContext& context) {
  PaintImage(node.image_properties, context, node.ContentBounds(), node.image_properties.tint);
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
  PaintImage(node.image_properties, context, icon_bounds, text_style.foreground);
  if (!show_label || text_width <= 0.0F) {
    return;
  }
  context.DrawText(
      {leading + icon_width + spacing, content.y, text_width, content.height},
      node.text,
      text_style,
      node.properties.text_layout_options
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
    const CornerRadii corner_radii = NormalizeCornerRadii(node.bounds, node.resolved_corner_radii);
    if (corner_radii.IsUniform()) {
      clips.emplace_back(PushClipCommand{node.bounds, corner_radii.top_left});
    } else {
      clips.emplace_back(PushPathClipCommand{Path::RoundedRect(node.bounds, corner_radii)});
    }
  }
  if (IsScrollContainer(node)) {
    const Rect bounds = node.ContentBounds();
    clips.emplace_back(PushClipCommand{bounds, 0.0F});
  }
  return clips;
}

Transform2D ResolveChildrenTransform(const MountedNode& node) {
  Transform2D scroll_transform;
  if (node.scroll_state != nullptr) {
    const bool vertical = ScrollAxis(node) == Axis::Vertical;
    const float content_offset = node.kind == NodeKind::ScrollView
                                     ? (vertical ? node.scroll_state->offset_y : node.scroll_state->offset_x)
                                     : 0.0F;
    const float translation = -content_offset - node.scroll_state->overscroll_offset;
    scroll_transform = vertical ? TranslationTransform({0.0F, translation})
                                : TranslationTransform({translation, 0.0F});
  }
  return ComposeTransform(node.presentation.children_transform, scroll_transform);
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

void SnapshotExternalTextures(
    const PaintSequence& sequence,
    const Transform2D& world_transform,
    const std::optional<Rect>& world_clip,
    std::vector<ExternalTextureUseSnapshot>& textures
) {
  if (!sequence.HasExternalTextureCommands()) {
    return;
  }
  Transform2D transform = world_transform;
  std::optional<Rect> clip = world_clip;
  std::vector<Transform2D> transforms;
  std::vector<std::optional<Rect>> clips;
  for (const PaintCommand& command : sequence.Commands()) {
    std::visit(
        [&](const auto& value) {
          using Command = std::decay_t<decltype(value)>;
          if constexpr (std::same_as<Command, DrawExternalTextureCommand>) {
            Rect bounds = TransformBounds(transform, value.destination);
            if (clip.has_value()) {
              bounds = bounds.Intersection(*clip);
            }
            if (!bounds.IsEmpty()) {
              textures.push_back({value.texture, value.texture->Revision(), bounds});
            }
          } else if constexpr (std::same_as<Command, PushTransformCommand>) {
            transforms.push_back(transform);
            transform = ComposeTransform(transform, value.transform);
          } else if constexpr (std::same_as<Command, PopTransformCommand>) {
            transform = transforms.back();
            transforms.pop_back();
          } else if constexpr (std::same_as<Command, PushClipCommand>) {
            clips.push_back(clip);
            const Rect bounds = TransformBounds(transform, value.rect);
            clip = clip.has_value() ? clip->Intersection(bounds) : std::optional<Rect>{bounds};
          } else if constexpr (std::same_as<Command, PushPathClipCommand>) {
            clips.push_back(clip);
            const Rect bounds = TransformBounds(transform, value.path.Bounds());
            clip = clip.has_value() ? clip->Intersection(bounds) : std::optional<Rect>{bounds};
          } else if constexpr (std::same_as<Command, PopClipCommand>) {
            clip = clips.back();
            clips.pop_back();
          }
        },
        command
    );
  }
}

std::optional<Rect> SnapshotRenderNode(
    const RenderNode& node,
    const Transform2D& inherited_transform,
    const std::optional<Rect>& inherited_clip,
    RenderDamageSnapshot& snapshot
) {
  RenderNodeSnapshot node_snapshot;
  node_snapshot.content_revision = node.content.Revision();
  node_snapshot.foreground_revision = node.foreground.Revision();
  node_snapshot.visible = node.visible;
  node_snapshot.opacity = node.opacity;
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
  node_snapshot.world_clip = inherited_clip;
  if (!node.visible) {
    snapshot.insert_or_assign(node.id, std::move(node_snapshot));
    return std::nullopt;
  }

  SnapshotExternalTextures(
      node.content, node_snapshot.world_transform, node_snapshot.world_clip, node_snapshot.external_textures
  );
  SnapshotExternalTextures(
      node.foreground, node_snapshot.world_transform, node_snapshot.world_clip, node_snapshot.external_textures
  );

  std::optional<Rect> own_bounds;
  own_bounds = UnionBounds(std::move(own_bounds), node.content.Bounds());
  own_bounds = UnionBounds(std::move(own_bounds), node.foreground.Bounds());
  if (own_bounds.has_value()) {
    own_bounds = TransformBounds(node_snapshot.world_transform, *own_bounds);
    own_bounds = ClipBounds(std::move(own_bounds), node_snapshot.world_clip);
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
    const std::optional<Rect> child_bounds = SnapshotRenderNode(
        *child, node_snapshot.world_children_transform, node_snapshot.world_child_clip, snapshot
    );
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

void AddExternalTextureDamage(
    DamageRegion& damage,
    const std::vector<ExternalTextureUseSnapshot>& previous,
    const std::vector<ExternalTextureUseSnapshot>& current,
    Rect viewport
) {
  if (previous.size() != current.size()) {
    for (const ExternalTextureUseSnapshot& texture : previous) {
      AddDamageRect(damage, texture.bounds, viewport);
    }
    for (const ExternalTextureUseSnapshot& texture : current) {
      AddDamageRect(damage, texture.bounds, viewport);
    }
    return;
  }
  for (std::size_t index = 0; index < current.size(); ++index) {
    const ExternalTextureUseSnapshot& before = previous[index];
    const ExternalTextureUseSnapshot& after = current[index];
    if (before.texture != after.texture || before.bounds != after.bounds) {
      AddDamageRect(damage, before.bounds, viewport);
      AddDamageRect(damage, after.bounds, viewport);
    } else if (before.revision != after.revision) {
      AddDamageRect(damage, after.bounds, viewport);
    }
  }
}

std::unordered_map<ExternalTexture*, std::shared_ptr<ExternalTexture>> ExternalTextures(
    const RenderDamageSnapshot& scene
) {
  std::unordered_map<ExternalTexture*, std::shared_ptr<ExternalTexture>> textures;
  for (const auto& [id, node] : scene) {
    static_cast<void>(id);
    for (const ExternalTextureUseSnapshot& texture : node.external_textures) {
      textures.try_emplace(texture.texture.get(), texture.texture);
    }
  }
  return textures;
}

void UpdateExternalTextureActivity(
    const RenderDamageSnapshot& previous,
    const RenderDamageSnapshot& current,
    const std::shared_ptr<ExternalTextureFrameRequester>& texture_frame_requester
) {
  const auto previous_textures = ExternalTextures(previous);
  const auto current_textures = ExternalTextures(current);
  for (const auto& [identity, texture] : previous_textures) {
    if (!current_textures.contains(identity)) {
      texture_frame_requester->SetActive(texture, false);
    }
  }
  for (const auto& [identity, texture] : current_textures) {
    if (!previous_textures.contains(identity)) {
      texture_frame_requester->SetActive(texture, true);
    }
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
  if (!node.participates_in_layout) {
    node.presentation.render_opacity = 0.0F;
    node.presentation.resolved_opacity = 0.0F;
    return;
  }
  // render_opacity is emitted as this node's group opacity. resolved_opacity is the inherited product used for
  // visibility and descendant geometry without baking ancestor opacity into retained paint commands.
  float render_opacity = std::clamp(node.presentation.local_opacity, 0.0F, 1.0F);
  // Apply disabled opacity only when entering a disabled subtree; descendants inherit the result without multiplying
  // the same disabled state again.
  if (node.applies_disabled_appearance) {
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

void PaintNodeExtensionsBehindContent(MountedNode& node, PaintContext& context) {
  for (const auto& entry : node.extensions) {
    if (entry.extension) {
      entry.extension->PaintBehindContent(node, context);
    }
  }
}

void PaintNodeExtensionsAboveContent(MountedNode& node, PaintContext& context) {
  for (const auto& entry : node.extensions) {
    if (entry.extension) {
      entry.extension->PaintAboveContent(node, context);
    }
  }
}

void PaintFocusRing(const MountedNode& node, PaintContext& context) {
  const FocusRing& ring = node.properties.focus_ring;
  if (!node.interaction.enabled || !node.interaction.focus_visible || ring.width <= 0.0F) {
    return;
  }
  const Rect frame = node.indication_bounds_override.value_or(node.bounds);
  const float width = std::max(0.0F, ring.width);
  const float outset = std::max(0.0F, ring.offset) + width;
  CornerRadii radii = node.resolved_corner_radii;
  radii.top_left += outset;
  radii.top_right += outset;
  radii.bottom_right += outset;
  radii.bottom_left += outset;
  context.DrawBorder(
      {
          frame.x - outset,
          frame.y - outset,
          frame.width + outset * 2.0F,
          frame.height + outset * 2.0F,
      },
      ring.color, StrokeStyle{.width = width}, radii);
}

void HideRenderTree(MountedNode& node) {
  RenderNode& render_node = node.render_node;
  // Non-participating subtrees keep their retained RenderNode links so existing invisible PlatformView placements can
  // preserve native instances. Rebuild those links only after mounted child structure changes.
  if (!node.render_structure_dirty && !render_node.visible) {
    return;
  }
  std::vector<const RenderNode*> children;
  children.reserve(node.children.size());
  for (auto& child : node.children) {
    HideRenderTree(*child);
    children.push_back(&child->render_node);
  }
  bool changed = false;
  if (render_node.id != node.identity) {
    render_node.id = node.identity;
    changed = true;
  }
  if (render_node.opacity != 0.0F) {
    render_node.opacity = 0.0F;
    changed = true;
  }
  if (render_node.children != children) {
    render_node.children = std::move(children);
    changed = true;
  }
  if (render_node.visible) {
    render_node.visible = false;
    changed = true;
  }
  if (changed) {
    ++render_node.revision;
  }
  node.render_structure_dirty = false;
}

} // namespace

void PaintNodeWithinClip(huxerui::MountedNode& mounted_node, const Rect& clip, const RenderNode* extra_child) {
  auto& node = static_cast<detail::MountedNode&>(mounted_node);
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
                                         std::max(0.0F, bounds.width - node.resolved_padding.Horizontal()),
                                         std::max(0.0F, bounds.height - node.resolved_padding.Vertical()),
                                     }
                                   : bounds;
    PaintContext content{
        render_node.content, canvas_bounds,
        node.kind == NodeKind::Canvas ? node.properties.text_layout_options.shaping.locale : std::string{},
    };
    std::optional<VisualFill> background = node.properties.background;
    TextStyle text_style = node.properties.text_style;
    if (node.applies_disabled_appearance) {
      if (node.properties.disabled_background.has_value()) {
        background = node.properties.disabled_background;
      }
      if (node.properties.disabled_foreground.has_value()) {
        text_style.foreground = *node.properties.disabled_foreground;
      }
    }
    if (node.properties.shadow.has_value() && node.properties.shadow->color.alpha > 0.0F) {
      const Shadow& shadow = *node.properties.shadow;
      content.DrawShadow(bounds, shadow.color, shadow.offset, shadow.blur_radius, shadow.spread,
                         node.resolved_corner_radii);
    }
    if (background.has_value()) {
      PaintVisualFill(content, node.kind == NodeKind::Divider ? node.ContentBounds() : bounds, *background,
                      node.resolved_corner_radii);
    }
    PaintNodeExtensionsBehindContent(node, content);
    if (node.resolved_border.has_value() && node.resolved_border->color.alpha > 0.0F &&
        node.resolved_border->width > 0.0F) {
      content.DrawBorder(bounds, node.resolved_border->color,
                         StrokeStyle{.width = node.resolved_border->width}, node.resolved_corner_radii);
    }
    if (node.kind == NodeKind::Text) {
      if (node.image_properties.HasValue() || node.layout_values.contains(typeid(LabelContentMetrics))) {
        PaintLabelContent(node, content, text_style);
      } else {
        content.DrawText(
            node.ContentBounds(), node.text, text_style, node.properties.text_layout_options
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
          node.properties.text_layout_options
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
    } else if (node.kind == NodeKind::PlatformView) {
      PlatformViewPaintAccess::Paint(node, content);
    } else if (node.kind == NodeKind::Canvas && node.canvas_painter) {
      const Point content_origin{node.resolved_padding.left, node.resolved_padding.top};
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
    PaintContext foreground{render_node.foreground, node.bounds};
    PaintNodeExtensionsAboveContent(node, foreground);
    PaintFocusRing(node, foreground);
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
    if (child->participates_in_layout) {
      PaintNodeWithinClip(*child, child_clip, nullptr);
    } else {
      HideRenderTree(*child);
    }
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
  node.render_structure_dirty = false;
}

void PaintVisualFill(PaintContext& context, Rect bounds, const VisualFill& fill, CornerRadii corner_radii,
                     float opacity) {
  opacity = std::clamp(opacity, 0.0F, 1.0F);
  if (opacity <= 0.0F || bounds.IsEmpty()) {
    return;
  }
  std::visit(
      [&](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::same_as<Value, Brush>) {
          context.DrawRect(bounds, detail::ModulateBrush(value, {}, opacity), corner_radii);
        } else {
          ImageProperties properties;
          properties.fit = value.fit;
          properties.horizontal_alignment = value.horizontal_alignment;
          properties.vertical_alignment = value.vertical_alignment;
          properties.sampling = value.sampling;
          properties.tint = value.tint;
          const float image_opacity = opacity * std::clamp(value.opacity, 0.0F, 1.0F);
          std::visit(
              [&](const auto& source) {
                using Source = std::decay_t<decltype(source)>;
                if constexpr (std::same_as<Source, ImageResource>) {
                  throw std::logic_error("HuxerUI unresolved image fill resource reached retained paint recording");
                } else {
                  properties.source = source;
                }
              },
              value.source
          );
          if (image_opacity <= 0.0F || !properties.HasValue()) {
            return;
          }
          context.PushClip(bounds, corner_radii);
          PaintImage(properties, context, bounds, properties.tint, image_opacity);
          context.PopClip();
        }
      },
      fill.Get()
  );
}

std::shared_ptr<FrozenScene> FreezeRenderScene(const RenderNode* root) {
  auto frozen = std::make_shared<FrozenScene>();
  if (root == nullptr) {
    return frozen;
  }
  std::uint64_t next_identity = 0xEFFFFFFFFFFFFFFFULL;
  frozen->root = FreezeRenderNode(*root, *frozen, next_identity);
  return frozen;
}

bool RenderSceneHasPlatformViews(const RenderNode* root) {
  return root != nullptr && RenderNodeHasPlatformViews(*root);
}

void ResolvePresentationTree(MountedNode& node) {
  ResolvePresentationTreeImpl(node, Transform2D{}, 1.0F);
}

void UpdateRenderScene(MountedNode& node, Rect clip, const RenderNode* overlay) {
  PaintNodeWithinClip(node, clip, overlay);
}

DamageRegion ComputeDamageRegion(
    const RenderNode* root,
    Size viewport,
    RenderDamageSnapshot& committed_scene,
    Size& committed_viewport,
    bool& has_committed_scene,
    const std::shared_ptr<ExternalTextureFrameRequester>& texture_frame_requester
) {
  const Rect viewport_bounds{
      0.0F,
      0.0F,
      viewport.width,
      viewport.height,
  };
  RenderDamageSnapshot current_scene;
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
      } else {
        AddExternalTextureDamage(damage, previous.external_textures, current.external_textures, viewport_bounds);
      }
    }

    for (const auto& [id, previous] : committed_scene) {
      if (!current_scene.contains(id)) {
        AddSnapshotBounds(damage, previous, true, viewport_bounds);
      }
    }
  }

  UpdateExternalTextureActivity(committed_scene, current_scene, texture_frame_requester);
  committed_scene = std::move(current_scene);
  committed_viewport = viewport;
  has_committed_scene = true;
  return damage;
}

void DeactivateExternalTextures(
    RenderDamageSnapshot& committed_scene,
    const std::shared_ptr<ExternalTextureFrameRequester>& texture_frame_requester
) {
  for (const auto& [identity, texture] : ExternalTextures(committed_scene)) {
    static_cast<void>(identity);
    texture_frame_requester->SetActive(texture, false);
  }
  committed_scene.clear();
}

} // namespace huxerui::detail
