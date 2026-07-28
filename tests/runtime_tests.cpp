#include <huxerui/huxerui.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "internal.h"

namespace {

using huxerui::Axis;
using huxerui::AnimateTo;
using huxerui::Button;
using huxerui::ButtonStyle;
using huxerui::ButtonStyleKey;
using huxerui::Checkbox;
using huxerui::CheckboxStyle;
using huxerui::CheckboxStyleKey;
using huxerui::Color;
using huxerui::Column;
using huxerui::CrossAxisAlignment;
using huxerui::DisplayList;
using huxerui::Dialog;
using huxerui::DialogContext;
using huxerui::DialogHandle;
using huxerui::DrawArcCommand;
using huxerui::DrawBorderCommand;
using huxerui::DrawRectCommand;
using huxerui::DrawTextCommand;
using huxerui::Event;
using huxerui::EventEmitter;
using huxerui::Easing;
using huxerui::EnvironmentValues;
using huxerui::Enabled;
using huxerui::ForEach;
using huxerui::Focusable;
using huxerui::GridColumns;
using huxerui::HorizontalAlignment;
using huxerui::Layout;
using huxerui::LayoutContext;
using huxerui::LayoutResult;
using huxerui::LayerController;
using huxerui::LayerId;
using huxerui::LayerKind;
using huxerui::Key;
using huxerui::KeyEvent;
using huxerui::KeyEventType;
using huxerui::MainAxisAlignment;
using huxerui::MountedModifier;
using huxerui::MountedNode;
using huxerui::Offset;
using huxerui::Opacity;
using huxerui::PointerEvent;
using huxerui::PointerEventType;
using huxerui::Point;
using huxerui::PopClipCommand;
using huxerui::ProgressCircle;
using huxerui::ProgressCircleStyle;
using huxerui::ProgressCircleStyleKey;
using huxerui::PushClipCommand;
using huxerui::Rect;
using huxerui::Row;
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
using huxerui::ToggleEvents;
using huxerui::ToastHandle;
using huxerui::TweenSpec;
using huxerui::UseEvents;
using huxerui::UseEnvironment;
using huxerui::UseScrollState;
using huxerui::UseService;
using huxerui::UseState;
using huxerui::UseTheme;
using huxerui::UseToast;
using huxerui::UseDialogs;
using huxerui::VerticalAlignment;
using huxerui::View;
using huxerui::ViewEvents;
using huxerui::VirtualGrid;
using huxerui::VirtualLayout;
using huxerui::VirtualLayoutContext;
using huxerui::VirtualLayoutResult;
using huxerui::VirtualList;
using huxerui::detail::PlatformHost;
using huxerui::detail::Runtime;

class TestPlatform final : public PlatformHost, public huxerui::TextService {
public:
  int Run(Runtime &runtime, const huxerui::AppOptions &options) override {
    static_cast<void>(runtime);
    static_cast<void>(options);
    return 0;
  }

  void RequestFrame(double delay_seconds) override {
    ++requested_frames;
    requested_delays.push_back(delay_seconds);
  }

  double Now() const noexcept override { return current_time; }

  void AdvanceTime(double seconds) { current_time += seconds; }

  huxerui::TextService &Text() override { return *this; }

  Size MeasureText(std::string_view text, float font_size,
                   float max_width) override {
    static_cast<void>(font_size);
    const float natural_width = static_cast<float>(text.size()) * 10.0F;
    if (!std::isfinite(max_width)) {
      return {natural_width, 20.0F};
    }
    if (max_width <= 0.0F) {
      return {};
    }
    const float line_count =
        std::max(1.0F, std::ceil(natural_width / max_width));
    return {
        std::min(natural_width, max_width),
        line_count * 20.0F,
    };
  }

  int requested_frames = 0;
  double current_time = 0.0;
  std::vector<double> requested_delays;
};

struct TestEnvironmentKey {
  using Value = std::string;

  static Value Default() {
    return "fallback";
  }
};

std::vector<std::string> observed_environment_values;
State<bool> alternate_theme;
Color observed_theme_color;
Color observed_nested_theme_color;

struct TestRootService {
  LayerController *layers = nullptr;
  int value = 0;
};

std::shared_ptr<TestRootService> installed_root_service;
int observed_root_service_value = 0;
int root_app_clicks = 0;
std::optional<ToastHandle> saved_toast;
std::optional<DialogHandle> saved_dialogs;
std::optional<DialogContext> saved_dialog_context;
State<bool> declarative_dialog_visible;
State<bool> animation_target;
int indication_clicks = 0;
State<bool> show_modifier_branch;
State<bool> first_focus_enabled;
std::vector<std::string> focus_changes;
std::vector<Key> received_keys;
int first_keyboard_clicks = 0;
int third_keyboard_clicks = 0;
int custom_keyboard_clicks = 0;
int disabled_clicks = 0;
int underlying_clicks = 0;
int background_dialog_clicks = 0;
int first_dialog_clicks = 0;
int second_dialog_clicks = 0;
State<bool> checkbox_checked;
State<bool> switch_checked;
int checkbox_changes = 0;
int switch_changes = 0;
State<float> progress_circle_value;

View EnvironmentReader() {
  HUXERUI_SCOPE({
    observed_environment_values.push_back(
        UseEnvironment<TestEnvironmentKey>());
    return Text(UseEnvironment<TestEnvironmentKey>());
  });
}

View EnvironmentApp() {
  EnvironmentValues outer;
  outer.Set<TestEnvironmentKey>("outer");
  return huxerui::ProvideEnvironment(
      std::move(outer), [] {
        EnvironmentValues inner;
        inner.Set<TestEnvironmentKey>("inner");
        return Column{
            EnvironmentReader(),
            huxerui::ProvideEnvironment(
                std::move(inner), EnvironmentReader),
        };
      });
}

View NestedThemeReader();
View TestButtonTheme(std::function<View()> content);

View ThemedReader() {
  HUXERUI_SCOPE({
    observed_theme_color = UseTheme().colors.primary;
    return Column{
        Text("theme text"),
        Text("theme title", TextRole::Title),
        Text("theme label", TextRole::Label),
        Button("theme button"),
        Text("explicit text").With(
            huxerui::Foreground{Color::Rgb(255, 140, 0)},
            huxerui::FontSize{29.0F}),
        HUXERUI_THEME(
            TestButtonTheme,
            NestedThemeReader()),
    };
  });
}

View NestedThemeReader() {
  HUXERUI_SCOPE({
    observed_nested_theme_color =
        UseTheme().colors.primary;
    return Button("nested button");
  });
}

View TestButtonTheme(std::function<View()> content) {
  ThemeDefinition definition;
  definition.Set<ButtonStyleKey>(ButtonStyle{
      .background = Color::Rgb(130, 80, 210),
      .foreground = Color::White(),
      .font_size = 21.0F,
      .padding = huxerui::EdgeInsets::All(11.0F),
      .corner_radius = 13.0F,
  });
  return Theme(std::move(definition), std::move(content));
}

View TestThemeProvider(std::function<View()> content) {
  ThemeSpec spec;
  spec.colors.primary =
      alternate_theme
          ? Color::Rgb(220, 70, 50)
          : Color::Rgb(40, 100, 220);
  spec.colors.on_surface = Color::Rgb(30, 90, 55);
  spec.typography.body = 18.0F;
  spec.typography.label = 16.0F;
  spec.typography.title = 25.0F;
  return Theme(
      ThemeDefinition{spec}, std::move(content));
}

View ThemeApp() {
  alternate_theme = UseState(false);
  return HUXERUI_THEME(TestThemeProvider, ThemedReader());
}

View FlatDarkThemeApp() {
  return HUXERUI_THEME(
      TestButtonTheme,
      HUXERUI_THEME(
          huxerui::FlatDarkTheme,
          Column{
              Text("dark body"),
              Text("dark title", TextRole::Title),
              Button("dark button"),
          }));
}

View FlatThemeInteractionApp() {
  return HUXERUI_THEME(
      huxerui::FlatTheme,
      Button("flat interaction").OnClick([] {}));
}

View MaterialThemeApp() {
  return HUXERUI_THEME(
      huxerui::MaterialTheme,
      Button("material button").OnClick([] {}));
}

View MaterialDarkThemeApp() {
  return HUXERUI_THEME(
      huxerui::MaterialDarkTheme,
      Button("material dark button"));
}

View ToggleApp() {
  auto checkbox = UseState(false);
  auto switch_value = UseState(false);
  checkbox_checked = checkbox;
  switch_checked = switch_value;
  return Row{
      Checkbox(checkbox).OnChanged([checkbox](bool checked) {
        ++checkbox_changes;
        checkbox = checked;
      }),
      Switch(switch_value).On<ToggleEvents::Changed>(
          [switch_value](bool checked) {
            ++switch_changes;
            switch_value = checked;
          }),
  }.With(huxerui::Spacing{8.0F});
}

View DeterminateProgressCircleApp() {
  auto progress = UseState(0.25F);
  progress_circle_value = progress;
  return Row{
      ProgressCircle(progress),
  };
}

View IndeterminateProgressCircleApp() {
  return ProgressCircle();
}

View EmptyProgressCircleApp() {
  return ProgressCircle(-1.0F);
}

View FullProgressCircleApp() {
  return ProgressCircle(2.0F);
}

template <class Factory>
View ReducedMotionProgressTheme(Factory &&content) {
  ThemeSpec spec = huxerui::FlatLightThemeSpec();
  spec.motion.reduced_motion = true;
  return Theme(
      ThemeDefinition{spec},
      std::forward<Factory>(content));
}

View ReducedMotionProgressCircleApp() {
  return HUXERUI_THEME(
      ReducedMotionProgressTheme,
      ProgressCircle());
}

template <class Factory>
View InteractionTestTheme(Factory &&content) {
  ThemeSpec spec = huxerui::FlatLightThemeSpec();
  spec.motion.reduced_motion = true;
  spec.interactions.hover_overlay =
      Color::Rgb(20, 80, 160, 0.2F);
  spec.interactions.pressed_overlay =
      Color::Rgb(200, 40, 60, 0.3F);
  return Theme(
      ThemeDefinition{spec},
      std::forward<Factory>(content));
}

View ThemedIndicationApp() {
  return HUXERUI_THEME(
      InteractionTestTheme,
      Button("themed indication").OnClick([] {}));
}

template <class Factory>
View FocusTestTheme(Factory &&content) {
  ThemeSpec spec = huxerui::FlatLightThemeSpec();
  spec.motion.reduced_motion = true;
  spec.interactions.focus_ring =
      Color::Rgb(40, 180, 90);
  spec.interactions.focus_ring_width = 3.0F;
  spec.interactions.disabled_opacity = 0.3F;
  return Theme(
      ThemeDefinition{spec},
      std::forward<Factory>(content));
}

View FocusContent() {
  HUXERUI_SCOPE({
    first_focus_enabled = UseState(true);
    return Column{
        Button("first")
            .With(Enabled{first_focus_enabled})
            .OnClick([] { ++first_keyboard_clicks; })
            .On<ViewEvents::FocusChanged>([](bool focused) {
              focus_changes.push_back(
                  focused ? "first:on" : "first:off");
            }),
        Button("disabled")
            .With(Enabled{false})
            .OnClick([] { ++disabled_clicks; }),
        Button("third")
            .OnClick([] { ++third_keyboard_clicks; })
            .On<ViewEvents::FocusChanged>([](bool focused) {
              focus_changes.push_back(
                  focused ? "third:on" : "third:off");
            }),
        Text("custom focus")
            .With(Focusable{})
            .OnClick([] { ++custom_keyboard_clicks; })
            .On<ViewEvents::KeyDown>([](const KeyEvent &event) {
              received_keys.push_back(event.key);
            }),
    };
  });
}

View FocusApp() {
  return HUXERUI_THEME(
      FocusTestTheme,
      FocusContent());
}

View DisabledHitTestApp() {
  return Stack{
      Button("underlying")
          .OnClick([] { ++underlying_clicks; }),
      Button("disabled overlay")
          .With(Enabled{false})
          .OnClick([] { ++disabled_clicks; }),
  };
}

View DisabledSubtreeApp() {
  return Column{
      Button("disabled child")
          .With(Enabled{true})
          .OnClick([] { ++disabled_clicks; }),
  }.With(Enabled{false});
}

View FocusDialogApp() {
  HUXERUI_SCOPE({
    saved_dialogs = UseDialogs();
    return Button("background focus")
        .OnClick([] { ++background_dialog_clicks; });
  });
}

View RootHookApp() {
  HUXERUI_SCOPE({
    observed_root_service_value =
        UseService<TestRootService>()->value;
    return Button("application").OnClick([] {
      ++root_app_clicks;
    });
  });
}

View PresentationApp() {
  HUXERUI_SCOPE({
    saved_toast = UseToast();
    saved_dialogs = UseDialogs();
    return Text("content");
  });
}

View PresentationThemeApp() {
  ThemeDefinition definition;
  definition.Set<huxerui::ToastStyleKey>(
      huxerui::ToastStyle{
          .background = Color::Rgb(20, 30, 40, 0.9F),
          .foreground = Color::Rgb(240, 245, 250),
          .padding = 10.0F,
          .corner_radius = 9.0F,
      });
  definition.Set<huxerui::DialogStyleKey>(
      huxerui::DialogStyle{
          .scrim = Color::Rgb(180, 20, 20, 0.3F),
      });
  return Theme(
      std::move(definition), PresentationApp);
}

View FlatDarkPresentationApp() {
  return HUXERUI_THEME(
      huxerui::FlatDarkTheme,
      PresentationApp());
}

View DeclarativeDialogApp() {
  declarative_dialog_visible = UseState(false);
  return Text("content").With(
      Dialog{
          .visible = declarative_dialog_visible,
          .content = [] {
            return Text("declarative dialog");
          },
          .dismiss_on_outside_press = true,
          .on_dismiss_request = [
              visible = declarative_dialog_visible] {
            visible = false;
          },
      });
}

View AnimationApp() {
  animation_target = UseState(false);
  const bool moved = animation_target.Get();
  return Text("animated").With(
      Offset{AnimateTo(
          Point{moved ? 100.0F : 0.0F, 0.0F},
          TweenSpec{1.0, Easing::Linear})},
      Opacity{AnimateTo(
          moved ? 0.0F : 1.0F,
          TweenSpec{1.0, Easing::Linear})});
}

View IndicationApp() {
  return Button("press").OnClick([] {
    ++indication_clicks;
  });
}

View PresentedIndicationApp() {
  return Stack{
      Button("presented")
          .With(
              huxerui::Frame{80.0F, 40.0F},
              Offset{Point{50.0F, 0.0F}},
              Opacity{0.5F})
          .OnClick([] {}),
  };
}

View ExplicitIndicationApp() {
  return Button("explicit")
      .OnClick([] { ++indication_clicks; })
      .With(huxerui::Indication{
          huxerui::NoIndication{},
      });
}

View ModifierPruningApp() {
  auto visible = UseState(true);
  show_modifier_branch = visible;
  if (visible.Get()) {
    return Column{
        Text("plain"),
        Button("interactive").OnClick([] {}),
    };
  }
  return Column{
      Text("plain"),
  };
}

struct FlowBreakBefore {
  using Value = bool;
};

class TestFlow final : public Layout<TestFlow> {
public:
  using Layout::Layout;

  TestFlow Gap(float value) && {
    return std::move(*this).With(huxerui::Spacing{value});
  }

  static LayoutResult Measure(LayoutContext &context, MountedNode &node,
                              huxerui::Constraints constraints) {
    LayoutResult result;
    float x = 0.0F;
    float y = 0.0F;
    float line_height = 0.0F;
    float measured_width = 0.0F;

    for (MountedNode &child : node.Children()) {
      const Size child_size = context.Measure(child, constraints.Loose());
      const bool break_before = child.LayoutValueOr<FlowBreakBefore>(false);
      if (x > 0.0F &&
          (break_before || x + child_size.width > constraints.max_width)) {
        measured_width = std::max(measured_width, x - node.Spacing());
        x = 0.0F;
        y += line_height + node.Spacing();
        line_height = 0.0F;
      }

      result.Place(child, {x, y});
      x += child_size.width + node.Spacing();
      line_height = std::max(line_height, child_size.height);
    }

    if (x > 0.0F) {
      measured_width = std::max(measured_width, x - node.Spacing());
    }
    result.SetSize(constraints.Constrain({
        measured_width,
        y + line_height,
    }));
    return result;
  }
};

class TestVirtualStrip final : public VirtualLayout<TestVirtualStrip> {
public:
  using VirtualLayout::VirtualLayout;

  static VirtualLayoutResult Measure(VirtualLayoutContext &context,
                                     MountedNode &node,
                                     huxerui::Constraints constraints) {
    constexpr float item_extent = 25.0F;
    const auto viewport = context.Viewport();
    const std::size_t count = context.ItemCount();
    const std::size_t first =
        count == 0
            ? 0
            : std::min(count - 1,
                       static_cast<std::size_t>(std::floor(
                           std::max(0.0F, viewport.offset.y) / item_extent)));
    const std::size_t visible_count = static_cast<std::size_t>(std::ceil(
                                          viewport.size.height / item_extent)) +
                                      1;
    const std::size_t last = std::min(count, first + visible_count);

    VirtualLayoutResult result;
    for (std::size_t index = first; index < last; ++index) {
      MountedNode &item = context.Item(index);
      static_cast<void>(context.Measure(
          item, constraints.LooseHeight().TightHeight(item_extent)));
      result.Place(item, {0.0F, static_cast<float>(index) * item_extent});
    }

    const float content_height = static_cast<float>(count) * item_extent;
    const Size size =
        constraints.Constrain({constraints.max_width, content_height});
    result.SetAxis(Axis::Vertical)
        .SetSize(size)
        .SetContentSize({size.width, content_height});
    static_cast<void>(node);
    return result;
  }

  static std::optional<float> ScrollOffsetForItem(MountedNode &node,
                                                  std::size_t index,
                                                  ScrollAlignment alignment,
                                                  float viewport_extent) {
    constexpr float item_extent = 25.0F;
    const float start = static_cast<float>(index) * item_extent;
    static_cast<void>(node);
    switch (alignment) {
    case ScrollAlignment::Center:
      return start - (viewport_extent - item_extent) * 0.5F;
    case ScrollAlignment::End:
      return start - (viewport_extent - item_extent);
    case ScrollAlignment::Start:
      return start;
    }
    return start;
  }
};

struct TestGridSpan {
  using Value = std::size_t;
};

struct TestGridSpans {
  using Value = std::vector<std::size_t>;
};

class TestVirtualGrid final : public VirtualLayout<TestVirtualGrid> {
public:
  using VirtualLayout::VirtualLayout;

  TestVirtualGrid Spans(std::vector<std::size_t> spans) && {
    SetLayoutValue(typeid(TestGridSpans), std::move(spans));
    return std::move(*this);
  }

