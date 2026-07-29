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
using huxerui::ScrollController;
using huxerui::ScrollEvent;
using huxerui::ScrollView;
using huxerui::SelectionArea;
using huxerui::Size;
using huxerui::Spacer;
using huxerui::Stack;
using huxerui::State;
using huxerui::StrokeCap;
using huxerui::Switch;
using huxerui::SwitchStyle;
using huxerui::SwitchStyleKey;
using huxerui::Text;
using huxerui::TextEditingAction;
using huxerui::TextEditingValue;
using huxerui::TextField;
using huxerui::TextFieldEvents;
using huxerui::TextFieldStyle;
using huxerui::TextFieldStyleKey;
using huxerui::TextInputApplyResult;
using huxerui::TextInputCommandBatch;
using huxerui::TextInputContext;
using huxerui::TextInputGeometry;
using huxerui::TextInputSessionId;
using huxerui::TextOffset;
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
using huxerui::UseScrollController;
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

  bool CanPerformTextEditingAction(huxerui::TextEditingAction action) const {
    return runtime_.CanPerformTextEditingAction(action);
  }

  bool PerformTextEditingAction(huxerui::TextEditingAction action) {
    return runtime_.PerformTextEditingAction(action);
  }

  TextInputApplyResult HandleTextInputCommands(const TextInputCommandBatch& batch) {
    return runtime_.HandleTextInputCommands(batch);
  }

  TextInputContext QueryTextInputContext(TextInputSessionId session_id, TextOffset start, TextOffset length) const {
    return runtime_.QueryTextInputContext(session_id, start, length);
  }

  TextInputGeometry QueryTextInputGeometry(TextInputSessionId session_id, huxerui::TextRange range) const {
    return runtime_.QueryTextInputGeometry(session_id, range);
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
  class TextLayout final : public huxerui::detail::TextLayout {
  public:
    explicit TextLayout(std::string_view text) {
      offsets_.push_back(0);
      positions_.push_back(0.0F);
      TextOffset offset = 0;
      float position = 0.0F;
      for (std::size_t index = 0; index < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[index]);
        std::uint32_t code_point = first;
        std::size_t length = 1;
        if ((first & 0xE0U) == 0xC0U) {
          code_point = first & 0x1FU;
          length = 2;
        } else if ((first & 0xF0U) == 0xE0U) {
          code_point = first & 0x0FU;
          length = 3;
        } else if ((first & 0xF8U) == 0xF0U) {
          code_point = first & 0x07U;
          length = 4;
        }
        for (std::size_t continuation = 1; continuation < length; ++continuation) {
          code_point = (code_point << 6U) | (static_cast<unsigned char>(text[index + continuation]) & 0x3FU);
        }
        index += length;
        offset += code_point > 0xFFFFU ? 2 : 1;
        const bool combining = code_point >= 0x0300U && code_point <= 0x036FU;
        if (!combining) {
          position += code_point > 0xFFFFU ? 20.0F : 10.0F;
        }
        if (combining && offsets_.size() > 1) {
          offsets_.back() = offset;
        } else {
          offsets_.push_back(offset);
          positions_.push_back(position);
        }
      }
    }

    Size Measure() const override {
      return {positions_.back(), 20.0F};
    }

    huxerui::detail::TextHit HitTest(Point point) const override {
      for (std::size_t index = 1; index < positions_.size(); ++index) {
        if (point.x < (positions_[index - 1] + positions_[index]) * 0.5F) {
          return {offsets_[index - 1], huxerui::TextAffinity::Downstream};
        }
      }
      return {offsets_.back(), huxerui::TextAffinity::Downstream};
    }

    Rect CaretRect(TextOffset offset, huxerui::TextAffinity affinity) const override {
      static_cast<void>(affinity);
      return {Position(offset), 0.0F, 1.0F, 20.0F};
    }

    std::vector<Rect> RangeRects(huxerui::TextRange range) const override {
      if (range.IsCollapsed()) {
        return {};
      }
      const float start = Position(range.start);
      const float end = Position(range.end);
      return {{start, 0.0F, end - start, 20.0F}};
    }

    TextOffset PreviousCaretOffset(TextOffset offset) const override {
      const auto found = std::lower_bound(offsets_.begin(), offsets_.end(), offset);
      return found == offsets_.begin() ? 0 : *std::prev(found);
    }

    TextOffset NextCaretOffset(TextOffset offset) const override {
      const auto found = std::upper_bound(offsets_.begin(), offsets_.end(), offset);
      return found == offsets_.end() ? offsets_.back() : *found;
    }

  private:
    float Position(TextOffset offset) const {
      const auto found = std::lower_bound(offsets_.begin(), offsets_.end(), offset);
      if (found == offsets_.end()) {
        return positions_.back();
      }
      return positions_[static_cast<std::size_t>(std::distance(offsets_.begin(), found))];
    }

    std::vector<TextOffset> offsets_;
    std::vector<float> positions_;
  };

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

  std::unique_ptr<huxerui::detail::TextLayout>
  CreateTextLayout(std::string_view text, float font_size, float max_width) override {
    static_cast<void>(font_size);
    static_cast<void>(max_width);
    return std::make_unique<TextLayout>(text);
  }

  huxerui::PlatformTextInput* TextInput() noexcept override {
    return platform_text_input;
  }

  huxerui::PlatformClipboard* Clipboard() noexcept override {
    return platform_clipboard;
  }

  int requested_frames = 0;
  double current_time = 0.0;
  std::vector<double> requested_delays;
  huxerui::PlatformTextInput* platform_text_input = nullptr;
  huxerui::PlatformClipboard* platform_clipboard = nullptr;
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
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          pointer_id,
          position,
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          pointer_id,
          position,
      }
  );
}

} // namespace huxerui::test
