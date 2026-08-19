#pragma once

#include <memory>

namespace huxerui::detail {

class HttpTransport;

std::shared_ptr<HttpTransport> CreateWebHttpTransport();

} // namespace huxerui::detail