  static VirtualLayoutResult Measure(VirtualLayoutContext &context,
                                     MountedNode &node,
                                     huxerui::Constraints constraints) {
    constexpr float minimum_cell_width = 30.0F;
    constexpr float row_height = 20.0F;
    constexpr float cache_extent = row_height;
    const huxerui::VirtualViewport viewport = context.Viewport();
    const std::size_t columns =
        std::max(std::size_t{1},
                 static_cast<std::size_t>(
                     std::floor(viewport.size.width / minimum_cell_width)));
    const float cell_width = viewport.size.width / static_cast<float>(columns);
    const auto *spans = node.LayoutValue<TestGridSpans>();
    auto &cache = node.Cache<GridCache>();

    const bool had_layout = cache.initialized;
    const bool columns_changed = had_layout && cache.columns != columns;
    const std::size_t previous_anchor = cache.anchor_index;
    const float previous_anchor_delta = cache.anchor_delta;
    cache.Prepare(context.ItemCount(), columns,
                  spans == nullptr ? std::vector<std::size_t>{} : *spans);

    float scroll_offset = viewport.offset.y;
    if (columns_changed && previous_anchor < cache.cells.size()) {
      scroll_offset =
          static_cast<float>(cache.cells[previous_anchor].row) * row_height +
          previous_anchor_delta;
    }
    const float content_height =
        static_cast<float>(cache.row_count) * row_height;
    scroll_offset =
        std::clamp(scroll_offset, 0.0F,
                   std::max(0.0F, content_height - viewport.size.height));

    const std::size_t visible_row =
        static_cast<std::size_t>(std::floor(scroll_offset / row_height));
    cache.anchor_index = cache.FirstIndexInRow(visible_row);
    cache.anchor_delta =
        scroll_offset - static_cast<float>(visible_row) * row_height;

    const std::size_t first_row = static_cast<std::size_t>(
        std::floor(std::max(0.0F, scroll_offset - cache_extent) / row_height));
    const std::size_t last_row =
        std::min(cache.row_count,
                 static_cast<std::size_t>(std::ceil(
                     (scroll_offset + viewport.size.height + cache_extent) /
                     row_height)));

    VirtualLayoutResult result;
    for (std::size_t index = 0; index < cache.cells.size(); ++index) {
      const GridCell &cell = cache.cells[index];
      if (cell.row < first_row || cell.row >= last_row) {
        continue;
      }

      MountedNode &item = context.Item(index);
      if (item.LayoutValueOr<TestGridSpan>(std::size_t{1}) != cell.span) {
        throw std::logic_error(
            "HuxerUI test virtual grid item span does not match its plan");
      }
      static_cast<void>(context.Measure(
          item, constraints.Loose()
                    .TightWidth(cell_width * static_cast<float>(cell.span))
                    .TightHeight(row_height)));
      result.Place(item, {
                             static_cast<float>(cell.column) * cell_width,
                             static_cast<float>(cell.row) * row_height,
                         });
    }

    const Size size =
        constraints.Constrain({viewport.size.width, content_height});
    return result.SetAxis(Axis::Vertical)
        .SetSize(size)
        .SetContentSize({size.width, content_height})
        .SetScrollOffset(scroll_offset);
  }

private:
  struct GridCell {
    std::size_t row;
    std::size_t column;
    std::size_t span;
  };

  struct GridCache {
    void Prepare(std::size_t item_count, std::size_t next_columns,
                 const std::vector<std::size_t> &next_spans) {
      if (initialized && columns == next_columns && spans == next_spans &&
          cells.size() == item_count) {
        return;
      }

      columns = next_columns;
      spans = next_spans;
      cells.clear();
      cells.reserve(item_count);
      std::size_t row = 0;
      std::size_t column = 0;
      for (std::size_t index = 0; index < item_count; ++index) {
        const std::size_t requested_span =
            index < spans.size() ? spans[index] : std::size_t{1};
        const std::size_t span =
            std::clamp(requested_span, std::size_t{1}, columns);
        if (column > 0 && column + span > columns) {
          ++row;
          column = 0;
        }
        cells.push_back({row, column, span});
        column += span;
        if (column == columns) {
          ++row;
          column = 0;
        }
      }
      row_count = row + (column > 0 ? 1 : 0);
      initialized = true;
    }

    [[nodiscard]] std::size_t FirstIndexInRow(std::size_t row) const {
      const auto found =
          std::find_if(cells.begin(), cells.end(),
                       [row](const GridCell &cell) { return cell.row >= row; });
      return found == cells.end()
                 ? cells.size()
                 : static_cast<std::size_t>(found - cells.begin());
    }

    bool initialized = false;
    std::size_t columns = 0;
    std::vector<std::size_t> spans;
    std::vector<GridCell> cells;
    std::size_t row_count = 0;
    std::size_t anchor_index = 0;
    float anchor_delta = 0.0F;
  };
};

struct SearchBoxEvents {
  struct Submitted : Event<SearchBoxEvents, void(std::string)> {};
};

State<int> event_mode;
State<bool> use_column_layout;
EventEmitter<SearchBoxEvents> saved_event_emitter;
std::string received_event;
std::vector<PointerEvent> received_pointer_events;
State<bool> show_pointer_target;
int pointer_clicks = 0;
State<int> modifier_value;
int modifier_mounts = 0;
int modifier_updates = 0;
int modifier_destroys = 0;

struct ProbeModifier;

class MountedProbeModifier final : public MountedModifier {
public:
  MountedProbeModifier(MountedNode &node, const ProbeModifier &modifier);
  ~MountedProbeModifier() override { ++modifier_destroys; }

  void Update(MountedNode &node, const ProbeModifier &modifier);

  int value = 0;
};

struct ProbeModifier {
  static const huxerui::detail::ModifierDescriptor &Descriptor() {
    return huxerui::detail::ModifierDescriptorFor<
        ProbeModifier, MountedProbeModifier>();
  }

