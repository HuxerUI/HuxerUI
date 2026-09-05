#include "runtime_test_support.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace huxerui::test {

namespace {

using Date = std::chrono::year_month_day;
using Minutes = std::chrono::minutes;

State<Date> selected_date;
State<Minutes> selected_time;
State<bool> pickers_enabled;
State<Date> date_minimum;
State<Date> date_maximum;
State<bool> time_candidates_disabled;
State<std::string> localized_picker_locale;
int date_changes = 0;
int time_changes = 0;
ThemeDefinition picker_theme;
std::string picker_locale = "en-US";

class PickerMeasuringPlatform final : public TestPlatform {
public:
  using TestPlatform::TestPlatform;

  TextLayoutMetrics MeasureText(const huxerui::AttributedText& text, const TextStyle& style, float max_width,
      const TextLayoutOptions& options) override {
    ++measurements;
    return TestPlatform::MeasureText(text, style, max_width, options);
  }

  std::size_t measurements = 0;
};

View ChangingRangeDatePickerApp() {
  selected_date = UseState(Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{16}});
  date_minimum = UseState(Date{std::chrono::year{2020}, std::chrono::January, std::chrono::day{1}});
  date_maximum = UseState(Date{std::chrono::year{2045}, std::chrono::December, std::chrono::day{31}});
  return huxerui::DatePicker(selected_date)
      .Range(date_minimum.Get(), date_maximum.Get())
      .Label("Travel date")
      .OnChanged([](Date value) {
        ++date_changes;
        selected_date = value;
      });
}

View ChangingAvailabilityTimePickerApp() {
  selected_time = UseState(Minutes{13 * 60 + 30});
  time_candidates_disabled = UseState(false);
  pickers_enabled = UseState(true);
  return huxerui::TimePicker(selected_time)
      .Step(Minutes{5})
      .DisabledTimes([disabled = time_candidates_disabled.Get()](Minutes value) {
        const auto hour = value.count() / 60;
        return disabled && (hour == 1 || hour == 14);
      })
      .Label("Departure time")
      .OnChanged([](Minutes value) {
        ++time_changes;
        selected_time = value;
      })
      .With(Enabled{pickers_enabled.Get()});
}

View DatePickerApp() {
  selected_date = UseState(Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{16}});
  return huxerui::DatePicker(selected_date)
      .Range(
          Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{1}},
          Date{std::chrono::year{2024}, std::chrono::April, std::chrono::day{30}}
      )
      .DisabledDates([](Date value) {
        return value == Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{17}};
      })
      .Label("Travel date")
      .OnChanged([](Date value) {
        ++date_changes;
        selected_date = value;
      });
}

View InvalidDatePickerApp() {
  return huxerui::DatePicker(Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{16}})
      .Label("Travel date")
      .Validation(ValidationResult::Invalid("Choose an available date"));
}

View YearDatePickerApp() {
  selected_date = UseState(Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{16}});
  return huxerui::DatePicker(selected_date).Label("Travel date").OnChanged([](Date value) {
    ++date_changes;
    selected_date = value;
  });
}

View BoundedYearDatePickerApp() {
  return huxerui::DatePicker(Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{16}})
      .Range(
          Date{std::chrono::year{2010}, std::chrono::June, std::chrono::day{1}},
          Date{std::chrono::year{2025}, std::chrono::May, std::chrono::day{31}}
      )
      .Label("Travel date");
}

View EndBoundedDatePickerApp() {
  selected_date = UseState(Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{31}});
  return huxerui::DatePicker(selected_date)
      .Range(
          Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{1}},
          Date{std::chrono::year{2024}, std::chrono::April, std::chrono::day{15}}
      )
      .Label("Travel date")
      .OnChanged([](Date value) {
        ++date_changes;
        selected_date = value;
      });
}

View TimePickerApp() {
  selected_time = UseState(Minutes{13 * 60 + 30});
  return huxerui::TimePicker(selected_time)
      .Step(Minutes{5})
      .DisabledTimes([](Minutes value) { return value == Minutes{14 * 60 + 35}; })
      .Label("Departure time")
      .OnChanged([](Minutes value) {
        ++time_changes;
        selected_time = value;
      });
}

View HourFallbackTimePickerApp() {
  selected_time = UseState(Minutes{13 * 60 + 30});
  return huxerui::TimePicker(selected_time)
      .Step(Minutes{5})
      .DisabledTimes([](Minutes value) { return value == Minutes{14 * 60 + 30}; })
      .Label("Departure time")
      .OnChanged([](Minutes value) { selected_time = value; });
}

View UnavailableHourTimePickerApp() {
  return huxerui::TimePicker(Minutes{13 * 60 + 30})
      .Step(Minutes{5})
      .DisabledTimes([](Minutes value) { return value >= Minutes{14 * 60} && value < Minutes{15 * 60}; })
      .Label("Departure time")
      .OnChanged([](Minutes) { ++time_changes; });
}

View RejectedPickersApp() {
  pickers_enabled = UseState(true);
  return Row {
    huxerui::DatePicker(Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{16}})
        .Label("Travel date")
        .OnChanged([](Date) { ++date_changes; }),
    huxerui::TimePicker(Minutes{13 * 60 + 30})
        .Step(Minutes{5})
        .Label("Departure time")
        .OnChanged([](Minutes) { ++time_changes; }),
  }.With(Enabled{pickers_enabled.Get()});
}

View EnglishTimePickerApp() {
  return ProvideEnvironment(
      Locale::FromLanguageTag("en-US"),
      huxerui::TimePicker(Minutes{13 * 60 + 30}).Label("Departure time")
  );
}

View BritishTimePickerApp() {
  return ProvideEnvironment(
      Locale::FromLanguageTag("en-GB"),
      huxerui::TimePicker(Minutes{13 * 60 + 30}).Label("Departure time")
  );
}

View BritishDatePickerApp() {
  return ProvideEnvironment(
      Locale::FromLanguageTag("en-GB"),
      huxerui::DatePicker(Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{16}})
  );
}

View ChinesePickersApp() {
  return ProvideEnvironment(
      Locale::FromLanguageTag("zh-CN"),
      Row {
        huxerui::DatePicker(Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{16}}),
        huxerui::TimePicker(Minutes{13 * 60 + 30}),
      }
  );
}

View StyledTimePickerApp() {
  return Theme{picker_theme, ProvideEnvironment(Locale::FromLanguageTag(picker_locale), TimePickerApp())};
}

