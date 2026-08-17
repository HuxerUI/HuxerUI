#include <catch2/catch_amalgamated.hpp>

#include <X11/keysym.h>

#include <limits>
#include <string>

#include "linux_text_input.h"
#include "linux_text_input_internal.h"

namespace huxerui::test {
namespace {

TEST_CASE("XIM filters active non-secure key events and all protocol events") {
  REQUIRE(detail::ShouldFilterXimEvent(KeyPress, true, true, false));
  REQUIRE(detail::ShouldFilterXimEvent(KeyRelease, true, true, false));
  REQUIRE(detail::ShouldFilterXimEvent(ClientMessage, true, true, false));
  REQUIRE(detail::ShouldFilterXimEvent(ClientMessage, true, true, true));
  REQUIRE(detail::ShouldFilterXimEvent(ClientMessage, true, false, false));
}

TEST_CASE("XIM leaves secure and inactive key events on the shared path") {
  REQUIRE_FALSE(detail::ShouldFilterXimEvent(KeyPress, true, true, true));
  REQUIRE_FALSE(detail::ShouldFilterXimEvent(KeyRelease, true, true, true));
  REQUIRE_FALSE(detail::ShouldFilterXimEvent(KeyPress, true, false, false));
  REQUIRE_FALSE(detail::ShouldFilterXimEvent(KeyPress, false, true, false));
  REQUIRE_FALSE(detail::ShouldFilterXimEvent(ClientMessage, false, true, false));
}

TEST_CASE("XIM lookup accepts unmodified shortcut letters as text") {
  REQUIRE_FALSE(detail::ShouldBypassXimLookup(XK_a, 0));
  REQUIRE_FALSE(detail::ShouldBypassXimLookup(XK_C, ShiftMask));
  REQUIRE_FALSE(detail::ShouldBypassXimLookup(XK_v, Mod1Mask));
  REQUIRE_FALSE(detail::ShouldBypassXimLookup(XK_X, ShiftMask | Mod1Mask));
  REQUIRE_FALSE(detail::ShouldBypassXimLookup(XK_y, 0));
  REQUIRE_FALSE(detail::ShouldBypassXimLookup(XK_Z, ShiftMask));
}

TEST_CASE("XIM lookup bypasses explicit application shortcuts after filtering") {
  REQUIRE(detail::ShouldBypassXimLookup(XK_a, ControlMask));
  REQUIRE(detail::ShouldBypassXimLookup(XK_C, Mod4Mask));
  REQUIRE(detail::ShouldBypassXimLookup(XK_v, ControlMask | ShiftMask));
  REQUIRE(detail::ShouldBypassXimLookup(XK_X, ControlMask | Mod4Mask));
  REQUIRE(detail::ShouldBypassXimLookup(XK_y, Mod4Mask | ShiftMask));
  REQUIRE(detail::ShouldBypassXimLookup(XK_Z, ControlMask));
}

TEST_CASE("XIM lookup leaves switching and composition keys to the input method") {
  REQUIRE(detail::ShouldBypassXimLookup(XK_Shift_L, 0));
  REQUIRE(detail::ShouldBypassXimLookup(XK_Super_L, 0));
  REQUIRE(detail::ShouldBypassXimLookup(XK_space, Mod4Mask));
  REQUIRE_FALSE(detail::ShouldBypassXimLookup(XK_space, 0));
  REQUIRE_FALSE(detail::ShouldBypassXimLookup(XK_BackSpace, 0));
  REQUIRE_FALSE(detail::ShouldBypassXimLookup(XK_Return, 0));
  REQUIRE_FALSE(detail::ShouldBypassXimLookup(XK_Left, 0));
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
  REQUIRE_FALSE(detail::ApplyXimPreeditEdit("abc", std::numeric_limits<int>::max(), 1, "x").has_value());
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

TEST_CASE("Utf8BytePrefixUtf16Length maps Fcitx byte cursors onto the UTF-16 space") {
  const std::string text = "a\xE4\xBD\xA0\xF0\x9F\x98\x80";
  REQUIRE(detail::Utf8BytePrefixUtf16Length(text, 0) == TextOffset{0});
  REQUIRE(detail::Utf8BytePrefixUtf16Length(text, 1) == TextOffset{1});
  REQUIRE(detail::Utf8BytePrefixUtf16Length(text, 4) == TextOffset{2});
  REQUIRE(detail::Utf8BytePrefixUtf16Length(text, 8) == TextOffset{4});
  REQUIRE_FALSE(detail::Utf8BytePrefixUtf16Length(text, 2).has_value());
  REQUIRE_FALSE(detail::Utf8BytePrefixUtf16Length(text, 9).has_value());
  REQUIRE_FALSE(detail::Utf8BytePrefixUtf16Length(text, -1).has_value());
  REQUIRE_FALSE(detail::Utf8BytePrefixUtf16Length("\xFF", 1).has_value());
}

TEST_CASE("Utf16OffsetToUtf8Byte maps surrounding-text offsets onto UTF-8 bytes") {
  const std::string text = "a\xE4\xBD\xA0\xF0\x9F\x98\x80";
  REQUIRE(detail::Utf16OffsetToUtf8Byte(text, 0) == 0);
  REQUIRE(detail::Utf16OffsetToUtf8Byte(text, 1) == 1);
  REQUIRE(detail::Utf16OffsetToUtf8Byte(text, 2) == 4);
  REQUIRE(detail::Utf16OffsetToUtf8Byte(text, 4) == 8);
  REQUIRE_FALSE(detail::Utf16OffsetToUtf8Byte(text, 3).has_value());
  REQUIRE_FALSE(detail::Utf16OffsetToUtf8Byte(text, 5).has_value());
  REQUIRE_FALSE(detail::Utf16OffsetToUtf8Byte(text, -1).has_value());
  REQUIRE_FALSE(detail::Utf16OffsetToUtf8Byte("\xFF", 1).has_value());
}

TEST_CASE("Fcitx frontend selection recognizes common Linux input module variables") {
  REQUIRE(detail::ShouldUseFcitxFrontend("@im=fcitx", nullptr, nullptr));
  REQUIRE(detail::ShouldUseFcitxFrontend("@im=Fcitx5", nullptr, nullptr));
  REQUIRE(detail::ShouldUseFcitxFrontend(nullptr, "fcitx", nullptr));
  REQUIRE(detail::ShouldUseFcitxFrontend(nullptr, nullptr, "fcitx5"));
  REQUIRE_FALSE(detail::ShouldUseFcitxFrontend("@im=ibus", "ibus", "ibus"));
  REQUIRE_FALSE(detail::ShouldUseFcitxFrontend(nullptr, nullptr, nullptr));
}

TEST_CASE("XIM focus is limited to active non-secure sessions without Fcitx") {
  REQUIRE_FALSE(detail::ShouldFocusXim(true, true, true, false));
  REQUIRE(detail::ShouldFocusXim(true, false, true, false));
  REQUIRE_FALSE(detail::ShouldFocusXim(true, true, false, false));
  REQUIRE_FALSE(detail::ShouldFocusXim(true, true, true, true));
  REQUIRE_FALSE(detail::ShouldFocusXim(true, false, false, false));
  REQUIRE_FALSE(detail::ShouldFocusXim(true, false, true, true));
  REQUIRE_FALSE(detail::ShouldFocusXim(false, false, true, false));
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
