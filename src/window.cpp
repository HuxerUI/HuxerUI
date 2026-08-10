#include <huxerui/window.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

#include <huxerui/modifier.h>

#include "internal.h"
#include "window_internal.h"

namespace huxerui {

namespace detail {

class WindowControlsLayout final : public huxerui::Layout<WindowControlsLayout> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, huxerui::MountedNode& node, Constraints constraints) {
    const WindowTitleBarMetrics* metrics = context.TitleBarMetrics();
    if (metrics == nullptr || metrics->right_inset <= 0.0F || metrics->height <= 0.0F) {
      LayoutResult result;
      for (huxerui::MountedNode& child : node.Children()) {
        static_cast<void>(context.Measure(child, {0.0F, 0.0F, 0.0F, 0.0F}));
        result.Place(child, {});
      }
      return result.SetSize(constraints.Constrain({constraints.max_width, constraints.max_height}));
    }
    if (node.ChildCount() != 3) {
      throw std::logic_error("HuxerUI window controls require minimize, maximize, and close children");
    }

    const float width = std::min(metrics->right_inset, constraints.max_width);
    const float height = std::min(metrics->height, constraints.max_height);
    const float button_width = width / 3.0F;
    const float left = std::max(0.0F, constraints.max_width - width);
    LayoutResult result;
    for (std::size_t index = 0; index < node.ChildCount(); ++index) {
      huxerui::MountedNode& child = node.ChildAt(index);
      static_cast<void>(context.Measure(child, {button_width, button_width, height, height}));
      result.Place(child, {left + button_width * static_cast<float>(index), 0.0F});
    }
    return result.SetSize(constraints.Constrain({constraints.max_width, constraints.max_height}));
  }
};

namespace {

const StringVariant& WindowCommandLabel(const WindowCaptionLabels& labels, WindowCommand command) noexcept {
  switch (command) {
  case WindowCommand::Minimize:
    return labels.minimize;
  case WindowCommand::Maximize:
  case WindowCommand::Restore:
  case WindowCommand::ToggleMaximize:
    return labels.toggle_maximize;
  case WindowCommand::Close:
    return labels.close;
  }
  return labels.toggle_maximize;
}

View WindowControl(
    WindowCommand command,
    const std::shared_ptr<WindowService>& service,
    const std::shared_ptr<WindowState>& window,
    const WindowCaptionLabels& labels
) {
  View glyph = Canvas([command, window](PaintContext& context, Size size) {
    if (size.width <= 0.0F || size.height <= 0.0F) {
      return;
    }
    const Point center{size.width * 0.5F, size.height * 0.5F};
    Path path;
    switch (command) {
    case WindowCommand::Minimize:
      path.MoveTo({center.x - 5.0F, center.y + 3.0F}).LineTo({center.x + 5.0F, center.y + 3.0F});
      break;
    case WindowCommand::Maximize:
    case WindowCommand::Restore:
    case WindowCommand::ToggleMaximize: {
      const bool maximized = window->metrics.title_bar.has_value() && window->metrics.title_bar->maximized;
      if (command == WindowCommand::Restore || (command == WindowCommand::ToggleMaximize && maximized)) {
        path.MoveTo({center.x - 3.0F, center.y - 5.0F})
            .LineTo({center.x + 5.0F, center.y - 5.0F})
            .LineTo({center.x + 5.0F, center.y + 3.0F})
            .MoveTo({center.x - 5.0F, center.y - 3.0F})
            .LineTo({center.x + 3.0F, center.y - 3.0F})
            .LineTo({center.x + 3.0F, center.y + 5.0F})
            .LineTo({center.x - 5.0F, center.y + 5.0F})
            .Close();
      } else {
        path.MoveTo({center.x - 5.0F, center.y - 5.0F})
            .LineTo({center.x + 5.0F, center.y - 5.0F})
            .LineTo({center.x + 5.0F, center.y + 5.0F})
            .LineTo({center.x - 5.0F, center.y + 5.0F})
            .Close();
      }
      break;
    }
    case WindowCommand::Close:
      path.MoveTo({center.x - 5.0F, center.y - 5.0F})
          .LineTo({center.x + 5.0F, center.y + 5.0F})
          .MoveTo({center.x + 5.0F, center.y - 5.0F})
          .LineTo({center.x - 5.0F, center.y + 5.0F});
      break;
    }
    context.StrokePath(std::move(path), window->caption_foreground, 1.0F);
  });
  const bool close = command == WindowCommand::Close;
  View interaction_surface = Stack {}.With(
      Indication{StateOverlayIndication{
          .color = close ? Color::Rgb(196, 43, 28, 0.9F) : Color::Rgb(0, 0, 0, 0.16F),
          .hover_color = close ? Color::Rgb(196, 43, 28, 0.9F) : Color::Rgb(0, 0, 0, 0.08F),
      }},
      Semantics{.role = SemanticRole::Button, .label = WindowCommandLabel(labels, command)}
  );
  interaction_surface = std::move(interaction_surface).OnClick([service, command] { service->Request(command); });
  return Stack {
    std::move(interaction_surface),
    std::move(glyph),
  }.With(Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch));
}

} // namespace

