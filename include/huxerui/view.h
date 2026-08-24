#pragma once

#include <any>
#include <array>
#include <concepts>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/color.h>
#include <huxerui/event.h>
#include <huxerui/external_texture.h>
#include <huxerui/geometry.h>
#include <huxerui/layout.h>
#include <huxerui/modifier.h>
#include <huxerui/paint.h>
#include <huxerui/resource.h>
#include <huxerui/scroll.h>
#include <huxerui/state.h>
#include <huxerui/text.h>
#include <huxerui/text_input.h>
#include <huxerui/validation.h>
#include <huxerui/vector.h>
#include <huxerui/virtual_layout.h>

namespace huxerui {

class Environment;
class PaintContext;
class Runtime;

enum class TextRole {
  Body,
  Label,
  Title,
};

namespace detail {
struct ViewSpec;
struct PlatformEventDescriptor;
struct SegmentedButtonItemAccess;
struct TabItemAccess;
class VirtualMeasureSession;
} // namespace detail

class View {
public:
  View() = default;
  View(const View&) = default;
  View(View&&) noexcept = default;
  View& operator=(const View&) = default;
  View& operator=(View&&) noexcept = default;
  ~View() = default;

  template <class Function> View OnClick(Function&& function) && {
    ApplyEvent<ViewEvents::Click>(std::forward<Function>(function));
    return std::move(*this);
  }

  template <class Key, class Function>
    requires detail::EventKey<Key> && std::constructible_from<std::function<typename Key::Signature>, Function>
  View On(Function&& function) && {
    ApplyEvent<Key>(std::forward<Function>(function));
    return std::move(*this);
  }

  template <class Key> View LayoutValue(typename Key::Value value) && {
    ApplyLayoutValue<Key>(std::move(value));
    return std::move(*this);
  }

  template <ViewModifier... Modifiers> View With(Modifiers&&... modifiers) && {
    ApplyModifiers(std::forward<Modifiers>(modifiers)...);
    return std::move(*this);
  }

  View Key(std::int64_t value) &&;
  View Key(std::uint64_t value) &&;
  View Key(std::string value) &&;
  View Key(std::string_view value) &&;
  View Key(const char* value) &&;

  template <std::integral T>
    requires(!std::same_as<std::remove_cv_t<T>, bool>)
  View Key(T value) && {
    if constexpr (std::signed_integral<T>) {
      return std::move(*this).Key(static_cast<std::int64_t>(value));
    } else {
      return std::move(*this).Key(static_cast<std::uint64_t>(value));
    }
  }

  template <class T>
    requires std::is_enum_v<T>
  View Key(T value) && {
    using Underlying = std::underlying_type_t<T>;
    return std::move(*this).Key(static_cast<Underlying>(value));
  }

  [[nodiscard]] explicit operator bool() const noexcept {
    return static_cast<bool>(spec_);
  }

protected:
  explicit View(std::shared_ptr<detail::ViewSpec> spec);

  template <class Key, class Function> void ApplyEvent(Function&& function) {
    SetEventBinding(
        typeid(Key),
        std::make_shared<detail::EventHandler<typename Key::Signature>>(
            std::function<typename Key::Signature>(std::forward<Function>(function))
        )
    );
    if constexpr (std::same_as<Key, ViewEvents::Click>) {
      AddDefaultIndication();
    }
  }

  template <class Key> void ApplyLayoutValue(typename Key::Value value) {
    SetLayoutValue(typeid(Key), std::move(value));
  }

  template <class Value> void SetLayoutValue(std::type_index key, Value&& value) {
    SetErasedLayoutValue(key, detail::MakeErasedLayoutValue(std::forward<Value>(value)));
  }

  template <ViewModifier... Modifiers> void ApplyModifiers(Modifiers&&... modifiers) {
    (AddModifier(detail::MakeModifierSpec(std::forward<Modifiers>(modifiers))), ...);
  }