  int value;
};

MountedProbeModifier::MountedProbeModifier(
    MountedNode &node, const ProbeModifier &modifier)
    : value(modifier.value) {
  static_cast<void>(node);
  ++modifier_mounts;
}

void MountedProbeModifier::Update(
    MountedNode &node, const ProbeModifier &modifier) {
  static_cast<void>(node);
  value = modifier.value;
  ++modifier_updates;
}

View EventSource() {
  HUXERUI_SCOPE({
    auto events = UseEvents<SearchBoxEvents>();
    saved_event_emitter = events;
    return Button("Submit").OnClick(
        [events] { events.Emit<SearchBoxEvents::Submitted>("query"); });
  });
}

View EventApp() {
  auto mode = UseState(0);
  event_mode = mode;

  if (mode.Get() == 2) {
    return Column{
        Text("Hidden"),
    };
  }

  if (mode.Get() == 1) {
    return Column{
        EventSource().Key("source").On<SearchBoxEvents::Submitted>(
            [](std::string value) { received_event = "second:" + value; }),
    };
  }

  return Column{
      EventSource()
          .Key("source")
          .On<SearchBoxEvents::Submitted>(
              [](std::string value) { received_event = "replaced:" + value; })
          .On<SearchBoxEvents::Submitted>(
              [](std::string value) { received_event = "first:" + value; }),
  };
}

View PointerInputApp() {
  auto visible = UseState(true);
  show_pointer_target = visible;
  if (!visible.Get()) {
    return Stack{
        Text("Hidden").With(huxerui::Frame{100.0F, 40.0F}),
    };
  }

  return Stack{
      Button("Target")
          .With(huxerui::Frame{100.0F, 40.0F})
          .On<ViewEvents::PointerDown>([](const PointerEvent &event) {
            received_pointer_events.push_back(event);
          })
          .On<ViewEvents::PointerMove>([](const PointerEvent &event) {
            received_pointer_events.push_back(event);
          })
          .On<ViewEvents::PointerUp>([](const PointerEvent &event) {
            received_pointer_events.push_back(event);
          })
          .On<ViewEvents::PointerCancel>([](const PointerEvent &event) {
            received_pointer_events.push_back(event);
          })
          .OnClick([] { ++pointer_clicks; }),
  };
}

View CustomLayoutApp() {
  return TestFlow{
      Text("A").With(huxerui::Frame{40.0F, 10.0F}),
      Text("B")
          .With(huxerui::Frame{40.0F, 10.0F})
          .LayoutValue<FlowBreakBefore>(true),
      Text("C").With(huxerui::Frame{40.0F, 10.0F}),
  }
      .With(huxerui::Padding{5.0F})
      .Gap(5.0F);
}

View LayoutIdentityApp() {
  auto use_column = UseState(false);
  use_column_layout = use_column;
  if (use_column.Get()) {
    return Column{
        Text("Content"),
    };
  }
  return Row{
      Text("Content"),
  };
}

View CounterApp() {
  auto count = UseState(1);
  return Column{
      Text(count),
      Stack{
          Button("+1").OnClick([count] { count += 1; }),
      },
  }.With(huxerui::Spacing{4.0F});
}

View CopyOnWriteApp() {
  View original = Text("Shared");
  View modified = View(original).With(
      huxerui::Foreground{huxerui::Color::White()});
  return Column{
      original,
      modified,
  };
}

View ModifierApp() {
  auto value = UseState(1);
  modifier_value = value;
  return Text("Modifier").With(
      huxerui::Padding{5.0F},
      huxerui::Background{huxerui::Color::White()},
      ProbeModifier{value.Get()});
}

View ModifierCopyOnWriteApp() {
  View original = Text("Shared");
  View modified = View(original).With(
      huxerui::Foreground{huxerui::Color::White()});
  return Column{
      original,
      modified,
  };
}

View LocalCounter() {
  HUXERUI_SCOPE({
    auto count = UseState(0);
    return Column{
        Text(count),
        Button("+1").OnClick([count] { count += 1; }),
    };
  });
}

enum class CounterIdentity : std::uint8_t {
  First,
  Second,
};

View ScopedCountersApp() {
  return Column{
      LocalCounter().Key(CounterIdentity::First),
      LocalCounter().Key(CounterIdentity::Second),
  };
}

View SharedValue(State<int> value) {
  HUXERUI_SCOPE({ return Text(value); });
}

View SharedStateApp() {
  auto value = UseState(7);
  return Column{
      SharedValue(value),
      Button("+1").OnClick([value] { value += 1; }),
  };
}

View KeyedScopesApp() {
  auto reversed = UseState(false);
  if (reversed.Get()) {
    return Column{
        LocalCounter().Key("second"),
        LocalCounter().Key("first"),
        Button("Reorder").OnClick([reversed] { reversed = false; }),
    };
  }
  return Column{
      LocalCounter().Key("first"),
      LocalCounter().Key("second"),
      Button("Reorder").OnClick([reversed] { reversed = true; }),
  };
}

View DuplicateKeyApp() {
  return Column{
      Text("First").Key("duplicate"),
      Text("Second").Key(std::string{"duplicate"}),
  };
}

View RepeatedUseStateApp() {
  std::vector<View> children;
  for (int index = 0; index < 3; ++index) {
    static_cast<void>(index);
    auto value = UseState(0);
    children.emplace_back(
        Button(std::to_string(value.Get())).OnClick([value] { value += 1; }));
  }
  return Column(std::move(children));
}

int local_root_compositions = 0;
int left_scope_compositions = 0;
int right_scope_compositions = 0;

View CountedCounter(int *compositions) {
  HUXERUI_SCOPE({
    ++*compositions;
    auto count = UseState(0);
    return Column{
        Text(count),
        Button("+1").OnClick([count] { count += 1; }),
    };
  });
}

View LocalRecompositionApp() {
  ++local_root_compositions;
  return Column{
      CountedCounter(&left_scope_compositions),
      CountedCounter(&right_scope_compositions),
  };
}

int prop_root_compositions = 0;
int prop_scope_compositions = 0;
std::string scroll_clicked;
int virtual_item_factory_calls = 0;
int virtual_grid_factory_calls = 0;
int built_in_grid_factory_calls = 0;
State<std::vector<int>> virtual_reorder_items;
State<std::vector<int>> virtual_unkeyed_items;
State<bool> variable_height_expanded;
State<bool> variable_grid_height_expanded;
State<bool> horizontal_virtual_list;
State<bool> show_controlled_scroll;
ScrollState controlled_list_scroll;
ScrollState controlled_grid_scroll;
ScrollState controlled_view_scroll;
ScrollState custom_virtual_scroll;
ScrollState example_scroll;
ScrollState drag_scroll;
ScrollState horizontal_drag_scroll;
ScrollState nested_outer_scroll;
ScrollState nested_inner_scroll;
int scroll_observer_compositions = 0;
int drag_item_clicks = 0;
int drag_item_cancels = 0;

View PropLabel(int value) {
  HUXERUI_SCOPE({
    ++prop_scope_compositions;
    return Text(std::to_string(value));
  });
}

View PropUpdateApp() {
  ++prop_root_compositions;
  auto value = UseState(3);
  return Column{
      PropLabel(value.Get()),
      Button("+1").OnClick([value] { value += 1; }),
  };
}

View AxisAlignmentApp() {
  return Column{
      Text("A").With(huxerui::Frame{20.0F, 20.0F}),
      Text("B").With(huxerui::Frame{20.0F, 20.0F}),
  }.With(
      huxerui::MainAlign{MainAxisAlignment::SpaceBetween},
      huxerui::CrossAlign{CrossAxisAlignment::Center});
}

View SpacerLayoutApp() {
  return Row{
      Text("L").With(huxerui::Frame{20.0F, 20.0F}),
      Spacer(),
      Text("R").With(huxerui::Frame{30.0F, 20.0F}),
  }.With(huxerui::CrossAlign{CrossAxisAlignment::Center});
}

View GrowLayoutApp() {
  return Row{
      Spacer().With(huxerui::Grow{1.0F}),
      Spacer().With(huxerui::Grow{2.0F}),
  };
}

View StackAlignmentApp() {
  return Stack{
      Text("A").With(huxerui::Frame{20.0F, 10.0F}),
  }.With(huxerui::Align{
      HorizontalAlignment::End,
      VerticalAlignment::Center,
  });
}

View StretchLayoutApp() {
  return Column{
      Text("A").With(huxerui::Frame{20.0F, 20.0F}),
  }.With(huxerui::CrossAlign{CrossAxisAlignment::Stretch});
}

View WrappedTextApp() {
  return Column{
      Text("abcdefghij"),
  };
}

View ForEachLayoutApp() {
  const std::vector<std::string> items{
      "First",
      "Second",
      "Third",
  };
  const std::vector<std::string> empty;
  return Column{
      Text("Header"),
      ForEach(items, [](const std::string &item) { return Text(item); }),
      ForEach(empty, [](const std::string &item) { return Text(item); }),
      Text("Footer"),
  }.With(huxerui::Spacing{5.0F});
}

View ForEachIdentityApp() {
  auto expanded = UseState(false);
  const std::vector<std::string> items = expanded.Get()
                                             ? std::vector<std::string>{
                                                   "new",
                                                   "second",
                                                   "first",
                                               }
                                             : std::vector<std::string>{
                                                   "first",
                                                   "second",
                                               };
  return Column{
      ForEach(items,
              [](const std::string &item) { return LocalCounter().Key(item); }),
      Button("Toggle").OnClick([expanded] { expanded = !expanded; }),
  };
}

View ReactiveStateApiApp() {
  auto taps = UseState(2);
  auto items = UseState(std::vector<std::string>{
      "Alpha",
      "Bravo",
  });

  return Column{
      Text::Format("Taps {}", taps),
      ForEach(items, [](const std::string &item) { return Text(item); }),
      Button("Update").OnClick([taps, items] {
        taps += 1;
        items.Update([](auto &values) { values.push_back("Charlie"); });
      }),
  };
}

View ScrollViewApp() {
  return ScrollView{
      Column{
          Button("First").With(huxerui::Frame{100.0F, 40.0F}).OnClick([] {
            scroll_clicked = "First";
          }),
          Button("Second").With(huxerui::Frame{100.0F, 40.0F}).OnClick([] {
            scroll_clicked = "Second";
          }),
          Button("Third").With(huxerui::Frame{100.0F, 40.0F}).OnClick([] {
            scroll_clicked = "Third";
          }),
      },
  };
}

View StatefulListRow(int index) {
  HUXERUI_SCOPE({
    auto taps = UseState(0);
    return Button(std::to_string(index) + ":" + std::to_string(taps.Get()))
        .With(huxerui::Frame{100.0F, 20.0F})
        .OnClick([taps] { taps += 1; });
  });
}

View StatefulForEachScrollApp() {
  std::vector<int> items(100);
  std::iota(items.begin(), items.end(), 0);
  return ScrollView{
      Column{
          ForEach(items,
                  [](int index) { return StatefulListRow(index).Key(index); }),
      },
  };
}

View VirtualListApp() {
  std::vector<int> items(1000);
  std::iota(items.begin(), items.end(), 0);
  return VirtualList(items,
                     [](int index) {
                       ++virtual_item_factory_calls;
                       return Text(std::to_string(index)).Key(index);
                     })
      .ItemExtent(20.0F);
}

View StatefulVirtualListApp() {
  std::vector<int> items(1000);
  std::iota(items.begin(), items.end(), 0);
  return VirtualList(
             items, [](int index) { return StatefulListRow(index).Key(index); })
      .ItemExtent(20.0F);
}

View ReorderableStatefulVirtualListApp() {
  std::vector<int> initial_items(100);
  std::iota(initial_items.begin(), initial_items.end(), 0);
  auto items = UseState(std::move(initial_items));
  virtual_reorder_items = items;
  return VirtualList(
             items, [](int index) { return StatefulListRow(index).Key(index); })
      .ItemExtent(20.0F);
}

View UnkeyedStatefulVirtualListApp() {
  std::vector<int> initial_items(100);
  std::iota(initial_items.begin(), initial_items.end(), 0);
  auto items = UseState(std::move(initial_items));
  virtual_unkeyed_items = items;
  return VirtualList(items, [](int index) { return StatefulListRow(index); })
      .ItemExtent(20.0F);
}

View VariableVirtualListApp() {
  auto expanded = UseState(false);
  variable_height_expanded = expanded;
  const bool first_expanded = expanded.Get();
  std::vector<int> items(100);
  std::iota(items.begin(), items.end(), 0);
  return VirtualList(items, [first_expanded](int index) {
    const float height = index == 0 && first_expanded ? 40.0F
                         : index % 2 == 0             ? 20.0F
                                                      : 40.0F;
    return Text(std::to_string(index))
        .With(huxerui::Frame{100.0F, height})
        .Key(index);
  });
}

View TinyVariableVirtualListApp() {
  std::vector<int> items(1000);
  std::iota(items.begin(), items.end(), 0);
  return VirtualList(items, [](int index) {
    return Text(std::to_string(index))
        .With(huxerui::Frame{100.0F, 1.0F})
        .Key(index);
  });
}

View FixedHorizontalVirtualListApp() {
  std::vector<int> items(1000);
  std::iota(items.begin(), items.end(), 0);
  return VirtualList(items,
                     [](int index) {
                       return Text(std::to_string(index))
                           .With(huxerui::Frame{20.0F, 100.0F})
                           .Key(index);
                     })
      .ScrollAxis(Axis::Horizontal)
      .ItemExtent(20.0F);
}

View VariableHorizontalVirtualListApp() {
  std::vector<int> items(100);
  std::iota(items.begin(), items.end(), 0);
  return VirtualList(items,
                     [](int index) {
                       const float width = index % 2 == 0 ? 20.0F : 40.0F;
                       return Text(std::to_string(index))
                           .With(huxerui::Frame{width, 100.0F})
                           .Key(index);
                     })
      .ScrollAxis(Axis::Horizontal);
}

View StatefulHorizontalVirtualListApp() {
  std::vector<int> items(1000);
  std::iota(items.begin(), items.end(), 0);
  return VirtualList(
             items, [](int index) { return StatefulListRow(index).Key(index); })
      .ScrollAxis(Axis::Horizontal)
      .ItemExtent(100.0F);
}

View VirtualStateListApp() {
  auto items = UseState(std::vector<int>{
      7,
      8,
      9,
  });
  return VirtualList(
             items,
             [](int index) { return Text(std::to_string(index)).Key(index); })
      .ItemExtent(20.0F);
}

View CustomVirtualLayoutApp() {
  auto scroll = UseScrollState();
  custom_virtual_scroll = scroll;
  return TestVirtualStrip(std::size_t{100},
                          [](std::size_t index) {
                            return Text(std::to_string(index)).Key(index);
                          })
      .ScrollState(scroll)
      .With(
          huxerui::ScrollBar{},
          huxerui::Padding{0.0F});
}

View CustomVirtualGridApp() {
  std::vector<std::size_t> spans(200, std::size_t{1});
  for (std::size_t index = 0; index < spans.size(); index += 7) {
    spans[index] = 2;
  }

  return TestVirtualGrid(spans.size(),
                         [spans](std::size_t index) {
                           ++virtual_grid_factory_calls;
                           return StatefulListRow(static_cast<int>(index))
                               .LayoutValue<TestGridSpan>(spans[index])
                               .Key(index);
                         })
      .With(huxerui::Padding{0.0F})
      .Spans(std::move(spans));
}

View BuiltInVirtualGridApp() {
  std::vector<std::size_t> spans(200, std::size_t{1});
  for (std::size_t index = 0; index < spans.size(); index += 7) {
    spans[index] = 2;
  }

  return VirtualGrid(
             spans.size(),
             [](std::size_t index) {
               ++built_in_grid_factory_calls;
               return StatefulListRow(static_cast<int>(index)).Key(index);
             })
      .Columns(GridColumns::Adaptive(30.0F))
      .RowExtent(20.0F)
      .RowSpacing(4.0F)
      .ColumnSpacing(5.0F)
      .CacheExtent(0.0F)
      .ItemSpans(std::move(spans));
}

View VariableVirtualGridApp() {
  auto expanded = UseState(false);
  variable_grid_height_expanded = expanded;
  const bool first_expanded = expanded.Get();
  return VirtualGrid(std::size_t{100},
                     [first_expanded](std::size_t index) {
                       float height = 0.0F;
                       switch (index % 4) {
                       case 0:
                         height = index == 0 && first_expanded ? 60.0F : 20.0F;
                         break;
                       case 1:
                         height = 40.0F;
                         break;
                       case 2:
                         height = 30.0F;
                         break;
                       case 3:
                         height = 10.0F;
                         break;
                       }
                       return Text(std::to_string(index))
                           .With(huxerui::Frame{100.0F, height})
                           .Key(index);
                     })
      .Columns(GridColumns::Fixed(2))
      .EstimatedRowExtent(35.0F)
      .RowSpacing(5.0F);
}

View ScrollObserverItem(int index, ScrollState scroll) {
  HUXERUI_SCOPE({
    if (index == 0) {
      ++scroll_observer_compositions;
    }
    return Text::Format("{}:{}", index, scroll.Offset())
        .With(huxerui::Frame{100.0F, 20.0F});
  });
}

View ControlledVirtualListApp() {
  auto visible = UseState(true);
  auto scroll = UseScrollState(40.0F);
  show_controlled_scroll = visible;
  controlled_list_scroll = scroll;
  if (!visible.Get()) {
    return Text("Hidden");
  }

  return VirtualList(std::size_t{1000},
                     [scroll](std::size_t index) {
                       return ScrollObserverItem(static_cast<int>(index),
                                                 scroll)
                           .Key(index);
                     })
      .ItemExtent(20.0F)
      .CacheExtent(40.0F)
      .ScrollState(scroll);
}

View ScrollStateToolbar(ScrollState scroll) {
  HUXERUI_SCOPE({
    return Row{
        Button("Top").OnClick(
            [scroll] { static_cast<void>(scroll.ScrollTo(0.0F)); }),
        Button("Item 500").OnClick([scroll] {
          static_cast<void>(scroll.ScrollToItem(499, ScrollAlignment::Center));
        }),
        Spacer(),
        Text::Format("Offset {}", static_cast<int>(scroll.Offset())),
    }.With(
        huxerui::Spacing{12.0F},
        huxerui::CrossAlign{CrossAxisAlignment::Center});
  });
}

View ScrollStateExampleApp() {
  auto scroll = UseScrollState();
  example_scroll = scroll;
  return Column{
      ScrollStateToolbar(scroll),
      VirtualList(std::size_t{1000},
                  [](std::size_t index) {
                    return Text::Format("Item {}", index + 1)
                        .With(huxerui::Frame{100.0F, 40.0F})
                        .Key(index);
                  })
          .ItemExtent(48.0F)
          .ScrollState(scroll)
          .With(
              huxerui::Spacing{8.0F},
              huxerui::Grow{}),
  }.With(
      huxerui::Padding{24.0F},
      huxerui::Spacing{12.0F});
}

View DragScrollApp() {
  auto scroll = UseScrollState();
  drag_scroll = scroll;
  return VirtualList(std::size_t{100},
                     [](std::size_t index) {
                       return Button(std::to_string(index))
                           .With(huxerui::Frame{100.0F, 40.0F})
                           .On<ViewEvents::PointerCancel>(
                               [](const PointerEvent &) {
                                 ++drag_item_cancels;
                               })
                           .OnClick([] { ++drag_item_clicks; })
                           .Key(index);
                     })
      .ItemExtent(40.0F)
      .ScrollState(scroll)
      .With(huxerui::ScrollBar{});
}

View ThemedScrollBarApp() {
  ThemeDefinition definition;
  definition.Set<huxerui::ScrollBarStyleKey>(
      huxerui::ScrollBarStyle{
          .thickness = 9.0F,
          .minimum_thumb_extent = 30.0F,
          .margin = 4.0F,
          .corner_radius = 4.5F,
          .fade_in_duration = 0.1F,
          .fade_out_delay = 0.6F,
          .fade_out_duration = 0.2F,
          .track_color = Color::Transparent(),
          .thumb_color = Color::Rgb(200, 80, 60, 0.75F),
      });
  return Theme(
      std::move(definition), DragScrollApp);
}

View FlatDarkScrollBarApp() {
  return HUXERUI_THEME(
      huxerui::FlatDarkTheme,
      DragScrollApp());
}

View HorizontalDragScrollApp() {
  auto scroll = UseScrollState();
  horizontal_drag_scroll = scroll;
  return VirtualList(std::size_t{100},
                     [](std::size_t index) {
                       return Text(std::to_string(index))
                           .With(huxerui::Frame{40.0F, 40.0F})
                           .Key(index);
                     })
      .ScrollAxis(Axis::Horizontal)
      .ItemExtent(40.0F)
      .ScrollState(scroll)
      .With(huxerui::ScrollBar{});
}

View NestedDragScrollApp() {
  auto outer = UseScrollState();
  auto inner = UseScrollState();
  nested_outer_scroll = outer;
  nested_inner_scroll = inner;

  return ScrollView{
      Column{
          Text("Header").With(huxerui::Frame{100.0F, 40.0F}),
          ScrollView{
              Column{
                  Text("0").With(huxerui::Frame{100.0F, 20.0F}),
                  Text("1").With(huxerui::Frame{100.0F, 20.0F}),
                  Text("2").With(huxerui::Frame{100.0F, 20.0F}),
                  Text("3").With(huxerui::Frame{100.0F, 20.0F}),
                  Text("4").With(huxerui::Frame{100.0F, 20.0F}),
                  Text("5").With(huxerui::Frame{100.0F, 20.0F}),
                  Text("6").With(huxerui::Frame{100.0F, 20.0F}),
                  Text("7").With(huxerui::Frame{100.0F, 20.0F}),
                  Text("8").With(huxerui::Frame{100.0F, 20.0F}),
                  Text("9").With(huxerui::Frame{100.0F, 20.0F}),
              },
          }
              .ScrollState(inner)
              .With(
                  huxerui::Frame{100.0F, 60.0F},
                  huxerui::ScrollBar{}),
          Text("Footer").With(huxerui::Frame{100.0F, 200.0F}),
      },
  }
      .ScrollState(outer);
}

View ShortScrollBarApp() {
  return ScrollView{
      Text("Short").With(huxerui::Frame{100.0F, 40.0F}),
  }.With(huxerui::ScrollBar{});
}

View ControlledVirtualGridApp() {
  auto scroll = UseScrollState();
  controlled_grid_scroll = scroll;
  return VirtualGrid(std::size_t{300},
                     [](std::size_t index) {
                       return Text(std::to_string(index))
                           .With(huxerui::Frame{100.0F, 20.0F})
                           .Key(index);
                     })
      .Columns(GridColumns::Fixed(3))
      .RowExtent(20.0F)
      .RowSpacing(4.0F)
      .ScrollState(scroll);
}

View ControlledScrollViewApp() {
  auto scroll = UseScrollState(20.0F);
  controlled_view_scroll = scroll;
  std::vector<int> items(20);
  std::iota(items.begin(), items.end(), 0);
  return ScrollView{
      Column{
          ForEach(items,
                  [](int index) {
                    return Text(std::to_string(index))
                        .With(huxerui::Frame{100.0F, 20.0F});
                  }),
      },
  }
      .ScrollState(scroll);
}

View AdaptiveAxisVirtualListApp() {
  auto horizontal = UseState(false);
  horizontal_virtual_list = horizontal;
  std::vector<int> items(1000);
  std::iota(items.begin(), items.end(), 0);
  return VirtualList(items,
                     [](int index) {
                       return Text(std::to_string(index))
                           .With(huxerui::Frame{20.0F, 20.0F})
                           .Key(index);
                     })
      .ScrollAxis(horizontal.Get() ? Axis::Horizontal : Axis::Vertical)
      .ItemExtent(20.0F);
}

std::string FirstText(const DisplayList &display_list) {
  for (const auto &command : display_list.Commands()) {
    if (const auto *text = std::get_if<DrawTextCommand>(&command)) {
      return text->text;
    }
  }
  return {};
}

bool ContainsText(const DisplayList &display_list, std::string_view expected) {
  for (const auto &command : display_list.Commands()) {
    const auto *text = std::get_if<DrawTextCommand>(&command);
    if (text && text->text == expected) {
      return true;
    }
  }
  return false;
}

const DrawTextCommand *FindText(
    const DisplayList &display_list, std::string_view expected) {
  for (const auto &command : display_list.Commands()) {
    const auto *text = std::get_if<DrawTextCommand>(&command);
    if (text && text->text == expected) {
      return text;
    }
  }
  return nullptr;
}

const DrawRectCommand *FindRect(
    const DisplayList &display_list, Rect expected) {
  for (const auto &command : display_list.Commands()) {
    const auto *rect = std::get_if<DrawRectCommand>(&command);
    if (rect && rect->rect.x == expected.x &&
        rect->rect.y == expected.y &&
        rect->rect.width == expected.width &&
        rect->rect.height == expected.height) {
      return rect;
    }
  }
  return nullptr;
}

const DrawRectCommand *FindRectWithColor(
    const DisplayList &display_list, Color expected) {
  for (const auto &command : display_list.Commands()) {
    const auto *rect = std::get_if<DrawRectCommand>(&command);
    if (rect && rect->color.red == expected.red &&
        rect->color.green == expected.green &&
        rect->color.blue == expected.blue &&
        rect->color.alpha == expected.alpha) {
      return rect;
    }
  }
  return nullptr;
}

const DrawBorderCommand *FindBorderWithColor(
    const DisplayList &display_list, Color expected) {
  for (const auto &command : display_list.Commands()) {
    const auto *border =
        std::get_if<DrawBorderCommand>(&command);
    if (border && border->color.red == expected.red &&
        border->color.green == expected.green &&
        border->color.blue == expected.blue &&
        border->color.alpha == expected.alpha) {
      return border;
    }
  }
  return nullptr;
}

bool ContainsRect(const DisplayList &display_list, Rect expected) {
  for (const auto &command : display_list.Commands()) {
    const auto *rect = std::get_if<DrawRectCommand>(&command);
    if (rect && rect->rect.x == expected.x &&
        rect->rect.y == expected.y &&
        rect->rect.width == expected.width &&
        rect->rect.height == expected.height) {
      return true;
    }
  }
  return false;
}

std::optional<float> RectAlpha(const DisplayList &display_list,
                               Rect expected) {
  for (const auto &command : display_list.Commands()) {
    const auto *rect = std::get_if<DrawRectCommand>(&command);
    if (rect && rect->rect.x == expected.x &&
        rect->rect.y == expected.y &&
        rect->rect.width == expected.width &&
        rect->rect.height == expected.height) {
      return rect->color.alpha;
    }
  }
  return std::nullopt;
}

void Check(bool condition, const char *expression, int line) {
  if (condition) {
    return;
  }
  std::cerr << "Check failed at line " << line << ": " << expression << '\n';
  std::exit(1);
}

#define HUXERUI_CHECK(expression) Check((expression), #expression, __LINE__)

void InvokeClick(const huxerui::detail::MountedNode &node) {
  HUXERUI_CHECK(huxerui::detail::EmitEvent<ViewEvents::Click>(
      node.event_bindings));
}

void ClickAt(Runtime &runtime, Point position, std::int64_t pointer_id = 0) {
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

void TestUseStateAndStateUpdate() {
  TestPlatform platform;
  Runtime runtime{CounterApp, platform};
  runtime.SetViewport({320.0F, 240.0F});

  const DisplayList &initial = runtime.BuildFrame();
  HUXERUI_CHECK(FirstText(initial) == "1");

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  const std::uint64_t root_identity = root->identity;

  runtime.InvalidateRoot();
  const DisplayList &recomposed = runtime.BuildFrame();
  HUXERUI_CHECK(FirstText(recomposed) == "1");
  HUXERUI_CHECK(runtime.RootNode()->identity == root_identity);

  ClickAt(runtime, {10.0F, 42.0F});
  HUXERUI_CHECK(platform.requested_frames > 0);

  const DisplayList &updated = runtime.BuildFrame();
  HUXERUI_CHECK(FirstText(updated) == "2");
  HUXERUI_CHECK(runtime.RootNode()->identity == root_identity);
}

void TestLayoutAndHitTest() {
  TestPlatform platform;
  Runtime runtime{CounterApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->children.size() == 2);
  HUXERUI_CHECK(root->children[0]->frame.y == 0.0F);
  HUXERUI_CHECK(root->children[1]->frame.y == 24.0F);
  HUXERUI_CHECK(root->children[1]->children.size() == 1);
  HUXERUI_CHECK(huxerui::detail::HasEventBinding<ViewEvents::Click>(
      root->children[1]->children[0]->event_bindings));
}

void TestViewCopyOnWrite() {
  TestPlatform platform;
  Runtime runtime{CopyOnWriteApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->children.size() == 2);
  HUXERUI_CHECK(root->children[0]->style.foreground.has_value());
  HUXERUI_CHECK(
      root->children[0]->style.foreground->red ==
      huxerui::TextStyleKey::Default().foreground.red);
  HUXERUI_CHECK(root->children[1]->style.foreground.has_value());
  HUXERUI_CHECK(root->children[1]->style.foreground->red == 1.0F);
}

void TestModifierReconciliationAndCopyOnWrite() {
  modifier_mounts = 0;
  modifier_updates = 0;
  modifier_destroys = 0;

  TestPlatform platform;
  {
    Runtime runtime{ModifierApp, platform};
    runtime.SetViewport({320.0F, 240.0F});
    runtime.BuildFrame();

    const auto *root = runtime.RootNode();
    HUXERUI_CHECK(root != nullptr);
    HUXERUI_CHECK(root->style.padding.left == 5.0F);
    HUXERUI_CHECK(root->style.background.has_value());
    HUXERUI_CHECK(root->modifiers.size() == 3);
    HUXERUI_CHECK(root->modifiers[2].mounted != nullptr);
    HUXERUI_CHECK(modifier_mounts == 1);
    HUXERUI_CHECK(modifier_updates == 0);
    const std::uint64_t identity = root->identity;

    modifier_value = 2;
    runtime.BuildFrame();

    root = runtime.RootNode();
    HUXERUI_CHECK(root->identity == identity);
    HUXERUI_CHECK(modifier_mounts == 1);
    HUXERUI_CHECK(modifier_updates == 1);
    HUXERUI_CHECK(
        static_cast<MountedProbeModifier *>(
            root->modifiers[2].mounted.get())
            ->value == 2);
  }
  HUXERUI_CHECK(modifier_destroys == 1);

  Runtime copy_runtime{ModifierCopyOnWriteApp, platform};
  copy_runtime.SetViewport({320.0F, 240.0F});
  copy_runtime.BuildFrame();
  const auto *copy_root = copy_runtime.RootNode();
  HUXERUI_CHECK(copy_root != nullptr);
  HUXERUI_CHECK(copy_root->children[0]->style.foreground.has_value());
  HUXERUI_CHECK(
      copy_root->children[0]->style.foreground->red ==
      huxerui::TextStyleKey::Default().foreground.red);
  HUXERUI_CHECK(copy_root->children[1]->style.foreground.has_value());
}

void TestNestedEnvironmentValues() {
  observed_environment_values.clear();

  TestPlatform platform;
  Runtime runtime{EnvironmentApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();

  HUXERUI_CHECK(observed_environment_values.size() == 2);
  HUXERUI_CHECK(observed_environment_values[0] == "outer");
  HUXERUI_CHECK(observed_environment_values[1] == "inner");
}

void TestThemeProviderUpdatesNestedContent() {
  TestPlatform platform;
  Runtime runtime{ThemeApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  const DisplayList &initial = runtime.BuildFrame();

  HUXERUI_CHECK(
      observed_theme_color.red == Color::Rgb(40, 100, 220).red);
  HUXERUI_CHECK(
      observed_nested_theme_color.red ==
      observed_theme_color.red);

  const DrawTextCommand *theme_text =
      FindText(initial, "theme text");
  HUXERUI_CHECK(theme_text != nullptr);
  HUXERUI_CHECK(
      theme_text->color.green == Color::Rgb(30, 90, 55).green);
  HUXERUI_CHECK(theme_text->font_size == 18.0F);

  const DrawTextCommand *theme_title =
      FindText(initial, "theme title");
  HUXERUI_CHECK(theme_title != nullptr);
  HUXERUI_CHECK(theme_title->font_size == 25.0F);

  const DrawTextCommand *theme_label =
      FindText(initial, "theme label");
  HUXERUI_CHECK(theme_label != nullptr);
  HUXERUI_CHECK(theme_label->font_size == 16.0F);

  const DrawTextCommand *theme_button =
      FindText(initial, "theme button");
  HUXERUI_CHECK(theme_button != nullptr);
  HUXERUI_CHECK(theme_button->font_size == 16.0F);
  const DrawRectCommand *theme_button_background =
      FindRect(initial, theme_button->rect);
  HUXERUI_CHECK(theme_button_background != nullptr);
  HUXERUI_CHECK(
      theme_button_background->color.blue ==
      Color::Rgb(40, 100, 220).blue);

  const DrawTextCommand *nested_button =
      FindText(initial, "nested button");
  HUXERUI_CHECK(nested_button != nullptr);
  HUXERUI_CHECK(nested_button->font_size == 21.0F);
  const DrawRectCommand *nested_button_background =
      FindRect(initial, nested_button->rect);
  HUXERUI_CHECK(nested_button_background != nullptr);
  HUXERUI_CHECK(
      nested_button_background->corner_radius == 13.0F);

  const DrawTextCommand *explicit_text =
      FindText(initial, "explicit text");
  HUXERUI_CHECK(explicit_text != nullptr);
  HUXERUI_CHECK(explicit_text->font_size == 29.0F);
  HUXERUI_CHECK(
      explicit_text->color.red ==
      Color::Rgb(255, 140, 0).red);

  alternate_theme = true;
  const DisplayList &updated = runtime.BuildFrame();
  HUXERUI_CHECK(
      observed_theme_color.red == Color::Rgb(220, 70, 50).red);
  const DrawTextCommand *updated_button =
      FindText(updated, "theme button");
  HUXERUI_CHECK(updated_button != nullptr);
  const DrawRectCommand *updated_button_background =
      FindRect(updated, updated_button->rect);
  HUXERUI_CHECK(updated_button_background != nullptr);
  HUXERUI_CHECK(
      updated_button_background->color.red ==
      Color::Rgb(220, 70, 50).red);
}

void TestFlatDarkThemeAndSemanticTextRoles() {
  TestPlatform platform;
  Runtime runtime{FlatDarkThemeApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  const DisplayList &display_list = runtime.BuildFrame();

  const ThemeSpec dark = huxerui::FlatDarkThemeSpec();
  const DrawTextCommand *body =
      FindText(display_list, "dark body");
  HUXERUI_CHECK(body != nullptr);
  HUXERUI_CHECK(
      body->color.red == dark.colors.on_surface.red);
  HUXERUI_CHECK(
      body->font_size == dark.typography.body);

  const DrawTextCommand *title =
      FindText(display_list, "dark title");
  HUXERUI_CHECK(title != nullptr);
  HUXERUI_CHECK(
      title->font_size == dark.typography.title);

  const DrawTextCommand *button =
      FindText(display_list, "dark button");
  HUXERUI_CHECK(button != nullptr);
  HUXERUI_CHECK(
      button->color.red == dark.colors.on_primary.red);
  const DrawRectCommand *background =
      FindRect(display_list, button->rect);
  HUXERUI_CHECK(background != nullptr);
  HUXERUI_CHECK(
      background->color.blue == dark.colors.primary.blue);
}

void TestFlatThemeHoverAndPressedIndication() {
  const ThemeSpec light = huxerui::FlatLightThemeSpec();
  const ThemeSpec dark = huxerui::FlatDarkThemeSpec();
  HUXERUI_CHECK(
      std::abs(light.interactions.hover_overlay.alpha - 0.10F) <
      0.001F);
  HUXERUI_CHECK(
      std::abs(light.interactions.pressed_overlay.alpha - 0.16F) <
      0.001F);
  HUXERUI_CHECK(
      std::abs(dark.interactions.hover_overlay.alpha - 0.12F) <
      0.001F);
  HUXERUI_CHECK(
      std::abs(dark.interactions.pressed_overlay.alpha - 0.18F) <
      0.001F);

  TestPlatform platform;
  Runtime runtime{FlatThemeInteractionApp, platform};
  runtime.SetViewport({200.0F, 80.0F});
  const DisplayList &initial = runtime.BuildFrame();
  const DrawTextCommand *button =
      FindText(initial, "flat interaction");
  HUXERUI_CHECK(button != nullptr);
  const Point pointer{
      button->rect.x + button->rect.width * 0.5F,
      button->rect.y + button->rect.height * 0.5F,
  };

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      105,
      pointer,
  });
  runtime.BuildFrame();
  platform.AdvanceTime(light.motion.fast);
  const DisplayList &hovered = runtime.BuildFrame();
  HUXERUI_CHECK(
      FindRectWithColor(
          hovered, light.interactions.hover_overlay) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      105,
      pointer,
  });
  const DisplayList &pressed = runtime.BuildFrame();
  HUXERUI_CHECK(
      FindRectWithColor(
          pressed, light.interactions.pressed_overlay) != nullptr);
}

void TestMaterialThemeDefinitionsAndIndication() {
  const ThemeSpec light = huxerui::MaterialLightThemeSpec();
  const ThemeSpec dark = huxerui::MaterialDarkThemeSpec();
  HUXERUI_CHECK(
      light.colors.primary.red ==
      Color::Rgb(103, 80, 164).red);
  HUXERUI_CHECK(
      dark.colors.primary.blue ==
      Color::Rgb(208, 188, 255).blue);
  HUXERUI_CHECK(light.typography.title == 22.0F);
  HUXERUI_CHECK(light.shapes.large == 28.0F);
  HUXERUI_CHECK(light.elevation.medium == 3.0F);
  HUXERUI_CHECK(
      light.interactions.indication ==
      huxerui::IndicationKind::Ripple);

  const ThemeDefinition definition =
      huxerui::MaterialThemeDefinition();
  const auto *button_style = std::any_cast<ButtonStyle>(
      definition.Values().Find(typeid(ButtonStyleKey)));
  HUXERUI_CHECK(button_style != nullptr);
  HUXERUI_CHECK(button_style->corner_radius == 20.0F);
  HUXERUI_CHECK(button_style->padding.left == 24.0F);
  HUXERUI_CHECK(button_style->padding.top == 10.0F);

  const auto *checkbox_style = std::any_cast<CheckboxStyle>(
      definition.Values().Find(typeid(CheckboxStyleKey)));
  HUXERUI_CHECK(checkbox_style != nullptr);
  HUXERUI_CHECK(checkbox_style->size == 20.0F);
  HUXERUI_CHECK(checkbox_style->corner_radius == 2.0F);
  HUXERUI_CHECK(
      checkbox_style->checked_background.red ==
      light.colors.primary.red);

  const auto *switch_style = std::any_cast<SwitchStyle>(
      definition.Values().Find(typeid(SwitchStyleKey)));
  HUXERUI_CHECK(switch_style != nullptr);
  HUXERUI_CHECK(switch_style->width == 52.0F);
  HUXERUI_CHECK(switch_style->height == 32.0F);
  HUXERUI_CHECK(switch_style->thumb_radius == 12.0F);

  const auto *progress_circle_style =
      std::any_cast<ProgressCircleStyle>(
          definition.Values().Find(
              typeid(ProgressCircleStyleKey)));
  HUXERUI_CHECK(progress_circle_style != nullptr);
  HUXERUI_CHECK(progress_circle_style->size == 40.0F);
  HUXERUI_CHECK(progress_circle_style->stroke_width == 4.0F);
  HUXERUI_CHECK(
      progress_circle_style->indicator_color.red ==
      light.colors.primary.red);

  const auto *toast_style =
      std::any_cast<huxerui::ToastStyle>(
          definition.Values().Find(
              typeid(huxerui::ToastStyleKey)));
  HUXERUI_CHECK(toast_style != nullptr);
  HUXERUI_CHECK(
      toast_style->background.red ==
      Color::Rgb(50, 47, 53).red);

  const auto *dialog_style =
      std::any_cast<huxerui::DialogStyle>(
          definition.Values().Find(
              typeid(huxerui::DialogStyleKey)));
  HUXERUI_CHECK(dialog_style != nullptr);
  HUXERUI_CHECK(
      dialog_style->scrim.alpha ==
      light.colors.scrim.alpha);

  const auto *scroll_bar_style =
      std::any_cast<huxerui::ScrollBarStyle>(
          definition.Values().Find(
              typeid(huxerui::ScrollBarStyleKey)));
  HUXERUI_CHECK(scroll_bar_style != nullptr);
  HUXERUI_CHECK(scroll_bar_style->thickness == 4.0F);
  HUXERUI_CHECK(scroll_bar_style->corner_radius == 2.0F);

  ThemeSpec brand = light;
  brand.colors.primary = Color::Rgb(20, 110, 90);
  const ThemeDefinition brand_definition =
      huxerui::MaterialThemeDefinition(brand);
  const auto *brand_button_style =
      std::any_cast<ButtonStyle>(
          brand_definition.Values().Find(
              typeid(ButtonStyleKey)));
  HUXERUI_CHECK(brand_button_style != nullptr);
  HUXERUI_CHECK(
      brand_button_style->background.green ==
      brand.colors.primary.green);

  TestPlatform platform;
  Runtime runtime{MaterialThemeApp, platform};
  runtime.SetViewport({240.0F, 80.0F});
  const DisplayList &initial = runtime.BuildFrame();
  const DrawTextCommand *button =
      FindText(initial, "material button");
  HUXERUI_CHECK(button != nullptr);
  HUXERUI_CHECK(
      button->color.red == light.colors.on_primary.red);
  HUXERUI_CHECK(
      button->font_size == light.typography.label);
  const DrawRectCommand *background =
      FindRect(initial, button->rect);
  HUXERUI_CHECK(background != nullptr);
  HUXERUI_CHECK(
      background->color.red == light.colors.primary.red);
  HUXERUI_CHECK(background->corner_radius == 20.0F);

  const Point pointer{
      button->rect.x + button->rect.width * 0.5F,
      button->rect.y + button->rect.height * 0.5F,
  };
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      110,
      pointer,
  });
  runtime.BuildFrame();
  platform.AdvanceTime(light.motion.fast);
  const DisplayList &hovered = runtime.BuildFrame();
  HUXERUI_CHECK(
      FindRectWithColor(
          hovered, light.interactions.hover_overlay) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      110,
      pointer,
  });
  runtime.BuildFrame();
  platform.AdvanceTime(light.motion.slow * 0.5);
  const DisplayList &pressed = runtime.BuildFrame();
  const huxerui::DrawCircleCommand *ripple = nullptr;
  const PushClipCommand *ripple_clip = nullptr;
  for (const auto &command : pressed.Commands()) {
    if (const auto *clip =
            std::get_if<PushClipCommand>(&command);
        clip && clip->corner_radius > 0.0F) {
      ripple_clip = clip;
    }
    const auto *circle =
        std::get_if<huxerui::DrawCircleCommand>(&command);
    if (circle && circle->color.alpha > 0.0F) {
      ripple = circle;
      break;
    }
  }
  HUXERUI_CHECK(ripple != nullptr);
  HUXERUI_CHECK(ripple_clip != nullptr);
  HUXERUI_CHECK(ripple_clip->corner_radius == 20.0F);
  HUXERUI_CHECK(ripple->radius > 0.0F);
  HUXERUI_CHECK(
      ripple->color.alpha == light.interactions.ripple.alpha);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      110,
      pointer,
  });
  runtime.BuildFrame();
  platform.AdvanceTime(light.motion.normal);
  runtime.BuildFrame();

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Space,
  });
  runtime.BuildFrame();
  platform.AdvanceTime(light.motion.slow * 0.5);
  const DisplayList &keyboard_pressed =
      runtime.BuildFrame();
  const huxerui::DrawCircleCommand *keyboard_ripple =
      nullptr;
  for (const auto &command : keyboard_pressed.Commands()) {
    const auto *circle =
        std::get_if<huxerui::DrawCircleCommand>(&command);
    if (circle && circle->radius > 0.0F) {
      keyboard_ripple = circle;
      break;
    }
  }
  HUXERUI_CHECK(keyboard_ripple != nullptr);
  HUXERUI_CHECK(
      std::abs(keyboard_ripple->center.x - pointer.x) <
      0.01F);
  HUXERUI_CHECK(
      std::abs(keyboard_ripple->center.y - pointer.y) <
      0.01F);
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Up,
      .key = Key::Space,
  });

  Runtime dark_runtime{MaterialDarkThemeApp, platform};
  dark_runtime.SetViewport({240.0F, 80.0F});
  const DisplayList &dark_display =
      dark_runtime.BuildFrame();
  const DrawTextCommand *dark_button =
      FindText(dark_display, "material dark button");
  HUXERUI_CHECK(dark_button != nullptr);
  const DrawRectCommand *dark_background =
      FindRect(dark_display, dark_button->rect);
  HUXERUI_CHECK(dark_background != nullptr);
  HUXERUI_CHECK(
      dark_background->color.red ==
      dark.colors.primary.red);
}