View LocalizedPickersApp() {
  localized_picker_locale = UseState(std::string{"en-US"});
  return ProvideEnvironment(
      Locale::FromLanguageTag(localized_picker_locale.Get()),
      Row {
        huxerui::DatePicker(Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{16}}),
        huxerui::TimePicker(Minutes{13 * 60 + 30}),
      }
  );
}

View StyledDatePickerApp() {
  return Theme{picker_theme, ProvideEnvironment(Locale::FromLanguageTag(picker_locale), YearDatePickerApp())};
}

const SemanticNode& FindNode(const SemanticFrame& frame, SemanticRole role, const std::string& label) {
  const auto found = std::ranges::find_if(frame.nodes, [&](const SemanticNode& node) {
    return node.role == role && node.label == label;
  });
  REQUIRE(found != frame.nodes.end());
  return *found;
}

const SemanticNode& FindPicker(const SemanticFrame& frame, const std::string& label) {
  return FindNode(frame, SemanticRole::Grid, label);
}

} // namespace

TEST_CASE("DatePickerValidatesCalendarValuesAndRanges") {
  REQUIRE_THROWS_AS(
      huxerui::DatePicker(Date{std::chrono::year{2024}, std::chrono::February, std::chrono::day{30}}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      huxerui::DatePicker(Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{16}})
          .Range(
              Date{std::chrono::year{2024}, std::chrono::April, std::chrono::day{1}},
              Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{1}}
          ),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      huxerui::DatePicker(Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{16}})
          .Minimum(Date{std::chrono::year{2024}, std::chrono::April, std::chrono::day{1}}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      huxerui::DatePicker(Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{16}})
          .DisabledDates({}),
      std::invalid_argument
  );
}

TEST_CASE("TimePickerValidatesTimeOfDayAndStep") {
  REQUIRE_THROWS_AS(huxerui::TimePicker(Minutes{-1}), std::invalid_argument);
  REQUIRE_THROWS_AS(huxerui::TimePicker(Minutes{24 * 60}), std::invalid_argument);
  REQUIRE_THROWS_AS(huxerui::TimePicker(Minutes{61}).Step(Minutes{5}), std::invalid_argument);
  REQUIRE_THROWS_AS(huxerui::TimePicker(Minutes{60}).Step(Minutes{7}), std::invalid_argument);
  REQUIRE_THROWS_AS(huxerui::TimePicker(Minutes{60}).DisabledTimes({}), std::invalid_argument);
}

TEST_CASE("DatePickerExposesControlledDatesAndSkipsDisabledKeyboardTargets") {
  date_changes = 0;
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{DatePickerApp, platform};
  runtime.SetWindowMetrics({.viewport = {420.0F, 420.0F}});

  const std::shared_ptr<const SemanticFrame> initial = runtime.BuildCommit().semantic_frame;
  const SemanticNode& picker = FindPicker(*initial, "Travel date");
  REQUIRE(picker.value == "March 16, 2024");
  REQUIRE(picker.collection == SemanticCollection{.item_count = 42, .row_count = 6, .column_count = 7});
  const SemanticNode& current = FindNode(*initial, SemanticRole::GridCell, "March 16, 2024");
  const SemanticNode& disabled = FindNode(*initial, SemanticRole::GridCell, "March 17, 2024");
  REQUIRE(current.selected == true);
  REQUIRE(current.enabled);
  REQUIRE_FALSE(disabled.enabled);
  REQUIRE(disabled.actions == 0);
  REQUIRE_FALSE(FindNode(*initial, SemanticRole::Button, "Previous month").enabled);

  ClickAt(runtime, {current.bounds.x + current.bounds.width * 0.5F, current.bounds.y + current.bounds.height * 0.5F});
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::ArrowRight});
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::Enter});
  REQUIRE(date_changes == 1);
  REQUIRE(selected_date.Get() == Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{18}});

  const SemanticNode& changed = FindPicker(*runtime.BuildCommit().semantic_frame, "Travel date");
  REQUIRE(changed.value == "March 18, 2024");
}

TEST_CASE("DatePickerSemanticCellsRespectDisabledDatesAndEmitControlledChanges") {
  date_changes = 0;
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{DatePickerApp, platform};
  runtime.SetWindowMetrics({.viewport = {420.0F, 420.0F}});

  const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
  const SemanticNode& disabled = FindNode(*frame, SemanticRole::GridCell, "March 17, 2024");
  const SemanticNode& enabled = FindNode(*frame, SemanticRole::GridCell, "March 18, 2024");
  REQUIRE_FALSE(disabled.enabled);
  REQUIRE(enabled.enabled);
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(
      disabled.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      enabled.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(date_changes == 1);
  REQUIRE(selected_date.Get() == Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{18}});
}

TEST_CASE("DatePickerYearGridKeepsBrowsingSeparateFromControlledSelection") {
  date_changes = 0;
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{YearDatePickerApp, platform};
  runtime.SetWindowMetrics({.viewport = {420.0F, 420.0F}});

  const std::shared_ptr<const SemanticFrame> initial = runtime.BuildCommit().semantic_frame;
  const SemanticNode& current = FindNode(*initial, SemanticRole::GridCell, "March 16, 2024");
  ClickAt(runtime, {current.bounds.x + current.bounds.width * 0.5F, current.bounds.y + current.bounds.height * 0.5F});
  const std::shared_ptr<const SemanticFrame> focused = runtime.BuildCommit().semantic_frame;
  const SemanticNode& choose_year = FindNode(*focused, SemanticRole::Button, "Choose year");
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      choose_year.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(FindPicker(*runtime.BuildCommit().semantic_frame, "Travel date").collection ==
          SemanticCollection{.item_count = 20, .row_count = 5, .column_count = 4});

  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::ArrowRight});
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::Enter});
  const FlattenedScene& browsed = runtime.BuildFrame();
  REQUIRE(FindText(browsed, "March 2025") != nullptr);
  REQUIRE(selected_date.Get() == Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{16}});
  REQUIRE(date_changes == 0);
}

