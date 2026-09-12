#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/event.h>
#include <huxerui/resource.h>
#include <huxerui/scroll.h>
#include <huxerui/semantics.h>
#include <huxerui/text_input.h>
#include <huxerui/theme.h>
#include <huxerui/view.h>

namespace huxerui::detail {
class UiTestSession;
struct UiSnapshotData;
}

namespace huxerui::testing {

/// Reports a missing or ambiguous target, an unavailable operation, or an exhausted test budget.
///
/// Invalid caller arguments use std::invalid_argument; wrong-thread and reentrant access use std::logic_error.
/// Application exceptions retain their original types rather than being wrapped in this exception.
/// Query mismatches and bounded wait failures do not themselves invalidate the fixture.
/// @code
/// try {
///   ui.Find(UiSelector::Text("Save")).Tap();
/// } catch (const UiTestFailure& failure) {
///   std::string diagnostic = failure.what();
/// }
/// @endcode
class UiTestFailure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// Key alternatives follow View::Key without numeric coercion or stringification.
/// Signed 42, unsigned 42, and the string "42" are distinct identities; keys are unique only among siblings.
/// @see UiSelector::Key
using UiKey = std::variant<std::int64_t, std::uint64_t, std::string>;

/// Owns one completed-frame observation. This is neither a live handle nor a subtree snapshot.
///
/// Values survive later frames and fixture destruction. Changing this value does not affect the application.
/// Geometry describes the shared layout and presentation model, not native rendering or screen coordinates.
/// @code
/// UiNodeInfo before = ui.Find(UiSelector::Key("save")).One();
/// ui.Pump(std::chrono::milliseconds(100));
/// UiNodeInfo after = ui.Find(UiSelector::Key("save")).One();
/// bool moved = before.bounds != after.bounds;
/// @endcode
struct UiNodeInfo {
  /// Descriptive retained category, such as "Text", "Button", or "Layout", not a C++ RTTI name.
  std::string type;
  /// Existing reconciliation key, if declared.
  std::optional<UiKey> key;
  /// Resolved own text; absent for non-text nodes and distinct from empty text.
  std::optional<std::string> text;
  /// Built-in TextField content without requiring focus; absent for secure or unsupported clients.
  std::optional<std::string> value;
  /// Local measured extent in logical units.
  Size size;
  /// Full transformed window-local axis-aligned bounding box in logical units, before clipping or occlusion.
  Rect bounds;
  /// Conservative intersection with the viewport and rectangular ancestor clips; empty when not participating.
  /// Rotation, path clips, opacity, and occlusion still require Runtime hit testing.
  Rect visible_bounds;
  /// Effective inherited interaction availability.
  bool enabled = true;
  /// Runtime input focus, not platform accessibility focus.
  bool focused = false;
  /// Whether this node and all its ancestors participate in layout.
  bool participates_in_layout = false;
  /// Whether laid-out bounds intersect the viewport and rectangular ancestor clips with positive area.
  /// Path clips, opacity, and occlusion are not tested; true does not guarantee that Tap() activates this node.
  bool in_viewport = false;
};

/// Matches real mounted nodes, independently of accessibility merging and hiding.
///
/// Factories own their matching values and do not inspect a fixture. Expressions are reusable across fixtures.
/// Use AllOf() for conjunction and UiNodeQuery::Find() for strict-descendant scoping.
/// @code
/// auto save = UiSelector::AllOf(UiSelector::Type<Button>(), UiSelector::Text("Save"), UiSelector::Enabled(true));
/// auto action = ui.Find(UiSelector::Key("account-row")).Find(save);
/// bool present = action.Exists();
/// @endcode
class UiSelector {
public:
  /// Matches exact, case-sensitive resolved own text, not descendant text or semantic labels.
  /// Nodes without an own text observation never match, including when text is empty.
  /// @param text UTF-8 content to match, owned by the resulting expression.
  static UiSelector Text(std::string text);
  /// Matches non-secure built-in editor content without focusing the editor.
  /// Nodes without a value observation, including secure editors, never match an empty value either.
  /// @param value Exact UTF-8 content to match.
  static UiSelector Value(std::string value);
  /// Matches a reconciliation key using the same alternative as View::Key.
  /// @param key Stable sibling identity; use descendant scope when keys repeat.
  static UiSelector Key(std::string key);
  /// Matches a string key, copying the supplied view into owned storage.
  /// @param key Exact string identity; the source need not outlive the expression.
  static UiSelector Key(std::string_view key);
  /// Matches a null-terminated string key, copying its content into owned storage.
  /// @param key Non-null string identity.
  /// @throws std::invalid_argument If key is null.
  static UiSelector Key(const char* key);
  /// Matches only the signed integer key alternative.
  /// @param key Exact signed identity, distinct from an equal unsigned or string key.
  static UiSelector Key(std::int64_t key);
  /// Matches only the unsigned integer key alternative.
  /// @param key Exact unsigned identity, distinct from an equal signed or string key.
  static UiSelector Key(std::uint64_t key);
  /// Converts an integral key using the same signedness-preserving rule as View::Key.
  /// @tparam T Integral type other than bool.
  /// @param key Identity converted to std::int64_t or std::uint64_t according to T's signedness.
  template <std::integral T> requires(!std::same_as<T, bool>)
  static UiSelector Key(T key) {
    if constexpr (std::is_signed_v<T>) return Key(static_cast<std::int64_t>(key));
    else return Key(static_cast<std::uint64_t>(key));
  }
  /// Matches an enum key through its underlying integral type, as View::Key does.
  /// @tparam T Enumeration whose underlying signedness selects the integer alternative.
  /// @param key Enum value identifying the node, not its enumerator name.
  template <class T> requires std::is_enum_v<T>
  static UiSelector Key(T key) { return Key(static_cast<std::underlying_type_t<T>>(key)); }
  /// Matches effective inherited enabled state.
  /// @param enabled Expected state.
  static UiSelector Enabled(bool enabled);
  /// Matches Runtime input focus.
  /// @param focused Expected state.
  static UiSelector Focused(bool focused);
  /// Matches every supplied expression. Empty conjunctions are invalid.
  /// @param selectors Nonempty list of predicates.
  /// @throws std::invalid_argument If selectors is empty.
  static UiSelector AllOf(std::initializer_list<UiSelector> selectors);
  /// Matches a nonempty conjunction written as separate arguments.
  /// @tparam Rest Additional UiSelector argument types; an empty pack is allowed.
  /// @param first Required first predicate.
  /// @param rest Additional predicates, all of which must match the same node.
  template <class... Rest> requires (std::same_as<Rest, UiSelector> && ...)
  static UiSelector AllOf(UiSelector first, Rest... rest) {
    return AllOf({std::move(first), std::move(rest)...});
  }
  /// Matches a supported retained primitive; custom View-producing functions are not retained types.
  /// Unsupported types are rejected at compile time. This does not match a custom function by its return type.
  /// @tparam T Text, Button, IconButton, TextField, Checkbox, RadioButton, Switch, Slider, Image, or Canvas.
  /// @code
  /// auto editor = ui.Find(UiSelector::AllOf(UiSelector::Type<TextField>(), UiSelector::Key("name")));
  /// @endcode
  template <class T> static UiSelector Type() {
    if constexpr (std::same_as<T, huxerui::Text>) return Kind("Text");
    else if constexpr (std::same_as<T, Button>) return Kind("Button");
    else if constexpr (std::same_as<T, IconButton>) return Kind("IconButton");
    else if constexpr (std::same_as<T, TextField>) return Kind("TextField");
    else if constexpr (std::same_as<T, Checkbox>) return Kind("Checkbox");
    else if constexpr (std::same_as<T, RadioButton>) return Kind("RadioButton");
    else if constexpr (std::same_as<T, Switch>) return Kind("Switch");
    else if constexpr (std::same_as<T, Slider>) return Kind("Slider");
    else if constexpr (std::same_as<T, Image>) return Kind("Image");
    else if constexpr (std::same_as<T, Canvas>) return Kind("Canvas");
    else static_assert(!std::same_as<T, T>, "HuxerUI testing Type requires a supported retained primitive");
  }

private:
  UiSelector(std::string description, std::function<bool(const UiNodeInfo&)> matches);
  static UiSelector Kind(std::string type);
  std::string description_;
  std::function<bool(const UiNodeInfo&)> matches_;
  friend class huxerui::detail::UiTestSession;
};

/// Matches committed accessibility output; Label intentionally differs from ordinary Text.
///
/// Factories own their values and are independent of a fixture. Matching follows the published semantic tree,
/// including virtual semantic nodes; it does not bypass accessibility merging or descendant exclusion.
/// String matches are exact and case-sensitive.
/// @code
/// auto save = UiSemanticSelector::AllOf(UiSemanticSelector::Role(SemanticRole::Button),
///                                       UiSemanticSelector::Label("Save changes"));
/// bool accessible = ui.FindSemantics(save).Exists();
/// @endcode
class UiSemanticSelector {
public:
  /// Matches an author-provided semantic identifier, not a View::Key or runtime node ID.
  /// @param identifier Exact Semantics identifier; use query scoping if it is not unique.
  static UiSemanticSelector Identifier(std::string identifier);
  /// Matches the resolved accessible label, which may differ from visible text.
  /// @param label Exact resolved accessibility label.
  static UiSemanticSelector Label(std::string label);
  /// Matches an accessible value while excluding secure semantic nodes.
  /// @param value Exact accessible value; secure content never matches, even for an empty string.
  static UiSemanticSelector Value(std::string value);
  /// Matches the published shared accessibility role.
  /// @param role Expected shared semantic role.
  static UiSemanticSelector Role(SemanticRole role);
  /// Matches effective enabled state in the semantic tree.
  /// @param enabled Expected effective availability.
  static UiSemanticSelector Enabled(bool enabled);
  /// Matches Runtime input focus, not a screen reader's independent accessibility focus.
  /// @param focused Expected Runtime input focus.
  static UiSemanticSelector Focused(bool focused);
  /// Matches an explicitly published selection state.
  /// @param selected Expected state; an absent state does not match false.
  static UiSemanticSelector Selected(bool selected);
  /// Matches an explicitly published checked, unchecked, or mixed state.
  /// @param checked Expected state; absent state does not match.
  static UiSemanticSelector Checked(SemanticCheckedState checked);
  /// Matches every supplied expression against the same semantic node.
  /// @param selectors Nonempty conjunction of semantic predicates.
  /// @throws std::invalid_argument If selectors is empty.
  static UiSemanticSelector AllOf(std::initializer_list<UiSemanticSelector> selectors);
  /// Matches a nonempty conjunction written as separate arguments.
  /// @tparam Rest Additional UiSemanticSelector argument types; an empty pack is allowed.
  /// @param first Required first predicate.
  /// @param rest Additional predicates, all of which must match the same semantic node.
  template <class... Rest> requires (std::same_as<Rest, UiSemanticSelector> && ...)
  static UiSemanticSelector AllOf(UiSemanticSelector first, Rest... rest) {
    return AllOf({std::move(first), std::move(rest)...});
  }

private:
  UiSemanticSelector(std::string description, std::function<bool(const SemanticNode&)> matches);
  std::string description_;
  std::function<bool(const SemanticNode&)> matches_;
  friend class huxerui::detail::UiTestSession;
};

/// High-level pointer defaults are portable touch input, not operating-system events.
/// The same device and identity are used for every event in one Tap() or Drag() sequence.
/// @code
/// ui.TapAt({40.0F, 30.0F}, {.device_kind = PointerDeviceKind::Mouse, .pointer_id = 7});
/// @endcode
struct UiPointerOptions {
  /// Normalized device kind, independent of the runner's hardware.
  PointerDeviceKind device_kind = PointerDeviceKind::Touch;
  /// Must not collide with an active raw pointer when starting a high-level gesture.
  std::int64_t pointer_id = 1;
};

/// Configures a linearly interpolated pointer gesture and its virtual movement frames.
/// Down and Up each commit an additional frame without advancing time; segments counts Move events only.
/// @code
/// UiDragOptions drag{.duration = std::chrono::milliseconds(300), .segments = 12};
/// ui.Drag({20.0F, 40.0F}, {180.0F, 40.0F}, drag);
/// @endcode
struct UiDragOptions {
  /// Finite nonnegative virtual duration of the movement sequence.
  std::chrono::duration<double> duration{0.2};
  /// Number of Move events, between 1 and 10000, with one Pump after each.
  std::size_t segments = 10;
  /// Device and identity retained for the complete gesture.
  UiPointerOptions pointer;
};

/// Bounds PumpUntil() and PumpAndSettle() by virtual time and committed frames.
/// PumpUntil checks its predicate first; PumpAndSettle always starts by committing one frame.
/// @code
/// UiPumpOptions wait{.timeout = std::chrono::seconds(1), .step = std::chrono::milliseconds(10)};
/// ui.PumpUntil([&] { return ui.Find(UiSelector::Text("Done")).Exists(); }, wait);
/// @endcode
struct UiPumpOptions {
  /// Finite nonnegative virtual timeout, not a wall-clock timeout. Zero forbids time advancement.
  std::chrono::duration<double> timeout{2.0};
  /// Finite positive virtual step, shortened at the timeout. PumpAndSettle may jump to a later deadline.
  std::chrono::duration<double> step{0.016};
  /// Positive limit on Pump calls, including PumpAndSettle's initial frame, independent of the timeout.
  std::size_t maximum_frames = 10000;
};

/// Bounds real wheel scrolling while searching a container's mounted descendants.
/// @code
/// auto item = list.ScrollUntil(UiSelector::Key("message.100"), {.step = {0.0F, 240.0F}});
/// item.Tap();
/// @endcode
struct UiScrollOptions {
  /// Finite, nonzero wheel delta in logical units for each attempt.
  Point step{0.0F, 240.0F};
  /// Finite positive virtual interval committed after each wheel update.
  std::chrono::duration<double> interval{0.1};
  /// Positive maximum number of wheel updates; an initially visible unique target needs none.
  std::size_t maximum_steps = 20;
};

/// Configures one windowless fixture without changing the borrowed Application declaration.
/// Resource payloads are explicit: supply resource_provider when the application needs packaged assets or icons.
/// @code
/// UiTestOptions options{
///   .viewport = {390.0F, 844.0F},
///   .resources = {.locale = Locale::FromLanguageTag("fr")},
/// };
/// UiTest localized(application, options);
/// @endcode
struct UiTestOptions {
  /// Initial logical surface extent; ordinary WindowMetrics validation applies.
  Size viewport{800.0F, 600.0F};
  /// Initial logical safe-area insets.
  EdgeInsets safe_area{};
  /// Authoritative test locale and density, independent of native defaults.
  ResourceConfiguration resources{};
  /// Authoritative system appearance reported to the application under test.
  SystemColorScheme system_color_scheme = SystemColorScheme::Light;
  /// Optional owning payload provider; the resources field above determines test locale and density.
  /// The provider's Configuration() is not used. No provider means no packaged payloads, not a filesystem search.
  std::shared_ptr<PlatformResources> resource_provider;
  /// Initial ordinary application activation delivered during Runtime construction.
  ApplicationActivation activation = LaunchActivation{};
  /// Maximum callbacks admitted in one frame's entry batch; must be positive.
  /// Also bounds the final best-effort cleanup drain, including callbacks posted during teardown.
  /// An oversized frame-entry batch fails the session; shutdown discards work beyond the limit.
  std::size_t maximum_callbacks_per_frame = 10000;
};

/// Re-resolves every operation against the latest completed frame. Does not retain mounted pointers.
///
/// A query does not keep its fixture alive. Resolving an expired query throws UiTestFailure; resolving on another
/// thread or reentering frame/input processing throws std::logic_error. An invalidated session cannot be queried.
/// Reads never pump. Matches include mounted layers and virtualization overscan, but not unmounted items or native
/// descendants. Offscreen or nonparticipating retained nodes may still match.
/// All ancestor scopes must resolve uniquely, even when the final operation only asks Exists() or Count().
/// @code
/// auto remove = ui.Find(UiSelector::Key("account-row")).Find(UiSelector::Text("Remove"));
/// if (remove.Exists()) {
///   UiNodeInfo node = remove.One();
///   if (node.enabled && node.in_viewport) remove.Tap();
/// }
/// @endcode
class UiNodeQuery {
public:
  /// Appends a strict-descendant scope without resolving nodes or modifying this query.
  /// The current query must match exactly one ancestor when the returned query is resolved; the ancestor itself
  /// is excluded from the next search. Scopes are resolved again on each operation, not frozen by this call.
  /// @param selector Expression to match beneath the current query's unique target.
  /// @return A new scoped query sharing the same fixture lifetime.
  [[nodiscard]] UiNodeQuery Find(UiSelector selector) const;
  /// Returns whether the final expression has any mounted matches, without pumping.
  /// @return False for no final matches; true for one or more, even if those matches are disabled or offscreen.
  [[nodiscard]] bool Exists() const;
  /// Returns match count; this does not imply visibility or interactability.
  /// @return Number of final matches in the last completed frame; zero is valid.
  [[nodiscard]] std::size_t Count() const;
  /// Returns an owning observation; throws UiTestFailure unless exactly one node matches.
  /// @return A value independent of later frames and fixture destruction.
  /// @throws UiTestFailure If the final target is missing or ambiguous.
  [[nodiscard]] UiNodeInfo One() const;
  /// Returns owning observations in mounted preorder.
  /// @return All final matches, or an empty vector when none match. Returned values are not live handles.
  [[nodiscard]] std::vector<UiNodeInfo> All() const;
  /// Sends Down/Up at the transformed local center, or the visible-bounds center when the former is clipped.
  /// Pumps after each event; never retries or activates directly.
  /// Runtime hit testing, clipping, and modal routing still decide the event recipient. A successful call does not
  /// guarantee activation of the selected node. No scrolling or semantic-action fallback is performed.
  /// The original point and pointer identity are kept through recomposition; failure attempts best-effort Cancel.
  /// @param options Pointer device and identity, which must not collide with an active raw pointer.
  /// @throws UiTestFailure If the target is not unique/enabled, lacks visible geometry, or the pointer is active.
  void Tap(UiPointerOptions options = {}) const;
  /// Sends a wheel update at the selected visible region and pumps once without advancing time.
  /// @param delta Finite logical content-offset delta. Touch scrolling uses UiTest::Drag instead.
  /// @return Delta consumed by normal Runtime routing, which may include nested scroll ancestors.
  /// @throws UiTestFailure If the target is missing, ambiguous, disabled, or outside the viewport.
  Point ScrollBy(Point delta) const;
  /// Repeatedly sends real wheel input until a unique descendant has visible geometry.
  /// Mounted overscan outside the viewport does not satisfy the search. No virtual item is created directly.
  /// @param target Selector scoped to strict descendants of this query's unique container.
  /// @param options Finite scrolling and virtual-time bounds.
  /// @return A reusable descendant query, resolved again by subsequent operations.
  /// @throws UiTestFailure On ambiguity, container replacement, or exhaustion of the step bound.
  /// @throws std::invalid_argument If delta, interval, resulting time, or step count is invalid.
  [[nodiscard]] UiNodeQuery ScrollUntil(UiSelector target, UiScrollOptions options = {}) const;
  /// Commits text through the selected active input client and pumps once.
  /// Does not focus the editor automatically. Uses the current selection/composition through the ordinary editing
  /// protocol; controlled applications decide the resulting value instead of receiving a direct State assignment.
  /// @param text UTF-8 editing intent; controlled applications may reject or transform it.
  /// @throws UiTestFailure If the target is not unique, does not own the active input session, or input is rejected.
  /// @code
  /// auto editor = ui.Find(UiSelector::Key("name"));
  /// editor.Tap();
  /// editor.EnterText("A😀B");
  /// editor.SetSelection({1, 3});
  /// editor.EnterText("X");
  /// @endcode
  void EnterText(std::string text) const;
  /// Replaces the selected active client's complete text through the ordinary editing protocol.
  /// Finishes composition, queries the total UTF-16 extent, submits replacement intent, and pumps once if accepted.
  /// Like EnterText(), this requires an already active editor and respects controlled application updates.
  /// @param text UTF-8 replacement, not a direct State assignment.
  /// @throws UiTestFailure If the target/session is unavailable, its extent cannot be read, or input is rejected.
  /// @code
  /// auto editor = ui.Find(UiSelector::Key("name"));
  /// editor.Tap();
  /// editor.ReplaceText("Ada");
  /// @endcode
  void ReplaceText(std::string text) const;
  /// Requests a selection change through the uniquely selected active editor and pumps once if accepted.
  /// Does not focus the editor or modify text directly; the application remains authoritative over editing state.
  /// @param selection Selection endpoints in UTF-16 code units, not UTF-8 bytes or Unicode scalar indices,
  /// including affinity and directional anchor/active endpoints.
  /// @throws UiTestFailure If the target does not own the active session or the editing command is rejected.
  /// @see EnterText
  void SetSelection(TextSelection selection) const;
  /// Performs the active client's configured input action and pumps once.
  /// Requires the unique target to own the input session. Does not synthesize an Enter key or force submission
  /// when the client's configured action has another meaning.
  /// @throws UiTestFailure If the editor/session is unavailable or the action is rejected.
  /// @code
  /// auto editor = ui.Find(UiSelector::Key("name"));
  /// editor.Tap();
  /// editor.ReplaceText("Ada");
  /// editor.Submit();
  /// @endcode
  void Submit() const;

private:
  UiNodeQuery(std::weak_ptr<huxerui::detail::UiTestSession> session, std::vector<UiSelector> selectors);
  std::weak_ptr<huxerui::detail::UiTestSession> session_;
  std::vector<UiSelector> selectors_;
  friend class UiTest;
};

/// Re-resolves expressions against the last committed accessibility tree, independently of ordinary node queries.
///
/// The query borrows the fixture lifetime and uses its creating thread. Resolution after fixture destruction throws
/// UiTestFailure; wrong-thread or reentrant resolution throws std::logic_error. Reads do not pump or force a new
/// semantic publication. Every ancestor scope must resolve uniquely, including for Exists(), Count(), and All().
/// @code
/// auto form = ui.FindSemantics(UiSemanticSelector::Identifier("account-form"));
/// auto submit = form.Find(UiSemanticSelector::Label("Save changes"));
/// SemanticNode before = submit.One();
/// submit.PerformSemanticAction({SemanticActionKind::Activate, {}});
/// @endcode
class UiSemanticQuery {
public:
  /// Appends a strict-descendant semantic scope without resolving nodes or modifying this query.
  /// The current target is excluded from descendant matching and must resolve uniquely on subsequent operations.
  /// @param selector Expression to match below the current semantic scope.
  /// @return A new scoped query sharing the same fixture lifetime.
  [[nodiscard]] UiSemanticQuery Find(UiSemanticSelector selector) const;
  /// Reports whether the final semantic expression matches, without requiring final-target uniqueness.
  /// @return True for one or more matches, including retained offscreen nodes; false for no final matches.
  [[nodiscard]] bool Exists() const;
  /// Counts final matches without pumping or filtering by visibility or enabled state.
  /// @return Number of semantic matches; zero is valid.
  [[nodiscard]] std::size_t Count() const;
  /// Returns a value copy, independent of later semantic publications.
  /// Node and child IDs belong to that publication; the copy is not a live subtree or a persistent action target.
  /// @return The single matching SemanticNode.
  /// @throws UiTestFailure If the final target is missing or ambiguous.
  [[nodiscard]] SemanticNode One() const;
  /// Returns owning copies of all final matches in semantic tree preorder.
  /// @return Matching SemanticNode values, or an empty vector. IDs retain their original publication meaning.
  [[nodiscard]] std::vector<SemanticNode> All() const;
  /// Uses Runtime's accessibility path, not physical input, then pumps once.
  /// Resolves the target again before dispatch. Normal semantic action availability, payload validation, and
  /// controlled component behavior apply; it does not synthesize pointer or key events.
  /// @param action Action to submit to the unique current target; its payload must match SemanticActionKind.
  /// @throws UiTestFailure If the target is not unique or Runtime rejects the action.
  void PerformSemanticAction(const SemanticAction& action) const;

private:
  UiSemanticQuery(std::weak_ptr<huxerui::detail::UiTestSession> session,
                  std::vector<UiSemanticSelector> selectors);
  std::weak_ptr<huxerui::detail::UiTestSession> session_;
  std::vector<UiSemanticSelector> selectors_;
  friend class UiTest;
};

/// Owns canonical structural data, not platform pixels or a mutable RenderScene.
///
/// Captures survive subsequent frames and fixture destruction. They describe semantic hierarchy and render intent,
/// excluding runtime identities, native handles, ExternalTexture pixels, and native PlatformView descendants.
/// Secure editor values are excluded, but ordinary application text and encoded image content are not anonymized.
/// The test runner owns assertions, storage, and baseline approval; this type does not write files.
/// @code
/// UiSnapshot before = ui.CaptureSnapshot();
/// ui.Pump(std::chrono::milliseconds(100));
/// UiSnapshot after = ui.CaptureSnapshot();
/// bool changed = before != after;
/// @endcode
class UiSnapshot {
public:
  /// Serializes the captured structure on demand. Does not access Runtime or advance time.
  /// This is a locale-independent structural format, not JSON or an image. Equal strings do not establish
  /// native rendering equality, especially for explicitly uncaptured external or native content.
  /// @return An owning versioned string, independent of the snapshot's lifetime. No text export is cached.
  [[nodiscard]] std::string ToString() const;

