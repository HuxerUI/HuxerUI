#include "runtime_test_support.h"

namespace huxerui::test {

struct SearchSubmitted : Event<void(std::string)> {};
struct SearchAccepted : Event<bool(std::string_view)> {};
struct NodeEventTransfer : Event<std::unique_ptr<int>(std::unique_ptr<int>)> {};

struct NodeEventProbe {
  class Extension;
  int* destructions = nullptr;

  bool operator==(const NodeEventProbe&) const = default;
};

class NodeEventProbe::Extension final : public NodeExtension {
public:
  Extension() = default;
  Extension(ViewNode& node, const NodeEventProbe& modifier) { Update(node, modifier); }
  ~Extension() override {
    if (destructions_) {
      ++*destructions_;
    }
  }

  using NodeExtension::EmitEvent;

  void Update(ViewNode&, const NodeEventProbe& modifier) { destructions_ = modifier.destructions; }

  bool HitTest(ViewNode& node, Point position) const override { return node.Bounds().Contains(position); }

  PointerResult OnPointer(ViewNode&, const PointerEvent& event) override {
    if (event.type == PointerEventType::Up) {
      EmitEvent<SearchSubmitted>("pointer");
    }
    return event.type == PointerEventType::Down ? PointerResult::Capture : PointerResult::Handled;
  }

  void BuildSemantics(SemanticBuilder& builder) const override {
    builder.SetOwner(Semantics{.role = SemanticRole::Button});
    builder.AddAction(0, SemanticActionKind::Activate);
  }

  bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) override {
    if (local_id != 0 || action.kind != SemanticActionKind::Activate) {
      return false;
    }
    EmitEvent<SearchSubmitted>("semantic");
    return true;
  }

private:
  int* destructions_ = nullptr;
};

template <class Key, class... Arguments>
concept ExtensionCanEmit = requires(const NodeEventProbe::Extension& extension, Arguments&&... arguments) {
  extension.template EmitEvent<Key>(std::forward<Arguments>(arguments)...);
};

static_assert(ExtensionCanEmit<SearchSubmitted, std::string>);
static_assert(!ExtensionCanEmit<SearchSubmitted, int>);
static_assert(!ExtensionCanEmit<int>);
static_assert(std::same_as<
    decltype(std::declval<const NodeEventProbe::Extension&>().EmitEvent<SearchSubmitted>("")), void>);
static_assert(std::same_as<
    decltype(std::declval<const NodeEventProbe::Extension&>().EmitEvent<SearchAccepted>("")), std::optional<bool>>);

State<int> event_mode;

EventEmitter saved_event_emitter;
std::string received_event;

State<int> modifier_value;
State<bool> modifier_style_changed;
int extension_creations = 0;
int extension_updates = 0;
int extension_destroys = 0;
State<bool> property_modifier_value;
int property_modifier_applications = 0;
TextMeasurer* observed_text_measurer = nullptr;
ViewportClass observed_viewport_class = ViewportClass::Compact;
int viewport_compositions = 0;
StateList<std::string> observed_state_list;
StateList<int> empty_state_list;
StateList<int> unobserved_state_list;
int state_list_compositions = 0;
int unobserved_state_list_compositions = 0;
State<int> environment_boundary_value;
State<int> environment_boundary_trigger;
bool environment_boundary_should_throw = false;

State<int> operator_state;
int operator_state_compositions = 0;

struct AddableStateValue {
  int value = 0;

  bool operator==(const AddableStateValue&) const = default;

  friend AddableStateValue operator+(const AddableStateValue& left, int right) {
    return {left.value + right};
  }
};

struct SubtractAssignableStateValue {
  int value = 0;

  bool operator==(const SubtractAssignableStateValue&) const = default;

  SubtractAssignableStateValue& operator-=(int delta) {
    value -= delta;
    return *this;
  }
};

struct NonAssignableStateValue {
  NonAssignableStateValue() = default;
  NonAssignableStateValue(const NonAssignableStateValue&) = default;
  NonAssignableStateValue(NonAssignableStateValue&&) = default;
  NonAssignableStateValue& operator=(const NonAssignableStateValue&) = delete;
  NonAssignableStateValue& operator=(NonAssignableStateValue&&) = delete;

  NonAssignableStateValue& operator-=(int) {
    return *this;
  }

  NonAssignableStateValue& operator++() {
    return *this;
  }
};

struct CopyCountedStateValue {
  static inline int copies = 0;

  int value = 0;

  CopyCountedStateValue() = default;
  explicit CopyCountedStateValue(int initial) : value(initial) {}
  CopyCountedStateValue(const CopyCountedStateValue& other) : value(other.value) {
    ++copies;
  }
  CopyCountedStateValue(CopyCountedStateValue&&) noexcept = default;
  CopyCountedStateValue& operator=(const CopyCountedStateValue&) = default;
  CopyCountedStateValue& operator=(CopyCountedStateValue&&) noexcept = default;

  bool operator==(const CopyCountedStateValue&) const = default;

  CopyCountedStateValue& operator++() {
    ++value;
    return *this;
  }
};

enum class StateFlags : std::uint8_t {
  First = 1U,
  Second = 2U,
};

