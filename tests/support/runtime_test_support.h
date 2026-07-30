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
using huxerui::Checkbox;
using huxerui::CheckboxStyle;
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
using huxerui::Text;
using huxerui::TextEditingAction;
using huxerui::TextEditingValue;
using huxerui::TextField;
using huxerui::TextFieldEvents;
using huxerui::TextFieldStyle;
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

template <huxerui::EnvironmentValue Value> Value ThemeDefinitionValue(const ThemeDefinition& definition) {
  EnvironmentValues values;
  huxerui::detail::ApplyThemeDefinition(values, definition);
  const std::any* stored = huxerui::detail::FindLocalEnvironmentValue(values, typeid(Value));
  const auto* typed = stored ? std::any_cast<Value>(stored) : nullptr;
  if (!typed) {
    throw std::logic_error("HuxerUI test theme definition does not contain the requested value");
  }
  return *typed;
}

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

  bool PerformTextInputAction(TextInputSessionId session_id, huxerui::TextInputAction action) {
    return runtime_.PerformTextInputAction(session_id, action);
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

  huxerui::TextInputPositionResult QueryTextInputPosition(TextInputSessionId session_id, Point point) const {
    return runtime_.QueryTextInputPosition(session_id, point);
  }

  huxerui::Runtime& NativeRuntime() noexcept {
    return runtime_;
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
    TextLayout(std::string_view text, float max_width) {
      lines_.push_back({});
      offsets_.push_back(0);
      lines_.back().boundaries.push_back({0, 0.0F});
      TextOffset offset = 0;
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
        const TextOffset code_units = code_point > 0xFFFFU ? 2 : 1;
        if (code_point == '\n') {
          offset += code_units;
          offsets_.push_back(offset);
          lines_.back().hard_break = true;
          lines_.push_back({});
          lines_.back().boundaries.push_back({offset, 0.0F});
          continue;
        }

        const bool combining = code_point >= 0x0300U && code_point <= 0x036FU;
        const float width = code_point > 0xFFFFU ? 20.0F : 10.0F;
        Line& line = lines_.back();
        const float position = line.boundaries.back().x;
        if (!combining && std::isfinite(max_width) && position > 0.0F && position + width > max_width) {
          lines_.push_back({});
          lines_.back().boundaries.push_back({offset, 0.0F});
        }
        offset += code_units;
        if (!combining) {
          offsets_.push_back(offset);
          Line& target = lines_.back();
          target.boundaries.push_back({offset, target.boundaries.back().x + width});
        } else if (offsets_.size() > 1) {
          offsets_.back() = offset;
          lines_.back().boundaries.back().offset = offset;
        }
      }
    }

    Size Measure() const override {
      float width = 0.0F;
      for (const Line& line : lines_) {
        width = std::max(width, line.boundaries.back().x);
      }
      return {width, static_cast<float>(lines_.size()) * 20.0F};
    }

    TextPosition HitTest(Point point) const override {
      const std::size_t line_index = std::min(
          lines_.size() - 1,
          static_cast<std::size_t>(std::max(0.0F, std::floor(point.y / 20.0F)))
      );
      const Line& line = lines_[line_index];
      for (std::size_t index = 1; index < line.boundaries.size(); ++index) {
        if (point.x < (line.boundaries[index - 1].x + line.boundaries[index].x) * 0.5F) {
          return {
              line.boundaries[index - 1].offset,
              huxerui::TextAffinity::Downstream,
          };
        }
      }
      return {
          line.boundaries.back().offset,
          line_index + 1 < lines_.size() && !line.hard_break &&
                  lines_[line_index + 1].boundaries.front().offset == line.boundaries.back().offset
              ? huxerui::TextAffinity::Upstream
              : huxerui::TextAffinity::Downstream,
      };
    }

    Rect CaretRect(TextOffset offset, huxerui::TextAffinity affinity) const override {
      const std::size_t line_index = LineIndex(offset, affinity);
      return {Position(lines_[line_index], offset), static_cast<float>(line_index) * 20.0F, 1.0F, 20.0F};
    }

    std::vector<Rect> RangeRects(huxerui::TextRange range) const override {
      if (range.IsCollapsed()) {
        return {};
      }
      std::vector<Rect> rects;
      for (std::size_t index = 0; index < lines_.size(); ++index) {
        const Line& line = lines_[index];
        const TextOffset line_start = line.boundaries.front().offset;
        const TextOffset line_end = line.boundaries.back().offset;
        const TextOffset start = std::max(range.start, line_start);
        const TextOffset end = std::min(range.end, line_end);
        if (start < end) {
          const float x = Position(line, start);
          rects.push_back({
              x,
              static_cast<float>(index) * 20.0F,
              Position(line, end) - x,
              20.0F,
          });
        }
      }
      return rects;
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
    struct Boundary {
      TextOffset offset = 0;
      float x = 0.0F;
    };

    struct Line {
      std::vector<Boundary> boundaries;
      bool hard_break = false;
    };

    std::size_t LineIndex(TextOffset offset, huxerui::TextAffinity affinity) const {
      for (std::size_t index = 0; index < lines_.size(); ++index) {
        const Line& line = lines_[index];
        const TextOffset start = line.boundaries.front().offset;
        const TextOffset end = line.boundaries.back().offset;
        if (offset < end || (offset == end && (affinity == huxerui::TextAffinity::Upstream ||
                                               index + 1 == lines_.size() || line.hard_break))) {
          return index;
        }
        if (offset < start) {
          return index;
        }
      }
      return lines_.size() - 1;
    }

    static float Position(const Line& line, TextOffset offset) {
      const auto found = std::lower_bound(
          line.boundaries.begin(),
          line.boundaries.end(),
          offset,
          [](const Boundary& boundary, TextOffset value) { return boundary.offset < value; }
      );
      return found == line.boundaries.end() ? line.boundaries.back().x : found->x;
    }

    std::vector<TextOffset> offsets_;
    std::vector<Line> lines_;
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
    return std::make_unique<TextLayout>(text, max_width);
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
