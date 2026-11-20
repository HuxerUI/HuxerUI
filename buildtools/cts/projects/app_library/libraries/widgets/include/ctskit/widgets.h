#pragma once

#include <string>

#include <huxerui/huxerui.h>

namespace ctskit {

/// Displays the widgets library's caption resource.
huxerui::View Caption();

/// Resolves the widgets library's own string resource in the caller's composition scope.
std::string UseWidgetsCaption();

/// Resolves the string of the core library that the widgets library depends on.
std::string UseWidgetsCoreName();

} // namespace ctskit
