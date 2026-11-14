#include "runtime_test_support.h"

#include <memory>
#include <array>
#include <functional>
#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace huxerui::test {
namespace {

class ProbeTextInputClient : public huxerui::TextInputClient {
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
    };
  }

  TextInputContext
  QueryTextInputContext(TextInputSessionId session_id, TextOffset, TextOffset) const override {
    ++context_queries;
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
            invalid_geometry ? -1.0F : 1.0F,
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

  void AdvanceContentRevisionWithoutStateRevision() {
    ++state.content_revision;
  }

  void ChangeAction(TextInputAction action) {
    configuration.action = action;
  }

  TextInputConfiguration configuration;
  TextInputState state;
  bool handle_keys = false;
  bool invalid_geometry = false;
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

class EditingActionProbe final : public ProbeTextInputClient, public TextSelectionClient {
public:
  TextEditingActionStatus GetTextEditingActionStatus(TextEditingAction, const Clipboard&) const override {
    return status;
  }

  void PerformTextEditingAction(TextEditingAction action, const Clipboard& clipboard) override {
    actions.push_back(action);
    tasks.Launch(ReceiveWriteResult(clipboard.WriteContentAsync(std::string("custom"))));
  }

  bool ClearSelection() override { return false; }
  bool SelectWord(Point) override {
    SetSelection({0, 2});
    return true;
  }
  bool ExtendSelection(Point, bool) override { return false; }
  std::optional<TextSelectionGeometry> QuerySelectionGeometry() const override {
    return TextSelectionGeometry{.toolbar_anchor = Rect{0.0F, 0.0F, 30.0F, 20.0F}};
  }
  Color SelectionHandleColor() const noexcept override { return {}; }

  TextEditingActionStatus status = TextEditingActionStatus::Handle;
  std::vector<TextEditingAction> actions;
  TaskScope tasks;
  std::optional<bool> write_result;

private:
  Task<void> ReceiveWriteResult(Task<bool> pending) {
    write_result = co_await std::move(pending);
  }
};

class EditingActionClipboard final : public PlatformClipboard {
public:
  std::optional<std::string> ReadText() override { return "clipboard"; }
  bool WriteText(std::string_view text) override {
    written_text = text;
    return true;
  }

  std::optional<std::string> written_text;

private:
  bool WriteContent(ClipboardContent) override { return false; }
};

std::shared_ptr<EditingActionProbe> editing_action_probe;

class ProbePlatformTextInput final : public huxerui::PlatformTextInput {
public:
  void Start(
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) override {
    started_sessions.push_back(session_id);
    started_configurations.push_back(configuration);
    started_states.push_back(state);
    started_geometry.push_back(geometry);
  }

  void Update(TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry) override {
    updated_sessions.push_back(session_id);
    updated_states.push_back(state);
    updated_geometry.push_back(geometry);
  }

  void Restart(
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) override {
    restarted_sessions.push_back(session_id);
    restarted_configurations.push_back(configuration);
    restarted_states.push_back(state);
    restarted_geometry.push_back(geometry);
  }

  void Stop(TextInputSessionId session_id) override {
    stopped_sessions.push_back(session_id);
    if (on_stop) {
      on_stop(session_id);
    }
  }

  void RequestShowKeyboard(TextInputSessionId session_id) override {
    keyboard_requests.emplace_back(session_id, true);
  }

  void RequestHideKeyboard(TextInputSessionId session_id) override {
    keyboard_requests.emplace_back(session_id, false);
  }

  std::vector<TextInputSessionId> started_sessions;
  std::vector<TextInputConfiguration> started_configurations;
  std::vector<TextInputState> started_states;
  std::vector<TextInputGeometry> started_geometry;
  std::vector<TextInputSessionId> updated_sessions;
  std::vector<TextInputState> updated_states;
  std::vector<TextInputGeometry> updated_geometry;
  std::vector<TextInputSessionId> restarted_sessions;
  std::vector<TextInputConfiguration> restarted_configurations;
  std::vector<TextInputState> restarted_states;
  std::vector<TextInputGeometry> restarted_geometry;
  std::vector<TextInputSessionId> stopped_sessions;
  std::vector<std::pair<TextInputSessionId, bool>> keyboard_requests;
  std::function<void(TextInputSessionId)> on_stop;
};

struct TextInputProbe;

class TextInputProbeExtension final : public NodeExtension {
public:
  TextInputProbeExtension(ViewNode& node, const TextInputProbe& probe);

  void Update(ViewNode& node, const TextInputProbe& probe);

  std::shared_ptr<huxerui::TextInputClient> GetTextInputClient() noexcept override {
    return client_;
  }

  TextSelectionClient* GetTextSelectionClient() noexcept override {
    return dynamic_cast<TextSelectionClient*>(client_.get());
  }

  void OnFocusChanged(ViewNode&, bool focused, bool) override {
    if (focused && show_keyboard_on_focus_) {
      ShowKeyboard();
    }
  }

  bool HitTest(ViewNode&, Point) const override {
    return handle_pointer_;
  }

  PointerResult OnPointer(ViewNode&, const PointerEvent& event) override {
    if (!handle_pointer_ || event.type != PointerEventType::Down) {
      return PointerResult::Ignored;
    }
    client_->SetSelection({pointer_selection_, pointer_selection_});
    if (keyboard_on_press_) {
      if (*keyboard_on_press_) {
        HideKeyboard();
        ShowKeyboard();
      } else {
        ShowKeyboard();
        HideKeyboard();
      }
    }
    return PointerResult::Handled;
  }

private:
  std::shared_ptr<ProbeTextInputClient> client_;
  bool handle_pointer_ = false;
  TextOffset pointer_selection_ = 0;
  std::optional<bool> keyboard_on_press_;
  bool show_keyboard_on_focus_ = false;
};

struct TextInputProbe {
  using Extension = TextInputProbeExtension;

  std::shared_ptr<ProbeTextInputClient> client;
  bool handle_pointer = false;
  TextOffset pointer_selection = 0;
  std::optional<bool> keyboard_on_press = std::nullopt;
  bool show_keyboard_on_focus = false;
};

TextInputProbeExtension::TextInputProbeExtension(ViewNode& node, const TextInputProbe& probe) {
  Update(node, probe);
}

void TextInputProbeExtension::Update(ViewNode&, const TextInputProbe& probe) {
  client_ = probe.client;
  handle_pointer_ = probe.handle_pointer;
  pointer_selection_ = probe.pointer_selection;
  keyboard_on_press_ = probe.keyboard_on_press;
  show_keyboard_on_focus_ = probe.show_keyboard_on_focus;
}

std::shared_ptr<ProbeTextInputClient> first_text_client;
std::shared_ptr<ProbeTextInputClient> second_text_client;
std::optional<DialogHandle> text_input_dialog;
std::optional<WindowHandle> text_input_window;
std::optional<bool> keyboard_on_press;
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
  text_input_window = UseWindow();
  return Text("Pointer").With(
      Focusable{},
      TextInputProbe{
          first_text_client,
          true,
          2,
          keyboard_on_press,
      }
  );
}

