#include <huxerui/view.h>

#include <algorithm>
#include <any>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

#include <huxerui/environment.h>
#include <huxerui/semantics.h>
#include <huxerui/theme.h>

#include "huxerui_builtin_resources.h"
#include "mounted_node_internal.h"

namespace huxerui {

namespace {

using Date = std::chrono::year_month_day;
using Days = std::chrono::days;
using Minutes = std::chrono::minutes;
using SysDays = std::chrono::sys_days;
using YearMonth = std::chrono::year_month;

constexpr float pi = 3.14159265358979323846F;
constexpr float full_circle = pi * 2.0F;
constexpr Minutes minutes_per_day{24 * 60};

struct DateTimeLocaleData {
  std::string language_tag;
  std::array<std::string, 12> months;
  std::array<std::string, 7> weekdays;
  std::string date_picker_label;
  std::string time_picker_label;
  std::string previous_month_label;
  std::string next_month_label;
  std::string previous_years_label;
  std::string next_years_label;
  std::string choose_year_label;
  std::string hour_label;
  std::string minute_label;
  std::string am_label;
  std::string pm_label;
  unsigned int first_weekday = 0;
  bool year_first = false;
  bool use_12_hour = false;
  bool right_to_left = false;
};

struct DatePickerConfiguration {
  static const detail::ModifierDescriptor& Descriptor();

  Date value;
  std::optional<Date> minimum;
  std::optional<Date> maximum;
  std::function<bool(Date)> disabled_dates;
  StringVariant label;
  ValidationResult validation;
};

struct TimePickerConfiguration {
  static const detail::ModifierDescriptor& Descriptor();

  Minutes value;
  Minutes step{1};
  std::function<bool(Minutes)> disabled_times;
  StringVariant label;
  ValidationResult validation;
};

bool StartsWithLanguage(std::string_view tag, std::string_view language) {
  return tag == language || (tag.size() > language.size() && tag.substr(0, language.size()) == language &&
                             tag[language.size()] == '-');
}

std::string_view LocaleRegion(std::string_view tag) {
  std::size_t start = tag.find('-');
  while (start != std::string_view::npos) {
    ++start;
    const std::size_t end = tag.find('-', start);
    const std::string_view subtag = tag.substr(start, end == std::string_view::npos ? end : end - start);
    const bool alpha_region = subtag.size() == 2U &&
                              std::ranges::all_of(subtag, [](char value) { return value >= 'A' && value <= 'Z'; });
    const bool numeric_region = subtag.size() == 3U &&
                                std::ranges::all_of(subtag, [](char value) { return value >= '0' && value <= '9'; });
    if (alpha_region || numeric_region) {
      return subtag;
    }
    start = end;
  }
  return {};
}

unsigned int LocaleFirstWeekday(std::string_view tag) {
  const std::string_view region = LocaleRegion(tag);
  if (!region.empty()) {
    constexpr auto sunday_regions = std::to_array<std::string_view>({
        "AG", "AS", "BD", "BR", "BS", "BT", "BW", "BZ", "CA", "CO", "DM", "DO", "ET", "GT", "GU", "HK",
        "HN", "ID", "IL", "IN", "IS", "JM", "JP", "KE", "KH", "KR", "LA", "MH", "MM", "MO", "MT", "MX",
        "MZ", "NI", "NP", "PA", "PE", "PH", "PK", "PR", "PT", "PY", "SA", "SG", "SV", "TH", "TT", "TW",
        "UM", "US", "VE", "VI", "WS", "YE", "ZA", "ZW",
    });
    constexpr auto saturday_regions = std::to_array<std::string_view>({
        "AF", "BH", "DJ", "DZ", "EG", "IQ", "IR", "JO", "KW", "LY", "OM", "QA", "SD", "SY",
    });
    if (std::ranges::find(sunday_regions, region) != sunday_regions.end()) {
      return 0;
    }
    if (std::ranges::find(saturday_regions, region) != saturday_regions.end()) {
      return 6;
    }
    return region == "MV" ? 5U : 1U;
  }
  if (StartsWithLanguage(tag, "ar") || StartsWithLanguage(tag, "fa")) {
    return 6;
  }
  const bool sunday_first = StartsWithLanguage(tag, "en") || StartsWithLanguage(tag, "bn") ||
                            StartsWithLanguage(tag, "fil") || StartsWithLanguage(tag, "he") ||
                            StartsWithLanguage(tag, "hi") || StartsWithLanguage(tag, "ja") ||
                            StartsWithLanguage(tag, "ko") || StartsWithLanguage(tag, "th") ||
                            StartsWithLanguage(tag, "ur");
  return sunday_first ? 0U : 1U;
}

bool LocaleUses12Hour(std::string_view tag) {
  const std::string_view region = LocaleRegion(tag);
  if (!region.empty() && region != "001") {
    if ((region == "CA" && StartsWithLanguage(tag, "fr")) ||
        (region == "SY" && StartsWithLanguage(tag, "ku"))) {
      return false;
    }
    constexpr auto regions = std::to_array<std::string_view>({
        "419", "AE", "AG", "AL", "AR", "AS", "AU", "BB", "BD", "BH", "BM", "BN", "BO", "BS", "BT", "CA",
        "CL", "CO", "CR", "CU", "CY", "DJ", "DM", "DO", "DZ", "EC", "EG", "EH", "ER", "ET", "FJ", "FM",
        "GD", "GH", "GM", "GR", "GT", "GU", "GY", "HK", "HN", "IN", "IQ", "JM", "JO", "KH", "KI", "KN",
        "KP", "KR", "KW", "KY", "LB", "LC", "LR", "LS", "LY", "MH", "MO", "MP", "MR", "MW", "MX", "MY",
        "NA", "NI", "NZ", "OM", "PA", "PE", "PG", "PH", "PK", "PR", "PS", "PW", "PY", "QA", "SA", "SB",
        "SD", "SG", "SL", "SO", "SS", "SV", "SY", "SZ", "TC", "TD", "TN", "TO", "TT", "TW", "UM", "US",
        "UY", "VC", "VE", "VG", "VI", "VU", "WS", "YE", "ZM",
    });
    return std::ranges::find(regions, region) != regions.end();
  }
  return StartsWithLanguage(tag, "en") || StartsWithLanguage(tag, "ar") || StartsWithLanguage(tag, "bn") ||
         StartsWithLanguage(tag, "fil") || StartsWithLanguage(tag, "hi") || StartsWithLanguage(tag, "ko") ||
         StartsWithLanguage(tag, "ms") || StartsWithLanguage(tag, "ur");
}

DateTimeLocaleData ResolveDateTimeLocale(const Locale& locale) {
  DateTimeLocaleData result;
  result.language_tag = locale.LanguageTag();
  constexpr std::array<const StringResource*, 12> month_resources{
      &strings::date_picker_month_january, &strings::date_picker_month_february, &strings::date_picker_month_march,
      &strings::date_picker_month_april, &strings::date_picker_month_may, &strings::date_picker_month_june,
      &strings::date_picker_month_july, &strings::date_picker_month_august, &strings::date_picker_month_september,
      &strings::date_picker_month_october, &strings::date_picker_month_november, &strings::date_picker_month_december,
  };
  constexpr std::array<const StringResource*, 7> weekday_resources{
      &strings::date_picker_weekday_sunday, &strings::date_picker_weekday_monday,
      &strings::date_picker_weekday_tuesday, &strings::date_picker_weekday_wednesday,
      &strings::date_picker_weekday_thursday, &strings::date_picker_weekday_friday,
      &strings::date_picker_weekday_saturday,
  };
  for (std::size_t index = 0; index < result.months.size(); ++index) {
    result.months[index] = UseString(*month_resources[index]);
  }
  for (std::size_t index = 0; index < result.weekdays.size(); ++index) {
    result.weekdays[index] = UseString(*weekday_resources[index]);
  }
  result.date_picker_label = UseString(strings::date_picker);
  result.time_picker_label = UseString(strings::time_picker);
  result.previous_month_label = UseString(strings::date_picker_previous_month);
  result.next_month_label = UseString(strings::date_picker_next_month);
  result.previous_years_label = UseString(strings::date_picker_previous_years);
  result.next_years_label = UseString(strings::date_picker_next_years);
  result.choose_year_label = UseString(strings::date_picker_choose_year);
  result.hour_label = UseString(strings::time_picker_hour);
  result.minute_label = UseString(strings::time_picker_minute);
  result.am_label = UseString(strings::time_picker_am);
  result.pm_label = UseString(strings::time_picker_pm);

  result.first_weekday = LocaleFirstWeekday(result.language_tag);
  result.year_first = StartsWithLanguage(result.language_tag, "zh") ||
                      StartsWithLanguage(result.language_tag, "ja") ||
                      StartsWithLanguage(result.language_tag, "ko");
  result.use_12_hour = LocaleUses12Hour(result.language_tag);
  result.right_to_left = StartsWithLanguage(result.language_tag, "ar") ||
                         StartsWithLanguage(result.language_tag, "fa") ||
                         StartsWithLanguage(result.language_tag, "he") ||
                         StartsWithLanguage(result.language_tag, "ur");
  return result;
}

DatePickerStyle ResolveDatePickerStyle(const std::shared_ptr<const Environment>& environment) {
  if (const std::any* value = detail::FindThemeStyleValue(environment, typeid(DatePickerStyle))) {
    if (const auto* style = std::any_cast<DatePickerStyle>(value)) {
      return *style;
    }
    throw std::logic_error("HuxerUI DatePicker style environment value has an invalid type");
  }
  return detail::DefaultDatePickerStyle(detail::ResolveThemeSpec(environment));
}

TimePickerStyle ResolveTimePickerStyle(const std::shared_ptr<const Environment>& environment) {
  if (const std::any* value = detail::FindThemeStyleValue(environment, typeid(TimePickerStyle))) {
    if (const auto* style = std::any_cast<TimePickerStyle>(value)) {
      return *style;
    }
    throw std::logic_error("HuxerUI TimePicker style environment value has an invalid type");
  }
  return detail::DefaultTimePickerStyle(detail::ResolveThemeSpec(environment));
}

bool IsFiniteNonNegative(float value) {
  return std::isfinite(value) && value >= 0.0F;
}

bool IsFiniteNonNegative(const EdgeInsets& value) {
  return IsFiniteNonNegative(value.top) && IsFiniteNonNegative(value.right) &&
         IsFiniteNonNegative(value.bottom) && IsFiniteNonNegative(value.left);
}

void ValidateDatePickerStyle(const DatePickerStyle& style) {
  const bool valid = IsFiniteNonNegative(style.padding) && std::isfinite(style.cell_size) && style.cell_size > 0.0F &&
                     IsFiniteNonNegative(style.column_spacing) && IsFiniteNonNegative(style.row_spacing) &&
                     std::isfinite(style.header_height) && style.header_height > 0.0F &&
                     IsFiniteNonNegative(style.corner_radius) && IsFiniteNonNegative(style.selection_corner_radius) &&
                     IsFiniteNonNegative(style.border_width) && IsFiniteNonNegative(style.label_spacing) &&
                     IsFiniteNonNegative(style.validation_spacing);
  if (!valid) {
    throw std::invalid_argument(
        "HuxerUI DatePicker geometry must be finite with positive cell and header sizes and non-negative extents"
    );
  }
}

void ValidateTimePickerStyle(const TimePickerStyle& style) {
  const bool valid = IsFiniteNonNegative(style.padding) && std::isfinite(style.dial_size) && style.dial_size > 0.0F &&
                     std::isfinite(style.header_height) && style.header_height > 0.0F &&
                     std::isfinite(style.field_width) && style.field_width > 0.0F &&
                     std::isfinite(style.separator_width) && style.separator_width > 0.0F &&
                     std::isfinite(style.period_width) && style.period_width > 0.0F &&
                     IsFiniteNonNegative(style.field_corner_radius) && IsFiniteNonNegative(style.period_spacing) &&
                     IsFiniteNonNegative(style.period_corner_radius) &&
                     IsFiniteNonNegative(style.period_border_width) &&
                     IsFiniteNonNegative(style.content_spacing) && std::isfinite(style.selection_radius) &&
                     style.selection_radius > 0.0F && std::isfinite(style.hand_width) && style.hand_width > 0.0F &&
                     IsFiniteNonNegative(style.corner_radius) && IsFiniteNonNegative(style.border_width) &&
                     IsFiniteNonNegative(style.label_spacing) && IsFiniteNonNegative(style.validation_spacing);
  if (!valid) {
    throw std::invalid_argument(
        "HuxerUI TimePicker geometry must be finite with positive dial, header, field, separator, period, selection, "
        "and hand sizes and non-negative extents"
    );
  }
}

SysDays ToSysDays(Date value) {
  return SysDays{value};
}

Date FromSysDays(SysDays value) {
  return Date{value};
}

unsigned int DaysInMonth(YearMonth value) {
  return static_cast<unsigned int>(Date{value / std::chrono::last}.day());
}

Date ClampDay(YearMonth month, unsigned int day) {
  return Date{month.year(), month.month(), std::chrono::day{std::min(day, DaysInMonth(month))}};
}

std::string Number(unsigned int value, unsigned int width = 0) {
  std::ostringstream stream;
  if (width > 0) {
    stream << std::setfill('0') << std::setw(static_cast<int>(width));
  }
  stream << value;
  return stream.str();
}

std::string YearNumber(std::chrono::year value) {
  return std::to_string(static_cast<int>(value));
}

std::string FormatMonth(YearMonth month, const DateTimeLocaleData& locale) {
  const std::size_t month_index = static_cast<unsigned int>(month.month()) - 1U;
  if (StartsWithLanguage(locale.language_tag, "zh")) {
    return YearNumber(month.year()) + "\xE5\xB9\xB4" + locale.months[month_index];
  }
  if (locale.year_first) {
    return YearNumber(month.year()) + " " + locale.months[month_index];
  }
  return locale.months[month_index] + " " + YearNumber(month.year());
}

std::string FormatDate(Date value, const DateTimeLocaleData& locale) {
  const unsigned int month = static_cast<unsigned int>(value.month());
  const unsigned int day = static_cast<unsigned int>(value.day());
  if (StartsWithLanguage(locale.language_tag, "zh")) {
    return YearNumber(value.year()) + "\xE5\xB9\xB4" + Number(month) + "\xE6\x9C\x88" + Number(day) +
           "\xE6\x97\xA5";
  }
  if (locale.year_first) {
    return YearNumber(value.year()) + " " + locale.months[month - 1U] + " " + Number(day);
  }
  return locale.months[month - 1U] + " " + Number(day) + ", " + YearNumber(value.year());
}

TextLayoutOptions CenteredText(const DateTimeLocaleData& locale) {
  return {
      .shaping = TextShapingOptions{
          .direction = locale.right_to_left ? TextDirection::RightToLeft : TextDirection::LeftToRight,
          .locale = locale.language_tag,
      },
      .align = TextAlign::Center,
      .vertical_align = TextVerticalAlign::Center,
      .wrap = TextWrap::NoWrap,
  };
}

struct DatePickerBehavior {
  static const detail::ModifierDescriptor& Descriptor();