  void SetEventBinding(std::type_index key, std::shared_ptr<detail::EventHandlerBase> handler);
  void AddPlatformEvent(detail::PlatformEventDescriptor descriptor);
  void SetErasedLayoutValue(std::type_index key, detail::ErasedLayoutValue value);
  void AddDefaultIndication();
  void AddModifier(detail::ModifierSpec modifier);
  void SetModifier(detail::ModifierSpec modifier);
  void SetTextStyle(TextStyle style);
  void SetImageFit(ImageFit fit);
  void SetImageAlignment(HorizontalAlignment horizontal, VerticalAlignment vertical);
  void SetImageSampling(ImageSampling sampling);
  void SetImageTint(std::optional<Color> tint);
  void SetKey(std::int64_t value);
  void SetKey(std::uint64_t value);
  void SetKey(std::string value);

private:
  void EnsureUniqueSpec();

  std::shared_ptr<detail::ViewSpec> spec_;

  friend View ProvideEnvironment(Environment environment, View content);
  friend class Runtime;
  friend class detail::VirtualMeasureSession;
};

namespace detail {

std::shared_ptr<ViewSpec> MakeLayoutSpec(const LayoutDescriptor& layout, std::vector<View> children);

template <class Factory, class... Arguments>
concept ViewFactoryFor =
    std::copy_constructible<std::decay_t<Factory>> && (std::copy_constructible<std::decay_t<Arguments>> && ...) &&
    requires(std::decay_t<Factory>& factory, const std::decay_t<Arguments>&... arguments) {
      { std::invoke(factory, arguments...) } -> std::convertible_to<View>;
    };

template <class Factory, class... Arguments>
  requires ViewFactoryFor<Factory, Arguments...>
std::function<View()> BindViewFactory(Factory&& factory, Arguments&&... arguments) {
  using StoredFactory = std::decay_t<Factory>;
  using StoredArguments = std::tuple<std::decay_t<Arguments>...>;
  StoredFactory stored_factory(std::forward<Factory>(factory));
  StoredArguments stored_arguments(std::forward<Arguments>(arguments)...);
  return [factory = std::move(stored_factory), arguments = std::move(stored_arguments)]() mutable -> View {
    return std::apply([&factory](const auto&... values) -> View { return std::invoke(factory, values...); }, arguments);
  };
}

} // namespace detail

class Views {
public:
  Views() = default;

  explicit Views(std::vector<View> items) : items_(std::move(items)) {}

  void Reserve(std::size_t capacity) {
    items_.reserve(capacity);
  }

  void Add(View view) {
    items_.push_back(std::move(view));
  }

  [[nodiscard]] std::size_t Size() const noexcept {
    return items_.size();
  }

  [[nodiscard]] const std::vector<View>& Items() const noexcept {
    return items_;
  }

