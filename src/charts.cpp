#include <huxerui/charts.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/modifier.h>
#include <huxerui/paint.h>
#include <huxerui/semantics.h>
#include <huxerui/text.h>
#include <huxerui/theme.h>

#include "internal.h"

namespace huxerui {
namespace {

constexpr float pi = std::numbers::pi_v<float>;

struct ResolvedChartDataPoint {
  std::string label;
  float value = 0.0F;
  Color color;

  bool operator==(const ResolvedChartDataPoint&) const = default;
};

Color WithAlpha(Color color, float alpha) {
  color.alpha *= alpha;
  return color;
}

std::string FormatValue(float value) {
  char buffer[48]{};
  const bool is_whole = std::abs(value - std::round(value)) < 0.005F;
  std::snprintf(buffer, sizeof(buffer), is_whole ? "%.0f" : "%.1f", static_cast<double>(value));
  return buffer;
}

std::string FormatPercentage(float value, float total) {
  if (total <= 0.0F) {
    return "0%";
  }
  return FormatValue(value / total * 100.0F) + "%";
}

void ValidateData(const std::vector<ChartDataPoint>& data) {
  if (data.empty()) {
    throw std::invalid_argument("HuxerUI chart requires at least one data point");
  }
  for (const ChartDataPoint& point : data) {
    if (point.label.empty()) {
      throw std::invalid_argument("HuxerUI chart data labels must not be empty");
    }
    if (!std::isfinite(point.value) || point.value < 0.0F) {
      throw std::invalid_argument("HuxerUI chart values must be finite and non-negative");
    }
  }
}

void ValidateBarOptions(const std::vector<ChartDataPoint>& data, const BarChartOptions& options) {
  if (options.maximum_value.has_value()) {
    if (!std::isfinite(*options.maximum_value) || *options.maximum_value <= 0.0F) {
      throw std::invalid_argument("HuxerUI BarChart maximum value must be finite and greater than zero");
    }
    const float data_maximum = std::ranges::max(data, {}, &ChartDataPoint::value).value;
    if (*options.maximum_value < data_maximum) {
      throw std::invalid_argument("HuxerUI BarChart maximum value must include every data point");
    }
  }
  if (options.grid_line_count == 0 || options.grid_line_count > 12) {
    throw std::invalid_argument("HuxerUI BarChart grid line count must be between 1 and 12");
  }
  if (!std::isfinite(options.bar_width_fraction) || options.bar_width_fraction <= 0.0F ||
      options.bar_width_fraction > 1.0F) {
    throw std::invalid_argument("HuxerUI BarChart bar width fraction must be in the range (0, 1]");
  }
  if (!std::isfinite(options.corner_radius) || options.corner_radius < 0.0F) {
    throw std::invalid_argument("HuxerUI BarChart corner radius must be finite and non-negative");
  }
  if (options.accessibility_label.empty()) {
    throw std::invalid_argument("HuxerUI BarChart accessibility label must not be empty");
  }
}

void ValidateDonutOptions(const std::vector<ChartDataPoint>& data, const DonutChartOptions& options) {
  float total = 0.0F;
  for (const ChartDataPoint& point : data) {
    total += point.value;
  }
  if (total <= 0.0F) {
    throw std::invalid_argument("HuxerUI DonutChart requires a positive data total");
  }
  if (!std::isfinite(options.inner_radius_fraction) || options.inner_radius_fraction < 0.2F ||
      options.inner_radius_fraction > 0.85F) {
    throw std::invalid_argument("HuxerUI DonutChart inner radius fraction must be in the range [0.2, 0.85]");
  }
  if (!std::isfinite(options.segment_gap_degrees) || options.segment_gap_degrees < 0.0F ||
      options.segment_gap_degrees > 12.0F) {
    throw std::invalid_argument("HuxerUI DonutChart segment gap must be between 0 and 12 degrees");
  }
  if (options.accessibility_label.empty()) {
    throw std::invalid_argument("HuxerUI DonutChart accessibility label must not be empty");
  }
}

std::vector<ResolvedChartDataPoint> ResolveData(std::vector<ChartDataPoint> data, const ColorScheme& colors) {
  const std::array<Color, 8> palette{
      colors.primary,
      Color::Rgb(16, 185, 129),
      Color::Rgb(245, 158, 11),
      Color::Rgb(139, 92, 246),
      Color::Rgb(14, 165, 233),
      Color::Rgb(236, 72, 153),
      colors.secondary,
      colors.error,
  };

  std::vector<ResolvedChartDataPoint> resolved;
  resolved.reserve(data.size());
  for (std::size_t index = 0; index < data.size(); ++index) {
    ChartDataPoint& point = data[index];
    resolved.push_back({std::move(point.label), point.value, point.color.value_or(palette[index % palette.size()])});
  }
  return resolved;
}

std::string AccessibilityValue(const std::vector<ResolvedChartDataPoint>& data) {
  std::string result;
  for (std::size_t index = 0; index < data.size(); ++index) {
    if (index != 0) {
      result += ", ";
    }
    result += data[index].label;
    result += ": ";
    result += FormatValue(data[index].value);
  }
  return result;
}

float BarMaximum(const std::vector<ResolvedChartDataPoint>& data, const BarChartOptions& options) {
  if (options.maximum_value.has_value()) {
    return *options.maximum_value;
  }
  const float maximum = std::ranges::max(data, {}, &ResolvedChartDataPoint::value).value;
  return maximum > 0.0F ? maximum * 1.1F : 1.0F;
}

struct BarChartGeometry {
  float left = 0.0F;
  float top = 0.0F;
  float plot_width = 0.0F;
  float plot_height = 0.0F;
  float maximum = 1.0F;
  float slot_width = 0.0F;
  float bar_width = 0.0F;
  float corner_radius = 0.0F;