TEST_CASE("DatePickerYearGridPreservesValidGeometryWhenNarrowedAndRestored") {
  for (const auto& theme : {FlatThemeDefinition(), MaterialThemeDefinition()}) {
    picker_theme = theme;
    for (const std::string locale : {"en-US", "ar"}) {
      picker_locale = locale;
      date_changes = 0;
      TestPlatform platform{BuiltinTestResources()};
      Runtime runtime{StyledDatePickerApp, platform};
      runtime.SetWindowMetrics({.viewport = {420.0F, 440.0F}});
      const auto initial = runtime.BuildCommit().semantic_frame;
      const std::string choose_year = locale == "ar" ? "اختيار السنة" : "Choose year";
      REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
          FindNode(*initial, SemanticRole::Button, choose_year).id,
          {SemanticActionKind::Activate, std::monostate{}}
      ));
      const auto expanded = runtime.BuildCommit().semantic_frame;
      const SemanticNode& original_year = FindNode(*expanded, SemanticRole::Button, "2025");
      REQUIRE(original_year.bounds.width > 0.0F);

      for (float width : {20.0F, 1.0F}) {
        CAPTURE(locale, width);
        runtime.SetWindowMetrics({.viewport = {width, 440.0F}});
        const auto narrow = runtime.BuildCommit().semantic_frame;
        REQUIRE(FindPicker(*narrow, "Travel date").bounds.width == width);
        REQUIRE(FindPicker(*narrow, "Travel date").collection->item_count == 20);
        for (int year = 2020; year < 2040; ++year) {
          const std::string label = std::to_string(year);
          const SemanticNode& cell = FindNode(*narrow, SemanticRole::Button, label);
          REQUIRE(cell.bounds.width == 0.0F);
          REQUIRE(cell.id == FindNode(*expanded, SemanticRole::Button, label).id);
        }
      }

      runtime.SetWindowMetrics({.viewport = {420.0F, 440.0F}});
      const auto restored = runtime.BuildCommit().semantic_frame;
      const SemanticNode& restored_year = FindNode(*restored, SemanticRole::Button, "2025");
      REQUIRE(restored_year.id == original_year.id);
      REQUIRE(restored_year.bounds == original_year.bounds);
      ClickAt(runtime, {restored_year.bounds.x + restored_year.bounds.width * 0.5F,
                        restored_year.bounds.y + restored_year.bounds.height * 0.5F});
      REQUIRE(FindPicker(*runtime.BuildCommit().semantic_frame, "Travel date").collection->item_count == 42);
      REQUIRE(selected_date.Get() == Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{16}});
      REQUIRE(date_changes == 0);
    }
  }
}

TEST_CASE("DatePickerYearPagesRemainReachableAtRangeBoundaries") {
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{BoundedYearDatePickerApp, platform};
  runtime.SetWindowMetrics({.viewport = {420.0F, 420.0F}});

  const std::shared_ptr<const SemanticFrame> initial = runtime.BuildCommit().semantic_frame;
  const SemanticNode& choose_year = FindNode(*initial, SemanticRole::Button, "Choose year");
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      choose_year.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  const std::shared_ptr<const SemanticFrame> years = runtime.BuildCommit().semantic_frame;
  const SemanticNode& previous = FindNode(*years, SemanticRole::Button, "Previous years");
  REQUIRE(previous.enabled);
  REQUIRE(previous.actions != 0);
  REQUIRE_FALSE(FindNode(*years, SemanticRole::Button, "Next years").enabled);
  REQUIRE(FindNode(*years, SemanticRole::Button, "2025").enabled);
  REQUIRE_FALSE(FindNode(*years, SemanticRole::Button, "2026").enabled);
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      previous.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  const SemanticNode& boundary = FindNode(*runtime.BuildCommit().semantic_frame, SemanticRole::Button, "2010");
  REQUIRE(boundary.actions != 0);
}

TEST_CASE("DatePickerMonthPagingKeepsKeyboardDateSelectable") {
  date_changes = 0;
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{EndBoundedDatePickerApp, platform};
  runtime.SetWindowMetrics({.viewport = {420.0F, 420.0F}});

  const std::shared_ptr<const SemanticFrame> initial = runtime.BuildCommit().semantic_frame;
  const SemanticNode& current = FindNode(*initial, SemanticRole::GridCell, "March 31, 2024");
  ClickAt(runtime, {current.bounds.x + current.bounds.width * 0.5F, current.bounds.y + current.bounds.height * 0.5F});
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::PageDown});
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::Enter});

  REQUIRE(date_changes == 1);
  REQUIRE(selected_date.Get() == Date{std::chrono::year{2024}, std::chrono::April, std::chrono::day{15}});
}

