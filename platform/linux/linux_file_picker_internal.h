#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <huxerui/file.h>

namespace huxerui::detail {

class FilePickerTransport;

[[nodiscard]] std::optional<FileReference>
MakeLinuxFileReference(const File& file, bool directory = false, bool writable = true);

[[nodiscard]] std::string LinuxPortalParentWindow(unsigned long window);
[[nodiscard]] std::shared_ptr<FilePickerTransport> CreateLinuxFilePickerTransport(
    std::function<unsigned long()> window_provider, std::optional<std::string> bus_address = std::nullopt
);

} // namespace huxerui::detail
