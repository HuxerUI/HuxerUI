#include <huxerui/app.h>

#include <memory>
#include <stdexcept>

#include "internal.h"

namespace huxerui {

int RunApp(RootFactory root_factory, AppOptions options) {
  auto platform = detail::CreateDefaultPlatformHost();
  if (!platform) {
    throw std::runtime_error("HuxerUI could not create a platform host");
  }

  detail::Runtime runtime{
      root_factory, *platform, options};
  return platform->Run(runtime, options);
}

}  // namespace huxerui