TEST_CASE("TimePickerClockFaceEmitsHourAndMinuteProposals") {
  time_changes = 0;
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{TimePickerApp, platform};
  runtime.SetWindowMetrics({.viewport = {360.0F, 420.0F}});

  const std::shared_ptr<const SemanticFrame> hours = runtime.BuildCommit().semantic_frame;
  const SemanticNode& picker = FindPicker(*hours, "Departure time");
  REQUIRE(picker.value == "01:30 PM");
  REQUIRE(FindNode(*hours, SemanticRole::GridCell, "1").selected == true);
  const SemanticNode& hour = FindNode(*hours, SemanticRole::GridCell, "2");
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      hour.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(selected_time.Get() == Minutes{14 * 60 + 30});

  const std::shared_ptr<const SemanticFrame> changed_hour = runtime.BuildCommit().semantic_frame;
  const SemanticNode& minute_header = FindNode(*changed_hour, SemanticRole::Button, "Minute");
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      minute_header.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  const std::shared_ptr<const SemanticFrame> minutes = runtime.BuildCommit().semantic_frame;
  REQUIRE(FindPicker(*minutes, "Departure time").collection == SemanticCollection{.item_count = 12});
  const SemanticNode& disabled = FindNode(*minutes, SemanticRole::GridCell, "35");
  const SemanticNode& enabled = FindNode(*minutes, SemanticRole::GridCell, "40");
  REQUIRE_FALSE(disabled.enabled);
  REQUIRE(enabled.enabled);
  REQUIRE(disabled.actions == 0);
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(
      disabled.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      enabled.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(selected_time.Get() == Minutes{14 * 60 + 40});
  REQUIRE(time_changes == 2);
}

TEST_CASE("TimePickerMarksHoursWithoutSelectableMinutesDisabled") {
  time_changes = 0;
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{UnavailableHourTimePickerApp, platform};
  runtime.SetWindowMetrics({.viewport = {360.0F, 420.0F}});

  const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
  const SemanticNode& disabled = FindNode(*frame, SemanticRole::GridCell, "2");
  const SemanticNode& enabled = FindNode(*frame, SemanticRole::GridCell, "3");
  REQUIRE_FALSE(disabled.enabled);
  REQUIRE(disabled.actions == 0);
  REQUIRE(enabled.enabled);
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(
      disabled.id, {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(time_changes == 0);
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      enabled.id, {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(time_changes == 1);
}

TEST_CASE("TimePickerKeyboardHoursChooseTheNearestAvailableMinute") {
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{HourFallbackTimePickerApp, platform};
  runtime.SetWindowMetrics({.viewport = {360.0F, 420.0F}});

  const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
  const Rect hour = FindNode(*frame, SemanticRole::Button, "Hour").bounds;
  ClickAt(runtime, {hour.x + hour.width * 0.5F, hour.y + hour.height * 0.5F});
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::ArrowUp});

  REQUIRE(selected_time.Get() == Minutes{14 * 60 + 25});
}

TEST_CASE("PickersRepeatRejectedProposalsAndRespectDisabledRecomposition") {
  date_changes = 0;
  time_changes = 0;
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{RejectedPickersApp, platform};
  runtime.SetWindowMetrics({.viewport = {760.0F, 440.0F}});

  for (int attempt = 0; attempt < 2; ++attempt) {
    const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
    const SemanticNode& date = FindNode(*frame, SemanticRole::GridCell, "March 18, 2024");
    const SemanticNode& hour = FindNode(*frame, SemanticRole::GridCell, "2");
    REQUIRE(runtime.CoreRuntime().PerformSemanticAction(date.id, {SemanticActionKind::Activate, std::monostate{}}));
    REQUIRE(runtime.CoreRuntime().PerformSemanticAction(hour.id, {SemanticActionKind::Activate, std::monostate{}}));
  }
  REQUIRE(date_changes == 2);
  REQUIRE(time_changes == 2);

  pickers_enabled = false;
  const std::shared_ptr<const SemanticFrame> disabled = runtime.BuildCommit().semantic_frame;
  const SemanticNode& date = FindNode(*disabled, SemanticRole::GridCell, "March 18, 2024");
  const SemanticNode& hour = FindNode(*disabled, SemanticRole::GridCell, "2");
  REQUIRE_FALSE(date.enabled);
  REQUIRE_FALSE(hour.enabled);
  REQUIRE(date.actions == 0);
  REQUIRE(hour.actions == 0);
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(
      date.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(
      hour.id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(date_changes == 2);
  REQUIRE(time_changes == 2);
}

TEST_CASE("PickersReleasePointerCaptureOnCancel") {
  TestPlatform platform{BuiltinTestResources()};
  SECTION("Calendar cancellation does not select the released date") {
    date_changes = 0;
    Runtime runtime{DatePickerApp, platform};
    runtime.SetWindowMetrics({.viewport = {420.0F, 420.0F}});
    const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
    const Rect date = FindNode(*frame, SemanticRole::GridCell, "March 18, 2024").bounds;
    const Point position{date.x + date.width * 0.5F, date.y + date.height * 0.5F};
    runtime.HandlePointerEvent({.type = PointerEventType::Down, .pointer_id = 61, .position = position});
    runtime.HandlePointerEvent({.type = PointerEventType::Cancel, .pointer_id = 61, .position = position});
    runtime.HandlePointerEvent({.type = PointerEventType::Up, .pointer_id = 61, .position = position});
    REQUIRE(date_changes == 0);
  }
  SECTION("Clock cancellation ends the live drag without advancing the dial") {
    time_changes = 0;
    Runtime runtime{TimePickerApp, platform};
    runtime.SetWindowMetrics({.viewport = {360.0F, 420.0F}});
    const std::shared_ptr<const SemanticFrame> frame = runtime.BuildCommit().semantic_frame;
    const Rect hour = FindNode(*frame, SemanticRole::GridCell, "3").bounds;
    const Point position{hour.x + hour.width * 0.5F, hour.y + hour.height * 0.5F};
    runtime.HandlePointerEvent({.type = PointerEventType::Down, .pointer_id = 62, .position = position});
    runtime.HandlePointerEvent({.type = PointerEventType::Cancel, .pointer_id = 62, .position = position});
    runtime.HandlePointerEvent({.type = PointerEventType::Up, .pointer_id = 62, .position = position});
    REQUIRE(time_changes == 1);
    const SemanticNode& active = FindNode(*runtime.BuildCommit().semantic_frame, SemanticRole::Button, "Hour");
    REQUIRE(active.selected == true);
  }
}

TEST_CASE("TimePickerTouchSelectionUsesClockGeometry") {
  time_changes = 0;
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{TimePickerApp, platform};
  runtime.SetWindowMetrics({.viewport = {360.0F, 420.0F}});

  const std::shared_ptr<const SemanticFrame> hours = runtime.BuildCommit().semantic_frame;
  const Rect hour = FindNode(*hours, SemanticRole::GridCell, "3").bounds;
  const Point hour_center{hour.x + hour.width * 0.5F, hour.y + hour.height * 0.5F};
  runtime.HandlePointerEvent(PointerEvent{
      .type = PointerEventType::Down,
      .pointer_id = 51,
      .position = hour_center,
      .device_kind = PointerDeviceKind::Touch,
  });
  runtime.HandlePointerEvent(PointerEvent{
      .type = PointerEventType::Up,
      .pointer_id = 51,
      .position = hour_center,
      .device_kind = PointerDeviceKind::Touch,
  });
  REQUIRE(selected_time.Get() == Minutes{15 * 60 + 30});

  const std::shared_ptr<const SemanticFrame> minutes = runtime.BuildCommit().semantic_frame;
  const Rect minute = FindNode(*minutes, SemanticRole::GridCell, "45").bounds;
  const Point minute_center{minute.x + minute.width * 0.5F, minute.y + minute.height * 0.5F};
  runtime.HandlePointerEvent(PointerEvent{
      .type = PointerEventType::Down,
      .pointer_id = 52,
      .position = minute_center,
      .device_kind = PointerDeviceKind::Touch,
  });
  runtime.HandlePointerEvent(PointerEvent{
      .type = PointerEventType::Up,
      .pointer_id = 52,
      .position = minute_center,
      .device_kind = PointerDeviceKind::Touch,
  });
  REQUIRE(selected_time.Get() == Minutes{15 * 60 + 45});
  REQUIRE(time_changes == 2);
}

TEST_CASE("PickersResolveLocalizedCalendarAndHourCyclePresentation") {
  TestPlatform platform{BuiltinTestResources()};

  Runtime english{EnglishTimePickerApp, platform};
  english.SetWindowMetrics({.viewport = {360.0F, 420.0F}});
  const FlattenedScene& english_scene = english.BuildFrame();
  REQUIRE(FindText(english_scene, "01") != nullptr);
  REQUIRE(FindText(english_scene, "PM") != nullptr);

  Runtime british{BritishTimePickerApp, platform};
  british.SetWindowMetrics({.viewport = {360.0F, 420.0F}});
  const FlattenedScene& british_scene = british.BuildFrame();
  REQUIRE(FindText(british_scene, "13") != nullptr);
  REQUIRE(FindText(british_scene, "PM") == nullptr);

  Runtime british_calendar{BritishDatePickerApp, platform};
  british_calendar.SetWindowMetrics({.viewport = {420.0F, 420.0F}});
  const std::shared_ptr<const SemanticFrame> british_dates = british_calendar.BuildCommit().semantic_frame;
  const SemanticNode& monday = FindNode(*british_dates, SemanticRole::GridCell, "March 11, 2024");
  REQUIRE(monday.collection_item->column_index == 0);

  Runtime chinese{ChinesePickersApp, platform};
  chinese.SetWindowMetrics({.viewport = {760.0F, 440.0F}});
  const FlattenedScene& chinese_scene = chinese.BuildFrame();
  REQUIRE(FindText(chinese_scene, "2024\xE5\xB9\xB4" "3\xE6\x9C\x88") != nullptr);
  REQUIRE(FindText(chinese_scene, "一") != nullptr);
  REQUIRE(FindText(chinese_scene, "13") != nullptr);
  REQUIRE(FindText(chinese_scene, "下午") == nullptr);
}

TEST_CASE("DatePickerValidationRemainsApplicationOwned") {
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{InvalidDatePickerApp, platform};
  runtime.SetWindowMetrics({.viewport = {420.0F, 440.0F}});

  const FlattenedScene& scene = runtime.BuildFrame();
  const SemanticNode& picker = FindPicker(*runtime.LastCommit().semantic_frame, "Travel date");
  REQUIRE(picker.invalid == true);
  REQUIRE(picker.error == "Choose an available date");
  REQUIRE(FindText(scene, "Choose an available date") != nullptr);
  REQUIRE(FindBorderWithColor(scene, DatePickerStyle::Default().validation_error) != nullptr);
}

TEST_CASE("DateAndTimePickerStylesFollowFlatAndMaterialThemes") {
  for (bool dark : {false, true}) {
    const ThemeSpec flat_spec = dark ? FlatDarkThemeSpec() : FlatLightThemeSpec();
    const ThemeSpec material_spec = dark ? MaterialDarkThemeSpec() : MaterialLightThemeSpec();
    const DatePickerStyle flat_date = detail::DefaultDatePickerStyle(flat_spec);
    const TimePickerStyle flat_time = detail::DefaultTimePickerStyle(flat_spec);
    const ThemeDefinition material = MaterialThemeDefinition(material_spec);
    const DatePickerStyle material_date = ThemeDefinitionValue<DatePickerStyle>(material);
    const TimePickerStyle material_time = ThemeDefinitionValue<TimePickerStyle>(material);

    REQUIRE(flat_date.background == flat_spec.colors.surface);
    REQUIRE(flat_date.cell_size < material_date.cell_size);
    REQUIRE(flat_date.selection_corner_radius < flat_date.cell_size * 0.5F);
    REQUIRE(flat_time.dial_size < material_time.dial_size);
    REQUIRE(flat_time.header_height < material_time.header_height);
    REQUIRE(flat_time.border_width > 0.0F);
    REQUIRE(flat_time.selected_field_background == flat_spec.colors.primary_container);
    REQUIRE(flat_time.selected_field_foreground == flat_spec.colors.on_primary_container);
    REQUIRE(material_date.background == material_spec.colors.surface_container_high);
    REQUIRE(material_date.border_width == 0.0F);
    REQUIRE(material_time.background == material_spec.colors.surface_container_high);
    REQUIRE(material_time.border_width == 0.0F);
    REQUIRE(material_time.field_background == material_spec.colors.surface_container_highest);
    REQUIRE(material_time.selected_field_background == material_spec.colors.primary_container);
    REQUIRE(material_time.selected_field_foreground == material_spec.colors.on_primary_container);
    REQUIRE(material_time.selected_period_background == material_spec.colors.tertiary_container);
    REQUIRE(material_time.selected_period_foreground == material_spec.colors.on_tertiary_container);
    REQUIRE(material_time.period_border == material_spec.colors.outline);
    REQUIRE(material_time.selected_background == material_spec.colors.primary);
    REQUIRE(material_time.selected_foreground == material_spec.colors.on_primary);
  }
}

TEST_CASE("PickersRefreshLocalizedLabelsWhenTheLocaleChanges") {
  struct Labels {
    const char* locale;
    const char* date_picker;
    const char* time_picker;
    const char* next_month;
    const char* month_title;
    const char* monday;
    const char* hour;
    const char* minute;
    const char* am;
    const char* pm;
  };
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{LocalizedPickersApp, platform};
  runtime.SetWindowMetrics({.viewport = {820.0F, 480.0F}});
  runtime.BuildFrame();
  for (const Labels& labels : {
           Labels{"zh-Hant-TW", "日期選擇器", "時間選擇器", "下個月", "2024年3月", "一", "小時", "分鐘", "上午", "下午"},
           Labels{"zh-HK", "日期選擇器", "時間選擇器", "下個月", "2024年3月", "一", "小時", "分鐘", "上午", "下午"},
           Labels{"zh-MO", "日期選擇器", "時間選擇器", "下個月", "2024年3月", "一", "小時", "分鐘", "上午", "下午"},
           Labels{"ja-JP", "日付選択", "時刻選択", "次の月", "2024 3月", "月", "時", "分", nullptr, nullptr},
           Labels{"ko-KR", "날짜 선택", "시간 선택", "다음 달", "2024 3월", "월", "시", "분", "오전", "오후"},
           Labels{"fr-FR", "Sélecteur de date", "Sélecteur d’heure", "Mois suivant", "mars 2024", "lu",
                  "Heure", "Minute", nullptr, nullptr},
           Labels{"ar-EG", "منتقي التاريخ", "منتقي الوقت", "الشهر التالي", "مارس 2024", "إثنين",
                  "الساعة", "الدقيقة", "ص", "م"},
           Labels{"pt-BR", "Seletor de data", "Seletor de hora", "Próximo mês", "março 2024", "seg.",
                  "Hora", "Minuto", nullptr, nullptr},
           Labels{"pt-PT", "Seletor de data", "Seletor de hora", "Mês seguinte", "março 2024", "seg.",
                  "Hora", "Minuto", nullptr, nullptr},
           Labels{"en-US", "Date picker", "Time picker", "Next month", "March 2024", "Mon", "Hour", "Minute", "AM", "PM"},
       }) {
    CAPTURE(labels.locale);
    localized_picker_locale = std::string{labels.locale};
    const FlattenedScene& scene = runtime.BuildFrame();
    const auto frame = runtime.LastCommit().semantic_frame;
    FindPicker(*frame, labels.date_picker);
    FindPicker(*frame, labels.time_picker);
    FindNode(*frame, SemanticRole::Button, labels.next_month);
    FindNode(*frame, SemanticRole::Button, labels.hour);
    FindNode(*frame, SemanticRole::Button, labels.minute);
    REQUIRE(FindText(scene, labels.month_title) != nullptr);
    REQUIRE(FindText(scene, labels.monday) != nullptr);
    REQUIRE(FindText(scene, labels.am ? "01" : "13") != nullptr);
    if (labels.am) {
      REQUIRE(FindText(scene, labels.am) != nullptr);
      REQUIRE(FindText(scene, labels.pm) != nullptr);
      REQUIRE(FindNode(*frame, SemanticRole::Button, labels.am).selected == false);
      REQUIRE(FindNode(*frame, SemanticRole::Button, labels.pm).selected == true);
    }
  }
}

TEST_CASE("DatePickerChevronsPointInTheVisualPagingDirection") {
  picker_theme = FlatThemeDefinition();
  for (const std::string locale : {"en-US", "ar"}) {
    picker_locale = locale;
    TestPlatform platform{BuiltinTestResources()};
    Runtime runtime{StyledDatePickerApp, platform};
    runtime.SetWindowMetrics({.viewport = {420.0F, 440.0F}});
    const FlattenedScene& scene = runtime.BuildFrame();
    std::vector<DrawLineCommand> lines;
    for (const PaintCommand& command : scene.Commands()) {
      if (const auto* line = std::get_if<DrawLineCommand>(&command)) {
        lines.push_back(*line);
      }
    }
    REQUIRE(lines.size() == 4);
    const bool rtl = locale == "ar";
    REQUIRE((lines[0].end.x > lines[0].start.x) == rtl);
    REQUIRE(lines[0].end == lines[1].start);
    REQUIRE((lines[2].end.x < lines[2].start.x) == rtl);
    REQUIRE(lines[2].end == lines[3].start);
    const auto frame = runtime.LastCommit().semantic_frame;
    const Rect previous = FindNode(*frame, SemanticRole::Button, rtl ? "الشهر السابق" : "Previous month").bounds;
    const Rect next = FindNode(*frame, SemanticRole::Button, rtl ? "الشهر التالي" : "Next month").bounds;
    REQUIRE((previous.x > next.x) == rtl);
  }
}

TEST_CASE("TimePickerPaintsIndependentFieldsAndOnePeriodGroup") {
  picker_locale = "en-US";
  for (const auto& [theme, style] : {
           std::pair{FlatThemeDefinition(), detail::DefaultTimePickerStyle(FlatLightThemeSpec())},
           std::pair{FlatDarkThemeDefinition(), detail::DefaultTimePickerStyle(FlatDarkThemeSpec())},
           std::pair{MaterialThemeDefinition(), ThemeDefinitionValue<TimePickerStyle>(MaterialThemeDefinition())},
           std::pair{MaterialDarkThemeDefinition(),
                     ThemeDefinitionValue<TimePickerStyle>(MaterialDarkThemeDefinition())},
       }) {
    picker_theme = theme;
    TestPlatform platform{BuiltinTestResources()};
    Runtime runtime{StyledTimePickerApp, platform};
    runtime.SetWindowMetrics({.viewport = {500.0F, 520.0F}});
    const FlattenedScene& scene = runtime.BuildFrame();
    const DrawTextCommand* hour = FindText(scene, "01");
    const DrawTextCommand* minute = FindText(scene, "30");
    REQUIRE(hour != nullptr);
    REQUIRE(minute != nullptr);
    const DrawRectCommand* selected = FindRectWithColor(scene, style.selected_field_background);
    const DrawRectCommand* inactive = FindRectWithColor(scene, style.field_background);
    REQUIRE(selected != nullptr);
    REQUIRE(inactive != nullptr);
    REQUIRE(selected->rect == hour->rect);
    REQUIRE(inactive->rect == minute->rect);
    REQUIRE(selected->corner_radius == style.field_corner_radius);
    REQUIRE(hour->style.foreground == style.selected_field_foreground);
    REQUIRE(minute->style.foreground == style.header_style.foreground);
    REQUIRE(FindText(scene, "PM")->style.foreground == style.selected_period_foreground);
    const DrawBorderCommand* group = nullptr;
    for (const PaintCommand& command : scene.Commands()) {
      if (const auto* border = std::get_if<DrawBorderCommand>(&command);
          border && border->rect.width == style.period_width && border->rect.height == style.header_height) {
        group = border;
      }
    }
    REQUIRE(group != nullptr);
    REQUIRE(group->color == style.period_border);
    REQUIRE(group->style.width == style.period_border_width);
    REQUIRE(group->rect.x == minute->rect.x + minute->rect.width + style.period_spacing);
    const auto frame = runtime.LastCommit().semantic_frame;
    const Rect minute_bounds = FindNode(*frame, SemanticRole::Button, "Minute").bounds;
    ClickAt(runtime, {minute_bounds.x + minute_bounds.width * 0.5F, minute_bounds.y + minute_bounds.height * 0.5F});
    const FlattenedScene& changed = runtime.BuildFrame();
    REQUIRE(FindRectWithColor(changed, style.selected_field_background)->rect == FindText(changed, "30")->rect);
    REQUIRE(selected_time.Get() == Minutes{13 * 60 + 30});
    const Rect am = FindNode(*runtime.LastCommit().semantic_frame, SemanticRole::Button, "AM").bounds;
    ClickAt(runtime, {am.x + am.width * 0.5F, am.y + am.height * 0.5F});
    REQUIRE(selected_time.Get() == Minutes{60 + 30});
  }
}

TEST_CASE("TimePickerMeasuresTheEntireHeaderAndOmitsThePeriodIn24HourLocales") {
  TimePickerStyle style = TimePickerStyle::Default();
  style.field_width = 112.0F;
  style.period_width = 68.0F;
  style.period_spacing = 20.0F;
  picker_theme = FlatThemeDefinition();
  picker_theme.Set(style);
  for (const std::string locale : {"en-US", "en-GB"}) {
    picker_locale = locale;
    TestPlatform platform{BuiltinTestResources()};
    Runtime runtime{StyledTimePickerApp, platform};
    runtime.SetWindowMetrics({.viewport = {600.0F, 520.0F}});
    const FlattenedScene& scene = runtime.BuildFrame();
    const bool use_12_hour = locale == "en-US";
    const auto frame = runtime.LastCommit().semantic_frame;
    const Rect picker = FindPicker(*frame, "Departure time").bounds;
    const float header_width = 2.0F * style.field_width + style.separator_width +
                               (use_12_hour ? style.period_width + style.period_spacing : 0.0F);
    REQUIRE(picker.width == style.padding.Horizontal() + header_width);
    REQUIRE((FindText(scene, "PM") != nullptr) == use_12_hour);
    const Rect hour = FindNode(*frame, SemanticRole::Button, "Hour").bounds;
    const Rect end = FindNode(*frame, SemanticRole::Button, use_12_hour ? "PM" : "Minute").bounds;
    REQUIRE(hour.x == picker.x + style.padding.left);
    REQUIRE(end.x + end.width == picker.x + picker.width - style.padding.right);
  }
}

TEST_CASE("TimePickerRejectsInvalidHeaderGeometry") {
  picker_locale = "en-US";
  for (float invalid : {0.0F, -1.0F, std::numeric_limits<float>::infinity()}) {
    TimePickerStyle style = TimePickerStyle::Default();
    style.field_width = invalid;
    picker_theme = FlatThemeDefinition();
    picker_theme.Set(style);
    TestPlatform platform{BuiltinTestResources()};
    Runtime runtime{StyledTimePickerApp, platform};
    runtime.SetWindowMetrics({.viewport = {420.0F, 440.0F}});
    REQUIRE_THROWS_AS(runtime.BuildFrame(), std::invalid_argument);
  }
}

TEST_CASE("DateAndTimePickerValidationKeepsAnOutlineOnBorderlessThemes") {
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{[]() -> View {
    return MaterialTheme {
      Row {
        huxerui::DatePicker(Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{16}})
            .Validation(ValidationResult::Invalid("Invalid date")),
        huxerui::TimePicker(Minutes{60}).Validation(ValidationResult::Invalid("Invalid time")),
      },
    };
  }, platform};
  runtime.SetWindowMetrics({.viewport = {800.0F, 520.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();
  const Color error = MaterialLightThemeSpec().colors.error;
  REQUIRE(std::ranges::count_if(scene.Commands(), [error](const PaintCommand& command) {
    const auto* border = std::get_if<DrawBorderCommand>(&command);
    return border && border->color == error && border->style.width >= 1.0F;
  }) == 2);
}

TEST_CASE("DatePickerRangeUpdatesClampBrowsingWithoutChangingSelection") {
  date_changes = 0;
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{ChangingRangeDatePickerApp, platform};
  runtime.SetWindowMetrics({.viewport = {420.0F, 440.0F}});
  const auto initial = runtime.BuildCommit().semantic_frame;
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      FindPicker(*initial, "Travel date").id, {SemanticActionKind::Focus, std::monostate{}}
  ));

  Date expected;
  SECTION("A new minimum restores the first allowed month and day") {
    runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::PageUp, .modifiers = {.shift = true}});
    REQUIRE(FindText(runtime.BuildFrame(), "March 2023") != nullptr);
    date_minimum = Date{std::chrono::year{2024}, std::chrono::February, std::chrono::day{20}};
    REQUIRE(FindText(runtime.BuildFrame(), "February 2024") != nullptr);
    expected = date_minimum.Get();
  }
  SECTION("A new maximum restores the last allowed month and day") {
    runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::PageDown, .modifiers = {.shift = true}});
    REQUIRE(FindText(runtime.BuildFrame(), "March 2025") != nullptr);
    date_maximum = Date{std::chrono::year{2024}, std::chrono::April, std::chrono::day{10}};
    REQUIRE(FindText(runtime.BuildFrame(), "April 2024") != nullptr);
    expected = date_maximum.Get();
  }
  SECTION("Legal browsing survives a range update") {
    runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::PageDown});
    runtime.BuildFrame();
    date_maximum = Date{std::chrono::year{2024}, std::chrono::June, std::chrono::day{30}};
    REQUIRE(FindText(runtime.BuildFrame(), "April 2024") != nullptr);
    expected = Date{std::chrono::year{2024}, std::chrono::April, std::chrono::day{16}};
  }
  REQUIRE(selected_date.Get() == Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{16}});
  REQUIRE(date_changes == 0);
  runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Enter});
  REQUIRE(selected_date.Get() == expected);
  REQUIRE(date_changes == 1);
}

