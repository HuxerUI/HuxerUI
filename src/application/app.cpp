#include <huxerui/app.h>
#include <huxerui/gesture.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "graphics/external_texture_internal.h"
#include "platform_registry_internal.h"
#include "system_tray_internal.h"
#include "text/text_internal.h"

namespace huxerui {

namespace {

std::vector<const Application*>& Applications() {
  static std::vector<const Application*> applications;
  return applications;
}

} // namespace

PlatformAdapter::PlatformAdapter(UIThreadDispatcher dispatch_to_ui_thread)
    : ui_thread_dispatcher_(std::move(dispatch_to_ui_thread)),
      external_texture_frame_requester_(
          std::make_shared<detail::ExternalTextureFrameRequester>(*this, ui_thread_dispatcher_)
      ),
      platform_registry_(std::make_unique<detail::PlatformRegistry>(*this)) {}

PlatformAdapter::~PlatformAdapter() {
  external_texture_frame_requester_->Close();
}

namespace detail {

PlatformChannelEndpoint MakePlatformChannelEndpoint(PlatformAdapter& adapter) {
  return MakePlatformChannelEndpoint(adapter.ui_thread_dispatcher_);
}

} // namespace detail

detail::PlatformRegistry& PlatformAdapter::PlatformRegistry() noexcept {
  return *platform_registry_;
}

void PlatformAdapter::DispatchToUIThread(std::function<void()> task) const {
  if (!task) {
    throw std::invalid_argument("HuxerUI UI-thread task must not be empty");
  }
  ui_thread_dispatcher_(std::move(task));
}

GestureSettings PlatformAdapter::GestureDefaults() const noexcept {
  return {};
}

ScrollPhysics PlatformAdapter::ScrollDefaults() const noexcept {
  return {};
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

std::shared_ptr<detail::PermissionTransport> PlatformAdapter::CreatePermissionTransport() {
  return {};
}

std::shared_ptr<detail::SystemTrayTransport> PlatformAdapter::CreateSystemTrayTransport() {
  return {};
}

std::unique_ptr<detail::TextLayout> PlatformAdapter::CreateTextLayout(std::string_view text, const TextStyle& style,
    float max_width, const TextLayoutOptions& options) {
  return CreateTextLayout(AttributedText(std::string(text)), style, max_width, options);
}

std::unique_ptr<detail::TextLayout> PlatformAdapter::CreateTextLayout(const AttributedText& text,
    const TextStyle& style, float max_width, const TextLayoutOptions& options) {
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
