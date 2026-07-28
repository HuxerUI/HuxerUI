#pragma once

#include <huxerui/view.h>

#define HUXERUI_SCOPE(...) \
  return ::huxerui::Scope([=]() -> ::huxerui::View __VA_ARGS__)

#define HUXERUI_SCOPE_BEGIN \
  return ::huxerui::Scope([=]() -> ::huxerui::View {

#define HUXERUI_SCOPE_END \
  });
