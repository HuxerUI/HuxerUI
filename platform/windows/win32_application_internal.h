#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/app.h>

namespace huxerui::detail {

class PermissionTransport;

inline constexpr std::uintptr_t win32_application_activation_data_id = 0x48555841U;
inline constexpr std::size_t win32_application_activation_max_characters = 32768;

struct Win32StartupInput {
  std::vector<std::wstring> arguments;
  ApplicationActivation activation;
};

[[nodiscard]] ApplicationActivation ParseWin32ApplicationActivation(std::span<const std::wstring> arguments);
[[nodiscard]] std::vector<wchar_t> EncodeWin32ApplicationArguments(std::span<const std::wstring> arguments);
[[nodiscard]] std::optional<ApplicationActivation> DecodeWin32ApplicationActivation(
    std::span<const wchar_t> payload
) noexcept;
[[nodiscard]] Win32StartupInput CurrentWin32StartupInput();
[[nodiscard]] std::wstring Win32ApplicationWindowClassName();
[[nodiscard]] bool TryForwardWin32ApplicationActivation(
    std::wstring_view window_class_name,
    std::span<const std::wstring> arguments
);
[[nodiscard]] std::shared_ptr<PermissionTransport> CreateWin32PermissionTransport();

} // namespace huxerui::detail