void TestControlledTogglesAndAnimation() {
  checkbox_changes = 0;
  switch_changes = 0;

  TestPlatform platform;
  Runtime runtime{ToggleApp, platform};
  runtime.SetViewport({160.0F, 64.0F});
  const DisplayList &initial = runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->children.size() == 2);
  const auto *checkbox = root->children[0].get();
  const auto *switch_node = root->children[1].get();
  HUXERUI_CHECK(
      checkbox->kind == huxerui::detail::NodeKind::Checkbox);
  HUXERUI_CHECK(
      switch_node->kind == huxerui::detail::NodeKind::Switch);
  HUXERUI_CHECK(checkbox->focusable);
  HUXERUI_CHECK(switch_node->focusable);
  HUXERUI_CHECK(checkbox->measured_size.width == 20.0F);
  HUXERUI_CHECK(switch_node->measured_size.width == 40.0F);

  const huxerui::DrawCircleCommand *initial_thumb = nullptr;
  for (const auto &command : initial.Commands()) {
    if (const auto *circle =
            std::get_if<huxerui::DrawCircleCommand>(&command)) {
      initial_thumb = circle;
      break;
    }
  }
  HUXERUI_CHECK(initial_thumb != nullptr);
  const float initial_thumb_x = initial_thumb->center.x;

  const std::uint64_t checkbox_identity = checkbox->identity;
  ClickAt(runtime, {
                       checkbox->frame.x + checkbox->frame.width * 0.5F,
                       checkbox->frame.y + checkbox->frame.height * 0.5F,
                   });
  const DisplayList &checked_display = runtime.BuildFrame();
  HUXERUI_CHECK(checkbox_changes == 1);
  HUXERUI_CHECK(checkbox_checked.Get());
  HUXERUI_CHECK(FindText(checked_display, "✓") != nullptr);
  HUXERUI_CHECK(
      runtime.RootNode()->children[0]->identity == checkbox_identity);

  switch_node = runtime.RootNode()->children[1].get();
  ClickAt(runtime, {
                       switch_node->frame.x +
                           switch_node->frame.width * 0.5F,
                       switch_node->frame.y +
                           switch_node->frame.height * 0.5F,
                   });
  const DisplayList &switch_start = runtime.BuildFrame();
  HUXERUI_CHECK(switch_changes == 1);
  HUXERUI_CHECK(switch_checked.Get());

  const huxerui::DrawCircleCommand *start_thumb = nullptr;
  for (const auto &command : switch_start.Commands()) {
    if (const auto *circle =
            std::get_if<huxerui::DrawCircleCommand>(&command)) {
      start_thumb = circle;
      break;
    }
  }
  HUXERUI_CHECK(start_thumb != nullptr);
  HUXERUI_CHECK(
      std::abs(start_thumb->center.x - initial_thumb_x) < 0.001F);

  platform.AdvanceTime(0.1);
  const DisplayList &switch_middle = runtime.BuildFrame();
  const huxerui::DrawCircleCommand *middle_thumb = nullptr;
  for (const auto &command : switch_middle.Commands()) {
    if (const auto *circle =
            std::get_if<huxerui::DrawCircleCommand>(&command)) {
      middle_thumb = circle;
      break;
    }
  }
  HUXERUI_CHECK(middle_thumb != nullptr);
  HUXERUI_CHECK(middle_thumb->center.x > initial_thumb_x);
  const float middle_thumb_x = middle_thumb->center.x;

  platform.AdvanceTime(0.2);
  const DisplayList &switch_end = runtime.BuildFrame();
  const huxerui::DrawCircleCommand *end_thumb = nullptr;
  for (const auto &command : switch_end.Commands()) {
    if (const auto *circle =
            std::get_if<huxerui::DrawCircleCommand>(&command)) {
      end_thumb = circle;
      break;
    }
  }
  HUXERUI_CHECK(end_thumb != nullptr);
  HUXERUI_CHECK(end_thumb->center.x > middle_thumb_x);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Space,
  });
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Up,
      .key = Key::Space,
  });
  HUXERUI_CHECK(checkbox_changes == 2);
  HUXERUI_CHECK(!checkbox_checked.Get());

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Enter,
  });
  HUXERUI_CHECK(switch_changes == 2);
  HUXERUI_CHECK(!switch_checked.Get());
}

void TestProgressCircleDrawingStateAndAnimation() {
  constexpr float pi = 3.14159265358979323846F;
  const auto arcs = [](const DisplayList &display_list) {
    std::vector<DrawArcCommand> result;
    for (const auto &command : display_list.Commands()) {
      if (const auto *arc =
              std::get_if<DrawArcCommand>(&command)) {
        result.push_back(*arc);
      }
    }
    return result;
  };

  TestPlatform platform;
  Runtime determinate{DeterminateProgressCircleApp, platform};
  determinate.SetViewport({64.0F, 64.0F});
  const DisplayList &initial = determinate.BuildFrame();
  const auto initial_arcs = arcs(initial);
  HUXERUI_CHECK(initial_arcs.size() == 2);
  HUXERUI_CHECK(
      std::abs(initial_arcs[0].sweep_angle - pi * 2.0F) <
      0.001F);
  HUXERUI_CHECK(initial_arcs[0].cap == StrokeCap::Butt);
  HUXERUI_CHECK(
      std::abs(initial_arcs[1].sweep_angle - pi * 0.5F) <
      0.001F);
  HUXERUI_CHECK(initial_arcs[1].cap == StrokeCap::Round);

  const auto *root = determinate.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->children.size() == 1);
  const auto *progress_node = root->children[0].get();
  HUXERUI_CHECK(
      progress_node->kind ==
      huxerui::detail::NodeKind::ProgressCircle);
  HUXERUI_CHECK(progress_node->measured_size.width == 24.0F);
  HUXERUI_CHECK(progress_node->measured_size.height == 24.0F);
  const std::uint64_t identity = progress_node->identity;

  progress_circle_value = 0.75F;
  const auto updated_arcs = arcs(determinate.BuildFrame());
  HUXERUI_CHECK(updated_arcs.size() == 2);
  HUXERUI_CHECK(
      std::abs(updated_arcs[1].sweep_angle - pi * 1.5F) <
      0.001F);
  HUXERUI_CHECK(
      determinate.RootNode()->children[0]->identity == identity);

  Runtime empty{EmptyProgressCircleApp, platform};
  empty.SetViewport({64.0F, 64.0F});
  HUXERUI_CHECK(arcs(empty.BuildFrame()).size() == 1);

  Runtime full{FullProgressCircleApp, platform};
  full.SetViewport({64.0F, 64.0F});
  const auto full_arcs = arcs(full.BuildFrame());
  HUXERUI_CHECK(full_arcs.size() == 2);
  HUXERUI_CHECK(
      std::abs(full_arcs[1].sweep_angle - pi * 2.0F) <
      0.001F);

  TestPlatform animated_platform;
  Runtime animated{
      IndeterminateProgressCircleApp,
      animated_platform};
  animated.SetViewport({64.0F, 64.0F});
  const int requests_before =
      animated_platform.requested_frames;
  const auto animated_initial = arcs(animated.BuildFrame());
  HUXERUI_CHECK(animated_initial.size() == 2);
  HUXERUI_CHECK(
      animated_platform.requested_frames >
      requests_before);
  const float initial_start =
      animated_initial[1].start_angle;

  animated_platform.AdvanceTime(0.48);
  const auto animated_next = arcs(animated.BuildFrame());
  HUXERUI_CHECK(animated_next.size() == 2);
  HUXERUI_CHECK(
      std::abs(
          animated_next[1].start_angle -
          initial_start) >
      0.1F);

  TestPlatform reduced_platform;
  Runtime reduced{
      ReducedMotionProgressCircleApp,
      reduced_platform};
  reduced.SetViewport({64.0F, 64.0F});
  const int reduced_requests_before =
      reduced_platform.requested_frames;
  const auto reduced_arcs = arcs(reduced.BuildFrame());
  HUXERUI_CHECK(reduced_arcs.size() == 2);
  HUXERUI_CHECK(
      reduced_platform.requested_frames ==
      reduced_requests_before);
}

void TestThemeDrivesHoverAndPressedIndication() {
  TestPlatform platform;
  Runtime runtime{ThemedIndicationApp, platform};
  runtime.SetViewport({200.0F, 80.0F});
  runtime.BuildFrame();

  const Color hover = Color::Rgb(20, 80, 160, 0.2F);
  const Color pressed = Color::Rgb(200, 40, 60, 0.3F);
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      101,
      {20.0F, 20.0F},
  });
  const DisplayList &hovered = runtime.BuildFrame();
  HUXERUI_CHECK(
      FindRectWithColor(hovered, hover) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      101,
      {20.0F, 20.0F},
  });
  const DisplayList &down = runtime.BuildFrame();
  HUXERUI_CHECK(
      FindRectWithColor(down, pressed) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      101,
      {20.0F, 20.0F},
  });
  const DisplayList &released = runtime.BuildFrame();
  HUXERUI_CHECK(
      FindRectWithColor(released, hover) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      101,
      {240.0F, 120.0F},
  });
  const DisplayList &outside = runtime.BuildFrame();
  HUXERUI_CHECK(
      FindRectWithColor(outside, hover) == nullptr);
}

