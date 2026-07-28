#include <huxerui/layer.h>

#include <stdexcept>

#include "internal.h"

namespace huxerui {

LayerController::LayerController(detail::Runtime &runtime)
    : state_(std::make_shared<detail::LayerControllerState>(
          detail::LayerControllerState{&runtime})) {}

void LayerController::Disconnect() noexcept {
  state_->runtime = nullptr;
}

LayerId LayerController::Attach(
    LayerOptions options, ViewFactory content) const {
  return AttachCaptured(
      options, std::move(content),
      detail::CurrentEnvironmentFrame());
}

LayerId LayerController::AttachCaptured(
    LayerOptions options, ViewFactory content,
    std::shared_ptr<const detail::EnvironmentFrame> environment) const {
  if (state_->runtime == nullptr) {
    throw std::logic_error(
        "HuxerUI layer controller is disconnected");
  }
  return state_->runtime->AttachLayer(
      options, std::move(content), std::move(environment));
}

bool LayerController::Update(
    LayerId id, ViewFactory content) const {
  return state_->runtime != nullptr &&
         state_->runtime->UpdateLayer(id, std::move(content));
}

bool LayerController::Update(
    LayerId id, LayerOptions options,
    ViewFactory content) const {
  return state_->runtime != nullptr &&
         state_->runtime->UpdateLayer(
             id, std::move(options), std::move(content));
}

bool LayerController::Dismiss(LayerId id) const {
  return state_->runtime != nullptr &&
         state_->runtime->DismissLayer(id);
}

} // namespace huxerui
