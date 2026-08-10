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

namespace huxerui {
namespace {

constexpr float pi = std::numbers::pi_v<float>;

struct ResolvedChartDataPoint {
  std::string label;
  float value = 0.0F;
  Color color;
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

void PaintBarChart(
    PaintContext& paint,
    Size size,
    const std::vector<ResolvedChartDataPoint>& data,
    const BarChartOptions& options,
    const ThemeSpec& theme
) {
  if (size.width < 96.0F || size.height < 96.0F) {
    return;
  }

  const float left = std::min(48.0F, size.width * 0.17F);
  const float right = 10.0F;
  const float top = 24.0F;
  const float bottom = 34.0F;
  const float plot_width = std::max(0.0F, size.width - left - right);
  const float plot_height = std::max(0.0F, size.height - top - bottom);
  const float maximum = BarMaximum(data, options);
  const TextStyle axis_style{Font::System(10.0F), theme.colors.on_surface_variant};
  const TextStyle value_style{Font::System(11.0F).WithWeight(FontWeight::Medium), theme.colors.on_surface};
  const Color grid_color = WithAlpha(theme.colors.outline, 0.22F);

  for (std::size_t index = 0; index <= options.grid_line_count; ++index) {
    const float fraction = static_cast<float>(index) / static_cast<float>(options.grid_line_count);
    const float y = top + plot_height * (1.0F - fraction);
    paint.DrawRect({left, y, plot_width, 1.0F}, grid_color);
    paint.DrawText(
        {0.0F, y - 8.0F, left - 6.0F, 16.0F},
        FormatValue(maximum * fraction),
        axis_style,
        TextLayoutOptions{.align = TextAlign::Trailing, .wrap = TextWrap::NoWrap}
    );
  }

  const float slot_width = plot_width / static_cast<float>(data.size());
  const float bar_width = std::max(2.0F, std::min(56.0F, slot_width * options.bar_width_fraction));
  const float corner_radius = std::min(options.corner_radius, bar_width * 0.5F);
  for (std::size_t index = 0; index < data.size(); ++index) {
    const ResolvedChartDataPoint& point = data[index];
    const float center_x = left + slot_width * (static_cast<float>(index) + 0.5F);
    const float bar_height = plot_height * std::clamp(point.value / maximum, 0.0F, 1.0F);
    const float bar_top = top + plot_height - bar_height;
    if (bar_height > 0.0F) {
      paint.DrawRect(
          {center_x - bar_width * 0.5F, bar_top, bar_width, bar_height},
          point.color,
          CornerRadii{corner_radius}
      );
    }
    if (options.show_values) {
      paint.DrawText(
          {center_x - slot_width * 0.5F, std::max(0.0F, bar_top - 20.0F), slot_width, 18.0F},
          FormatValue(point.value),
          value_style,
          TextLayoutOptions{.align = TextAlign::Center, .wrap = TextWrap::NoWrap}
      );
    }
    paint.DrawText(
        {center_x - slot_width * 0.5F, top + plot_height + 8.0F, slot_width, 20.0F},
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

void PaintDonutLegend(
    PaintContext& paint,
    Size size,
    const std::vector<ResolvedChartDataPoint>& data,
    float chart_width,
    bool wide,
    const ThemeSpec& theme
) {
  if (data.empty()) {
    return;
  }

  const float legend_x = wide ? chart_width + 8.0F : 8.0F;
  const float legend_width = std::max(0.0F, size.width - legend_x - 8.0F);
  const float available_height = wide ? size.height - 16.0F : size.height * 0.38F - 8.0F;
  const float row_height = std::clamp(available_height / static_cast<float>(data.size()), 14.0F, 24.0F);
  const float legend_height = row_height * static_cast<float>(data.size());
  const float legend_y = wide ? (size.height - legend_height) * 0.5F : size.height - legend_height - 4.0F;
  const TextStyle label_style{Font::System(row_height < 18.0F ? 10.0F : 12.0F), theme.colors.on_surface};
  const TextStyle value_style{
      Font::System(row_height < 18.0F ? 10.0F : 12.0F).WithWeight(FontWeight::Medium),
      theme.colors.on_surface_variant,
  };

  for (std::size_t index = 0; index < data.size(); ++index) {
    const ResolvedChartDataPoint& point = data[index];
    const float y = legend_y + row_height * static_cast<float>(index);
    const float marker = std::min(10.0F, row_height - 4.0F);
    paint.DrawRect(
        {legend_x, y + (row_height - marker) * 0.5F, marker, marker},
        point.color,
        CornerRadii{marker * 0.3F}
    );
    const float value_width = std::min(64.0F, legend_width * 0.32F);
    paint.DrawText(
        {legend_x + marker + 8.0F, y, std::max(0.0F, legend_width - marker - value_width - 12.0F), row_height},
        point.label,
        label_style,
        TextLayoutOptions{.wrap = TextWrap::NoWrap}
    );
    paint.DrawText(
        {legend_x + legend_width - value_width, y, value_width, row_height},
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
  if (size.width < 96.0F || size.height < 96.0F) {
    return;
  }

  const bool wide = options.show_legend && size.width >= 360.0F;
  const float chart_width = options.show_legend && wide ? size.width * 0.58F : size.width;
  const float chart_height = options.show_legend && !wide ? size.height * 0.62F : size.height;
  const float extent = std::min(chart_width, chart_height);
  const Point center{chart_width * 0.5F, chart_height * 0.5F};
  const float outer_radius = extent * 0.40F;
  const float inner_radius = outer_radius * options.inner_radius_fraction;
  const float stroke_width = outer_radius - inner_radius;
  const float arc_radius = inner_radius + stroke_width * 0.5F;
  const float total = DataTotal(data);
  const float requested_gap = options.segment_gap_degrees * pi / 180.0F;

  paint.DrawArc(
      center,
      arc_radius,
      -pi * 0.5F,
      pi * 2.0F,
      WithAlpha(theme.colors.outline, 0.14F),
      stroke_width
  );

  float start_angle = -pi * 0.5F;
  for (const ResolvedChartDataPoint& point : data) {
    const float full_sweep = point.value / total * pi * 2.0F;
    const float gap = std::min(requested_gap, full_sweep * 0.4F);
    const float visible_sweep = std::max(0.0F, full_sweep - gap);
    if (visible_sweep > 0.0F) {
      paint.DrawArc(center, arc_radius, start_angle + gap * 0.5F, visible_sweep, point.color, stroke_width);
    }
    start_angle += full_sweep;
  }

  const TextStyle total_style{
      Font::System(std::max(14.0F, outer_radius * 0.22F)).WithWeight(FontWeight::Bold),
      theme.colors.on_surface,
  };
  const TextStyle label_style{Font::System(11.0F), theme.colors.on_surface_variant};
  paint.DrawText(
      {center.x - inner_radius, center.y - 22.0F, inner_radius * 2.0F, 24.0F},
      FormatValue(total),
      total_style,
      TextLayoutOptions{.align = TextAlign::Center, .wrap = TextWrap::NoWrap}
  );
  if (!options.center_label.empty()) {
    paint.DrawText(
        {center.x - inner_radius, center.y + 3.0F, inner_radius * 2.0F, 18.0F},
        options.center_label,
        label_style,
        TextLayoutOptions{.align = TextAlign::Center, .wrap = TextWrap::NoWrap}
    );
  }

  if (options.show_legend) {
    PaintDonutLegend(paint, size, data, chart_width, wide, theme);
  }
}

} // namespace

View BarChart(std::vector<ChartDataPoint> data, BarChartOptions options) {
  ValidateData(data);
  ValidateBarOptions(data, options);
  const ThemeSpec theme = UseTheme();
  std::vector<ResolvedChartDataPoint> resolved = ResolveData(std::move(data), theme.colors);
  const std::string accessibility_value = AccessibilityValue(resolved);
  const std::string accessibility_label = options.accessibility_label;

  return Canvas([resolved, options, theme](PaintContext& paint, Size size) {
           PaintBarChart(paint, size, resolved, options, theme);
         })
      .With(Semantics{
          .role = SemanticRole::Image,
          .label = StringVariant(accessibility_label),
          .value = StringVariant(accessibility_value),
      });
}

View DonutChart(std::vector<ChartDataPoint> data, DonutChartOptions options) {
  ValidateData(data);
  ValidateDonutOptions(data, options);
  const ThemeSpec theme = UseTheme();
  std::vector<ResolvedChartDataPoint> resolved = ResolveData(std::move(data), theme.colors);
  const std::string accessibility_value = AccessibilityValue(resolved);
  const std::string accessibility_label = options.accessibility_label;

  return Canvas([resolved, options, theme](PaintContext& paint, Size size) {
           PaintDonutChart(paint, size, resolved, options, theme);
         })
      .With(Semantics{
          .role = SemanticRole::Image,
          .label = StringVariant(accessibility_label),
          .value = StringVariant(accessibility_value),
      });
}

} // namespace huxerui
