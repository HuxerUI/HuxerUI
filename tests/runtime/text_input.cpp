#include "runtime_test_support.h"

#include <memory>
#include <utility>
#include <vector>

namespace huxerui::test {
namespace {

class ProbeTextInputClient final : public huxerui::TextInputClient {
public:
  TextInputConfiguration Configuration() const override {
    return configuration;
  }

  TextInputState State() const override {
    return state;
  }

  TextInputState BeginTextInput(TextInputSessionId session_id) override {
    ++begin_count;
    state.session_id = session_id;
    state.revision = 1;
    return state;
  }

  TextInputApplyResult ApplyTextInput(const TextInputCommandBatch& batch) override {
    ++apply_count;
    if (batch.session_id != state.session_id || batch.commands.empty()) {
      return {
          TextInputResultCode::SessionMismatch,
          TextInputSyncAction::None,
          false,
      };
    }
    for (const TextInputCommand& command : batch.commands) {
      if (command.selection_after.has_value()) {
        state.selection = *command.selection_after;
      }
    }
    ++state.revision;
    return {
        TextInputResultCode::Ok,
        TextInputSyncAction::Update,
        true,
    };
  }

  TextInputContext
  QueryTextInputContext(TextInputSessionId session_id, TextOffset start, TextOffset length) const override {
    ++context_queries;
    static_cast<void>(start);
    static_cast<void>(length);
    return {
        TextInputResultCode::Ok,
        session_id,
        0,
        6,
        "abcdef",
        state.selection,
        state.composition,
    };
  }

  TextInputGeometry QueryTextInputGeometry(TextInputSessionId session_id, TextRange range) const override {
    ++geometry_queries;
    return {
        TextInputResultCode::Ok,
        session_id,
        {
            static_cast<float>(range.end * 10),
            0.0F,
            1.0F,
            20.0F,
        },
        {},
    };
  }

  TextInputPositionResult QueryTextInputPosition(TextInputSessionId session_id, Point point) const override {
    ++position_queries;
    return {
        TextInputResultCode::Ok,
        session_id,
        {
            static_cast<TextOffset>(point.x / 10.0F),
            TextAffinity::Downstream,
        },
    };
  }

  TextInputKeyResult HandleTextKey(const KeyEvent& event) override {
    ++key_count;
    last_key = event.key;
    return handle_keys ? TextInputKeyResult::Handled : TextInputKeyResult::Unhandled;
  }

  void EndTextInput(TextInputSessionId session_id, TextInputEndReason reason) override {
    ++end_count;
    ended_sessions.push_back(session_id);
    end_reasons.push_back(reason);
    state.session_id = 0;
  }

  void SetSelection(TextSelection selection) {
    state.selection = selection;
    ++state.revision;
  }

  void ChangeAction(TextInputAction action) {
    configuration.action = action;
  }

  TextInputConfiguration configuration;
  TextInputState state;
  bool handle_keys = false;
  int begin_count = 0;
  int apply_count = 0;
  int end_count = 0;
  mutable int context_queries = 0;
  mutable int geometry_queries = 0;
  mutable int position_queries = 0;
  int key_count = 0;
  Key last_key = Key::Unknown;
  std::vector<TextInputSessionId> ended_sessions;
  std::vector<TextInputEndReason> end_reasons;
};

class ProbePlatformTextInput final : public huxerui::PlatformTextInput {
public:
  void Start(
      TextInputSessionId session_id, const TextInputConfiguration& configuration, const TextInputState& state
  ) override {
    started_sessions.push_back(session_id);
    started_configurations.push_back(configuration);
    started_states.push_back(state);
  }

  void Update(TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry) override {
    updated_sessions.push_back(session_id);
    updated_states.push_back(state);
    updated_geometry.push_back(geometry);
  }

  void Restart(
      TextInputSessionId session_id, const TextInputConfiguration& configuration, const TextInputState& state
  ) override {
    restarted_sessions.push_back(session_id);
    restarted_configurations.push_back(configuration);
    restarted_states.push_back(state);
  }

  void Stop(TextInputSessionId session_id) override {
    stopped_sessions.push_back(session_id);
  }

