#pragma once

#include <memory>

#include <huxerui/clipboard.h>

namespace huxerui::detail {

class PermissionTransport;

[[nodiscard]] std::shared_ptr<PermissionTransport> CreateWebPermissionTransport();
Task<bool> WriteWebClipboardAsync(const ClipboardItem& item);
Task<IoResult<ClipboardItem>> ReadWebClipboardAsync(const ClipboardReadOptions& options);

} // namespace huxerui::detail