  bool operator==(const UiSnapshot& other) const;
  /// Reports named structural field changes, grouping inserted or removed subtrees.
  /// Does not compare serialized lines. Geometry follows the export's six-decimal canonical precision.
  /// @param expected Baseline snapshot; changes are reported from expected to this snapshot.
  /// @return Empty when equal; otherwise a bounded human-readable report without image bytes or runtime IDs.
  /// @code
  /// INFO(actual.Diff(expected));
  /// REQUIRE(actual == expected);
  /// @endcode
  [[nodiscard]] std::string Diff(const UiSnapshot& expected) const;
private:
  explicit UiSnapshot(std::shared_ptr<const huxerui::detail::UiSnapshotData> data) : data_(std::move(data)) {}
  std::shared_ptr<const huxerui::detail::UiSnapshotData> data_;
  friend class UiTest;
};

/// Runs an Application through the real shared Runtime without creating a native window.
///
/// Link HuxerUI::testing and include this header explicitly; it is not included by the production umbrella header.
/// The Application must outlive the fixture. Initialization executes ordinary root hooks and commits one frame at
/// virtual time zero. Create, use, and destroy the fixture on the same thread; do not reenter from application
/// callbacks. Wrong-thread and reentrant operations throw std::logic_error.
///
/// Frames use a scalar reference text layout, not native font shaping. This tests shared layout, input, state,
/// semantics, and render intent, not operating-system input conversion, permissions, native views, or pixels.
/// Direct platform work performed by application hooks is not sandboxed. Supply packaged resources explicitly
/// through UiTestOptions::resource_provider when required.
///
/// Exceptions while Runtime processes a frame, input, or configuration update retain their original type and
/// invalidate the session; subsequent operations fail with UiTestFailure. Query mismatches and bounded waits do not
/// themselves invalidate it. Teardown destroys Runtime, then runs a bounded best-effort callback drain without
/// committing frames or waiting for workers. Cleanup exceptions are contained and never replace an initialization error.
/// Examples in this header assume huxerui and huxerui::testing names are in scope, an Application named application,
/// and, where used, a fixture named ui with the illustrated application content.
/// @code
/// UiTest ui(application, {.viewport = {390.0F, 844.0F}});
/// ui.Find(UiSelector::Text("Save")).Tap();
/// ui.Pump(std::chrono::milliseconds(100));
/// const UiSnapshot snapshot = ui.CaptureSnapshot();
/// @endcode
class UiTest {
public:
  explicit UiTest(const Application& application, UiTestOptions options = {});
  ~UiTest();
  UiTest(const UiTest&) = delete;
  UiTest& operator=(const UiTest&) = delete;
  UiTest(UiTest&&) = delete;
  UiTest& operator=(UiTest&&) = delete;

