#include <huxerui/app.h>

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace huxerui {

namespace {

std::vector<const Application*>& Applications() {
  static std::vector<const Application*> applications;
  return applications;
}

} // namespace

namespace detail {

#if defined(__EMSCRIPTEN__)
void EnsureWebPlatformLinked();
#endif

const Application& CurrentApplication() {
  const auto& applications = Applications();
  if (applications.empty()) {
    throw std::logic_error("HuxerUI application has not been declared");
  }
  if (applications.size() != 1) {
    throw std::logic_error("HuxerUI application declaration is not unique");
  }
  return *applications.front();
}

} // namespace detail

Application::Application(RootFactory root_factory, AppOptions options)
    : root_factory(root_factory), options(std::move(options)) {
  if (root_factory == nullptr) {
    throw std::invalid_argument("HuxerUI application requires a root factory");
  }
#if defined(__EMSCRIPTEN__)
  detail::EnsureWebPlatformLinked();
#endif
  Applications().push_back(this);
}

Application::~Application() {
  auto& applications = Applications();
  const auto found = std::find(applications.begin(), applications.end(), this);
  if (found != applications.end()) {
    applications.erase(found);
  }
}

int RunApplication() {
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
  throw std::runtime_error("RunApplication() is not available on Android or Web");
#else
  return detail::RunPlatformApplication(detail::CurrentApplication());
#endif
}

} // namespace huxerui