TEST_CASE("DatePickerRangeUpdatesKeepYearBrowsingReachable") {
  date_changes = 0;
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{ChangingRangeDatePickerApp, platform};
  runtime.SetWindowMetrics({.viewport = {420.0F, 440.0F}});
  const auto initial = runtime.BuildCommit().semantic_frame;
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      FindNode(*initial, SemanticRole::Button, "Choose year").id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  const auto years = runtime.BuildCommit().semantic_frame;
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      FindNode(*years, SemanticRole::Button, "Next years").id,
      {SemanticActionKind::Activate, std::monostate{}}
  ));
  runtime.BuildFrame();
  date_maximum = Date{std::chrono::year{2025}, std::chrono::May, std::chrono::day{31}};
  const auto clamped = runtime.BuildCommit().semantic_frame;
  REQUIRE(FindPicker(*clamped, "Travel date").collection->item_count == 20);
  REQUIRE(FindNode(*clamped, SemanticRole::Button, "2025").enabled);
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      FindNode(*clamped, SemanticRole::Button, "2025").id, {SemanticActionKind::Activate, std::monostate{}}
  ));
  REQUIRE(FindText(runtime.BuildFrame(), "May 2025") != nullptr);
  REQUIRE(date_changes == 0);
}

TEST_CASE("TimePickerPeriodButtonsCommitOnlyOnMatchingRelease") {
  for (const auto device : {PointerDeviceKind::Mouse, PointerDeviceKind::Touch}) {
    time_changes = 0;
    TestPlatform platform{BuiltinTestResources()};
    Runtime runtime{TimePickerApp, platform};
    runtime.SetWindowMetrics({.viewport = {360.0F, 420.0F}});
    const auto initial = runtime.BuildCommit().semantic_frame;
    const Rect am = FindNode(*initial, SemanticRole::Button, "AM").bounds;
    const Rect pm = FindNode(*initial, SemanticRole::Button, "PM").bounds;
    const Point position{am.x + am.width * 0.5F, am.y + am.height * 0.5F};
    const Point other_period{pm.x + pm.width * 0.5F, pm.y + pm.height * 0.5F};
    const auto send = [&](PointerEventType type, Point point) {
      runtime.HandlePointerEvent({.type = type, .pointer_id = 83, .position = point, .device_kind = device});
    };
    send(PointerEventType::Down, position);
    REQUIRE(time_changes == 0);
    send(PointerEventType::Cancel, position);
    send(PointerEventType::Up, position);
    REQUIRE(time_changes == 0);

    send(PointerEventType::Down, position);
    send(PointerEventType::Move, other_period);
    send(PointerEventType::Up, other_period);
    REQUIRE(time_changes == 0);

    send(PointerEventType::Down, position);
    REQUIRE(time_changes == 0);
    send(PointerEventType::Up, position);
    REQUIRE(time_changes == 1);
    REQUIRE(selected_time.Get() == Minutes{60 + 30});
    REQUIRE(FindNode(*runtime.BuildCommit().semantic_frame, SemanticRole::Button, "Hour").selected == true);
  }
}

