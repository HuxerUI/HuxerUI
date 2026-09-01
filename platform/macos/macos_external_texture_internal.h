#pragma once

#include <huxerui/macos/external_texture.h>

@class HUXExternalTexture;

namespace huxerui::macos::detail {

HUXExternalTexture* WrapExternalTexture(std::shared_ptr<ExternalTexture> texture);
std::shared_ptr<ExternalTexture> UnwrapExternalTexture(HUXExternalTexture* texture);

} // namespace huxerui::macos::detail
