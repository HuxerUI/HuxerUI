#include <huxerui/layer.h>

#include <algorithm>
#include <stdexcept>

#include "internal.h"

namespace huxerui {

namespace {

void ValidateLayerOptions(const LayerOptions& options) {
  if (options.dismiss_on_outside_press && options.pointer_policy != LayerPointerPolicy::Barrier) {
    throw std::invalid_argument("HuxerUI outside-press layer dismissal requires a barrier pointer policy");
  }
  if (options.barrier_color.has_value() && options.pointer_policy != LayerPointerPolicy::Barrier) {
    throw std::invalid_argument("HuxerUI layer barrier color requires a barrier pointer policy");
  }
}

} // namespace

LayerController::LayerController(Runtime& runtime)
    : state_(
          std::make_shared<State>(State{
              .runtime = &runtime,
              .entries = {},
              .next_id = 1,
              .next_sequence = 1,
          })
      ) {}

void LayerController::Disconnect() noexcept {
  state_->runtime = nullptr;
  state_->entries.clear();
}

LayerId LayerController::Attach(LayerOptions options, ViewFactory content) const {
  return AttachCaptured(std::move(options), std::move(content), detail::CurrentEnvironment(), detail::LayerPlacement{});
}

LayerId LayerController::AttachCaptured(
    LayerOptions options,
    ViewFactory content,
    std::shared_ptr<const Environment> environment,
    detail::LayerPlacement placement,
    std::shared_ptr<detail::LayerTransitionState> transition
) const {
  if (state_->runtime == nullptr) {
    throw std::logic_error("HuxerUI layer controller is disconnected");
  }
  if (!content) {
    throw std::invalid_argument("HuxerUI layer content factory must not be empty");
  }
  ValidateLayerOptions(options);
  const LayerId id = state_->next_id++;
  state_->entries.push_back(
      detail::LayerEntry{
          .id = id,
          .sequence = state_->next_sequence++,
          .revision = 1,
          .options = std::move(options),
          .content = std::move(content),
          .environment = std::move(environment),
          .placement = std::make_shared<detail::LayerPlacement>(std::move(placement)),
          .transition = std::move(transition),
      }
  );
  BindTransitionCompletion(id, state_->entries.back().transition);
  state_->runtime->InvalidateLayers();
  return id;
}

bool LayerController::UpdatePlacement(LayerId id, detail::LayerPlacement placement) const {
  if (state_->runtime == nullptr) {
    return false;
  }
  const auto found = std::ranges::find(state_->entries, id, &detail::LayerEntry::id);
  if (found == state_->entries.end()) {
    return false;
  }
  if (*found->placement == placement) {
    return true;
  }
  *found->placement = std::move(placement);
  state_->runtime->InvalidateLayerPlacement(id);
  return true;
}

std::optional<LayerOptions> LayerController::EntryOptions(LayerId id) const {
  if (state_->runtime == nullptr) {
    return std::nullopt;
  }
  const auto found = std::ranges::find(state_->entries, id, &detail::LayerEntry::id);
  return found == state_->entries.end() ? std::nullopt : std::optional<LayerOptions>{found->options};
}

std::shared_ptr<detail::LayerTransitionState> LayerController::Transition(LayerId id) const {
  if (state_->runtime == nullptr) {
    return {};
  }
  const auto found = std::ranges::find(state_->entries, id, &detail::LayerEntry::id);
  return found == state_->entries.end() ? nullptr : found->transition;
}

void LayerController::BindTransitionCompletion(
    LayerId id, const std::shared_ptr<detail::LayerTransitionState>& transition
) const {
  if (!transition) {
    return;
  }
  transition->on_exit_complete = [state = std::weak_ptr<State>(state_),
                                  transition = std::weak_ptr<detail::LayerTransitionState>(transition),
                                  id] {
    const std::shared_ptr<State> locked = state.lock();
    const std::shared_ptr<detail::LayerTransitionState> completed = transition.lock();
    if (!locked || locked->runtime == nullptr || !completed) {
      return;
    }
    const auto found = std::ranges::find(locked->entries, id, &detail::LayerEntry::id);
    if (found == locked->entries.end() || found->transition != completed) {
      return;
    }
    locked->entries.erase(found);
    locked->runtime->InvalidateLayers();
  };
}

bool LayerController::UpdateTransition(LayerId id, std::shared_ptr<detail::LayerTransitionState> transition) const {
  if (state_->runtime == nullptr) {
    return false;
  }
  const auto found = std::ranges::find(state_->entries, id, &detail::LayerEntry::id);
  if (found == state_->entries.end()) {
    return false;
  }
  if (found->transition == transition) {
    return true;
  }
  found->transition = std::move(transition);
  BindTransitionCompletion(id, found->transition);
  ++found->revision;
  state_->runtime->InvalidateLayers();
  return true;
}

bool LayerController::Update(LayerId id, ViewFactory content) const {
  return UpdateEntry(id, std::nullopt, std::move(content), std::nullopt);
}

bool LayerController::Update(LayerId id, LayerOptions options, ViewFactory content) const {
  return UpdateEntry(id, std::move(options), std::move(content), std::nullopt);
}

bool LayerController::UpdateCaptured(
    LayerId id, LayerOptions options, ViewFactory content, std::shared_ptr<const Environment> environment
) const {
  return UpdateEntry(id, std::move(options), std::move(content), std::move(environment));
}

bool LayerController::UpdateEntry(
    LayerId id,
    std::optional<LayerOptions> options,
    ViewFactory content,
    std::optional<std::shared_ptr<const Environment>> environment
) const {
  if (state_->runtime == nullptr) {
    return false;
  }
  if (!content) {
    throw std::invalid_argument("HuxerUI layer content factory must not be empty");
  }
  if (options.has_value()) {
    ValidateLayerOptions(*options);
  }
  const auto found = std::ranges::find(state_->entries, id, &detail::LayerEntry::id);
  if (found == state_->entries.end()) {
    return false;
  }
  if (options.has_value()) {
    found->options = std::move(*options);
  }
  found->content = std::move(content);
  if (environment.has_value()) {
    found->environment = std::move(*environment);
  }
  if (found->transition && !found->transition->target_visible) {
    found->transition->target_visible = true;
  }
  ++found->revision;
  state_->runtime->InvalidateLayers();
  return true;
}

bool LayerController::Dismiss(LayerId id) const {
  if (state_->runtime == nullptr) {
    return false;
  }
  const auto found = std::ranges::find(state_->entries, id, &detail::LayerEntry::id);
  if (found == state_->entries.end()) {
    return false;
  }
  if (found->transition) {
    if (!found->transition->target_visible) {
      return false;
    }
    found->transition->target_visible = false;
    ++found->revision;
    state_->runtime->InvalidateLayers();
    state_->runtime->DeactivateLayerInput(id);
    return true;
  }
  state_->entries.erase(found);
  state_->runtime->InvalidateLayers();
  state_->runtime->DeactivateLayerInput(id);
  return true;
}

} // namespace huxerui