  Date value;
  std::optional<Date> minimum;
  std::optional<Date> maximum;
  std::function<bool(Date)> disabled_dates;
  DatePickerStyle style;
  DateTimeLocaleData locale;
  EventEmitter events;
  std::string accessible_label;
  bool invalid = false;
  std::string validation_message;
};

struct DateGeometry {
  Rect previous;
  Rect title;
  Rect next;
  std::array<Rect, 7> weekdays;
  std::array<Rect, 42> days;
  std::array<Rect, 20> years;
};

DateGeometry MakeDateGeometry(Rect bounds, const DatePickerStyle& style, bool right_to_left) {
  DateGeometry result;
  const float content_x = bounds.x + style.padding.left;
  const float content_y = bounds.y + style.padding.top;
  const float content_width = std::max(0.0F, bounds.width - style.padding.Horizontal());
  const float action_width = style.header_height;
  const Rect left{content_x, content_y, action_width, style.header_height};
  const Rect right{content_x + std::max(0.0F, content_width - action_width), content_y, action_width,
                   style.header_height};
  result.previous = right_to_left ? right : left;
  result.next = right_to_left ? left : right;
  result.title = {
      content_x + action_width,
      content_y,
      std::max(0.0F, content_width - action_width * 2.0F),
      style.header_height,
  };
  const float weekday_y = content_y + style.header_height;
  const float day_y = weekday_y + style.cell_size + style.row_spacing;
  for (std::size_t index = 0; index < result.weekdays.size(); ++index) {
    const std::size_t visual_column = right_to_left ? result.weekdays.size() - 1U - index : index;
    const float x = content_x + static_cast<float>(visual_column) * (style.cell_size + style.column_spacing);
    result.weekdays[index] = {x, weekday_y, style.cell_size, style.cell_size};
  }
  for (std::size_t index = 0; index < result.days.size(); ++index) {
    const std::size_t row = index / 7U;
    const std::size_t logical_column = index % 7U;
    const std::size_t visual_column = right_to_left ? 6U - logical_column : logical_column;
    result.days[index] = {
        content_x + static_cast<float>(visual_column) * (style.cell_size + style.column_spacing),
        day_y + static_cast<float>(row) * (style.cell_size + style.row_spacing),
        style.cell_size,
        style.cell_size,
    };
  }
  for (std::size_t index = 0; index < result.years.size(); ++index) {
    const std::size_t row = index / 4U;
    const std::size_t logical_column = index % 4U;
    const std::size_t visual_column = right_to_left ? 3U - logical_column : logical_column;
    const float column_width = std::max(0.0F, content_width - style.column_spacing * 3.0F) / 4.0F;
    result.years[index] = {
        content_x + static_cast<float>(visual_column) * (column_width + style.column_spacing),
        weekday_y + static_cast<float>(row) * (style.cell_size + style.row_spacing),
        column_width,
        style.cell_size,
    };
  }
  return result;
}

class DatePickerBehaviorExtension final : public NodeExtension {
public:
  DatePickerBehaviorExtension(MountedNode& node, const DatePickerBehavior& value) {
    Update(node, value);
  }

  void Update(MountedNode& node, const DatePickerBehavior& value) {
    static_cast<void>(node);
    const bool controlled_changed = !initialized_ || value_ != value.value;
    const bool range_changed = minimum_ != value.minimum || maximum_ != value.maximum;
    value_ = value.value;
    minimum_ = value.minimum;
    maximum_ = value.maximum;
    disabled_dates_ = value.disabled_dates;
    style_ = value.style;
    locale_ = value.locale;
    events_ = value.events;
    accessible_label_ = value.accessible_label.empty() ? locale_.date_picker_label : value.accessible_label;
    invalid_ = value.invalid;
    validation_message_ = value.validation_message;
    if (controlled_changed) {
      displayed_month_ = YearMonth{value_.year(), value_.month()};
      active_date_ = value_;
      active_year_ = static_cast<int>(value_.year());
    } else if (range_changed) {
      YearMonth month = displayed_month_;
      if (minimum_) {
        month = std::max(month, YearMonth{minimum_->year(), minimum_->month()});
      }
      if (maximum_) {
        month = std::min(month, YearMonth{maximum_->year(), maximum_->month()});
      }
      const int active_year = active_year_;
      ShowMonth(month);
      if (YearAllowed(active_year) && active_year >= YearPageStart() && active_year < YearPageStart() + 20) {
        active_year_ = active_year;
      }
    }
    initialized_ = true;
    InvalidatePaint();
    InvalidateSemantics();
  }

  [[nodiscard]] bool HitTest(MountedNode& node, Point position) const override {
    return node.IsEnabled() && node.Bounds().Contains(position);
  }

  [[nodiscard]] bool HoverHitTest(MountedNode& node, Point position) const override {
    return HitTest(node, position);
  }

