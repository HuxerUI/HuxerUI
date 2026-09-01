#pragma once

#include <huxerui/ios/external_texture.h>

@class HUXExternalTexture;

namespace huxerui::ios::detail {

HUXExternalTexture* WrapExternalTexture(std::shared_ptr<ExternalTexture> texture);
std::shared_ptr<ExternalTexture> UnwrapExternalTexture(HUXExternalTexture* texture);

} // namespace huxerui::ios::detail