View MakeWindowControls(
    const std::shared_ptr<WindowService>& service,
    const std::shared_ptr<WindowState>& window,
    std::shared_ptr<const Environment> environment,
    bool visible
) {
  Composer composer(nullptr, std::move(environment));
  Composer::Guard guard(composer);
  return WindowControlsLayout {
    WindowControl(WindowCommand::Minimize, service, window, window->caption_labels),
    WindowControl(WindowCommand::ToggleMaximize, service, window, window->caption_labels),
    WindowControl(WindowCommand::Close, service, window, window->caption_labels),
  }.With(Enabled(visible), Semantics{.hidden = !visible});
}

bool IsWindowControlsNode(const MountedNode& node) noexcept {
  return node.layout_descriptor != nullptr && node.layout_descriptor->type == typeid(WindowControlsLayout);
}

bool IsValidSystemBarsAppearance(const SystemBarsAppearance& appearance) noexcept {
  const auto finite = [](const Color& color) {
    return std::isfinite(color.red) && std::isfinite(color.green) && std::isfinite(color.blue) &&
           std::isfinite(color.alpha);
  };
  const auto valid_brightness = [](SystemBarContentBrightness brightness) {
    return brightness == SystemBarContentBrightness::Automatic || brightness == SystemBarContentBrightness::Light ||
           brightness == SystemBarContentBrightness::Dark;
  };
  return finite(appearance.status_bar_background) && finite(appearance.navigation_bar_background) &&
         valid_brightness(appearance.status_bar_content) && valid_brightness(appearance.navigation_bar_content);
}

WindowService::WindowService(PlatformAdapter& platform) : platform_(&platform) {}

void WindowService::Request(WindowCommand command) const {
  if (platform_ != nullptr) {
    platform_->RequestWindowCommand(command);
  }
}

void WindowService::Disconnect() noexcept {
  platform_ = nullptr;
}

} // namespace detail

namespace {

void ApplySystemBarsAppearance(detail::ViewSpec& spec, const SystemBarsAppearance& appearance) {
  if (!detail::IsValidSystemBarsAppearance(appearance)) {
    throw std::invalid_argument("HuxerUI system bars appearance is invalid");
  }
  spec.properties.system_bars_appearance = appearance;
}

void ApplySafeAreaPadding(detail::ViewSpec& spec, const SafeAreaPadding& padding) {
  spec.properties.safe_area_padding = padding;
}

void ApplyWindowDragRegion(detail::ViewSpec& spec, const WindowDragRegion&) {
  spec.properties.window_drag_region = true;
}

template <class Modifier, void (*Apply)(detail::ViewSpec&, const Modifier&)>
const detail::ModifierDescriptor& ApplyOnlyModifierDescriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec, const void* value) { Apply(spec, *static_cast<const Modifier*>(value)); },
      nullptr,
      nullptr,
      false,
      detail::ErasedEqualsFor<Modifier>(),
      nullptr,
  };
  return descriptor;
}

} // namespace

SystemBarsAppearance SystemBarsAppearance::Default() {
  return {};
}

const detail::ModifierDescriptor& SystemBarsAppearance::Descriptor() {
  return ApplyOnlyModifierDescriptor<SystemBarsAppearance, ApplySystemBarsAppearance>();
}

const detail::ModifierDescriptor& SafeAreaPadding::Descriptor() {
  return ApplyOnlyModifierDescriptor<SafeAreaPadding, ApplySafeAreaPadding>();
}

const detail::ModifierDescriptor& WindowDragRegion::Descriptor() {
  return ApplyOnlyModifierDescriptor<WindowDragRegion, ApplyWindowDragRegion>();
}

void WindowHandle::Minimize() const {
  service_->Request(WindowCommand::Minimize);
}

void WindowHandle::Maximize() const {
  service_->Request(WindowCommand::Maximize);
}

void WindowHandle::Restore() const {
  service_->Request(WindowCommand::Restore);
}

void WindowHandle::ToggleMaximize() const {
  service_->Request(WindowCommand::ToggleMaximize);
}

void WindowHandle::Close() const {
  service_->Request(WindowCommand::Close);
}

WindowHandle UseWindow() {
  return WindowHandle{UseService<detail::WindowService>()};
}

} // namespace huxerui
