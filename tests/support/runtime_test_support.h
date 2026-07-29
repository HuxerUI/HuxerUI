#pragma once
#include <catch2/catch_amalgamated.hpp>

#include <huxerui/huxerui.h>

#include <cmath>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "internal.h"

namespace huxerui::test {

using huxerui::AnimateTo;
using huxerui::Axis;
using huxerui::Button;
using huxerui::ButtonStyle;
using huxerui::ButtonStyleKey;
using huxerui::Checkbox;
using huxerui::CheckboxStyle;
using huxerui::CheckboxStyleKey;
using huxerui::Color;
using huxerui::Column;
using huxerui::CrossAxisAlignment;
using huxerui::Dialog;
using huxerui::DialogContext;
using huxerui::DialogHandle;
using huxerui::DisplayList;
using huxerui::DrawArcCommand;
using huxerui::DrawBorderCommand;
using huxerui::DrawRectCommand;
using huxerui::DrawTextCommand;
using huxerui::Easing;
using huxerui::Enabled;
using huxerui::EnvironmentValues;
using huxerui::Event;
using huxerui::EventEmitter;
using huxerui::Focusable;
using huxerui::ForEach;
using huxerui::GridColumns;
using huxerui::HorizontalAlignment;
using huxerui::Key;
using huxerui::KeyEvent;
using huxerui::KeyEventType;
using huxerui::LayerController;
using huxerui::LayerId;
using huxerui::LayerKind;
using huxerui::Layout;
using huxerui::LayoutContext;
using huxerui::LayoutResult;
using huxerui::MainAxisAlignment;
using huxerui::MountedNode;
using huxerui::NodeExtension;
using huxerui::Offset;
using huxerui::Opacity;
using huxerui::Point;
using huxerui::PointerEvent;
using huxerui::PointerEventType;
using huxerui::PopClipCommand;
using huxerui::ProgressCircle;
using huxerui::ProgressCircleStyle;
using huxerui::ProgressCircleStyleKey;
using huxerui::PushClipCommand;
using huxerui::PushTransformCommand;
using huxerui::Rect;
using huxerui::Rotation;
using huxerui::Row;
using huxerui::Scale;
using huxerui::ScrollAlignment;
using huxerui::ScrollEvent;
using huxerui::ScrollState;
using huxerui::ScrollView;
using huxerui::Size;
using huxerui::Spacer;
using huxerui::Stack;
using huxerui::State;
using huxerui::StrokeCap;
using huxerui::Switch;
using huxerui::SwitchStyle;
using huxerui::SwitchStyleKey;
using huxerui::Text;
using huxerui::TextRole;
using huxerui::Theme;
using huxerui::ThemeDefinition;
using huxerui::ThemeSpec;
using huxerui::ToastHandle;
using huxerui::ToggleEvents;
using huxerui::TweenSpec;
using huxerui::UseDialog;
using huxerui::UseEnvironment;
using huxerui::UseEvents;
using huxerui::UseScrollState;
using huxerui::UseService;
using huxerui::UseState;
using huxerui::UseTheme;
using huxerui::UseToast;
using huxerui::VerticalAlignment;
using huxerui::View;
using huxerui::ViewEvents;
using huxerui::VirtualGrid;
using huxerui::VirtualLayout;
using huxerui::VirtualLayoutContext;
using huxerui::VirtualLayoutResult;
using huxerui::VirtualList;

class Runtime final {
public:
  Runtime(huxerui::RootFactory root_factory, huxerui::PlatformHost& platform, huxerui::AppOptions options = {})
      : runtime_(
            {
                .root_factory = root_factory,
                .options = std::move(options),
            },
            platform
        ) {}

  void SetViewport(Size viewport) {
    runtime_.SetViewport(viewport);
  }

  const DisplayList& BuildFrame() {
    return runtime_.BuildFrame();
  }

  void HandlePointerEvent(const PointerEvent& event) {
    runtime_.HandlePointerEvent(event);
  }

  void HandleScrollEvent(const ScrollEvent& event) {
    runtime_.HandleScrollEvent(event);
  }

  void HandleKeyEvent(const KeyEvent& event) {
    runtime_.HandleKeyEvent(event);
  }

  void InvalidateRoot() {
    huxerui::detail::RuntimeAccess::InvalidateRoot(runtime_);
  }