  [[nodiscard]] std::vector<View> Take() && {
    return std::move(items_);
  }

private:
  std::vector<View> items_;
};

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

namespace detail {

template <class T> std::string FormatText(const T& value) {
  if constexpr (std::is_same_v<std::decay_t<T>, std::string>) {
    return value;
  } else if constexpr (std::is_same_v<std::decay_t<T>, std::string_view>) {
    return std::string(value);
  } else if constexpr (std::is_same_v<std::decay_t<T>, const char*> || std::is_same_v<std::decay_t<T>, char*>) {
    return value == nullptr ? std::string{} : std::string(value);
  } else {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  }
}

template <class T> std::string FormatText(const State<T>& value) {
  return FormatText(value.Get());
}

template <class... Arguments> std::string InterpolateText(std::string_view format, const Arguments&... arguments) {
  const std::array<std::string, sizeof...(Arguments)> values{
      FormatText(arguments)...,
  };

  std::string result;
  result.reserve(format.size());
  std::size_t argument_index = 0;

  for (std::size_t index = 0; index < format.size();) {
    const char character = format[index];
    if (character == '{' && index + 1 < format.size()) {
      const char next = format[index + 1];
      if (next == '{') {
        result.push_back('{');
        index += 2;
        continue;
      }
      if (next == '}') {
        if (argument_index >= values.size()) {
          throw std::invalid_argument("HuxerUI text format has fewer arguments than placeholders");
        }
        result += values[argument_index++];
        index += 2;
        continue;
      }
    }

    if (character == '}' && index + 1 < format.size() && format[index + 1] == '}') {
      result.push_back('}');
      index += 2;
      continue;
    }

    result.push_back(character);
    ++index;
  }

  if (argument_index != values.size()) {
    throw std::invalid_argument("HuxerUI text format has more arguments than placeholders");
  }
  return result;
}

template <class T>
concept ViewChild = std::convertible_to<T, View> || std::same_as<std::remove_cvref_t<T>, Views>;

template <class Child> std::size_t ChildCount(const Child& child) {
  if constexpr (std::same_as<std::remove_cvref_t<Child>, Views>) {
    return child.Size();
  } else {
    return 1;
  }
}

template <class Child>
  requires std::convertible_to<Child, View>
void AppendChild(std::vector<View>& result, Child&& child) {
  result.emplace_back(std::forward<Child>(child));
}

inline void AppendChild(std::vector<View>& result, const Views& children) {
  result.insert(result.end(), children.Items().begin(), children.Items().end());
}

inline void AppendChild(std::vector<View>& result, Views&& children) {
  std::vector<View> items = std::move(children).Take();
  result.insert(result.end(), std::make_move_iterator(items.begin()), std::make_move_iterator(items.end()));
}

template <ViewChild... Children> std::vector<View> CollectChildren(Children&&... children) {
  std::vector<View> result;
  result.reserve((ChildCount(children) + ... + 0));
  (AppendChild(result, std::forward<Children>(children)), ...);
  return result;
}

struct VirtualItemSource {
  std::size_t size = 0;
  std::function<View(std::size_t)> factory;
};

template <std::ranges::input_range Range, class Factory>
  requires std::constructible_from<std::ranges::range_value_t<Range>, std::ranges::range_reference_t<Range>> &&
           std::invocable<Factory&, std::ranges::range_reference_t<Range>> &&
           std::convertible_to<std::invoke_result_t<Factory&, std::ranges::range_reference_t<Range>>, View>
VirtualItemSource MakeVirtualItemSource(Range&& range, Factory&& factory) {
  using Value = std::ranges::range_value_t<Range>;
  auto values = std::make_shared<std::vector<Value>>();
  if constexpr (std::ranges::sized_range<Range>) {
    values->reserve(static_cast<std::size_t>(std::ranges::size(range)));
  }
  for (auto&& value : range) {
    values->emplace_back(value);
  }

  auto shared_factory = std::make_shared<std::decay_t<Factory>>(std::forward<Factory>(factory));
  return {
      values->size(),
      [values = std::move(values), shared_factory = std::move(shared_factory)](std::size_t index) -> View {
        return std::invoke(*shared_factory, (*values)[index]);
      },
  };
}

template <class Factory>
  requires std::invocable<Factory&, std::size_t> &&
           std::convertible_to<std::invoke_result_t<Factory&, std::size_t>, View>
VirtualItemSource MakeVirtualItemSource(std::size_t item_count, Factory&& factory) {
  auto shared_factory = std::make_shared<std::decay_t<Factory>>(std::forward<Factory>(factory));
  return {
      item_count,
      [shared_factory = std::move(shared_factory)](std::size_t index) -> View {
        return std::invoke(*shared_factory, index);
      },
  };
}

} // namespace detail

namespace detail {

std::shared_ptr<ViewSpec> MakeVirtualLayoutSpec(const VirtualLayoutDescriptor& layout, VirtualItemSource source);

template <class Derived> class TypedView : public View {
public:
  template <class Function> Derived OnClick(Function&& function) && {
    this->template ApplyEvent<ViewEvents::Click>(std::forward<Function>(function));
    return TakeDerived();
  }

  template <class Key, class Function>
    requires detail::EventKey<Key> && std::constructible_from<std::function<typename Key::Signature>, Function>
  Derived On(Function&& function) && {
    this->template ApplyEvent<Key>(std::forward<Function>(function));
    return TakeDerived();
  }

  template <class Key> Derived LayoutValue(typename Key::Value value) && {
    this->template ApplyLayoutValue<Key>(std::move(value));
    return TakeDerived();
  }

  template <ViewModifier... Modifiers> Derived With(Modifiers&&... modifiers) && {
    this->ApplyModifiers(std::forward<Modifiers>(modifiers)...);
    return TakeDerived();
  }

  Derived Key(std::int64_t value) && {
    this->SetKey(value);
    return TakeDerived();
  }

  Derived Key(std::uint64_t value) && {
    this->SetKey(value);
    return TakeDerived();
  }

  Derived Key(std::string value) && {
    this->SetKey(std::move(value));
    return TakeDerived();
  }

  Derived Key(std::string_view value) && {
    return std::move(*this).Key(std::string(value));
  }

  Derived Key(const char* value) && {
    if (value == nullptr) {
      throw std::invalid_argument("HuxerUI key string must not be null");
    }
    return std::move(*this).Key(std::string(value));
  }

  template <std::integral T>
    requires(!std::same_as<std::remove_cv_t<T>, bool>)
  Derived Key(T value) && {
    if constexpr (std::signed_integral<T>) {
      return std::move(*this).Key(static_cast<std::int64_t>(value));
    } else {
      return std::move(*this).Key(static_cast<std::uint64_t>(value));
    }
  }

  template <class T>
    requires std::is_enum_v<T>
  Derived Key(T value) && {
    using Underlying = std::underlying_type_t<T>;
    return std::move(*this).Key(static_cast<Underlying>(value));
  }

protected:
  explicit TypedView(std::shared_ptr<ViewSpec> spec) : View(std::move(spec)) {}
  explicit TypedView(View view) : View(std::move(view)) {}

private:
  Derived TakeDerived() {
    return std::move(static_cast<Derived&>(*this));
  }
};

} // namespace detail

template <class Derived> class Layout : public detail::TypedView<Derived> {
public:
  explicit Layout(std::vector<View> children)
      : detail::TypedView<Derived>(
            detail::MakeLayoutSpec(detail::LayoutDescriptorFor<Derived>(), std::move(children))
        ) {}

  template <class... Children>
    requires(detail::ViewChild<Children> && ...)
  explicit Layout(Children&&... children) : Layout(detail::CollectChildren(std::forward<Children>(children)...)) {}

protected:
  explicit Layout(std::shared_ptr<detail::ViewSpec> spec) : detail::TypedView<Derived>(std::move(spec)) {}
};

template <class Derived> class VirtualLayout : public detail::TypedView<Derived> {
public:
  Derived Controller(huxerui::ScrollController controller) && {
    this->SetLayoutValue(typeid(detail::ScrollControllerBinding), std::move(controller));
    return std::move(static_cast<Derived&>(*this));
  }

  template <std::ranges::input_range Range, class Factory>
    requires std::constructible_from<std::ranges::range_value_t<Range>, std::ranges::range_reference_t<Range>> &&
             std::invocable<Factory&, std::ranges::range_reference_t<Range>> &&
             std::convertible_to<std::invoke_result_t<Factory&, std::ranges::range_reference_t<Range>>, View>
  explicit VirtualLayout(Range&& range, Factory&& factory)
      : VirtualLayout(detail::MakeVirtualItemSource(std::forward<Range>(range), std::forward<Factory>(factory))) {}

  template <class Range, class Factory>
    requires std::ranges::input_range<const Range&> &&
             std::invocable<Factory&, std::ranges::range_reference_t<const Range&>> &&
             std::convertible_to<std::invoke_result_t<Factory&, std::ranges::range_reference_t<const Range&>>, View>
  explicit VirtualLayout(const State<Range>& range, Factory&& factory)
      : VirtualLayout(range.Get(), std::forward<Factory>(factory)) {}

  template <class Factory>
    requires std::invocable<Factory&, std::size_t> &&
             std::convertible_to<std::invoke_result_t<Factory&, std::size_t>, View>
  VirtualLayout(std::size_t item_count, Factory&& factory)
      : VirtualLayout(detail::MakeVirtualItemSource(item_count, std::forward<Factory>(factory))) {}

protected:
  explicit VirtualLayout(detail::VirtualItemSource source)
      : detail::TypedView<Derived>(
            detail::MakeVirtualLayoutSpec(detail::VirtualLayoutDescriptorFor<Derived>(), std::move(source))
        ) {}
};

class Text final : public View {
public:
  explicit Text(StringVariant value, TextRole role = TextRole::Body);