View KeyboardRequestBeforeFocusMoveApp() {
  return TwoTextClientsApp().On<ViewEvents::KeyIntercept>([window = UseWindow()](const KeyEvent&) {
    window.ShowKeyboard();
    return false;
  });
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

View DialogTextInputApp() {
  text_input_dialog = UseDialog();
  return Text("Application");
}

View DialogTextInputContent() {
  return Text("Dialog input").With(Focusable{}, TextInputProbe{first_text_client});
}

View TextKeyClientApp() {
  return Text("Keys").OnClick(
                         [] { ++text_activations; }
  ).On<ViewEvents::KeyDown>([](const KeyEvent&) {
     ++text_key_events;
     return false;
   }).With(Focusable{}, TextInputProbe{first_text_client});
}

void ResetTextInputProbes() {
  first_text_client = std::make_shared<ProbeTextInputClient>();
  second_text_client = std::make_shared<ProbeTextInputClient>();
  text_input_dialog.reset();
  text_input_window.reset();
  keyboard_on_press.reset();
  text_client_visible = State<bool>{};
  use_second_text_client = State<bool>{};
  text_key_events = 0;
  text_activations = 0;
}

void FocusNext(UiWindow& runtime) {
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
    UiWindow runtime{TwoTextClientsApp, platform};
    runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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

TEST_CASE("A hosted composition failure withdraws text input before client cleanup", "[composition-failure]") {
  ResetTextInputProbes();
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  UiWindow window{
      []() -> View {
        text_client_visible = UseState(true);
        if (!text_client_visible.Get()) {
          throw std::runtime_error("editing composition failed");
        }
        return Text("Editable").With(Focusable{}, TextInputProbe{first_text_client});
      },
      platform,
  };
  window.SetWindowMetrics({.viewport = {400.0F, 250.0F}});
  platform.BuildFrameOrErrorPage();
  FocusNext(window);
  REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1});

  bool mounted_during_stop = true;
  auto reentrant_result = TextInputResultCode::Ok;
  text_input.on_stop = [&](TextInputSessionId session_id) {
    mounted_during_stop = detail::InternalAccess::MountedRoot(platform) != nullptr;
    reentrant_result = window.HandleTextInputCommands({.session_id = session_id}).result_code;
  };
  text_client_visible = false;
  const auto& failed = platform.BuildFrameOrErrorPage();
  REQUIRE(failed.render_frame.damage.full);
  REQUIRE(detail::InternalAccess::MountedRoot(platform) != nullptr);
  REQUIRE_FALSE(mounted_during_stop);
  REQUIRE(reentrant_result == TextInputResultCode::SessionMismatch);
  REQUIRE(first_text_client->apply_count == 0);
  REQUIRE(first_text_client->end_count == 1);
  REQUIRE(first_text_client->end_reasons.back() == TextInputEndReason::ClientRemoved);
  REQUIRE(text_input.stopped_sessions == std::vector<TextInputSessionId>{1});
  REQUIRE_FALSE(detail::InternalAccess::FocusedNodeIdentity(platform));
}