  void OnHover(MountedNode& node, const HoverEvent& event) override {
    const std::optional<std::size_t> previous = hover_day_;
    hover_day_.reset();
    if (event.type != HoverEventType::Leave && !year_mode_) {
      const DateGeometry geometry = MakeDateGeometry(node.Bounds(), style_, locale_.right_to_left);
      for (std::size_t index = 0; index < geometry.days.size(); ++index) {
        if (geometry.days[index].Contains(event.position)) {
          hover_day_ = index;
          break;
        }
      }
    }
    if (previous != hover_day_) {
      InvalidatePaint();
    }
  }

  PointerResult OnPointer(MountedNode& node, const PointerEvent& event) override {
    if (!node.IsEnabled()) {
      pointer_id_.reset();
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Down) {
      pointer_id_ = event.pointer_id;
      return PointerResult::Capture;
    }
    if (!pointer_id_.has_value() || *pointer_id_ != event.pointer_id) {
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Cancel) {
      pointer_id_.reset();
      return PointerResult::Handled;
    }
    if (event.type == PointerEventType::Up) {
      pointer_id_.reset();
      if (node.Bounds().Contains(event.position)) {
        PerformAt(node.Bounds(), event.position);
      }
      return PointerResult::Handled;
    }
    return PointerResult::Capture;
  }

  bool OnKey(MountedNode& node, const KeyEvent& event) override {
    static_cast<void>(node);
    if (event.type != KeyEventType::Down || event.modifiers.alt || event.modifiers.control || event.modifiers.meta) {
      return false;
    }
    if (year_mode_) {
      switch (event.key) {
      case Key::Escape:
        year_mode_ = false;
        InvalidatePaint();
        InvalidateSemantics();
        return true;
      case Key::ArrowLeft:
        MoveActiveYear(locale_.right_to_left ? 1 : -1);
        return true;
      case Key::ArrowRight:
        MoveActiveYear(locale_.right_to_left ? -1 : 1);
        return true;
      case Key::ArrowUp:
        MoveActiveYear(-4);
        return true;
      case Key::ArrowDown:
        MoveActiveYear(4);
        return true;
      case Key::Home:
        MoveActiveYear(YearPageStart() - active_year_);
        return true;
      case Key::End:
        MoveActiveYear(YearPageStart() + 19 - active_year_);
        return true;
      case Key::PageUp:
      case Key::PageDown:
        ChangeYearPage(event.key == Key::PageUp ? -1 : 1);
        return true;
      case Key::Enter:
      case Key::Space:
        ChooseYear(active_year_);
        return true;
      default:
        return false;
      }
    }
    switch (event.key) {
    case Key::ArrowLeft:
      MoveActive(locale_.right_to_left ? Days{1} : Days{-1});
      return true;
    case Key::ArrowRight:
      MoveActive(locale_.right_to_left ? Days{-1} : Days{1});
      return true;
    case Key::ArrowUp:
      MoveActive(Days{-7});
      return true;
    case Key::ArrowDown:
      MoveActive(Days{7});
      return true;
    case Key::Home:
      MoveToWeekEdge(false);
      return true;
    case Key::End:
      MoveToWeekEdge(true);
      return true;
    case Key::PageUp:
      ChangeMonth(event.modifiers.shift ? -12 : -1);
      return true;
    case Key::PageDown:
      ChangeMonth(event.modifiers.shift ? 12 : 1);
      return true;
    case Key::Enter:
    case Key::Space:
      SelectDate(active_date_);
      return true;
    default:
      return false;
    }
  }

  void PaintAboveContent(const MountedNode& node, PaintContext& context) const override {
    const DateGeometry geometry = MakeDateGeometry(node.Bounds(), style_, locale_.right_to_left);
    const TextLayoutOptions text_options = CenteredText(locale_);
    PaintChevron(context, geometry.previous, locale_.right_to_left ? 1 : -1);
    PaintChevron(context, geometry.next, locale_.right_to_left ? -1 : 1);
    context.DrawText(geometry.title, FormatMonth(displayed_month_, locale_), style_.title_style, text_options);
    if (year_mode_) {
      PaintYears(context, geometry, text_options);
      return;
    }
    for (std::size_t index = 0; index < geometry.weekdays.size(); ++index) {
      const std::size_t weekday = (locale_.first_weekday + index) % 7U;
      context.DrawText(geometry.weekdays[index], locale_.weekdays[weekday], style_.weekday_style, text_options);
    }
    const SysDays first = GridStart();
    for (std::size_t index = 0; index < geometry.days.size(); ++index) {
      const Date date = FromSysDays(first + Days{static_cast<int>(index)});
      const bool selected = date == value_;
      const bool enabled = IsSelectable(date);
      if (selected) {
        context.DrawRect(geometry.days[index], style_.selected_background,
                         std::min(style_.selection_corner_radius, style_.cell_size * 0.5F));
      } else if ((hover_day_ == index || (node.IsFocused() && date == active_date_)) && enabled) {
        context.DrawRect(geometry.days[index], style_.hover_background,
                         std::min(style_.selection_corner_radius, style_.cell_size * 0.5F));
      }
      TextStyle text_style = style_.day_style;
      if (selected) {
        text_style.foreground = style_.selected_foreground;
      } else if (!enabled) {
        text_style.foreground = style_.disabled_foreground;
      } else if (date.month() != displayed_month_.month()) {
        text_style.foreground = style_.secondary_foreground;
      } else {
        text_style.foreground = style_.foreground;
      }
      context.DrawText(geometry.days[index], Number(static_cast<unsigned int>(date.day())), text_style, text_options);
    }
  }

  void BuildSemantics(SemanticBuilder& builder) const override {
    const DateGeometry geometry = MakeDateGeometry(bounds_, style_, locale_.right_to_left);
    Semantics owner;
    owner.role = SemanticRole::Grid;
    owner.label = accessible_label_;
    owner.value = FormatDate(value_, locale_);
    owner.invalid = invalid_;
    if (invalid_ && !validation_message_.empty()) {
      owner.error = validation_message_;
    }
    owner.collection = SemanticCollection{
        .item_count = year_mode_ ? std::optional<std::size_t>{20} : std::optional<std::size_t>{42},
        .row_count = year_mode_ ? std::optional<std::size_t>{5} : std::optional<std::size_t>{6},
        .column_count = year_mode_ ? std::optional<std::size_t>{4} : std::optional<std::size_t>{7},
    };
    builder.SetOwner(std::move(owner));

    const int page_offset = year_mode_ ? 20 * 12 : 1;
    const std::string& previous_label = year_mode_ ? locale_.previous_years_label : locale_.previous_month_label;
    const std::string& next_label = year_mode_ ? locale_.next_years_label : locale_.next_month_label;
    AddButtonSemantics(builder, 1, geometry.previous, previous_label, CanNavigate(-page_offset));
    AddButtonSemantics(builder, 2, geometry.title, locale_.choose_year_label, true);
    AddButtonSemantics(builder, 3, geometry.next, next_label, CanNavigate(page_offset));
    if (year_mode_) {
      const int first_year = YearPageStart();
      for (std::size_t index = 0; index < geometry.years.size(); ++index) {
        const int year = first_year + static_cast<int>(index);
        AddButtonSemantics(builder, 200U + index, geometry.years[index], std::to_string(year), YearAllowed(year));
      }
      return;
    }
    const SysDays first = GridStart();
    for (std::size_t index = 0; index < geometry.days.size(); ++index) {
      const Date date = FromSysDays(first + Days{static_cast<int>(index)});
      Semantics cell;
      cell.role = SemanticRole::GridCell;
      cell.label = FormatDate(date, locale_);
      cell.selected = date == value_;
      cell.collection_item = SemanticCollectionItem{
          .index = index,
          .row_index = index / 7U,
          .column_index = index % 7U,
      };
      builder.AddChild(100U + index, geometry.days[index], std::move(cell), IsSelectable(date));
      builder.AddAction(100U + index, SemanticActionKind::Activate);
    }
  }

  bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) override {
    if (action.kind != SemanticActionKind::Activate) {
      return false;
    }
    if (local_id == 1) {
      if (!CanNavigate(year_mode_ ? -20 * 12 : -1)) {
        return false;
      }
      if (year_mode_) {
        ChangeYearPage(-1);
      } else {
        ChangeMonth(-1);
      }
      return true;
    }
    if (local_id == 2) {
      year_mode_ = !year_mode_;
      active_year_ = static_cast<int>(displayed_month_.year());
      InvalidatePaint();
      InvalidateSemantics();
      return true;
    }
    if (local_id == 3) {
      if (!CanNavigate(year_mode_ ? 20 * 12 : 1)) {
        return false;
      }
      if (year_mode_) {
        ChangeYearPage(1);
      } else {
        ChangeMonth(1);
      }
      return true;
    }
    if (year_mode_ && local_id >= 200U && local_id < 220U) {
      ChooseYear(YearPageStart() + static_cast<int>(local_id - 200U));
      return true;
    }
    if (!year_mode_ && local_id >= 100U && local_id < 142U) {
      SelectDate(FromSysDays(GridStart() + Days{static_cast<int>(local_id - 100U)}));
      return true;
    }
    return false;
  }

  PaintInvalidation PrepareGeometry(MountedNode& node, TextMeasurer& text_measurer) override {
    static_cast<void>(text_measurer);
    const Rect next = node.Bounds();
    if (next == bounds_) {
      return PaintInvalidation::None;
    }
    bounds_ = next;
    InvalidateSemantics();
    return PaintInvalidation::Foreground;
  }

private:
  bool IsWithinRange(Date date) const {
    return (!minimum_.has_value() || ToSysDays(date) >= ToSysDays(*minimum_)) &&
           (!maximum_.has_value() || ToSysDays(date) <= ToSysDays(*maximum_));
  }

  bool IsSelectable(Date date) const {
    return date.ok() && IsWithinRange(date) && (!disabled_dates_ || !disabled_dates_(date));
  }