  [[nodiscard]] bool IsValid() const noexcept {
    return plot_width > 0.0F && plot_height > 0.0F && slot_width > 0.0F;
  }

  [[nodiscard]] Rect PlotBounds() const noexcept {
    return {left, top, plot_width, plot_height};
  }

  [[nodiscard]] Rect BarBounds(std::size_t index, float value) const noexcept {
    const float center_x = left + slot_width * (static_cast<float>(index) + 0.5F);
    const float height = plot_height * std::clamp(value / maximum, 0.0F, 1.0F);
    return {
        center_x - bar_width * 0.5F,
        top + plot_height - height,
        bar_width,
        height,
    };
  }
};

BarChartGeometry ResolveBarChartGeometry(
    Size size, const std::vector<ResolvedChartDataPoint>& data, const BarChartOptions& options
) {
  if (size.width < 96.0F || size.height < 96.0F || data.empty()) {
    return {};
  }
  BarChartGeometry geometry;
  geometry.left = std::min(48.0F, size.width * 0.17F);
  geometry.top = 24.0F;
  geometry.plot_width = std::max(0.0F, size.width - geometry.left - 10.0F);
  geometry.plot_height = std::max(0.0F, size.height - geometry.top - 34.0F);
  geometry.maximum = BarMaximum(data, options);
  geometry.slot_width = geometry.plot_width / static_cast<float>(data.size());
  geometry.bar_width = std::max(2.0F, std::min(56.0F, geometry.slot_width * options.bar_width_fraction));
  geometry.corner_radius = std::min(options.corner_radius, geometry.bar_width * 0.5F);
  return geometry;
}

std::optional<std::size_t> BarIndexAt(Point position, const BarChartGeometry& geometry, std::size_t count) {
  if (!geometry.IsValid() || !geometry.PlotBounds().Contains(position)) {
    return std::nullopt;
  }
  const float slot = (position.x - geometry.left) / geometry.slot_width;
  return std::min(static_cast<std::size_t>(slot), count - 1);
}

void PaintBarChart(
    PaintContext& paint,
    Size size,
    const std::vector<ResolvedChartDataPoint>& data,
    const BarChartOptions& options,
    const ThemeSpec& theme
) {
  const BarChartGeometry geometry = ResolveBarChartGeometry(size, data, options);
  if (!geometry.IsValid()) {
    return;
  }

  const TextStyle axis_style{Font::System(10.0F), theme.colors.on_surface_variant};
  const TextStyle value_style{Font::System(11.0F).WithWeight(FontWeight::Medium), theme.colors.on_surface};
  const Color grid_color = WithAlpha(theme.colors.outline, 0.22F);

  for (std::size_t index = 0; index <= options.grid_line_count; ++index) {
    const float fraction = static_cast<float>(index) / static_cast<float>(options.grid_line_count);
    const float y = geometry.top + geometry.plot_height * (1.0F - fraction);
    paint.DrawRect({geometry.left, y, geometry.plot_width, 1.0F}, grid_color);
    paint.DrawText(
        {0.0F, y - 8.0F, geometry.left - 6.0F, 16.0F},
        FormatValue(geometry.maximum * fraction),
        axis_style,
        TextLayoutOptions{.align = TextAlign::Trailing, .wrap = TextWrap::NoWrap}
    );
  }

  for (std::size_t index = 0; index < data.size(); ++index) {
    const ResolvedChartDataPoint& point = data[index];
    const Rect bar = geometry.BarBounds(index, point.value);
    const float center_x = bar.x + bar.width * 0.5F;
    if (bar.height > 0.0F) {
      paint.DrawRect(bar, point.color, CornerRadii{geometry.corner_radius});
    }
    if (options.show_values) {
      paint.DrawText(
          {
              center_x - geometry.slot_width * 0.5F,
              std::max(0.0F, bar.y - 20.0F),
              geometry.slot_width,
              18.0F,
          },
          FormatValue(point.value),
          value_style,
          TextLayoutOptions{.align = TextAlign::Center, .wrap = TextWrap::NoWrap}
      );
    }
    paint.DrawText(
        {
            center_x - geometry.slot_width * 0.5F,
            geometry.top + geometry.plot_height + 8.0F,
            geometry.slot_width,
            20.0F,
        },
        point.label,
        axis_style,
        TextLayoutOptions{.align = TextAlign::Center, .wrap = TextWrap::NoWrap}
    );
  }
}

float DataTotal(const std::vector<ResolvedChartDataPoint>& data) {
  float total = 0.0F;
  for (const ResolvedChartDataPoint& point : data) {
    total += point.value;
  }
  return total;
}

struct DonutChartGeometry {
  bool wide = false;
  float chart_width = 0.0F;
  float chart_height = 0.0F;
  Point center;
  float outer_radius = 0.0F;
  float inner_radius = 0.0F;
  float stroke_width = 0.0F;
  float arc_radius = 0.0F;
  float total = 0.0F;
  float requested_gap = 0.0F;

