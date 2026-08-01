#include <catch2/catch_amalgamated.hpp>

#include "android_text_input_internal.h"

namespace huxerui::detail {

TEST_CASE("Android cursor positions follow InputConnection replacement semantics") {
  const TextInputContext context{
      .result_code = TextInputResultCode::Ok,
      .total_length = 8,
      .selection = {2, 5},
  };

  CHECK(AndroidCursorSelection(context, {2, 5}, 4, 1) == (TextSelection{6, 6}));
  CHECK(AndroidCursorSelection(context, {2, 5}, 4, 2) == (TextSelection{7, 7}));
  CHECK(AndroidCursorSelection(context, {2, 5}, 4, 20) == (TextSelection{9, 9}));
  CHECK(AndroidCursorSelection(context, {2, 5}, 4, 0) == (TextSelection{2, 2}));
  CHECK(AndroidCursorSelection(context, {2, 5}, 4, -1) == (TextSelection{1, 1}));
  CHECK(AndroidCursorSelection(context, {2, 5}, 4, -20) == (TextSelection{0, 0}));
}

TEST_CASE("Android cursor position rejects invalid replacement lengths") {
  const TextInputContext context{
      .result_code = TextInputResultCode::Ok,
      .total_length = 8,
      .selection = {2, 5},
  };

  CHECK_FALSE(AndroidCursorSelection(context, {2, 9}, 1, 1).has_value());
  CHECK_FALSE(AndroidCursorSelection(context, {2, 5}, -1, 1).has_value());
}

} // namespace huxerui::detail
