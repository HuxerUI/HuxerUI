#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

#include <huxerui/view.h>

namespace huxerui {

namespace detail {
class Runtime;
struct EnvironmentFrame;
struct LayerControllerState {
  Runtime *runtime = nullptr;
};
}

class DialogService;
class ToastService;

using LayerId = std::uint64_t;
using ViewFactory = std::function<View()>;

enum class LayerKind {
  Popup,
  Modal,
  Toast,
  System,
};

enum class LayerInputPolicy {
  PassThrough,
  Content,
  Modal,
};

struct LayerOptions {
  LayerKind kind = LayerKind::Popup;
  LayerInputPolicy input_policy = LayerInputPolicy::Content;
  bool dismiss_on_outside_press = false;
  std::function<void()> on_dismiss_request;
  std::optional<Color> modal_scrim;
};

class LayerController {
public:
  LayerController(const LayerController &) = default;
  LayerController &operator=(const LayerController &) = default;

  LayerId Attach(LayerOptions options, ViewFactory content) const;

  template <class Factory>
    requires std::invocable<Factory &> &&
             std::convertible_to<std::invoke_result_t<Factory &>, View>
  LayerId Attach(LayerKind kind, Factory &&content) const {
    LayerInputPolicy input_policy = LayerInputPolicy::Content;
    if (kind == LayerKind::Toast) {
      input_policy = LayerInputPolicy::PassThrough;
    } else if (kind == LayerKind::Modal) {
      input_policy = LayerInputPolicy::Modal;
    }
    return Attach(
        LayerOptions{
            .kind = kind,
            .input_policy = input_policy,
        },
        ViewFactory(std::forward<Factory>(content)));
  }

  bool Update(LayerId id, ViewFactory content) const;
  bool Update(
      LayerId id, LayerOptions options, ViewFactory content) const;
  bool Dismiss(LayerId id) const;

private:
  LayerId AttachCaptured(
      LayerOptions options, ViewFactory content,
      std::shared_ptr<const detail::EnvironmentFrame> environment) const;

  explicit LayerController(detail::Runtime &runtime)
      : state_(std::make_shared<detail::LayerControllerState>(
            detail::LayerControllerState{&runtime})) {}

  void Disconnect() noexcept {
    state_->runtime = nullptr;
  }

  std::shared_ptr<detail::LayerControllerState> state_;

  friend class detail::Runtime;
  friend class DialogService;
  friend class ToastService;
};

} // namespace huxerui