  std::vector<TextInputSessionId> started_sessions;
  std::vector<TextInputConfiguration> started_configurations;
  std::vector<TextInputState> started_states;
  std::vector<TextInputSessionId> updated_sessions;
  std::vector<TextInputState> updated_states;
  std::vector<TextInputGeometry> updated_geometry;
  std::vector<TextInputSessionId> restarted_sessions;
  std::vector<TextInputConfiguration> restarted_configurations;
  std::vector<TextInputState> restarted_states;
  std::vector<TextInputSessionId> stopped_sessions;
};

struct TextInputProbe;

class TextInputProbeExtension final : public NodeExtension {
public:
  TextInputProbeExtension(MountedNode& node, const TextInputProbe& probe);

  void Update(MountedNode& node, const TextInputProbe& probe);

  std::shared_ptr<huxerui::TextInputClient> GetTextInputClient() noexcept override {
    return client_;
  }

  bool HitTest(MountedNode& node, Point position) const override {
    static_cast<void>(node);
    static_cast<void>(position);
    return handle_pointer_;
  }

  PointerResult OnPointer(MountedNode& node, const PointerEvent& event) override {
    static_cast<void>(node);
    if (!handle_pointer_ || event.type != PointerEventType::Down) {
      return PointerResult::Ignored;
    }
    client_->SetSelection({pointer_selection_, pointer_selection_});
    return PointerResult::Handled;
  }

private:
  std::shared_ptr<ProbeTextInputClient> client_;
  bool handle_pointer_ = false;
  TextOffset pointer_selection_ = 0;
};

struct TextInputProbe {
  using Extension = TextInputProbeExtension;

  std::shared_ptr<ProbeTextInputClient> client;
  bool handle_pointer = false;
  TextOffset pointer_selection = 0;
};

TextInputProbeExtension::TextInputProbeExtension(MountedNode& node, const TextInputProbe& probe) {
  Update(node, probe);
}

void TextInputProbeExtension::Update(MountedNode& node, const TextInputProbe& probe) {
  static_cast<void>(node);
  client_ = probe.client;
  handle_pointer_ = probe.handle_pointer;
  pointer_selection_ = probe.pointer_selection;
}

std::shared_ptr<ProbeTextInputClient> first_text_client;
std::shared_ptr<ProbeTextInputClient> second_text_client;
State<bool> text_client_visible;
State<bool> use_second_text_client;
int text_key_events = 0;
int text_activations = 0;

View TwoTextClientsApp() {
  return Column{
      Text("First").With(Focusable{}, TextInputProbe{first_text_client}),
      Text("Second").With(Focusable{}, TextInputProbe{second_text_client}),
  };
}

View PointerTextClientApp() {
  return Text("Pointer").With(
      Focusable{},
      TextInputProbe{
          first_text_client,
          true,
          2,
      }
  );
}

View ReplaceTextClientApp() {
  auto second = UseState(false);
  use_second_text_client = second;
  return Text("Replace").With(
      Focusable{},
      TextInputProbe{
          second.Get() ? second_text_client : first_text_client,
      }
  );
}

View RemoveTextClientApp() {
  auto visible = UseState(true);
  text_client_visible = visible;
  if (visible.Get()) {
    return Text("Editable").With(Focusable{}, TextInputProbe{first_text_client});
  }
  return Text("Editable").With(Focusable{});
}

View MultipleTextClientsApp() {
  return Text("Invalid").With(Focusable{}, TextInputProbe{first_text_client}, TextInputProbe{second_text_client});
}

View TextKeyClientApp() {
  return Text("Keys").OnClick(
                         [] { ++text_activations; }
  ).On<ViewEvents::KeyDown>([](const KeyEvent&) {
     ++text_key_events;
   }).With(Focusable{}, TextInputProbe{first_text_client});
}

void ResetTextInputProbes() {
  first_text_client = std::make_shared<ProbeTextInputClient>();
  second_text_client = std::make_shared<ProbeTextInputClient>();
  text_client_visible = State<bool>{};
  use_second_text_client = State<bool>{};
  text_key_events = 0;
  text_activations = 0;
}

void FocusNext(Runtime& runtime) {
  runtime.HandleKeyEvent(
      KeyEvent{
          KeyEventType::Down,
          Key::Tab,
      }
  );
}

} // namespace

TEST_CASE("TestTextInputSessionFollowsFocus") {
  ResetTextInputProbes();
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;

  {
    Runtime runtime{TwoTextClientsApp, platform};
    runtime.SetViewport({200.0F, 100.0F});
    runtime.BuildFrame();

    FocusNext(runtime);
    REQUIRE(first_text_client->begin_count == 1);
    REQUIRE(second_text_client->begin_count == 0);
    REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1});

