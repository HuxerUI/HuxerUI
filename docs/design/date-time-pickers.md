# Date and Time Pickers Design

This document defines the shared ownership, controlled values, localization, interaction, retained state, painting, and accessibility contracts for DatePicker and TimePicker.

## Goals

- Use standard chrono values at the public boundary.
- Keep the application authoritative for the selected date or time.
- Present one localized inline calendar and one localized clock face on every host.
- Share pointer, touch, keyboard, and accessibility behavior in the C++ core.
- Reuse Scope, typed events, Theme, Locale, Canvas painting, and NodeExtension without a Runtime component branch.
- Keep validation separate from availability rules.

## Non-goals

The components do not define a time zone, instant, calendar system other than the proleptic Gregorian calendar represented by `std::chrono::year_month_day`, combined DateTimePicker, recurrence rule, duration, or platform-native picker abstraction.

TimePicker does not define a public range.
Time windows can wrap midnight or contain disjoint intervals, so `DisabledTimes` is the single availability policy instead of assigning surprising semantics to minimum and maximum values.

The components do not expose a general date and time formatting service.
Their localized labels are a bounded internal use of the framework resource package and effective Locale.

## Public values and events

DatePicker receives `std::chrono::year_month_day` and emits the same type through `DatePickerEvents::Changed`.
The value must satisfy `ok()`.

TimePicker receives `std::chrono::minutes` since local midnight and emits the same type through `TimePickerEvents::Changed`.
The value belongs to `[0min, 1440min)` and has no time-zone interpretation.

The State overloads read the current State value at declaration time.
They do not create a two-way binding.
Applications handle `OnChanged` and decide whether to write the proposal back.

DatePicker supports inclusive `Range`, `Minimum`, and `Maximum` configuration.
The controlled value must stay inside the configured endpoints.
TimePicker supports a positive Step no greater than 60 minutes that divides 60 minutes exactly; its controlled value must align with that step.

`DisabledDates` and `DisabledTimes` are pure availability predicates retained with the declaration.
They suppress pointer, keyboard, and semantic proposals for matching candidates.
They do not reject the current controlled value and do not manufacture a validation message.
An application can therefore preserve a saved value whose business availability changed while presenting an explicit `ValidationResult`.

Invalid caller configuration throws `std::invalid_argument` at the public boundary.
Diagnostics begin with `HuxerUI`.

## Ownership and composition

Each public declaration creates a Scope View and installs one configuration modifier.
The modifier supplies a scope factory that resolves the effective Theme style, Locale, framework strings, validation text, and current event emitter.

The scope composes an optional visible label, one fixed-size Canvas surface, and optional validation text.
The Canvas owns one compatible retained NodeExtension.
No NodeKind, Runtime branch, platform callback, platform View, or native picker state is added.

The application owns:

- The authoritative date or time.
- Business validation and availability predicates.
- Whether the inline picker is placed directly in content, a Dialog, a popup, or another Layer.
- Any conversion between a time of day and a time zone or instant.

The retained extension owns only transient interaction and browsing state:

- DatePicker displayed month, active keyboard date, year-grid mode, hover, and pointer capture.
- TimePicker active hour or minute dial, direct-digit buffer, pointer capture, measured dial-label geometry, and duplicate-proposal suppression within one pointer drag.

When DatePicker receives a different controlled value, it returns the displayed month and active date to that value.
Ordinary recomposition with the same value preserves month and year browsing.
Changing the range clamps out-of-range browsing to the nearest allowed month and repairs the active keyboard date without emitting a value change.
Year-grid mode remains active, and a still-valid active year is preserved within the displayed page.
When TimePicker receives a different value, its active dial remains stable while selected geometry updates from the new controlled value.

## Locale resolution

The effective inherited Locale controls:

- Month and abbreviated weekday strings.
- Month and year-page navigation, year-choice, field, hour, minute, AM, and PM labels.
- First weekday.
- Year-first or month-first presentation.
- Left-to-right or right-to-left calendar columns.
- 12- or 24-hour clock presentation.