  Text Style(TextStyle style) &&;

  template <class... Arguments> static Text Format(std::string_view format, const Arguments&... arguments) {
    return Text(detail::InterpolateText(format, arguments...));
  }

  template <class... Arguments> static Text Format(StringResource resource, const Arguments&... arguments) {
    return Text(StringVariant::Format(std::move(resource), arguments...));
  }

  template <class... Arguments>
  static Text Format(TextRole role, std::string_view format, const Arguments&... arguments) {
    return Text(detail::InterpolateText(format, arguments...), role);
  }

  template <class... Arguments>
  static Text Format(TextRole role, StringResource resource, const Arguments&... arguments) {
    return Text(StringVariant::Format(std::move(resource), arguments...), role);
  }

  template <class T>
  explicit Text(const State<T>& value, TextRole role = TextRole::Body) : Text(detail::FormatText(value), role) {}
};

class Button final : public View {
public:
  explicit Button(StringVariant label);
};

class IconButton final : public detail::TypedView<IconButton> {
public:
  IconButton(ImageVariant icon, StringVariant semantic_label);
};

class Chip final : public detail::TypedView<Chip> {
public:
  explicit Chip(StringVariant label);
  Chip(StringVariant label, bool selected);

  Chip(ImageVariant icon, StringVariant label);
  Chip(ImageVariant icon, StringVariant label, bool selected);

