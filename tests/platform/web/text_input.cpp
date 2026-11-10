#include "runtime_test_support.h"

#include <memory>
#include <string>
#include <utility>

#include <emscripten.h>
#include <emscripten/val.h>

#include "text/text_input_internal.h"
#include "web_text_input.h"

namespace huxerui::test {
namespace {

class BoundedEditor final : public TextInputClient {
public:
  BoundedEditor() {
    editing_.value = TextEditingValue::FromText(std::string(100, 'a') + "A中🙂B" + std::string(15, 'b'));
    editing_.value.selection = {101, 104};
  }

  const TextEditingValue& Value() const { return editing_.value; }
  TextInputConfiguration Configuration() const override {
    return {.multiline = true, .keyboard_policy = SoftwareKeyboardPolicy::Manual};
  }
  TextInputState State() const override { return state_; }
  TextInputState BeginTextInput(TextInputSessionId session_id) override {
    state_.session_id = session_id;
    state_.revision = 1;
    state_.selection = editing_.value.selection;
    return state_;
  }

  TextInputApplyResult ApplyTextInput(const TextInputCommandBatch& batch) override {
    auto result = detail::ReduceTextInputCommands(editing_, batch.commands);
    if (result.status != detail::TextInputReductionStatus::Accepted) {
      return {TextInputResultCode::Rejected, TextInputSyncAction::None};
    }
    if (result.state.value.text != editing_.value.text) ++state_.content_revision;
    if (result.changed) ++state_.revision;
    editing_ = std::move(result.state);
    state_.selection = editing_.value.selection;
    state_.composition = editing_.value.composition;
    return {TextInputResultCode::Ok, TextInputSyncAction::Update};
  }

  TextInputContext QueryTextInputContext(TextInputSessionId session_id, TextOffset, TextOffset length) const override {
    const auto total = detail::Utf16Length(editing_.value.text).value();
    const TextRange slice = length == 0 ? TextRange{0, 0} : TextRange{100, total - 15};
    return {TextInputResultCode::Ok, session_id, slice.start, total,
            detail::Utf8TextInRange(editing_.value.text, slice).value(), state_.selection, state_.composition};
  }

