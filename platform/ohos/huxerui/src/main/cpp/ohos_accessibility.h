#pragma once

#include <arkui/native_node.h>

#include <memory>

namespace huxerui {

struct SemanticFrame;
class UiWindow;

namespace detail {

class OhosTextInput;

// Owns the XComponent's accessibility provider on the ArkUI thread. The window and text input outlive the provider.
class OhosAccessibility final {
public:
  OhosAccessibility(UiWindow& window, OhosTextInput& text_input);
  ~OhosAccessibility();

  // Registers the provider after XComponent initialization.
  bool Attach(ArkUI_NodeHandle node);
  // Retains the committed snapshot for provider queries. Scale converts logical coordinates to local pixels.
  void Commit(std::shared_ptr<const SemanticFrame> frame, float scale);
  // Invalidates queued provider callbacks. Call before retiring the window or destroying the XComponent.
  void Detach() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace detail
} // namespace huxerui
