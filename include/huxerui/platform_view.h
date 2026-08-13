#pragma once

#include <memory>
#include <string>
#include <typeindex>
#include <utility>

#include <huxerui/platform_module.h>
#include <huxerui/view.h>

namespace huxerui {

namespace detail {

struct PlatformEventDescriptor {
  std::type_index key;
  std::string name;
  void (*dispatch)(const PlatformPayload&, const EventBindings&) = nullptr;

  bool operator==(const PlatformEventDescriptor&) const = default;
};

std::shared_ptr<ViewSpec> MakePlatformViewSpec(std::string type, PlatformPayload properties);

template <PlatformEventKey Key>
void DispatchPlatformViewEvent(const PlatformPayload& payload, const EventBindings& bindings) {
  auto handler = [&bindings](auto&&... values) {
    static_cast<void>(EmitEvent<Key>(bindings, std::forward<decltype(values)>(values)...));
  };
  DispatchPlatformEvent<Key>(payload, handler);
}

template <PlatformEventKey Key> PlatformEventDescriptor MakePlatformEventDescriptor() {
  return {
      typeid(Key),
      std::string(std::string_view(Key::Name)),
      DispatchPlatformViewEvent<Key>,
  };
}

} // namespace detail

class PlatformView final : public View {
public:
  explicit PlatformView(std::string type, PlatformPayload properties = {});

  template <detail::PlatformEventKey... Keys> PlatformView&& Events() && {
    (AddPlatformEvent(detail::MakePlatformEventDescriptor<Keys>()), ...);
    return std::move(*this);
  }
};

} // namespace huxerui
