#include "runtime_test_support.h"

namespace huxerui::test {

struct SearchSubmitted : Event<std::string> {};

State<int> event_mode;

EventEmitter saved_event_emitter;
std::string received_event;

State<int> modifier_value;
int modifier_mounts = 0;
int modifier_updates = 0;
int modifier_destroys = 0;

struct ProbeModifier;

class MountedProbeModifier final : public MountedModifier {
public:
  MountedProbeModifier(MountedNode &node, const ProbeModifier &modifier);
  ~MountedProbeModifier() override {
    ++modifier_destroys;
  }

  void Update(MountedNode &node, const ProbeModifier &modifier);

  int value = 0;
};

struct ProbeModifier {
  using Mounted = MountedProbeModifier;

  int value;
};

MountedProbeModifier::MountedProbeModifier(MountedNode &node, const ProbeModifier &modifier) : value(modifier.value) {
  static_cast<void>(node);
  ++modifier_mounts;
}

void MountedProbeModifier::Update(MountedNode &node, const ProbeModifier &modifier) {
  static_cast<void>(node);
  value = modifier.value;
  ++modifier_updates;
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
        EventSource().Key("source").On<SearchSubmitted>(
            [](std::string value) { received_event = "second:" + value; }),
    };
  }

  return Column{
      EventSource()
          .Key("source")
          .On<SearchSubmitted>([](std::string value) { received_event = "replaced:" + value; })
          .On<SearchSubmitted>([](std::string value) { received_event = "first:" + value; }),
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

View CopyOnWriteApp() {
  View original = Text("Shared");
  View modified = View(original).With(huxerui::Foreground{huxerui::Color::White()});
  return Column{
      original,
      modified,
  };
}

View ModifierApp() {
  auto value = UseState(1);
  modifier_value = value;
  return Text("Modifier")
      .With(huxerui::Padding{5.0F}, huxerui::Background{huxerui::Color::White()}, ProbeModifier{value.Get()});
}

View ModifierCopyOnWriteApp() {
  View original = Text("Shared");
  View modified = View(original).With(huxerui::Foreground{huxerui::Color::White()});
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
    children.emplace_back(Button(std::to_string(value.Get())).OnClick([value] { value += 1; }));
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

TEST_CASE("TestUseStateAndStateUpdate") {
  TestPlatform platform;
  Runtime runtime{CounterApp, platform};
  runtime.SetViewport({320.0F, 240.0F});

  const DisplayList &initial = runtime.BuildFrame();
  REQUIRE(FirstText(initial) == "1");

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const std::uint64_t root_identity = root->identity;

  runtime.InvalidateRoot();
  const DisplayList &recomposed = runtime.BuildFrame();
  REQUIRE(FirstText(recomposed) == "1");
  REQUIRE(runtime.RootNode()->identity == root_identity);

  ClickAt(runtime, {10.0F, 42.0F});
  REQUIRE(platform.requested_frames > 0);

  const DisplayList &updated = runtime.BuildFrame();
  REQUIRE(FirstText(updated) == "2");
  REQUIRE(runtime.RootNode()->identity == root_identity);
}

TEST_CASE("TestLayoutAndHitTest") {
  TestPlatform platform;
  Runtime runtime{CounterApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 2);
  REQUIRE(root->children[0]->frame.y == 0.0F);
  REQUIRE(root->children[1]->frame.y == 24.0F);
  REQUIRE(root->children[1]->children.size() == 1);
  REQUIRE(huxerui::detail::HasEventBinding<ViewEvents::Click>(root->children[1]->children[0]->event_bindings));
}

TEST_CASE("TestViewCopyOnWrite") {
  TestPlatform platform;
  Runtime runtime{CopyOnWriteApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 2);
  REQUIRE(root->children[0]->style.foreground.has_value());
  REQUIRE(root->children[0]->style.foreground->red == huxerui::TextStyleKey::Default().foreground.red);
  REQUIRE(root->children[1]->style.foreground.has_value());
  REQUIRE(root->children[1]->style.foreground->red == 1.0F);
}

TEST_CASE("TestModifierReconciliationAndCopyOnWrite") {
  modifier_mounts = 0;
  modifier_updates = 0;
  modifier_destroys = 0;

  TestPlatform platform;
  {
    Runtime runtime{ModifierApp, platform};
    runtime.SetViewport({320.0F, 240.0F});
    runtime.BuildFrame();

    const auto *root = runtime.RootNode();
    REQUIRE(root != nullptr);
    REQUIRE(root->style.padding.left == 5.0F);
    REQUIRE(root->style.background.has_value());
    REQUIRE(root->modifiers.size() == 3);
    REQUIRE(root->modifiers[2].mounted != nullptr);
    REQUIRE(modifier_mounts == 1);
    REQUIRE(modifier_updates == 0);
    const std::uint64_t identity = root->identity;

    modifier_value = 2;
    runtime.BuildFrame();

    root = runtime.RootNode();
    REQUIRE(root->identity == identity);
    REQUIRE(modifier_mounts == 1);
    REQUIRE(modifier_updates == 1);
    REQUIRE(static_cast<MountedProbeModifier *>(root->modifiers[2].mounted.get())->value == 2);
  }
  REQUIRE(modifier_destroys == 1);

  Runtime copy_runtime{ModifierCopyOnWriteApp, platform};
  copy_runtime.SetViewport({320.0F, 240.0F});
  copy_runtime.BuildFrame();
  const auto *copy_root = copy_runtime.RootNode();
  REQUIRE(copy_root != nullptr);
  REQUIRE(copy_root->children[0]->style.foreground.has_value());
  REQUIRE(copy_root->children[0]->style.foreground->red == huxerui::TextStyleKey::Default().foreground.red);
  REQUIRE(copy_root->children[1]->style.foreground.has_value());
}

TEST_CASE("TestScopeStateIsolation") {
  TestPlatform platform;
  Runtime runtime{ScopedCountersApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 2);
  REQUIRE(root->children[0]->children[0]->children[0]->text == "0");
  REQUIRE(root->children[1]->children[0]->children[0]->text == "0");

  InvokeClick(*root->children[0]->children[0]->children[1]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->children[0]->children[0]->children[0]->text == "1");
  REQUIRE(root->children[1]->children[0]->children[0]->text == "0");
}

TEST_CASE("TestStatePassedIntoScope") {
  TestPlatform platform;
  Runtime runtime{SharedStateApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children[0]->children[0]->text == "7");

  InvokeClick(*root->children[1]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->children[0]->children[0]->text == "8");
}

TEST_CASE("TestKeyedScopeIdentity") {
  TestPlatform platform;
  Runtime runtime{KeyedScopesApp, platform};
  runtime.SetViewport({320.0F, 320.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const std::uint64_t first_scope_identity = root->children[0]->identity;

  InvokeClick(*root->children[0]->children[0]->children[1]);
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->children[0]->children[0]->children[0]->text == "1");

  InvokeClick(*root->children[2]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->children[1]->identity == first_scope_identity);
  REQUIRE(root->children[1]->children[0]->children[0]->text == "1");
  REQUIRE(root->children[0]->children[0]->children[0]->text == "0");
}

TEST_CASE("TestDuplicateSiblingKeys") {
  TestPlatform platform;
  Runtime runtime{DuplicateKeyApp, platform};
  runtime.SetViewport({320.0F, 240.0F});

  bool rejected = false;
  try {
    runtime.BuildFrame();
  } catch (const std::logic_error &) {
    rejected = true;
  }
  REQUIRE(rejected);
}

TEST_CASE("TestRepeatedUseStateCallSite") {
  TestPlatform platform;
  Runtime runtime{RepeatedUseStateApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 3);
  REQUIRE(root->children[0]->text == "0");
  REQUIRE(root->children[1]->text == "0");
  REQUIRE(root->children[2]->text == "0");

  InvokeClick(*root->children[1]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->children[0]->text == "0");
  REQUIRE(root->children[1]->text == "1");
  REQUIRE(root->children[2]->text == "0");
}

TEST_CASE("TestLocalScopeRecomposition") {
  local_root_compositions = 0;
  left_scope_compositions = 0;
  right_scope_compositions = 0;

  TestPlatform platform;
  Runtime runtime{LocalRecompositionApp, platform};
  runtime.SetViewport({320.0F, 320.0F});
  runtime.BuildFrame();

  REQUIRE(local_root_compositions == 1);
  REQUIRE(left_scope_compositions == 1);
  REQUIRE(right_scope_compositions == 1);

  const auto *root = runtime.RootNode();
  const int requested_frames = platform.requested_frames;
  InvokeClick(*root->children[0]->children[0]->children[1]);
  InvokeClick(*root->children[0]->children[0]->children[1]);

  REQUIRE(platform.requested_frames == requested_frames + 1);
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(local_root_compositions == 1);
  REQUIRE(left_scope_compositions == 2);
  REQUIRE(right_scope_compositions == 1);
  REQUIRE(root->children[0]->children[0]->children[0]->text == "2");
  REQUIRE(root->children[1]->children[0]->children[0]->text == "0");
}

TEST_CASE("TestScopeReceivesUpdatedProps") {
  prop_root_compositions = 0;
  prop_scope_compositions = 0;

  TestPlatform platform;
  Runtime runtime{PropUpdateApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root->children[0]->children[0]->text == "3");
  REQUIRE(prop_root_compositions == 1);
  REQUIRE(prop_scope_compositions == 1);

  InvokeClick(*root->children[1]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->children[0]->children[0]->text == "4");
  REQUIRE(prop_root_compositions == 2);
  REQUIRE(prop_scope_compositions == 2);
}

TEST_CASE("TestTypedScopeEvents") {
  received_event.clear();
  saved_event_emitter = {};

  TestPlatform platform;
  Runtime runtime{EventApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();

  const auto *root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const auto *source = root->children[0].get();
  const std::uint64_t source_identity = source->identity;
  const std::uint64_t scope_id = source->recompose_scope->Id();
  InvokeClick(*source->children[0]);
  REQUIRE(received_event == "first:query");
  REQUIRE(saved_event_emitter.IsConnected());

  event_mode = 1;
  runtime.BuildFrame();
  root = runtime.RootNode();
  source = root->children[0].get();
  REQUIRE(source->identity == source_identity);
  REQUIRE(source->recompose_scope->Id() == scope_id);
  InvokeClick(*source->children[0]);
  REQUIRE(received_event == "second:query");

  event_mode = 2;
  runtime.BuildFrame();
  REQUIRE(!saved_event_emitter.IsConnected());
  saved_event_emitter.Emit<SearchSubmitted>("ignored");
  REQUIRE(received_event == "second:query");
}

} // namespace huxerui::test