  bool CanShowMonth(YearMonth month) const {
    if (!month.ok()) {
      return false;
    }
    const Date first{month.year(), month.month(), std::chrono::day{1}};
    const Date last = ClampDay(month, DaysInMonth(month));
    return (!maximum_.has_value() || ToSysDays(first) <= ToSysDays(*maximum_)) &&
           (!minimum_.has_value() || ToSysDays(last) >= ToSysDays(*minimum_));
  }

  bool CanNavigate(int month_offset) const {
    if (year_mode_) {
      return ClosestAllowedYear(YearPageStart() + month_offset / 12,
                                static_cast<int>(displayed_month_.year()) + month_offset / 12)
          .has_value();
    }
    return CanShowMonth(displayed_month_ + std::chrono::months{month_offset});
  }

  SysDays GridStart() const {
    const Date first{displayed_month_.year(), displayed_month_.month(), std::chrono::day{1}};
    const unsigned int sunday_index = std::chrono::weekday{ToSysDays(first)}.c_encoding();
    const unsigned int offset = (sunday_index + 7U - locale_.first_weekday) % 7U;
    return ToSysDays(first) - Days{offset};
  }

  int YearPageStart() const {
    const int year = static_cast<int>(displayed_month_.year());
    const int remainder = ((year % 20) + 20) % 20;
    return year - remainder;
  }

  bool YearAllowed(int year) const {
    return year >= static_cast<int>(std::chrono::year::min()) &&
           year <= static_cast<int>(std::chrono::year::max()) &&
           (!minimum_.has_value() || year >= static_cast<int>(minimum_->year())) &&
           (!maximum_.has_value() || year <= static_cast<int>(maximum_->year()));
  }

  std::optional<int> ClosestAllowedYear(int page_start, int preferred_year) const {
    std::optional<int> result;
    int best_distance = std::numeric_limits<int>::max();
    for (int offset = 0; offset < 20; ++offset) {
      const int year = page_start + offset;
      if (!YearAllowed(year)) {
        continue;
      }
      const int distance = std::abs(year - preferred_year);
      if (distance < best_distance) {
        result = year;
        best_distance = distance;
      }
    }
    return result;
  }

  std::optional<YearMonth> ClosestVisibleMonth(int year, unsigned int preferred_month) const {
    std::optional<YearMonth> result;
    int best_distance = std::numeric_limits<int>::max();
    for (unsigned int month = 1; month <= 12; ++month) {
      const YearMonth candidate{std::chrono::year{year}, std::chrono::month{month}};
      if (!CanShowMonth(candidate)) {
        continue;
      }
      const int distance = std::abs(static_cast<int>(month) - static_cast<int>(preferred_month));
      if (distance < best_distance) {
        result = candidate;
        best_distance = distance;
      }
    }
    return result;
  }

  std::optional<Date> ClosestSelectableDate(YearMonth month, unsigned int preferred_day) const {
    const unsigned int last_day = DaysInMonth(month);
    const int preferred = static_cast<int>(std::clamp(preferred_day, 1U, last_day));
    for (int distance = 0; distance < static_cast<int>(last_day); ++distance) {
      const int earlier = preferred - distance;
      if (earlier >= 1) {
        const Date candidate{month.year(), month.month(), std::chrono::day{static_cast<unsigned int>(earlier)}};
        if (IsSelectable(candidate)) {
          return candidate;
        }
      }
      const int later = preferred + distance;
      if (distance != 0 && later <= static_cast<int>(last_day)) {
        const Date candidate{month.year(), month.month(), std::chrono::day{static_cast<unsigned int>(later)}};
        if (IsSelectable(candidate)) {
          return candidate;
        }
      }
    }
    return std::nullopt;
  }

  void ShowMonth(YearMonth month) {
    displayed_month_ = month;
    active_date_ = ClampDay(month, static_cast<unsigned int>(active_date_.day()));
    if (minimum_ && active_date_ < *minimum_) {
      active_date_ = *minimum_;
    }
    if (maximum_ && active_date_ > *maximum_) {
      active_date_ = *maximum_;
    }
    if (const std::optional<Date> active =
            ClosestSelectableDate(month, static_cast<unsigned int>(active_date_.day()));
        active.has_value()) {
      active_date_ = *active;
    }
    active_year_ = static_cast<int>(month.year());
  }

  void AddButtonSemantics(SemanticBuilder& builder, std::uint64_t id, Rect bounds, const std::string& label,
                          bool enabled) const {
    Semantics semantics;
    semantics.role = SemanticRole::Button;
    semantics.label = label;
    builder.AddChild(id, bounds, std::move(semantics), enabled);
    builder.AddAction(id, SemanticActionKind::Activate);
  }

  void PaintChevron(PaintContext& context, Rect bounds, int direction) const {
    const float size = std::min(bounds.width, bounds.height) * 0.18F;
    const Point center{bounds.x + bounds.width * 0.5F, bounds.y + bounds.height * 0.5F};
    const float offset = static_cast<float>(direction) * size * 0.5F;
    context.DrawLine({center.x - offset, center.y - size}, {center.x + offset, center.y}, style_.foreground,
                     StrokeStyle{.width = 1.8F, .cap = StrokeCap::Round});
    context.DrawLine({center.x + offset, center.y}, {center.x - offset, center.y + size}, style_.foreground,
                     StrokeStyle{.width = 1.8F, .cap = StrokeCap::Round});
  }

  void PaintYears(PaintContext& context, const DateGeometry& geometry, const TextLayoutOptions& options) const {
    const int first_year = YearPageStart();
    for (std::size_t index = 0; index < geometry.years.size(); ++index) {
      const int year = first_year + static_cast<int>(index);
      const bool selected = year == static_cast<int>(displayed_month_.year());
      const bool enabled = YearAllowed(year);
      if (selected) {
        context.DrawRect(geometry.years[index], style_.selected_background,
                         std::min(style_.selection_corner_radius, geometry.years[index].height * 0.5F));
      } else if (active_year_ == year) {
        context.DrawRect(geometry.years[index], style_.hover_background,
                         std::min(style_.selection_corner_radius, geometry.years[index].height * 0.5F));
      }
      TextStyle text_style = style_.day_style;
      text_style.foreground = selected ? style_.selected_foreground
                                       : (enabled ? style_.foreground : style_.disabled_foreground);
      context.DrawText(geometry.years[index], std::to_string(year), text_style, options);
    }
  }

  void PerformAt(Rect bounds, Point position) {
    const DateGeometry geometry = MakeDateGeometry(bounds, style_, locale_.right_to_left);
    if (geometry.previous.Contains(position)) {
      if (year_mode_) {
        ChangeYearPage(-1);
      } else {
        ChangeMonth(-1);
      }
      return;
    }
    if (geometry.next.Contains(position)) {
      if (year_mode_) {
        ChangeYearPage(1);
      } else {
        ChangeMonth(1);
      }
      return;
    }
    if (geometry.title.Contains(position)) {
      year_mode_ = !year_mode_;
      active_year_ = static_cast<int>(displayed_month_.year());
      InvalidatePaint();
      InvalidateSemantics();
      return;
    }
    if (year_mode_) {
      for (std::size_t index = 0; index < geometry.years.size(); ++index) {
        if (geometry.years[index].Contains(position)) {
          ChooseYear(YearPageStart() + static_cast<int>(index));
          return;
        }
      }
      return;
    }
    const SysDays first = GridStart();
    for (std::size_t index = 0; index < geometry.days.size(); ++index) {
      if (geometry.days[index].Contains(position)) {
        SelectDate(FromSysDays(first + Days{static_cast<int>(index)}));
        return;
      }
    }
  }

  void SelectDate(Date date) {
    if (!IsSelectable(date)) {
      return;
    }
    active_date_ = date;
    displayed_month_ = YearMonth{date.year(), date.month()};
    if (date != value_) {
      events_.Emit<DatePickerEvents::Changed>(date);
    }
    InvalidatePaint();
    InvalidateSemantics();
  }

  void MoveActive(Days delta) {
    const int direction = delta.count() < 0 ? -1 : 1;
    SysDays candidate = ToSysDays(active_date_) + delta;
    for (int attempt = 0; attempt < 3660; ++attempt) {
      const Date date = FromSysDays(candidate);
      if ((minimum_.has_value() && candidate < ToSysDays(*minimum_)) ||
          (maximum_.has_value() && candidate > ToSysDays(*maximum_))) {
        return;
      }
      if (IsSelectable(date)) {
        active_date_ = date;
        displayed_month_ = YearMonth{date.year(), date.month()};
        InvalidatePaint();
        InvalidateSemantics();
        return;
      }
      candidate += Days{direction};
    }
  }

  void MoveToWeekEdge(bool end) {
    const unsigned int weekday = std::chrono::weekday{ToSysDays(active_date_)}.c_encoding();
    const unsigned int offset = (weekday + 7U - locale_.first_weekday) % 7U;
    const int delta = end ? static_cast<int>(6U - offset) : -static_cast<int>(offset);
    MoveActive(Days{delta});
  }

  void ChangeMonth(int offset) {
    const YearMonth target = displayed_month_ + std::chrono::months{offset};
    if (!CanShowMonth(target)) {
      return;
    }
    ShowMonth(target);
    InvalidatePaint();
    InvalidateSemantics();
  }

  void ChangeYearPage(int direction) {
    const int preferred_year = static_cast<int>(displayed_month_.year()) + direction * 20;
    const std::optional<int> year = ClosestAllowedYear(YearPageStart() + direction * 20, preferred_year);
    if (!year.has_value()) {
      return;
    }
    const std::optional<YearMonth> target =
        ClosestVisibleMonth(*year, static_cast<unsigned int>(displayed_month_.month()));
    if (!target.has_value()) {
      return;
    }
    ShowMonth(*target);
    active_year_ = *year;
    InvalidatePaint();
    InvalidateSemantics();
  }