constexpr StateFlags operator|(StateFlags left, StateFlags right) {
  return static_cast<StateFlags>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

constexpr StateFlags& operator|=(StateFlags& left, StateFlags right) {
  left = left | right;
  return left;
}

State<std::string> string_operator_state;
State<StateFlags> flags_operator_state;
State<AddableStateValue> custom_operator_state;
State<SubtractAssignableStateValue> compound_operator_state;
State<std::unique_ptr<int>> move_only_operator_state;
State<CopyCountedStateValue> copy_counted_operator_state;

template <class T>
concept CompleteStateOperatorSurface = requires(const State<T>& state, T operand) {
  { state += operand } -> std::same_as<const State<T>&>;
  { state -= operand } -> std::same_as<const State<T>&>;
  { state *= operand } -> std::same_as<const State<T>&>;
  { state /= operand } -> std::same_as<const State<T>&>;
  { state %= operand } -> std::same_as<const State<T>&>;
  { state &= operand } -> std::same_as<const State<T>&>;
  { state |= operand } -> std::same_as<const State<T>&>;
  { state ^= operand } -> std::same_as<const State<T>&>;
  { state <<= operand } -> std::same_as<const State<T>&>;
  { state >>= operand } -> std::same_as<const State<T>&>;
  { ++state } -> std::same_as<const State<T>&>;
  { state++ } -> std::same_as<T>;
  { --state } -> std::same_as<const State<T>&>;
  { state-- } -> std::same_as<T>;
};

template <class T>
concept StateSupportsModuloAssignment = requires(const State<T>& state, T operand) { state %= operand; };

template <class T>
concept StateSupportsSubtractAssignment = requires(const State<T>& state) { state -= 1; };

template <class T>
concept StateSupportsPrefixIncrement = requires(const State<T>& state) { ++state; };

template <class T>
concept StateSupportsPostfixIncrement = requires(const State<T>& state) { state++; };

template <class T>
concept SupportsBinarySubtract = requires(const T& value) { value - 1; };

static_assert(CompleteStateOperatorSurface<int>);
static_assert(requires(const State<AddableStateValue>& state) { state += 1; });
static_assert(requires(const State<StateFlags>& state) { state |= StateFlags::Second; });
static_assert(StateSupportsSubtractAssignment<SubtractAssignableStateValue>);
static_assert(!SupportsBinarySubtract<SubtractAssignableStateValue>);
static_assert(StateSupportsSubtractAssignment<NonAssignableStateValue>);
static_assert(StateSupportsPrefixIncrement<NonAssignableStateValue>);
static_assert(StateSupportsPostfixIncrement<NonAssignableStateValue>);
static_assert(StateSupportsPostfixIncrement<CopyCountedStateValue>);
static_assert(!std::copy_constructible<std::unique_ptr<int>>);
static_assert(!StateSupportsModuloAssignment<std::string>);
static_assert(std::assignable_from<State<int>&, const State<int>&>);
static_assert(std::assignable_from<State<int>&, State<int>>);

struct ParameterizedEnvironmentValue {
  int value = 0;

  static ParameterizedEnvironmentValue Default() {
    return {};
  }

  bool operator==(const ParameterizedEnvironmentValue&) const = default;
};

int viewport_environment_compositions = 0;
int locale_environment_compositions = 0;

View ViewportEnvironmentContent() {
  ++viewport_environment_compositions;
  return Text(std::to_string(static_cast<int>(UseViewportClass()))).Shaping({.locale = "en-US"});
}

View LocaleEnvironmentContent() {
  ++locale_environment_compositions;
  return Text(UseEnvironment<Locale>().LanguageTag());
}

View PreciseEnvironmentDependencyApp() {
  return Column {
    Scope(ViewportEnvironmentContent),
    Scope(LocaleEnvironmentContent),
  };
}

View EnvironmentBoundaryContent() {
  HUXERUI_SCOPE({
    if (environment_boundary_should_throw) {
      throw std::runtime_error("environment content failed");
    }
    return Text("environment " + std::to_string(UseEnvironment<ParameterizedEnvironmentValue>().value));
  });
}

View EnvironmentBoundaryApp() {
  environment_boundary_value = UseState(1);
  environment_boundary_trigger = UseState(0);
  static_cast<void>(environment_boundary_trigger.Get());
  return ProvideEnvironment(
      ParameterizedEnvironmentValue{environment_boundary_value.Get()},
      EnvironmentBoundaryContent()
  );
}

View ParameterizedScopeContent(std::string label, int value) {
  return Text(label + " " + std::to_string(value));
}

View ParameterizedEnvironmentContent(std::string label) {
  return Scope(ParameterizedScopeContent, std::move(label), UseEnvironment<ParameterizedEnvironmentValue>().value);
}

View ParameterizedThemeContent(int value, std::string label) {
  return ProvideEnvironment(
      ParameterizedEnvironmentValue{value},
      Scope(ParameterizedEnvironmentContent, std::move(label))
  );
}

View ParameterizedFactoryApp() {
  return MaterialTheme {Scope(ParameterizedThemeContent, 42, std::string{"bound"})};
}

View ReconciliationThemeApp() {
  View content = Column {
    Button("themed"),
    Button("explicit").With(
        Padding{23.0F},
        Foreground{Color::Rgb(190, 40, 30)},
        FontSize{27.0F},
        Frame{.width = 144.0F}
    ),
    Button("explicit zero").With(Frame{.min_width = 0.0F}),
  };

  ButtonStyle style = ButtonStyle::Default();
  style.background = Color::Rgb(20, 90, 170);
  style.padding = EdgeInsets::All(9.0F);
  style.label_style = {
      Font::Monospace(19.0F).WithWeight(FontWeight::Bold),
      Color::Rgb(230, 240, 250),
      TextDecoration::Underline,
  };
  style.minimum_width = 72.0F;
  ThemeDefinition definition;
  definition.Set(std::move(style));
  return Theme {std::move(definition), std::move(content)};
}

View SharedDeclarationThemeApp() {
  View shared = Button("shared");
  const auto themed = [](View content, Color background) {
    ButtonStyle style = ButtonStyle::Default();
    style.background = background;
    ThemeDefinition definition;
    definition.Set(std::move(style));
    return Theme {std::move(definition), std::move(content)};
  };
  return Row {
    themed(shared, Color::Rgb(180, 30, 40)),
    themed(shared, Color::Rgb(20, 100, 180)),
  };
}

struct ProbeModifier;

View TextMeasurerApp() {
  observed_text_measurer = &UseTextMeasurer();
  return Text("measured");
}

View ViewportClassApp() {
  ++viewport_compositions;
  observed_viewport_class = UseViewportClass();
  return Text("viewport");
}

class ProbeModifierExtension final : public NodeExtension {
public:
  ProbeModifierExtension(ViewNode& node, const ProbeModifier& modifier);
  ~ProbeModifierExtension() override {
    ++extension_destroys;
  }

  void Update(ViewNode& node, const ProbeModifier& modifier);

  int value = 0;
};

struct ProbeModifier {
  using Extension = ProbeModifierExtension;

  int value;

  bool operator==(const ProbeModifier&) const = default;
};

struct PropertyProbe {
  static const detail::ModifierDescriptor& Descriptor();

  bool value = false;

  bool operator==(const PropertyProbe&) const = default;
};

const detail::ModifierDescriptor& PropertyProbe::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec,
         detail::ModifierSpec& modifier,
         const std::shared_ptr<const Environment>&,
         detail::AppResources&) {
        ++property_modifier_applications;
        spec.properties.clip_children = static_cast<const PropertyProbe*>(modifier.value.get())->value;
      },
      nullptr,
      nullptr,
      false,
      detail::ErasedEqualsFor<PropertyProbe>(),
      nullptr,
  };
  return descriptor;
}

struct OpaqueProbeModifier;

int opaque_extension_updates = 0;

class OpaqueProbeModifierExtension final : public NodeExtension {
public:
  OpaqueProbeModifierExtension(ViewNode& node, const OpaqueProbeModifier& modifier);
  void Update(ViewNode& node, const OpaqueProbeModifier& modifier);
};

struct OpaqueProbeModifier {
  using Extension = OpaqueProbeModifierExtension;

  int value;
};

OpaqueProbeModifierExtension::OpaqueProbeModifierExtension(ViewNode& node, const OpaqueProbeModifier& modifier) {
  static_cast<void>(node);
  static_cast<void>(modifier);
}

void OpaqueProbeModifierExtension::Update(ViewNode& node, const OpaqueProbeModifier& modifier) {
  static_cast<void>(node);
  static_cast<void>(modifier);
  ++opaque_extension_updates;
}

ProbeModifierExtension::ProbeModifierExtension(ViewNode& node, const ProbeModifier& modifier)
    : value(modifier.value) {
  static_cast<void>(node);
  ++extension_creations;
}

void ProbeModifierExtension::Update(ViewNode& node, const ProbeModifier& modifier) {
  static_cast<void>(node);
  value = modifier.value;
  ++extension_updates;
}

View EventSource() {
  HUXERUI_SCOPE({
    auto events = UseEvents();
    saved_event_emitter = events;
    return Button("Submit").OnClick([events] { events.Emit<SearchSubmitted>("query"); });
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
        EventSource()
            .Key("source")
            .On<SearchSubmitted>([](std::string value) { received_event = "second:" + value; })
            .On<SearchAccepted>([](std::string_view) { return false; }),
    };
  }

  return Column{
      EventSource()
          .Key("source")
          .On<SearchSubmitted>([](std::string value) { received_event = "replaced:" + value; })
          .On<SearchSubmitted>([](std::string value) { received_event = "first:" + value; })
          .On<SearchAccepted>([](std::string_view value) {
            if (value == "throw") {
              throw std::runtime_error("event decision failed");
            }
            return true;
          }),
  };
}

