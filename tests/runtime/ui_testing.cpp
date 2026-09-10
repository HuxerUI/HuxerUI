#include <catch2/catch_amalgamated.hpp>

#include <chrono>
#include <algorithm>
#include <memory>
#include <optional>
#include <thread>

#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>
#include <ui_test_fixture_resources.h>

#include "application/platform_registry_internal.h"
#include "external_texture_test_support.h"
#include "image_test_support.h"

namespace huxerui::test {
namespace {

using namespace huxerui::testing;
using namespace std::chrono_literals;

State<int> count;
State<TextEditingValue> editing;
State<float> slider_value;
State<bool> animated;
int shutdown_frames = 0;

struct FrameProbe {
  int* frames;

  struct Extension final : NodeExtension {
    Extension(ViewNode&, const FrameProbe& value) : frames(value.frames) {}
    void Update(ViewNode&, const FrameProbe& value) { frames = value.frames; }
    FrameResult OnFrame(ViewNode&, const FrameInfo&) override {
      ++*frames;
      return {.needs_frame = true};
    }
    int* frames;
  };
};

View Adjustable() {
  slider_value = UseState(0.0F);
  return Slider(slider_value.Get()).OnChanged([value = slider_value](float next) mutable { value = next; })
      .With(Frame{200.0F, 48.0F});
}

View Animated() {
  animated = UseState(false);
  return Stack {}.With(
      Frame{100.0F, 50.0F},
      Transition{AnimateTo(animated.Get() ? 1.0F : 0.0F, TweenSpec{1.0, Easing::Linear})}
          .Offset({}, {100.0F, 0.0F})
  ).Key("animated");
}

View Counter() {
  count = UseState(0);
  return Column {
    Text(std::to_string(count.Get())).Key("count"),
    Button("Increment").OnClick([value = count]() mutable { value = value.Get() + 1; }).Key("increment"),
    Text("Hidden semantics").With(Semantics{.label = "Accessible label"}).Key("label"),
  };
}

View Editor() {
  editing = UseState(TextEditingValue::FromText(""));
  return Column {
    TextField(editing.Get()).OnChanged([value = editing](const TextEditingValue& next) mutable { value = next; })
        .Key("editor"),
    Text(editing.Get().text).Key("echo"),
  };
}

View Scopes() {
  return Column {
    Row {Text("Delete").Key("action")}.Key("first"),
    Row {Text("Delete").Key("action")}.Key("second"),
    Text("signed").Key(std::int64_t{42}),
    Text("unsigned").Key(std::uint64_t{42}),
  };
}

View Disabled() {
  count = UseState(0);
  return Button("Disabled").OnClick([value = count]() mutable { value = value.Get() + 1; }).With(Enabled(false));
}

View SecureEditor() {
  editing = UseState(TextEditingValue::FromText("confidential-value"));
  return TextField(editing.Get()).Secure().OnChanged([value = editing](const TextEditingValue& next) mutable {
    value = next;
  });
}

View DelayedCounter() {
  count = UseState(0);
  auto tasks = UseTaskScope();
  return Button(std::to_string(count.Get())).OnClick([tasks, value = count] {
    tasks.Launch([value]() mutable -> Task<void> {
      co_await Delay(100ms);
      value = 1;
    });
  });
}

View Empty() { return Column {}.Key("empty"); }

View ScrollableItems() {
  count = UseState(0);
  return VirtualList(100, [](std::size_t index) {
    return Button("Item " + std::to_string(index))
        .OnClick([value = count]() mutable { value += 1; })
        .With(Frame{200.0F, 40.0F})
        .Key(index);
  }).EstimatedItemExtent(40.0F).Key("list");
}

View SnapshotItems() {
  count = UseState(0);
  std::vector<View> children;
  if (count.Get()) children.push_back(Text("Inserted").With(Semantics{.identifier = "inserted"}).Key("inserted"));
  children.push_back(Text("Stable").With(Semantics{.identifier = "stable"}).Key("stable"));
  return Stack(std::move(children)).With(Frame{200.0F, 100.0F});
}

View FailingRoot() { throw std::runtime_error("initial composition failed"); }

struct ShutdownProbe {
  detail::PlatformChannelEndpoint endpoint;
  detail::PlatformChannelEndpoint late_endpoint;
  int cancellations = 0;
  int disposals = 0;
  int followups = 0;
  std::thread::id disposal_thread;
};

struct ChannelOwner {
  detail::PlatformChannelEndpoint endpoint;
  ~ChannelOwner() { endpoint.Close(); }
};

void InstallShutdownProbe(RootContext& root, ShutdownProbe& probe) {
  root.RegisterPlatformModule<std::shared_ptr<ChannelOwner>>("testing/Shutdown", [&probe](PlatformAdapter& adapter) {
    probe.endpoint = detail::MakePlatformChannelEndpoint(adapter);
    probe.endpoint.Connect({
      .invoke = [&probe](std::string, PlatformPayload, std::function<void(PlatformResult<PlatformPayload>)>) {
        return [&probe] { ++probe.cancellations; };
      },
      .dispose = [&probe, &adapter] {
        ++probe.disposals;
        probe.disposal_thread = std::this_thread::get_id();
        adapter.DispatchToUIThread([] { throw std::runtime_error("cleanup failed"); });
        adapter.DispatchToUIThread([&probe] { ++probe.followups; });
      },
    });
    probe.late_endpoint = detail::MakePlatformChannelEndpoint(adapter);
    auto owner = std::make_shared<ChannelOwner>();
    owner->endpoint = probe.endpoint;
    return owner;
  });
  root.Provide(root.OpenPlatformModule<std::shared_ptr<ChannelOwner>>("testing/Shutdown"));
}

View UnicodeText() {
  return Column {
    Text("A😀B").Key("unicode"),
    Text("ABC").Key("ascii"),
  };
}

class FixtureResources final : public PlatformResources {
public:
  ResourceConfiguration Configuration() const override { return {}; }
  std::optional<InputStream> OpenRead(std::string_view path) override {
    ++opens;
    auto result = File(std::string(HUXERUI_UI_TEST_PACKAGE) + "/" + std::string(path)).OpenRead();
    if (!result.Succeeded()) return std::nullopt;
    return std::move(result).Value();
  }
  int opens = 0;
};

View Resources() {
  auto raw = UseRawResource(ui_test_fixture::raw::library_value_txt);
  return Column {
    Text(raw.ReadString()),
    Checkbox("Built-in check mark", true),
    Text(std::string(UseEnvironment<Locale>().LanguageTag())).Key("locale"),
  };
}

} // namespace

TEST_CASE("Windowless queries use mounted content independently of semantics", "[ui-testing]") {
  Application application(Counter, {.show_debug_overlay = false});
  UiTest ui(application);
  auto button = ui.Find(UiSelector::AllOf(UiSelector::Type<Button>(), UiSelector::Text("Increment")));
  REQUIRE(button.Count() == 1);
  REQUIRE(ui.Find(UiSelector::Text("Hidden semantics")).Exists());
  REQUIRE_FALSE(ui.Find(UiSelector::Text("Accessible label")).Exists());
  REQUIRE(ui.FindSemantics(UiSemanticSelector::Label("Accessible label")).Exists());
  button.Tap();
  REQUIRE(count.Get() == 1);
  REQUIRE(ui.Find(UiSelector::Text("1")).Exists());
  auto old = ui.Find(UiSelector::Key("count")).One();
  count = 2;
  REQUIRE(ui.Find(UiSelector::Text("1")).Exists());
  ui.Pump();
  REQUIRE(ui.Find(UiSelector::Text("2")).Exists());
  REQUIRE(old.text == "1");
}

TEST_CASE("Windowless scopes preserve key alternatives and diagnose ambiguity", "[ui-testing]") {
  Application application(Scopes, {.show_debug_overlay = false});
  UiTest ui(application);
  REQUIRE(ui.Find(UiSelector::Key("action")).Count() == 2);
  REQUIRE_THROWS_AS(ui.Find(UiSelector::Key("action")).One(), UiTestFailure);
  REQUIRE(ui.Find(UiSelector::Key("first")).Find(UiSelector::Text("Delete")).Count() == 1);
  REQUIRE_THROWS_AS(ui.Find(UiSelector::Key("missing")).Find(UiSelector::Text("Delete")).Exists(), UiTestFailure);
  REQUIRE(ui.Find(UiSelector::Key(std::int64_t{42})).One().text == "signed");
  REQUIRE(ui.Find(UiSelector::Key(std::uint64_t{42})).One().text == "unsigned");
  REQUIRE_FALSE(ui.Find(UiSelector::Text("")).Exists());
  REQUIRE_THROWS_AS(UiSelector::AllOf({}), std::invalid_argument);
}

TEST_CASE("Windowless editing follows controlled text input and UTF-16 selection", "[ui-testing]") {
  Application application(Editor, {.show_debug_overlay = false});
  UiTest ui(application);
  auto editor = ui.Find(UiSelector::Type<TextField>());
  REQUIRE(editor.One().value == "");
  REQUIRE_THROWS_AS(editor.EnterText("unfocused"), UiTestFailure);
  editor.Tap();
  editor.EnterText("A😀B");
  REQUIRE(editing.Get().text == "A😀B");
  REQUIRE(editor.One().value == "A😀B");
  editor.SetSelection({1, 3});
  editor.EnterText("X");
  REQUIRE(editing.Get().text == "AXB");
  editor.ReplaceText("replacement");
  REQUIRE(editing.Get().text == "replacement");
  REQUIRE(ui.SendTextInput({}).result_code != TextInputResultCode::Ok);
}

TEST_CASE("Windowless frame time is explicit and bounded", "[ui-testing]") {
  Application application(Counter, {.show_debug_overlay = false});
  UiTest ui(application);
  ui.Pump();
  REQUIRE(ui.Now() == 0);
  ui.Pump(100ms);
  REQUIRE(ui.Now() == 0.1);
  REQUIRE_THROWS_AS(ui.Pump(-1ms), std::invalid_argument);
  REQUIRE_THROWS_AS(ui.PumpUntil([] { return false; }, {.timeout = 10ms, .step = 5ms}), UiTestFailure);
  REQUIRE_THROWS_AS(ui.PumpUntil([&] { ui.Pump(); return true; }), std::logic_error);
  ui.Pump();
}

TEST_CASE("Windowless queries expire but information and captures own their data", "[ui-testing]") {
  Application application(Counter, {.show_debug_overlay = false});
  std::optional<UiNodeQuery> query;
  std::optional<UiSnapshot> snapshot;
  std::string exported;
  UiNodeInfo info;
  {
    UiTest ui(application);
    query = ui.Find(UiSelector::Key("count"));
    info = query->One();
    snapshot = ui.CaptureSnapshot();
    STATIC_REQUIRE(std::same_as<decltype(snapshot->ToString()), std::string>);
    exported = ui.CaptureSnapshot().ToString();
    const auto before = snapshot->ToString();
    ui.Find(UiSelector::Text("Increment")).Tap();
    REQUIRE(ui.CaptureSnapshot().ToString() != before);
    REQUIRE(snapshot->ToString() == before);
  }
  REQUIRE_THROWS_AS(query->Exists(), UiTestFailure);
  REQUIRE(info.text == "0");
  REQUIRE(snapshot->ToString().starts_with("huxerui-ui-snapshot 2"));
  REQUIRE(exported == snapshot->ToString());
}

TEST_CASE("Windowless input helpers reject disabled controls without activating", "[ui-testing]") {
  Application application(Disabled, {.show_debug_overlay = false});
  UiTest ui(application);
  auto button = ui.Find(UiSelector::Text("Disabled"));
  REQUIRE_FALSE(button.One().enabled);
  REQUIRE_THROWS_AS(button.Tap(), UiTestFailure);
  ui.TapAt({5, 5});
  REQUIRE(count.Get() == 0);
}

TEST_CASE("Windowless raw input preserves frame boundaries and cancellation", "[ui-testing]") {
  Application application(Counter, {.show_debug_overlay = false});
  UiTest ui(application);
  const auto bounds = ui.Find(UiSelector::Text("Increment")).One().bounds;
  PointerEvent event{.type = PointerEventType::Down, .pointer_id = 1,
      .position = {bounds.x + bounds.width / 2, bounds.y + bounds.height / 2},
      .device_kind = PointerDeviceKind::Touch, .changed_button = PointerButton::Primary,
      .pressed_buttons = PointerButton::Primary};
  ui.SendPointer(event);
  REQUIRE_THROWS_AS(ui.TapAt(event.position), UiTestFailure);
  event.type = PointerEventType::Cancel;
  event.pressed_buttons = PointerButton::None;
  ui.SendPointer(event);
  ui.Pump();
  REQUIRE(count.Get() == 0);
  event.type = PointerEventType::Down;
  event.pressed_buttons = PointerButton::Primary;
  ui.SendPointer(event);
  event.type = PointerEventType::Up;
  event.pressed_buttons = PointerButton::None;
  ui.SendPointer(event);
  REQUIRE(count.Get() == 1);
  REQUIRE(ui.Find(UiSelector::Text("0")).Exists());
  ui.Pump();
  REQUIRE(ui.Find(UiSelector::Text("1")).Exists());
}

TEST_CASE("Windowless secure editors exclude values from observations and captures", "[ui-testing]") {
  Application application(SecureEditor, {.show_debug_overlay = false});
  UiTest ui(application);
  auto editor = ui.Find(UiSelector::Type<TextField>());
  REQUIRE_FALSE(editor.One().value.has_value());
  REQUIRE_FALSE(ui.Find(UiSelector::Value("confidential-value")).Exists());
  REQUIRE_FALSE(ui.FindSemantics(UiSemanticSelector::Value("confidential-value")).Exists());
  REQUIRE(ui.CaptureSnapshot().ToString().find("confidential-value") == std::string::npos);
  editor.Tap();
  editor.ReplaceText("another-secret");
  REQUIRE(editing.Get().text == "another-secret");
  REQUIRE(ui.CaptureSnapshot().ToString().find("another-secret") == std::string::npos);
}

TEST_CASE("Windowless virtual frames drive ordinary Delay tasks", "[ui-testing]") {
  Application application(DelayedCounter, {.show_debug_overlay = false});
  UiTest ui(application);
  ui.Find(UiSelector::Type<Button>()).Tap();
  ui.Pump();
  REQUIRE(count.Get() == 0);
  ui.Pump(99ms);
  REQUIRE(count.Get() == 0);
  ui.PumpUntil([&] { return count.Get() == 1; }, {.timeout = 10ms, .step = 1ms});
  REQUIRE(ui.Find(UiSelector::Text("1")).Exists());
}

TEST_CASE("Windowless queries include live root presentation layers", "[ui-testing]") {
  std::optional<LayerController> layers;
  LayerId id = 0;
  Application application(Counter, {
    .show_debug_overlay = false,
    .root_hooks = {[&](RootContext& root) {
      layers = root.Layers();
      id = layers->Attach({}, [] { return Text("Live layer").Key("layer"); });
    }},
  });
  UiTest ui(application);
  auto layer = ui.Find(UiSelector::Key("layer"));
  REQUIRE(layer.One().text == "Live layer");
  REQUIRE(layers->Dismiss(id));
  ui.Pump();
  REQUIRE_FALSE(layer.Exists());
}

TEST_CASE("Windowless resources use generated packages and explicit configuration", "[ui-testing]") {
  auto provider = std::make_shared<FixtureResources>();
  Application application(Resources, {.show_debug_overlay = false});
  UiTest ui(application, {.resources = {.locale = Locale::FromLanguageTag("fr")}, .resource_provider = provider});
  REQUIRE(ui.Find(UiSelector::Text("application\n")).Exists());
  REQUIRE(ui.Find(UiSelector::Key("locale")).One().text == "fr");
  REQUIRE(provider->opens > 1);
  ui.UpdateResourceConfiguration({.locale = Locale::FromLanguageTag("de")});
  ui.Pump();
  REQUIRE(ui.Find(UiSelector::Key("locale")).One().text == "de");
}

TEST_CASE("Windowless queries enforce creating-thread access", "[ui-testing]") {
  Application application(Counter, {.show_debug_overlay = false});
  UiTest ui(application);
  auto query = ui.Find(UiSelector::Text("0"));
  bool rejected = false;
  std::thread worker([&] {
    try { static_cast<void>(query.Count()); } catch (const std::logic_error&) { rejected = true; }
  });
  worker.join();
  REQUIRE(rejected);
}

TEST_CASE("Windowless empty roots have valid captures and no fabricated text", "[ui-testing]") {
  Application application(Empty, {.show_debug_overlay = false});
  UiTest ui(application);
  REQUIRE(ui.Find(UiSelector::Key("empty")).Exists());
  REQUIRE_FALSE(ui.Find(UiSelector::Text("")).Exists());
  REQUIRE_FALSE(ui.CaptureSnapshot().ToString().empty());
}

TEST_CASE("Windowless captures exclude session identities", "[ui-testing]") {
  Application application(Counter, {.show_debug_overlay = false});
  std::string first;
  {
    UiTest ui(application);
    first = ui.CaptureSnapshot().ToString();
  }
  UiTest ui(application);
  REQUIRE(ui.CaptureSnapshot().ToString() == first);
  ui.Pump();
  REQUIRE(ui.CaptureSnapshot().ToString() == first);
}

TEST_CASE("Windowless reference text measures scalar cells instead of UTF-8 bytes", "[ui-testing]") {
  Application application(UnicodeText, {.show_debug_overlay = false});
  UiTest ui(application);
  REQUIRE(ui.Find(UiSelector::Key("unicode")).One().size == ui.Find(UiSelector::Key("ascii")).One().size);
}

TEST_CASE("Windowless shutdown delivers channel cancellation and disposal on its owner thread", "[ui-testing]") {
  ShutdownProbe probe;
  Application application(Empty, {
    .show_debug_overlay = false,
    .root_hooks = {[&](RootContext& root) { InstallShutdownProbe(root, probe); }},
  });
  {
    UiTest ui(application);
    probe.endpoint.Channel().Invoke("pending", PlatformPayload{}, [](PlatformResult<PlatformPayload>) {});
    ui.Pump();
    REQUIRE(probe.disposals == 0);
  }
  REQUIRE(probe.cancellations == 1);
  REQUIRE(probe.disposals == 1);
  REQUIRE(probe.followups == 1);
  REQUIRE(probe.disposal_thread == std::this_thread::get_id());

  auto payload = std::make_shared<int>(1);
  std::weak_ptr<int> retained = payload;
  bool called = false;
  probe.late_endpoint.Connect({
    .invoke = [](auto, auto, auto) { return std::function<void()>{}; },
    .dispose = [payload, &called] { called = true; },
  });
  payload.reset();
  std::thread worker([&] { probe.late_endpoint.Close(); });
  worker.join();
  probe.late_endpoint = {};
  REQUIRE_FALSE(called);
  REQUIRE(retained.expired());
}

TEST_CASE("Windowless initialization failures drain cleanup without replacing the original exception", "[ui-testing]") {
  ShutdownProbe probe;
  bool fail_hook = false;
  SECTION("Runtime root hook fails") { fail_hook = true; }
  SECTION("First composition fails") {}
  Application application(FailingRoot, {
    .show_debug_overlay = false,
    .root_hooks = {[&](RootContext& root) {
      InstallShutdownProbe(root, probe);
      if (fail_hook) throw std::runtime_error("root hook failed");
    }},
  });
  REQUIRE_THROWS_WITH(UiTest(application), fail_hook ? "root hook failed" : "initial composition failed");
  REQUIRE(probe.disposals == 1);
  REQUIRE(probe.followups == 1);
  REQUIRE(probe.disposal_thread == std::this_thread::get_id());

  auto payload = std::make_shared<int>(1);
  std::weak_ptr<int> retained = payload;
  probe.late_endpoint.Connect({
    .invoke = [](auto, auto, auto) { return std::function<void()>{}; },
    .dispose = [payload] {},
  });
  payload.reset();
  probe.late_endpoint.Close();
  // The original endpoint keeps the dispatcher alive, but this transport no longer owns the payload.
  probe.late_endpoint = {};
  REQUIRE(retained.expired());
}

TEST_CASE("Windowless shutdown bounds cleanup callbacks without committing another frame", "[ui-testing]") {
  ShutdownProbe probe;
  auto& frames = shutdown_frames;
  frames = 0;
  Application application([] { return Empty().With(FrameProbe{&shutdown_frames}); }, {
    .show_debug_overlay = false,
    .root_hooks = {[&](RootContext& root) { InstallShutdownProbe(root, probe); }},
  });
  {
    UiTest ui(application, {.maximum_callbacks_per_frame = 1});
    REQUIRE(ui.Now() == 0);
    REQUIRE(frames == 1);
  }
  REQUIRE(probe.disposals == 1);
  REQUIRE(probe.followups == 0);
  REQUIRE(frames == 1);
}

TEST_CASE("Windowless drag drives controlled sliders and advances each movement frame", "[ui-testing]") {
  Application application(Adjustable, {.show_debug_overlay = false});
  UiTest ui(application);
  const auto bounds = ui.Find(UiSelector::Type<Slider>()).One().bounds;
  const float y = bounds.y + bounds.height * 0.5F;
  ui.Drag({bounds.x + 20.0F, y}, {bounds.x + bounds.width - 20.0F, y}, {.duration = 200ms, .segments = 4});
  REQUIRE(slider_value.Get() > 0.75F);
  REQUIRE(ui.Now() == Catch::Approx(0.2));
  REQUIRE_THROWS_AS(ui.Drag({}, {}, {.segments = 0}), std::invalid_argument);
}

TEST_CASE("Windowless key helpers deliver focus traversal and complete activation", "[ui-testing]") {
  Application application(Counter, {.show_debug_overlay = false});
  UiTest ui(application);
  ui.PressKey(Key::Tab);
  REQUIRE(ui.Find(UiSelector::Focused(true)).One().text == "Increment");
  ui.PressKey(Key::Space);
  REQUIRE(count.Get() == 1);
  REQUIRE(ui.Find(UiSelector::Key("count")).One().text == "1");
  ui.PressKey(Key::Space);
  REQUIRE(count.Get() == 2);
}

TEST_CASE("Windowless semantic queries retain scopes and copies across actions and publications", "[ui-testing]") {
  Application application([] { return Counter().With(Semantics{.identifier = "counter"}); },
                          {.show_debug_overlay = false});
  UiTest ui(application);
  auto scope = ui.FindSemantics(UiSemanticSelector::Identifier("counter"));
  auto button = scope.Find(UiSemanticSelector::Label("Increment"));
  REQUIRE(button.Count() == 1);
  REQUIRE(scope.Find(UiSemanticSelector::Identifier("counter")).Count() == 0);
  const auto before = scope.Find(UiSemanticSelector::Label("0")).All();
  REQUIRE(before.size() == 1);
  button.PerformSemanticAction({SemanticActionKind::Activate, {}});
  REQUIRE(count.Get() == 1);
  REQUIRE(scope.Find(UiSemanticSelector::Label("1")).Exists());
  REQUIRE(before.front().label == "0");
  REQUIRE_FALSE(scope.Find(UiSemanticSelector::Label("0")).Exists());
  REQUIRE_THROWS_AS(button.PerformSemanticAction({SemanticActionKind::SetText, std::string("invalid")}),
                    UiTestFailure);
}

TEST_CASE("Windowless Pump exposes exact animation intermediate frames", "[ui-testing]") {
  Application application(Animated, {.show_debug_overlay = false});
  UiTest ui(application);
  auto node = ui.Find(UiSelector::Key("animated"));
  const float origin = node.One().bounds.x;
  animated = true;
  ui.Pump();
  ui.Pump(250ms);
  REQUIRE(node.One().bounds.x == Catch::Approx(origin + 25.0F));
  const auto quarter = ui.CaptureSnapshot();
  ui.Pump(250ms);
  REQUIRE(node.One().bounds.x == Catch::Approx(origin + 50.0F));
  REQUIRE(ui.CaptureSnapshot().ToString() != quarter.ToString());
  ui.Pump(500ms);
  REQUIRE(node.One().bounds.x == Catch::Approx(origin + 100.0F));
}

TEST_CASE("Windowless settle follows animation and delayed task deadlines", "[ui-testing]") {
  SECTION("Finite animation") {
    Application application(Animated, {.show_debug_overlay = false});
    UiTest ui(application);
    const float origin = ui.Find(UiSelector::Key("animated")).One().bounds.x;
    animated = true;
    ui.PumpAndSettle({.timeout = 2s, .step = 100ms});
    REQUIRE(ui.Find(UiSelector::Key("animated")).One().bounds.x == Catch::Approx(origin + 100));
    REQUIRE(ui.Now() >= 1.0);
    const double completed = ui.Now();
    ui.PumpAndSettle();
    REQUIRE(ui.Now() == completed);
  }
  SECTION("Delayed callback") {
    Application application(DelayedCounter, {.show_debug_overlay = false});
    UiTest ui(application);
    ui.FindSemantics(UiSemanticSelector::Role(SemanticRole::Button))
        .PerformSemanticAction({SemanticActionKind::Activate, {}});
    ui.PumpAndSettle();
    REQUIRE(count.Get() == 1);
    REQUIRE(ui.Now() == Catch::Approx(0.1));
    REQUIRE(ui.Find(UiSelector::Text("1")).Exists());
  }
}

TEST_CASE("Windowless settle bounds continuous work and preserves observations on timeout", "[ui-testing]") {
  shutdown_frames = 0;
  Application application([] { return Empty().With(FrameProbe{&shutdown_frames}); }, {.show_debug_overlay = false});
  UiTest ui(application);
  REQUIRE_THROWS_AS(ui.PumpAndSettle({.timeout = 1s, .maximum_frames = 3}), UiTestFailure);
  REQUIRE(shutdown_frames == 4);
  REQUIRE(ui.Find(UiSelector::Key("empty")).Exists());
  REQUIRE_FALSE(ui.CaptureSnapshot().ToString().empty());
  const double before = ui.Now();
  REQUIRE_THROWS_AS(ui.PumpAndSettle({.timeout = 25ms, .step = 10ms}), UiTestFailure);
  REQUIRE(ui.Now() - before == Catch::Approx(0.025));
  REQUIRE_THROWS_AS(ui.PumpAndSettle({.step = 0ms}), std::invalid_argument);
  REQUIRE_THROWS_AS(ui.PumpAndSettle({.maximum_frames = 0}), std::invalid_argument);
  ui.Pump();
}

TEST_CASE("Windowless input observations expose composition and reject ended sessions", "[ui-testing]") {
  Application application(Editor, {.show_debug_overlay = false});
  UiTest ui(application);
  REQUIRE_FALSE(ui.ActiveTextInput());
  auto editor = ui.Find(UiSelector::Key("editor"));
  editor.Tap();
  const auto initial = ui.ActiveTextInput();
  REQUIRE(initial);
  REQUIRE(initial->session_id != 0);
  const auto before = ui.CaptureSnapshot();
  TextInputCommand compose;
  compose.kind = TextInputCommandKind::UpdateComposition;
  compose.text = "A😀";
  REQUIRE(ui.SendTextInput({initial->session_id, {compose}}).result_code == TextInputResultCode::Ok);
  const auto composing = ui.ActiveTextInput();
  REQUIRE(composing);
  REQUIRE(composing->composition == TextRange{0, 3});
  REQUIRE(composing->selection == TextSelection{3, 3});
  REQUIRE(composing->revision > initial->revision);
  REQUIRE(ui.CaptureSnapshot() == before);
  REQUIRE(editor.One().value == "");
  ui.Pump();
  REQUIRE(editor.One().value == "A😀");
  TextInputCommand finish;
  finish.kind = TextInputCommandKind::FinishComposition;
  REQUIRE(ui.SendTextInput({initial->session_id, {finish}}).result_code == TextInputResultCode::Ok);
  REQUIRE_FALSE(ui.ActiveTextInput()->composition);
  REQUIRE(composing->composition == TextRange{0, 3});
  ui.Pump();
  ui.TapAt({790.0F, 590.0F});
  REQUIRE_FALSE(ui.ActiveTextInput());
  REQUIRE(ui.SendTextInput({initial->session_id, {compose}}).result_code == TextInputResultCode::SessionMismatch);
  editor.Tap();
  REQUIRE(ui.ActiveTextInput()->session_id != initial->session_id);
}

TEST_CASE("Windowless taps use the visible portion of clipped nodes", "[ui-testing]") {
  Application application([] {
    count = UseState(0);
    return Button("Clipped").OnClick([value = count]() mutable { value += 1; })
        .With(Frame{100.0F, 40.0F}, Offset{Point{-75.0F, 0.0F}});
  }, {.show_debug_overlay = false});
  UiTest ui(application, {.viewport = {100.0F, 80.0F}, .resource_provider = {}});
  const auto button = ui.Find(UiSelector::Text("Clipped"));
  const auto info = button.One();
  REQUIRE(info.bounds.x + info.bounds.width * 0.5F < 0);
  REQUIRE(info.visible_bounds.x == 0);
  REQUIRE(info.visible_bounds.width > 0);
  REQUIRE(info.visible_bounds.width < info.bounds.width);
  button.Tap();
  REQUIRE(count.Get() == 1);
}

TEST_CASE("Windowless scrolling searches virtual items through physical input", "[ui-testing]") {
  Application application(ScrollableItems, {.show_debug_overlay = false});
  UiTest ui(application, {.viewport = {200.0F, 120.0F}, .resource_provider = {}});
  auto list = ui.Find(UiSelector::Key("list"));
  REQUIRE_FALSE(list.Find(UiSelector::Key(std::size_t{30})).Exists());
  const auto consumed = list.ScrollBy({0, 80});
  REQUIRE(consumed.y > 0);
  auto item = list.ScrollUntil(UiSelector::Key(std::size_t{30}), {.step = {0, 100}, .maximum_steps = 30});
  REQUIRE(item.One().in_viewport);
  item.Tap();
  REQUIRE(count.Get() == 1);
  const double found_at = ui.Now();
  REQUIRE(list.ScrollUntil(UiSelector::Key(std::size_t{30})).One().in_viewport);
  REQUIRE(ui.Now() == found_at);
  REQUIRE_THROWS_AS(list.ScrollUntil(UiSelector::Key("absent"), {.maximum_steps = 2}), UiTestFailure);
  REQUIRE_THROWS_AS(list.ScrollUntil(UiSelector::Type<Button>()), UiTestFailure);
  REQUIRE_THROWS_AS(list.ScrollUntil(UiSelector::Key("absent"), {.step = {}}), std::invalid_argument);
  REQUIRE(list.Exists());
}

TEST_CASE("Windowless query failures include selector values and useful scope candidates", "[ui-testing]") {
  Application application(Scopes, {.show_debug_overlay = false});
  UiTest ui(application);
  try {
    ui.Find(UiSelector::Key("first")).Find(UiSelector::Text("Missing")).Tap();
    FAIL("Expected a query failure");
  } catch (const UiTestFailure& failure) {
    const std::string message = failure.what();
    INFO(message);
    REQUIRE(message.find("Tap expected one node, found 0") != std::string::npos);
    REQUIRE(message.find("Key(\"first\") -> Text(\"Missing\")") != std::string::npos);
    REQUIRE(message.find("Delete") != std::string::npos);
    REQUIRE(message.find("Viewport:") != std::string::npos);
  }
  REQUIRE_THROWS_WITH(ui.Find(UiSelector::Key("action")).One(), Catch::Matchers::ContainsSubstring("found 2"));
  REQUIRE_THROWS_WITH(ui.FindSemantics(UiSemanticSelector::Label("Missing")).One(),
                      Catch::Matchers::ContainsSubstring("Delete"));
}

TEST_CASE("Windowless snapshot differences identify fields and own comparison data", "[ui-testing]") {
  Application application(Counter, {.show_debug_overlay = false});
  std::optional<UiSnapshot> baseline;
  std::optional<UiSnapshot> changed;
  {
    UiTest ui(application);
    baseline = ui.CaptureSnapshot();
    ui.Pump();
    REQUIRE(ui.CaptureSnapshot() == *baseline);
    REQUIRE(ui.CaptureSnapshot().Diff(*baseline).empty());
    ui.Find(UiSelector::Key("increment")).Tap();
    changed = ui.CaptureSnapshot();
    REQUIRE(*changed != *baseline);
    const auto diff = changed->Diff(*baseline);
    REQUIRE(diff.find("/label: \"0\" -> \"1\"") != std::string::npos);
    REQUIRE(diff.find("/text: \"0\" -> \"1\"") != std::string::npos);
    REQUIRE(diff.find("Increment") == std::string::npos);
  }
  REQUIRE_FALSE(changed->Diff(*baseline).empty());
  UiTest recreated(application);
  REQUIRE(recreated.CaptureSnapshot() == *baseline);
}

TEST_CASE("Windowless snapshot differences group inserted and removed subtrees", "[ui-testing]") {
  Application application(SnapshotItems, {.show_debug_overlay = false});
  UiTest ui(application);
  const auto baseline = ui.CaptureSnapshot();
  count = 1;
  ui.Pump();
  const auto inserted = ui.CaptureSnapshot();
  const auto added = inserted.Diff(baseline);
  INFO(added);
  REQUIRE(added.find("subtree added") != std::string::npos);
  REQUIRE(added.find("identifier=\"inserted\"") != std::string::npos);
  REQUIRE(added.find("Stable") == std::string::npos);
  REQUIRE(std::count(added.begin(), added.end(), '\n') < 10);
  const auto removed = baseline.Diff(inserted);
  REQUIRE(removed.find("subtree removed") != std::string::npos);
  count = 0;
  ui.Pump();
  REQUIRE(ui.CaptureSnapshot() == baseline);
}

TEST_CASE("Windowless settle drains callback batches without inventing time", "[ui-testing]") {
  PlatformAdapter* dispatcher = nullptr;
  Application application(Empty, {
    .show_debug_overlay = false,
    .root_hooks = {[&](RootContext& root) {
      root.RegisterPlatformModule<int>("testing/Dispatcher", [&](PlatformAdapter& adapter) {
        dispatcher = &adapter;
        return 0;
      });
      static_cast<void>(root.OpenPlatformModule<int>("testing/Dispatcher"));
    }},
  });
  int calls = 0;
  bool repeat = true;
  std::function<void()> callback = [&] {
    ++calls;
    if (repeat) dispatcher->DispatchToUIThread(callback);
  };
  UiTest ui(application);
  REQUIRE(dispatcher);
  dispatcher->DispatchToUIThread(callback);
  REQUIRE_THROWS_WITH(ui.PumpAndSettle({.maximum_frames = 3}),
                      Catch::Matchers::ContainsSubstring("callbacks=1"));
  REQUIRE(calls == 3);
  REQUIRE(ui.Now() == 0);
  repeat = false;
  ui.PumpAndSettle();
  REQUIRE(calls == 4);
  REQUIRE(ui.Now() == 0);
}

TEST_CASE("Windowless snapshots retain image content and external texture geometry", "[ui-testing]") {
  Application application([] {
    count = UseState(0);
    const auto extent = static_cast<std::uint32_t>(10 + count.Get());
    return Canvas([image = ImageAsset::FromEncoded(MakeTestPng(extent, 10)),
                   texture = MakeTestExternalTexture({static_cast<float>(extent), 10})](PaintContext& paint, Size) {
      paint.DrawImage(image, {0, 0, 40, 40});
      paint.DrawImage(texture, {40, 0, 40, 40});
    }).With(Frame{80.0F, 40.0F});
  }, {.show_debug_overlay = false});
  std::optional<UiSnapshot> before;
  std::optional<UiSnapshot> after;
  {
    UiTest ui(application);
    before = ui.CaptureSnapshot();
    count = 1;
    ui.Pump();
    after = ui.CaptureSnapshot();
    count = 0;
    ui.Pump();
    REQUIRE(ui.CaptureSnapshot() == *before);
  }
  const auto diff = after->Diff(*before);
  REQUIRE(diff.find("image content changed") != std::string::npos);
  REQUIRE(diff.find("/intrinsic_size/width: 10.000000 -> 11.000000") != std::string::npos);
  REQUIRE(diff.find("89504e47") == std::string::npos);
  REQUIRE(before->ToString().find("89504e47") != std::string::npos);
}

TEST_CASE("Windowless snapshot reports bound display without truncating equality", "[ui-testing]") {
  Application application([]() -> View {
    count = UseState(0);
    std::vector<View> children;
    for (int i = 0; i < 100; ++i) {
      children.push_back(Text(std::string(count.Get() ? "After " : "Before ") + std::to_string(i)).Key(i));
    }
    return Stack(std::move(children));
  }, {.show_debug_overlay = false});
  UiTest ui(application);
  const auto before = ui.CaptureSnapshot();
  count = 1;
  ui.Pump();
  const auto after = ui.CaptureSnapshot();
  REQUIRE(after != before);
  const auto diff = after.Diff(before);
  REQUIRE(diff.find("additional structural changes omitted") != std::string::npos);
  REQUIRE(std::count(diff.begin(), diff.end(), '\n') <= 65);
  REQUIRE(after.ToString().find("After 99") != std::string::npos);
}

TEST_CASE("Windowless snapshot reports omission only beyond the difference limit", "[ui-testing]") {
  const int differences = GENERATE(0, 63, 64, 65);
  Application application([] {
    count = UseState(0);
    return Canvas([changed = count.Get()](PaintContext& paint, Size) {
      for (int i = 0; i < 65; ++i) {
        paint.DrawRect({static_cast<float>(i), 0, 1, 1}, i < changed ? Color::Rgb(255, 0, 0) : Color::Black());
      }
    }).With(Frame{65.0F, 1.0F});
  }, {.show_debug_overlay = false});
  UiTest ui(application);
  const auto before = ui.CaptureSnapshot();
  count = differences;
  ui.Pump();
  const auto after = ui.CaptureSnapshot();
  const auto diff = after.Diff(before);
  INFO(diff);
  REQUIRE((after == before) == (differences == 0));
  REQUIRE((diff.find("additional structural changes omitted") != std::string::npos) == (differences > 64));
  REQUIRE(std::count(diff.begin(), diff.end(), '\n') == std::min(differences, 64) + (differences > 64 ? 1 : 0));
}

} // namespace huxerui::test
