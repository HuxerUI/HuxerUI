#include <catch2/catch_amalgamated.hpp>

#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include <huxerui/text.h>

#include "selection_area_internal.h"

namespace huxerui::test {
namespace {

class TestTextSource final : public TextSelectionSource {
public:
  struct Block {
    TextBlockId id;
    AttributedText text;
    std::string separator = "\n";
  };

  explicit TestTextSource(std::vector<Block> blocks) : blocks_(std::move(blocks)) {
    for (std::size_t index = 0; index < blocks_.size(); ++index) {
      indexes_.emplace(blocks_[index].id, index);
    }
  }
  std::size_t Count() const noexcept override { return blocks_.size(); }
  TextBlockId IdAt(std::size_t index) const override { return blocks_.at(index).id; }
  std::optional<std::size_t> IndexOf(TextBlockId id) const override {
    const auto found = indexes_.find(id);
    return found == indexes_.end() ? std::nullopt : std::optional(found->second);
  }
  TextSelectionBlock BlockAt(std::size_t index) const override {
    ++reads;
    const auto& block = blocks_.at(index);
    return {block.text, block.separator};
  }

  mutable std::size_t reads = 0;

private:
  std::vector<Block> blocks_;
  std::unordered_map<TextBlockId, std::size_t> indexes_;
};

auto Source(std::vector<TestTextSource::Block> blocks) {
  return std::make_shared<TestTextSource>(std::move(blocks));
}

} // namespace

TEST_CASE("LogicalSelectionCopiesUnmountedBlocksAndOnlyInterveningSeparators") {
  detail::LogicalTextSelection selection;
  selection.SetSource(Source({{1, AttributedText("Alpha"), "\n\n"},
                              {2, AttributedText("Beta"), " | "}, {3, AttributedText("Gamma"), "unused"}}));
  selection.Select({1, {1}}, {3, {3}});
  REQUIRE(selection.Copy() == "lpha\n\nBeta | Gam");
  selection.Select({3, {3}}, {1, {1}});
  REQUIRE(selection.Copy() == "lpha\n\nBeta | Gam");
  selection.Select({2, {1}}, {2, {3}});
  REQUIRE(selection.Copy() == "et");
  selection.Select({2, {2}}, {2, {2}});
  REQUIRE_FALSE(selection.Copy());
}

TEST_CASE("LogicalSelectionSelectAllReadsEndpointsNotTheWholeDocument") {
  std::vector<TestTextSource::Block> blocks;
  const AttributedText text("block");
  for (TextBlockId id = 0; id < 10000; ++id) {
    blocks.push_back({id, text});
  }
  const auto source = Source(std::move(blocks));
  detail::LogicalTextSelection selection;
  selection.SetSource(source);
  REQUIRE(source->reads == 0);
  REQUIRE(selection.SelectAll());
  REQUIRE(source->reads <= 3);
  const auto range = selection.Range();
  REQUIRE(range->start.block == 0);
  REQUIRE(range->end.block == 9999);
  REQUIRE(range->end.position.offset == 5);
  REQUIRE_FALSE(selection.SelectAll());
}

TEST_CASE("LogicalSelectionPreservesEndpointsAcrossAppendStylesAndBlockMovement") {
  detail::LogicalTextSelection selection;
  const AttributedText first("one");
  const AttributedText last("three");
  selection.SetSource(Source({{1, first}, {2, AttributedText("two")}, {3, last}}));
  selection.Select({1, {1}}, {3, {5, TextAffinity::Upstream}});
  selection.SetSource(Source({{1, first.WithStyles({{{0, 3}, {.font_weight = FontWeight::Bold}}})},
                              {2, AttributedText("two")}, {3, AttributedText("three more")}}));
  REQUIRE(selection.Copy() == "ne\ntwo\nthree");
  REQUIRE(selection.Range()->end.position.affinity == TextAffinity::Upstream);
  selection.SetSource(Source({{3, AttributedText("three more")}, {1, first}, {2, AttributedText("two")}}));
  REQUIRE(selection.Copy() == " more\no");
  REQUIRE(selection.Anchor()->block == 1);
  REQUIRE(selection.Anchor()->position.offset == 1);
}

TEST_CASE("LogicalSelectionMapsOnlyChangedEndpointBodies") {
  detail::LogicalTextSelection selection;
  const auto original = Source({{1, AttributedText("abc😀xyz")}, {2, AttributedText("middle")},
                                {3, AttributedText("last")}});
  selection.SetSource(original);
  selection.Select({1, {5}}, {3, {4}});
  const auto replaced = Source({{1, AttributedText("a中xyz")}, {2, AttributedText("new middle")},
                                {3, AttributedText("last")}});
  original->reads = 0;
  selection.SetSource(replaced);
  REQUIRE(original->reads == 2);
  REQUIRE(replaced->reads == 2);
  REQUIRE(selection.Copy() == "xyz\nnew middle\nlast");

  selection.Select({1, {1}}, {1, {2}});
  selection.SetSource(Source({{1, AttributedText("a😀xyz")}}));
  REQUIRE(selection.Copy() == "😀");
  selection.Select({1, {3}}, {1, {5}});
  selection.SetSource(Source({{1, AttributedText("aQz")}}));
  REQUIRE(selection.Copy() == "Q");
}

TEST_CASE("LogicalSelectionClearsWhenAnEndpointDisappearsButNotAnIntermediateBlock") {
  detail::LogicalTextSelection selection;
  selection.SetSource(Source({{1, AttributedText("one")}, {2, AttributedText("two")}, {3, AttributedText("three")}}));
  selection.Select({1, {0}}, {3, {5}});
  selection.SetSource(Source({{1, AttributedText("one")}, {3, AttributedText("three")}}));
  REQUIRE(selection.Copy() == "one\nthree");
  selection.SetSource(Source({{1, AttributedText("one")}}));
  REQUIRE_FALSE(selection.Range());
  REQUIRE_FALSE(selection.Anchor());
  REQUIRE(selection.SelectAll());
  selection.SetSource(nullptr);
  REQUIRE_FALSE(selection.Copy());
}

TEST_CASE("LogicalSelectionValidatesOffsetsAndSourceConsistency") {
  detail::LogicalTextSelection selection;
  REQUIRE_THROWS_AS(selection.Select({1, {0}}, {1, {1}}), std::logic_error);
  selection.SetSource(Source({{1, AttributedText("😀")}}));
  REQUIRE_THROWS_AS(selection.Select({1, {1}}, {1, {2}}), std::invalid_argument);
  REQUIRE_THROWS_AS(selection.Select({2, {0}}, {1, {2}}), std::invalid_argument);
  REQUIRE(selection.SelectAll());
  REQUIRE(selection.Copy() == "😀");
}

} // namespace huxerui::test
