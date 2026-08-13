#pragma once

#import <AppKit/AppKit.h>

#include <memory>

#include <huxerui/semantics.h>

namespace huxerui {

class Runtime;

namespace detail {

class AppKitPlatformViews;

class MacAccessibility final {
public:
  MacAccessibility(Runtime& runtime, NSView* root_view, AppKitPlatformViews& platform_views) noexcept;
  ~MacAccessibility();

  MacAccessibility(const MacAccessibility&) = delete;
  MacAccessibility& operator=(const MacAccessibility&) = delete;

  void Commit(std::shared_ptr<const SemanticFrame> frame);
  NSArray* RootChildren();

  const SemanticNode* NodeForId(SemanticNodeId id) const noexcept;
  id Element(SemanticNodeId id);
  NSArray* Children(SemanticNodeId id);
  NSRect Frame(SemanticNodeId id) const;
  bool PerformAction(SemanticNodeId id, SemanticAction action);

private:
  Runtime* runtime_;
  __weak NSView* root_view_;
  AppKitPlatformViews* platform_views_;
  std::shared_ptr<const SemanticFrame> frame_;
  __strong NSMutableDictionary* elements_ = nil;
};

} // namespace detail

} // namespace huxerui