TEST_CASE("TimePickerPeriodAvailabilityUpdatesPaintInputAndSemanticsTogether") {
  time_changes = 0;
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{ChangingAvailabilityTimePickerApp, platform};
  runtime.SetWindowMetrics({.viewport = {360.0F, 420.0F}});
  const auto initial = runtime.BuildCommit().semantic_frame;
  const SemanticNode& am = FindNode(*initial, SemanticRole::Button, "AM");
  const Point position{am.bounds.x + am.bounds.width * 0.5F, am.bounds.y + am.bounds.height * 0.5F};
  runtime.HandlePointerEvent({.type = PointerEventType::Down, .pointer_id = 84, .position = position});
  SECTION("The target period becomes unavailable during a press") { time_candidates_disabled = true; }
  SECTION("The owner is disabled during a press") { pickers_enabled = false; }
  const auto disabled = runtime.BuildCommit().semantic_frame;
  const SemanticNode& unavailable = FindNode(*disabled, SemanticRole::Button, "AM");
  REQUIRE(unavailable.id == am.id);
  REQUIRE_FALSE(unavailable.enabled);
  REQUIRE(unavailable.actions == 0);
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(am.id, {SemanticActionKind::Activate, std::monostate{}}));
  runtime.HandlePointerEvent({.type = PointerEventType::Up, .pointer_id = 84, .position = position});
  ClickAt(runtime, position);
  REQUIRE(time_changes == 0);
  REQUIRE(selected_time.Get() == Minutes{13 * 60 + 30});
  if (time_candidates_disabled.Get()) {
    REQUIRE(FindText(runtime.BuildFrame(), "AM")->style.foreground == TimePickerStyle::Default().disabled_foreground);
  }
  time_candidates_disabled = false;
  pickers_enabled = true;
  const auto enabled = runtime.BuildCommit().semantic_frame;
  REQUIRE(FindNode(*enabled, SemanticRole::Button, "AM").id == am.id);
  REQUIRE(FindNode(*enabled, SemanticRole::Button, "AM").enabled);
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(am.id, {SemanticActionKind::Activate, std::monostate{}}));
  REQUIRE(time_changes == 1);
  REQUIRE(selected_time.Get() == Minutes{60 + 30});
}

