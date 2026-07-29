#pragma once

#include <any>
#include <concepts>
#include <cstddef>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <typeindex>
#include <utility>
#include <vector>

#include <huxerui/geometry.h>

namespace huxerui {

enum class MainAxisAlignment {
  Start,
  Center,
  End,
  SpaceBetween,
  SpaceAround,
  SpaceEvenly,
};

enum class CrossAxisAlignment {
  Start,
  Center,
  End,
  Stretch,
};

enum class HorizontalAlignment {
  Start,
  Center,
  End,
  Stretch,
};

enum class VerticalAlignment {
  Start,
  Center,
  End,
  Stretch,
};

namespace detail {
struct LayoutContextAccess;
}

class MountedNode {
public:
  class ChildIterator {
  public:
    using difference_type = std::ptrdiff_t;
    using value_type = MountedNode;
    using reference = MountedNode&;
    using pointer = MountedNode*;
    using iterator_category = std::forward_iterator_tag;

    ChildIterator() = default;

    reference operator*() const {
      return owner_->ChildAt(index_);
    }

    pointer operator->() const {
      return &owner_->ChildAt(index_);
    }

    ChildIterator& operator++() {
      ++index_;
      return *this;
    }

    ChildIterator operator++(int) {
      ChildIterator previous = *this;
      ++*this;
      return previous;
    }

    bool operator==(const ChildIterator&) const = default;

  private:
    ChildIterator(MountedNode& owner, std::size_t index) : owner_(&owner), index_(index) {}

    MountedNode* owner_ = nullptr;
    std::size_t index_ = 0;

    friend class MountedNode;
  };

  class ChildrenRange {
  public:
    ChildIterator begin() const {
      return ChildIterator{*owner_, 0};
    }

    ChildIterator end() const {
      return ChildIterator{*owner_, owner_->ChildCount()};
    }

    [[nodiscard]] std::size_t Size() const noexcept {
      return owner_->ChildCount();
    }

    [[nodiscard]] bool Empty() const noexcept {
      return Size() == 0;
    }

  private:
    explicit ChildrenRange(MountedNode& owner) : owner_(&owner) {}

    MountedNode* owner_;

    friend class MountedNode;
  };

  virtual ~MountedNode() = default;

  [[nodiscard]] ChildrenRange Children() noexcept {
    return ChildrenRange{*this};
  }

  [[nodiscard]] std::size_t ChildCount() const noexcept {
    return ChildCountImpl();
  }

  [[nodiscard]] MountedNode& ChildAt(std::size_t index) {
    if (index >= ChildCount()) {
      throw std::out_of_range("HuxerUI mounted child index is out of range");
    }
    return ChildAtImpl(index);
  }

  [[nodiscard]] const MountedNode& ChildAt(std::size_t index) const {
    if (index >= ChildCount()) {
      throw std::out_of_range("HuxerUI mounted child index is out of range");
    }
    return ChildAtImpl(index);
  }

  [[nodiscard]] Size MeasuredSize() const noexcept {
    return MeasuredSizeImpl();
  }

  [[nodiscard]] Rect Frame() const noexcept {
    return FrameImpl();
  }

  [[nodiscard]] Rect PresentationFrame() const noexcept {
    return PresentationFrameImpl();
  }

  [[nodiscard]] float PresentationOpacity() const noexcept {
    return PresentationOpacityImpl();
  }

  [[nodiscard]] bool IsEnabled() const noexcept {
    return IsEnabledImpl();
  }

  [[nodiscard]] bool IsFocused() const noexcept {
    return IsFocusedImpl();
  }

  [[nodiscard]] float Spacing() const noexcept {
    return SpacingImpl();
  }

  [[nodiscard]] float GrowFactor() const noexcept {
    return GrowFactorImpl();
  }

  [[nodiscard]] MainAxisAlignment MainAlignment() const noexcept {
    return MainAlignmentImpl();
  }

  [[nodiscard]] CrossAxisAlignment CrossAlignment() const noexcept {
    return CrossAlignmentImpl();
  }

  [[nodiscard]] HorizontalAlignment HorizontalAlignmentValue() const noexcept {
    return HorizontalAlignmentImpl();
  }

