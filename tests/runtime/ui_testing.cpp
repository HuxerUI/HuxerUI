#include <catch2/catch_amalgamated.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <thread>

#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>
#include <ui_test_fixture_resources.h>

#include "application/platform_registry_internal.h"

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
  UiNodeInfo info;
  {
    UiTest ui(application);
    query = ui.Find(UiSelector::Key("count"));
    info = query->One();
    snapshot = ui.CaptureSnapshot();
    const auto before = snapshot->ToString();
    ui.Find(UiSelector::Text("Increment")).Tap();
    REQUIRE(ui.CaptureSnapshot().ToString() != before);
    REQUIRE(snapshot->ToString() == before);
  }
  REQUIRE_THROWS_AS(query->Exists(), UiTestFailure);
  REQUIRE(info.text == "0");
  REQUIRE(snapshot->ToString().starts_with("huxerui-ui-snapshot 1"));
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

} // namespace huxerui::test