View CounterApp() {
  auto count = UseState(1);
  return Column{
      Text(count),
      Stack{
          Button("+1").OnClick([count] { count += 1; }),
      },
  }
      .With(huxerui::Spacing{4.0F});
}

View StateOperatorApp() {
  ++operator_state_compositions;
  operator_state = UseState(8);
  return Text(std::to_string(operator_state.Get()));
}

View StateOperatorTypesApp() {
  string_operator_state = UseState(std::string{"base"});
  flags_operator_state = UseState(StateFlags::First);
  custom_operator_state = UseState(AddableStateValue{3});
  compound_operator_state = UseState(SubtractAssignableStateValue{10});
  return Text(
      string_operator_state.Get() + ":" + std::to_string(static_cast<std::uint8_t>(flags_operator_state.Get())) + ":" +
      std::to_string(custom_operator_state.Get().value) + ":" + std::to_string(compound_operator_state.Get().value)
  );
}

View MoveOnlyStateOperatorApp() {
  move_only_operator_state = UseState(std::make_unique<int>(5));
  copy_counted_operator_state = UseState(CopyCountedStateValue{8});
  return Text(
      std::to_string(*move_only_operator_state.Get()) + ":" + std::to_string(copy_counted_operator_state.Get().value)
  );
}

View StateListApp() {
  ++state_list_compositions;
  auto items = UseStateList(std::vector<std::string>{
      "Alpha",
      "Bravo",
  });
  observed_state_list = items;
  return Column {
    ForEach(items, [](const std::string& item) { return Text(item).Key(item); }),
  };
}

View EmptyStateListApp() {
  auto items = UseStateList<int>();
  empty_state_list = items;
  return Text::Format("{}", items.Size());
}

View UnobservedStateListApp() {
  ++unobserved_state_list_compositions;
  unobserved_state_list = UseStateList<int>({1, 2});
  return Text("Stable");
}

View CopyOnWriteApp() {
  View original = Text("Shared");
  View modified = View(original).With(huxerui::Foreground{huxerui::Color::White()});
  return Column{
      original,
      modified,
  };
}

View TextStyleApp() {
  return Text("Styled")
      .Style({
          Font::Monospace(18.0F).WithWeight(FontWeight::Bold),
          Color::Rgb(10, 20, 30),
          TextDecoration::Underline,
      })
      .With(Foreground{Color::Rgb(40, 50, 60)}, FontSize{22.0F});
}

View ModifierApp() {
  auto value = UseState(1);
  auto style_changed = UseState(false);
  modifier_value = value;
  modifier_style_changed = style_changed;
  return Text("Modifier")
      .With(
          huxerui::Padding{5.0F},
          huxerui::Background{style_changed.Get() ? huxerui::Color::Black() : huxerui::Color::White()},
          ProbeModifier{value.Get()}
      );
}

View PropertyModifierApp() {
  auto value = UseState(false);
  property_modifier_value = value;
  return Text("Property modifier").With(PropertyProbe{value.Get()});
}

View ModifierCopyOnWriteApp() {
  View original = Text("Shared");
  View modified = View(original).With(huxerui::Foreground{huxerui::Color::White()});
  return Column{
      original,
      modified,
  };
}

View OpaqueModifierApp() {
  return Text("Opaque").With(OpaqueProbeModifier{1});
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
    children.emplace_back(Button(std::to_string(value.Get())).OnClick([value] { value += 1; }));
  }
  return Column(std::move(children));
}

int local_root_compositions = 0;
int left_scope_compositions = 0;
int right_scope_compositions = 0;

