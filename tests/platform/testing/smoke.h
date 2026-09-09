#pragma once

#include <string>

// Carriers supply a deployed package root and translate this diagnostic at their runner boundary.
std::string RunUiTestingSmoke(const std::string& package_root) noexcept;
