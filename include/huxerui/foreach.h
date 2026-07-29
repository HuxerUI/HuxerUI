#pragma once

#include <cstddef>
#include <concepts>
#include <functional>
#include <ranges>
#include <type_traits>
#include <utility>

#include <huxerui/state.h>
#include <huxerui/view.h>

namespace huxerui {

template <std::ranges::input_range Range, class Factory>
  requires std::invocable<Factory&, std::ranges::range_reference_t<Range>> &&
           std::convertible_to<std::invoke_result_t<Factory&, std::ranges::range_reference_t<Range>>, View>
Views ForEach(Range&& range, Factory&& factory) {
  Views result;
  if constexpr (std::ranges::sized_range<Range>) {
    result.Reserve(static_cast<std::size_t>(std::ranges::size(range)));
  }

  for (auto&& value : range) {
    result.Add(std::invoke(factory, value));
  }
  return result;
}

template <class Range, class Factory>
  requires std::ranges::input_range<const Range&> &&
           std::invocable<Factory&, std::ranges::range_reference_t<const Range&>> &&
           std::convertible_to<std::invoke_result_t<Factory&, std::ranges::range_reference_t<const Range&>>, View>
Views ForEach(const State<Range>& range, Factory&& factory) {
  return ForEach(range.Get(), std::forward<Factory>(factory));
}

} // namespace huxerui