View CountedCounter(int* compositions) {
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

State<int> root_recovery_state;
bool root_composition_should_throw = false;

View RecoveringRootApp() {
  auto value = UseState(1);
  root_recovery_state = value;
  const int current = value.Get();
  if (root_composition_should_throw) {
    throw std::runtime_error("root composition failed");
  }
  return Text(std::to_string(current));
}

State<int> child_recovery_state;
State<int> child_recovery_trigger;
bool child_composition_should_throw = false;

View RecoveringChildScope() {
  HUXERUI_SCOPE({
    auto value = UseState(0);
    child_recovery_state = value;
    const int current = value.Get();
    if (child_composition_should_throw) {
      throw std::runtime_error("child composition failed");
    }
    return Text(std::to_string(current));
  });
}

View RecoveringChildApp() {
  auto trigger = UseState(0);
  child_recovery_trigger = trigger;
  return Column{
      Text(std::to_string(trigger.Get())),
      RecoveringChildScope(),
  };
}

class ThrowingModifierExtension final : public NodeExtension {
public:
  ThrowingModifierExtension(ViewNode& node, const struct ThrowingModifier& modifier);

  void Update(ViewNode& node, const struct ThrowingModifier& modifier);
};

struct ThrowingModifier {
  using Extension = ThrowingModifierExtension;
};

ThrowingModifierExtension::ThrowingModifierExtension(ViewNode& node, const ThrowingModifier& modifier) {
  static_cast<void>(node);
  static_cast<void>(modifier);
  throw std::runtime_error("modifier creation failed");
}

void ThrowingModifierExtension::Update(ViewNode& node, const ThrowingModifier& modifier) {
  static_cast<void>(node);
  static_cast<void>(modifier);
}

State<bool> throwing_modifier_visible;

View RecoveringModifierApp() {
  auto visible = UseState(false);
  throwing_modifier_visible = visible;
  View content = Text("modifier recovery").With(ProbeModifier{7});
  if (visible.Get()) {
    content = std::move(content).With(ThrowingModifier{});
  }
  return content;
}

TEST_CASE("TestUseStateAndStateUpdate") {
  TestPlatform platform;
  Runtime runtime{CounterApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  const FlattenedScene& initial = runtime.BuildFrame();
  REQUIRE(FirstText(initial) == "1");

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const std::uint64_t root_identity = root->identity;

  runtime.InvalidateRoot();
  const FlattenedScene& recomposed = runtime.BuildFrame();
  REQUIRE(FirstText(recomposed) == "1");
  REQUIRE(runtime.RootNode()->identity == root_identity);

  ClickAt(runtime, {10.0F, 42.0F});
  REQUIRE(platform.requested_frames > 0);

  const FlattenedScene& updated = runtime.BuildFrame();
  REQUIRE(FirstText(updated) == "2");
  REQUIRE(runtime.RootNode()->identity == root_identity);
}

TEST_CASE("TestStateOperatorsUseTheEqualityAwareWritePath") {
  operator_state = State<int>{};
  operator_state_compositions = 0;

  TestPlatform platform;
  Runtime runtime{StateOperatorApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  REQUIRE(FirstText(runtime.BuildFrame()) == "8");
  REQUIRE(operator_state_compositions == 1);

  int expected_compositions = 1;
  auto mutate_and_expect = [&](auto&& mutation, int expected) {
    const int requested_frames = platform.requested_frames;
    std::forward<decltype(mutation)>(mutation)();
    REQUIRE(platform.requested_frames == requested_frames + 1);
    REQUIRE(FirstText(runtime.BuildFrame()) == std::to_string(expected));
    ++expected_compositions;
    REQUIRE(operator_state_compositions == expected_compositions);
  };

  mutate_and_expect([] { operator_state += 2; }, 10);
  mutate_and_expect([] { operator_state -= 3; }, 7);
  mutate_and_expect([] { operator_state *= 6; }, 42);
  mutate_and_expect([] { operator_state /= 2; }, 21);
  mutate_and_expect([] { operator_state %= 5; }, 1);
  mutate_and_expect([] { operator_state |= 8; }, 9);
  mutate_and_expect([] { operator_state &= 10; }, 8);
  mutate_and_expect([] { operator_state ^= 3; }, 11);
  mutate_and_expect([] { operator_state <<= 1; }, 22);
  mutate_and_expect([] { operator_state >>= 2; }, 5);
  mutate_and_expect([] { static_cast<void>(++operator_state); }, 6);

  int previous = 0;
  mutate_and_expect([&] { previous = operator_state++; }, 7);
  REQUIRE(previous == 6);

  mutate_and_expect([] { static_cast<void>(--operator_state); }, 6);
  mutate_and_expect([&] { previous = operator_state--; }, 5);
  REQUIRE(previous == 6);

  const int requested_frames = platform.requested_frames;
  operator_state += 0;
  REQUIRE(platform.requested_frames == requested_frames);
  REQUIRE(FirstText(runtime.BuildFrame()) == "5");
  REQUIRE(operator_state_compositions == 15);

  State<int> empty;
  REQUIRE_THROWS_AS(empty -= 1, std::logic_error);
  REQUIRE_THROWS_AS(empty++, std::logic_error);
}

TEST_CASE("TestStateOperatorsSupportStringEnumAndCustomValues") {
  string_operator_state = State<std::string>{};
  flags_operator_state = State<StateFlags>{};
  custom_operator_state = State<AddableStateValue>{};
  compound_operator_state = State<SubtractAssignableStateValue>{};

  TestPlatform platform;
  Runtime runtime{StateOperatorTypesApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  REQUIRE(FirstText(runtime.BuildFrame()) == "base:1:3:10");

  int requested_frames = platform.requested_frames;
  string_operator_state += "-next";
  REQUIRE(platform.requested_frames == requested_frames + 1);
  REQUIRE(FirstText(runtime.BuildFrame()) == "base-next:1:3:10");

  requested_frames = platform.requested_frames;
  flags_operator_state |= StateFlags::Second;
  REQUIRE(platform.requested_frames == requested_frames + 1);
  REQUIRE(FirstText(runtime.BuildFrame()) == "base-next:3:3:10");

  requested_frames = platform.requested_frames;
  custom_operator_state += 4;
  REQUIRE(platform.requested_frames == requested_frames + 1);
  REQUIRE(FirstText(runtime.BuildFrame()) == "base-next:3:7:10");

  requested_frames = platform.requested_frames;
  compound_operator_state -= 4;
  REQUIRE(platform.requested_frames == requested_frames + 1);
  REQUIRE(FirstText(runtime.BuildFrame()) == "base-next:3:7:6");
}

TEST_CASE("TestStateUpdateSupportsMoveOnlyValuesAndPostfixCopiesOnce") {
  move_only_operator_state = State<std::unique_ptr<int>>{};
  copy_counted_operator_state = State<CopyCountedStateValue>{};

  TestPlatform platform;
  Runtime runtime{MoveOnlyStateOperatorApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  REQUIRE(FirstText(runtime.BuildFrame()) == "5:8");

  int requested_frames = platform.requested_frames;
  move_only_operator_state.Update([](std::unique_ptr<int>& value) { *value = 13; });
  REQUIRE(platform.requested_frames == requested_frames + 1);
  REQUIRE(FirstText(runtime.BuildFrame()) == "13:8");

  CopyCountedStateValue::copies = 0;
  requested_frames = platform.requested_frames;
  const CopyCountedStateValue previous = copy_counted_operator_state++;
  REQUIRE(previous.value == 8);
  REQUIRE(CopyCountedStateValue::copies == 1);
  REQUIRE(platform.requested_frames == requested_frames + 1);
  REQUIRE(FirstText(runtime.BuildFrame()) == "13:9");
}

TEST_CASE("TestStateListMutatesInPlaceAndInvalidatesObservedScopes") {
  observed_state_list = {};
  state_list_compositions = 0;

  TestPlatform platform;
  Runtime runtime{StateListApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  REQUIRE(state_list_compositions == 1);
  REQUIRE(observed_state_list.Size() == 2);
  REQUIRE(observed_state_list[0] == "Alpha");
  REQUIRE(observed_state_list[1] == "Bravo");

  StateList<std::string> copy = observed_state_list;
  copy.PushBack("Charlie");
  copy.Insert(1, "Inserted");
  copy.Set(2, "Updated");
  copy.Move(3, 0);
  copy.Erase(2);
  copy.PopBack();

  REQUIRE(observed_state_list.Size() == 2);
  REQUIRE(observed_state_list[0] == "Charlie");
  REQUIRE(observed_state_list[1] == "Alpha");

  runtime.BuildFrame();
  REQUIRE(state_list_compositions == 2);
  const auto* root = runtime.RootNode();
  REQUIRE(root->children.size() == 2);
  REQUIRE(root->children[0]->text.PlainText() == "Charlie");
  REQUIRE(root->children[1]->text.PlainText() == "Alpha");

  copy.Set(0, "Charlie");
  copy.Move(0, 0);
  runtime.BuildFrame();
  REQUIRE(state_list_compositions == 2);

  copy.Clear();
  runtime.BuildFrame();
  REQUIRE(state_list_compositions == 3);
  REQUIRE(runtime.RootNode()->children.empty());
  REQUIRE(copy.Empty());

  copy.Clear();
  REQUIRE_THROWS_AS(copy.At(0), std::out_of_range);
  REQUIRE_THROWS_AS(copy.Insert(1, "invalid"), std::out_of_range);
  REQUIRE_THROWS_AS(copy.PopBack(), std::out_of_range);
}

TEST_CASE("TestStateListSupportsEmptyInitializationAndOnlyTracksReads") {
  empty_state_list = {};
  unobserved_state_list = {};
  unobserved_state_list_compositions = 0;

  TestPlatform platform;
  Runtime empty_runtime{EmptyStateListApp, platform};
  empty_runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
  REQUIRE(FirstText(empty_runtime.BuildFrame()) == "0");
  REQUIRE(empty_state_list.Empty());

  Runtime unobserved_runtime{UnobservedStateListApp, platform};
  unobserved_runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
  unobserved_runtime.BuildFrame();
  REQUIRE(unobserved_state_list_compositions == 1);
  REQUIRE(unobserved_state_list.Size() == 2);

  unobserved_state_list.PushBack(3);
  unobserved_runtime.BuildFrame();
  REQUIRE(unobserved_state_list_compositions == 1);

  StateList<int> invalid;
  REQUIRE_FALSE(invalid.IsValid());
  REQUIRE_THROWS_AS(invalid.Size(), std::logic_error);
  REQUIRE_THROWS_AS(invalid.PushBack(1), std::logic_error);
}

TEST_CASE("TestViewportClassRecomposesOnlyAcrossConfiguredBreakpoints") {
  viewport_compositions = 0;
  observed_viewport_class = ViewportClass::Compact;

  TestPlatform platform;
  Runtime runtime{
      ViewportClassApp,
      platform,
      {
          .viewport_breakpoints = ViewportBreakpoints{500.0F, 900.0F},
          .show_debug_overlay = false,
      },
  };

  runtime.SetWindowMetrics({.viewport = {320.0F, 600.0F}});
  runtime.BuildFrame();
  REQUIRE(viewport_compositions == 1);
  REQUIRE(observed_viewport_class == ViewportClass::Compact);

  runtime.SetWindowMetrics({.viewport = {480.0F, 720.0F}});
  runtime.BuildFrame();
  REQUIRE(viewport_compositions == 1);

  runtime.SetWindowMetrics({.viewport = {500.0F, 720.0F}});
  runtime.BuildFrame();
  REQUIRE(viewport_compositions == 2);
  REQUIRE(observed_viewport_class == ViewportClass::Medium);

  runtime.SetWindowMetrics({.viewport = {899.0F, 800.0F}});
  runtime.BuildFrame();
  REQUIRE(viewport_compositions == 2);

  runtime.SetWindowMetrics({.viewport = {900.0F, 800.0F}});
  runtime.BuildFrame();
  REQUIRE(viewport_compositions == 3);
  REQUIRE(observed_viewport_class == ViewportClass::Expanded);

  REQUIRE_THROWS_AS(
      Runtime(
          ViewportClassApp,
          platform,
          {
              .viewport_breakpoints = ViewportBreakpoints{600.0F, 600.0F},
              .show_debug_overlay = false,
          }
      ),
      std::invalid_argument
  );
}

TEST_CASE("TestRootCompositionRecoversAfterException") {
  root_composition_should_throw = false;

  TestPlatform platform;
  Runtime runtime{RecoveringRootApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const std::uint64_t identity = root->identity;

  root_composition_should_throw = true;
  root_recovery_state = 2;
  REQUIRE_THROWS_AS(runtime.BuildFrame(), std::runtime_error);
  REQUIRE(runtime.RootNode()->identity == identity);
  REQUIRE(runtime.RootNode()->text.PlainText() == "1");

  root_composition_should_throw = false;
  root_recovery_state = 3;
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->identity == identity);
  REQUIRE(runtime.RootNode()->text.PlainText() == "3");
}

TEST_CASE("TestChildReconciliationRecoversAfterException") {
  child_composition_should_throw = false;

  TestPlatform platform;
  Runtime runtime{RecoveringChildApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 2);
  const std::uint64_t label_identity = root->children[0]->identity;
  const std::uint64_t scope_identity = root->children[1]->identity;

  child_composition_should_throw = true;
  child_recovery_trigger = 1;
  REQUIRE_THROWS_AS(runtime.BuildFrame(), std::runtime_error);

  root = runtime.RootNode();
  REQUIRE(root->children.size() == 2);
  REQUIRE(root->children[0]->identity == label_identity);
  REQUIRE(root->children[1]->identity == scope_identity);
  REQUIRE(root->children[1]->children.size() == 1);
  REQUIRE(root->children[1]->children[0]->text.PlainText() == "0");

  child_composition_should_throw = false;
  child_recovery_state = 2;
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->children[0]->identity == label_identity);
  REQUIRE(root->children[0]->text.PlainText() == "1");
  REQUIRE(root->children[1]->identity == scope_identity);
  REQUIRE(root->children[1]->children[0]->text.PlainText() == "2");
}

TEST_CASE("TestModifierReconciliationPreservesExtensionsOnException") {
  extension_creations = 0;
  extension_updates = 0;
  extension_destroys = 0;

  TestPlatform platform;
  Runtime runtime{RecoveringModifierApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->extensions.size() == 1);
  NodeExtension* extension = root->extensions[0].extension.get();
  const std::uint64_t identity = root->identity;

  throwing_modifier_visible = true;
  REQUIRE_THROWS_AS(runtime.BuildFrame(), std::runtime_error);

  root = runtime.RootNode();
  REQUIRE(root->identity == identity);
  REQUIRE(root->extensions.size() == 1);
  REQUIRE(root->extensions[0].extension.get() == extension);
  REQUIRE(extension_destroys == 0);

  throwing_modifier_visible = false;
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->identity == identity);
  REQUIRE(root->extensions.size() == 1);
  REQUIRE(root->extensions[0].extension.get() == extension);
}

TEST_CASE("TestLayoutAndHitTest") {
  TestPlatform platform;
  Runtime runtime{CounterApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 2);
  REQUIRE(root->children[0]->layout_offset.y == 0.0F);
  REQUIRE(root->children[1]->layout_offset.y == 24.0F);
  REQUIRE(root->children[1]->children.size() == 1);
  REQUIRE(huxerui::detail::HasEventBinding<ViewEvents::Click>(root->children[1]->children[0]->event_bindings));
}

TEST_CASE("TestViewCopyOnWrite") {
  TestPlatform platform;
  Runtime runtime{CopyOnWriteApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 2);
  REQUIRE(root->children[0]->properties.text_style.foreground.red == huxerui::TextStyle::Default().foreground.red);
  REQUIRE(root->children[1]->properties.text_style.foreground.red == 1.0F);
}

TEST_CASE("ViewDeclarationsResolveInheritedStylesDuringReconciliation") {
  TestPlatform platform;
  Runtime runtime{ReconciliationThemeApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  const auto* environment = runtime.RootNode();
  REQUIRE(environment != nullptr);
  REQUIRE(environment->kind == detail::NodeKind::Environment);
  REQUIRE(environment->children.size() == 1);
  const auto* content = environment->children[0].get();
  REQUIRE(content->children.size() == 3);

  const auto& themed = *content->children[0];
  REQUIRE(themed.properties.background == Color::Rgb(20, 90, 170));
  REQUIRE(themed.properties.padding == EdgeInsets::All(9.0F));
  REQUIRE(themed.properties.text_style.font.FamilyKind() == FontFamilyKind::Monospace);
  REQUIRE(themed.properties.text_style.font.Size() == 19.0F);
  REQUIRE(themed.properties.text_style.decoration == TextDecoration::Underline);
  REQUIRE(themed.properties.frame.min_width == 72.0F);

  const auto& explicit_button = *content->children[1];
  REQUIRE(explicit_button.properties.background == Color::Rgb(20, 90, 170));
  REQUIRE(explicit_button.properties.padding == EdgeInsets::All(23.0F));
  REQUIRE(explicit_button.properties.text_style.font.FamilyKind() == FontFamilyKind::Monospace);
  REQUIRE(explicit_button.properties.text_style.font.Size() == 27.0F);
  REQUIRE(explicit_button.properties.text_style.foreground == Color::Rgb(190, 40, 30));
  REQUIRE(explicit_button.properties.text_style.decoration == TextDecoration::Underline);
  REQUIRE(explicit_button.properties.frame.width == 144.0F);
  REQUIRE(explicit_button.properties.frame.min_width == 72.0F);

  const auto& explicit_zero = *content->children[2];
  REQUIRE(explicit_zero.properties.frame.min_width == 0.0F);
}

TEST_CASE("EnvironmentBoundaryRetainsIdentityWithoutOwningARecomposeScope") {
  environment_boundary_should_throw = false;
  TestPlatform platform;
  Runtime runtime{EnvironmentBoundaryApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 120.0F}});
  runtime.BuildFrame();

  const auto* environment = runtime.RootNode();
  REQUIRE(environment != nullptr);
  REQUIRE(environment->kind == detail::NodeKind::Environment);
  REQUIRE(environment->recompose_scope == nullptr);
  REQUIRE(environment->owned_environment != nullptr);
  REQUIRE(environment->children.size() == 1);
  const auto* content = environment->children[0].get();
  REQUIRE(content->kind == detail::NodeKind::Scope);
  REQUIRE(content->children.size() == 1);
  REQUIRE(content->children[0]->text.PlainText() == "environment 1");

  const std::uint64_t environment_identity = environment->identity;
  const std::uint64_t content_identity = content->identity;
  const Environment* mounted_environment = environment->owned_environment.get();
  const std::uint64_t measure_revision = environment->measure_revision;

  environment_boundary_trigger = 1;
  runtime.BuildFrame();
  environment = runtime.RootNode();
  REQUIRE(environment->identity == environment_identity);
  REQUIRE(environment->children[0]->identity == content_identity);
  REQUIRE(environment->owned_environment.get() == mounted_environment);
  REQUIRE(environment->measure_revision == measure_revision);

  environment_boundary_value = 2;
  runtime.BuildFrame();
  environment = runtime.RootNode();
  REQUIRE(environment->identity == environment_identity);
  REQUIRE(environment->children[0]->identity == content_identity);
  REQUIRE(environment->owned_environment.get() == mounted_environment);
  REQUIRE(environment->children[0]->children[0]->text.PlainText() == "environment 2");
}

TEST_CASE("EnvironmentEntriesInvalidateOnlyTheirExactReaders") {
  viewport_environment_compositions = 0;
  locale_environment_compositions = 0;
  TestPlatform platform;
  Runtime runtime{PreciseEnvironmentDependencyApp, platform};
  runtime.SetWindowMetrics({.viewport = {500.0F, 120.0F}});
  runtime.BuildFrame();

  REQUIRE(viewport_environment_compositions == 1);
  REQUIRE(locale_environment_compositions == 1);

  runtime.SetWindowMetrics({.viewport = {700.0F, 120.0F}});
  runtime.BuildFrame();
  REQUIRE(viewport_environment_compositions == 2);
  REQUIRE(locale_environment_compositions == 1);

  runtime.UpdateResourceConfiguration({Locale::Default(), 2.0F});
  runtime.BuildFrame();
  REQUIRE(viewport_environment_compositions == 2);
  REQUIRE(locale_environment_compositions == 1);

  runtime.UpdateResourceConfiguration({Locale::FromLanguageTag("fr-FR"), 2.0F});
  runtime.BuildFrame();
  REQUIRE(viewport_environment_compositions == 2);
  REQUIRE(locale_environment_compositions == 2);
}

TEST_CASE("EnvironmentBoundaryRecoversAfterDescendantCompositionFailure") {
  environment_boundary_should_throw = false;

  TestPlatform platform;
  Runtime runtime{EnvironmentBoundaryApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 120.0F}});
  runtime.BuildFrame();

  const auto* environment = runtime.RootNode();
  REQUIRE(environment != nullptr);
  const Environment* mounted_environment = environment->owned_environment.get();

  environment_boundary_should_throw = true;
  environment_boundary_value = 2;
  REQUIRE_THROWS_AS(runtime.BuildFrame(), std::runtime_error);
  environment = runtime.RootNode();
  REQUIRE(environment->owned_environment.get() == mounted_environment);
  const std::any* committed =
      detail::FindEnvironmentValue(environment->environment, typeid(ParameterizedEnvironmentValue));
  REQUIRE(committed != nullptr);
  REQUIRE(std::any_cast<const ParameterizedEnvironmentValue&>(*committed).value == 2);
  REQUIRE(environment->children[0]->children[0]->text.PlainText() == "environment 1");

  environment_boundary_should_throw = false;
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->children[0]->children[0]->text.PlainText() == "environment 2");
}

