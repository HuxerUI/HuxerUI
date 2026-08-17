#pragma once

#import <UIKit/UIKit.h>

#include <cstdint>
#include <memory>

#include <huxerui/semantics.h>

namespace huxerui {
class Runtime;
}

namespace huxerui::detail {

class UIKitPlatformViews;

class UIKitAccessibility final {
public:
  UIKitAccessibility(Runtime& runtime, UIView* root, UIKitPlatformViews& platform_views, id<UITextInput> text_input);
  ~UIKitAccessibility();

  UIKitAccessibility(const UIKitAccessibility&) = delete;
  UIKitAccessibility& operator=(const UIKitAccessibility&) = delete;

  void Commit(std::shared_ptr<const SemanticFrame> frame, bool platform_views_changed);
  [[nodiscard]] NSArray* Elements() const noexcept;
  void Shutdown();

  [[nodiscard]] bool PerformDefault(SemanticNodeId id);
  [[nodiscard]] bool Perform(SemanticNodeId id, SemanticActionKind action);
  [[nodiscard]] bool PerformCustom(SemanticNodeId id, std::uint64_t action_id);
  [[nodiscard]] bool PerformScroll(SemanticNodeId id, UIAccessibilityScrollDirection direction);
  void AccessibilityFocusChanged(SemanticNodeId id);

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace huxerui::detail
