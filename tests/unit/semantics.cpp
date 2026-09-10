#include <catch2/catch_amalgamated.hpp>

#include <type_traits>

#include <huxerui/semantics.h>

namespace huxerui::test {

static_assert(!std::is_copy_constructible_v<SemanticBuilder>);
static_assert(!std::is_move_constructible_v<SemanticBuilder>);

TEST_CASE("SemanticAggregatesKeepOmittedValuesUnspecified") {
  STATIC_REQUIRE(std::is_aggregate_v<Semantics>);
  STATIC_REQUIRE(std::is_aggregate_v<SemanticCollection>);
  STATIC_REQUIRE(std::is_aggregate_v<SemanticCollectionItem>);

  const Semantics declared{.busy = false};
  Semantics expected;
  expected.busy = false;
  REQUIRE(declared == expected);
  REQUIRE(declared.busy.has_value());
  REQUIRE_FALSE(*declared.busy);
  REQUIRE_FALSE(declared.selected.has_value());
  REQUIRE_FALSE(declared.label.has_value());

  const SemanticCollection collection{.item_count = 0};
  REQUIRE(collection.item_count == 0);
  REQUIRE_FALSE(collection.row_count.has_value());
  REQUIRE_FALSE(collection.column_count.has_value());
  const SemanticCollectionItem item{.index = 0};
  REQUIRE_FALSE(item.row_index.has_value());
  REQUIRE_FALSE(item.column_index.has_value());
  REQUIRE(item.row_span == 1);
  REQUIRE(item.column_span == 1);
}

} // namespace huxerui::test