    FocusNext(runtime);
    REQUIRE(first_text_client->end_count == 1);
    REQUIRE(first_text_client->end_reasons.back() == TextInputEndReason::FocusLost);
    REQUIRE(second_text_client->begin_count == 1);
    REQUIRE(text_input.stopped_sessions == std::vector<TextInputSessionId>{1});
    REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1, 2});
  }

  REQUIRE(second_text_client->end_count == 1);
  REQUIRE(second_text_client->end_reasons.back() == TextInputEndReason::RuntimeDestroyed);
  REQUIRE(text_input.stopped_sessions == std::vector<TextInputSessionId>{1, 2});
}

TEST_CASE("TestTextInputSessionSurvivesRecompositionAndSynchronizesState") {
  ResetTextInputProbes();
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;

  Runtime runtime{TwoTextClientsApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();
  FocusNext(runtime);

  runtime.InvalidateRoot();
  runtime.BuildFrame();
  REQUIRE(first_text_client->begin_count == 1);
  REQUIRE(text_input.started_sessions.size() == 1);
  REQUIRE(text_input.stopped_sessions.empty());

  first_text_client->SetSelection({2, 2});
  runtime.BuildFrame();
  REQUIRE(text_input.updated_sessions == std::vector<TextInputSessionId>{1});
  REQUIRE(text_input.updated_states.back().selection == TextSelection{2, 2});
  REQUIRE(text_input.updated_geometry.back().result_code == TextInputResultCode::Ok);

  first_text_client->ChangeAction(TextInputAction::Search);
  runtime.BuildFrame();
  REQUIRE(text_input.restarted_sessions == std::vector<TextInputSessionId>{1});
  REQUIRE(text_input.restarted_configurations.back().action == TextInputAction::Search);
}

TEST_CASE("TestTextInputActionValidatesSessionConfigurationAndClientHandling") {
  ResetTextInputProbes();
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;

  Runtime runtime{TwoTextClientsApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();
  FocusNext(runtime);

  REQUIRE_FALSE(runtime.PerformTextInputAction(2, TextInputAction::Done));
  REQUIRE_FALSE(runtime.PerformTextInputAction(1, TextInputAction::Search));
  REQUIRE_FALSE(runtime.PerformTextInputAction(1, TextInputAction::Done));
  REQUIRE(first_text_client->key_count == 1);
  REQUIRE(first_text_client->last_key == Key::Enter);

  first_text_client->handle_keys = true;
  REQUIRE(runtime.PerformTextInputAction(1, TextInputAction::Done));
  REQUIRE(first_text_client->key_count == 2);

  first_text_client->configuration.multiline = true;
  runtime.BuildFrame();
  REQUIRE_FALSE(runtime.PerformTextInputAction(1, TextInputAction::Done));
  REQUIRE(runtime.PerformTextInputAction(1, TextInputAction::Newline));
  REQUIRE(first_text_client->key_count == 3);
}

TEST_CASE("TestTextInputCommandsAndQueriesRejectStaleSessions") {
  ResetTextInputProbes();
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;

  Runtime runtime{TwoTextClientsApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();
  FocusNext(runtime);

  TextInputCommand selection;
  selection.kind = TextInputCommandKind::SetSelection;
  selection.selection_after = TextSelection{3, 3};

  const TextInputApplyResult stale = runtime.HandleTextInputCommands({
      2,
      {selection},
  });
  REQUIRE(stale.result_code == TextInputResultCode::SessionMismatch);
  REQUIRE(first_text_client->apply_count == 0);
  REQUIRE(runtime.QueryTextInputContext(2, 0, 2).result_code == TextInputResultCode::SessionMismatch);
  REQUIRE(runtime.QueryTextInputGeometry(2, {0, 0}).result_code == TextInputResultCode::SessionMismatch);
  REQUIRE(runtime.QueryTextInputPosition(2, {20.0F, 0.0F}).result_code == TextInputResultCode::SessionMismatch);

  const TextInputApplyResult applied = runtime.HandleTextInputCommands({
      1,
      {selection},
  });
  REQUIRE(applied.result_code == TextInputResultCode::Ok);
  REQUIRE(applied.changed);
  REQUIRE(first_text_client->apply_count == 1);
  REQUIRE(first_text_client->state.selection == TextSelection{3, 3});
  REQUIRE(text_input.updated_states.back().selection == TextSelection{3, 3});

  const TextInputContext context = runtime.QueryTextInputContext(1, 0, 3);
  REQUIRE(context.result_code == TextInputResultCode::Ok);
  REQUIRE(context.text == "abcdef");
  REQUIRE(first_text_client->context_queries == 1);

  const TextInputGeometry geometry = runtime.QueryTextInputGeometry(1, {3, 3});
  REQUIRE(geometry.result_code == TextInputResultCode::Ok);
  REQUIRE(geometry.caret.x == 30.0F);

  const TextInputPositionResult position = runtime.QueryTextInputPosition(1, {40.0F, 0.0F});
  REQUIRE(position.result_code == TextInputResultCode::Ok);
  REQUIRE(position.position.offset == 4);
  REQUIRE(first_text_client->position_queries == 1);
}

TEST_CASE("TestPointerUpdatesSelectionBeforeStartingTextInput") {
  ResetTextInputProbes();
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;

  Runtime runtime{PointerTextClientApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();

  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          1,
          {10.0F, 10.0F},
      }
  );

  REQUIRE(text_input.started_states.size() == 1);
  REQUIRE(text_input.started_states.front().selection == TextSelection{2, 2});
}

TEST_CASE("TestTextInputClientReplacementAndRemovalCloseSessions") {
  ResetTextInputProbes();
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;

  Runtime replacement{ReplaceTextClientApp, platform};
  replacement.SetViewport({200.0F, 100.0F});
  replacement.BuildFrame();
  FocusNext(replacement);

  use_second_text_client = true;
  replacement.BuildFrame();
  REQUIRE(first_text_client->end_count == 1);
  REQUIRE(first_text_client->end_reasons.back() == TextInputEndReason::ClientRemoved);
  REQUIRE(second_text_client->begin_count == 1);
  REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1, 2});

  ResetTextInputProbes();
  ProbePlatformTextInput removal_text_input;
  TestPlatform removal_platform;
  removal_platform.platform_text_input = &removal_text_input;
  Runtime removal{RemoveTextClientApp, removal_platform};
  removal.SetViewport({200.0F, 100.0F});
  removal.BuildFrame();
  FocusNext(removal);

  text_client_visible = false;
  removal.BuildFrame();
  REQUIRE(first_text_client->end_count == 1);
  REQUIRE(first_text_client->end_reasons.back() == TextInputEndReason::ClientRemoved);
  REQUIRE(removal_text_input.stopped_sessions == std::vector<TextInputSessionId>{1});
}

