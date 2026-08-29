#pragma once
#include <catch2/catch_amalgamated.hpp>

#include <huxerui/huxerui.h>

#include <cmath>
#include <functional>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "internal.h"
#include "system_tray_internal.h"
#include "text_layout_internal.h"

namespace huxerui::test {

class Runtime;

PlatformResources* BuiltinTestResources();

using huxerui::AnimateTo;
using huxerui::Axis;
using huxerui::BackEvent;
using huxerui::BackPhase;
using huxerui::BottomSheetContext;
using huxerui::BottomSheetHandle;
using huxerui::Button;
using huxerui::ButtonStyle;
using huxerui::IconButton;
using huxerui::IconButtonStyle;
using huxerui::Checkbox;
using huxerui::CheckboxStyle;
using huxerui::Color;
using huxerui::Column;
using huxerui::CrossAxisAlignment;
using huxerui::Dialog;
using huxerui::DialogContext;
using huxerui::DialogHandle;
using huxerui::DrawArcCommand;
using huxerui::DrawBorderCommand;
using huxerui::DrawRectCommand;
using huxerui::DrawTextCommand;
using huxerui::Easing;
using huxerui::Enabled;
using huxerui::Environment;
using huxerui::Event;
using huxerui::EventEmitter;
using huxerui::Focusable;
using huxerui::Font;
using huxerui::FontMetrics;
using huxerui::ForEach;
using huxerui::FrameCommit;
using huxerui::GridColumns;
using huxerui::HorizontalAlignment;
using huxerui::IndexedPages;
using huxerui::Key;
using huxerui::KeyEvent;
using huxerui::KeyEventType;
using huxerui::LayerCancelPolicy;
using huxerui::LayerController;
using huxerui::LayerId;
using huxerui::LayerLevel;
using huxerui::LayerOptions;
using huxerui::LayerPointerPolicy;
using huxerui::Layout;
using huxerui::LayoutContext;
using huxerui::LayoutResult;
using huxerui::MainAxisAlignment;
using huxerui::MenuEntry;
using huxerui::MenuHandle;
using huxerui::MenuItem;
using huxerui::MenuSection;
using huxerui::MountedNode;
using huxerui::NavigationController;
using huxerui::NavigationStack;
using huxerui::NodeExtension;
using huxerui::Offset;
using huxerui::Opacity;
using huxerui::PaintCommand;
using huxerui::PaintContext;
using huxerui::PaintSequence;
using huxerui::Point;
using huxerui::PointerEvent;
using huxerui::PointerEventType;
using huxerui::PopClipCommand;
using huxerui::PopTransformCommand;
using huxerui::PopupContext;
using huxerui::PopupHandle;
using huxerui::ProgressBar;
using huxerui::ProgressBarStyle;
using huxerui::ProgressCircle;
using huxerui::ProgressCircleStyle;
using huxerui::PushClipCommand;
using huxerui::PushTransformCommand;
using huxerui::RadioButton;
using huxerui::RadioButtonStyle;
using huxerui::Rect;
using huxerui::RenderFrame;
using huxerui::RenderNode;
using huxerui::Rotation;
using huxerui::Row;
using huxerui::Scale;
using huxerui::StrokePathCommand;
using huxerui::ScrollAlignment;
using huxerui::ScrollController;
using huxerui::ScrollEvent;
using huxerui::ScrollView;
using huxerui::Select;
using huxerui::SelectEvents;
using huxerui::SelectStyle;
using huxerui::SelectionArea;
using huxerui::SegmentedButton;
using huxerui::SegmentedButtonEvents;
using huxerui::SegmentedButtonItem;
using huxerui::SegmentedButtonStyle;
using huxerui::Size;
using huxerui::Slider;
using huxerui::SliderStyle;
using huxerui::Spacer;
using huxerui::Stack;
using huxerui::State;
using huxerui::StateList;
using huxerui::StringVariant;
using huxerui::StrokeCap;
using huxerui::Switch;
using huxerui::SwitchStyle;
using huxerui::TabItem;
using huxerui::Tabs;
using huxerui::TabsEvents;
using huxerui::TabsStyle;
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
using huxerui::TextLayoutMetrics;
using huxerui::TextLayoutOptions;
using huxerui::TextOffset;
using huxerui::TextRole;
using huxerui::TextRunMetrics;
using huxerui::TextShapingOptions;
using huxerui::TextStyle;
using huxerui::TextWrap;
using huxerui::Theme;
using huxerui::ThemeDefinition;
using huxerui::ThemeSpec;
using huxerui::ToastHandle;
using huxerui::ToggleEvents;
using huxerui::Transform2D;
using huxerui::TweenSpec;
using huxerui::UseBottomSheet;
using huxerui::UseDialog;
using huxerui::UseEnvironment;
using huxerui::UseEvents;
using huxerui::UseMenu;
using huxerui::UseNavigation;
using huxerui::UsePopup;
using huxerui::UseScrollController;
using huxerui::UseService;
using huxerui::UseState;
using huxerui::UseStateList;
using huxerui::UseTheme;
using huxerui::UseToast;
using huxerui::UseViewportClass;
using huxerui::UseWindow;
using huxerui::VerticalAlignment;
using huxerui::View;
using huxerui::ViewEvents;
using huxerui::ViewportBreakpoints;
using huxerui::ViewportClass;
using huxerui::VirtualGrid;
using huxerui::VirtualLayout;
using huxerui::VirtualLayoutContext;
using huxerui::VirtualLayoutResult;
using huxerui::VirtualList;
using huxerui::WindowChromeMode;
using huxerui::WindowCommand;
using huxerui::WindowHandle;
using huxerui::WindowTitleBar;

template <huxerui::EnvironmentValue Value> Value ThemeDefinitionValue(const ThemeDefinition& definition) {
  Environment environment;
  huxerui::detail::ApplyThemeDefinition(environment, definition);
  const std::any* stored = huxerui::detail::FindLocalEnvironmentValue(environment, typeid(Value));
  const auto* typed = stored ? std::any_cast<Value>(stored) : nullptr;
  if (!typed) {
    throw std::logic_error("HuxerUI test theme definition does not contain the requested value");
  }
  return *typed;
}

class FlattenedScene {
public:
  [[nodiscard]] const std::vector<PaintCommand>& Commands() const noexcept {
    return commands_;
  }

private:
  void Append(const PaintSequence& sequence) {
    commands_.insert(commands_.end(), sequence.Commands().begin(), sequence.Commands().end());
  }