  [[nodiscard]] bool IsValid() const noexcept {
    return outer_radius > 0.0F && stroke_width > 0.0F && total > 0.0F;
  }
};

struct DonutLegendGeometry {
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float row_height = 0.0F;

  [[nodiscard]] Rect RowBounds(std::size_t index) const noexcept {
    return {x, y + row_height * static_cast<float>(index), width, row_height};
  }
};

DonutChartGeometry ResolveDonutChartGeometry(
    Size size, const std::vector<ResolvedChartDataPoint>& data, const DonutChartOptions& options
) {
  if (size.width < 96.0F || size.height < 96.0F || data.empty()) {
    return {};
  }
  DonutChartGeometry geometry;
  geometry.wide = options.show_legend && size.width >= 360.0F;
  geometry.chart_width = options.show_legend && geometry.wide ? size.width * 0.58F : size.width;
  geometry.chart_height = options.show_legend && !geometry.wide ? size.height * 0.62F : size.height;
  const float extent = std::min(geometry.chart_width, geometry.chart_height);
  geometry.center = {geometry.chart_width * 0.5F, geometry.chart_height * 0.5F};
  geometry.outer_radius = extent * 0.40F;
  geometry.inner_radius = geometry.outer_radius * options.inner_radius_fraction;
  geometry.stroke_width = geometry.outer_radius - geometry.inner_radius;
  geometry.arc_radius = geometry.inner_radius + geometry.stroke_width * 0.5F;
  geometry.total = DataTotal(data);
  geometry.requested_gap = options.segment_gap_degrees * pi / 180.0F;
  return geometry;
}

DonutLegendGeometry ResolveDonutLegendGeometry(
    Size size, std::size_t count, const DonutChartGeometry& chart
) {
  if (count == 0) {
    return {};
  }
  DonutLegendGeometry legend;
  legend.x = chart.wide ? chart.chart_width + 8.0F : 8.0F;
  legend.width = std::max(0.0F, size.width - legend.x - 8.0F);
  const float available_height = chart.wide ? size.height - 16.0F : size.height * 0.38F - 8.0F;
  legend.row_height = std::clamp(available_height / static_cast<float>(count), 14.0F, 24.0F);
  const float height = legend.row_height * static_cast<float>(count);
  legend.y = chart.wide ? (size.height - height) * 0.5F : size.height - height - 4.0F;
  return legend;
}

std::optional<std::size_t> DonutIndexAt(
    Point position,
    Size size,
    const std::vector<ResolvedChartDataPoint>& data,
    const DonutChartOptions& options,
    const DonutChartGeometry& geometry
) {
  if (!geometry.IsValid()) {
    return std::nullopt;
  }

  if (options.show_legend) {
    const DonutLegendGeometry legend = ResolveDonutLegendGeometry(size, data.size(), geometry);
    for (std::size_t index = 0; index < data.size(); ++index) {
      if (legend.RowBounds(index).Contains(position)) {
        return index;
      }
    }
  }

  const float delta_x = position.x - geometry.center.x;
  const float delta_y = position.y - geometry.center.y;
  const float distance = std::hypot(delta_x, delta_y);
  if (distance < geometry.inner_radius || distance > geometry.outer_radius) {
    return std::nullopt;
  }

  float angle = std::atan2(delta_y, delta_x) + pi * 0.5F;
  if (angle < 0.0F) {
    angle += pi * 2.0F;
  }
  float segment_start = 0.0F;
  for (std::size_t index = 0; index < data.size(); ++index) {
    const float sweep = data[index].value / geometry.total * pi * 2.0F;
    const float gap = std::min(geometry.requested_gap, sweep * 0.4F);
    if (angle >= segment_start + gap * 0.5F && angle <= segment_start + sweep - gap * 0.5F) {
      return index;
    }
    segment_start += sweep;
  }
  return std::nullopt;
}

void PaintDonutLegend(
    PaintContext& paint,
    Size size,
    const std::vector<ResolvedChartDataPoint>& data,
    const DonutChartGeometry& chart,
    const ThemeSpec& theme
) {
  if (data.empty()) {
    return;
  }

  const DonutLegendGeometry legend = ResolveDonutLegendGeometry(size, data.size(), chart);
  const TextStyle label_style{
      Font::System(legend.row_height < 18.0F ? 10.0F : 12.0F),
      theme.colors.on_surface,
  };
  const TextStyle value_style{
      Font::System(legend.row_height < 18.0F ? 10.0F : 12.0F).WithWeight(FontWeight::Medium),
      theme.colors.on_surface_variant,
  };

  for (std::size_t index = 0; index < data.size(); ++index) {
    const ResolvedChartDataPoint& point = data[index];
    const Rect row = legend.RowBounds(index);
    const float marker = std::min(10.0F, row.height - 4.0F);
    paint.DrawRect(
        {row.x, row.y + (row.height - marker) * 0.5F, marker, marker},
        point.color,
        CornerRadii{marker * 0.3F}
    );
    const float value_width = std::min(64.0F, row.width * 0.32F);
    paint.DrawText(
        {
            row.x + marker + 8.0F,
            row.y,
            std::max(0.0F, row.width - marker - value_width - 12.0F),
            row.height,
        },
        point.label,
        label_style,
        TextLayoutOptions{.wrap = TextWrap::NoWrap}
    );
    paint.DrawText(
        {row.x + row.width - value_width, row.y, value_width, row.height},
        FormatValue(point.value),
        value_style,
        TextLayoutOptions{.align = TextAlign::Trailing, .wrap = TextWrap::NoWrap}
    );
  }
}

void PaintDonutChart(
    PaintContext& paint,
    Size size,
    const std::vector<ResolvedChartDataPoint>& data,
    const DonutChartOptions& options,
    const ThemeSpec& theme
) {
  const DonutChartGeometry geometry = ResolveDonutChartGeometry(size, data, options);
  if (!geometry.IsValid()) {
    return;
  }

  paint.DrawArc(
      geometry.center,
      geometry.arc_radius,
      -pi * 0.5F,
      pi * 2.0F,
      WithAlpha(theme.colors.outline, 0.14F),
      geometry.stroke_width
  );

  float start_angle = -pi * 0.5F;
  for (const ResolvedChartDataPoint& point : data) {
    const float full_sweep = point.value / geometry.total * pi * 2.0F;
    const float gap = std::min(geometry.requested_gap, full_sweep * 0.4F);
    const float visible_sweep = std::max(0.0F, full_sweep - gap);
    if (visible_sweep > 0.0F) {
      paint.DrawArc(
          geometry.center,
          geometry.arc_radius,
          start_angle + gap * 0.5F,
          visible_sweep,
          point.color,
          geometry.stroke_width
      );
    }
    start_angle += full_sweep;
  }

  const TextStyle total_style{
      Font::System(std::max(14.0F, geometry.outer_radius * 0.22F)).WithWeight(FontWeight::Bold),
      theme.colors.on_surface,
  };
  const TextStyle label_style{Font::System(11.0F), theme.colors.on_surface_variant};
  paint.DrawText(
      {
          geometry.center.x - geometry.inner_radius,
          geometry.center.y - 22.0F,
          geometry.inner_radius * 2.0F,
          24.0F,
      },
      FormatValue(geometry.total),
      total_style,
      TextLayoutOptions{.align = TextAlign::Center, .wrap = TextWrap::NoWrap}
  );
  if (!options.center_label.empty()) {
    paint.DrawText(
        {
            geometry.center.x - geometry.inner_radius,
            geometry.center.y + 3.0F,
            geometry.inner_radius * 2.0F,
            18.0F,
        },
        options.center_label,
        label_style,
        TextLayoutOptions{.align = TextAlign::Center, .wrap = TextWrap::NoWrap}
    );
  }

  if (options.show_legend) {
    PaintDonutLegend(paint, size, data, geometry, theme);
  }
}

void PaintHoverCard(
    PaintContext& paint,
    Size size,
    Point anchor,
    const ResolvedChartDataPoint& point,
    float total,
    const ThemeSpec& theme
) {
  const float width = std::min(168.0F, std::max(80.0F, size.width - 8.0F));
  const float height = 58.0F;
  float x = anchor.x + 12.0F;
  if (x + width > size.width - 4.0F) {
    x = anchor.x - width - 12.0F;
  }
  x = std::clamp(x, 4.0F, std::max(4.0F, size.width - width - 4.0F));
  float y = anchor.y - height - 12.0F;
  if (y < 4.0F) {
    y = anchor.y + 12.0F;
  }
  y = std::clamp(y, 4.0F, std::max(4.0F, size.height - height - 4.0F));

  const Rect card{x, y, width, height};
  paint.DrawShadow(card, Color::Rgb(0, 0, 0, 0.28F), {0.0F, 4.0F}, 10.0F, 0.0F, CornerRadii{8.0F});
  paint.DrawRect(card, theme.colors.inverse_surface, CornerRadii{8.0F});
  paint.DrawRect({x + 8.0F, y + 9.0F, 4.0F, height - 18.0F}, point.color, CornerRadii{2.0F});
  paint.DrawText(
      {x + 18.0F, y + 7.0F, width - 26.0F, 20.0F},
      point.label,
      TextStyle{Font::System(12.0F).WithWeight(FontWeight::SemiBold), theme.colors.inverse_on_surface},
      TextLayoutOptions{.wrap = TextWrap::NoWrap}
  );
  paint.DrawText(
      {x + 18.0F, y + 30.0F, width - 26.0F, 18.0F},
      FormatValue(point.value) + "  ·  " + FormatPercentage(point.value, total),
      TextStyle{Font::System(11.0F), WithAlpha(theme.colors.inverse_on_surface, 0.76F)},
      TextLayoutOptions{.wrap = TextWrap::NoWrap}
  );
}

enum class ChartHoverKind {
  Bar,
  Donut,
};

class ChartHoverExtension;

struct ChartHover {
  using Extension = ChartHoverExtension;