void TestEnabledInheritanceAndHitTestBlocking() {
  disabled_clicks = 0;
  underlying_clicks = 0;

  TestPlatform platform;
  Runtime overlay{DisabledHitTestApp, platform};
  overlay.SetViewport({200.0F, 80.0F});
  const DisplayList &display_list = overlay.BuildFrame();
  const DrawTextCommand *disabled =
      FindText(display_list, "disabled overlay");
  HUXERUI_CHECK(disabled != nullptr);
  HUXERUI_CHECK(
      std::abs(disabled->color.alpha - 0.42F) < 0.001F);

  const auto *overlay_root = overlay.RootNode();
  HUXERUI_CHECK(overlay_root != nullptr);
  HUXERUI_CHECK(overlay_root->children.size() == 2);
  HUXERUI_CHECK(!overlay_root->children[1]->IsEnabled());
  ClickAt(
      overlay,
      {
          disabled->rect.x + disabled->rect.width * 0.5F,
          disabled->rect.y + disabled->rect.height * 0.5F,
      },
      102);
  HUXERUI_CHECK(disabled_clicks == 0);
  HUXERUI_CHECK(underlying_clicks == 0);

  Runtime subtree{DisabledSubtreeApp, platform};
  subtree.SetViewport({200.0F, 80.0F});
  const DisplayList &subtree_display = subtree.BuildFrame();
  const auto *subtree_root = subtree.RootNode();
  HUXERUI_CHECK(subtree_root != nullptr);
  HUXERUI_CHECK(!subtree_root->IsEnabled());
  HUXERUI_CHECK(subtree_root->children.size() == 1);
  HUXERUI_CHECK(!subtree_root->children[0]->IsEnabled());
  const DrawTextCommand *child =
      FindText(subtree_display, "disabled child");
  HUXERUI_CHECK(child != nullptr);
  ClickAt(
      subtree,
      {
          child->rect.x + child->rect.width * 0.5F,
          child->rect.y + child->rect.height * 0.5F,
      },
      103);
  HUXERUI_CHECK(disabled_clicks == 0);
}

void TestFocusTraversalKeyboardAndThemeVisuals() {
  focus_changes.clear();
  received_keys.clear();
  first_keyboard_clicks = 0;
  third_keyboard_clicks = 0;
  custom_keyboard_clicks = 0;
  disabled_clicks = 0;

  TestPlatform platform;
  Runtime runtime{FocusApp, platform};
  runtime.SetViewport({240.0F, 180.0F});
  runtime.BuildFrame();

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  const DisplayList &first_focused = runtime.BuildFrame();
  HUXERUI_CHECK(focus_changes.size() == 1);
  HUXERUI_CHECK(focus_changes.back() == "first:on");
  const DrawBorderCommand *first_border =
      FindBorderWithColor(
          first_focused, Color::Rgb(40, 180, 90));
  HUXERUI_CHECK(first_border != nullptr);
  HUXERUI_CHECK(first_border->width == 3.0F);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Enter,
  });
  HUXERUI_CHECK(first_keyboard_clicks == 1);
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Up,
      .key = Key::Enter,
  });

  first_focus_enabled = false;
  const DisplayList &disabled_first = runtime.BuildFrame();
  HUXERUI_CHECK(focus_changes.back() == "first:off");
  const DrawTextCommand *first_text =
      FindText(disabled_first, "first");
  HUXERUI_CHECK(first_text != nullptr);
  HUXERUI_CHECK(
      std::abs(first_text->color.alpha - 0.3F) < 0.001F);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.BuildFrame();
  HUXERUI_CHECK(focus_changes.back() == "third:on");

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Space,
  });
  HUXERUI_CHECK(third_keyboard_clicks == 0);
  const DisplayList &space_down = runtime.BuildFrame();
  HUXERUI_CHECK(
      FindRectWithColor(
          space_down,
          huxerui::FlatLightThemeSpec()
              .interactions.pressed_overlay) != nullptr);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Up,
      .key = Key::Space,
  });
  HUXERUI_CHECK(third_keyboard_clicks == 1);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::ArrowRight,
  });
  HUXERUI_CHECK(received_keys.size() == 1);
  HUXERUI_CHECK(received_keys.front() == Key::ArrowRight);
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Enter,
  });
  HUXERUI_CHECK(custom_keyboard_clicks == 1);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
      .modifiers = {
          .shift = true,
      },
  });
  runtime.BuildFrame();
  HUXERUI_CHECK(focus_changes.back() == "third:on");
}

void TestPointerFocusDoesNotPaintFocusRing() {
  focus_changes.clear();

  TestPlatform platform;
  Runtime runtime{FocusApp, platform};
  runtime.SetViewport({240.0F, 180.0F});
  const DisplayList &initial = runtime.BuildFrame();
  const DrawTextCommand *first = FindText(initial, "first");
  HUXERUI_CHECK(first != nullptr);
  const Point pointer{
      first->rect.x + first->rect.width * 0.5F,
      first->rect.y + first->rect.height * 0.5F,
  };

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      104,
      pointer,
  });
  const DisplayList &pointer_focused = runtime.BuildFrame();
  HUXERUI_CHECK(focus_changes.size() == 1);
  HUXERUI_CHECK(focus_changes.back() == "first:on");
  HUXERUI_CHECK(
      FindBorderWithColor(
          pointer_focused, Color::Rgb(40, 180, 90)) ==
      nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      104,
      pointer,
  });
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  const DisplayList &keyboard_focused = runtime.BuildFrame();
  HUXERUI_CHECK(focus_changes.back() == "third:on");
  HUXERUI_CHECK(
      FindBorderWithColor(
          keyboard_focused, Color::Rgb(40, 180, 90)) !=
      nullptr);
}

void TestModalDialogTrapsAndRestoresFocusTraversal() {
  saved_dialogs.reset();
  background_dialog_clicks = 0;
  first_dialog_clicks = 0;
  second_dialog_clicks = 0;

  huxerui::AppOptions options;
  options.root_hooks = {
      huxerui::InstallDialogs(),
  };
  TestPlatform platform;
  Runtime runtime{
      FocusDialogApp, platform, std::move(options)};
  runtime.SetViewport({240.0F, 160.0F});
  runtime.BuildFrame();

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.BuildFrame();

  const LayerId dialog = saved_dialogs->Show(
      [] {
        return Column{
            Button("first dialog focus")
                .OnClick([] { ++first_dialog_clicks; }),
            Button("second dialog focus")
                .OnClick([] { ++second_dialog_clicks; }),
        };
      },
      huxerui::DialogOptions{false});
  runtime.BuildFrame();

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Enter,
  });
  HUXERUI_CHECK(first_dialog_clicks == 1);
  HUXERUI_CHECK(background_dialog_clicks == 0);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Enter,
  });
  HUXERUI_CHECK(second_dialog_clicks == 1);
  HUXERUI_CHECK(background_dialog_clicks == 0);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Enter,
  });
  HUXERUI_CHECK(first_dialog_clicks == 2);

  HUXERUI_CHECK(saved_dialogs->Dismiss(dialog));
  runtime.BuildFrame();
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Enter,
  });
  HUXERUI_CHECK(background_dialog_clicks == 1);
}

void TestRootHooksServicesAndLayers() {
  installed_root_service.reset();
  observed_root_service_value = 0;
  root_app_clicks = 0;

  huxerui::AppOptions options;
  options.root_hooks.push_back([](huxerui::RootContext &root) {
    installed_root_service = std::make_shared<TestRootService>(
        TestRootService{
            &root.Layers(),
            42,
        });
    root.Provide(installed_root_service);
  });

  TestPlatform platform;
  Runtime runtime{RootHookApp, platform, std::move(options)};
  runtime.SetViewport({200.0F, 100.0F});
  const DisplayList &initial = runtime.BuildFrame();
  HUXERUI_CHECK(observed_root_service_value == 42);
  HUXERUI_CHECK(ContainsText(initial, "application"));

  const LayerId toast = installed_root_service->layers->Attach(
      LayerKind::Toast, [] { return Text("toast"); });
  const DisplayList &with_toast = runtime.BuildFrame();
  HUXERUI_CHECK(ContainsText(with_toast, "application"));
  HUXERUI_CHECK(ContainsText(with_toast, "toast"));

  const LayerId modal = installed_root_service->layers->Attach(
      LayerKind::Modal, [] { return Text("modal"); });
  runtime.BuildFrame();
  ClickAt(runtime, {20.0F, 20.0F}, 82);
  HUXERUI_CHECK(root_app_clicks == 0);

  HUXERUI_CHECK(installed_root_service->layers->Dismiss(modal));
  runtime.BuildFrame();
  ClickAt(runtime, {20.0F, 20.0F}, 83);
  HUXERUI_CHECK(root_app_clicks == 1);

  HUXERUI_CHECK(installed_root_service->layers->Dismiss(toast));
  const DisplayList &dismissed = runtime.BuildFrame();
  HUXERUI_CHECK(!ContainsText(dismissed, "toast"));
}

void TestToastAndDialogPresentation() {
  saved_toast.reset();
  saved_dialogs.reset();
  saved_dialog_context.reset();

  huxerui::AppOptions options;
  options.root_hooks = {
      huxerui::InstallToast(),
      huxerui::InstallDialogs(),
  };

  TestPlatform platform;
  Runtime runtime{
      PresentationThemeApp, platform, std::move(options)};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();
  HUXERUI_CHECK(saved_toast.has_value());
  HUXERUI_CHECK(saved_dialogs.has_value());

  saved_toast->Show(
      "saved", huxerui::ToastOptions{0.5});
  const DisplayList &toast = runtime.BuildFrame();
  HUXERUI_CHECK(ContainsText(toast, "saved"));
  HUXERUI_CHECK(
      FindRectWithColor(
          toast, Color::Rgb(20, 30, 40, 0.9F)) != nullptr);
  const DrawTextCommand *toast_text = FindText(toast, "saved");
  HUXERUI_CHECK(toast_text != nullptr);
  HUXERUI_CHECK(
      toast_text->color.green ==
      Color::Rgb(240, 245, 250).green);
  platform.AdvanceTime(0.5);
  runtime.BuildFrame();
  const DisplayList &expired = runtime.BuildFrame();
  HUXERUI_CHECK(!ContainsText(expired, "saved"));

  const LayerId dialog = saved_dialogs->Show(
      [] { return Text("command dialog"); },
      huxerui::DialogOptions{false});
  const DisplayList &shown = runtime.BuildFrame();
  HUXERUI_CHECK(ContainsText(shown, "command dialog"));
  const DrawRectCommand *scrim = FindRect(
      shown, Rect{0.0F, 0.0F, 200.0F, 100.0F});
  HUXERUI_CHECK(scrim != nullptr);
  HUXERUI_CHECK(
      scrim->color.red == Color::Rgb(180, 20, 20, 0.3F).red);
  HUXERUI_CHECK(scrim->color.alpha == 0.3F);
  HUXERUI_CHECK(saved_dialogs->Dismiss(dialog));
  const DisplayList &dismissed = runtime.BuildFrame();
  HUXERUI_CHECK(!ContainsText(dismissed, "command dialog"));

  const LayerId contextual_dialog = saved_dialogs->Show(
      [](DialogContext dialog_context) {
        saved_dialog_context = dialog_context;
        return Text("context dialog");
      },
      huxerui::DialogOptions{false});
  const DisplayList &contextual = runtime.BuildFrame();
  HUXERUI_CHECK(ContainsText(contextual, "context dialog"));
  HUXERUI_CHECK(saved_dialog_context.has_value());
  HUXERUI_CHECK(
      saved_dialog_context->Id() == contextual_dialog);

  saved_dialog_context.reset();
  HUXERUI_CHECK(saved_dialogs->Update(
      contextual_dialog,
      [](DialogContext dialog_context) {
        saved_dialog_context = dialog_context;
        return Text("updated context dialog");
      }));
  const DisplayList &updated_contextual = runtime.BuildFrame();
  HUXERUI_CHECK(
      ContainsText(updated_contextual, "updated context dialog"));
  HUXERUI_CHECK(saved_dialog_context.has_value());
  HUXERUI_CHECK(
      saved_dialog_context->Id() == contextual_dialog);
  HUXERUI_CHECK(saved_dialog_context->Dismiss());
  const DisplayList &context_dismissed = runtime.BuildFrame();
  HUXERUI_CHECK(
      !ContainsText(context_dismissed, "updated context dialog"));

  const LayerId outside_dialog = saved_dialogs->Show(
      [] { return Text("outside dismiss dialog"); });
  const DisplayList &outside_shown = runtime.BuildFrame();
  HUXERUI_CHECK(
      ContainsText(outside_shown, "outside dismiss dialog"));
  ClickAt(runtime, {1.0F, 1.0F}, 85);
  const DisplayList &outside_dismissed = runtime.BuildFrame();
  HUXERUI_CHECK(
      !ContainsText(outside_dismissed, "outside dismiss dialog"));
  HUXERUI_CHECK(!saved_dialogs->Dismiss(outside_dialog));
}

void TestFlatDarkPresentationStyles() {
  saved_toast.reset();
  saved_dialogs.reset();

  huxerui::AppOptions options;
  options.root_hooks = {
      huxerui::InstallToast(),
      huxerui::InstallDialogs(),
  };

  TestPlatform platform;
  Runtime runtime{
      FlatDarkPresentationApp, platform, std::move(options)};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();

  const ThemeSpec dark = huxerui::FlatDarkThemeSpec();
  Color toast_background = dark.colors.on_surface;
  toast_background.alpha *= 0.94F;
  saved_toast->Show(
      "dark toast", huxerui::ToastOptions{10.0});
  const DisplayList &toast = runtime.BuildFrame();
  HUXERUI_CHECK(
      FindRectWithColor(toast, toast_background) != nullptr);
  const DrawTextCommand *toast_text =
      FindText(toast, "dark toast");
  HUXERUI_CHECK(toast_text != nullptr);
  HUXERUI_CHECK(
      toast_text->color.red == dark.colors.surface.red);

  saved_dialogs->Show(
      [] { return Text("dark dialog"); },
      huxerui::DialogOptions{false});
  const DisplayList &dialog = runtime.BuildFrame();
  const DrawRectCommand *scrim = FindRect(
      dialog, Rect{0.0F, 0.0F, 200.0F, 100.0F});
  HUXERUI_CHECK(scrim != nullptr);
  HUXERUI_CHECK(
      scrim->color.alpha == dark.colors.scrim.alpha);
}

void TestDeclarativeDialogModifier() {
  huxerui::AppOptions options;
  options.root_hooks = {
      huxerui::InstallDialogs(),
  };

  TestPlatform platform;
  Runtime runtime{
      DeclarativeDialogApp, platform, std::move(options)};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();

  declarative_dialog_visible = true;
  runtime.BuildFrame();
  const DisplayList &shown = runtime.BuildFrame();
  HUXERUI_CHECK(ContainsText(shown, "declarative dialog"));

  ClickAt(runtime, {1.0F, 1.0F}, 84);
  HUXERUI_CHECK(!declarative_dialog_visible);
  runtime.BuildFrame();
  const DisplayList &hidden = runtime.BuildFrame();
  HUXERUI_CHECK(!ContainsText(hidden, "declarative dialog"));
}

void TestAnimatedOffsetAndOpacityModifiers() {
  TestPlatform platform;
  Runtime runtime{AnimationApp, platform};
  runtime.SetViewport({240.0F, 100.0F});
  runtime.BuildFrame();

  animation_target = true;
  runtime.BuildFrame();
  platform.AdvanceTime(0.5);
  const DisplayList &middle = runtime.BuildFrame();

  const DrawTextCommand *animated = nullptr;
  for (const auto &command : middle.Commands()) {
    if (const auto *text =
            std::get_if<DrawTextCommand>(&command);
        text && text->text == "animated") {
      animated = text;
      break;
    }
  }
  HUXERUI_CHECK(animated != nullptr);
  HUXERUI_CHECK(std::abs(animated->rect.x - 50.0F) < 0.01F);
  HUXERUI_CHECK(std::abs(animated->color.alpha - 0.5F) < 0.01F);

  platform.AdvanceTime(0.5);
  const DisplayList &finished = runtime.BuildFrame();
  HUXERUI_CHECK(!ContainsText(finished, "animated"));
}

void TestClickIndicationUsesPointerObservation() {
  indication_clicks = 0;
  TestPlatform platform;
  Runtime runtime{IndicationApp, platform};
  runtime.SetViewport({160.0F, 80.0F});
  runtime.BuildFrame();

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      91,
      {20.0F, 20.0F},
  });
  runtime.BuildFrame();
  platform.AdvanceTime(0.04);
  const DisplayList &pressed = runtime.BuildFrame();

  std::size_t rectangles = 0;
  for (const auto &command : pressed.Commands()) {
    if (std::holds_alternative<DrawRectCommand>(command)) {
      ++rectangles;
    }
  }
  HUXERUI_CHECK(rectangles >= 2);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      91,
      {20.0F, 20.0F},
  });
  HUXERUI_CHECK(indication_clicks == 1);
}

void TestModifierPresentationGeometry() {
  TestPlatform platform;
  Runtime runtime{PresentedIndicationApp, platform};
  runtime.SetViewport({160.0F, 80.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->children.size() == 1);
  const auto *button = root->children[0].get();
  HUXERUI_CHECK(
      std::abs(button->PresentationFrame().x - 50.0F) < 0.01F);
  HUXERUI_CHECK(
      std::abs(button->PresentationOpacity() - 0.5F) < 0.01F);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      92,
      {60.0F, 20.0F},
  });
  runtime.BuildFrame();
  platform.AdvanceTime(0.04);
  const DisplayList &pressed = runtime.BuildFrame();

  std::size_t presented_rectangles = 0;
  for (const auto &command : pressed.Commands()) {
    if (const auto *rectangle =
            std::get_if<DrawRectCommand>(&command);
        rectangle &&
        std::abs(rectangle->rect.x - 50.0F) < 0.01F) {
      ++presented_rectangles;
      HUXERUI_CHECK(rectangle->color.alpha <= 0.5F);
    }
  }
  HUXERUI_CHECK(presented_rectangles >= 2);
}

