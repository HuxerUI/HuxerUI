#include <huxerui/platform_view.h>

#include <cmath>
#include <ranges>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "geometry_internal.h"
#include "internal.h"

namespace huxerui {

void PaintContext::PlacePlatformView(PlacePlatformViewCommand command) {
  RequireOpen();
  const Rect bounds = command.Bounds();
  if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) || !std::isfinite(bounds.width) ||
      !std::isfinite(bounds.height) || bounds.width < 0.0F || bounds.height < 0.0F) {
    throw std::logic_error("HuxerUI PlatformView placement bounds are invalid");
  }
  sequence_.commands_.emplace_back(std::move(command));
  Include(bounds);
}

namespace detail {

std::shared_ptr<ViewSpec> MakePlatformViewSpec(std::string type, PlatformPayload properties) {
  if (type.empty()) {
    throw std::invalid_argument("HuxerUI PlatformView type must not be empty");
  }
  static_cast<void>(PlatformPayload(type));
  auto spec = std::make_shared<ViewSpec>(NodeKind::PlatformView);
  spec->platform_view = std::make_shared<PlatformViewDeclaration>(PlatformViewDeclaration{
      std::move(type),
      std::move(properties),
      {},
  });
  return spec;
}

void PlatformViewPaintAccess::Paint(const MountedNode& node, PaintContext& context) {
  if (!node.platform_view) {
    throw std::logic_error("HuxerUI mounted PlatformView has no declaration");
  }
  context.PlacePlatformView(PlacePlatformViewCommand{
      node.identity,
      node.platform_view->type,
      node.platform_view->properties,
      node.platform_view_properties_revision,
      node.ContentBounds(),
  });
}

namespace {

bool IsTranslation(const Transform2D& transform) noexcept {
  return transform.m11 == 1.0F && transform.m12 == 0.0F && transform.m21 == 0.0F && transform.m22 == 1.0F;
}

struct CompositionState {
  RenderComposition result;
  std::unordered_set<std::uint64_t> platform_view_identities;
  std::optional<std::uint64_t> preceding_platform_view;
  std::size_t raster_command_count = 0;
  std::size_t slice_first_command = 0;
  bool slice_has_commands = false;
};

struct TraversalState {
  Transform2D transform;
  std::optional<Rect> clip;
  float opacity = 1.0F;
  bool visible = true;
  bool path_clipped = false;
};

void FlushSlice(CompositionState& state, std::optional<std::uint64_t> following_platform_view) {
  if (!state.slice_has_commands) {
    return;
  }
  state.result.layers.emplace_back(RenderSlice{
      state.preceding_platform_view,
      following_platform_view,
      state.slice_first_command,
      state.raster_command_count - state.slice_first_command,
  });
  state.slice_first_command = state.raster_command_count;
  state.slice_has_commands = false;
}

void VisitSequence(const PaintSequence& sequence, const TraversalState& traversal, CompositionState& state) {
  for (const PaintCommand& paint_command : sequence.Commands()) {
    const auto* placement = std::get_if<PlacePlatformViewCommand>(&paint_command);
    if (placement == nullptr) {
      if (traversal.visible) {
        ++state.raster_command_count;
        state.slice_has_commands = true;
      }
      continue;
    }

    if (!state.platform_view_identities.insert(placement->Identity()).second) {
      throw std::logic_error("HuxerUI RenderScene contains a duplicate PlatformView identity");
    }
    if (!IsTranslation(traversal.transform)) {
      throw std::logic_error("HuxerUI PlatformView does not support transformed composition");
    }
    if (traversal.path_clipped) {
      throw std::logic_error("HuxerUI PlatformView does not support path-clipped composition");
    }
    if (traversal.opacity > 0.0F && traversal.opacity < 1.0F) {
      throw std::logic_error("HuxerUI PlatformView does not support group-opacity composition");
    }

    FlushSlice(state, placement->Identity());
    Rect world_bounds = TransformBounds(traversal.transform, placement->Bounds());
    const bool visible = traversal.visible && traversal.opacity > 0.0F &&
                         (!traversal.clip.has_value() || world_bounds.Intersects(*traversal.clip));
    state.result.layers.emplace_back(PlatformViewPlacement{
        placement,
        world_bounds,
        traversal.clip,
        visible,
    });
    state.preceding_platform_view = placement->Identity();
    state.slice_first_command = state.raster_command_count;
  }
}

void VisitNode(const RenderNode& node, const TraversalState& parent, CompositionState& state) {
  Transform2D local_transform = node.transform;
  local_transform.translate_x += node.offset.x;
  local_transform.translate_y += node.offset.y;

  TraversalState own = parent;
  own.transform = ComposeTransform(parent.transform, local_transform);
  own.opacity *= node.opacity;
  own.visible = parent.visible && node.visible && node.opacity > 0.0F;
  VisitSequence(node.content, own, state);

  TraversalState children = own;
  for (const RenderClip& clip : node.child_clips) {
    if (const auto* rectangle = std::get_if<PushClipCommand>(&clip)) {
      if (rectangle->corner_radius != 0.0F) {
        children.path_clipped = true;
      }
      const Rect world_clip = TransformBounds(own.transform, rectangle->rect);
      children.clip = children.clip.has_value() ? children.clip->Intersection(world_clip) : world_clip;
    } else {
      children.path_clipped = true;
    }
  }
  children.transform = ComposeTransform(own.transform, node.children_transform);
  for (const RenderNode* child : node.children) {
    if (child != nullptr) {
      VisitNode(*child, children, state);
    }
  }
  VisitSequence(node.foreground, own, state);
}

} // namespace

RenderComposition BuildRenderComposition(const RenderScene& scene) {
  CompositionState state;
  if (scene.root != nullptr) {
    VisitNode(*scene.root, TraversalState{}, state);
  }
  FlushSlice(state, std::nullopt);
  return std::move(state.result);
}

} // namespace detail

PlatformView::PlatformView(std::string type, PlatformPayload properties)
    : View(detail::MakePlatformViewSpec(std::move(type), std::move(properties))) {}

void View::AddPlatformEvent(detail::PlatformEventDescriptor descriptor) {
  if (descriptor.name.empty() || descriptor.dispatch == nullptr) {
    throw std::invalid_argument("HuxerUI PlatformView event name and decoder must not be empty");
  }
  static_cast<void>(PlatformPayload(descriptor.name));
  EnsureUniqueSpec();
  if (spec_->kind != detail::NodeKind::PlatformView || !spec_->platform_view) {
    throw std::logic_error("HuxerUI PlatformView event was applied to a different View kind");
  }
  const bool duplicate = std::ranges::any_of(spec_->platform_view->events, [&descriptor](const auto& existing) {
    return existing.key == descriptor.key || existing.name == descriptor.name;
  });
  if (duplicate) {
    throw std::invalid_argument("HuxerUI PlatformView event keys and names must be unique");
  }
  auto declaration = std::make_shared<detail::PlatformViewDeclaration>(*spec_->platform_view);
  declaration->events.push_back(std::move(descriptor));
  spec_->platform_view = std::move(declaration);
}

} // namespace huxerui
