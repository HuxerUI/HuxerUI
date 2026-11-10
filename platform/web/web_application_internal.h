#pragma once

#include <memory>

#include <huxerui/clipboard.h>

namespace huxerui::detail {

class PermissionTransport;

[[nodiscard]] std::shared_ptr<PermissionTransport> CreateWebPermissionTransport();
Task<bool> WriteWebClipboardAsync(const ClipboardContent& content);
Task<IoResult<ClipboardContent>> ReadWebClipboardAsync(ClipboardKind kind);

} // namespace huxerui::detail