  void MoveActiveYear(int offset) {
    const int candidate = active_year_ + offset;
    if (candidate < YearPageStart() || candidate > YearPageStart() + 19 || !YearAllowed(candidate)) {
      return;
    }
    active_year_ = candidate;
    InvalidatePaint();
    InvalidateSemantics();
  }

  void ChooseYear(int year) {
    if (!YearAllowed(year)) {
      return;
    }
    const std::optional<YearMonth> target =
        ClosestVisibleMonth(year, static_cast<unsigned int>(displayed_month_.month()));
    if (!target.has_value()) {
      return;
    }
    ShowMonth(*target);
    active_year_ = year;
    year_mode_ = false;
    InvalidatePaint();
    InvalidateSemantics();
  }

  Date value_{std::chrono::year{1970}, std::chrono::January, std::chrono::day{1}};
  Date active_date_{value_};
  YearMonth displayed_month_{std::chrono::year{1970}, std::chrono::January};
  std::optional<Date> minimum_;
  std::optional<Date> maximum_;
  std::function<bool(Date)> disabled_dates_;
  DatePickerStyle style_;
  DateTimeLocaleData locale_;
  EventEmitter events_;
  std::string accessible_label_;
  std::string validation_message_;
  std::optional<std::size_t> hover_day_;
  std::optional<std::int64_t> pointer_id_;
  Rect bounds_;
  bool invalid_ = false;
  bool year_mode_ = false;
  bool initialized_ = false;
  int active_year_ = 1970;
};

const detail::ModifierDescriptor& DatePickerBehavior::Descriptor() {
  return detail::ModifierDescriptorFor<DatePickerBehavior, DatePickerBehaviorExtension>();
}

enum class TimeSelectionMode {
  Hour,
  Minute,
};

struct TimePickerBehavior {
  static const detail::ModifierDescriptor& Descriptor();

  Minutes value;
  Minutes step{1};
  std::function<bool(Minutes)> disabled_times;
  TimePickerStyle style;
  DateTimeLocaleData locale;
  EventEmitter events;
  std::string accessible_label;
  bool invalid = false;
  std::string validation_message;
};

struct TimeGeometry {
  Rect hour;
  Rect separator;
  Rect minute;
  Rect am;
  Rect pm;
  Rect period;
  Point dial_center;
  float dial_radius = 0.0F;
};

float TimeHeaderWidth(const TimePickerStyle& style, bool use_12_hour) {
  return style.field_width * 2.0F + style.separator_width +
         (use_12_hour ? style.period_spacing + style.period_width : 0.0F);
}

TimeGeometry MakeTimeGeometry(Rect bounds, const TimePickerStyle& style, bool use_12_hour) {
  TimeGeometry result;
  const float content_x = bounds.x + style.padding.left;
  const float content_y = bounds.y + style.padding.top;
  const float content_width = std::max(0.0F, bounds.width - style.padding.Horizontal());
  const float field_width = style.field_width;
  const float separator_width = style.separator_width;
  const float fields_width = field_width * 2.0F + separator_width;
  const float fields_x = content_x + (content_width - TimeHeaderWidth(style, use_12_hour)) * 0.5F;
  result.hour = {fields_x, content_y, field_width, style.header_height};
  result.separator = {fields_x + field_width, content_y, separator_width, style.header_height};
  result.minute = {fields_x + field_width + separator_width, content_y, field_width, style.header_height};
  if (use_12_hour) {
    const float period_x = fields_x + fields_width + style.period_spacing;
    result.period = {period_x, content_y, style.period_width, style.header_height};
    result.am = {period_x, content_y, style.period_width, style.header_height * 0.5F};
    result.pm = {period_x, content_y + style.header_height * 0.5F, style.period_width, style.header_height * 0.5F};
  }
  const float dial_top = content_y + style.header_height + style.content_spacing;
  result.dial_center = {content_x + content_width * 0.5F, dial_top + style.dial_size * 0.5F};
  result.dial_radius = style.dial_size * 0.5F;
  return result;
}

std::string FormatTime(Minutes value, bool use_12_hour, const DateTimeLocaleData& locale) {
  const int total = static_cast<int>(value.count());
  const int hour = total / 60;
  const int minute = total % 60;
  if (!use_12_hour) {
    return Number(static_cast<unsigned int>(hour), 2) + ":" + Number(static_cast<unsigned int>(minute), 2);
  }
  const int display_hour = hour % 12 == 0 ? 12 : hour % 12;
  return Number(static_cast<unsigned int>(display_hour), 2) + ":" + Number(static_cast<unsigned int>(minute), 2) +
         " " + (hour < 12 ? locale.am_label : locale.pm_label);
}

struct DialLabel {
  std::uint64_t local_id = 0;
  std::string text;
  Rect bounds;
  bool selected = false;
  bool enabled = true;
};

class TimePickerBehaviorExtension final : public NodeExtension {
public:
  TimePickerBehaviorExtension(MountedNode& node, const TimePickerBehavior& value) {
    Update(node, value);
  }

  void Update(MountedNode& node, const TimePickerBehavior& value) {
    static_cast<void>(node);
    const bool controlled_changed = !initialized_ || value_ != value.value;
    value_ = value.value;
    step_ = value.step;
    disabled_times_ = value.disabled_times;
    style_ = value.style;
    locale_ = value.locale;
    events_ = value.events;
    accessible_label_ = value.accessible_label.empty() ? locale_.time_picker_label : value.accessible_label;
    invalid_ = value.invalid;
    validation_message_ = value.validation_message;
    if (controlled_changed) {
      last_emitted_.reset();
    }
    // Availability predicates cannot be compared; every configuration update refreshes the cached labels.
    geometry_prepared_ = false;
    initialized_ = true;
    InvalidatePaint();
    InvalidateSemantics();
  }

  [[nodiscard]] bool HitTest(MountedNode& node, Point position) const override {
    return node.IsEnabled() && node.Bounds().Contains(position);
  }

  PointerResult OnPointer(MountedNode& node, const PointerEvent& event) override {
    if (!node.IsEnabled()) {
      pointer_id_.reset();
      last_emitted_.reset();
      pressed_period_.reset();
      return PointerResult::Ignored;
    }
    const TimeGeometry geometry = MakeTimeGeometry(node.Bounds(), style_, locale_.use_12_hour);
    if (event.type == PointerEventType::Down) {
      if (pointer_id_) {
        return PointerResult::Ignored;
      }
      if (geometry.hour.Contains(event.position)) {
        SetMode(TimeSelectionMode::Hour);
        return PointerResult::Handled;
      }
      if (geometry.minute.Contains(event.position)) {
        SetMode(TimeSelectionMode::Minute);
        return PointerResult::Handled;
      }
      if (locale_.use_12_hour && (geometry.am.Contains(event.position) || geometry.pm.Contains(event.position))) {
        const bool pm = geometry.pm.Contains(event.position);
        if (!CandidateForPeriod(pm)) {
          return PointerResult::Handled;
        }
        pointer_id_ = event.pointer_id;
        pressed_period_ = pm;
        return PointerResult::Capture;
      }
      if (Distance(event.position, geometry.dial_center) <= geometry.dial_radius) {
        pointer_id_ = event.pointer_id;
        last_emitted_.reset();
        EmitDialValue(geometry, event.position);
        return PointerResult::Capture;
      }
      return PointerResult::Ignored;
    }
    if (!pointer_id_.has_value() || *pointer_id_ != event.pointer_id) {
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Cancel) {
      pointer_id_.reset();
      last_emitted_.reset();
      pressed_period_.reset();
      return PointerResult::Handled;
    }
    if (pressed_period_.has_value()) {
      if (event.type == PointerEventType::Up) {
        const bool pm = *pressed_period_;
        pointer_id_.reset();
        pressed_period_.reset();
        if (locale_.use_12_hour && (pm ? geometry.pm : geometry.am).Contains(event.position)) {
          SetPeriod(pm);
        }
        return PointerResult::Handled;
      }
      return PointerResult::Capture;
    }
    EmitDialValue(geometry, event.position);
    if (event.type == PointerEventType::Up) {
      pointer_id_.reset();
      last_emitted_.reset();
      if (mode_ == TimeSelectionMode::Hour) {
        SetMode(TimeSelectionMode::Minute);
      }
      return PointerResult::Handled;
    }
    return PointerResult::Capture;
  }

  bool OnKey(MountedNode& node, const KeyEvent& event) override {
    static_cast<void>(node);
    if (event.type != KeyEventType::Down || event.modifiers.alt || event.modifiers.control || event.modifiers.meta) {
      return false;
    }
    if (event.text.size() == 1U && event.text.front() >= '0' && event.text.front() <= '9') {
      ApplyDigit(event.text.front() - '0');
      return true;
    }
    digit_buffer_.clear();
    switch (event.key) {
    case Key::ArrowLeft:
    case Key::ArrowDown:
      Increment(-1);
      return true;
    case Key::ArrowRight:
    case Key::ArrowUp:
      Increment(1);
      return true;
    case Key::Home:
      EmitFirstEnabled(false);
      return true;
    case Key::End:
      EmitFirstEnabled(true);
      return true;
    case Key::Enter:
    case Key::Space:
      SetMode(mode_ == TimeSelectionMode::Hour ? TimeSelectionMode::Minute : TimeSelectionMode::Hour);
      return true;
    case Key::Escape:
      SetMode(TimeSelectionMode::Hour);
      return true;
    default:
      return false;
    }
  }