TEST_CASE("TimePickerReusesMeasuredLabelsAndRefreshesAvailabilityOnRecomposition") {
  PickerMeasuringPlatform platform{BuiltinTestResources()};
  Runtime runtime{ChangingAvailabilityTimePickerApp, platform};
  runtime.SetWindowMetrics({.viewport = {360.0F, 420.0F}});
  const auto initial = runtime.BuildCommit().semantic_frame;
  const SemanticNodeId hour = FindNode(*initial, SemanticRole::GridCell, "2").id;
  const std::size_t measured = platform.measurements;
  REQUIRE(measured > 0);
  for (int frame = 0; frame < 3; ++frame) {
    runtime.BuildFrame();
    REQUIRE(platform.measurements == measured);
  }

  time_candidates_disabled = true;
  const FlattenedScene& changed = runtime.BuildFrame();
  REQUIRE(platform.measurements > measured);
  REQUIRE_FALSE(FindNode(*runtime.LastCommit().semantic_frame, SemanticRole::GridCell, "2").enabled);
  REQUIRE(FindNode(*runtime.LastCommit().semantic_frame, SemanticRole::GridCell, "2").id == hour);
  REQUIRE(FindText(changed, "2")->style.foreground == TimePickerStyle::Default().disabled_foreground);

  time_candidates_disabled = false;
  runtime.BuildFrame();
  REQUIRE(FindNode(*runtime.LastCommit().semantic_frame, SemanticRole::GridCell, "2").enabled);
  selected_time = Minutes{15 * 60 + 30};
  runtime.BuildFrame();
  REQUIRE(FindNode(*runtime.LastCommit().semantic_frame, SemanticRole::GridCell, "3").selected == true);
  const std::size_t refreshed = platform.measurements;
  runtime.BuildFrame();
  REQUIRE(platform.measurements == refreshed);
  runtime.SetWindowMetrics({.viewport = {200.0F, 420.0F}});
  runtime.BuildFrame();
  REQUIRE(platform.measurements > refreshed);
  REQUIRE(FindPicker(*runtime.LastCommit().semantic_frame, "Departure time").bounds.width == 200.0F);
}