void TestExplicitIndicationOverridesAutomaticDefault() {
  indication_clicks = 0;
  TestPlatform platform;
  Runtime runtime{ExplicitIndicationApp, platform};
  runtime.SetViewport({160.0F, 80.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->modifiers.size() == 1);
  HUXERUI_CHECK(
      huxerui::detail::IsExplicitIndicationDescriptor(
          root->modifiers[0].descriptor));

  ClickAt(runtime, {20.0F, 20.0F}, 93);
  HUXERUI_CHECK(indication_clicks == 1);
}

void TestModifierFrameSubtreeCache() {
  TestPlatform platform;
  Runtime runtime{ModifierPruningApp, platform};
  runtime.SetViewport({160.0F, 80.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->subtree_has_mounted_modifiers);
  HUXERUI_CHECK(root->children.size() == 2);
  HUXERUI_CHECK(
      !root->children[0]->subtree_has_mounted_modifiers);
  HUXERUI_CHECK(
      root->children[1]->subtree_has_mounted_modifiers);

  show_modifier_branch = false;
  runtime.BuildFrame();
  root = runtime.RootNode();
  HUXERUI_CHECK(root->children.size() == 1);
  HUXERUI_CHECK(!root->subtree_has_mounted_modifiers);
}

void TestScopeStateIsolation() {
  TestPlatform platform;
  Runtime runtime{ScopedCountersApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->children.size() == 2);
  HUXERUI_CHECK(root->children[0]->children[0]->children[0]->text == "0");
  HUXERUI_CHECK(root->children[1]->children[0]->children[0]->text == "0");

  InvokeClick(*root->children[0]->children[0]->children[1]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->children[0]->children[0]->children[0]->text == "1");
  HUXERUI_CHECK(root->children[1]->children[0]->children[0]->text == "0");
}

void TestStatePassedIntoScope() {
  TestPlatform platform;
  Runtime runtime{SharedStateApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->children[0]->children[0]->text == "7");

  InvokeClick(*root->children[1]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->children[0]->children[0]->text == "8");
}

void TestKeyedScopeIdentity() {
  TestPlatform platform;
  Runtime runtime{KeyedScopesApp, platform};
  runtime.SetViewport({320.0F, 320.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  const std::uint64_t first_scope_identity = root->children[0]->identity;

  InvokeClick(*root->children[0]->children[0]->children[1]);
  runtime.BuildFrame();
  root = runtime.RootNode();
  HUXERUI_CHECK(root->children[0]->children[0]->children[0]->text == "1");

  InvokeClick(*root->children[2]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->children[1]->identity == first_scope_identity);
  HUXERUI_CHECK(root->children[1]->children[0]->children[0]->text == "1");
  HUXERUI_CHECK(root->children[0]->children[0]->children[0]->text == "0");
}

void TestDuplicateSiblingKeys() {
  TestPlatform platform;
  Runtime runtime{DuplicateKeyApp, platform};
  runtime.SetViewport({320.0F, 240.0F});

  bool rejected = false;
  try {
    runtime.BuildFrame();
  } catch (const std::logic_error &) {
    rejected = true;
  }
  HUXERUI_CHECK(rejected);
}

void TestRepeatedUseStateCallSite() {
  TestPlatform platform;
  Runtime runtime{RepeatedUseStateApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->children.size() == 3);
  HUXERUI_CHECK(root->children[0]->text == "0");
  HUXERUI_CHECK(root->children[1]->text == "0");
  HUXERUI_CHECK(root->children[2]->text == "0");

  InvokeClick(*root->children[1]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->children[0]->text == "0");
  HUXERUI_CHECK(root->children[1]->text == "1");
  HUXERUI_CHECK(root->children[2]->text == "0");
}

void TestLocalScopeRecomposition() {
  local_root_compositions = 0;
  left_scope_compositions = 0;
  right_scope_compositions = 0;

  TestPlatform platform;
  Runtime runtime{LocalRecompositionApp, platform};
  runtime.SetViewport({320.0F, 320.0F});
  runtime.BuildFrame();

  HUXERUI_CHECK(local_root_compositions == 1);
  HUXERUI_CHECK(left_scope_compositions == 1);
  HUXERUI_CHECK(right_scope_compositions == 1);

  const auto *root = runtime.RootNode();
  const int requested_frames = platform.requested_frames;
  InvokeClick(*root->children[0]->children[0]->children[1]);
  InvokeClick(*root->children[0]->children[0]->children[1]);

  HUXERUI_CHECK(platform.requested_frames == requested_frames + 1);
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(local_root_compositions == 1);
  HUXERUI_CHECK(left_scope_compositions == 2);
  HUXERUI_CHECK(right_scope_compositions == 1);
  HUXERUI_CHECK(root->children[0]->children[0]->children[0]->text == "2");
  HUXERUI_CHECK(root->children[1]->children[0]->children[0]->text == "0");
}

void TestScopeReceivesUpdatedProps() {
  prop_root_compositions = 0;
  prop_scope_compositions = 0;

  TestPlatform platform;
  Runtime runtime{PropUpdateApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root->children[0]->children[0]->text == "3");
  HUXERUI_CHECK(prop_root_compositions == 1);
  HUXERUI_CHECK(prop_scope_compositions == 1);

  InvokeClick(*root->children[1]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->children[0]->children[0]->text == "4");
  HUXERUI_CHECK(prop_root_compositions == 2);
  HUXERUI_CHECK(prop_scope_compositions == 2);
}

void TestMainAndCrossAxisAlignment() {
  TestPlatform platform;
  Runtime runtime{AxisAlignmentApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->frame.width == 100.0F);
  HUXERUI_CHECK(root->frame.height == 100.0F);
  HUXERUI_CHECK(root->children[0]->frame.x == 40.0F);
  HUXERUI_CHECK(root->children[0]->frame.y == 0.0F);
  HUXERUI_CHECK(root->children[1]->frame.x == 40.0F);
  HUXERUI_CHECK(root->children[1]->frame.y == 80.0F);
}

void TestSpacerAndGrowLayout() {
  TestPlatform platform;
  Runtime runtime{SpacerLayoutApp, platform};
  runtime.SetViewport({200.0F, 60.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->children[0]->frame.x == 0.0F);
  HUXERUI_CHECK(root->children[0]->frame.y == 20.0F);
  HUXERUI_CHECK(root->children[1]->frame.x == 20.0F);
  HUXERUI_CHECK(root->children[1]->frame.width == 150.0F);
  HUXERUI_CHECK(root->children[2]->frame.x == 170.0F);
  HUXERUI_CHECK(root->children[2]->frame.y == 20.0F);

  Runtime grow_runtime{GrowLayoutApp, platform};
  grow_runtime.SetViewport({300.0F, 40.0F});
  grow_runtime.BuildFrame();

  root = grow_runtime.RootNode();
  HUXERUI_CHECK(root->children[0]->frame.width == 100.0F);
  HUXERUI_CHECK(root->children[1]->frame.x == 100.0F);
  HUXERUI_CHECK(root->children[1]->frame.width == 200.0F);
}

void TestStackAndStretchAlignment() {
  TestPlatform platform;
  Runtime stack_runtime{StackAlignmentApp, platform};
  stack_runtime.SetViewport({100.0F, 80.0F});
  stack_runtime.BuildFrame();

  const auto *root = stack_runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->children[0]->frame.x == 80.0F);
  HUXERUI_CHECK(root->children[0]->frame.y == 35.0F);

  Runtime stretch_runtime{StretchLayoutApp, platform};
  stretch_runtime.SetViewport({120.0F, 80.0F});
  stretch_runtime.BuildFrame();

  root = stretch_runtime.RootNode();
  HUXERUI_CHECK(root->children[0]->frame.width == 120.0F);
}

void TestWrappedTextMeasurement() {
  TestPlatform platform;
  Runtime runtime{WrappedTextApp, platform};
  runtime.SetViewport({40.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->children[0]->frame.width == 40.0F);
  HUXERUI_CHECK(root->children[0]->frame.height == 60.0F);
}

void TestForEachFlattensChildren() {
  TestPlatform platform;
  Runtime runtime{ForEachLayoutApp, platform};
  runtime.SetViewport({200.0F, 160.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->children.size() == 5);
  HUXERUI_CHECK(root->children[0]->text == "Header");
  HUXERUI_CHECK(root->children[1]->text == "First");
  HUXERUI_CHECK(root->children[2]->text == "Second");
  HUXERUI_CHECK(root->children[3]->text == "Third");
  HUXERUI_CHECK(root->children[4]->text == "Footer");
  HUXERUI_CHECK(root->children[0]->frame.y == 0.0F);
  HUXERUI_CHECK(root->children[1]->frame.y == 25.0F);
  HUXERUI_CHECK(root->children[2]->frame.y == 50.0F);
  HUXERUI_CHECK(root->children[3]->frame.y == 75.0F);
  HUXERUI_CHECK(root->children[4]->frame.y == 100.0F);
}

void TestForEachKeyedIdentity() {
  TestPlatform platform;
  Runtime runtime{ForEachIdentityApp, platform};
  runtime.SetViewport({320.0F, 320.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->children.size() == 3);
  const std::uint64_t first_identity = root->children[0]->identity;

  InvokeClick(*root->children[0]->children[0]->children[1]);
  runtime.BuildFrame();
  root = runtime.RootNode();
  HUXERUI_CHECK(root->children[0]->children[0]->children[0]->text == "1");

  InvokeClick(*root->children[2]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->children.size() == 4);
  HUXERUI_CHECK(root->children[2]->identity == first_identity);
  HUXERUI_CHECK(root->children[2]->children[0]->children[0]->text == "1");

  InvokeClick(*root->children[3]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->children.size() == 3);
  HUXERUI_CHECK(root->children[0]->identity == first_identity);
  HUXERUI_CHECK(root->children[0]->children[0]->children[0]->text == "1");
}

void TestReactiveStateApis() {
  State<int> empty;
  HUXERUI_CHECK(!empty.IsValid());

  TestPlatform platform;
  Runtime runtime{ReactiveStateApiApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->children.size() == 4);
  HUXERUI_CHECK(root->children[0]->text == "Taps 2");
  HUXERUI_CHECK(root->children[1]->text == "Alpha");
  HUXERUI_CHECK(root->children[2]->text == "Bravo");

  InvokeClick(*root->children[3]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->children.size() == 5);
  HUXERUI_CHECK(root->children[0]->text == "Taps 3");
  HUXERUI_CHECK(root->children[3]->text == "Charlie");
}

void TestScrollViewLayoutClipAndHitTest() {
  scroll_clicked.clear();

  TestPlatform platform;
  Runtime runtime{ScrollViewApp, platform};
  runtime.SetViewport({100.0F, 60.0F});
  const DisplayList &initial = runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->measured_size.width == 100.0F);
  HUXERUI_CHECK(root->measured_size.height == 60.0F);
  HUXERUI_CHECK(root->scroll_content_height == 120.0F);
  HUXERUI_CHECK(root->scroll_offset_y == 0.0F);
  HUXERUI_CHECK(root->children[0]->frame.y == 0.0F);

  int push_clips = 0;
  int pop_clips = 0;
  for (const auto &command : initial.Commands()) {
    push_clips += std::holds_alternative<PushClipCommand>(command) ? 1 : 0;
    pop_clips += std::holds_alternative<PopClipCommand>(command) ? 1 : 0;
  }
  HUXERUI_CHECK(push_clips == 1);
  HUXERUI_CHECK(pop_clips == 1);
  HUXERUI_CHECK(ContainsText(initial, "First"));
  HUXERUI_CHECK(ContainsText(initial, "Second"));
  HUXERUI_CHECK(!ContainsText(initial, "Third"));

  const int requested_frames = platform.requested_frames;
  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 30.0F},
      0.0F,
      45.0F,
  });
  HUXERUI_CHECK(platform.requested_frames == requested_frames + 1);
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_y == 45.0F);
  HUXERUI_CHECK(root->children[0]->frame.y == -45.0F);

  ClickAt(runtime, {50.0F, 50.0F});
  HUXERUI_CHECK(scroll_clicked == "Third");

  runtime.InvalidateRoot();
  runtime.BuildFrame();
  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_y == 45.0F);

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 30.0F},
      0.0F,
      100.0F,
  });
  runtime.BuildFrame();
  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_y == 60.0F);

  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();
  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_y == 20.0F);
}

void TestForEachStateSurvivesScrolling() {
  TestPlatform platform;
  Runtime runtime{StatefulForEachScrollApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  auto *first_button = root->children[0]->children[0]->children[0].get();
  for (int tap = 0; tap < 4; ++tap) {
    InvokeClick(*first_button);
  }
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->children[0]->children[0]->children[0]->text == "0:4");

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      1000.0F,
  });
  runtime.BuildFrame();
  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      -1000.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_y == 0.0F);
  HUXERUI_CHECK(root->children[0]->children[0]->children[0]->text == "0:4");
}

void TestVirtualListVirtualization() {
  virtual_item_factory_calls = 0;

  TestPlatform platform;
  Runtime runtime{VirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  const DisplayList &initial = runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->virtual_state->source.size == 1000);
  HUXERUI_CHECK(root->children.size() < root->virtual_state->source.size);
  const auto materialized_items = root->virtual_state->item_views.size();
  HUXERUI_CHECK(materialized_items == root->children.size());
  HUXERUI_CHECK(virtual_item_factory_calls ==
                static_cast<int>(materialized_items));
  HUXERUI_CHECK(!root->virtual_state->child_indices.empty());
  HUXERUI_CHECK(root->virtual_state->child_indices.front() == 0);
  HUXERUI_CHECK(root->scroll_content_height == 20000.0F);
  HUXERUI_CHECK(FirstText(initial) == "0");
  HUXERUI_CHECK(ContainsText(initial, "4"));
  HUXERUI_CHECK(!ContainsText(initial, "5"));

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      1000.0F,
  });
  const DisplayList &scrolled = runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_y == 1000.0F);
  HUXERUI_CHECK(root->virtual_state->child_indices.front() == 40);
  HUXERUI_CHECK(root->virtual_state->child_indices.back() == 65);
  HUXERUI_CHECK(!root->virtual_state->saved_state);
  HUXERUI_CHECK(virtual_item_factory_calls < 1000);
  HUXERUI_CHECK(FirstText(scrolled) == "50");
  HUXERUI_CHECK(ContainsText(scrolled, "54"));
  HUXERUI_CHECK(!ContainsText(scrolled, "55"));

  const std::size_t visible_position =
      50 - root->virtual_state->child_indices.front();
  HUXERUI_CHECK(root->children[visible_position]->frame.y == 0.0F);

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      50000.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_y == 19900.0F);
  HUXERUI_CHECK(root->virtual_state->child_indices.back() == 999);

  Runtime state_runtime{VirtualStateListApp, platform};
  state_runtime.SetViewport({100.0F, 40.0F});
  const DisplayList &state_list = state_runtime.BuildFrame();
  HUXERUI_CHECK(FirstText(state_list) == "7");
}

void TestVirtualListStateSurvivesCacheEviction() {
  TestPlatform platform;
  Runtime runtime{StatefulVirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  auto *first_button = root->children[0]->children[0].get();
  for (int tap = 0; tap < 4; ++tap) {
    InvokeClick(*first_button);
  }
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->children[0]->children[0]->text == "0:4");

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      1000.0F,
  });
  runtime.BuildFrame();
  root = runtime.RootNode();
  HUXERUI_CHECK(root->virtual_state->child_indices.front() > 0);
  HUXERUI_CHECK(root->children.size() < root->virtual_state->source.size);
  HUXERUI_CHECK(root->virtual_state->saved_state != nullptr);
  HUXERUI_CHECK(
      root->virtual_state->saved_state->keyed.contains(std::int64_t{0}));

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      -1000.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_y == 0.0F);
  HUXERUI_CHECK(root->virtual_state->child_indices.front() == 0);
  HUXERUI_CHECK(
      !root->virtual_state->saved_state ||
      !root->virtual_state->saved_state->keyed.contains(std::int64_t{0}));
  HUXERUI_CHECK(root->children[0]->children[0]->text == "0:4");
}

void TestVirtualListStateSurvivesKeyRemovalAndReinsertion() {
  TestPlatform platform;
  Runtime runtime{ReorderableStatefulVirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  auto *first_button = root->children[0]->children[0].get();
  for (int tap = 0; tap < 4; ++tap) {
    InvokeClick(*first_button);
  }
  runtime.BuildFrame();

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      1000.0F,
  });
  runtime.BuildFrame();
  HUXERUI_CHECK(runtime.RootNode()->virtual_state->saved_state->keyed.contains(
      std::int64_t{0}));

  virtual_reorder_items.Update([](auto &items) { items.erase(items.begin()); });
  runtime.BuildFrame();
  HUXERUI_CHECK(runtime.RootNode()->virtual_state->saved_state->keyed.contains(
      std::int64_t{0}));

  virtual_reorder_items.Update([](auto &items) { items.push_back(0); });
  runtime.BuildFrame();

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      5000.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  const auto found = std::find_if(
      root->children.begin(), root->children.end(), [](const auto &child) {
        return child->key == huxerui::detail::ViewKey{std::int64_t{0}};
      });
  HUXERUI_CHECK(found != root->children.end());
  HUXERUI_CHECK((*found)->children[0]->text == "0:4");
}

void TestVirtualListPrunesOutOfRangeIndexState() {
  TestPlatform platform;
  Runtime runtime{UnkeyedStatefulVirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      5000.0F,
  });
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  const auto position =
      std::find(root->virtual_state->child_indices.begin(),
                root->virtual_state->child_indices.end(), std::size_t{99});
  HUXERUI_CHECK(position != root->virtual_state->child_indices.end());
  const std::size_t child_index = static_cast<std::size_t>(
      position - root->virtual_state->child_indices.begin());
  InvokeClick(*root->children[child_index]->children[0]);
  runtime.BuildFrame();

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      -5000.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->virtual_state->saved_state != nullptr);
  HUXERUI_CHECK(
      root->virtual_state->saved_state->indexed.contains(std::size_t{99}));

  virtual_unkeyed_items.Update([](auto &items) { items.resize(10); });
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(
      !root->virtual_state->saved_state ||
      !root->virtual_state->saved_state->indexed.contains(std::size_t{99}));
}

void TestVariableVirtualListMeasurementAndAnchor() {
  TestPlatform platform;
  Runtime runtime{VariableVirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->children[0]->measured_size.height == 20.0F);
  HUXERUI_CHECK(root->children[1]->measured_size.height == 40.0F);
  HUXERUI_CHECK(root->children[2]->frame.y == 60.0F);

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      70.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_y == 70.0F);
  const auto item_two =
      std::find(root->virtual_state->child_indices.begin(),
                root->virtual_state->child_indices.end(), std::size_t{2});
  HUXERUI_CHECK(item_two != root->virtual_state->child_indices.end());
  std::size_t child_index = static_cast<std::size_t>(
      item_two - root->virtual_state->child_indices.begin());
  HUXERUI_CHECK(root->children[child_index]->frame.y == -10.0F);

  variable_height_expanded = true;
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_y == 90.0F);
  const auto anchored_item =
      std::find(root->virtual_state->child_indices.begin(),
                root->virtual_state->child_indices.end(), std::size_t{2});
  HUXERUI_CHECK(anchored_item != root->virtual_state->child_indices.end());
  child_index = static_cast<std::size_t>(
      anchored_item - root->virtual_state->child_indices.begin());
  HUXERUI_CHECK(root->children[child_index]->frame.y == -10.0F);
}

void TestVariableVirtualListRefinesEstimatedExtent() {
  TestPlatform platform;
  Runtime runtime{TinyVariableVirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->scroll_content_height == 1000.0F);
  HUXERUI_CHECK(root->virtual_state->child_indices.back() >= 99);
}

void TestFixedHorizontalVirtualListLayoutAndScrolling() {
  TestPlatform platform;
  Runtime runtime{FixedHorizontalVirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->kind == huxerui::detail::NodeKind::VirtualLayout);
  HUXERUI_CHECK(root->virtual_state->axis == Axis::Horizontal);
  HUXERUI_CHECK(root->scroll_content_width == 20000.0F);

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      1000.0F,
      0.0F,
  });
  const DisplayList &scrolled = runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_x == 1000.0F);
  HUXERUI_CHECK(root->virtual_state->child_indices.front() == 40);
  HUXERUI_CHECK(root->virtual_state->child_indices.back() == 65);
  HUXERUI_CHECK(FirstText(scrolled) == "50");
  const std::size_t visible_position =
      50 - root->virtual_state->child_indices.front();
  HUXERUI_CHECK(root->children[visible_position]->frame.x == 0.0F);
}

