#include <huxerui/data.h>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "data_internal.h"

namespace huxerui {

namespace {

using detail::HexDigitValue;
using detail::IsAsciiAlpha;
using detail::IsAsciiDigit;
using detail::IsUriSubDelimiter;
using detail::IsUriUnreserved;

bool ConsumePercentEscape(std::string_view value, std::size_t& index, std::size_t end) {
  if (value[index] != '%') {
    return false;
  }
  if (index + 2 >= end || HexDigitValue(value[index + 1]) < 0 || HexDigitValue(value[index + 2]) < 0) {
    throw std::invalid_argument("HuxerUI URI contains a malformed percent escape");
  }
  index += 3;
  return true;
}

template <class Predicate>
void ValidateCharacters(std::string_view value, std::size_t begin, std::size_t end, Predicate allowed) {
  for (std::size_t index = begin; index < end;) {
    if (ConsumePercentEscape(value, index, end)) {
      continue;
    }
    const unsigned char character = static_cast<unsigned char>(value[index]);
    if (character > 0x7FU || !allowed(static_cast<char>(character))) {
      throw std::invalid_argument("HuxerUI URI contains a character outside RFC 3986 syntax");
    }
    ++index;
  }
}

void ValidateAuthority(std::string_view value, std::size_t begin, std::size_t end) {
  const std::size_t first_at = value.find('@', begin);
  const std::size_t last_at = value.rfind('@', end == 0 ? 0 : end - 1);
  std::size_t host_begin = begin;
  if (first_at != std::string_view::npos && first_at < end) {
    if (last_at != first_at) {
      throw std::invalid_argument("HuxerUI URI authority contains multiple user-info delimiters");
    }
    ValidateCharacters(value, begin, first_at, [](char character) {
      return IsUriUnreserved(character) || IsUriSubDelimiter(character) || character == ':';
    });
    host_begin = first_at + 1;
  }

  if (host_begin < end && value[host_begin] == '[') {
    const std::size_t close = value.find(']', host_begin + 1);
    if (close == std::string_view::npos || close >= end || close == host_begin + 1) {
      throw std::invalid_argument("HuxerUI URI authority contains an invalid IP literal");
    }
    ValidateCharacters(value, host_begin + 1, close, [](char character) {
      return IsUriUnreserved(character) || IsUriSubDelimiter(character) || character == ':';
    });
    if (close + 1 < end) {
      if (value[close + 1] != ':') {
        throw std::invalid_argument("HuxerUI URI authority contains invalid data after an IP literal");
      }
      for (std::size_t index = close + 2; index < end; ++index) {
        if (!IsAsciiDigit(value[index])) {
          throw std::invalid_argument("HuxerUI URI authority port contains an invalid character");
        }
      }
    }
    return;
  }

  const std::size_t colon = value.find(':', host_begin);
  const std::size_t host_end = colon == std::string_view::npos || colon >= end ? end : colon;
  if (colon != std::string_view::npos && colon < end && value.find(':', colon + 1) < end) {
    throw std::invalid_argument("HuxerUI URI authority contains an unbracketed IP literal");
  }
  ValidateCharacters(value, host_begin, host_end, [](char character) {
    return IsUriUnreserved(character) || IsUriSubDelimiter(character);
  });
  if (host_end < end) {
    for (std::size_t index = host_end + 1; index < end; ++index) {
      if (!IsAsciiDigit(value[index])) {
        throw std::invalid_argument("HuxerUI URI authority port contains an invalid character");
      }
    }
  }
}

void ValidatePath(std::string_view value, std::size_t begin, std::size_t end) {
  ValidateCharacters(value, begin, end, [](char character) {
    return IsUriUnreserved(character) || IsUriSubDelimiter(character) || character == ':' || character == '@' ||
           character == '/';
  });
}

void ValidateQueryOrFragment(std::string_view value, std::size_t begin, std::size_t end) {
  ValidateCharacters(value, begin, end, [](char character) {
    return IsUriUnreserved(character) || IsUriSubDelimiter(character) || character == ':' || character == '@' ||
           character == '/' || character == '?';
  });
}

} // namespace

Uri::Uri(std::string value) : value_(std::move(value)) {
  ParseValue();
}

std::optional<Uri> Uri::Parse(std::string_view value) {
  try {
    return Uri(std::string(value));
  } catch (const std::invalid_argument&) {
    return std::nullopt;
  }
}

std::string_view Uri::Scheme() const noexcept {
  return View(scheme_);
}

std::optional<std::string_view> Uri::Authority() const noexcept {
  if (!authority_.has_value()) {
    return std::nullopt;
  }
  return View(*authority_);
}

std::string_view Uri::Path() const noexcept {
  return View(path_);
}

std::optional<std::string_view> Uri::Query() const noexcept {
  if (!query_.has_value()) {
    return std::nullopt;
  }
  return View(*query_);
}

std::optional<std::string_view> Uri::Fragment() const noexcept {
  if (!fragment_.has_value()) {
    return std::nullopt;
  }
  return View(*fragment_);
}

const std::string& Uri::ToString() const noexcept {
  return value_;
}

bool Uri::operator==(const Uri& other) const noexcept {
  return value_ == other.value_;
}

void Uri::ParseValue() {
  const std::size_t scheme_end = value_.find(':');
  if (scheme_end == std::string::npos || scheme_end == 0 || !IsAsciiAlpha(value_.front())) {
    throw std::invalid_argument("HuxerUI URI must contain an absolute scheme");
  }
  for (std::size_t index = 1; index < scheme_end; ++index) {
    const char character = value_[index];
    if (!IsAsciiAlpha(character) && !IsAsciiDigit(character) && character != '+' && character != '-' &&
        character != '.') {
      throw std::invalid_argument("HuxerUI URI scheme contains an invalid character");
    }
  }
  scheme_ = {0, scheme_end};

  const std::size_t fragment_delimiter = value_.find('#', scheme_end + 1);
  const std::size_t before_fragment =
      fragment_delimiter == std::string::npos ? value_.size() : fragment_delimiter;
  const std::size_t query_delimiter = value_.find('?', scheme_end + 1);
  const bool has_query = query_delimiter != std::string::npos && query_delimiter < before_fragment;
  const std::size_t hierarchy_end = has_query ? query_delimiter : before_fragment;

  std::size_t path_begin = scheme_end + 1;
  if (path_begin + 1 < hierarchy_end && value_[path_begin] == '/' && value_[path_begin + 1] == '/') {
    const std::size_t authority_begin = path_begin + 2;
    const std::size_t separator = value_.find('/', authority_begin);
    const std::size_t authority_end = separator == std::string::npos || separator > hierarchy_end
                                          ? hierarchy_end
                                          : separator;
    authority_ = Range{authority_begin, authority_end - authority_begin};
    ValidateAuthority(value_, authority_begin, authority_end);
    path_begin = authority_end;
  } else {
    authority_.reset();
  }

  path_ = {path_begin, hierarchy_end - path_begin};
  ValidatePath(value_, path_begin, hierarchy_end);

  if (has_query) {
    const std::size_t query_begin = query_delimiter + 1;
    query_ = Range{query_begin, before_fragment - query_begin};
    ValidateQueryOrFragment(value_, query_begin, before_fragment);
  } else {
    query_.reset();
  }

  if (fragment_delimiter != std::string::npos) {
    const std::size_t fragment_begin = fragment_delimiter + 1;
    fragment_ = Range{fragment_begin, value_.size() - fragment_begin};
    ValidateQueryOrFragment(value_, fragment_begin, value_.size());
  } else {
    fragment_.reset();
  }
}

std::string_view Uri::View(Range range) const noexcept {
  if (range.offset > value_.size() || range.length > value_.size() - range.offset) {
    return {};
  }
  return std::string_view(value_).substr(range.offset, range.length);
}

} // namespace huxerui