TEST_CASE("OneViewDeclarationResolvesIndependentlyUnderDifferentThemes") {
  TestPlatform platform;
  Runtime runtime{SharedDeclarationThemeApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 120.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 2);
  REQUIRE(root->children[0]->kind == detail::NodeKind::Environment);
  REQUIRE(root->children[1]->kind == detail::NodeKind::Environment);
  REQUIRE(root->children[0]->children.size() == 1);
  REQUIRE(root->children[1]->children.size() == 1);
  REQUIRE(root->children[0]->children[0]->properties.background == Color::Rgb(180, 30, 40));
  REQUIRE(root->children[1]->children[0]->properties.background == Color::Rgb(20, 100, 180));
}

TEST_CASE("TextStyleSetsTheCompleteStyleBeforeModifiers") {
  TestPlatform platform;
  Runtime runtime{TextStyleApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->properties.text_style.font.FamilyKind() == FontFamilyKind::Monospace);
  REQUIRE(root->properties.text_style.font.Weight() == FontWeight::Bold);
  REQUIRE(root->properties.text_style.font.Size() == 22.0F);
  REQUIRE(root->properties.text_style.foreground == Color::Rgb(40, 50, 60));
  REQUIRE(root->properties.text_style.decoration == TextDecoration::Underline);
}

