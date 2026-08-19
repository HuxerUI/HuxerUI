#pragma once

#include <memory>

namespace huxerui::detail {

class HttpTransport;

std::shared_ptr<HttpTransport> CreateWin32HttpTransport();

} // namespace huxerui::detail