  ChartHoverKind kind = ChartHoverKind::Bar;
  std::vector<ResolvedChartDataPoint> data;
  BarChartOptions bar_options;
  DonutChartOptions donut_options;
  ThemeSpec theme;

  bool operator==(const ChartHover&) const = default;
};

class ChartHoverExtension final : public NodeExtension {
public:
  ChartHoverExtension(MountedNode& node, const ChartHover& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const ChartHover& modifier) {
    static_cast<void>(node);
    const bool changed = kind_ != modifier.kind || data_ != modifier.data || bar_options_ != modifier.bar_options ||
                         donut_options_ != modifier.donut_options || theme_ != modifier.theme;
    kind_ = modifier.kind;
    data_ = modifier.data;
    bar_options_ = modifier.bar_options;
    donut_options_ = modifier.donut_options;
    theme_ = modifier.theme;
    if (hovered_index_.has_value() && *hovered_index_ >= data_.size()) {
      hovered_index_.reset();
    }
    if (changed && hovered_index_.has_value()) {
      InvalidatePaint();
    }
  }

  bool HoverHitTest(MountedNode& node, Point position) const override {
    const auto& mounted = static_cast<const detail::MountedNode&>(node);
    return node.IsEnabled() && mounted.ContentBounds().Contains(position);
  }