  template <class Function> Chip OnChanged(Function&& function) && {
    return std::move(*this).On<ToggleEvents::Changed>(std::forward<Function>(function));
  }
};

class Divider final : public View {
public:
  explicit Divider(Axis axis = Axis::Horizontal);
};

class SegmentedButtonItem final {
public:
  explicit SegmentedButtonItem(StringVariant label);
  SegmentedButtonItem(ImageVariant icon, StringVariant label);

  static SegmentedButtonItem IconOnly(ImageVariant icon, StringVariant semantic_label);

private:
  std::optional<ImageVariant> icon_;
  StringVariant label_;
  bool show_label_ = true;

  friend struct detail::SegmentedButtonItemAccess;
};

class SegmentedButton final : public detail::TypedView<SegmentedButton> {
public:
  SegmentedButton(std::initializer_list<StringVariant> labels, std::size_t selected_index)
      : SegmentedButton(std::vector<StringVariant>(labels), selected_index) {}
  SegmentedButton(std::initializer_list<StringVariant> labels, const State<std::size_t>& selected_index)
      : SegmentedButton(std::vector<StringVariant>(labels), selected_index.Get()) {}
  SegmentedButton(std::vector<StringVariant> labels, std::size_t selected_index);
  SegmentedButton(std::vector<StringVariant> labels, const State<std::size_t>& selected_index)
      : SegmentedButton(std::move(labels), selected_index.Get()) {}
  SegmentedButton(std::vector<SegmentedButtonItem> items, std::size_t selected_index);
  SegmentedButton(std::vector<SegmentedButtonItem> items, const State<std::size_t>& selected_index)
      : SegmentedButton(std::move(items), selected_index.Get()) {}

  template <class Function> SegmentedButton OnChanged(Function&& function) && {
    return std::move(*this).On<SegmentedButtonEvents::Changed>(std::forward<Function>(function));
  }
};

class TabItem final {
public:
  explicit TabItem(StringVariant label);
  TabItem(ImageVariant icon, StringVariant label);

  static TabItem IconOnly(ImageVariant icon, StringVariant semantic_label);

  TabItem Enabled(bool enabled) &&;

private:
  std::optional<ImageVariant> icon_;
  StringVariant label_;
  bool show_label_ = true;
  bool enabled_ = true;

  friend struct detail::TabItemAccess;
};

class Tabs final : public detail::TypedView<Tabs> {
public:
  Tabs(std::initializer_list<StringVariant> labels, std::size_t selected_index)
      : Tabs(std::vector<StringVariant>(labels), selected_index) {}
  Tabs(std::initializer_list<StringVariant> labels, const State<std::size_t>& selected_index)
      : Tabs(std::vector<StringVariant>(labels), selected_index.Get()) {}
  Tabs(std::vector<StringVariant> labels, std::size_t selected_index);
  Tabs(std::vector<StringVariant> labels, const State<std::size_t>& selected_index)
      : Tabs(std::move(labels), selected_index.Get()) {}
  Tabs(std::vector<TabItem> items, std::size_t selected_index);
  Tabs(std::vector<TabItem> items, const State<std::size_t>& selected_index)
      : Tabs(std::move(items), selected_index.Get()) {}

  template <class Function> Tabs OnChanged(Function&& function) && {
    return std::move(*this).On<TabsEvents::Changed>(std::forward<Function>(function));
  }
};

class Image final : public View {
public:
  explicit Image(ImageVariant image);
  explicit Image(ExternalTexture texture);