  void Append(const RenderNode& node) {
    if (!node.visible) {
      return;
    }
    Transform2D transform = node.transform;
    transform.translate_x += node.offset.x;
    transform.translate_y += node.offset.y;
    const bool transformed = !transform.IsIdentity();
    if (transformed) {
      commands_.emplace_back(PushTransformCommand{transform});
    }
    Append(node.content);
    for (const RenderClip& clip : node.child_clips) {
      std::visit([this](const auto& command) { commands_.emplace_back(command); }, clip);
    }
    const bool children_transformed = !node.children_transform.IsIdentity();
    if (children_transformed) {
      commands_.emplace_back(PushTransformCommand{node.children_transform});
    }
    for (const RenderNode* child : node.children) {
      if (child != nullptr) {
        Append(*child);
      }
    }
    if (children_transformed) {
      commands_.emplace_back(PopTransformCommand{});
    }
    for (std::size_t index = 0; index < node.child_clips.size(); ++index) {
      commands_.emplace_back(PopClipCommand{});
    }
    Append(node.foreground);
    if (transformed) {
      commands_.emplace_back(PopTransformCommand{});
    }
  }

  void Update(const RenderFrame& frame) {
    commands_.clear();
    if (frame.scene.root != nullptr) {
      Append(*frame.scene.root);
    }
  }

  std::vector<PaintCommand> commands_;

  friend class Runtime;
};

class Runtime final {
public:
  Runtime(
      huxerui::RootFactory root_factory,
      huxerui::PlatformAdapter& platform,
      huxerui::AppOptions options = {.show_debug_overlay = false},
      huxerui::ApplicationActivation startup_activation = huxerui::LaunchActivation{}
  )
      : application_(root_factory, std::move(options)),
        runtime_(application_, platform, std::move(startup_activation)) {}

  void SetWindowMetrics(huxerui::WindowMetrics metrics) {
    runtime_.SetWindowMetrics(metrics);
  }

  bool IsWindowDragRegion(Point position) const {
    return runtime_.IsWindowDragRegion(position);
  }

  void UpdateResourceConfiguration(huxerui::ResourceConfiguration configuration) {
    runtime_.UpdateResourceConfiguration(std::move(configuration));
  }