  void OnHoverChanged(MountedNode& node, bool hovered) override {
    static_cast<void>(node);
    hovered_ = hovered;
    if (!hovered_ && hovered_index_.has_value()) {
      hovered_index_.reset();
      InvalidatePaint();
    }
  }

  PointerResult OnPointer(MountedNode& node, const PointerEvent& event) override {
    if (!hovered_ || event.type != PointerEventType::Move) {
      return PointerResult::Ignored;
    }
    const auto& mounted = static_cast<const detail::MountedNode&>(node);
    const Rect content = mounted.ContentBounds();
    const Size size{content.width, content.height};
    const Point local{event.position.x - content.x, event.position.y - content.y};
    std::optional<std::size_t> next;
    if (kind_ == ChartHoverKind::Bar) {
      const BarChartGeometry geometry = ResolveBarChartGeometry(size, data_, bar_options_);
      next = BarIndexAt(local, geometry, data_.size());
    } else {
      const DonutChartGeometry geometry = ResolveDonutChartGeometry(size, data_, donut_options_);
      next = DonutIndexAt(local, size, data_, donut_options_, geometry);
    }
    if (next != hovered_index_) {
      hovered_index_ = next;
      InvalidatePaint();
    }
    return PointerResult::Ignored;
  }

  void Paint(const MountedNode& node, PaintContext& context) const override {
    if (!hovered_index_.has_value() || *hovered_index_ >= data_.size()) {
      return;
    }
    const auto& mounted = static_cast<const detail::MountedNode&>(node);
    const Rect content = mounted.ContentBounds();
    const Size size{content.width, content.height};
    context.PushTransform(Transform2D{.translate_x = content.x, .translate_y = content.y});
    if (kind_ == ChartHoverKind::Bar) {
      PaintBarHover(context, size, *hovered_index_);
    } else {
      PaintDonutHover(context, size, *hovered_index_);
    }
    context.PopTransform();
  }

private:
  void PaintBarHover(PaintContext& paint, Size size, std::size_t index) const {
    const BarChartGeometry geometry = ResolveBarChartGeometry(size, data_, bar_options_);
    if (!geometry.IsValid()) {
      return;
    }
    Rect bar = geometry.BarBounds(index, data_[index].value);
    if (bar.height <= 0.0F) {
      bar.y = geometry.top + geometry.plot_height - 3.0F;
      bar.height = 3.0F;
    }
    paint.StrokePath(
        Path::RoundedRect(bar, CornerRadii{geometry.corner_radius}),
        theme_.colors.on_surface,
        2.5F,
        StrokeCap::Round,
        StrokeJoin::Round
    );
    PaintHoverCard(
        paint,
        size,
        {bar.x + bar.width * 0.5F, bar.y},
        data_[index],
        DataTotal(data_),
        theme_
    );
  }

