#include <catch2/catch_amalgamated.hpp>

#include <string>

#include <huxerui/event.h>

#include "linux_text_input_internal.h"

namespace huxerui::test {
namespace {

TEST_CASE("IsTextProducingKey classifies editing keys") {
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::Backspace));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::Delete));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::ArrowLeft));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::ArrowRight));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::ArrowUp));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::ArrowDown));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::Home));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::End));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::PageUp));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::PageDown));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::Enter));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::Tab));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::Escape));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::Space));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::Unknown));
}

TEST_CASE("IsTextProducingKey classifies shortcut and modifier keys") {
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::A));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::C));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::V));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::X));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::Y));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::Z));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::Shift));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::Control));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::Alt));
  REQUIRE_FALSE(detail::IsTextProducingKey(Key::Meta));
}

TEST_CASE("ApplyXimPreeditEdit replaces the reported code-point range") {
  // "你好abc" is 5 code points: two 3-byte CJK characters plus three ASCII.
  const std::string current = "\xE4\xBD\xA0\xE5\xA5\xBD"
                              "abc";
  const std::optional<std::string> replaced = detail::ApplyXimPreeditEdit(current, 2, 3, "world");
  REQUIRE(replaced.has_value());
  REQUIRE(
      *replaced == "\xE4\xBD\xA0\xE5\xA5\xBD"
                   "world"
  );
}

TEST_CASE("ApplyXimPreeditEdit deletes a range when the replacement is empty") {
  const std::optional<std::string> deleted = detail::ApplyXimPreeditEdit("abc", 1, 2, "");
  REQUIRE(deleted.has_value());
  REQUIRE(*deleted == "a");
}

TEST_CASE("ApplyXimPreeditEdit inserts at the reported position") {
  const std::optional<std::string> inserted = detail::ApplyXimPreeditEdit("ab", 1, 0, "X");
  REQUIRE(inserted.has_value());
  REQUIRE(*inserted == "aXb");
}

TEST_CASE("ApplyXimPreeditEdit replaces a range split across a multi-byte code point") {
  // The changed range covers the final two code points of "a你b".
  const std::string current = "a\xE4\xBD\xA0"
                              "b";
  const std::optional<std::string> replaced = detail::ApplyXimPreeditEdit(current, 1, 2, "c");
  REQUIRE(replaced.has_value());
  REQUIRE(*replaced == "ac");
}

TEST_CASE("ApplyXimPreeditEdit rejects out-of-range changes") {
  REQUIRE_FALSE(detail::ApplyXimPreeditEdit("abc", 4, 0, "x").has_value());
  REQUIRE_FALSE(detail::ApplyXimPreeditEdit("abc", 2, 2, "x").has_value());
  REQUIRE_FALSE(detail::ApplyXimPreeditEdit("abc", -1, 1, "x").has_value());
  REQUIRE_FALSE(detail::ApplyXimPreeditEdit("abc", 1, -1, "x").has_value());
}

TEST_CASE("ApplyXimPreeditEdit rejects invalid UTF-8") {
  REQUIRE_FALSE(detail::ApplyXimPreeditEdit("\xFF\xFE", 0, 2, "x").has_value());
  REQUIRE_FALSE(detail::ApplyXimPreeditEdit("abc", 0, 0, "\xF0\x9F").has_value());
}

TEST_CASE("Utf8PrefixUtf16Length maps code points onto the UTF-16 space") {
  // "a" + U+4F60 (你) + U+1F600 (emoji) is 3 code points and 4 UTF-16 units.
  const std::string text = "a\xE4\xBD\xA0\xF0\x9F\x98\x80";
  REQUIRE(detail::Utf8PrefixUtf16Length(text, 0) == TextOffset{0});
  REQUIRE(detail::Utf8PrefixUtf16Length(text, 1) == TextOffset{1});
  REQUIRE(detail::Utf8PrefixUtf16Length(text, 2) == TextOffset{2});
  REQUIRE(detail::Utf8PrefixUtf16Length(text, 3) == TextOffset{4});
  REQUIRE_FALSE(detail::Utf8PrefixUtf16Length(text, 4).has_value());
  REQUIRE_FALSE(detail::Utf8PrefixUtf16Length(text, -1).has_value());
  REQUIRE_FALSE(detail::Utf8PrefixUtf16Length("\xFF", 1).has_value());
}

TEST_CASE("Utf8CodePointCount counts valid UTF-8") {
  const std::string text = "a\xE4\xBD\xA0\xF0\x9F\x98\x80";
  REQUIRE(detail::Utf8CodePointCount(text) == 3);
  REQUIRE(detail::Utf8CodePointCount("") == 0);
  REQUIRE(detail::Utf8CodePointCount("abc") == 3);
  REQUIRE_FALSE(detail::Utf8CodePointCount("\xF0\x9F").has_value());
  REQUIRE_FALSE(detail::Utf8CodePointCount("\x80").has_value());
}

} // namespace
} // namespace huxerui::test