Strings use the built-in `huxerui` resource domain and ordinary locale fallback.
Every existing framework language and regional catalog supplies the complete picker key set, including Traditional Chinese for Taiwan, Hong Kong, and Macao and regional Portuguese variants.
Month names use stand-alone forms, weekday headings use compact short forms, and period labels use compact AM/PM forms checked against the CLDR 48 [Gregorian calendar data](https://github.com/unicode-org/cldr-json/tree/48.0.0/cldr-json/cldr-dates-full/main).
The existing English weekday abbreviations are retained; Korean periods use the localized wide forms `오전` and `오후`.
The data is stored in ordinary properties catalogs with no runtime CLDR dependency; its [Unicode license](../../resources/raw/licenses/unicode.txt) ships in the framework resource package.
Presentation profile selection remains internal so adding more locale data does not change the component API.
Explicit territory codes use the first-weekday and preferred hour-cycle rules from CLDR 48 [week data](https://github.com/unicode-org/cldr-json/blob/48.0.0/cldr-json/cldr-core/supplemental/weekData.json) and [time data](https://github.com/unicode-org/cldr-json/blob/48.0.0/cldr-json/cldr-core/supplemental/timeData.json), including the French-Canadian and Kurdish-Syrian hour-cycle overrides.
Regionless tags use bounded language defaults; full likely-subtag expansion and Unicode locale-extension overrides are not implemented.
Localized labels do not imply a general date formatter: field ordering, punctuation, numeric digits, and grammatical month inflection retain the bounded component formatting rules.

Locale changes replace resolved configuration through ordinary composition.
They do not mutate the controlled value or create a second resource context.

## DatePicker calendar

The date surface contains a previous action, month and year title, next action, weekday row, and fixed six-by-seven day grid.
The grid begins at the effective locale's first weekday.
Adjacent-month cells remain visible and use the same range and disabled predicate as current-month cells.

Selecting the title enters a twenty-year grid.
Selecting a year changes only the displayed year and returns to the day grid.
A date event is emitted only after a selectable day is activated.

Left and Right move one day in visual direction, Up and Down move one week, Home and End move to locale week edges, Page Up and Page Down move by month, and Shift with Page Up or Page Down moves by year.
Enter and Space propose the active date.
Escape leaves the year grid without changing the value.
Keyboard movement skips disabled dates and stops at configured range boundaries.
Month paging retains the closest selectable day in the target month when one exists, and year-page navigation remains available whenever any year on the target page intersects the range.
If a month has no selectable day, the active date stays in that month and inside the range, but activation cannot propose the unavailable date.

## TimePicker clock

The time surface contains an `HH:MM` header, optional AM and PM actions, and a circular dial.
The active header segment selects the hour or minute dial.

The 12-hour dial uses one ring with 12 at the top.
The 24-hour dial uses an outer ring for 1 through 12 and an inner ring for 13 through 23 and 00.
The minute dial has 60 logical angular positions and visible labels every five minutes.
Pointer angle is quantized to Step before a proposal is emitted.

An hour is available when at least one step-aligned minute in that hour is not disabled.
Selecting an hour preserves the current minute when possible and otherwise chooses the nearest available step-aligned minute in that hour.
Selecting an hour by pointer advances to the minute dial after pointer release.

AM/PM switches the current twelve-hour position to the corresponding hour in the requested period, using the same nearest-available-minute rule.
If that hour has no available minute, the period button is disabled in painting, pointer handling, and semantics; it does not jump to a different hour.
A period press commits only when released inside the same button, and Cancel or a release outside that button emits no change.
Availability is checked again at release so a configuration update cannot commit a newly disabled target.

Arrow keys change the active unit and wrap through the day while skipping disabled candidates.
Home and End request the first or last available step-aligned time.
Direct digits update the active hour or minute segment and quantize minutes to Step.
Enter and Space switch the active segment; Escape clears transient numeric entry and returns to the hour dial.

## Painting and measurement

Both components record platform-neutral PaintCommands only.
Calendar chevrons are lines rather than font glyphs.
The clock face uses circles, a line hand, and text.

TimePicker computes dial-label rectangles during `NodeExtension::PrepareGeometry` with the borrowed `TextMeasurer`.
The measurer is not retained.
Changed bounds, style, locale, mode, value, or step invalidate the foreground sequence and semantic geometry.
Unchanged frame builds reuse measured dial labels instead of repeating text measurements.
Every configuration update refreshes the cache because availability predicates cannot be compared for equality.

Theme supplies separate `DatePickerStyle` and `TimePickerStyle` values.
Styles own colors, typography, padding, cell or dial dimensions, selection geometry, borders, labels, and validation presentation.
Geometry is validated before composition.

TimePicker measures its preferred content width as the larger of the dial diameter and the complete time header.
The header includes two field widths, the separator width, and, only in 12-hour presentation, period spacing and the period-group width.
Painting, pointer hit testing, and semantics share those rectangles.
The AM/PM selection is clipped to one rounded group with a shared outline and divider, not two independent pills.
The active time field, selected period, and dial handle have independent foreground/background pairs.
An invalid picker retains at least a one-DIP error outline even when the theme omits the ordinary surface border.

Material styling follows the [Material Components time-picker tokens](https://github.com/material-components/material-components-android/blob/master/lib/java/com/google/android/material/timepicker/res/values/tokens.xml): surface-container-high outer surface, surface-container-highest inactive fields and dial, primary-container active field, tertiary-container active period, and primary dial handle.
Flat styling uses its own compact dimensions, subtle outlines, and rounded-square calendar selection rather than inheriting Material geometry.
These remain inline HuxerUI components, not complete Material dialogs: dialog titles, input-mode switching, confirmation actions, and dialog elevation belong to a separate presentation design.

## Semantics

Each picker surface is one focusable semantic grid whose name comes from `Label` or the localized built-in field label.
Its semantic value is the formatted controlled value, and application validation remains a distinct invalid state and error string.

DatePicker contributes virtual button semantics for header actions and the 20 year choices, and virtual grid cells for the 42 visible dates.
Disabled candidates have no activation action.
Selected cells expose selected state and collection positions.

TimePicker contributes virtual button semantics for header segments and periods.
The hour dial exposes every hour candidate.
The minute dial exposes every step-aligned logical position, including positions without a visible five-minute label.
The owner also supports increment, decrement, and set-value actions subject to the same Step and disabled predicate.

Runtime supplies final enabled state, focus, transformed bounds, clipping, and action routing.
Virtual candidates declare availability through SemanticBuilder using the same checks as actual selection.
Runtime combines that availability with the host's enabled state, preserving disabled candidates and their identities while removing executable actions and publishing `enabled = false`.
Platform accessibility adapters consume the shared SemanticFrame and do not infer picker meaning from pixels.

## Validation

Focused tests cover invalid public configuration, inclusive date ranges, disabled dates and times, controlled event delivery and rejection, keyboard skipping, range-boundary browsing, pointer cancellation, disabled recomposition, semantic actions, locale changes across languages and regional variants, 12- and 24-hour cycles, theme resolution, and application-owned validation.
The built-in resource test compares every compiled string catalog with the default key set and rejects missing, duplicate, or blank entries without applying locale fallback.
The ordinary common and platform test targets continue to validate shared rendering and accessibility bridges.
