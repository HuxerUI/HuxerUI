#include "color_stream.h"

#include <stdexcept>
#include <utility>
#include <variant>

namespace {

struct ColorStreamMethods {
  struct Texture {
    using Request = std::monostate;
    using Result = huxerui::ExternalTexture;
    static constexpr std::string_view Name = huxerui::example::color_stream::texture_method;

    static huxerui::PlatformPayload Encode(const Request&) {
      return {};
    }

    static Result Decode(const huxerui::PlatformPayload& payload) {
      return payload.AsExternalTexture();
    }
  };
};

} // namespace

namespace huxerui::example {

ColorStreamService::ColorStreamService(PlatformInstance instance) : instance_(std::move(instance)) {}

PlatformRequestId ColorStreamService::Texture(std::function<void(PlatformResult<ExternalTexture>)> completion) {
  if (!completion) {
    throw std::invalid_argument("HuxerUI example color stream completion must not be empty");
  }
  return instance_.Call<ColorStreamMethods::Texture>(std::monostate{}, std::move(completion));
}

std::shared_ptr<ColorStreamService> UseColorStream() {
  return UseService<ColorStreamService>();
}

} // namespace huxerui::example
