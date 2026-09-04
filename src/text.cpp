#include <huxerui/text.h>

#include <cmath>
#include <stdexcept>
#include <utility>

#include <huxerui/root.h>

#include "view_internal.h"

namespace huxerui {
namespace {

void RequireFontSize(float size) {
  if (!std::isfinite(size) || size <= 0.0F) {
    throw std::invalid_argument("HuxerUI font size must be finite and greater than zero");
  }
}

} // namespace

Font::Font(FontFamilyKind family_kind, std::string family_name, float size)
    : family_kind_(family_kind), family_name_(std::move(family_name)), size_(size) {
  RequireFontSize(size);
  if (family_kind_ == FontFamilyKind::Named && family_name_.empty()) {
    throw std::invalid_argument("HuxerUI named font family must not be empty");
  }
}

Font Font::System(float size) {
  return Font{FontFamilyKind::System, {}, size};
}

Font Font::Monospace(float size) {
  return Font{FontFamilyKind::Monospace, {}, size};
}

Font Font::Named(std::string family, float size) {
  return Font{FontFamilyKind::Named, std::move(family), size};
}

Font Font::WithSize(float size) const {
  RequireFontSize(size);
  Font result = *this;
  result.size_ = size;
  return result;
}

Font Font::WithWeight(FontWeight weight) const {
  Font result = *this;
  result.weight_ = weight;
  return result;
}

Font Font::WithSlant(FontSlant slant) const {
  Font result = *this;
  result.slant_ = slant;
  return result;
}

TextMeasurer& UseTextMeasurer() {
  const std::shared_ptr<detail::TextMeasurerService> service = UseService<detail::TextMeasurerService>();
  if (service->measurer == nullptr) {
    throw std::logic_error("HuxerUI text measurer service is disconnected");
  }
  return *service->measurer;
}

} // namespace huxerui
