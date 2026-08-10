#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <huxerui/color.h>
#include <huxerui/view.h>

namespace huxerui {

struct ChartDataPoint {
  std::string label;
  float value = 0.0F;
  std::optional<Color> color;

  bool operator==(const ChartDataPoint&) const = default;
};

struct BarChartOptions {
  std::optional<float> maximum_value;
  std::size_t grid_line_count = 4;
  float bar_width_fraction = 0.62F;
  float corner_radius = 6.0F;
  bool show_values = true;
  bool show_hover_info = true;
  std::string accessibility_label = "Bar chart";

  bool operator==(const BarChartOptions&) const = default;
};

struct DonutChartOptions {
  float inner_radius_fraction = 0.62F;
  float segment_gap_degrees = 2.0F;
  bool show_legend = true;
  bool show_hover_info = true;
  std::string center_label = "Total";
  std::string accessibility_label = "Donut chart";

  bool operator==(const DonutChartOptions&) const = default;
};

View BarChart(std::vector<ChartDataPoint> data, BarChartOptions options = {});
View DonutChart(std::vector<ChartDataPoint> data, DonutChartOptions options = {});

} // namespace huxerui
