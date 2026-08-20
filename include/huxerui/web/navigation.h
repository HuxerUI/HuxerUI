#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <huxerui/navigation.h>
#include <huxerui/state.h>
#include <huxerui/view.h>

namespace huxerui::web {

namespace detail {

struct BrowserNavigationBinding {
  std::string encoded_path;
  std::function<std::optional<std::string>(std::string_view)> apply_location;
};

class BrowserNavigationCoordinator final {
public:
  BrowserNavigationCoordinator();
  ~BrowserNavigationCoordinator();

  BrowserNavigationCoordinator(const BrowserNavigationCoordinator&) = delete;
  BrowserNavigationCoordinator& operator=(const BrowserNavigationCoordinator&) = delete;

  void Update(BrowserNavigationBinding binding);
  void Commit(huxerui::detail::NavigationHistoryAction action, std::string location);
  void LocationChanged();

private:
  class Implementation;
  std::unique_ptr<Implementation> implementation_;
};

template <class Codec, class Route>
concept BrowserRouteCodec =
    std::copy_constructible<std::decay_t<Codec>> &&
    requires(std::decay_t<Codec>& codec, std::string_view location, const NavigationPath<Route>& path) {
      { codec.Decode(location) } -> std::same_as<std::optional<NavigationPath<Route>>>;
      { codec.Encode(path) } -> std::convertible_to<std::string>;
    };

} // namespace detail

// The codec owns the application's URL policy. NavigationPath remains the authoritative in-memory history, while
// this adapter mirrors controller operations to the browser and applies Back/Forward locations without echoing them.
// Encode must return a same-document path, query, fragment, or same-origin URL accepted by the Browser History API.
// One BrowserNavigationStack may own URL synchronization in a browser document at a time.
template <huxerui::detail::NavigationRouteValue Route, class RootFactory, class Resolver, class Codec>
  requires huxerui::detail::ViewFactoryFor<RootFactory> && std::copy_constructible<std::decay_t<Resolver>> &&
           detail::BrowserRouteCodec<Codec, Route> && requires(std::decay_t<Resolver>& resolver, const Route& route) {
             { std::invoke(resolver, route) } -> std::convertible_to<View>;
           }
View BrowserNavigationStack(RootFactory&& root, State<NavigationPath<Route>> path, Resolver&& resolver, Codec&& codec) {
  std::function<View()> root_factory = huxerui::detail::BindViewFactory(std::forward<RootFactory>(root));
  using StoredResolver = std::decay_t<Resolver>;
  using StoredCodec = std::decay_t<Codec>;

  return Scope(
      [root_factory = std::move(root_factory),
       path,
       resolver = StoredResolver(std::forward<Resolver>(resolver)),
       codec = StoredCodec(std::forward<Codec>(codec))]() mutable -> View {
        auto coordinator_state = UseState(std::make_shared<detail::BrowserNavigationCoordinator>());
        const std::shared_ptr<detail::BrowserNavigationCoordinator> coordinator = coordinator_state.Get();
        auto history_commit_state = UseState(std::make_shared<huxerui::detail::NavigationHistoryCommit<Route>>());
        const std::shared_ptr<huxerui::detail::NavigationHistoryCommit<Route>> history_commit =
            history_commit_state.Get();
        auto shared_codec = std::make_shared<StoredCodec>(codec);
        coordinator->Update({
            .encoded_path = std::string(shared_codec->Encode(path.Get())),
            .apply_location = [path, shared_codec](std::string_view location) -> std::optional<std::string> {
              std::optional<NavigationPath<Route>> decoded = shared_codec->Decode(location);
              if (!decoded) {
                return std::nullopt;
              }
              std::string canonical = std::string(shared_codec->Encode(*decoded));
              path = std::move(*decoded);
              return canonical;
            },
        });

        *history_commit =
            [weak_coordinator = std::weak_ptr(coordinator),
             shared_codec,
             path](huxerui::detail::NavigationHistoryAction action, NavigationPath<Route> next_path) mutable {
              const std::string location = std::string(shared_codec->Encode(next_path));
              if (const std::shared_ptr<detail::BrowserNavigationCoordinator> coordinator = weak_coordinator.lock()) {
                coordinator->Commit(action, location);
              }
              path = std::move(next_path);
            };
        return huxerui::detail::BuildTypedNavigationStack(root_factory, path, resolver, history_commit);
      }
  );
}

} // namespace huxerui::web
