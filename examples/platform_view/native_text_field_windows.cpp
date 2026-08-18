#include "native_text_field.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <huxerui/windows/platform_view.h>

namespace huxerui::example {

namespace {

constexpr wchar_t kNativeTextFieldClassName[] = L"HuxerUI.Example.NativeTextField";

struct NativeTextFieldState {
  HWND edit = nullptr;
  PlatformEventSink events;
  bool applying_properties = false;
};

std::wstring NativeString(std::string_view value) {
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

void ApplyProperties(HWND view, const PlatformPayload& properties) {
  auto* state = reinterpret_cast<NativeTextFieldState*>(GetWindowLongPtrW(view, GWLP_USERDATA));
  if (state == nullptr || state->edit == nullptr) {
    return;
  }
  const std::wstring text = NativeString(properties.AsObject().at(native_text_field::text_property).AsString());
  const int current_length = GetWindowTextLengthW(state->edit);
  std::wstring current(static_cast<std::size_t>(std::max(current_length, 0)) + 1, L'\0');
  if (current_length > 0) {
    GetWindowTextW(state->edit, current.data(), current_length + 1);
  }
  current.resize(static_cast<std::size_t>(std::max(current_length, 0)));
  if (current != text) {
    state->applying_properties = true;
    SetWindowTextW(state->edit, text.c_str());
    state->applying_properties = false;
  }
}

LRESULT CALLBACK NativeTextFieldProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
  auto* state = reinterpret_cast<NativeTextFieldState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
    state = static_cast<NativeTextFieldState*>(create->lpCreateParams);
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
    if (state != nullptr && !state->applying_properties && HIWORD(w_param) == EN_CHANGE && state->events) {
      state->events(NativeTextFieldEvents::Changed::Name, PlatformPayload(Utf8Text(state->edit)));
    }
    return 0;
  default:
    return DefWindowProcW(window, message, w_param, l_param);
  }
}

void RegisterNativeTextFieldClass() {
  WNDCLASSEXW window_class{
      sizeof(WNDCLASSEXW),
      0,
      NativeTextFieldProcedure,
      0,
      0,
      GetModuleHandleW(nullptr),
      nullptr,
      LoadCursor(nullptr, IDC_IBEAM),
      nullptr,
      nullptr,
      kNativeTextFieldClassName,
      nullptr,
  };
  if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    throw std::runtime_error("HuxerUI example could not register its native text-field class");
  }
}

HWND CreateNativeTextField(HWND parent, const PlatformPayload& properties, PlatformEventSink event_sink) {
  RegisterNativeTextFieldClass();
  auto state = std::make_unique<NativeTextFieldState>();
  state->events = std::move(event_sink);
  HWND view = CreateWindowExW(
      0,
      kNativeTextFieldClassName,
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
    return nullptr;
  }
  state.release();
  ApplyProperties(view, properties);
  return view;
}

void UpdateNativeTextField(HWND view, const PlatformPayload& properties) {
  ApplyProperties(view, properties);
}

void DisposeNativeTextField(HWND view) {
  auto* state = reinterpret_cast<NativeTextFieldState*>(GetWindowLongPtrW(view, GWLP_USERDATA));
  SetWindowLongPtrW(view, GWLP_USERDATA, 0);
  delete state;
}

windows::PlatformViewFactory NativeTextFieldFactory() {
  return {
      .create = CreateNativeTextField,
      .update = UpdateNativeTextField,
      .dispose = DisposeNativeTextField,
  };
}

} // namespace

void InstallNativeTextField(RootContext& root) {
  root.Modules().Register(native_text_field::type, NativeTextFieldFactory());
}

} // namespace huxerui::example
