#include <huxerui/app.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "external_texture_internal.h"
#include "text_layout_internal.h"

namespace huxerui {

namespace {

std::vector<const Application*>& Applications() {
  static std::vector<const Application*> applications;
  return applications;
}

} // namespace

PlatformAdapter::PlatformAdapter(UIThreadDispatcher dispatch_to_ui_thread)
    : ui_thread_dispatcher_(std::move(dispatch_to_ui_thread)),
      external_texture_surface_(std::make_shared<detail::ExternalTextureSurface>(*this, ui_thread_dispatcher_)),
      platform_modules_(new PlatformModules(*this, ui_thread_dispatcher_)) {}

PlatformAdapter::~PlatformAdapter() {
  external_texture_surface_->Close();
}

std::shared_ptr<FileSystem> PlatformAdapter::CreateFileSystem() {
  return {};
}

std::shared_ptr<detail::FilePickerTransport> PlatformAdapter::CreateFilePickerTransport() {
  return {};
}

std::shared_ptr<detail::HttpTransport> PlatformAdapter::CreateHttpTransport() {
  return {};
}

PlatformModuleFactory::Instance PlatformAdapter::CreatePlatformModule(
    std::string_view type, const PlatformPayload& options, PlatformEventSink events
) {
  const PlatformModuleFactory* factory = FindPlatformModuleRegistration<PlatformModuleFactory>(type);
  if (factory == nullptr) {
    throw std::logic_error("HuxerUI platform module registration has an incompatible type");
  }
  if (!factory->create) {
    throw std::logic_error("HuxerUI platform module factory must provide create");
  }
  return factory->create(options, std::move(events));
}

std::unique_ptr<detail::TextLayout> PlatformAdapter::CreateTextLayout(
    std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
) {
  static_cast<void>(text);
  static_cast<void>(style);
  static_cast<void>(max_width);
  static_cast<void>(options);
  return {};
}

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