  const FlattenedScene& BuildFrame() {
    flattened_scene_.Update(BuildCommit().render_frame);
    return flattened_scene_;
  }

  const RenderFrame& BuildRenderFrame() {
    return BuildCommit().render_frame;
  }

  const FrameCommit& BuildCommit() {
    last_commit_ = &runtime_.BuildFrame();
    return *last_commit_;
  }

  [[nodiscard]] const FrameCommit& LastCommit() const {
    if (last_commit_ == nullptr) {
      throw std::logic_error("HuxerUI test Runtime has not built a frame");
    }
    return *last_commit_;
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

  void HandleApplicationActivation(huxerui::ApplicationActivation activation) {
    runtime_.HandleApplicationActivation(std::move(activation));
  }

  void UpdateApplicationLifecycleState(huxerui::ApplicationLifecycleState lifecycle_state) {
    runtime_.UpdateApplicationLifecycleState(lifecycle_state);
  }

  bool HandleWindowRequest(huxerui::WindowCommand command) {
    return runtime_.HandleWindowRequest(command);
  }

  bool HandleBack() {
    return runtime_.HandleBack();
  }

  bool HandleBack(const BackEvent& event) {
    return runtime_.HandleBack(event);
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

  huxerui::Runtime& CoreRuntime() noexcept {
    return runtime_;
  }

  void InvalidateRoot() {
    huxerui::detail::RuntimeAccess::InvalidateRoot(runtime_);
  }

  const huxerui::detail::MountedNode* RootNode() const noexcept {
    return huxerui::detail::RuntimeAccess::RootNode(runtime_);
  }

private:
  huxerui::Application application_;
  huxerui::Runtime runtime_;
  FlattenedScene flattened_scene_;
  const FrameCommit* last_commit_ = nullptr;
};

class TestSystemTrayTransport : public huxerui::detail::SystemTrayTransport {
public:
  [[nodiscard]] bool IsAvailable() const noexcept override {
    return available;
  }

  void SetEventHandler(std::function<void(huxerui::detail::SystemTrayEvent)> handler) override {
    event_handler = std::move(handler);
    ++event_handler_updates;
  }

  void Show(const huxerui::detail::ResolvedSystemTrayPresentation& value) override {
    presentation = value;
    ++show_count;
  }

  void Hide() noexcept override {
    presentation.reset();
    ++hide_count;
  }

  void SetAvailable(bool value) {
    available = value;
    if (event_handler) {
      event_handler({
          .type = huxerui::detail::SystemTrayEventType::AvailabilityChanged,
          .available = value,
      });
    }
  }

  void Activate() {
    if (event_handler) {
      event_handler({.type = huxerui::detail::SystemTrayEventType::Activate});
    }
  }

  void Invoke(std::uint64_t generation, std::uint64_t command) {
    if (event_handler) {
      event_handler({
          .type = huxerui::detail::SystemTrayEventType::Command,
          .generation = generation,
          .command = command,
      });
    }
  }

  bool available = false;
  int show_count = 0;
  int hide_count = 0;
  int event_handler_updates = 0;
  std::optional<huxerui::detail::ResolvedSystemTrayPresentation> presentation;
  std::function<void(huxerui::detail::SystemTrayEvent)> event_handler;
};

class TestPlatform : public huxerui::PlatformAdapter {
public:
  TestPlatform()
      : PlatformAdapter([this](std::function<void()> task) { platform_module_tasks_.push_back(std::move(task)); }) {}

  explicit TestPlatform(huxerui::UIThreadDispatcher dispatch_to_ui_thread)
      : PlatformAdapter(std::move(dispatch_to_ui_thread)) {}

  void RunPlatformModuleTasks() {
    while (!platform_module_tasks_.empty()) {
      std::vector<std::function<void()>> tasks = std::move(platform_module_tasks_);
      platform_module_tasks_.clear();
      for (const auto& task : tasks) {
        task();
      }
    }
  }

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

  void RequestFrameAt(double deadline) override {
    ++requested_frames;
    requested_deadlines.push_back(deadline);
  }

  double Now() const noexcept override {
    return current_time;
  }

  void SetSystemBarsContentBrightness(
      huxerui::SystemBarContentBrightness status_bar, huxerui::SystemBarContentBrightness navigation_bar
  ) override {
    ++system_bars_updates;
    system_bar_brightness = std::pair{status_bar, navigation_bar};
  }

  void AdvanceTime(double seconds) {
    current_time += seconds;
  }

  FontMetrics Metrics(const Font& font) override {
    static_cast<void>(font);
    return {
        .ascent = 15.0F,
        .descent = 5.0F,
        .underline_position = 2.0F,
        .underline_thickness = 1.0F,
        .strike_through_position = 7.0F,
        .strike_through_thickness = 1.0F,
    };
  }

  TextRunMetrics MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options) override {
    static_cast<void>(options);
    const float width = static_cast<float>(text.size()) * 10.0F;
    const FontMetrics metrics = Metrics(style.font);
    const Rect bounds{0.0F, -metrics.ascent, width, metrics.LineHeight()};
    return {width, bounds, metrics};
  }

  TextLayoutMetrics MeasureText(
      std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
  ) override {
    static_cast<void>(style);
    if (max_width <= 0.0F) {
      return {};
    }

    std::vector<std::size_t> hard_lines{0};
    for (char character : text) {
      if (character == '\n') {
        hard_lines.push_back(0);
      } else {
        ++hard_lines.back();
      }
    }
    const auto widest = std::max_element(hard_lines.begin(), hard_lines.end());
    const float natural_width = static_cast<float>(*widest) * 10.0F;
    const bool wraps = std::isfinite(max_width) && options.wrap == TextWrap::Word;
    std::size_t line_count = hard_lines.size();
    if (wraps) {
      line_count = 0;
      for (std::size_t length : hard_lines) {
        const float width = static_cast<float>(length) * 10.0F;
        line_count += static_cast<std::size_t>(std::max(1.0F, std::ceil(width / max_width)));
      }
    }
    const float measured_width = std::isfinite(max_width) ? std::min(natural_width, max_width) : natural_width;
    const Size size{measured_width, static_cast<float>(line_count) * 20.0F};
    return {size, 15.0F, size.height - 5.0F, line_count};
  }

  std::unique_ptr<huxerui::detail::TextLayout> CreateTextLayout(
      std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
  ) override {
    static_cast<void>(style);
    return std::make_unique<TextLayout>(
        text,
        options.wrap == TextWrap::NoWrap ? std::numeric_limits<float>::infinity() : max_width
    );
  }

  huxerui::PlatformTextInput* TextInput() noexcept override {
    return platform_text_input;
  }

  huxerui::PlatformClipboard* Clipboard() noexcept override {
    return platform_clipboard;
  }

  huxerui::PlatformResources* Resources() noexcept override {
    return platform_resources;
  }

  std::optional<huxerui::ProcessMetrics> QueryProcessMetrics() noexcept override {
    return process_metrics;
  }

  void RequestWindowCommand(huxerui::WindowCommand command) override {
    window_commands.push_back(command);
  }

  void RequestApplicationQuit() override {
    ++application_quit_requests;
  }

  std::shared_ptr<huxerui::detail::SystemTrayTransport> CreateSystemTrayTransport() override {
    return system_tray_transport;
  }

  int requested_frames = 0;
  double current_time = 0.0;
  std::vector<double> requested_deadlines;
  int system_bars_updates = 0;
  std::optional<std::pair<huxerui::SystemBarContentBrightness, huxerui::SystemBarContentBrightness>>
      system_bar_brightness;
  std::optional<huxerui::ProcessMetrics> process_metrics;
  std::vector<huxerui::WindowCommand> window_commands;
  int application_quit_requests = 0;
  std::shared_ptr<TestSystemTrayTransport> system_tray_transport;
  huxerui::PlatformTextInput* platform_text_input = nullptr;
  huxerui::PlatformClipboard* platform_clipboard = nullptr;
  huxerui::PlatformResources* platform_resources = nullptr;

private:
  std::vector<std::function<void()>> platform_module_tasks_;
};

inline void SettlePresentation(TestPlatform& platform, Runtime& runtime, double duration = 0.5) {
  platform.AdvanceTime(duration);
  runtime.BuildFrame();
  // Exit completion invalidates the layer stack; the following commit removes the retained entry.
  runtime.BuildFrame();
}

inline std::string FirstText(const FlattenedScene& scene) {
  for (const auto& command : scene.Commands()) {
    if (const auto* text = std::get_if<DrawTextCommand>(&command)) {
      return text->text;
    }
  }
  return {};
}

inline bool ContainsText(const FlattenedScene& scene, std::string_view expected) {
  for (const auto& command : scene.Commands()) {
    const auto* text = std::get_if<DrawTextCommand>(&command);
    if (text && text->text == expected) {
      return true;
    }
  }
  return false;
}

inline const DrawTextCommand* FindText(const FlattenedScene& scene, std::string_view expected) {
  for (const auto& command : scene.Commands()) {
    const auto* text = std::get_if<DrawTextCommand>(&command);
    if (text && text->text == expected) {
      return text;
    }
  }
  return nullptr;
}

inline const DrawRectCommand* FindRect(const FlattenedScene& scene, Rect expected) {
  for (const auto& command : scene.Commands()) {
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    if (rect && rect->rect == expected) {
      return rect;
    }
  }
  return nullptr;
}

inline const DrawRectCommand* FindRectWithColor(const FlattenedScene& scene, Color expected) {
  for (const auto& command : scene.Commands()) {
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    if (rect && rect->color == expected) {
      return rect;
    }
  }
  return nullptr;
}

inline std::optional<Rect> FindPresentedRectWithColor(
    const FlattenedScene& scene,
    Color expected,
    std::optional<Size> expected_size = std::nullopt
) {
  std::vector<Transform2D> transform_stack{Transform2D{}};
  for (const auto& command : scene.Commands()) {
    if (const auto* transform = std::get_if<PushTransformCommand>(&command)) {
      transform_stack.push_back(detail::ComposeTransform(transform_stack.back(), transform->transform));
      continue;
    }
    if (std::holds_alternative<huxerui::PopTransformCommand>(command)) {
      transform_stack.pop_back();
      continue;
    }
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    if (rect && rect->color == expected &&
        (!expected_size.has_value() || Size{rect->rect.width, rect->rect.height} == *expected_size)) {
      return detail::TransformBounds(transform_stack.back(), rect->rect);
    }
    const auto* path = std::get_if<FillPathCommand>(&command);
    if (path && path->color == expected &&
        (!expected_size.has_value() || Size{path->path.Bounds().width, path->path.Bounds().height} == *expected_size)) {
      return detail::TransformBounds(transform_stack.back(), path->path.Bounds());
    }
  }
  return std::nullopt;
}

inline const DrawBorderCommand* FindBorderWithColor(const FlattenedScene& scene, Color expected) {
  for (const auto& command : scene.Commands()) {
    const auto* border = std::get_if<DrawBorderCommand>(&command);
    if (border && border->color == expected) {
      return border;
    }
  }
  return nullptr;
}

inline bool ContainsRect(const FlattenedScene& scene, Rect expected) {
  for (const auto& command : scene.Commands()) {
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    if (rect && rect->rect == expected) {
      return true;
    }
  }
  return false;
}

inline std::optional<float> RectAlpha(const FlattenedScene& scene, Rect expected) {
  for (const auto& command : scene.Commands()) {
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    if (rect && rect->rect == expected) {
      return rect->color.alpha;
    }
  }
  return std::nullopt;
}

inline std::optional<Rect> FindPresentedTextRect(const FlattenedScene& scene, std::string_view expected) {
  std::vector<Transform2D> transform_stack{Transform2D{}};
  for (const PaintCommand& command : scene.Commands()) {
    if (const auto* transform = std::get_if<PushTransformCommand>(&command)) {
      transform_stack.push_back(detail::ComposeTransform(transform_stack.back(), transform->transform));
      continue;
    }
    if (std::holds_alternative<PopTransformCommand>(command)) {
      transform_stack.pop_back();
      continue;
    }
    const auto* text = std::get_if<DrawTextCommand>(&command);
    if (text && text->text == expected) {
      return detail::TransformBounds(transform_stack.back(), text->rect);
    }
  }
  return std::nullopt;
}

inline std::optional<Rect> FindPresentedStrokePathRect(const FlattenedScene& scene, Color expected) {
  std::vector<Transform2D> transform_stack{Transform2D{}};
  for (const PaintCommand& command : scene.Commands()) {
    if (const auto* transform = std::get_if<PushTransformCommand>(&command)) {
      transform_stack.push_back(detail::ComposeTransform(transform_stack.back(), transform->transform));
      continue;
    }
    if (std::holds_alternative<PopTransformCommand>(command)) {
      transform_stack.pop_back();
      continue;
    }
    const auto* path = std::get_if<StrokePathCommand>(&command);
    if (path && path->color == expected) {
      return detail::TransformBounds(transform_stack.back(), path->path.Bounds());
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