TEST_CASE("TestExitingDialogStopsAndCanRestoreTextInput") {
  ResetTextInputProbes();
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;

  UiWindow runtime{DialogTextInputApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();
  REQUIRE(text_input_dialog.has_value());

  const LayerId dialog = text_input_dialog->Show(DialogTextInputContent);
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(first_text_client->begin_count == 1);
  REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1});

  REQUIRE(text_input_dialog->Dismiss(dialog));
  REQUIRE(first_text_client->end_count == 1);
  REQUIRE(first_text_client->end_reasons.back() == TextInputEndReason::FocusLost);
  REQUIRE(text_input.stopped_sessions == std::vector<TextInputSessionId>{1});
  REQUIRE(
      runtime.HandleTextInputCommands(TextInputCommandBatch{.session_id = 1}).result_code ==
      TextInputResultCode::SessionMismatch
  );
  runtime.HandleKeyEvent(KeyEvent{KeyEventType::Down, Key::A});
  REQUIRE(first_text_client->key_count == 0);

  runtime.BuildFrame();
  REQUIRE(first_text_client->begin_count == 1);
  REQUIRE(text_input_dialog->Update(dialog, DialogTextInputContent));
  runtime.BuildFrame();
  REQUIRE(first_text_client->begin_count == 2);
  REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1, 2});
}

