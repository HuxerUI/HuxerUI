#include <huxerui/web/navigation.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <emscripten/emscripten.h>

namespace huxerui::web::detail {

namespace {

BrowserNavigationCoordinator* active_coordinator = nullptr;

// clang-format off
EM_JS(char*, CurrentLocation, (), {
  const location = window.location.pathname + window.location.search + window.location.hash;
  const size = lengthBytesUTF8(location) + 1;
  const result = _malloc(size);
  stringToUTF8(location, result, size);
  return result;
});

EM_JS(void, InstallLocationListener, (std::uintptr_t handle), {
  const listener = () => Module._huxerui_web_browser_navigation_location_changed(handle);
  Module.huxeruiBrowserNavigationListener = {handle, listener};
  window.addEventListener('popstate', listener);
  window.addEventListener('hashchange', listener);
});

EM_JS(void, RemoveLocationListener, (std::uintptr_t handle), {
  const entry = Module.huxeruiBrowserNavigationListener;
  if (entry?.handle === handle) {
    window.removeEventListener('popstate', entry.listener);
    window.removeEventListener('hashchange', entry.listener);
    delete Module.huxeruiBrowserNavigationListener;
  }
});

EM_JS(void, PushLocation, (const char* location, const char* parent), {
  const state = {
    ...(history.state && typeof history.state === 'object' ? history.state : {}),
    __huxeruiNavigationEntry: true,
    __huxeruiNavigationParent: UTF8ToString(parent),
  };
  history.pushState(state, String(), UTF8ToString(location));
});

EM_JS(void, ReplaceLocation, (const char* location), {
  const previous = history.state && typeof history.state === 'object' ? history.state : {};
  const state = {...previous, __huxeruiNavigationEntry: true};
  if (!previous.__huxeruiNavigationEntry) {
    delete state.__huxeruiNavigationParent;
  }
  history.replaceState(state, String(), UTF8ToString(location));
});

EM_JS(bool, CanReturnToLocation, (const char* location), {
  const state = history.state;
  return Boolean(
    state?.__huxeruiNavigationEntry && state.__huxeruiNavigationParent === UTF8ToString(location)
  );
});

EM_JS(bool, SameLocation, (const char* first, const char* second), {
  const normalize = value => {
    const url = new URL(UTF8ToString(value), window.location.href);
    return url.pathname + url.search + url.hash;
  };
  return normalize(first) === normalize(second);
});

EM_JS(void, ReturnToPreviousLocation, (), { history.back(); });
// clang-format on

std::string ReadCurrentLocation() {
  std::unique_ptr<char, decltype(&std::free)> location(CurrentLocation(), &std::free);
  return location ? std::string(location.get()) : std::string{};
}

} // namespace

class BrowserNavigationCoordinator::Implementation final {
public:
  explicit Implementation(BrowserNavigationCoordinator& owner) : owner_(owner) {}

  ~Implementation() {
    if (!initialized_) {
      return;
    }
    RemoveLocationListener(reinterpret_cast<std::uintptr_t>(&owner_));
    if (active_coordinator == &owner_) {
      active_coordinator = nullptr;
    }
  }

  void Update(BrowserNavigationBinding binding) {
    binding_ = std::move(binding);
    if (!binding_.apply_location) {
      throw std::invalid_argument("HuxerUI browser navigation location decoder must not be empty");
    }
    if (!initialized_) {
      Initialize();
      return;
    }
    if (binding_.encoded_path != committed_location_) {
      ReplaceLocation(binding_.encoded_path.c_str());
      committed_location_ = binding_.encoded_path;
    }
  }

  void Commit(huxerui::detail::NavigationHistoryAction action, std::string location) {
    switch (action) {
    case huxerui::detail::NavigationHistoryAction::Push:
      PushLocation(location.c_str(), committed_location_.c_str());
      break;
    case huxerui::detail::NavigationHistoryAction::Pop:
      if (CanReturnToLocation(location.c_str())) {
        committed_location_ = std::move(location);
        ReturnToPreviousLocation();
        return;
      }
      ReplaceLocation(location.c_str());
      break;
    case huxerui::detail::NavigationHistoryAction::Replace:
      ReplaceLocation(location.c_str());
      break;
    }
    committed_location_ = std::move(location);
  }

  void LocationChanged() {
    const std::string location = ReadCurrentLocation();
    if (SameLocation(location.c_str(), committed_location_.c_str())) {
      return;
    }
    const std::optional<std::string> canonical = binding_.apply_location(location);
    if (!canonical) {
      ReplaceLocation(committed_location_.c_str());
      return;
    }
    if (!SameLocation(canonical->c_str(), location.c_str())) {
      ReplaceLocation(canonical->c_str());
    }
    committed_location_ = *canonical;
  }

private:
  void Initialize() {
    if (active_coordinator && active_coordinator != &owner_) {
      throw std::logic_error("HuxerUI supports one active BrowserNavigationStack per browser document");
    }

    const std::string location = ReadCurrentLocation();
    const std::optional<std::string> canonical = binding_.apply_location(location);
    std::string committed_location = canonical.value_or(binding_.encoded_path);
    if (!canonical || !SameLocation(committed_location.c_str(), location.c_str())) {
      ReplaceLocation(committed_location.c_str());
    }

    InstallLocationListener(reinterpret_cast<std::uintptr_t>(&owner_));
    active_coordinator = &owner_;
    initialized_ = true;
    committed_location_ = std::move(committed_location);
  }

  BrowserNavigationCoordinator& owner_;
  BrowserNavigationBinding binding_;
  std::string committed_location_;
  bool initialized_ = false;
};

BrowserNavigationCoordinator::BrowserNavigationCoordinator()
    : implementation_(std::make_unique<Implementation>(*this)) {}

BrowserNavigationCoordinator::~BrowserNavigationCoordinator() = default;

void BrowserNavigationCoordinator::Update(BrowserNavigationBinding binding) {
  implementation_->Update(std::move(binding));
}

void BrowserNavigationCoordinator::Commit(huxerui::detail::NavigationHistoryAction action, std::string location) {
  implementation_->Commit(action, std::move(location));
}

void BrowserNavigationCoordinator::LocationChanged() {
  implementation_->LocationChanged();
}

} // namespace huxerui::web::detail

extern "C" EMSCRIPTEN_KEEPALIVE void huxerui_web_browser_navigation_location_changed(std::uintptr_t handle) {
  auto* coordinator = reinterpret_cast<huxerui::web::detail::BrowserNavigationCoordinator*>(handle);
  if (coordinator) {
    coordinator->LocationChanged();
  }
}
