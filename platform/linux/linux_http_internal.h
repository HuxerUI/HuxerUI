#pragma once

#include <memory>

namespace huxerui::detail {

class HttpTransport;

std::shared_ptr<HttpTransport> CreateLinuxHttpTransport();

} // namespace huxerui::detail