  PaintInvalidation PrepareGeometry(MountedNode& node, TextMeasurer& text_measurer) override {
    const Rect bounds = node.Bounds();
    if (geometry_prepared_ && bounds_ == bounds) {
      return PaintInvalidation::None;
    }
    const TimeGeometry geometry = MakeTimeGeometry(bounds, style_, locale_.use_12_hour);
    std::vector<DialLabel> labels;
    if (mode_ == TimeSelectionMode::Hour) {
      const int count = locale_.use_12_hour ? 12 : 24;
      labels.reserve(static_cast<std::size_t>(count));
      for (int hour = 0; hour < count; ++hour) {
        const int display_value = locale_.use_12_hour ? (hour == 0 ? 12 : hour) : hour;
        const bool inner = !locale_.use_12_hour && (hour == 0 || hour >= 13);
        const float radius = geometry.dial_radius * (inner ? 0.52F : 0.78F);
        const int ring_value = hour % 12;
        const Point center = DialPoint(geometry.dial_center, radius, ring_value, 12);
        const std::string label = Number(static_cast<unsigned int>(display_value), locale_.use_12_hour ? 0U : 2U);
        labels.push_back(MakeDialLabel(text_measurer, 100U + static_cast<std::uint64_t>(hour), label, center,
                                       HourSelected(hour), HourEnabled(hour)));
      }
    } else {
      labels.reserve(12);
      for (int minute = 0; minute < 60; minute += 5) {
        const Point center = DialPoint(geometry.dial_center, geometry.dial_radius * 0.78F, minute, 60);
        labels.push_back(MakeDialLabel(text_measurer, 200U + static_cast<std::uint64_t>(minute),
                                       Number(static_cast<unsigned int>(minute), 2), center, MinuteSelected(minute),
                                       TimeEnabled(Minutes{Hour() * 60 + minute})));
      }
    }
    bounds_ = bounds;
    dial_labels_ = std::move(labels);
    geometry_prepared_ = true;
    InvalidateSemantics();
    return PaintInvalidation::Foreground;
  }

  void PaintAboveContent(const MountedNode& node, PaintContext& context) const override {
    const TimeGeometry geometry = MakeTimeGeometry(node.Bounds(), style_, locale_.use_12_hour);
    const TextLayoutOptions options = CenteredText(locale_);
    PaintHeader(context, geometry, options);
    context.DrawCircle(geometry.dial_center, geometry.dial_radius, style_.dial_background);

    const Point selected_point = SelectedDialPoint(geometry);
    context.DrawLine(geometry.dial_center, selected_point, style_.hand,
                     StrokeStyle{.width = style_.hand_width, .cap = StrokeCap::Round});
    context.DrawCircle(geometry.dial_center, std::max(2.0F, style_.hand_width), style_.hand);
    context.DrawCircle(selected_point, style_.selection_radius, style_.selected_background);

    for (const DialLabel& label : dial_labels_) {
      TextStyle text_style = style_.dial_style;
      text_style.foreground = label.selected ? style_.selected_foreground
                                             : (label.enabled ? style_.foreground : style_.disabled_foreground);
      context.DrawText(label.bounds, label.text, text_style, options);
    }
  }

  void BuildSemantics(SemanticBuilder& builder) const override {
    const TimeGeometry geometry = MakeTimeGeometry(bounds_, style_, locale_.use_12_hour);
    Semantics owner;
    owner.role = SemanticRole::Grid;
    owner.label = accessible_label_;
    owner.value = FormatTime(value_, locale_.use_12_hour, locale_);
    owner.invalid = invalid_;
    if (invalid_ && !validation_message_.empty()) {
      owner.error = validation_message_;
    }
    const std::size_t item_count = mode_ == TimeSelectionMode::Hour
                                       ? dial_labels_.size()
                                       : static_cast<std::size_t>(60 / step_.count());
    owner.collection = SemanticCollection{.item_count = item_count};
    builder.SetOwner(std::move(owner));
    builder.AddAction(0, SemanticActionKind::Increment);
    builder.AddAction(0, SemanticActionKind::Decrement);
    builder.AddAction(0, SemanticActionKind::SetValue);

    AddHeaderSemantics(builder, 1, geometry.hour, locale_.hour_label, mode_ == TimeSelectionMode::Hour, true);
    AddHeaderSemantics(builder, 2, geometry.minute, locale_.minute_label, mode_ == TimeSelectionMode::Minute, true);
    if (locale_.use_12_hour) {
      AddHeaderSemantics(builder, 10, geometry.am, locale_.am_label, Hour() < 12,
                         CandidateForPeriod(false).has_value());
      AddHeaderSemantics(builder, 11, geometry.pm, locale_.pm_label, Hour() >= 12,
                         CandidateForPeriod(true).has_value());
    }
    if (mode_ == TimeSelectionMode::Hour) {
      for (std::size_t index = 0; index < dial_labels_.size(); ++index) {
        const DialLabel& label = dial_labels_[index];
        AddDialSemantics(builder, label.local_id, label.bounds, label.text, label.selected, label.enabled, index);
      }
      return;
    }
    const int step = static_cast<int>(step_.count());
    std::size_t index = 0;
    for (int minute = 0; minute < 60; minute += step, ++index) {
      const Point center = DialPoint(geometry.dial_center, geometry.dial_radius * 0.78F, minute, 60);
      const float diameter = style_.selection_radius * 2.0F;
      const Rect bounds{center.x - style_.selection_radius, center.y - style_.selection_radius, diameter, diameter};
      const Minutes time{Hour() * 60 + minute};
      AddDialSemantics(builder, 200U + static_cast<std::uint64_t>(minute), bounds,
                       Number(static_cast<unsigned int>(minute), 2), MinuteSelected(minute), TimeEnabled(time), index);
    }
  }

  bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) override {
    if (local_id == 0) {
      if (action.kind == SemanticActionKind::Increment) {
        Increment(1);
        return true;
      }
      if (action.kind == SemanticActionKind::Decrement) {
        Increment(-1);
        return true;
      }
      if (action.kind == SemanticActionKind::SetValue) {
        const auto* value = std::get_if<double>(&action.value);
        if (value == nullptr || !std::isfinite(*value) || *value < 0.0 ||
            *value >= static_cast<double>(minutes_per_day.count())) {
          return false;
        }
        const auto requested = Minutes{static_cast<Minutes::rep>(std::llround(*value))};
        if (!IsValidTime(requested) || requested.count() % step_.count() != 0 || !TimeEnabled(requested)) {
          return false;
        }
        EmitTime(requested);
        return true;
      }
      return false;
    }
    if (action.kind != SemanticActionKind::Activate) {
      return false;
    }
    if (local_id == 1) {
      SetMode(TimeSelectionMode::Hour);
      return true;
    }
    if (local_id == 2) {
      SetMode(TimeSelectionMode::Minute);
      return true;
    }
    if (local_id == 10 || local_id == 11) {
      return SetPeriod(local_id == 11);
    }
    if (local_id >= 100U && local_id < 124U) {
      SelectHour(static_cast<int>(local_id - 100U));
      return true;
    }
    if (local_id >= 200U && local_id < 260U) {
      SelectMinute(static_cast<int>(local_id - 200U));
      return true;
    }
    return false;
  }

private:
  static float Distance(Point left, Point right) {
    return std::hypot(left.x - right.x, left.y - right.y);
  }

  static Point DialPoint(Point center, float radius, int value, int divisions) {
    const float angle = static_cast<float>(value) / static_cast<float>(divisions) * full_circle - pi * 0.5F;
    return {center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius};
  }

  static int DialValue(Point center, Point point, int divisions) {
    float angle = std::atan2(point.y - center.y, point.x - center.x) + pi * 0.5F;
    if (angle < 0.0F) {
      angle += full_circle;
    }
    return static_cast<int>(std::lround(angle / full_circle * static_cast<float>(divisions))) % divisions;
  }

  int Hour() const {
    return static_cast<int>(value_.count() / 60);
  }

  int Minute() const {
    return static_cast<int>(value_.count() % 60);
  }

  bool IsValidTime(Minutes value) const {
    return value >= Minutes{0} && value < minutes_per_day;
  }

  bool TimeEnabled(Minutes value) const {
    return IsValidTime(value) && (!disabled_times_ || !disabled_times_(value));
  }

  std::optional<Minutes> CandidateForHour(int hour, int preferred_minute) const {
    if (hour < 0 || hour >= 24) {
      return std::nullopt;
    }
    const int step = static_cast<int>(step_.count());
    const int slots = 60 / step;
    std::optional<Minutes> best;
    int best_distance = std::numeric_limits<int>::max();
    for (int slot = 0; slot < slots; ++slot) {
      const int minute = slot * step;
      const Minutes candidate{hour * 60 + minute};
      const int distance = std::abs(minute - preferred_minute);
      if (TimeEnabled(candidate) && distance < best_distance) {
        best = candidate;
        best_distance = distance;
      }
    }
    return best;
  }

  bool HourEnabled(int displayed_hour) const {
    int hour = displayed_hour;
    if (locale_.use_12_hour) {
      const bool pm = Hour() >= 12;
      hour = displayed_hour % 12 + (pm ? 12 : 0);
    }
    return CandidateForHour(hour, Minute()).has_value();
  }

  bool HourSelected(int displayed_hour) const {
    return locale_.use_12_hour ? Hour() % 12 == displayed_hour % 12 : Hour() == displayed_hour;
  }

  bool MinuteSelected(int minute) const {
    return Minute() == minute;
  }

  DialLabel MakeDialLabel(TextMeasurer& text_measurer, std::uint64_t local_id, std::string text,
                          Point center, bool selected, bool enabled) const {
    const TextLayoutMetrics metrics = text_measurer.MeasureText(
        text, style_.dial_style, std::numeric_limits<float>::infinity(), CenteredText(locale_)
    );
    const float width = std::max(metrics.size.width, style_.selection_radius * 2.0F);
    const float height = std::max(metrics.size.height, style_.selection_radius * 2.0F);
    return {
        .local_id = local_id,
        .text = std::move(text),
        .bounds = {center.x - width * 0.5F, center.y - height * 0.5F, width, height},
        .selected = selected,
        .enabled = enabled,
    };
  }

  Point SelectedDialPoint(const TimeGeometry& geometry) const {
    if (mode_ == TimeSelectionMode::Minute) {
      return DialPoint(geometry.dial_center, geometry.dial_radius * 0.78F, Minute(), 60);
    }
    const bool inner = !locale_.use_12_hour && (Hour() == 0 || Hour() >= 13);
    return DialPoint(geometry.dial_center, geometry.dial_radius * (inner ? 0.52F : 0.78F), Hour() % 12, 12);
  }