TEST_CASE("DatePickerEmptyMonthsDoNotActivateAnOffscreenDate") {
  date_changes = 0;
  TestPlatform platform{BuiltinTestResources()};
  Runtime runtime{[]() -> View {
    selected_date = UseState(Date{std::chrono::year{2024}, std::chrono::March, std::chrono::day{16}});
    return huxerui::DatePicker(selected_date)
        .DisabledDates([](Date value) { return value.month() == std::chrono::April; })
        .Label("Travel date")
        .OnChanged([](Date value) {
          ++date_changes;
          selected_date = value;
        });
  }, platform};
  runtime.SetWindowMetrics({.viewport = {420.0F, 440.0F}});
  const auto initial = runtime.BuildCommit().semantic_frame;
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(
      FindPicker(*initial, "Travel date").id, {SemanticActionKind::Focus, std::monostate{}}
  ));
  runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::PageDown});
  REQUIRE(FindText(runtime.BuildFrame(), "April 2024") != nullptr);
  runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Enter});
  REQUIRE(date_changes == 0);
  runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::ArrowRight});
  runtime.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Enter});
  REQUIRE(date_changes == 1);
  REQUIRE(selected_date.Get() == Date{std::chrono::year{2024}, std::chrono::May, std::chrono::day{1}});
}

} // namespace huxerui::test
