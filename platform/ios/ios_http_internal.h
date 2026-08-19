#pragma once

#include <memory>

namespace huxerui::detail {

class HttpTransport;

std::shared_ptr<HttpTransport> CreateIosHttpTransport();

} // namespace huxerui::detail
