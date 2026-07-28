#pragma once

#include <limits>
#include <string_view>

#include <huxerui/geometry.h>

namespace huxerui {

class TextService {
public:
  virtual ~TextService() = default;

  virtual Size MeasureText(
      std::string_view text,
      float font_size,
      float max_width = std::numeric_limits<float>::infinity()) = 0;
};

}  // namespace huxerui