TEST_CASE("PropertyModifiersApplyOncePerMountedDeclarationResolution") {
  property_modifier_applications = 0;
  static_cast<void>(Text("Declaration").With(PropertyProbe{true}));
  static_cast<void>(Checkbox(false));
  REQUIRE(property_modifier_applications == 0);

  TestPlatform platform;
  Runtime runtime{PropertyModifierApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  REQUIRE(property_modifier_applications == 1);
  REQUIRE_FALSE(runtime.RootNode()->properties.clip_children);

  property_modifier_value = true;
  runtime.BuildFrame();

  REQUIRE(property_modifier_applications == 2);
  REQUIRE(runtime.RootNode()->properties.clip_children);
}

TEST_CASE("TestModifierReconciliationAndCopyOnWrite") {
  extension_creations = 0;
  extension_updates = 0;
  extension_destroys = 0;

  TestPlatform platform;
  {
    Runtime runtime{ModifierApp, platform};
    runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
    runtime.BuildFrame();

    const auto* root = runtime.RootNode();
    REQUIRE(root != nullptr);
    REQUIRE(root->properties.padding.left == 5.0F);
    REQUIRE(root->properties.background.has_value());
    REQUIRE(root->extensions.size() == 1);
    REQUIRE(root->extensions[0].extension != nullptr);
    REQUIRE(extension_creations == 1);
    REQUIRE(extension_updates == 0);
    const std::uint64_t identity = root->identity;

    runtime.InvalidateRoot();
    runtime.BuildFrame();

    root = runtime.RootNode();
    REQUIRE(root->identity == identity);
    REQUIRE(extension_creations == 1);
    REQUIRE(extension_updates == 0);

    modifier_style_changed = true;
    runtime.BuildFrame();

    root = runtime.RootNode();
    REQUIRE(root->identity == identity);
    REQUIRE(extension_creations == 1);
    REQUIRE(extension_updates == 1);

    modifier_value = 2;
    runtime.BuildFrame();

    root = runtime.RootNode();
    REQUIRE(root->identity == identity);
    REQUIRE(extension_creations == 1);
    REQUIRE(extension_updates == 2);
    REQUIRE(static_cast<ProbeModifierExtension*>(root->extensions[0].extension.get())->value == 2);
  }
  REQUIRE(extension_destroys == 1);

  Runtime copy_runtime{ModifierCopyOnWriteApp, platform};
  copy_runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  copy_runtime.BuildFrame();
  const auto* copy_root = copy_runtime.RootNode();
  REQUIRE(copy_root != nullptr);
  REQUIRE(copy_root->children[0]->properties.text_style.foreground.red == huxerui::TextStyle::Default().foreground.red);
  REQUIRE(copy_root->children[1]->properties.text_style.foreground.red == 1.0F);
}

TEST_CASE("TestNonComparableModifierUpdatesConservatively") {
  opaque_extension_updates = 0;

  TestPlatform platform;
  Runtime runtime{OpaqueModifierApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  REQUIRE(opaque_extension_updates == 0);

  runtime.InvalidateRoot();
  runtime.BuildFrame();
  REQUIRE(opaque_extension_updates == 1);
}

TEST_CASE("TestScopeStateIsolation") {
  TestPlatform platform;
  Runtime runtime{ScopedCountersApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 2);
  REQUIRE(root->children[0]->children[0]->children[0]->text.PlainText() == "0");
  REQUIRE(root->children[1]->children[0]->children[0]->text.PlainText() == "0");

  InvokeClick(*root->children[0]->children[0]->children[1]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->children[0]->children[0]->children[0]->text.PlainText() == "1");
  REQUIRE(root->children[1]->children[0]->children[0]->text.PlainText() == "0");
}

TEST_CASE("TestStatePassedIntoScope") {
  TestPlatform platform;
  Runtime runtime{SharedStateApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children[0]->children[0]->text.PlainText() == "7");

  InvokeClick(*root->children[1]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->children[0]->children[0]->text.PlainText() == "8");
}

TEST_CASE("TestKeyedScopeIdentity") {
  TestPlatform platform;
  Runtime runtime{KeyedScopesApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 320.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const std::uint64_t first_scope_identity = root->children[0]->identity;

  InvokeClick(*root->children[0]->children[0]->children[1]);
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->children[0]->children[0]->children[0]->text.PlainText() == "1");

  InvokeClick(*root->children[2]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->children[1]->identity == first_scope_identity);
  REQUIRE(root->children[1]->children[0]->children[0]->text.PlainText() == "1");
  REQUIRE(root->children[0]->children[0]->children[0]->text.PlainText() == "0");
}

TEST_CASE("TestDuplicateSiblingKeys") {
  TestPlatform platform;
  Runtime runtime{DuplicateKeyApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  bool rejected = false;
  try {
    runtime.BuildFrame();
  } catch (const std::logic_error&) {
    rejected = true;
  }
  REQUIRE(rejected);
}

TEST_CASE("TestRepeatedUseStateCallSite") {
  TestPlatform platform;
  Runtime runtime{RepeatedUseStateApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 3);
  REQUIRE(root->children[0]->text.PlainText() == "0");
  REQUIRE(root->children[1]->text.PlainText() == "0");
  REQUIRE(root->children[2]->text.PlainText() == "0");
  
  InvokeClick(*root->children[1]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->children[0]->text.PlainText() == "0");
  REQUIRE(root->children[1]->text.PlainText() == "1");
  REQUIRE(root->children[2]->text.PlainText() == "0");
}

TEST_CASE("TestLocalScopeRecomposition") {
  local_root_compositions = 0;
  left_scope_compositions = 0;
  right_scope_compositions = 0;

  TestPlatform platform;
  Runtime runtime{LocalRecompositionApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 320.0F}});
  runtime.BuildFrame();

  REQUIRE(local_root_compositions == 1);
  REQUIRE(left_scope_compositions == 1);
  REQUIRE(right_scope_compositions == 1);

  const auto* root = runtime.RootNode();
  const int requested_frames = platform.requested_frames;
  InvokeClick(*root->children[0]->children[0]->children[1]);
  InvokeClick(*root->children[0]->children[0]->children[1]);

  REQUIRE(platform.requested_frames == requested_frames + 1);
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(local_root_compositions == 1);
  REQUIRE(left_scope_compositions == 2);
  REQUIRE(right_scope_compositions == 1);
  REQUIRE(root->children[0]->children[0]->children[0]->text.PlainText() == "2");
  REQUIRE(root->children[1]->children[0]->children[0]->text.PlainText() == "0");
}

TEST_CASE("TestScopeReceivesUpdatedProps") {
  prop_root_compositions = 0;
  prop_scope_compositions = 0;

  TestPlatform platform;
  Runtime runtime{PropUpdateApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root->children[0]->children[0]->text.PlainText() == "3");
  REQUIRE(prop_root_compositions == 1);
  REQUIRE(prop_scope_compositions == 1);

  InvokeClick(*root->children[1]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->children[0]->children[0]->text.PlainText() == "4");
  REQUIRE(prop_root_compositions == 2);
  REQUIRE(prop_scope_compositions == 2);
}

TEST_CASE("TestTypedScopeEvents") {
  received_event.clear();
  saved_event_emitter = {};

  TestPlatform platform;
  Runtime runtime{EventApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const auto* source = root->children[0].get();
  const std::uint64_t source_identity = source->identity;
  const std::uint64_t scope_id = source->recompose_scope->Id();
  InvokeClick(*source->children[0]);
  REQUIRE(received_event == "first:query");
  REQUIRE(saved_event_emitter.IsConnected());
  REQUIRE(saved_event_emitter.Emit<SearchAccepted>("query") == std::optional{true});
  REQUIRE_THROWS_AS(saved_event_emitter.Emit<SearchAccepted>("throw"), std::runtime_error);

  event_mode = 1;
  runtime.BuildFrame();
  root = runtime.RootNode();
  source = root->children[0].get();
  REQUIRE(source->identity == source_identity);
  REQUIRE(source->recompose_scope->Id() == scope_id);
  InvokeClick(*source->children[0]);
  REQUIRE(received_event == "second:query");
  REQUIRE(saved_event_emitter.Emit<SearchAccepted>("query") == std::optional{false});

  event_mode = 2;
  runtime.BuildFrame();
  REQUIRE(!saved_event_emitter.IsConnected());
  saved_event_emitter.Emit<SearchSubmitted>("ignored");
  REQUIRE_FALSE(saved_event_emitter.Emit<SearchAccepted>("query").has_value());
  REQUIRE(received_event == "second:query");
}

TEST_CASE("NodeExtensionsEmitThroughCurrentOwnerBindings") {
  NodeEventProbe::Extension disconnected;
  disconnected.EmitEvent<SearchSubmitted>("ignored");
  REQUIRE_FALSE(disconnected.EmitEvent<SearchAccepted>("accept").has_value());

  static int mode;
  static std::string received;
  static std::weak_ptr<int> handler_lifetime;
  mode = 0;
  received.clear();
  handler_lifetime.reset();
  TestPlatform platform;
  Runtime runtime{[]() -> View {
    View view = Text("source").With(NodeEventProbe{});
    if (mode == 2) {
      return view;
    }
    const auto token = std::make_shared<int>(mode);
    handler_lifetime = token;
    return std::move(view)
        .On<SearchSubmitted>([token](std::string value) { received = std::to_string(*token) + ":" + value; })
        .On<SearchAccepted>([](std::string_view value) {
          if (value == "throw") {
            throw std::runtime_error("event decision failed");
          }
          return value == "accept";
        })
        .On<NodeEventTransfer>([](std::unique_ptr<int> value) { return value; });
  }, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  const auto* extension = static_cast<const NodeEventProbe::Extension*>(
      runtime.RootNode()->extensions[0].extension.get());
  extension->EmitEvent<SearchSubmitted>("first");
  REQUIRE(received == "0:first");
  REQUIRE(extension->EmitEvent<SearchAccepted>("accept") == std::optional{true});
  REQUIRE(extension->EmitEvent<SearchAccepted>("reject") == std::optional{false});
  REQUIRE_THROWS_AS(extension->EmitEvent<SearchAccepted>("throw"), std::runtime_error);
  auto value = std::make_unique<int>(42);
  auto transferred = extension->EmitEvent<NodeEventTransfer>(std::move(value));
  REQUIRE(value == nullptr);
  REQUIRE(transferred.has_value());
  REQUIRE(**transferred == 42);

  const std::weak_ptr<int> previous_handler = handler_lifetime;
  mode = 1;
  runtime.InvalidateRoot();
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->extensions[0].extension.get() == extension);
  REQUIRE(previous_handler.expired());
  extension->EmitEvent<SearchSubmitted>("latest");
  REQUIRE(received == "1:latest");

  mode = 2;
  runtime.InvalidateRoot();
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->extensions[0].extension.get() == extension);
  REQUIRE(handler_lifetime.expired());
  extension->EmitEvent<SearchSubmitted>("ignored");
  REQUIRE(received == "1:latest");
  REQUIRE_FALSE(extension->EmitEvent<SearchAccepted>("accept").has_value());
}

TEST_CASE("NodeExtensionEventsStayWithKeyedNodesAndRebindAfterReplacement") {
  static int mode;
  static int destructions;
  static std::string received;
  mode = 0;
  destructions = 0;
  received.clear();
  TestPlatform platform;
  {
    Runtime runtime{[]() -> View {
      const auto item = [](int key) -> View {
        View view = Text("item").Key(key).On<SearchSubmitted>([key, revision = mode](std::string) {
          received = std::to_string(key) + ":" + std::to_string(revision);
        });
        if (mode != 3) {
          view = std::move(view).With(NodeEventProbe{&destructions});
        }
        return view;
      };
      return mode == 0 ? Column {item(1), item(2)} : Column {item(2), item(mode == 1 ? 1 : 3)};
    }, platform};
    runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
    runtime.BuildFrame();
    const auto* first = static_cast<const NodeEventProbe::Extension*>(
        runtime.RootNode()->children[0]->extensions[0].extension.get());
    const auto* second = static_cast<const NodeEventProbe::Extension*>(
        runtime.RootNode()->children[1]->extensions[0].extension.get());

    mode = 1;
    runtime.InvalidateRoot();
    runtime.BuildFrame();
    REQUIRE(runtime.RootNode()->children[1]->extensions[0].extension.get() == first);
    REQUIRE(runtime.RootNode()->children[0]->extensions[0].extension.get() == second);
    first->EmitEvent<SearchSubmitted>("");
    REQUIRE(received == "1:1");
    second->EmitEvent<SearchSubmitted>("");
    REQUIRE(received == "2:1");
    REQUIRE(destructions == 0);

    mode = 2;
    runtime.InvalidateRoot();
    runtime.BuildFrame();
    REQUIRE(destructions == 1);
    const auto* replacement = static_cast<const NodeEventProbe::Extension*>(
        runtime.RootNode()->children[1]->extensions[0].extension.get());
    replacement->EmitEvent<SearchSubmitted>("");
    REQUIRE(received == "3:2");

    mode = 3;
    runtime.InvalidateRoot();
    runtime.BuildFrame();
    REQUIRE(destructions == 3);
    REQUIRE(runtime.RootNode()->children[0]->extensions.empty());
    REQUIRE(runtime.RootNode()->children[1]->extensions.empty());

    mode = 4;
    runtime.InvalidateRoot();
    runtime.BuildFrame();
    const auto* reattached = static_cast<const NodeEventProbe::Extension*>(
        runtime.RootNode()->children[0]->extensions[0].extension.get());
    reattached->EmitEvent<SearchSubmitted>("");
    REQUIRE(received == "2:4");
  }
  REQUIRE(destructions == 5);
}

TEST_CASE("NodeExtensionEventsUseInputAndSemanticCallbacksWithoutBubbling") {
  static int received;
  static int parent_received;
  static bool enabled;
  static bool bind_handler;
  received = 0;
  parent_received = 0;
  enabled = true;
  bind_handler = true;
  TestPlatform platform;
  Runtime runtime{[]() -> View {
    View target = Text("source").With(Frame{100.0F, 40.0F}, Enabled{enabled}, NodeEventProbe{});
    if (bind_handler) {
      target = std::move(target).On<SearchSubmitted>([](std::string) { ++received; });
    }
    return Column {std::move(target)}.On<SearchSubmitted>([](std::string) { ++parent_received; });
  }, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();
  runtime.HandlePointerEvent({PointerEventType::Down, 1, {10.0F, 10.0F}});
  runtime.HandlePointerEvent({PointerEventType::Cancel, 1, {10.0F, 10.0F}});
  REQUIRE(received == 0);
  runtime.HandlePointerEvent({PointerEventType::Down, 2, {10.0F, 10.0F}});
  runtime.HandlePointerEvent({PointerEventType::Up, 2, {10.0F, 10.0F}});
  REQUIRE(received == 1);
  const SemanticNodeId target = runtime.RootNode()->children[0]->semantic_identity;
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(target, {.kind = SemanticActionKind::Activate}));
  REQUIRE(received == 2);
  REQUIRE(parent_received == 0);

  bind_handler = false;
  runtime.InvalidateRoot();
  runtime.BuildFrame();
  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(target, {.kind = SemanticActionKind::Activate}));
  REQUIRE(received == 2);
  REQUIRE(parent_received == 0);

  enabled = false;
  bind_handler = true;
  runtime.InvalidateRoot();
  runtime.BuildFrame();
  runtime.HandlePointerEvent({PointerEventType::Down, 3, {10.0F, 10.0F}});
  runtime.HandlePointerEvent({PointerEventType::Up, 3, {10.0F, 10.0F}});
  REQUIRE_FALSE(runtime.CoreRuntime().PerformSemanticAction(target, {.kind = SemanticActionKind::Activate}));
  REQUIRE(received == 2);
  REQUIRE(parent_received == 0);
}

TEST_CASE("TestRuntimeProvidesPlatformTextMeasurer") {
  observed_text_measurer = nullptr;
  TestPlatform platform;
  Runtime runtime{TextMeasurerApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 40.0F}});
  runtime.BuildFrame();

  REQUIRE(observed_text_measurer == &platform);
}

TEST_CASE("ViewFactoriesBindTypedArgumentsAcrossCompositionWrappers") {
  TestPlatform platform;
  Runtime runtime{ParameterizedFactoryApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});

  REQUIRE(ContainsText(runtime.BuildFrame(), "bound 42"));
}

} // namespace huxerui::test