  Image Fit(ImageFit fit) &&;
  Image Align(HorizontalAlignment horizontal, VerticalAlignment vertical) &&;
  Image Sampling(ImageSampling sampling) &&;
  Image Tint(Color tint) &&;
};

using CanvasPainter = std::function<void(PaintContext&, Size)>;

class Canvas final : public View {
public:
  explicit Canvas(CanvasPainter painter);
};

enum class TextFieldVariant {
  Filled,
  Outlined,
  Standard,
};

class TextFieldLineLimits final {
public:
  static TextFieldLineLimits SingleLine() noexcept;
  static TextFieldLineLimits MultiLine(std::size_t minimum = 1);
  static TextFieldLineLimits MultiLine(std::size_t minimum, std::size_t maximum);

  [[nodiscard]] bool IsMultiline() const noexcept {
    return multiline_;
  }

  [[nodiscard]] std::size_t Minimum() const noexcept {
    return minimum_;
  }

  [[nodiscard]] std::optional<std::size_t> Maximum() const noexcept {
    return maximum_;
  }

  bool operator==(const TextFieldLineLimits&) const = default;

private:
  TextFieldLineLimits(bool multiline, std::size_t minimum, std::optional<std::size_t> maximum) noexcept
      : multiline_(multiline), minimum_(minimum), maximum_(maximum) {}

  bool multiline_ = false;
  std::size_t minimum_ = 1;
  std::optional<std::size_t> maximum_;
};

class TextField final : public detail::TypedView<TextField> {
public:
  explicit TextField(TextEditingValue value);
  explicit TextField(const State<TextEditingValue>& value) : TextField(value.Get()) {}

  TextField Label(StringVariant value) &&;
  TextField Placeholder(StringVariant value) &&;
  TextField LeadingIcon(ImageVariant icon) &&;
  TextField TrailingIcon(ImageVariant icon) &&;
  TextField Variant(TextFieldVariant value) &&;
  TextField LineLimits(TextFieldLineLimits value) &&;
  TextField MaxLength(std::size_t value) &&;
  TextField Validation(ValidationResult value) &&;
  TextField Secure() &&;
  TextField InputConfiguration(TextInputConfiguration configuration) &&;

  template <class Function> TextField OnChanged(Function&& function) && {
    return std::move(*this).On<TextFieldEvents::Changed>(std::forward<Function>(function));
  }

  template <class Function> TextField OnSubmitted(Function&& function) && {
    return std::move(*this).On<TextFieldEvents::Submitted>(std::forward<Function>(function));
  }

private:
  void UpdateModifier();

  TextEditingValue value_;
  StringVariant label_;
  StringVariant placeholder_;
  std::optional<ImageVariant> leading_icon_;
  std::optional<ImageVariant> trailing_icon_;
  std::optional<TextFieldVariant> variant_;
  TextInputConfiguration configuration_;
  TextFieldLineLimits line_limits_ = TextFieldLineLimits::SingleLine();
  std::optional<std::size_t> max_length_;
  ValidationResult validation_;
};

class Checkbox final : public detail::TypedView<Checkbox> {
public:
  explicit Checkbox(bool checked);
  explicit Checkbox(const State<bool>& checked) : Checkbox(checked.Get()) {}
  Checkbox(StringVariant label, bool checked);
  Checkbox(StringVariant label, const State<bool>& checked) : Checkbox(std::move(label), checked.Get()) {}

  template <class Function> Checkbox OnChanged(Function&& function) && {
    return std::move(*this).On<ToggleEvents::Changed>(std::forward<Function>(function));
  }
};

class RadioButton final : public detail::TypedView<RadioButton> {
public:
  explicit RadioButton(bool selected);
  explicit RadioButton(const State<bool>& selected) : RadioButton(selected.Get()) {}
  RadioButton(StringVariant label, bool selected);
  RadioButton(StringVariant label, const State<bool>& selected) : RadioButton(std::move(label), selected.Get()) {}

  template <class Function> RadioButton OnChanged(Function&& function) && {
    return std::move(*this).On<ToggleEvents::Changed>(std::forward<Function>(function));
  }
};

class Switch final : public detail::TypedView<Switch> {
public:
  explicit Switch(bool checked);
  explicit Switch(const State<bool>& checked) : Switch(checked.Get()) {}
  Switch(StringVariant label, bool checked);
  Switch(StringVariant label, const State<bool>& checked) : Switch(std::move(label), checked.Get()) {}