TEST_CASE("TestTextInputSessionSurvivesRecompositionAndSynchronizesState") {
  ResetTextInputProbes();
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;

  UiWindow runtime{TwoTextClientsApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();
  FocusNext(runtime);
  REQUIRE(first_text_client->geometry_queries == 1);

  runtime.InvalidateRoot();
  runtime.BuildFrame();
  REQUIRE(first_text_client->begin_count == 1);
  REQUIRE(text_input.started_sessions.size() == 1);
  REQUIRE(text_input.stopped_sessions.empty());
  const int recomposed_geometry_queries = first_text_client->geometry_queries;
  runtime.BuildFrame();
  REQUIRE(first_text_client->geometry_queries == recomposed_geometry_queries);

  first_text_client->SetSelection({2, 2});
  runtime.BuildFrame();
  REQUIRE(text_input.updated_sessions == std::vector<TextInputSessionId>{1});
  REQUIRE(text_input.updated_states.back().selection == TextSelection{2, 2});
  REQUIRE(text_input.updated_geometry.back().result_code == TextInputResultCode::Ok);
  REQUIRE(first_text_client->geometry_queries == recomposed_geometry_queries + 1);

  first_text_client->ChangeAction(TextInputAction::Search);
  runtime.BuildFrame();
  REQUIRE(text_input.restarted_sessions == std::vector<TextInputSessionId>{1});
  REQUIRE(text_input.restarted_configurations.back().action == TextInputAction::Search);
  REQUIRE(text_input.restarted_geometry.back() == text_input.updated_geometry.back());
  REQUIRE(first_text_client->geometry_queries == recomposed_geometry_queries + 1);
}

TEST_CASE("TestTextInputActionValidatesSessionConfigurationAndClientHandling") {
  ResetTextInputProbes();
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;

  UiWindow runtime{TwoTextClientsApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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

  UiWindow runtime{TwoTextClientsApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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

TEST_CASE("TestTextInputRejectsContentChangesWithoutStateRevision") {
  ResetTextInputProbes();
  TestPlatform platform;
  UiWindow runtime{TwoTextClientsApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();
  FocusNext(runtime);

  first_text_client->AdvanceContentRevisionWithoutStateRevision();
  REQUIRE_THROWS_AS(runtime.BuildFrame(), std::logic_error);
}

TEST_CASE("TestTextInputRejectsInvalidGeometry") {
  ResetTextInputProbes();
  first_text_client->invalid_geometry = true;
  TestPlatform platform;
  UiWindow runtime{TwoTextClientsApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  REQUIRE_THROWS_AS(FocusNext(runtime), std::logic_error);
  REQUIRE(first_text_client->begin_count == 1);
  REQUIRE(first_text_client->end_count == 1);
}

TEST_CASE("TestPointerUpdatesSelectionBeforeStartingTextInput") {
  ResetTextInputProbes();
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;

  UiWindow runtime{PointerTextClientApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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
  REQUIRE(text_input.started_geometry.front().caret.x == 20.0F);
}

TEST_CASE("Manual keyboard requests preserve focus, session, and selection") {
  ResetTextInputProbes();
  first_text_client->configuration.keyboard_policy = SoftwareKeyboardPolicy::Manual;
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  UiWindow runtime{PointerTextClientApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  text_input_window->ShowKeyboard();
  REQUIRE(text_input.keyboard_requests.empty());
  runtime.HandlePointerEvent({PointerEventType::Down, 1, {10.0F, 10.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Up, 1, {10.0F, 10.0F}, PointerDeviceKind::Touch});
  REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1});
  platform.AdvanceTime(0.5);
  runtime.HandlePointerEvent({PointerEventType::Down, 2, {10.0F, 10.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Up, 2, {10.0F, 10.0F}, PointerDeviceKind::Touch});
  REQUIRE(text_input.keyboard_requests.empty());

  first_text_client->SetSelection({1, 4});
  runtime.BuildFrame();
  text_input_window->ShowKeyboard();
  text_input_window->HideKeyboard();
  REQUIRE(text_input.keyboard_requests == std::vector<std::pair<TextInputSessionId, bool>>{{1, true}, {1, false}});
  REQUIRE(first_text_client->State().selection == TextSelection{1, 4});
  REQUIRE(first_text_client->State().session_id == 1);
  REQUIRE(text_input.stopped_sessions.empty());

  first_text_client->configuration.keyboard_policy = SoftwareKeyboardPolicy::Automatic;
  runtime.BuildFrame();
  REQUIRE(text_input.restarted_sessions.empty());
  REQUIRE(text_input.keyboard_requests.size() == 2);
  text_input_window->ShowKeyboard();
  REQUIRE(text_input.keyboard_requests.back() == std::pair<TextInputSessionId, bool>{1, true});
}

TEST_CASE("The last editor keyboard request takes priority on the event that starts input") {
  ResetTextInputProbes();
  const bool show = GENERATE(true, false);
  keyboard_on_press = show;
  first_text_client->configuration.keyboard_policy =
      show ? SoftwareKeyboardPolicy::Manual : SoftwareKeyboardPolicy::Automatic;
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  UiWindow runtime{PointerTextClientApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 1, {10.0F, 10.0F}});
  REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1});
  REQUIRE(text_input.keyboard_requests == std::vector<std::pair<TextInputSessionId, bool>>{{1, show}});
}

TEST_CASE("A keyboard request for the previous editor is discarded when focus moves during dispatch") {
  ResetTextInputProbes();
  first_text_client->configuration.keyboard_policy = SoftwareKeyboardPolicy::Manual;
  second_text_client->configuration.keyboard_policy = SoftwareKeyboardPolicy::Manual;
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  UiWindow runtime{KeyboardRequestBeforeFocusMoveApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  FocusNext(runtime);
  FocusNext(runtime);
  FocusNext(runtime);
  REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1, 2, 3});
  REQUIRE(text_input.stopped_sessions == std::vector<TextInputSessionId>{1, 2});
  REQUIRE(text_input.keyboard_requests.empty());
}

TEST_CASE("An editor can request the keyboard when it receives accessibility focus") {
  ResetTextInputProbes();
  const bool replace_session = GENERATE(false, true);
  first_text_client->configuration.keyboard_policy = SoftwareKeyboardPolicy::Manual;
  second_text_client->configuration.keyboard_policy = SoftwareKeyboardPolicy::Manual;
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  UiWindow runtime{[]() -> View {
    return Column {
      Text("First").With(Focusable{}, TextInputProbe{first_text_client}),
      Text("Accessible editor").With(
          Focusable{},
          TextInputProbe{.client = second_text_client, .show_keyboard_on_focus = true},
          Semantics{.role = SemanticRole::TextField, .label = "Accessible editor"}
      ),
    };
  }, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  if (replace_session) {
    runtime.BuildFrame();
    FocusNext(runtime);
    REQUIRE(text_input.started_sessions == std::vector<TextInputSessionId>{1});
  }
  const auto frame = runtime.BuildCommit().semantic_frame;
  const auto editor = std::ranges::find(frame->nodes, "Accessible editor", &SemanticNode::label);
  REQUIRE(editor != frame->nodes.end());

  REQUIRE(runtime.CoreRuntime().PerformSemanticAction(editor->id, {SemanticActionKind::Focus}));
  const TextInputSessionId session = replace_session ? 2 : 1;
  REQUIRE(second_text_client->State().session_id == session);
  REQUIRE(text_input.keyboard_requests == std::vector<std::pair<TextInputSessionId, bool>>{{session, true}});
  runtime.BuildFrame();
  REQUIRE(text_input.keyboard_requests.size() == 1);
}

TEST_CASE("TestTextInputClientReplacementAndRemovalCloseSessions") {
  ResetTextInputProbes();
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;

  UiWindow replacement{ReplaceTextClientApp, platform};
  replacement.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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
  UiWindow removal{RemoveTextClientApp, removal_platform};
  removal.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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

  UiWindow runtime{TextKeyClientApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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

  UiWindow runtime{TextKeyClientApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
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

TEST_CASE("Focused text selection client routes editing actions", "[editing-actions]") {
  editing_action_probe = std::make_shared<EditingActionProbe>();
  auto& client = *editing_action_probe;
  EditingActionClipboard clipboard;
  TestPlatform platform;
  platform.platform_clipboard = &clipboard;
  UiWindow runtime{
      []() -> View {
        editing_action_probe->tasks = UseTaskScope();
        return ProvideEnvironment(TextSelectionMenuLabels{"Cut", "Copy", "Paste", "Select all"},
                                  Text("Editable").With(Focusable{}, TextInputProbe{editing_action_probe}));
      },
      platform,
  };
  runtime.SetWindowMetrics({.viewport = {300.0F, 120.0F}});
  runtime.BuildFrame();
  const auto shortcut = [&](Key key, bool repeat = false) {
    return runtime.HandleKeyEvent({.key = key, .modifiers = {.control = true}, .repeat = repeat});
  };

  SECTION("Keyboard and touch both dispatch to the editor") {
    SECTION("Keyboard") {
      FocusNext(runtime);
      client.SetSelection({0, 2});
      REQUIRE(runtime.CanPerformTextEditingAction(TextEditingAction::Copy));
      REQUIRE(client.actions.empty());
      REQUIRE(shortcut(Key::C));
      REQUIRE(shortcut(Key::C, true));
    }
    SECTION("Touch menu") {
      runtime.HandlePointerEvent({PointerEventType::Down, 1, {12.0F, 12.0F}, PointerDeviceKind::Touch});
      platform.AdvanceTime(0.5);
      const auto* copy = FindText(runtime.BuildFrame(), "Copy");
      REQUIRE(copy);
      const Point center{copy->rect.x + copy->rect.width * 0.5F, copy->rect.y + copy->rect.height * 0.5F};
      REQUIRE(client.actions.empty());
      runtime.HandlePointerEvent({PointerEventType::Down, 2, center, PointerDeviceKind::Touch});
      runtime.HandlePointerEvent({PointerEventType::Up, 2, center, PointerDeviceKind::Touch});
    }
    REQUIRE(client.actions == std::vector<TextEditingAction>{TextEditingAction::Copy});
    REQUIRE_FALSE(client.write_result);
    platform.RunPlatformModuleTasks();
    REQUIRE(client.write_result == std::optional{false});
    REQUIRE_FALSE(clipboard.written_text);
  }

  SECTION("Explicit default and disabled actions") {
    FocusNext(runtime);
    client.SetSelection({0, 2});
    client.status = TextEditingActionStatus::Disabled;
    REQUIRE_FALSE(runtime.CanPerformTextEditingAction(TextEditingAction::Copy));
    REQUIRE(shortcut(Key::C));
    REQUIRE_FALSE(clipboard.written_text);

    client.status = TextEditingActionStatus::UseDefault;
    REQUIRE(runtime.CanPerformTextEditingAction(TextEditingAction::Copy));
    REQUIRE(shortcut(Key::C));
    REQUIRE(clipboard.written_text == "ab");
    REQUIRE(client.actions.empty());
  }

  SECTION("Live input policy limits what the editor can handle") {
    FocusNext(runtime);
    client.configuration.read_only = true;
    client.configuration.secure = true;
    REQUIRE_FALSE(runtime.CanPerformTextEditingAction(TextEditingAction::Paste));
    REQUIRE(shortcut(Key::V));
    REQUIRE(client.state.session_id == 0);
    REQUIRE_FALSE(runtime.CanPerformTextEditingAction(TextEditingAction::Copy));
    REQUIRE_FALSE(runtime.PerformTextEditingAction(TextEditingAction::Cut));
    REQUIRE(shortcut(Key::C));
    REQUIRE(client.actions.empty());
    REQUIRE_FALSE(clipboard.written_text);
  }
}

TEST_CASE("TestFocusableNodeRejectsMultipleTextInputClients") {
  ResetTextInputProbes();
  TestPlatform platform;
  UiWindow runtime{MultipleTextClientsApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  REQUIRE_THROWS_AS(FocusNext(runtime), std::logic_error);
}

namespace {

std::array<std::optional<ViewHandle>, 3> input_focus;
TaskScope input_focus_tasks;
bool input_focus_accepted = false;

View ProgrammaticTextInputApp() {
  const auto first = UseViewHandle();
  const auto second = UseViewHandle();
  input_focus = {first, second, std::nullopt};
  input_focus_tasks = UseTaskScope();
  const auto window = UseWindow();
  text_input_window = window;
  return Column {
    Text("first requested editor").With(Focusable{}, first, TextInputProbe{first_text_client}),
    Text("second requested editor").With(Focusable{}, second, TextInputProbe{second_text_client}),
    Button("edit second").OnClick([second, window] {
      input_focus_accepted = second.RequestFocus();
      if (input_focus_accepted) window.ShowKeyboard();
    }),
  };
}

View InitialDialogFocusApp() {
  for (auto& requester : input_focus) requester = UseViewHandle();
  text_input_dialog = UseDialog();
  return Button("dialog opener").With(*input_focus[0]);
}

View InitialDialogFocusContent() {
  return Column {
    Text("default dialog editor").With(Focusable{}, TextInputProbe{first_text_client}),
    Text("requested dialog editor")
        .With(Focusable{}, *input_focus[1], TextInputProbe{second_text_client}),
    Button("other dialog control").With(*input_focus[2]),
  };
}

bool RequestedSemanticFocus(UiWindow& runtime, std::string_view label) {
  const auto frame = runtime.BuildCommit().semantic_frame;
  const auto found = std::ranges::find(frame->nodes, label, &SemanticNode::label);
  return found != frame->nodes.end() && found->focused;
}

} // namespace

TEST_CASE("Programmatic focus and keyboard presentation work outside input dispatch and after a Task resumes") {
  const bool task_callback = GENERATE(false, true);
  ResetTextInputProbes();
  first_text_client->configuration.keyboard_policy = SoftwareKeyboardPolicy::Manual;
  second_text_client->configuration.keyboard_policy = SoftwareKeyboardPolicy::Manual;
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  UiWindow runtime{ProgrammaticTextInputApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  REQUIRE(input_focus[0]->RequestFocus());
  const auto focus_and_show = [&] {
    REQUIRE(input_focus[1]->RequestFocus());
    text_input_window->ShowKeyboard();
  };
  if (task_callback) {
    std::optional<CallbackCompletion<void>> resume;
    input_focus_tasks.Launch([&]() -> Task<void> {
      co_await AwaitCallback<void>([&](auto completion) {
        resume = std::move(completion);
        return std::function<void()>{};
      });
      focus_and_show();
    });
    platform.RunPlatformModuleTasks();
    REQUIRE(resume);
    resume->Complete();
    platform.RunPlatformModuleTasks();
  } else {
    focus_and_show();
  }
  REQUIRE(first_text_client->begin_count == 1);
  REQUIRE(second_text_client->begin_count == 1);
  REQUIRE(text_input.keyboard_requests == std::vector<std::pair<TextInputSessionId, bool>>{{2, true}});
  REQUIRE(RequestedSemanticFocus(runtime, "second requested editor"));
}

TEST_CASE("One input event can request focus and show the new editor keyboard") {
  ResetTextInputProbes();
  first_text_client->configuration.keyboard_policy = SoftwareKeyboardPolicy::Manual;
  second_text_client->configuration.keyboard_policy = SoftwareKeyboardPolicy::Manual;
  input_focus_accepted = false;
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  UiWindow runtime{ProgrammaticTextInputApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  REQUIRE(input_focus[0]->RequestFocus());
  const auto button = FindPresentedTextRect(runtime.BuildFrame(), "edit second");
  REQUIRE(button);
  ClickAt(runtime, {button->x + button->width * 0.5F, button->y + button->height * 0.5F});
  REQUIRE(input_focus_accepted);
  REQUIRE(second_text_client->begin_count == 1);
  REQUIRE(text_input.keyboard_requests == std::vector<std::pair<TextInputSessionId, bool>>{{2, true}});
}

TEST_CASE("Dialog initial focus skips the default editor and nested dialogs restore the current target") {
  ResetTextInputProbes();
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  UiWindow runtime{InitialDialogFocusApp, platform};
  runtime.SetWindowMetrics({.viewport = {480.0F, 480.0F}});
  runtime.BuildFrame();
  REQUIRE(input_focus[0]->RequestFocus());
  const auto dialog = text_input_dialog->Show(InitialDialogFocusContent, {.initial_focus = input_focus[1]});
  runtime.BuildFrame();
  REQUIRE(first_text_client->begin_count == 0);
  REQUIRE(second_text_client->begin_count == 1);
  REQUIRE(RequestedSemanticFocus(runtime, "requested dialog editor"));
  REQUIRE_FALSE(input_focus[0]->RequestFocus());
  REQUIRE(input_focus[2]->RequestFocus());
  const auto nested = text_input_dialog->Show([] { return Button("nested control"); });
  runtime.BuildFrame();
  REQUIRE(RequestedSemanticFocus(runtime, "nested control"));
  REQUIRE(text_input_dialog->Dismiss(nested));
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(RequestedSemanticFocus(runtime, "other dialog control"));
  REQUIRE(text_input_dialog->Update(dialog, InitialDialogFocusContent));
  runtime.BuildFrame();
  REQUIRE(RequestedSemanticFocus(runtime, "other dialog control"));
  REQUIRE(second_text_client->begin_count == 1);
  REQUIRE(text_input_dialog->Dismiss(dialog));
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(RequestedSemanticFocus(runtime, "dialog opener"));
}

TEST_CASE("An unavailable Dialog initial target falls back once without focusing a later mount") {
  ResetTextInputProbes();
  ProbePlatformTextInput text_input;
  TestPlatform platform;
  platform.platform_text_input = &text_input;
  UiWindow runtime{InitialDialogFocusApp, platform};
  runtime.SetWindowMetrics({.viewport = {480.0F, 480.0F}});
  runtime.BuildFrame();
  text_input_dialog->Show([] {
    text_client_visible = UseState(false);
    return Column {
      Text("fallback editor").With(Focusable{}, TextInputProbe{first_text_client}),
      text_client_visible
          ? Text("late editor").With(Focusable{}, *input_focus[1], TextInputProbe{second_text_client})
          : Text("not mounted"),
    };
  }, {.initial_focus = input_focus[1]});
  runtime.BuildFrame();
  REQUIRE(first_text_client->begin_count == 1);
  REQUIRE(second_text_client->begin_count == 0);
  text_client_visible = true;
  runtime.BuildFrame();
  REQUIRE(first_text_client->begin_count == 1);
  REQUIRE(second_text_client->begin_count == 0);
  REQUIRE(input_focus[1]->RequestFocus());
  REQUIRE(second_text_client->begin_count == 1);
}

} // namespace huxerui::test