void TestVariableHorizontalVirtualListMeasurementAndScrolling() {
  TestPlatform platform;
  Runtime runtime{VariableHorizontalVirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->children[0]->measured_size.width == 20.0F);
  HUXERUI_CHECK(root->children[1]->measured_size.width == 40.0F);
  HUXERUI_CHECK(root->children[2]->frame.x == 60.0F);

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      70.0F,
      0.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_x == 70.0F);
  const auto item_two =
      std::find(root->virtual_state->child_indices.begin(),
                root->virtual_state->child_indices.end(), std::size_t{2});
  HUXERUI_CHECK(item_two != root->virtual_state->child_indices.end());
  const std::size_t child_index = static_cast<std::size_t>(
      item_two - root->virtual_state->child_indices.begin());
  HUXERUI_CHECK(root->children[child_index]->frame.x == -10.0F);
}

void TestHorizontalVirtualListStateSurvivesCacheEviction() {
  TestPlatform platform;
  Runtime runtime{StatefulHorizontalVirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  auto *first_button = root->children[0]->children[0].get();
  for (int tap = 0; tap < 4; ++tap) {
    InvokeClick(*first_button);
  }
  runtime.BuildFrame();

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      2000.0F,
      0.0F,
  });
  runtime.BuildFrame();
  root = runtime.RootNode();
  HUXERUI_CHECK(root->virtual_state->child_indices.front() > 0);
  HUXERUI_CHECK(root->virtual_state->saved_state != nullptr);
  HUXERUI_CHECK(
      root->virtual_state->saved_state->keyed.contains(std::int64_t{0}));

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      -2000.0F,
      0.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_x == 0.0F);
  HUXERUI_CHECK(root->children[0]->children[0]->text == "0:4");
}

void TestCustomVirtualLayoutProtocol() {
  TestPlatform platform;
  Runtime runtime{CustomVirtualLayoutApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  const DisplayList &initial = runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->kind == huxerui::detail::NodeKind::VirtualLayout);
  HUXERUI_CHECK(root->virtual_layout->type == typeid(TestVirtualStrip));
  HUXERUI_CHECK(root->children.size() == 5);
  HUXERUI_CHECK(root->virtual_state->child_indices.front() == 0);
  HUXERUI_CHECK(FirstText(initial) == "0");
  HUXERUI_CHECK(
      huxerui::detail::ResolveScrollBarGeometry(*root).has_value());

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      250.0F,
  });
  const DisplayList &scrolled = runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_y == 250.0F);
  HUXERUI_CHECK(root->virtual_state->child_indices.front() == 10);
  HUXERUI_CHECK(root->children.size() == 5);
  HUXERUI_CHECK(FirstText(scrolled) == "10");

  HUXERUI_CHECK(custom_virtual_scroll.ScrollToItem(std::size_t{20},
                                                   ScrollAlignment::Center));
  runtime.BuildFrame();
  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_y == 462.5F);
  const auto centered =
      std::find(root->virtual_state->child_indices.begin(),
                root->virtual_state->child_indices.end(), std::size_t{20});
  HUXERUI_CHECK(centered != root->virtual_state->child_indices.end());
  const std::size_t centered_position = static_cast<std::size_t>(
      centered - root->virtual_state->child_indices.begin());
  HUXERUI_CHECK(root->children[centered_position]->frame.y == 37.5F);
}

void TestCustomVirtualGridProtocol() {
  virtual_grid_factory_calls = 0;

  TestPlatform platform;
  Runtime runtime{CustomVirtualGridApp, platform};
  runtime.SetViewport({90.0F, 40.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->kind == huxerui::detail::NodeKind::VirtualLayout);
  HUXERUI_CHECK(root->virtual_layout->type == typeid(TestVirtualGrid));
  HUXERUI_CHECK(root->children.size() < root->virtual_state->source.size);

  const auto first =
      std::find(root->virtual_state->child_indices.begin(),
                root->virtual_state->child_indices.end(), std::size_t{0});
  const auto second =
      std::find(root->virtual_state->child_indices.begin(),
                root->virtual_state->child_indices.end(), std::size_t{1});
  HUXERUI_CHECK(first != root->virtual_state->child_indices.end());
  HUXERUI_CHECK(second != root->virtual_state->child_indices.end());
  const std::size_t first_position = static_cast<std::size_t>(
      first - root->virtual_state->child_indices.begin());
  const std::size_t second_position = static_cast<std::size_t>(
      second - root->virtual_state->child_indices.begin());
  HUXERUI_CHECK(root->children[first_position]->frame.width == 60.0F);
  HUXERUI_CHECK(root->children[second_position]->frame.x == 60.0F);
  HUXERUI_CHECK(root->children[second_position]->frame.width == 30.0F);

  auto *first_button = root->children[first_position]->children[0].get();
  for (int tap = 0; tap < 4; ++tap) {
    InvokeClick(*first_button);
  }
  runtime.BuildFrame();

  runtime.HandleScrollEvent(ScrollEvent{
      {45.0F, 20.0F},
      0.0F,
      200.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->virtual_state->saved_state != nullptr);
  HUXERUI_CHECK(
      root->virtual_state->saved_state->keyed.contains(std::uint64_t{0}));

  std::size_t anchor_index = root->virtual_state->source.size;
  for (std::size_t position = 0; position < root->children.size(); ++position) {
    if (root->children[position]->frame.y == 0.0F) {
      anchor_index = root->virtual_state->child_indices[position];
      break;
    }
  }
  HUXERUI_CHECK(anchor_index < root->virtual_state->source.size);
  const std::uint64_t identity = root->identity;

  runtime.SetViewport({60.0F, 40.0F});
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->identity == identity);
  const auto resized_anchor =
      std::find(root->virtual_state->child_indices.begin(),
                root->virtual_state->child_indices.end(), anchor_index);
  HUXERUI_CHECK(resized_anchor != root->virtual_state->child_indices.end());
  const std::size_t resized_position = static_cast<std::size_t>(
      resized_anchor - root->virtual_state->child_indices.begin());
  HUXERUI_CHECK(root->children[resized_position]->frame.y == 0.0F);

  runtime.HandleScrollEvent(ScrollEvent{
      {30.0F, 20.0F},
      0.0F,
      -10000.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  const auto restored =
      std::find(root->virtual_state->child_indices.begin(),
                root->virtual_state->child_indices.end(), std::size_t{0});
  HUXERUI_CHECK(restored != root->virtual_state->child_indices.end());
  const std::size_t restored_position = static_cast<std::size_t>(
      restored - root->virtual_state->child_indices.begin());
  HUXERUI_CHECK(root->children[restored_position]->frame.width == 60.0F);
  HUXERUI_CHECK(root->children[restored_position]->children[0]->text == "0:4");
  HUXERUI_CHECK(virtual_grid_factory_calls < 200);
}

void TestBuiltInVirtualGridLayoutStateAndResizeAnchor() {
  built_in_grid_factory_calls = 0;

  TestPlatform platform;
  Runtime runtime{BuiltInVirtualGridApp, platform};
  runtime.SetViewport({100.0F, 48.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->kind == huxerui::detail::NodeKind::VirtualLayout);
  HUXERUI_CHECK(root->virtual_layout->type == typeid(VirtualGrid));
  HUXERUI_CHECK(root->children.size() < root->virtual_state->source.size);

  const auto first =
      std::find(root->virtual_state->child_indices.begin(),
                root->virtual_state->child_indices.end(), std::size_t{0});
  const auto second =
      std::find(root->virtual_state->child_indices.begin(),
                root->virtual_state->child_indices.end(), std::size_t{1});
  HUXERUI_CHECK(first != root->virtual_state->child_indices.end());
  HUXERUI_CHECK(second != root->virtual_state->child_indices.end());
  const std::size_t first_position = static_cast<std::size_t>(
      first - root->virtual_state->child_indices.begin());
  const std::size_t second_position = static_cast<std::size_t>(
      second - root->virtual_state->child_indices.begin());
  HUXERUI_CHECK(root->children[first_position]->frame.width == 65.0F);
  HUXERUI_CHECK(root->children[second_position]->frame.x == 70.0F);
  HUXERUI_CHECK(root->children[second_position]->frame.width == 30.0F);

  auto *first_button = root->children[first_position]->children[0].get();
  for (int tap = 0; tap < 4; ++tap) {
    InvokeClick(*first_button);
  }
  runtime.BuildFrame();

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 24.0F},
      0.0F,
      240.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_y == 240.0F);
  HUXERUI_CHECK(root->virtual_state->saved_state != nullptr);
  HUXERUI_CHECK(
      root->virtual_state->saved_state->keyed.contains(std::uint64_t{0}));

  std::size_t anchor_index = root->virtual_state->source.size;
  for (std::size_t position = 0; position < root->children.size(); ++position) {
    if (root->children[position]->frame.y == 0.0F) {
      anchor_index = root->virtual_state->child_indices[position];
      break;
    }
  }
  HUXERUI_CHECK(anchor_index < root->virtual_state->source.size);
  const std::uint64_t identity = root->identity;

  runtime.SetViewport({65.0F, 48.0F});
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->identity == identity);
  const auto resized_anchor =
      std::find(root->virtual_state->child_indices.begin(),
                root->virtual_state->child_indices.end(), anchor_index);
  HUXERUI_CHECK(resized_anchor != root->virtual_state->child_indices.end());
  const std::size_t resized_position = static_cast<std::size_t>(
      resized_anchor - root->virtual_state->child_indices.begin());
  HUXERUI_CHECK(root->children[resized_position]->frame.y == 0.0F);

  runtime.HandleScrollEvent(ScrollEvent{
      {32.0F, 24.0F},
      0.0F,
      -10000.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  const auto restored =
      std::find(root->virtual_state->child_indices.begin(),
                root->virtual_state->child_indices.end(), std::size_t{0});
  HUXERUI_CHECK(restored != root->virtual_state->child_indices.end());
  const std::size_t restored_position = static_cast<std::size_t>(
      restored - root->virtual_state->child_indices.begin());
  HUXERUI_CHECK(root->children[restored_position]->frame.width == 65.0F);
  HUXERUI_CHECK(root->children[restored_position]->children[0]->text == "0:4");
  HUXERUI_CHECK(built_in_grid_factory_calls < 200);

  bool invalid_columns_rejected = false;
  try {
    static_cast<void>(GridColumns::Fixed(0));
  } catch (const std::invalid_argument &) {
    invalid_columns_rejected = true;
  }
  HUXERUI_CHECK(invalid_columns_rejected);
}

void TestVariableVirtualGridMeasurementAndAnchor() {
  TestPlatform platform;
  Runtime runtime{VariableVirtualGridApp, platform};
  runtime.SetViewport({100.0F, 60.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  const auto item_two =
      std::find(root->virtual_state->child_indices.begin(),
                root->virtual_state->child_indices.end(), std::size_t{2});
  HUXERUI_CHECK(item_two != root->virtual_state->child_indices.end());
  std::size_t item_two_position = static_cast<std::size_t>(
      item_two - root->virtual_state->child_indices.begin());
  HUXERUI_CHECK(root->children[item_two_position]->frame.y == 45.0F);

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 30.0F},
      0.0F,
      80.0F,
  });
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_y == 80.0F);
  const auto item_four =
      std::find(root->virtual_state->child_indices.begin(),
                root->virtual_state->child_indices.end(), std::size_t{4});
  HUXERUI_CHECK(item_four != root->virtual_state->child_indices.end());
  std::size_t item_four_position = static_cast<std::size_t>(
      item_four - root->virtual_state->child_indices.begin());
  HUXERUI_CHECK(root->children[item_four_position]->frame.y == 0.0F);

  variable_grid_height_expanded = true;
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_y == 100.0F);
  const auto anchored_item =
      std::find(root->virtual_state->child_indices.begin(),
                root->virtual_state->child_indices.end(), std::size_t{4});
  HUXERUI_CHECK(anchored_item != root->virtual_state->child_indices.end());
  item_four_position = static_cast<std::size_t>(
      anchored_item - root->virtual_state->child_indices.begin());
  HUXERUI_CHECK(root->children[item_four_position]->frame.y == 0.0F);
}

void TestScrollStateControlsVirtualListAndDisconnects() {
  scroll_observer_compositions = 0;

  TestPlatform platform;
  Runtime runtime{ControlledVirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(controlled_list_scroll.IsConnected());
  HUXERUI_CHECK(root->scroll_offset_y == 40.0F);
  HUXERUI_CHECK(controlled_list_scroll.Offset() == 40.0F);
  HUXERUI_CHECK(controlled_list_scroll.MaxOffset() == 19900.0F);
  HUXERUI_CHECK(controlled_list_scroll.ViewportExtent() == 100.0F);
  HUXERUI_CHECK(controlled_list_scroll.ContentExtent() == 20000.0F);

  const int compositions_before_scroll = scroll_observer_compositions;
  HUXERUI_CHECK(controlled_list_scroll.ScrollBy(20.0F));
  runtime.BuildFrame();
  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_y == 60.0F);
  HUXERUI_CHECK(controlled_list_scroll.Offset() == 60.0F);
  HUXERUI_CHECK(scroll_observer_compositions > compositions_before_scroll);

  HUXERUI_CHECK(controlled_list_scroll.ScrollToItem(std::size_t{50},
                                                    ScrollAlignment::Center));
  runtime.BuildFrame();
  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_y == 960.0F);
  const auto centered =
      std::find(root->virtual_state->child_indices.begin(),
                root->virtual_state->child_indices.end(), std::size_t{50});
  HUXERUI_CHECK(centered != root->virtual_state->child_indices.end());
  const std::size_t centered_position = static_cast<std::size_t>(
      centered - root->virtual_state->child_indices.begin());
  HUXERUI_CHECK(root->children[centered_position]->frame.y == 40.0F);

  HUXERUI_CHECK(controlled_list_scroll.ScrollTo(0.0F));
  runtime.BuildFrame();
  HUXERUI_CHECK(runtime.RootNode()->scroll_offset_y == 0.0F);

  show_controlled_scroll = false;
  runtime.BuildFrame();
  HUXERUI_CHECK(!controlled_list_scroll.IsConnected());
  HUXERUI_CHECK(!controlled_list_scroll.ScrollTo(100.0F));
}

void TestScrollStateExampleButtonsAndFollowUpFrame() {
  TestPlatform platform;
  Runtime runtime{ScrollStateExampleApp, platform};
  runtime.SetViewport({640.0F, 560.0F});
  const int frames_before_build = platform.requested_frames;
  runtime.BuildFrame();

  HUXERUI_CHECK(platform.requested_frames == frames_before_build + 1);
  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->children.size() == 2);
  HUXERUI_CHECK(root->children[0]->children.size() == 1);
  const auto *toolbar = root->children[0]->children[0].get();
  HUXERUI_CHECK(toolbar->children.size() == 4);
  const auto *item_button = toolbar->children[1].get();

  ClickAt(runtime,
          {
              item_button->frame.x + item_button->frame.width * 0.5F,
              item_button->frame.y + item_button->frame.height * 0.5F,
          });
  HUXERUI_CHECK(example_scroll.Offset() > 0.0F);
  runtime.BuildFrame();
  HUXERUI_CHECK(runtime.RootNode()->children[1]->scroll_offset_y > 0.0F);

  root = runtime.RootNode();
  toolbar = root->children[0]->children[0].get();
  const auto *top_button = toolbar->children[0].get();
  ClickAt(runtime,
          {
              top_button->frame.x + top_button->frame.width * 0.5F,
              top_button->frame.y + top_button->frame.height * 0.5F,
          });
  HUXERUI_CHECK(example_scroll.Offset() == 0.0F);
  HUXERUI_CHECK(runtime.RootNode()->children[1]->scroll_offset_y == 0.0F);
}

void TestScrollStateControlsVirtualGridItems() {
  TestPlatform platform;
  Runtime runtime{ControlledVirtualGridApp, platform};
  runtime.SetViewport({90.0F, 48.0F});
  runtime.BuildFrame();

  HUXERUI_CHECK(controlled_grid_scroll.IsConnected());
  HUXERUI_CHECK(controlled_grid_scroll.ScrollToItem(std::size_t{50}));
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_y == 384.0F);
  const auto item =
      std::find(root->virtual_state->child_indices.begin(),
                root->virtual_state->child_indices.end(), std::size_t{50});
  HUXERUI_CHECK(item != root->virtual_state->child_indices.end());
  std::size_t item_position = static_cast<std::size_t>(
      item - root->virtual_state->child_indices.begin());
  HUXERUI_CHECK(root->children[item_position]->frame.y == 0.0F);

  HUXERUI_CHECK(controlled_grid_scroll.ScrollToItem(std::size_t{50},
                                                    ScrollAlignment::Center));
  runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->scroll_offset_y == 370.0F);
  const auto centered =
      std::find(root->virtual_state->child_indices.begin(),
                root->virtual_state->child_indices.end(), std::size_t{50});
  HUXERUI_CHECK(centered != root->virtual_state->child_indices.end());
  item_position = static_cast<std::size_t>(
      centered - root->virtual_state->child_indices.begin());
  HUXERUI_CHECK(root->children[item_position]->frame.y == 14.0F);
}

void TestScrollStateControlsScrollView() {
  TestPlatform platform;
  Runtime runtime{ControlledScrollViewApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(controlled_view_scroll.IsConnected());
  HUXERUI_CHECK(root->scroll_offset_y == 20.0F);
  HUXERUI_CHECK(controlled_view_scroll.Offset() == 20.0F);
  HUXERUI_CHECK(controlled_view_scroll.MaxOffset() == 300.0F);
  HUXERUI_CHECK(controlled_view_scroll.ViewportExtent() == 100.0F);
  HUXERUI_CHECK(controlled_view_scroll.ContentExtent() == 400.0F);

  HUXERUI_CHECK(controlled_view_scroll.ScrollBy(30.0F));
  runtime.BuildFrame();
  HUXERUI_CHECK(runtime.RootNode()->scroll_offset_y == 50.0F);
  HUXERUI_CHECK(controlled_view_scroll.Offset() == 50.0F);
  HUXERUI_CHECK(!controlled_view_scroll.ScrollToItem(std::size_t{3}));
}

void TestVirtualListAxisChangePreservesAnchorAndIdentity() {
  TestPlatform platform;
  Runtime runtime{AdaptiveAxisVirtualListApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  const std::uint64_t identity = root->identity;
  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      1000.0F,
  });
  runtime.BuildFrame();
  HUXERUI_CHECK(FirstText(runtime.BuildFrame()) == "50");

  horizontal_virtual_list = true;
  const DisplayList &horizontal = runtime.BuildFrame();

  root = runtime.RootNode();
  HUXERUI_CHECK(root->identity == identity);
  HUXERUI_CHECK(root->virtual_state->axis == Axis::Horizontal);
  HUXERUI_CHECK(root->scroll_offset_x == 1000.0F);
  HUXERUI_CHECK(root->virtual_state->child_indices.front() == 40);
  HUXERUI_CHECK(FirstText(horizontal) == "50");
  const std::size_t visible_position =
      50 - root->virtual_state->child_indices.front();
  HUXERUI_CHECK(root->children[visible_position]->frame.x == 0.0F);
}