  TextInputGeometry QueryTextInputGeometry(TextInputSessionId session_id, TextRange) const override {
    return {TextInputResultCode::Ok, session_id, {10.0F, 0.0F, 1.0F, 20.0F}, {}};
  }
  TextInputPositionResult QueryTextInputPosition(TextInputSessionId session_id, Point) const override {
    return {TextInputResultCode::Rejected, session_id, {}};
  }
  TextInputKeyResult HandleTextKey(const KeyEvent&) override { return TextInputKeyResult::Unhandled; }
  void EndTextInput(TextInputSessionId, TextInputEndReason) override { state_.session_id = 0; }

private:
  detail::TextFieldEditingState editing_;
  TextInputState state_;
};

std::weak_ptr<BoundedEditor> bounded_editor;

struct BoundedEditorInput {
  struct Extension final : NodeExtension {
    Extension(ViewNode&, const BoundedEditorInput&) {}
    void Update(ViewNode&, const BoundedEditorInput&) {}
    std::shared_ptr<TextInputClient> GetTextInputClient() noexcept override { return bounded_editor.lock(); }
  };
};

// clang-format off
EM_JS(void, InstallTextInputTestDom, (), {
  Module.textInputTestSaved = { document: globalThis.document, sessions: Module.huxerUIWebSessions };
  Module.huxerUIWebSessions = new Map([[1, {
    root: { getBoundingClientRect() { return { left: 0, top: 0 }; } },
    listeners: [],
  }]]);
  globalThis.document = {
    body: { appendChild(element) { this.input = element; } },
    createElement(tag) {
      const listeners = new Map();
      const breaks = new RegExp(tag === "textarea" ? "\\r\\n?" : "[\\r\\n]", "g");
      let value = "";
      return {
        style: {},
        get value() { return value; },
        set value(text) { value = text.replace(breaks, tag === "textarea" ? "\n" : ""); },
        setAttribute() {},
        blur() {},
        addEventListener(type, callback) { listeners.set(type, callback); },
        dispatch(type, data) { listeners.get(type)({ data }); },
        setSelectionRange(start, end, direction = "forward") {
          this.selectionStart = Math.min(start, this.value.length);
          this.selectionEnd = Math.min(end, this.value.length);
          this.selectionDirection = direction;
        },
      };
    },
  };
});
// clang-format on

TEST_CASE("Web text input maps a bounded UTF-16 context through replacement and composition") {
  InstallTextInputTestDom();
  struct RestoreDom {
    ~RestoreDom() {
      EM_ASM({
        globalThis.document = Module.textInputTestSaved.document;
        Module.huxerUIWebSessions = Module.textInputTestSaved.sessions;
        delete Module.textInputTestSaved;
      });
    }
  } restore_dom;

  const auto editor = std::make_shared<BoundedEditor>();
  bounded_editor = editor;
  detail::WebTextInput input(1);
  TestPlatform platform;
  platform.platform_text_input = &input;
  input.SetUiWindow(&platform);
  UiWindow runtime([]() -> View { return Text("Editor").With(Focusable{}, BoundedEditorInput{}); }, platform);
  runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
  runtime.BuildFrame();
  REQUIRE(runtime.HandleKeyEvent({.key = Key::Tab}));

  auto proxy = emscripten::val::global("document")["body"]["input"];
  REQUIRE_FALSE(proxy["readOnly"].as<bool>());
  CHECK(proxy["value"].as<std::string>() == "A中🙂B");
  CHECK(proxy["selectionStart"].as<int>() == 1);
  CHECK(proxy["selectionEnd"].as<int>() == 4);

  proxy.set("value", std::string("AZB"));
  proxy.call<void>("setSelectionRange", 2, 2);
  proxy.call<void>("dispatch", std::string("input"));
  CHECK(editor->Value().text == std::string(100, 'a') + "AZB" + std::string(15, 'b'));
  CHECK(editor->Value().selection == TextSelection{102, 102});

  proxy.call<void>("setSelectionRange", 1, 2, std::string("backward"));
  proxy.call<void>("dispatch", std::string("select"));
  CHECK(editor->Value().selection == TextSelection{102, 101});

  proxy.call<void>("dispatch", std::string("compositionstart"));
  CHECK(editor->Value().composition == TextRange{101, 102});
  proxy.call<void>("dispatch", std::string("compositionupdate"), std::string("中🙂"));
  CHECK(editor->Value().composition == TextRange{101, 104});
  CHECK(editor->Value().selection == TextSelection{104, 104});
  CHECK(proxy["value"].as<std::string>() == "AZB");
  proxy.call<void>("dispatch", std::string("compositionend"), std::string("中🙂"));
  CHECK_FALSE(editor->Value().composition.has_value());
  CHECK(editor->Value().text == std::string(100, 'a') + "A中🙂B" + std::string(15, 'b'));
  CHECK(proxy["value"].as<std::string>() == "A中🙂B");
  CHECK(proxy["selectionStart"].as<int>() == 4);

  TextInputCommand replace;
  replace.kind = TextInputCommandKind::CommitText;
  replace.target = TextRange{100, 105};
  replace.text = "A\r\nBC\r\nD";
  replace.selection_after = TextSelection{103, 104};
  REQUIRE(runtime.HandleTextInputCommands({editor->State().session_id, {std::move(replace)}}).result_code ==
          TextInputResultCode::Ok);
  CHECK(proxy["value"].as<std::string>() == "A\nBC\nD");
  CHECK(proxy["selectionStart"].as<int>() == 2);
  CHECK(proxy["selectionEnd"].as<int>() == 3);

  proxy.set("value", std::string("A\nC\nD"));
  proxy.call<void>("setSelectionRange", 5, 5);
  proxy.call<void>("dispatch", std::string("input"));
  CHECK(editor->Value().text == std::string(100, 'a') + "A\r\nC\r\nD" + std::string(15, 'b'));
  CHECK(editor->Value().selection == TextSelection{107, 107});

  proxy.call<void>("setSelectionRange", 2, 3, std::string("backward"));
  proxy.call<void>("dispatch", std::string("select"));
  CHECK(editor->Value().selection == TextSelection{104, 103});
  proxy.call<void>("dispatch", std::string("compositionstart"));
  CHECK(editor->Value().composition == TextRange{103, 104});
}

} // namespace
} // namespace huxerui::test