  [[nodiscard]] VerticalAlignment VerticalAlignmentValue() const noexcept {
    return VerticalAlignmentImpl();
  }

  template <class Key> [[nodiscard]] const typename Key::Value* LayoutValue() const noexcept {
    const std::any* value = FindLayoutValue(typeid(Key));
    return value == nullptr ? nullptr : std::any_cast<typename Key::Value>(value);
  }

  template <class Key> [[nodiscard]] typename Key::Value LayoutValueOr(typename Key::Value fallback) const {
    if (const auto* value = LayoutValue<Key>()) {
      return *value;
    }
    return fallback;
  }

  template <class T, class... Arguments>
    requires std::constructible_from<T, Arguments...>
  T& Cache(Arguments&&... arguments) {
    std::any& entry = EnsureCacheEntry(typeid(T));
    if (!entry.has_value()) {
      entry.emplace<T>(std::forward<Arguments>(arguments)...);
    }
    auto* value = std::any_cast<T>(&entry);
    if (value == nullptr) {
      throw std::logic_error("HuxerUI layout cache type mismatch");
    }
    return *value;
  }

protected:
  virtual std::size_t ChildCountImpl() const noexcept = 0;
  virtual MountedNode& ChildAtImpl(std::size_t index) = 0;
  virtual const MountedNode& ChildAtImpl(std::size_t index) const = 0;
  virtual Size MeasuredSizeImpl() const noexcept = 0;
  virtual Rect FrameImpl() const noexcept = 0;
  virtual Rect PresentationFrameImpl() const noexcept = 0;
  virtual float PresentationOpacityImpl() const noexcept = 0;
  virtual bool IsEnabledImpl() const noexcept = 0;
  virtual bool IsFocusedImpl() const noexcept = 0;
  virtual float SpacingImpl() const noexcept = 0;
  virtual float GrowFactorImpl() const noexcept = 0;
  virtual MainAxisAlignment MainAlignmentImpl() const noexcept = 0;
  virtual CrossAxisAlignment CrossAlignmentImpl() const noexcept = 0;
  virtual HorizontalAlignment HorizontalAlignmentImpl() const noexcept = 0;
  virtual VerticalAlignment VerticalAlignmentImpl() const noexcept = 0;
  virtual const std::any* FindLayoutValue(std::type_index key) const noexcept = 0;
  virtual std::any& EnsureCacheEntry(std::type_index key) = 0;
};

class LayoutContext {
public:
  [[nodiscard]] Size Measure(MountedNode& child, Constraints constraints) const {
    return measure_(state_, child, constraints);
  }

private:
  using MeasureFunction = Size (*)(void*, MountedNode&, Constraints);

  LayoutContext(void* state, MeasureFunction measure) : state_(state), measure_(measure) {}

  void* state_;
  MeasureFunction measure_;

  friend struct detail::LayoutContextAccess;
};

class LayoutResult {
public:
  struct Placement {
    MountedNode* child;
    Point offset;
  };

  LayoutResult& Place(MountedNode& child, Point offset) {
    placements_.push_back({&child, offset});
    return *this;
  }

  LayoutResult& SetSize(huxerui::Size size) noexcept {
    size_ = size;
    return *this;
  }

  [[nodiscard]] huxerui::Size MeasuredSize() const noexcept {
    return size_;
  }

  [[nodiscard]] const std::vector<Placement>& Placements() const noexcept {
    return placements_;
  }

private:
  huxerui::Size size_;
  std::vector<Placement> placements_;
};

namespace detail {

struct LayoutDescriptor {
  std::type_index type;
  LayoutResult (*measure)(LayoutContext&, MountedNode&, Constraints);
};

template <class Derived> const LayoutDescriptor& LayoutDescriptorFor() {
  static const LayoutDescriptor descriptor{
      typeid(Derived),
      [](LayoutContext& context, MountedNode& node, Constraints constraints) -> LayoutResult {
        return Derived::Measure(context, node, constraints);
      },
  };
  return descriptor;
}

} // namespace detail

} // namespace huxerui
