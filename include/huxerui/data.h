#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace huxerui {

using Bytes = std::vector<std::byte>;

class Uri final {
public:
  explicit Uri(std::string value);

  Uri(const Uri&) = default;
  Uri(Uri&&) noexcept = default;
  Uri& operator=(const Uri&) = default;
  Uri& operator=(Uri&&) noexcept = default;

  [[nodiscard]] static std::optional<Uri> Parse(std::string_view value);

  [[nodiscard]] std::string_view Scheme() const noexcept;
  [[nodiscard]] std::optional<std::string_view> Authority() const noexcept;
  [[nodiscard]] std::string_view Path() const noexcept;
  [[nodiscard]] std::optional<std::string_view> Query() const noexcept;
  [[nodiscard]] std::optional<std::string_view> Fragment() const noexcept;

  [[nodiscard]] const std::string& ToString() const noexcept;
  [[nodiscard]] bool operator==(const Uri& other) const noexcept;

private:
  struct Range {
    std::size_t offset = 0;
    std::size_t length = 0;
  };

  void ParseValue();
  [[nodiscard]] std::string_view View(Range range) const noexcept;

  std::string value_;
  Range scheme_;
  std::optional<Range> authority_;
  Range path_;
  std::optional<Range> query_;
  std::optional<Range> fragment_;
};

} // namespace huxerui
