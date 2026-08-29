#include "platform_text_field.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <huxerui/windows/platform_registry.h>

namespace huxerui::example {

namespace {

constexpr wchar_t kPlatformTextFieldClassName[] = L"HuxerUI.Example.PlatformTextField";

struct PlatformTextFieldState {
  HWND view = nullptr;
  HWND edit = nullptr;
  PlatformEventEmitter events;
  bool applying_properties = false;
};

std::wstring WideStringFromUtf8(std::string_view value) {
  if (value.empty()) {
    return {};
  }
  const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  std::wstring result(static_cast<std::size_t>(std::max(length, 0)), L'\0');
  if (length > 0) {
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
  }
  return result;
}

std::string Utf8Text(HWND edit) {
  const int length = GetWindowTextLengthW(edit);
  std::wstring value(static_cast<std::size_t>(std::max(length, 0)) + 1, L'\0');
  if (length > 0) {
    GetWindowTextW(edit, value.data(), length + 1);
  }
  value.resize(static_cast<std::size_t>(std::max(length, 0)));
  if (value.empty()) {
    return {};
  }
  const int utf8_length =
      WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(std::max(utf8_length, 0)), '\0');
  if (utf8_length > 0) {
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        utf8_length,
        nullptr,
        nullptr
    );
  }
  return result;
}

void ApplyProperties(PlatformTextFieldState& state, const PlatformTextFieldProperties& properties) {
  if (state.edit == nullptr) {
    return;
  }
  const std::wstring text = WideStringFromUtf8(properties.text);
  const int current_length = GetWindowTextLengthW(state.edit);
  std::wstring current(static_cast<std::size_t>(std::max(current_length, 0)) + 1, L'\0');
  if (current_length > 0) {
    GetWindowTextW(state.edit, current.data(), current_length + 1);
  }
  current.resize(static_cast<std::size_t>(std::max(current_length, 0)));
  if (current != text) {
    state.applying_properties = true;
    SetWindowTextW(state.edit, text.c_str());
    state.applying_properties = false;
  }
}

LRESULT CALLBACK PlatformTextFieldProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
  auto* state = reinterpret_cast<PlatformTextFieldState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
    state = static_cast<PlatformTextFieldState*>(create->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  switch (message) {
  case WM_CREATE:
    state->edit = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0,
        0,
        0,
        0,
        window,
        reinterpret_cast<HMENU>(1),
        GetModuleHandleW(nullptr),
        nullptr
    );
    return state->edit == nullptr ? -1 : 0;
  case WM_SIZE:
    if (state != nullptr && state->edit != nullptr) {
      MoveWindow(state->edit, 0, 0, LOWORD(l_param), HIWORD(l_param), TRUE);
    }
    return 0;
  case WM_SETFOCUS:
    if (state != nullptr && state->edit != nullptr) {
      SetFocus(state->edit);
    }
    return 0;
  case WM_COMMAND:
    if (state != nullptr && !state->applying_properties && HIWORD(w_param) == EN_CHANGE) {
      state->events.Emit<PlatformTextFieldEvents::Changed>(Utf8Text(state->edit));
    }
    return 0;
  default:
    return DefWindowProcW(window, message, w_param, l_param);
  }
}

void RegisterPlatformTextFieldClass() {
  WNDCLASSEXW window_class{
      sizeof(WNDCLASSEXW),
      0,
      PlatformTextFieldProcedure,
      0,
      0,
      GetModuleHandleW(nullptr),
      nullptr,
      LoadCursor(nullptr, IDC_IBEAM),
      nullptr,
      nullptr,
      kPlatformTextFieldClassName,
      nullptr,
  };
  if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    throw std::runtime_error("HuxerUI example could not register its Win32 PlatformTextField window class");
  }
}

std::shared_ptr<PlatformTextFieldState>
CreatePlatformTextField(HWND parent, const PlatformTextFieldProperties& properties, PlatformEventEmitter events) {
  RegisterPlatformTextFieldClass();
  auto state = std::make_shared<PlatformTextFieldState>();
  state->events = std::move(events);
  HWND view = CreateWindowExW(
      0,
      kPlatformTextFieldClassName,
      L"",
      WS_CHILD | WS_TABSTOP | WS_CLIPCHILDREN,
      0,
      0,
      0,
      0,
      parent,
      nullptr,
      GetModuleHandleW(nullptr),
      state.get()
  );
  if (view == nullptr) {
    return {};
  }
  state->view = view;
  ApplyProperties(*state, properties);
  return state;
}

void UpdatePlatformTextField(PlatformTextFieldState& state, const PlatformTextFieldProperties& properties) {
  ApplyProperties(state, properties);
}

void DisposePlatformTextField(PlatformTextFieldState& state) {
  SetWindowLongPtrW(state.view, GWLP_USERDATA, 0);
  state.events = {};
}

windows::PlatformViewFactory<PlatformTextFieldProperties, PlatformTextFieldState> PlatformTextFieldFactory() {
  return {
      .create = CreatePlatformTextField,
      .view = [](const std::shared_ptr<PlatformTextFieldState>& state) { return state->view; },
      .update = UpdatePlatformTextField,
      .dispose = DisposePlatformTextField,
  };
}

} // namespace

void InstallPlatformTextField(RootContext& root) {
  root.RegisterPlatformView<PlatformTextFieldProperties>(platform_text_field::type, PlatformTextFieldFactory());
}

} // namespace huxerui::example