  void PaintDonutHover(PaintContext& paint, Size size, std::size_t index) const {
    const DonutChartGeometry geometry = ResolveDonutChartGeometry(size, data_, donut_options_);
    if (!geometry.IsValid()) {
      return;
    }
    float start_angle = -pi * 0.5F;
    for (std::size_t current = 0; current < index; ++current) {
      start_angle += data_[current].value / geometry.total * pi * 2.0F;
    }
    const float full_sweep = data_[index].value / geometry.total * pi * 2.0F;
    const float gap = std::min(geometry.requested_gap, full_sweep * 0.4F);
    const float visible_sweep = std::max(0.0F, full_sweep - gap);
    paint.DrawArc(
        geometry.center,
        geometry.arc_radius,
        start_angle + gap * 0.5F,
        visible_sweep,
        WithAlpha(theme_.colors.inverse_on_surface, 0.22F),
        geometry.stroke_width
    );
    paint.DrawArc(
        geometry.center,
        geometry.outer_radius + 3.0F,
        start_angle + gap * 0.5F,
        visible_sweep,
        data_[index].color,
        3.0F,
        StrokeCap::Round
    );
    const float middle = start_angle + full_sweep * 0.5F;
    const Point anchor{
        geometry.center.x + std::cos(middle) * geometry.outer_radius,
        geometry.center.y + std::sin(middle) * geometry.outer_radius,
    };
    PaintHoverCard(paint, size, anchor, data_[index], geometry.total, theme_);
  }

