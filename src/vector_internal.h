#pragma once

#include <huxerui/paint.h>
#include <huxerui/vector.h>

namespace huxerui::detail {

class VectorAccess final {
public:
  [[nodiscard]] static const PaintSequence& Sequence(const VectorAsset& asset) noexcept;
};

} // namespace huxerui::detail
