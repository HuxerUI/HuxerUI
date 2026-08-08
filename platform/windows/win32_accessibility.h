#pragma once

#include <windows.h>
#include <objbase.h>
#include <UIAutomation.h>

#include <memory>

#include <huxerui/semantics.h>

namespace huxerui {

class Runtime;

namespace detail {

class Win32SemanticProvider;

class Win32Accessibility final {
public:
  static constexpr UINT action_message = WM_APP + 2;

  Win32Accessibility();
  ~Win32Accessibility();

  Win32Accessibility(const Win32Accessibility&) = delete;
  Win32Accessibility& operator=(const Win32Accessibility&) = delete;

  void SetRuntime(Runtime* runtime) noexcept;
  void SetWindow(HWND window) noexcept;
  void SetDpiScale(float scale) noexcept;
  void Commit(std::shared_ptr<const SemanticFrame> frame);
  void Reset() noexcept;

  LRESULT HandleGetObject(WPARAM w_param, LPARAM l_param);
  LRESULT HandleActionMessage(LPARAM l_param);

  HRESULT ProviderForNode(SemanticNodeId id, IRawElementProviderSimple** provider);

private:
  struct State;
  std::shared_ptr<State> state_;

  friend class Win32SemanticProvider;
};

} // namespace detail

} // namespace huxerui