  ChartHoverKind kind_ = ChartHoverKind::Bar;
  std::vector<ResolvedChartDataPoint> data_;
  BarChartOptions bar_options_;
  DonutChartOptions donut_options_;
  ThemeSpec theme_;
  bool hovered_ = false;
  std::optional<std::size_t> hovered_index_;
};

} // namespace

View BarChart(std::vector<ChartDataPoint> data, BarChartOptions options) {
  ValidateData(data);
  ValidateBarOptions(data, options);
  const ThemeSpec theme = UseTheme();
  std::vector<ResolvedChartDataPoint> resolved = ResolveData(std::move(data), theme.colors);
  const std::string accessibility_value = AccessibilityValue(resolved);
  const std::string accessibility_label = options.accessibility_label;

  View chart = Canvas([resolved, options, theme](PaintContext& paint, Size size) {
                 PaintBarChart(paint, size, resolved, options, theme);
               })
                   .With(Semantics{
                       .role = SemanticRole::Image,
                       .label = StringVariant(accessibility_label),
                       .value = StringVariant(accessibility_value),
                   });
  if (options.show_hover_info) {
    chart = std::move(chart).With(ChartHover{
        .kind = ChartHoverKind::Bar,
        .data = resolved,
        .bar_options = options,
        .theme = theme,
    });
  }
  return chart;
}

View DonutChart(std::vector<ChartDataPoint> data, DonutChartOptions options) {
  ValidateData(data);
  ValidateDonutOptions(data, options);
  const ThemeSpec theme = UseTheme();
  std::vector<ResolvedChartDataPoint> resolved = ResolveData(std::move(data), theme.colors);
  const std::string accessibility_value = AccessibilityValue(resolved);
  const std::string accessibility_label = options.accessibility_label;

  View chart = Canvas([resolved, options, theme](PaintContext& paint, Size size) {
                 PaintDonutChart(paint, size, resolved, options, theme);
               })
                   .With(Semantics{
                       .role = SemanticRole::Image,
                       .label = StringVariant(accessibility_label),
                       .value = StringVariant(accessibility_value),
                   });
  if (options.show_hover_info) {
    chart = std::move(chart).With(ChartHover{
        .kind = ChartHoverKind::Donut,
        .data = resolved,
        .donut_options = options,
        .theme = theme,
    });
  }
  return chart;
}

} // namespace huxerui
