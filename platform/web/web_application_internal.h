#pragma once

#include <memory>

namespace huxerui::detail {

class PermissionTransport;

[[nodiscard]] std::shared_ptr<PermissionTransport> CreateWebPermissionTransport();

} // namespace huxerui::detail