  void PaintHeader(PaintContext& context, const TimeGeometry& geometry, const TextLayoutOptions& options) const {
    TextStyle hour_style = style_.header_style;
    TextStyle minute_style = style_.header_style;
    const bool selecting_hour = mode_ == TimeSelectionMode::Hour;
    context.DrawRect(geometry.hour, selecting_hour ? style_.selected_field_background : style_.field_background,
                     style_.field_corner_radius);
    context.DrawRect(geometry.minute, selecting_hour ? style_.field_background : style_.selected_field_background,
                     style_.field_corner_radius);
    if (mode_ == TimeSelectionMode::Hour) {
      hour_style.foreground = style_.selected_field_foreground;
    } else {
      minute_style.foreground = style_.selected_field_foreground;
    }
    const int display_hour = locale_.use_12_hour ? (Hour() % 12 == 0 ? 12 : Hour() % 12) : Hour();
    context.DrawText(geometry.hour, Number(static_cast<unsigned int>(display_hour), 2), hour_style, options);
    context.DrawText(geometry.separator, ":", style_.header_style, options);
    context.DrawText(geometry.minute, Number(static_cast<unsigned int>(Minute()), 2), minute_style, options);
    if (locale_.use_12_hour) {
      context.PushClip(geometry.period, style_.period_corner_radius);
      context.DrawRect(Hour() < 12 ? geometry.am : geometry.pm, style_.selected_period_background);
      context.PopClip();
      if (style_.period_border_width > 0.0F) {
        const StrokeStyle stroke{.width = style_.period_border_width};
        context.DrawBorder(geometry.period, style_.period_border, stroke, style_.period_corner_radius);
        context.DrawLine({geometry.pm.x, geometry.pm.y},
                         {geometry.pm.x + geometry.pm.width, geometry.pm.y}, style_.period_border, stroke);
      }
      TextStyle period_style = style_.period_style;
      if (!CandidateForPeriod(false)) {
        period_style.foreground = style_.disabled_foreground;
      } else if (Hour() < 12) {
        period_style.foreground = style_.selected_period_foreground;
      }
      context.DrawText(geometry.am, locale_.am_label, period_style, options);
      period_style = style_.period_style;
      if (!CandidateForPeriod(true)) {
        period_style.foreground = style_.disabled_foreground;
      } else if (Hour() >= 12) {
        period_style.foreground = style_.selected_period_foreground;
      }
      context.DrawText(geometry.pm, locale_.pm_label, period_style, options);
    }
  }

  void AddHeaderSemantics(SemanticBuilder& builder, std::uint64_t id, Rect bounds, const std::string& label,
                          bool selected, bool enabled) const {
    Semantics semantics;
    semantics.role = SemanticRole::Button;
    semantics.label = label;
    semantics.selected = selected;
    builder.AddChild(id, bounds, std::move(semantics), enabled);
    builder.AddAction(id, SemanticActionKind::Activate);
  }

  void AddDialSemantics(SemanticBuilder& builder, std::uint64_t id, Rect bounds, const std::string& label,
                        bool selected, bool enabled, std::size_t index) const {
    Semantics item;
    item.role = SemanticRole::GridCell;
    item.label = label;
    item.selected = selected;
    item.collection_item = SemanticCollectionItem{.index = index};
    builder.AddChild(id, bounds, std::move(item), enabled);
    builder.AddAction(id, SemanticActionKind::Activate);
  }

  void SetMode(TimeSelectionMode mode) {
    if (mode_ == mode) {
      return;
    }
    mode_ = mode;
    digit_buffer_.clear();
    geometry_prepared_ = false;
    InvalidatePaint();
    InvalidateSemantics();
  }

  std::optional<Minutes> CandidateForPeriod(bool pm) const {
    return CandidateForHour(Hour() % 12 + (pm ? 12 : 0), Minute());
  }

  bool SetPeriod(bool pm) {
    if (const std::optional<Minutes> candidate = CandidateForPeriod(pm)) {
      EmitTime(*candidate);
      return true;
    }
    return false;
  }

  void EmitDialValue(const TimeGeometry& geometry, Point position) {
    if (mode_ == TimeSelectionMode::Hour) {
      int hour = DialValue(geometry.dial_center, position, 12);
      if (locale_.use_12_hour) {
        hour += Hour() >= 12 ? 12 : 0;
      } else if (Distance(position, geometry.dial_center) < geometry.dial_radius * 0.66F) {
        hour = hour == 0 ? 0 : hour + 12;
      } else {
        hour = hour == 0 ? 12 : hour;
      }
      SelectHour(hour);
      return;
    }
    const int raw_minute = DialValue(geometry.dial_center, position, 60);
    const int step = static_cast<int>(step_.count());
    const int snapped = static_cast<int>(std::lround(static_cast<float>(raw_minute) / static_cast<float>(step))) * step;
    SelectMinute(snapped % 60);
  }

  void SelectHour(int hour) {
    if (locale_.use_12_hour && hour < 12) {
      hour = hour % 12 + (Hour() >= 12 ? 12 : 0);
    }
    if (const std::optional<Minutes> candidate = CandidateForHour(hour, Minute())) {
      EmitTime(*candidate);
    }
  }

  void SelectMinute(int minute) {
    if (minute < 0 || minute >= 60 || minute % step_.count() != 0) {
      return;
    }
    EmitTime(Minutes{Hour() * 60 + minute});
  }

  void EmitTime(Minutes value) {
    if (!IsValidTime(value) || value.count() % step_.count() != 0 || !TimeEnabled(value)) {
      return;
    }
    if (value != value_ && (!pointer_id_.has_value() || last_emitted_ != value)) {
      if (pointer_id_.has_value()) {
        last_emitted_ = value;
      }
      events_.Emit<TimePickerEvents::Changed>(value);
    }
  }

  void Increment(int direction) {
    if (mode_ == TimeSelectionMode::Hour) {
      int hour = Hour();
      for (int attempt = 0; attempt < 24; ++attempt) {
        hour = (hour + direction + 24) % 24;
        if (const std::optional<Minutes> candidate = CandidateForHour(hour, Minute())) {
          EmitTime(*candidate);
          return;
        }
      }
      return;
    }
    const int increment = static_cast<int>(step_.count());
    int value = static_cast<int>(value_.count());
    const int attempts = static_cast<int>(minutes_per_day.count()) / static_cast<int>(step_.count());
    for (int attempt = 0; attempt < attempts; ++attempt) {
      value = (value + direction * increment + static_cast<int>(minutes_per_day.count())) %
              static_cast<int>(minutes_per_day.count());
      const Minutes candidate{value};
      if (candidate.count() % step_.count() == 0 && TimeEnabled(candidate)) {
        EmitTime(candidate);
        return;
      }
    }
  }

  void EmitFirstEnabled(bool reverse) {
    const int step = static_cast<int>(step_.count());
    if (reverse) {
      for (int value = static_cast<int>(minutes_per_day.count()) - step; value >= 0; value -= step) {
        if (TimeEnabled(Minutes{value})) {
          EmitTime(Minutes{value});
          return;
        }
      }
      return;
    }
    for (int value = 0; value < minutes_per_day.count(); value += step) {
      if (TimeEnabled(Minutes{value})) {
        EmitTime(Minutes{value});
        return;
      }
    }
  }

  void ApplyDigit(int digit) {
    digit_buffer_.push_back(static_cast<char>('0' + digit));
    if (digit_buffer_.size() > 2U) {
      digit_buffer_.erase(digit_buffer_.begin());
    }
    int value = std::stoi(digit_buffer_);
    const int maximum = mode_ == TimeSelectionMode::Hour ? (locale_.use_12_hour ? 12 : 23) : 59;
    if (value > maximum || (mode_ == TimeSelectionMode::Hour && locale_.use_12_hour && value == 0)) {
      digit_buffer_.assign(1, static_cast<char>('0' + digit));
      value = digit;
    }
    if (mode_ == TimeSelectionMode::Hour) {
      if (locale_.use_12_hour && value >= 1 && value <= 12) {
        SelectHour(value % 12);
      } else if (!locale_.use_12_hour && value <= 23) {
        SelectHour(value);
      }
    } else if (value <= 59) {
      const int step = static_cast<int>(step_.count());
      const int snapped = std::clamp(
          static_cast<int>(std::lround(static_cast<float>(value) / step)) * step,
          0,
          60 - step
      );
      SelectMinute(snapped);
    }
    if (digit_buffer_.size() == 2U) {
      digit_buffer_.clear();
    }
  }

  Minutes value_{0};
  Minutes step_{1};
  std::function<bool(Minutes)> disabled_times_;
  TimePickerStyle style_;
  DateTimeLocaleData locale_;
  EventEmitter events_;
  std::string accessible_label_;
  std::string validation_message_;
  std::string digit_buffer_;
  std::vector<DialLabel> dial_labels_;
  std::optional<Minutes> last_emitted_;
  std::optional<std::int64_t> pointer_id_;
  std::optional<bool> pressed_period_;
  Rect bounds_;
  TimeSelectionMode mode_ = TimeSelectionMode::Hour;
  bool invalid_ = false;
  bool initialized_ = false;
  bool geometry_prepared_ = false;
};

const detail::ModifierDescriptor& TimePickerBehavior::Descriptor() {
  return detail::ModifierDescriptorFor<TimePickerBehavior, TimePickerBehaviorExtension>();
}