void TestCustomLayoutProtocol() {
  TestPlatform platform;
  Runtime runtime{CustomLayoutApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  HUXERUI_CHECK(root->kind == huxerui::detail::NodeKind::Layout);
  HUXERUI_CHECK(root->layout->type == std::type_index(typeid(TestFlow)));
  HUXERUI_CHECK(root->children.size() == 3);
  HUXERUI_CHECK(root->children[0]->frame.x == 5.0F);
  HUXERUI_CHECK(root->children[0]->frame.y == 5.0F);
  HUXERUI_CHECK(root->children[1]->frame.x == 5.0F);
  HUXERUI_CHECK(root->children[1]->frame.y == 20.0F);
  HUXERUI_CHECK(root->children[2]->frame.x == 50.0F);
  HUXERUI_CHECK(root->children[2]->frame.y == 20.0F);
}

void TestBuiltInPointerEventsAndClickLifecycle() {
  received_pointer_events.clear();
  pointer_clicks = 0;

  TestPlatform platform;
  Runtime runtime{PointerInputApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      1,
      {50.0F, 20.0F},
  });
  HUXERUI_CHECK(received_pointer_events.size() == 1);
  HUXERUI_CHECK(received_pointer_events[0].type == PointerEventType::Up);
  HUXERUI_CHECK(pointer_clicks == 0);

  received_pointer_events.clear();
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      7,
      {50.0F, 20.0F},
  });
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      7,
      {150.0F, 80.0F},
  });
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      7,
      {150.0F, 80.0F},
  });
  HUXERUI_CHECK(received_pointer_events.size() == 3);
  HUXERUI_CHECK(received_pointer_events[0].type == PointerEventType::Down);
  HUXERUI_CHECK(received_pointer_events[1].type == PointerEventType::Move);
  HUXERUI_CHECK(received_pointer_events[2].type == PointerEventType::Up);
  HUXERUI_CHECK(received_pointer_events[2].pointer_id == 7);
  HUXERUI_CHECK(pointer_clicks == 0);

  received_pointer_events.clear();
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      8,
      {50.0F, 20.0F},
  });
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Cancel,
      8,
      {150.0F, 80.0F},
  });
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      8,
      {50.0F, 20.0F},
  });
  HUXERUI_CHECK(received_pointer_events.size() == 3);
  HUXERUI_CHECK(received_pointer_events[1].type == PointerEventType::Cancel);
  HUXERUI_CHECK(pointer_clicks == 0);

  received_pointer_events.clear();
  ClickAt(runtime, {50.0F, 20.0F}, 9);
  HUXERUI_CHECK(received_pointer_events.size() == 2);
  HUXERUI_CHECK(pointer_clicks == 1);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      10,
      {50.0F, 20.0F},
  });
  show_pointer_target = false;
  runtime.BuildFrame();
  const std::size_t events_before_release = received_pointer_events.size();
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      10,
      {50.0F, 20.0F},
  });
  HUXERUI_CHECK(received_pointer_events.size() == events_before_release);
  HUXERUI_CHECK(pointer_clicks == 1);
}

void TestPointerDragScrollingAndClickArbitration() {
  drag_item_clicks = 0;
  drag_item_cancels = 0;

  TestPlatform platform;
  Runtime runtime{DragScrollApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      20,
      {50.0F, 20.0F},
  });
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      20,
      {50.0F, 16.0F},
  });
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      20,
      {50.0F, 16.0F},
  });
  HUXERUI_CHECK(drag_item_clicks == 1);
  HUXERUI_CHECK(drag_item_cancels == 0);
  HUXERUI_CHECK(drag_scroll.Offset() == 0.0F);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      21,
      {50.0F, 20.0F},
  });
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      21,
      {50.0F, 30.0F},
  });
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      21,
      {50.0F, 30.0F},
  });
  HUXERUI_CHECK(drag_item_clicks == 2);
  HUXERUI_CHECK(drag_item_cancels == 0);
  HUXERUI_CHECK(drag_scroll.Offset() == 0.0F);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      22,
      {50.0F, 20.0F},
  });
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      22,
      {50.0F, 10.0F},
  });
  HUXERUI_CHECK(drag_scroll.Offset() == 10.0F);
  HUXERUI_CHECK(drag_item_cancels == 1);
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      22,
      {50.0F, 0.0F},
  });
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      22,
      {50.0F, 0.0F},
  });
  HUXERUI_CHECK(drag_scroll.Offset() == 20.0F);
  HUXERUI_CHECK(drag_item_clicks == 2);

  runtime.BuildFrame();
  HUXERUI_CHECK(runtime.RootNode()->scroll_offset_y == 20.0F);
}

void TestHorizontalPointerDragUsesDominantAxis() {
  TestPlatform platform;
  Runtime runtime{HorizontalDragScrollApp, platform};
  runtime.SetViewport({100.0F, 40.0F});
  runtime.BuildFrame();

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      30,
      {50.0F, 20.0F},
  });
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      30,
      {50.0F, 5.0F},
  });
  HUXERUI_CHECK(horizontal_drag_scroll.Offset() == 0.0F);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      30,
      {20.0F, 18.0F},
  });
  HUXERUI_CHECK(horizontal_drag_scroll.Offset() == 30.0F);
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      30,
      {20.0F, 18.0F},
  });

  runtime.BuildFrame();
  HUXERUI_CHECK(runtime.RootNode()->scroll_offset_x == 30.0F);
}

void TestNestedPointerDragPassesRemainingDelta() {
  TestPlatform platform;
  Runtime runtime{NestedDragScrollApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  HUXERUI_CHECK(nested_inner_scroll.ScrollTo(130.0F));
  runtime.BuildFrame();
  HUXERUI_CHECK(nested_inner_scroll.MaxOffset() == 140.0F);
  HUXERUI_CHECK(nested_outer_scroll.Offset() == 0.0F);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      40,
      {50.0F, 50.0F},
  });
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      40,
      {50.0F, 20.0F},
  });
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      40,
      {50.0F, 20.0F},
  });

  HUXERUI_CHECK(nested_inner_scroll.Offset() == 140.0F);
  HUXERUI_CHECK(nested_outer_scroll.Offset() == 20.0F);
  runtime.BuildFrame();
  HUXERUI_CHECK(runtime.RootNode()->scroll_offset_y == 20.0F);
}

void TestScrollBarGeometryRenderingAndDragging() {
  drag_item_clicks = 0;
  drag_item_cancels = 0;

  TestPlatform platform;
  Runtime vertical{DragScrollApp, platform};
  vertical.SetViewport({100.0F, 100.0F});
  const DisplayList &vertical_display = vertical.BuildFrame();

  const auto vertical_bar =
      huxerui::detail::ResolveScrollBarGeometry(*vertical.RootNode());
  HUXERUI_CHECK(vertical_bar.has_value());
  HUXERUI_CHECK(vertical_bar->axis == Axis::Vertical);
  HUXERUI_CHECK(vertical_bar->track.x == 91.0F);
  HUXERUI_CHECK(vertical_bar->track.y == 3.0F);
  HUXERUI_CHECK(vertical_bar->track.width == 6.0F);
  HUXERUI_CHECK(vertical_bar->track.height == 94.0F);
  HUXERUI_CHECK(vertical_bar->thumb.x == 91.0F);
  HUXERUI_CHECK(vertical_bar->thumb.y == 3.0F);
  HUXERUI_CHECK(vertical_bar->thumb.height == 24.0F);
  HUXERUI_CHECK(ContainsRect(vertical_display, vertical_bar->thumb));

  vertical.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      49,
      {94.0F, 80.0F},
  });
  vertical.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      49,
      {94.0F, 80.0F},
  });
  HUXERUI_CHECK(drag_scroll.Offset() == 0.0F);
  HUXERUI_CHECK(drag_item_clicks == 0);

  vertical.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      50,
      {94.0F, 10.0F},
  });
  vertical.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      50,
      {94.0F, 40.0F},
  });
  vertical.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      50,
      {94.0F, 40.0F},
  });
  HUXERUI_CHECK(std::abs(drag_scroll.Offset() - 1671.4286F) < 0.01F);
  HUXERUI_CHECK(drag_item_clicks == 0);
  HUXERUI_CHECK(drag_item_cancels == 0);
  vertical.BuildFrame();
  const auto moved_vertical_bar =
      huxerui::detail::ResolveScrollBarGeometry(*vertical.RootNode());
  HUXERUI_CHECK(moved_vertical_bar.has_value());
  HUXERUI_CHECK(std::abs(moved_vertical_bar->thumb.y - 33.0F) < 0.01F);

  Runtime horizontal{HorizontalDragScrollApp, platform};
  horizontal.SetViewport({100.0F, 40.0F});
  const DisplayList &horizontal_display = horizontal.BuildFrame();
  const auto horizontal_bar =
      huxerui::detail::ResolveScrollBarGeometry(*horizontal.RootNode());
  HUXERUI_CHECK(horizontal_bar.has_value());
  HUXERUI_CHECK(horizontal_bar->axis == Axis::Horizontal);
  HUXERUI_CHECK(horizontal_bar->track.x == 3.0F);
  HUXERUI_CHECK(horizontal_bar->track.y == 31.0F);
  HUXERUI_CHECK(horizontal_bar->track.width == 94.0F);
  HUXERUI_CHECK(horizontal_bar->track.height == 6.0F);
  HUXERUI_CHECK(horizontal_bar->thumb.width == 24.0F);
  HUXERUI_CHECK(ContainsRect(horizontal_display, horizontal_bar->thumb));

  horizontal.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      51,
      {10.0F, 34.0F},
  });
  horizontal.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      51,
      {40.0F, 34.0F},
  });
  horizontal.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      51,
      {40.0F, 34.0F},
  });
  HUXERUI_CHECK(
      std::abs(horizontal_drag_scroll.Offset() - 1671.4286F) < 0.01F);

  Runtime short_content{ShortScrollBarApp, platform};
  short_content.SetViewport({100.0F, 100.0F});
  short_content.BuildFrame();
  HUXERUI_CHECK(!huxerui::detail::ResolveScrollBarGeometry(
      *short_content.RootNode()));

  bool invalid_style_rejected = false;
  try {
    static_cast<void>(
        VirtualList(std::size_t{1},
                    [](std::size_t) { return Text("Item"); })
            .With(huxerui::ScrollBar{
                huxerui::ScrollBarStyle{
                    .thickness = 0.0F,
                },
            }));
  } catch (const std::invalid_argument &) {
    invalid_style_rejected = true;
  }
  HUXERUI_CHECK(invalid_style_rejected);

  Runtime themed{ThemedScrollBarApp, platform};
  themed.SetViewport({100.0F, 100.0F});
  themed.BuildFrame();
  const auto *themed_root = themed.RootNode();
  HUXERUI_CHECK(themed_root != nullptr);
  HUXERUI_CHECK(themed_root->children.size() == 1);
  const auto themed_bar =
      huxerui::detail::ResolveScrollBarGeometry(
          *themed_root->children.front());
  HUXERUI_CHECK(themed_bar.has_value());
  HUXERUI_CHECK(themed_bar->style.thickness == 9.0F);
  HUXERUI_CHECK(
      themed_bar->style.minimum_thumb_extent == 30.0F);
  HUXERUI_CHECK(themed_bar->style.corner_radius == 4.5F);

  Runtime dark{FlatDarkScrollBarApp, platform};
  dark.SetViewport({100.0F, 100.0F});
  dark.BuildFrame();
  const auto *dark_root = dark.RootNode();
  HUXERUI_CHECK(dark_root != nullptr);
  HUXERUI_CHECK(dark_root->children.size() == 1);
  const auto dark_bar =
      huxerui::detail::ResolveScrollBarGeometry(
          *dark_root->children.front());
  HUXERUI_CHECK(dark_bar.has_value());
  const ThemeSpec dark_theme = huxerui::FlatDarkThemeSpec();
  HUXERUI_CHECK(
      dark_bar->style.thumb_color.red ==
      dark_theme.colors.on_surface.red);
  HUXERUI_CHECK(dark_bar->style.thumb_color.alpha == 0.55F);
  HUXERUI_CHECK(
      dark_bar->style.fade_in_duration ==
      static_cast<float>(dark_theme.motion.fast));
}

void TestFrameClockAndScrollBarAutoHide() {
  huxerui::detail::AnimatedValue<float> animated{0.0F};
  animated.AnimateTo(1.0F, 1.0, 0.2);
  HUXERUI_CHECK(animated.IsRunning());
  HUXERUI_CHECK(animated.Advance(1.1));
  HUXERUI_CHECK(std::abs(animated.Value() - 0.875F) < 0.001F);
  HUXERUI_CHECK(!animated.Advance(1.21));
  HUXERUI_CHECK(animated.Value() == 1.0F);

  drag_item_clicks = 0;
  drag_item_cancels = 0;

  TestPlatform platform;
  Runtime runtime{DragScrollApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  const DisplayList &initial = runtime.BuildFrame();
  const auto geometry =
      huxerui::detail::ResolveScrollBarGeometry(*runtime.RootNode());
  HUXERUI_CHECK(geometry.has_value());
  HUXERUI_CHECK(ContainsRect(initial, geometry->thumb));
  HUXERUI_CHECK(!platform.requested_delays.empty());
  HUXERUI_CHECK(
      std::abs(platform.requested_delays.back() - 0.7) < 0.001);

  platform.AdvanceTime(0.7);
  runtime.BuildFrame();
  HUXERUI_CHECK(platform.requested_delays.back() == 0.0);

  platform.AdvanceTime(0.11);
  const DisplayList &fading = runtime.BuildFrame();
  const auto fading_alpha = RectAlpha(fading, geometry->thumb);
  HUXERUI_CHECK(fading_alpha.has_value());
  HUXERUI_CHECK(*fading_alpha > 0.0F);
  HUXERUI_CHECK(*fading_alpha < geometry->style.thumb_color.alpha);

  platform.AdvanceTime(0.11);
  const DisplayList &hidden = runtime.BuildFrame();
  HUXERUI_CHECK(!ContainsRect(hidden, geometry->thumb));

  ClickAt(runtime, {94.0F, 80.0F}, 60);
  HUXERUI_CHECK(drag_item_clicks == 1);

  runtime.HandleScrollEvent(ScrollEvent{
      {50.0F, 50.0F},
      0.0F,
      40.0F,
  });
  runtime.BuildFrame();
  platform.AdvanceTime(0.12);
  const DisplayList &shown = runtime.BuildFrame();
  const auto shown_geometry =
      huxerui::detail::ResolveScrollBarGeometry(*runtime.RootNode());
  HUXERUI_CHECK(shown_geometry.has_value());
  HUXERUI_CHECK(ContainsRect(shown, shown_geometry->thumb));

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      61,
      {94.0F, 50.0F},
  });
  runtime.BuildFrame();
  platform.AdvanceTime(2.0);
  const DisplayList &held = runtime.BuildFrame();
  HUXERUI_CHECK(ContainsRect(held, shown_geometry->thumb));

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Cancel,
      61,
      {120.0F, 50.0F},
  });
  runtime.BuildFrame();
  platform.AdvanceTime(0.7);
  runtime.BuildFrame();
  platform.AdvanceTime(0.22);
  const DisplayList &hidden_after_exit = runtime.BuildFrame();
  HUXERUI_CHECK(
      !ContainsRect(hidden_after_exit, shown_geometry->thumb));
}

void TestTypedScopeEvents() {
  received_event.clear();
  saved_event_emitter = {};

  TestPlatform platform;
  Runtime runtime{EventApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  const auto *source = root->children[0].get();
  const std::uint64_t source_identity = source->identity;
  const std::uint64_t scope_id = source->recompose_scope->Id();
  InvokeClick(*source->children[0]);
  HUXERUI_CHECK(received_event == "first:query");
  HUXERUI_CHECK(saved_event_emitter.IsConnected());

  event_mode = 1;
  runtime.BuildFrame();
  root = runtime.RootNode();
  source = root->children[0].get();
  HUXERUI_CHECK(source->identity == source_identity);
  HUXERUI_CHECK(source->recompose_scope->Id() == scope_id);
  InvokeClick(*source->children[0]);
  HUXERUI_CHECK(received_event == "second:query");

  event_mode = 2;
  runtime.BuildFrame();
  HUXERUI_CHECK(!saved_event_emitter.IsConnected());
  saved_event_emitter.Emit<SearchBoxEvents::Submitted>("ignored");
  HUXERUI_CHECK(received_event == "second:query");
}

void TestLayoutTypeParticipatesInIdentity() {
  TestPlatform platform;
  Runtime runtime{LayoutIdentityApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  HUXERUI_CHECK(root != nullptr);
  const std::uint64_t row_identity = root->identity;
  HUXERUI_CHECK(root->layout->type == std::type_index(typeid(Row)));

  use_column_layout = true;
  runtime.BuildFrame();
  root = runtime.RootNode();
  HUXERUI_CHECK(root->identity != row_identity);
  HUXERUI_CHECK(root->layout->type == std::type_index(typeid(Column)));
}

} // namespace

int main() {
  TestUseStateAndStateUpdate();
  TestLayoutAndHitTest();
  TestViewCopyOnWrite();
  TestModifierReconciliationAndCopyOnWrite();
  TestNestedEnvironmentValues();
  TestThemeProviderUpdatesNestedContent();
  TestFlatDarkThemeAndSemanticTextRoles();
  TestFlatThemeHoverAndPressedIndication();
  TestMaterialThemeDefinitionsAndIndication();
  TestControlledTogglesAndAnimation();
  TestProgressCircleDrawingStateAndAnimation();
  TestThemeDrivesHoverAndPressedIndication();
  TestEnabledInheritanceAndHitTestBlocking();
  TestFocusTraversalKeyboardAndThemeVisuals();
  TestPointerFocusDoesNotPaintFocusRing();
  TestModalDialogTrapsAndRestoresFocusTraversal();
  TestRootHooksServicesAndLayers();
  TestToastAndDialogPresentation();
  TestFlatDarkPresentationStyles();
  TestDeclarativeDialogModifier();
  TestAnimatedOffsetAndOpacityModifiers();
  TestClickIndicationUsesPointerObservation();
  TestModifierPresentationGeometry();
  TestExplicitIndicationOverridesAutomaticDefault();
  TestModifierFrameSubtreeCache();
  TestScopeStateIsolation();
  TestStatePassedIntoScope();
  TestKeyedScopeIdentity();
  TestDuplicateSiblingKeys();
  TestRepeatedUseStateCallSite();
  TestLocalScopeRecomposition();
  TestScopeReceivesUpdatedProps();
  TestMainAndCrossAxisAlignment();
  TestSpacerAndGrowLayout();
  TestStackAndStretchAlignment();
  TestWrappedTextMeasurement();
  TestForEachFlattensChildren();
  TestForEachKeyedIdentity();
  TestReactiveStateApis();
  TestScrollViewLayoutClipAndHitTest();
  TestForEachStateSurvivesScrolling();
  TestVirtualListVirtualization();
  TestVirtualListStateSurvivesCacheEviction();
  TestVirtualListStateSurvivesKeyRemovalAndReinsertion();
  TestVirtualListPrunesOutOfRangeIndexState();
  TestVariableVirtualListMeasurementAndAnchor();
  TestVariableVirtualListRefinesEstimatedExtent();
  TestFixedHorizontalVirtualListLayoutAndScrolling();
  TestVariableHorizontalVirtualListMeasurementAndScrolling();
  TestHorizontalVirtualListStateSurvivesCacheEviction();
  TestCustomVirtualLayoutProtocol();
  TestCustomVirtualGridProtocol();
  TestBuiltInVirtualGridLayoutStateAndResizeAnchor();
  TestVariableVirtualGridMeasurementAndAnchor();
  TestScrollStateControlsVirtualListAndDisconnects();
  TestScrollStateExampleButtonsAndFollowUpFrame();
  TestScrollStateControlsVirtualGridItems();
  TestScrollStateControlsScrollView();
  TestVirtualListAxisChangePreservesAnchorAndIdentity();
  TestCustomLayoutProtocol();
  TestBuiltInPointerEventsAndClickLifecycle();
  TestPointerDragScrollingAndClickArbitration();
  TestHorizontalPointerDragUsesDominantAxis();
  TestNestedPointerDragPassesRemainingDelta();
  TestScrollBarGeometryRenderingAndDragging();
  TestFrameClockAndScrollBarAutoHide();
  TestTypedScopeEvents();
  TestLayoutTypeParticipatesInIdentity();
  std::cout << "HuxerUI runtime tests passed\n";
  return 0;
}