  const huxerui::detail::MountedNode* RootNode() const noexcept {
    return huxerui::detail::RuntimeAccess::RootNode(runtime_);
  }

private:
  huxerui::Runtime runtime_;
};

class TestPlatform final : public huxerui::PlatformHost {
public:
  void RequestFrame(double delay_seconds) override {
    ++requested_frames;
    requested_delays.push_back(delay_seconds);
  }

  double Now() const noexcept override {
    return current_time;
  }

  void AdvanceTime(double seconds) {
    current_time += seconds;
  }

  Size MeasureText(std::string_view text, float font_size, float max_width) override {
    static_cast<void>(font_size);
    const float natural_width = static_cast<float>(text.size()) * 10.0F;
    if (!std::isfinite(max_width)) {
      return {natural_width, 20.0F};
    }
    if (max_width <= 0.0F) {
      return {};
    }
    const float line_count = std::max(1.0F, std::ceil(natural_width / max_width));
    return {
        std::min(natural_width, max_width),
        line_count * 20.0F,
    };
  }

  int requested_frames = 0;
  double current_time = 0.0;
  std::vector<double> requested_delays;
};

inline std::string FirstText(const DisplayList& display_list) {
  for (const auto& command : display_list.Commands()) {
    if (const auto* text = std::get_if<DrawTextCommand>(&command)) {
      return text->text;
    }
  }
  return {};
}

inline bool ContainsText(const DisplayList& display_list, std::string_view expected) {
  for (const auto& command : display_list.Commands()) {
    const auto* text = std::get_if<DrawTextCommand>(&command);
    if (text && text->text == expected) {
      return true;
    }
  }
  return false;
}

inline const DrawTextCommand* FindText(const DisplayList& display_list, std::string_view expected) {
  for (const auto& command : display_list.Commands()) {
    const auto* text = std::get_if<DrawTextCommand>(&command);
    if (text && text->text == expected) {
      return text;
    }
  }
  return nullptr;
}

inline const DrawRectCommand* FindRect(const DisplayList& display_list, Rect expected) {
  for (const auto& command : display_list.Commands()) {
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    if (rect && rect->rect.x == expected.x && rect->rect.y == expected.y && rect->rect.width == expected.width &&
        rect->rect.height == expected.height) {
      return rect;
    }
  }
  return nullptr;
}

inline const DrawRectCommand* FindRectWithColor(const DisplayList& display_list, Color expected) {
  for (const auto& command : display_list.Commands()) {
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    if (rect && rect->color.red == expected.red && rect->color.green == expected.green &&
        rect->color.blue == expected.blue && rect->color.alpha == expected.alpha) {
      return rect;
    }
  }
  return nullptr;
}

inline const DrawBorderCommand* FindBorderWithColor(const DisplayList& display_list, Color expected) {
  for (const auto& command : display_list.Commands()) {
    const auto* border = std::get_if<DrawBorderCommand>(&command);
    if (border && border->color.red == expected.red && border->color.green == expected.green &&
        border->color.blue == expected.blue && border->color.alpha == expected.alpha) {
      return border;
    }
  }
  return nullptr;
}

inline bool ContainsRect(const DisplayList& display_list, Rect expected) {
  for (const auto& command : display_list.Commands()) {
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    if (rect && rect->rect.x == expected.x && rect->rect.y == expected.y && rect->rect.width == expected.width &&
        rect->rect.height == expected.height) {
      return true;
    }
  }
  return false;
}

inline std::optional<float> RectAlpha(const DisplayList& display_list, Rect expected) {
  for (const auto& command : display_list.Commands()) {
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    if (rect && rect->rect.x == expected.x && rect->rect.y == expected.y && rect->rect.width == expected.width &&
        rect->rect.height == expected.height) {
      return rect->color.alpha;
    }
  }
  return std::nullopt;
}
inline void InvokeClick(const huxerui::detail::MountedNode& node) {
  REQUIRE(huxerui::detail::EmitEvent<ViewEvents::Click>(node.event_bindings));
}

inline void ClickAt(Runtime& runtime, Point position, std::int64_t pointer_id = 0) {
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      pointer_id,
      position,
  });
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      pointer_id,
      position,
  });
}

} // namespace huxerui::test