TEST_CASE("TestTextInputClientHandlesKeysBeforeGenericEvents") {
  ResetTextInputProbes();
  first_text_client->handle_keys = true;
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;

  Runtime runtime{TextKeyClientApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();
  FocusNext(runtime);

  runtime.HandleKeyEvent(
      KeyEvent{
          KeyEventType::Down,
          Key::Enter,
      }
  );
  REQUIRE(first_text_client->key_count == 1);
  REQUIRE(first_text_client->last_key == Key::Enter);
  REQUIRE(text_key_events == 0);
  REQUIRE(text_activations == 0);
}

TEST_CASE("TestReadOnlyTextInputConfigurationDoesNotOpenKeyboard") {
  ResetTextInputProbes();
  first_text_client->configuration.read_only = true;
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;

  Runtime runtime{TextKeyClientApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();
  FocusNext(runtime);

  REQUIRE(first_text_client->begin_count == 0);
  REQUIRE(text_input.started_sessions.empty());

  first_text_client->configuration.read_only = false;
  runtime.BuildFrame();
  REQUIRE(first_text_client->begin_count == 1);
  REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1});

  first_text_client->configuration.read_only = true;
  runtime.BuildFrame();
  REQUIRE(first_text_client->end_count == 1);
  REQUIRE(first_text_client->end_reasons.back() == TextInputEndReason::ReadOnly);
  REQUIRE(text_input.stopped_sessions == std::vector<TextInputSessionId>{1});
}

TEST_CASE("TestFocusableNodeRejectsMultipleTextInputClients") {
  ResetTextInputProbes();
  TestPlatform platform;
  Runtime runtime{MultipleTextClientsApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();

  REQUIRE_THROWS_AS(FocusNext(runtime), std::logic_error);
}

} // namespace huxerui::test