std::function<View()> MakeDatePickerScopeFactory(DatePickerConfiguration configuration) {
  return [configuration = std::move(configuration)]() -> View {
    const std::shared_ptr<const Environment> environment = detail::CurrentEnvironment();
    const DatePickerStyle style = ResolveDatePickerStyle(environment);
    ValidateDatePickerStyle(style);
    const DateTimeLocaleData locale = ResolveDateTimeLocale(UseEnvironment<Locale>());
    const EventEmitter events = UseEvents();
    const std::string label = UseString(configuration.label);
    const std::string validation_message = UseString(configuration.validation.message);
    const float width = style.padding.Horizontal() + style.cell_size * 7.0F + style.column_spacing * 6.0F;
    const float height = style.padding.Vertical() + style.header_height + style.cell_size * 7.0F +
                         style.row_spacing * 6.0F;

    View picker = Canvas([](PaintContext&, Size) {}).With(
        Frame{.width = width, .height = height},
        Background{style.background},
        Border{configuration.validation.IsInvalid() ? style.validation_error : style.border,
               configuration.validation.IsInvalid() ? std::max(1.0F, style.border_width) : style.border_width},
        CornerRadius{style.corner_radius},
        ClipChildren{},
        Focusable{},
        PointerCursor{PointerCursorKind::Hand},
        DatePickerBehavior{
            .value = configuration.value,
            .minimum = configuration.minimum,
            .maximum = configuration.maximum,
            .disabled_dates = configuration.disabled_dates,
            .style = style,
            .locale = locale,
            .events = events,
            .accessible_label = label,
            .invalid = configuration.validation.IsInvalid(),
            .validation_message = validation_message,
        }
    );
    View field = std::move(picker);
    if (!label.empty()) {
      field = Column {
        Text(label).Style(style.label_style),
        std::move(field),
      }.With(Spacing{style.label_spacing}, CrossAlign{CrossAxisAlignment::Start});
    }
    if (!validation_message.empty() &&
        (configuration.validation.status == ValidationStatus::Invalid ||
         configuration.validation.status == ValidationStatus::Pending)) {
      return Column {
        std::move(field),
        Text(validation_message).Style(style.validation_text_style),
      }.With(Spacing{style.validation_spacing}, CrossAlign{CrossAxisAlignment::Start});
    }
    return field;
  };
}

std::function<View()> MakeTimePickerScopeFactory(TimePickerConfiguration configuration) {
  return [configuration = std::move(configuration)]() -> View {
    const std::shared_ptr<const Environment> environment = detail::CurrentEnvironment();
    const TimePickerStyle style = ResolveTimePickerStyle(environment);
    ValidateTimePickerStyle(style);
    const DateTimeLocaleData locale = ResolveDateTimeLocale(UseEnvironment<Locale>());
    const EventEmitter events = UseEvents();
    const std::string label = UseString(configuration.label);
    const std::string validation_message = UseString(configuration.validation.message);
    const float width = style.padding.Horizontal() +
                        std::max(style.dial_size, TimeHeaderWidth(style, locale.use_12_hour));
    const float height = style.padding.Vertical() + style.header_height + style.content_spacing + style.dial_size;

    View picker = Canvas([](PaintContext&, Size) {}).With(
        Frame{.width = width, .height = height},
        Background{style.background},
        Border{configuration.validation.IsInvalid() ? style.validation_error : style.border,
               configuration.validation.IsInvalid() ? std::max(1.0F, style.border_width) : style.border_width},
        CornerRadius{style.corner_radius},
        ClipChildren{},
        Focusable{},
        PointerCursor{PointerCursorKind::Hand},
        TimePickerBehavior{
            .value = configuration.value,
            .step = configuration.step,
            .disabled_times = configuration.disabled_times,
            .style = style,
            .locale = locale,
            .events = events,
            .accessible_label = label,
            .invalid = configuration.validation.IsInvalid(),
            .validation_message = validation_message,
        }
    );
    View field = std::move(picker);
    if (!label.empty()) {
      field = Column {
        Text(label).Style(style.label_style),
        std::move(field),
      }.With(Spacing{style.label_spacing}, CrossAlign{CrossAxisAlignment::Start});
    }
    if (!validation_message.empty() &&
        (configuration.validation.status == ValidationStatus::Invalid ||
         configuration.validation.status == ValidationStatus::Pending)) {
      return Column {
        std::move(field),
        Text(validation_message).Style(style.validation_text_style),
      }.With(Spacing{style.validation_spacing}, CrossAlign{CrossAxisAlignment::Start});
    }
    return field;
  };
}

const detail::ModifierDescriptor& DatePickerConfiguration::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec, detail::ModifierSpec& modifier, const std::shared_ptr<const Environment>&,
         detail::AppResources&) {
        const auto& configuration = *static_cast<const DatePickerConfiguration*>(modifier.value.get());
        spec.scope_factory = MakeDatePickerScopeFactory(configuration);
      },
      nullptr,
      nullptr,
      false,
      nullptr,
      nullptr,
  };
  return descriptor;
}

const detail::ModifierDescriptor& TimePickerConfiguration::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec, detail::ModifierSpec& modifier, const std::shared_ptr<const Environment>&,
         detail::AppResources&) {
        const auto& configuration = *static_cast<const TimePickerConfiguration*>(modifier.value.get());
        spec.scope_factory = MakeTimePickerScopeFactory(configuration);
      },
      nullptr,
      nullptr,
      false,
      nullptr,
      nullptr,
  };
  return descriptor;
}

void ValidateDate(Date value, std::string_view name) {
  if (!value.ok()) {
    throw std::invalid_argument("HuxerUI DatePicker " + std::string(name) + " must be a valid calendar date");
  }
}

void ValidateDateRange(Date value, const std::optional<Date>& minimum, const std::optional<Date>& maximum) {
  if (minimum.has_value() && maximum.has_value() && ToSysDays(*minimum) > ToSysDays(*maximum)) {
    throw std::invalid_argument("HuxerUI DatePicker minimum date must not be after maximum date");
  }
  if ((minimum.has_value() && ToSysDays(value) < ToSysDays(*minimum)) ||
      (maximum.has_value() && ToSysDays(value) > ToSysDays(*maximum))) {
    throw std::invalid_argument("HuxerUI DatePicker value must be within the configured range");
  }
}

void ValidateTime(Minutes value) {
  if (value < Minutes{0} || value >= minutes_per_day) {
    throw std::invalid_argument("HuxerUI TimePicker value must be in the range [0h, 24h)");
  }
}

void ValidateTimeStep(Minutes value, Minutes step) {
  if (step <= Minutes{0} || step > Minutes{60} || Minutes{60}.count() % step.count() != 0) {
    throw std::invalid_argument("HuxerUI TimePicker step must be positive, at most one hour, and divide one hour");
  }
  if (value.count() % step.count() != 0) {
    throw std::invalid_argument("HuxerUI TimePicker value must align with step");
  }
}

} // namespace

namespace detail {

std::shared_ptr<ViewSpec> MakeDatePickerSpec(Date value) {
  ValidateDate(value, "value");
  return std::make_shared<ViewSpec>(NodeKind::Scope);
}

std::shared_ptr<ViewSpec> MakeTimePickerSpec(Minutes value) {
  ValidateTime(value);
  return std::make_shared<ViewSpec>(NodeKind::Scope);
}

} // namespace detail

DatePicker::DatePicker(Date value) : detail::TypedView<DatePicker>(detail::MakeDatePickerSpec(value)), value_(value) {
  UpdateModifier();
}

DatePicker DatePicker::Range(Date minimum, Date maximum) && {
  ValidateDate(minimum, "minimum");
  ValidateDate(maximum, "maximum");
  minimum_ = minimum;
  maximum_ = maximum;
  ValidateDateRange(value_, minimum_, maximum_);
  UpdateModifier();
  return std::move(*this);
}

DatePicker DatePicker::Minimum(Date value) && {
  ValidateDate(value, "minimum");
  minimum_ = value;
  ValidateDateRange(value_, minimum_, maximum_);
  UpdateModifier();
  return std::move(*this);
}

DatePicker DatePicker::Maximum(Date value) && {
  ValidateDate(value, "maximum");
  maximum_ = value;
  ValidateDateRange(value_, minimum_, maximum_);
  UpdateModifier();
  return std::move(*this);
}

DatePicker DatePicker::DisabledDates(std::function<bool(Date)> predicate) && {
  if (!predicate) {
    throw std::invalid_argument("HuxerUI DatePicker disabled-dates predicate must not be empty");
  }
  disabled_dates_ = std::move(predicate);
  UpdateModifier();
  return std::move(*this);
}

DatePicker DatePicker::Label(StringVariant value) && {
  label_ = std::move(value);
  UpdateModifier();
  return std::move(*this);
}

DatePicker DatePicker::Validation(ValidationResult value) && {
  validation_ = std::move(value);
  UpdateModifier();
  return std::move(*this);
}

void DatePicker::UpdateModifier() {
  SetModifier(detail::MakeModifierSpec(DatePickerConfiguration{
      .value = value_,
      .minimum = minimum_,
      .maximum = maximum_,
      .disabled_dates = disabled_dates_,
      .label = label_,
      .validation = validation_,
  }));
}

TimePicker::TimePicker(Minutes value)
    : detail::TypedView<TimePicker>(detail::MakeTimePickerSpec(value)), value_(value) {
  UpdateModifier();
}

TimePicker TimePicker::Step(Minutes value) && {
  ValidateTimeStep(value_, value);
  step_ = value;
  UpdateModifier();
  return std::move(*this);
}

TimePicker TimePicker::DisabledTimes(std::function<bool(Minutes)> predicate) && {
  if (!predicate) {
    throw std::invalid_argument("HuxerUI TimePicker disabled-times predicate must not be empty");
  }
  disabled_times_ = std::move(predicate);
  UpdateModifier();
  return std::move(*this);
}

TimePicker TimePicker::Label(StringVariant value) && {
  label_ = std::move(value);
  UpdateModifier();
  return std::move(*this);
}

TimePicker TimePicker::Validation(ValidationResult value) && {
  validation_ = std::move(value);
  UpdateModifier();
  return std::move(*this);
}

void TimePicker::UpdateModifier() {
  SetModifier(detail::MakeModifierSpec(TimePickerConfiguration{
      .value = value_,
      .step = step_,
      .disabled_times = disabled_times_,
      .label = label_,
      .validation = validation_,
  }));
}

} // namespace huxerui