  /// Creates an unresolved query over the full mounted tree, including root presentation layers.
  /// Does not pump, require a match, or require Semantics declarations on application Views.
  /// @param selector Mounted-node expression, independent of Semantics.
  /// @return A query tied to this fixture, not to the lifetime of any particular mounted node.
  [[nodiscard]] UiNodeQuery Find(UiSelector selector) const;
  /// Creates an unresolved query over the last committed semantic tree.
  /// Unlike Find(), this observes accessibility merging, virtual semantic children, and descendant exclusion.
  /// @param selector Accessibility-output expression.
  /// @return A semantic query tied to this fixture; no publication or input action is triggered.
  [[nodiscard]] UiSemanticQuery FindSemantics(UiSemanticSelector selector) const;
  /// Advances time once, runs the entry queue batch, and commits exactly one frame.
  /// Queued callbacks observe the advanced time. Work posted while the entry batch runs waits for a later Pump.
  /// State changes made since the previous frame become queryable after this call completes successfully.
  /// @param duration Finite nonnegative virtual duration; zero commits without advancing time. No intermediate
  /// frames are replayed, and no real-time sleep or worker synchronization is performed.
  /// @throws std::invalid_argument If duration or the resulting virtual time is invalid.
  /// @throws UiTestFailure If the entry callback batch exceeds the configured budget; the session then fails.
  /// @code
  /// ui.Pump();
  /// ui.Pump(std::chrono::milliseconds(250));
  /// UiSnapshot intermediate = ui.CaptureSnapshot();
  /// @endcode
  void Pump(std::chrono::duration<double> duration = {});
  /// Checks a read-only condition, then advances bounded virtual steps until it holds.
  /// Checks immediately, then after each Pump. Success does not establish global idleness. A timeout leaves the
  /// last completed frame available; it does not reset the clock or invalidate an otherwise healthy fixture.
  /// @param predicate Nonempty observational condition; may query the fixture but must not pump, send input,
  /// or update its configuration. Application state should be observed, not mutated, by this predicate.
  /// @param options Finite virtual timeout, positive finite step, and positive frame bound. External workers,
  /// HTTP, and browser promises are not waited for; the runner must enforce a separate wall-clock timeout.
  /// @throws std::invalid_argument If the predicate is empty or the time/frame bounds are invalid.
  /// @throws UiTestFailure If the condition remains false at a bound or the step cannot advance the clock.
  /// @code
  /// ui.PumpUntil([&] { return ui.Find(UiSelector::Text("Done")).Exists(); },
  ///              {.timeout = std::chrono::seconds(1), .step = std::chrono::milliseconds(16)});
  /// @endcode
  void PumpUntil(const std::function<bool()>& predicate, UiPumpOptions options = {});
  /// Commits an initial frame, then follows known frame requests and queued callbacks until none remain.
  /// Continuous frames advance by at least options.step; isolated later deadlines may be reached directly.
  /// Includes timers and caret blinking, not just animations; does not wait for external workers or network work.
  /// @param options Virtual timeout, minimum continuous-frame interval, and total frame bound including the initial frame.
  /// @throws UiTestFailure If work remains at a bound; the last completed frame remains queryable.
  /// @throws std::invalid_argument If time or frame bounds are invalid.
  /// @code
  /// ui.PumpAndSettle({.timeout = std::chrono::seconds(2)});
  /// @endcode
  void PumpAndSettle(UiPumpOptions options = {});
  /// Returns the most recent state delivered to the testing text-input adapter, or nullopt when inactive.
  /// Unlike completed-frame queries, this protocol observation may change immediately after raw input.
  /// @return An owning state with session, revisions, selection, and composition; no text content or native handle.
  /// @code
  /// auto session = ui.ActiveTextInput();
  /// if (session) ui.SendTextInput({session->session_id, commands});
  /// @endcode
  [[nodiscard]] std::optional<TextInputState> ActiveTextInput() const;
  /// Returns virtual seconds since construction.
  /// @return The current deterministic clock value; reading it does not advance time or commit a frame.
  [[nodiscard]] double Now() const;
  /// Updates metrics; observations remain unchanged until Pump.
  /// @param metrics Native-style logical metrics for this test surface.
  /// @throws std::invalid_argument If ordinary Runtime window-metric validation rejects the values.
  /// @code
  /// ui.SetWindowMetrics({.viewport = {800.0F, 600.0F}});
  /// ui.Pump();
  /// @endcode
  void SetWindowMetrics(WindowMetrics metrics);
  /// Updates locale/density through the ordinary resource service; query observations change on Pump.
  /// Does not replace the payload provider. The configuration overrides the provider's locale and density.
  /// @param configuration Valid resource configuration for subsequent resolution.
  /// @code
  /// ui.UpdateResourceConfiguration({.locale = Locale::FromLanguageTag("fr")});
  /// ui.Pump();
  /// @endcode
  void UpdateResourceConfiguration(ResourceConfiguration configuration);
  /// Forwards a raw pointer event without pumping, preserving normal hit testing, capture, and cancellation.
  /// Use complete pointer sequences with stable identities; no missing Down, Up, or Cancel is synthesized.
  /// @param event Normalized event with finite window-local logical coordinates and the intended button state.
  /// @throws std::invalid_argument If the position is not finite.
  /// @code
  /// PointerEvent event{.type = PointerEventType::Down, .pointer_id = 7, .position = {40.0F, 30.0F},
  ///                    .device_kind = PointerDeviceKind::Touch, .changed_button = PointerButton::Primary,
  ///                    .pressed_buttons = PointerButton::Primary};
  /// ui.SendPointer(event);
  /// event.type = PointerEventType::Cancel;
  /// event.pressed_buttons = PointerButton::None;
  /// ui.SendPointer(event);
  /// ui.Pump();
  /// @endcode
  void SendPointer(const PointerEvent& event);
  /// Forwards scroll without pumping and returns the consumed delta.
  /// @param event Normalized scroll intent in logical coordinates.
  /// @return Delta consumed by Runtime's normal nested-scroll routing, not the remaining delta.
  /// @code
  /// Point consumed = ui.SendScroll({.position = {40.0F, 80.0F}, .delta_y = 120.0F});
  /// ui.Pump();
  /// @endcode
  Point SendScroll(const ScrollInputEvent& event);
  /// Forwards a key without pumping and reports whether Runtime handled it.
  /// @param event Normalized portable key event; this does not synthesize text.
  /// @return True if Runtime handled the event; false if it remained unhandled.
  /// @see PressKey
  bool SendKey(const KeyEvent& event);
  /// Forwards an editing-command batch without pumping or focusing an editor.
  /// Session and command validation follow the ordinary Runtime protocol; a rejected result is returned, not
  /// converted into UiTestFailure. Use query editing helpers when manual session management is unnecessary.
  /// @param batch Session-aware editing commands whose text is UTF-8 and whose ranges use UTF-16 code units.
  /// @return The ordinary Runtime apply result, including rejection or stale-session information.
  /// In this example, batch is supplied by the test's text-input integration with its captured session ID.
  /// @code
  /// TextInputApplyResult result = ui.SendTextInput(batch);
  /// if (result.result_code == TextInputResultCode::Ok) ui.Pump();
  /// @endcode
  TextInputApplyResult SendTextInput(const TextInputCommandBatch& batch);
  /// Sends Down/Up at a fixed point, pumping after each event without advancing virtual time.
  /// Uses normal hit testing with no semantic-action fallback; no control is required to occupy the point.
  /// Failure attempts best-effort Cancel without hiding the original exception.
  /// @param position Finite logical window-local point inside the viewport.
  /// @param options Pointer device and identity, retained throughout the sequence.
  /// @throws std::invalid_argument If the position is not finite.
  /// @throws UiTestFailure If the point is outside the viewport or the pointer identity is already active.
  /// @see UiPointerOptions
  void TapAt(Point position, UiPointerOptions options = {});
  /// Sends Down, linearly interpolated Move events, and Up, pumping after each event.
  /// Advances virtual time before each Move; Down and Up do not advance time. Endpoints are not automatically
  /// clipped to the viewport, allowing capture and boundary tests. This does not directly change a scroll offset
  /// or guarantee a component gesture succeeds. Failure attempts best-effort Cancel.
  /// @param start Finite logical window-local origin.
  /// @param end Finite logical window-local destination.
  /// @param options Finite nonnegative virtual duration, 1..10000 movement segments, and pointer identity/device.
  /// @throws std::invalid_argument If coordinates, duration, resulting time, or segment count are invalid.
  /// @throws UiTestFailure If the pointer identity is already active.
  /// @see UiDragOptions
  void Drag(Point start, Point end, UiDragOptions options = {});
  /// Sends one key Down/Up pair, pumping after each event without advancing virtual time.
  /// Uses current Runtime focus and traversal; it neither focuses a selected query nor synthesizes text.
  /// Individual handled/unhandled results are ignored; use SendKey() to inspect them or control frame boundaries.
  /// @param key Portable key identity.
  /// @param modifiers Modifier state on Down and Up.
  /// @code
  /// ui.PressKey(Key::Tab);
  /// ui.PressKey(Key::Space);
  /// @endcode
  void PressKey(Key key, KeyModifiers modifiers = {});
  /// Captures owning semantic/render intent at the current completed frame without pumping.
  /// Raw input or State changes since that frame are not reflected until a successful Pump.
  /// @return An independent UiSnapshot that remains valid after later frames and fixture destruction.
  /// @see UiSnapshot
  [[nodiscard]] UiSnapshot CaptureSnapshot() const;

private:
  std::shared_ptr<huxerui::detail::UiTestSession> session_;
};

} // namespace huxerui::testing