  template <class Function> Switch OnChanged(Function&& function) && {
    return std::move(*this).On<ToggleEvents::Changed>(std::forward<Function>(function));
  }
};

class ProgressCircle final : public detail::TypedView<ProgressCircle> {
public:
  ProgressCircle();
  explicit ProgressCircle(float progress);
  explicit ProgressCircle(const State<float>& progress) : ProgressCircle(progress.Get()) {}
};

class ProgressBar final : public detail::TypedView<ProgressBar> {
public:
  ProgressBar();
  explicit ProgressBar(float progress);
  explicit ProgressBar(const State<float>& progress) : ProgressBar(progress.Get()) {}
};

class Slider final : public detail::TypedView<Slider> {
public:
  explicit Slider(float value);
  explicit Slider(const State<float>& value) : Slider(value.Get()) {}

  Slider Range(float minimum, float maximum) &&;
  Slider Step(float step) &&;

  template <class Function> Slider OnChanged(Function&& function) && {
    return std::move(*this).On<SliderEvents::Changed>(std::forward<Function>(function));
  }

private:
  void UpdateModifier();

  float value_ = 0.0F;
  float minimum_ = 0.0F;
  float maximum_ = 1.0F;
  std::optional<float> step_;
};

class Scope final : public View {
public:
  explicit Scope(std::function<View()> factory);

  template <class Factory, class... Arguments>
    requires detail::ViewFactoryFor<Factory, Arguments...>
  explicit Scope(Factory&& factory, Arguments&&... arguments)
      : Scope(detail::BindViewFactory(std::forward<Factory>(factory), std::forward<Arguments>(arguments)...)) {}
};

class SelectionArea final : public View {
public:
  explicit SelectionArea(View content);
};

class Spacer final : public View {
public:
  Spacer();
};

class Column final : public Layout<Column> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints);
};

class Row final : public Layout<Row> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints);
};

class Flow final : public Layout<Flow> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints);
};

class Stack final : public Layout<Stack> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints);
};

class ScrollView final : public detail::TypedView<ScrollView> {
public:
  explicit ScrollView(View content);

  ScrollView ScrollAxis(Axis axis) &&;
  ScrollView Controller(huxerui::ScrollController controller) &&;
};

class VirtualList final : public VirtualLayout<VirtualList> {
public:
  using VirtualLayout::VirtualLayout;

  VirtualList ScrollAxis(Axis axis) &&;
  VirtualList ItemExtent(float extent) &&;
  VirtualList EstimatedItemExtent(float extent) &&;
  VirtualList CacheExtent(float extent) &&;

  static VirtualLayoutResult Measure(VirtualLayoutContext& context, MountedNode& node, Constraints constraints);
  static std::optional<float>
  ScrollOffsetForItem(MountedNode& node, std::size_t index, ScrollAlignment alignment, float viewport_extent);
};

class VirtualGrid final : public VirtualLayout<VirtualGrid> {
public:
  using VirtualLayout::VirtualLayout;

  VirtualGrid Columns(GridColumns columns) &&;
  VirtualGrid RowExtent(float extent) &&;
  VirtualGrid EstimatedRowExtent(float extent) &&;
  VirtualGrid RowSpacing(float spacing) &&;
  VirtualGrid ColumnSpacing(float spacing) &&;
  VirtualGrid CacheExtent(float extent) &&;
  VirtualGrid ItemSpans(std::vector<std::size_t> spans) &&;

  static VirtualLayoutResult Measure(VirtualLayoutContext& context, MountedNode& node, Constraints constraints);
  static std::optional<float>
  ScrollOffsetForItem(MountedNode& node, std::size_t index, ScrollAlignment alignment, float viewport_extent);
};

} // namespace huxerui

// clang-format off
#define HUXERUI_SCOPE(...) return ::huxerui::Scope([=]() -> ::huxerui::View __VA_ARGS__)

#define HUXERUI_SCOPE_BEGIN \
  return ::huxerui::Scope([=]() -> ::huxerui::View {

#define HUXERUI_SCOPE_END \
  });
// clang-format on
