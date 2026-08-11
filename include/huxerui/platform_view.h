#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
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

template <class Key>
concept PlatformEventKey = EventKey<Key> && requires {
  { Key::PlatformName } -> std::convertible_to<std::string_view>;
};

template <class Key, class Signature> struct PlatformEventDispatcher;

template <class Key> struct PlatformEventDispatcher<Key, void()> {
  static void Dispatch(const PlatformPayload& payload, const EventBindings& bindings) {
    Key::Decode(payload);
    static_cast<void>(EmitEvent<Key>(bindings));
  }
};

template <class Key, class Argument> struct PlatformEventDispatcher<Key, void(Argument)> {
  static void Dispatch(const PlatformPayload& payload, const EventBindings& bindings) {
    decltype(auto) decoded = Key::Decode(payload);
    static_cast<void>(EmitEvent<Key>(bindings, std::forward<decltype(decoded)>(decoded)));
  }
};

template <class Key, class First, class Second, class... Rest>
struct PlatformEventDispatcher<Key, void(First, Second, Rest...)> {
  static void Dispatch(const PlatformPayload& payload, const EventBindings& bindings) {
    auto decoded = Key::Decode(payload);
    std::apply(
        [&bindings](auto&&... values) {
          static_cast<void>(EmitEvent<Key>(bindings, std::forward<decltype(values)>(values)...));
        },
        std::move(decoded)
    );
  }
};

template <PlatformEventKey Key> PlatformEventDescriptor MakePlatformEventDescriptor() {
  return {
      typeid(Key),
      std::string(std::string_view(Key::PlatformName)),
      PlatformEventDispatcher<Key, typename Key::Signature>::Dispatch,
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
