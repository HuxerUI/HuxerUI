#pragma once

#include <any>
#include <array>
#include <concepts>
#include <cstddef>
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
#include <huxerui/platform_registry.h>
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

/// Selects the theme typography role used by Text when no explicit TextStyle overrides it.
enum class TextRole {
  Body,
  Label,
  Title,
};

namespace detail {
struct ViewSpec;
struct PlatformEventDescriptor;
std::shared_ptr<ViewSpec> MakePlatformViewSpec(std::string name, PlatformValue properties);
struct SegmentedButtonItemAccess;
struct TabItemAccess;
class VirtualMeasureSession;
} // namespace detail

/// A transient copy-on-write UI declaration.
///
/// Views describe the current UI tree; retained state belongs to composition state or mounted extensions. Fluent
/// operations consume the declaration and return its updated value.
/// @code
/// return Text("Status")
///     .Key("status")
///     .With(Padding(8.0F), Foreground(Color::Rgb(33, 111, 219)))
///     .On<ViewEvents::Pointer>([](const PointerEvent&) {});
/// @endcode
class View {
public:
  View() = default;
  View(const View&) = default;
  View(View&&) noexcept = default;
  View& operator=(const View&) = default;
  View& operator=(View&&) noexcept = default;
  ~View() = default;

  /// Handles semantic activation through the built-in Click event key.
  template <class Function> View OnClick(Function&& function) && {
    ApplyEvent<ViewEvents::Click>(std::forward<Function>(function));
    return std::move(*this);
  }

  /// Handles one typed event on this View.
  ///
  /// The event key defines both handler identity and signature. A later binding for the same key replaces the earlier
  /// binding on this declaration.
  template <class Key, class Function>
    requires detail::EventKey<Key> && std::constructible_from<std::function<typename Key::Signature>, Function>
  View On(Function&& function) && {
    ApplyEvent<Key>(std::forward<Function>(function));
    return std::move(*this);
  }

  /// Attaches typed metadata consumed by the compatible parent layout.
  ///
  /// Layout values describe parent-child participation, such as Grow, without becoming ordinary View properties.
  template <class Key> View LayoutValue(typename Key::Value value) && {
    ApplyLayoutValue<Key>(std::move(value));
    return std::move(*this);
  }

  /// Applies property or retained modifiers from left to right.
  template <ViewModifier... Modifiers> View With(Modifiers&&... modifiers) && {
    ApplyModifiers(std::forward<Modifiers>(modifiers)...);
    return std::move(*this);
  }

  /// Sets stable sibling identity used during reconciliation and state preservation.
  ///
  /// Use a semantic data identifier when stateful siblings can insert, remove, or reorder.
  /// @{
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
  /// @}

  /// Reports whether this declaration contains a View.
  ///
  /// An empty View is useful for conditional content and is not mounted.
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
    if constexpr (detail::PlatformEventKey<Key>) {
      AddPlatformEvent(detail::MakePlatformEventDescriptor<Key>());
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
  void SetPlatformController(PlatformValue controller);
  void SetErasedLayoutValue(std::type_index key, detail::ErasedLayoutValue value);
  void AddDefaultIndication();
  void AddModifier(detail::ModifierSpec modifier);
  void SetModifier(detail::ModifierSpec modifier);
  void SetTextStyle(TextStyle style);
  void SetTextAlign(TextAlign align);
  void SetTextVerticalAlign(TextVerticalAlign align);
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

/// Declares a platform-owned view embedded in the shared HuxerUI tree.
///
/// The stable string name selects the registered PlatformView type. Properties and an optional controller remain
/// strongly typed in direct C++ integrations, while typed platform events use the ordinary On API.
/// @code
/// return PlatformView("map", MapProperties{.latitude = 51.5, .longitude = -0.1})
///     .Controller(MapController{})
///     .On<MapEvents::MarkerSelected>([](std::string marker) { ShowMarker(std::move(marker)); });
/// @endcode
class PlatformView final : public View {
public:
  explicit PlatformView(std::string name);

  template <class Properties>
    requires std::move_constructible<std::decay_t<Properties>> && std::equality_comparable<std::decay_t<Properties>>
  PlatformView(std::string name, Properties&& properties)
      : View(detail::MakePlatformViewSpec(std::move(name),
                                          PlatformValue::Store(std::forward<Properties>(properties)))) {}

  /// Handles an event declared by the registered PlatformView contract.
  template <class Key, class Function>
    requires detail::PlatformEventKey<Key> && std::constructible_from<std::function<typename Key::Signature>, Function>
  PlatformView On(Function&& function) && {
    ApplyEvent<Key>(std::forward<Function>(function));
    return std::move(*this);
  }

  /// Supplies the typed controller connected to this mounted PlatformView instance.
  ///
  /// Connection and disconnection follow the PlatformView lifecycle and do not imply that controllers are singletons.
  template <class ControllerValue>
    requires std::move_constructible<std::decay_t<ControllerValue>> &&
             std::equality_comparable<std::decay_t<ControllerValue>>
  PlatformView Controller(ControllerValue&& controller) && {
    SetPlatformController(PlatformValue::Store(std::forward<ControllerValue>(controller)));
    return std::move(*this);
  }
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

/// A movable group of View declarations accepted as children by layout containers.
///
/// ForEach returns this type so a dynamic range can be inserted directly beside ordinary children.
class Views {
public:
  Views() = default;

  explicit Views(std::vector<View> items) : items_(std::move(items)) {}

  /// Reserves storage without changing the number of declarations.
  void Reserve(std::size_t capacity) {
    items_.reserve(capacity);
  }

  /// Appends one declaration to the group.
  void Add(View view) {
    items_.push_back(std::move(view));
  }

  /// Returns the number of declarations in the group.
  [[nodiscard]] std::size_t Size() const noexcept {
    return items_.size();
  }

  /// Returns the declarations without transferring ownership.
  [[nodiscard]] const std::vector<View>& Items() const noexcept {
    return items_;
  }

  /// Transfers all declarations out of this group.
  [[nodiscard]] std::vector<View> Take() && {
    return std::move(items_);
  }

private:
  std::vector<View> items_;
};

/// Eagerly maps an input range to a group of View declarations.
///
/// Add a stable Key to stateful results when the range can reorder.
/// @code
/// return Column {
///   ForEach(records, [](const Record& record) { return RecordRow(record).Key(record.id); }),
/// };
/// @endcode
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

/// Eagerly maps the current value of a controlled range State to View declarations.
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

struct ViewItemSource {
  std::size_t size = 0;
  std::function<View(std::size_t)> factory;
};

struct ComboBoxSuggestionDeclaration {
  StringVariant text;
  View content;
};

struct ComboBoxSuggestionSource {
  std::size_t size = 0;
  std::function<ComboBoxSuggestionDeclaration(std::size_t)> factory;
};

template <std::ranges::input_range Range, class Factory>
  requires std::constructible_from<std::ranges::range_value_t<Range>, std::ranges::range_reference_t<Range>> &&
           std::invocable<Factory&, std::ranges::range_reference_t<Range>> &&
           std::convertible_to<std::invoke_result_t<Factory&, std::ranges::range_reference_t<Range>>, View>
ViewItemSource MakeViewItemSource(Range&& range, Factory&& factory) {
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
ViewItemSource MakeViewItemSource(std::size_t item_count, Factory&& factory) {
  auto shared_factory = std::make_shared<std::decay_t<Factory>>(std::forward<Factory>(factory));
  return {
      item_count,
      [shared_factory = std::move(shared_factory)](std::size_t index) -> View {
        return std::invoke(*shared_factory, index);
      },
  };
}

template <std::ranges::input_range Range, class TextFactory, class ContentFactory>
  requires std::constructible_from<std::ranges::range_value_t<Range>, std::ranges::range_reference_t<Range>> &&
           std::invocable<TextFactory&, std::ranges::range_reference_t<Range>> &&
           std::constructible_from<
               StringVariant,
               std::invoke_result_t<TextFactory&, std::ranges::range_reference_t<Range>>> &&
           std::invocable<ContentFactory&, std::ranges::range_reference_t<Range>> &&
           std::convertible_to<
               std::invoke_result_t<ContentFactory&, std::ranges::range_reference_t<Range>>,
               View>
ComboBoxSuggestionSource MakeComboBoxSuggestionSource(
    Range&& range, TextFactory&& text_factory, ContentFactory&& content_factory
) {
  using Value = std::ranges::range_value_t<Range>;
  auto values = std::make_shared<std::vector<Value>>();
  if constexpr (std::ranges::sized_range<Range>) {
    values->reserve(static_cast<std::size_t>(std::ranges::size(range)));
  }
  for (auto&& value : range) {
    values->emplace_back(value);
  }

  auto shared_text_factory = std::make_shared<std::decay_t<TextFactory>>(std::forward<TextFactory>(text_factory));
  auto shared_content_factory =
      std::make_shared<std::decay_t<ContentFactory>>(std::forward<ContentFactory>(content_factory));
  return {
      values->size(),
      [values = std::move(values), shared_text_factory = std::move(shared_text_factory),
       shared_content_factory = std::move(shared_content_factory)](std::size_t index) {
        auto& value = (*values)[index];
        return ComboBoxSuggestionDeclaration{
            StringVariant(std::invoke(*shared_text_factory, value)),
            std::invoke(*shared_content_factory, value),
        };
      },
  };
}

} // namespace detail

namespace detail {

std::shared_ptr<ViewSpec> MakeVirtualLayoutSpec(const VirtualLayoutDescriptor& layout, ViewItemSource source);
std::shared_ptr<ViewSpec> MakeSelectSpec(ViewItemSource source, std::size_t selected_index);
std::shared_ptr<ViewSpec> MakeComboBoxSpec(ComboBoxSuggestionSource source, TextEditingValue value);

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

/// Base declaration for layouts that eagerly own ordinary child Views.
///
/// A custom Derived type supplies a static Measure function. Runtime owns reconciliation and calls the layout policy
/// with the current children and constraints.
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

/// Base declaration for layouts that materialize logical items only when measurement requires them.
///
/// A range input is copied into one declaration snapshot. Item factories must remain declarative because
/// virtual measurement may request the same logical item more than once.
template <class Derived> class VirtualLayout : public detail::TypedView<Derived> {
public:
  /// Connects a stable controller for programmatic scrolling and metric observation.
  Derived Controller(huxerui::ScrollController controller) && {
    this->SetLayoutValue(typeid(detail::ScrollControllerBinding), std::move(controller));
    return std::move(static_cast<Derived&>(*this));
  }

  template <std::ranges::input_range Range, class Factory>
    requires std::constructible_from<std::ranges::range_value_t<Range>, std::ranges::range_reference_t<Range>> &&
             std::invocable<Factory&, std::ranges::range_reference_t<Range>> &&
             std::convertible_to<std::invoke_result_t<Factory&, std::ranges::range_reference_t<Range>>, View>
  explicit VirtualLayout(Range&& range, Factory&& factory)
      : VirtualLayout(detail::MakeViewItemSource(std::forward<Range>(range), std::forward<Factory>(factory))) {}

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
      : VirtualLayout(detail::MakeViewItemSource(item_count, std::forward<Factory>(factory))) {}

protected:
  explicit VirtualLayout(detail::ViewItemSource source)
      : detail::TypedView<Derived>(
            detail::MakeVirtualLayoutSpec(detail::VirtualLayoutDescriptorFor<Derived>(), std::move(source))
        ) {}
};

/// Displays localized or literal text using theme typography or an explicit TextStyle.
/// @code
/// return Text("Account", TextRole::Title)
///     .Style(TextStyle{.font = Font::System(18.0F).WithWeight(FontWeight::SemiBold)})
///     .Align(TextAlign::Center);
/// @endcode
class Text final : public View {
public:
  explicit Text(StringVariant value, TextRole role = TextRole::Body);

  /// Replaces the resolved typography for this Text declaration.
  Text Style(TextStyle style) &&;
  /// Sets horizontal paragraph alignment inside the measured text bounds.
  Text Align(TextAlign align) &&;
  /// Sets vertical paragraph alignment when the Text receives extra height.
  Text VerticalAlign(TextVerticalAlign align) &&;

  /// Formats a literal string by replacing each `{}` placeholder in order.
  ///
  /// Use `{{` and `}}` for literal braces. A placeholder-count mismatch throws `std::invalid_argument`.
  template <class... Arguments> static Text Format(std::string_view format, const Arguments&... arguments) {
    return Text(detail::InterpolateText(format, arguments...));
  }

  /// Formats a localized string resource with its declared arguments.
  template <class... Arguments> static Text Format(StringResource resource, const Arguments&... arguments) {
    return Text(StringVariant::Format(std::move(resource), arguments...));
  }

  /// Formats literal text and applies the requested typography role.
  template <class... Arguments>
  static Text Format(TextRole role, std::string_view format, const Arguments&... arguments) {
    return Text(detail::InterpolateText(format, arguments...), role);
  }

  /// Formats localized text and applies the requested typography role.
  template <class... Arguments>
  static Text Format(TextRole role, StringResource resource, const Arguments&... arguments) {
    return Text(StringVariant::Format(std::move(resource), arguments...), role);
  }

  template <class T>
  explicit Text(const State<T>& value, TextRole role = TextRole::Body) : Text(detail::FormatText(value), role) {}
};

/// Presents a labeled semantic action control.
///
/// Bind activation with OnClick and use Enabled to control interaction.
/// @code
/// return Button("Save").OnClick([] { SaveDocument(); }).With(Enabled{can_save});
/// @endcode
class Button final : public View {
public:
  explicit Button(StringVariant label);
};

/// Presents an image-only action with a required accessible label.
class IconButton final : public detail::TypedView<IconButton> {
public:
  IconButton(ImageVariant icon, StringVariant semantic_label);
};

/// Presents a compact action or controlled selectable value, optionally with a leading icon.
///
/// Passing selected state enables toggle semantics; write OnChanged proposals back to application-owned state.
class Chip final : public detail::TypedView<Chip> {
public:
  explicit Chip(StringVariant label);
  Chip(StringVariant label, bool selected);

  Chip(ImageVariant icon, StringVariant label);
  Chip(ImageVariant icon, StringVariant label, bool selected);

  /// Handles a requested controlled selection change.
  template <class Function> Chip OnChanged(Function&& function) && {
    return std::move(*this).On<ToggleEvents::Changed>(std::forward<Function>(function));
  }
};

/// Draws a themed separator along the requested axis.
class Divider final : public View {
public:
  explicit Divider(Axis axis = Axis::Horizontal);
};

/// Describes one labeled or icon-only choice in a SegmentedButton.
class SegmentedButtonItem final {
public:
  explicit SegmentedButtonItem(StringVariant label);
  SegmentedButtonItem(ImageVariant icon, StringVariant label);

  /// Creates an icon-only item while retaining a required accessible label.
  static SegmentedButtonItem IconOnly(ImageVariant icon, StringVariant semantic_label);

private:
  std::optional<ImageVariant> icon_;
  StringVariant label_;
  bool show_label_ = true;

  friend struct detail::SegmentedButtonItemAccess;
};

/// Presents a finite set of mutually exclusive controlled choices.
/// @code
/// View PeriodPicker(State<std::size_t> selected) {
///   return SegmentedButton({"Day", "Week", "Month"}, selected)
///       .OnChanged([selected](std::size_t index) mutable { selected = index; });
/// }
/// @endcode
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

  /// Handles a requested controlled selected-index change.
  template <class Function> SegmentedButton OnChanged(Function&& function) && {
    return std::move(*this).On<SegmentedButtonEvents::Changed>(std::forward<Function>(function));
  }
};

/// Describes one destination in Tabs.
class TabItem final {
public:
  explicit TabItem(StringVariant label);
  TabItem(ImageVariant icon, StringVariant label);

  /// Creates an icon-only destination while retaining a required accessible label.
  static TabItem IconOnly(ImageVariant icon, StringVariant semantic_label);

  /// Controls whether this destination can be selected.
  TabItem Enabled(bool enabled) &&;

private:
  std::optional<ImageVariant> icon_;
  StringVariant label_;
  bool show_label_ = true;
  bool enabled_ = true;

  friend struct detail::TabItemAccess;
};

/// Presents controlled navigation destinations without owning page content.
///
/// Pair the selected index with IndexedPages or application navigation state when destination content is required.
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

  /// Handles a requested controlled selected-index change.
  template <class Function> Tabs OnChanged(Function&& function) && {
    return std::move(*this).On<TabsEvents::Changed>(std::forward<Function>(function));
  }
};

/// Presents a controlled, non-editable finite-choice field and anchored popup.
///
/// The item factory creates both the selected presentation and popup choices. Apply a stable Key to each result when
/// items can insert, remove, or reorder.
/// @code
/// View CityPicker(State<std::size_t> selected, const std::vector<City>& cities) {
///   return Select(cities, selected, [](const City& city) { return Text(city.name).Key(city.id); })
///       .Label("City")
///       .OnChanged([selected](std::size_t index) mutable { selected = index; });
/// }
/// @endcode
class Select final : public detail::TypedView<Select> {
public:
  template <std::ranges::input_range Range, class Factory>
    requires std::constructible_from<std::ranges::range_value_t<Range>, std::ranges::range_reference_t<Range>> &&
             std::invocable<Factory&, std::ranges::range_reference_t<Range>> &&
             std::convertible_to<std::invoke_result_t<Factory&, std::ranges::range_reference_t<Range>>, View>
  Select(Range&& items, std::size_t selected_index, Factory&& content)
      : Select(
            detail::MakeViewItemSource(std::forward<Range>(items), std::forward<Factory>(content)), selected_index
        ) {}

  template <std::ranges::input_range Range, class Factory>
    requires std::constructible_from<std::ranges::range_value_t<Range>, std::ranges::range_reference_t<Range>> &&
             std::invocable<Factory&, std::ranges::range_reference_t<Range>> &&
             std::convertible_to<std::invoke_result_t<Factory&, std::ranges::range_reference_t<Range>>, View>
  Select(Range&& items, const State<std::size_t>& selected_index, Factory&& content)
      : Select(std::forward<Range>(items), selected_index.Get(), std::forward<Factory>(content)) {}

  template <class Range, class Factory>
    requires std::ranges::input_range<const Range&> &&
             std::invocable<Factory&, std::ranges::range_reference_t<const Range&>> &&
             std::convertible_to<std::invoke_result_t<Factory&, std::ranges::range_reference_t<const Range&>>, View>
  Select(const State<Range>& items, std::size_t selected_index, Factory&& content)
      : Select(items.Get(), selected_index, std::forward<Factory>(content)) {}

  template <class Range, class Factory>
    requires std::ranges::input_range<const Range&> &&
             std::invocable<Factory&, std::ranges::range_reference_t<const Range&>> &&
             std::convertible_to<std::invoke_result_t<Factory&, std::ranges::range_reference_t<const Range&>>, View>
  Select(const State<Range>& items, const State<std::size_t>& selected_index, Factory&& content)
      : Select(items.Get(), selected_index.Get(), std::forward<Factory>(content)) {}

  /// Handles a requested controlled selected-index change.
  template <class Function> Select OnChanged(Function&& function) && {
    return std::move(*this).On<SelectEvents::Changed>(std::forward<Function>(function));
  }

  /// Sets the field label and accessible name.
  Select Label(StringVariant value) &&;
  /// Presents application-owned validation state without changing selection rules.
  Select Validation(ValidationResult value) &&;

private:
  Select(detail::ViewItemSource source, std::size_t selected_index)
      : detail::TypedView<Select>(detail::MakeSelectSpec(source, selected_index)),
        source_(std::move(source)),
        selected_index_(selected_index) {
    UpdateModifier();
  }

  void UpdateModifier();

  detail::ViewItemSource source_;
  std::size_t selected_index_ = 0;
  StringVariant label_;
  ValidationResult validation_;
};

/// Displays an image resource, vector asset, raster asset, or ExternalTexture.
/// @code
/// return Image(images::avatar).Fit(ImageFit::Cover).Align(HorizontalAlignment::Center, VerticalAlignment::Center);
/// @endcode
class Image final : public View {
public:
  explicit Image(ImageVariant image);
  explicit Image(std::shared_ptr<ExternalTexture> texture);

  /// Selects how source content scales into the measured bounds.
  Image Fit(ImageFit fit) &&;
  /// Positions fitted content inside the bounds; Stretch is not a valid content alignment.
  Image Align(HorizontalAlignment horizontal, VerticalAlignment vertical) &&;
  /// Selects raster filtering; vector sources reject raster sampling configuration.
  Image Sampling(ImageSampling sampling) &&;
  /// Applies a color tint to vector images and vector-backed image resources.
  Image Tint(Color tint) &&;
};

/// Paint callback invoked with the current canvas size in local DIPs.
using CanvasPainter = std::function<void(PaintContext&, Size)>;

/// Records platform-neutral drawing commands through a PaintContext.
/// @code
/// return Canvas([](PaintContext& context, Size size) {
///   context.DrawRect(Rect{0.0F, 0.0F, size.width, size.height}, Color::Rgb(33, 111, 219));
/// });
/// @endcode
class Canvas final : public View {
public:
  explicit Canvas(CanvasPainter painter);
};

/// Selects the themed TextField surface treatment.
enum class TextFieldVariant {
  Filled,
  Outlined,
  Standard,
};

/// Defines single-line behavior or the intrinsic visible-line range of a multiline TextField.
///
/// Parent constraints remain authoritative. Content beyond a multiline maximum remains internally scrollable.
class TextFieldLineLimits final {
public:
  /// Uses one non-wrapping line and the default non-newline input action.
  static TextFieldLineLimits SingleLine() noexcept;
  /// Enables multiline input with a minimum number of visible lines and no intrinsic maximum.
  static TextFieldLineLimits MultiLine(std::size_t minimum = 1);
  /// Enables multiline input with an inclusive intrinsic visible-line range.
  static TextFieldLineLimits MultiLine(std::size_t minimum, std::size_t maximum);

  /// Reports whether newline input and multiline layout are enabled.
  [[nodiscard]] bool IsMultiline() const noexcept {
    return multiline_;
  }

  /// Returns the minimum number of intrinsic visible lines.
  [[nodiscard]] std::size_t Minimum() const noexcept {
    return minimum_;
  }

  /// Returns the intrinsic visible-line maximum, or no value when unconstrained.
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

/// Presents a controlled text editor backed by a complete TextEditingValue.
///
/// The application owns text, selection, affinity, and composition. OnChanged proposes the next complete value; write
/// it back to preserve the native input session and editing state.
/// @code
/// View SearchBox(State<TextEditingValue> query) {
///   return TextField(query)
///       .Label("Search")
///       .Placeholder("Type a query")
///       .OnChanged([query](const TextEditingValue& value) mutable { query = value; })
///       .OnSubmitted([] { RunSearch(); });
/// }
/// @endcode
class TextField final : public detail::TypedView<TextField> {
public:
  explicit TextField(TextEditingValue value);
  explicit TextField(const State<TextEditingValue>& value) : TextField(value.Get()) {}

  /// Sets the field label and accessible name.
  TextField Label(StringVariant value) &&;
  /// Sets text shown when the controlled value is empty.
  TextField Placeholder(StringVariant value) &&;
  /// Adds a decorative image before the editable text.
  TextField LeadingIcon(ImageVariant icon) &&;
  /// Adds a decorative image after the editable text.
  TextField TrailingIcon(ImageVariant icon) &&;
  /// Selects the themed filled, outlined, or standard surface.
  TextField Variant(TextFieldVariant value) &&;
  /// Configures single-line behavior or intrinsic multiline height limits.
  TextField LineLimits(TextFieldLineLimits value) &&;
  /// Sets horizontal paragraph alignment inside the editor viewport.
  TextField Align(TextAlign value) &&;
  /// Sets vertical paragraph alignment inside the editor viewport.
  TextField VerticalAlign(TextVerticalAlign value) &&;
  /// Limits accepted edits to a maximum number of grapheme clusters.
  TextField MaxLength(std::size_t value) &&;
  /// Presents application-owned validation state without filtering edits.
  TextField Validation(ValidationResult value) &&;
  /// Enables secure entry and prevents copying or exposing the controlled text through semantics.
  TextField Secure() &&;
  /// Replaces platform input configuration and synchronizes its multiline mode with LineLimits.
  TextField InputConfiguration(TextInputConfiguration configuration) &&;

  /// Handles a proposed complete editing value after text, selection, or composition changes.
  template <class Function> TextField OnChanged(Function&& function) && {
    return std::move(*this).On<TextFieldEvents::Changed>(std::forward<Function>(function));
  }

  /// Handles the configured platform submission action.
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
  TextAlign text_align_ = TextAlign::Leading;
  std::optional<TextVerticalAlign> text_vertical_align_;
  std::optional<std::size_t> max_length_;
  ValidationResult validation_;
};

/// Presents a controlled single-line text editor with an application-provided suggestion popup.
///
/// The application owns the complete editing value and suggestion range. Direct edits emit `OnChanged`; accepting a
/// suggestion emits `OnSelected` with its index and proposed complete replacement value.
/// @code
/// View ProjectPicker(State<TextEditingValue> query) {
///   return ComboBox(query, std::vector<std::string>{"Alpha", "Beta"})
///       .Label("Project")
///       .OnChanged([query](const TextEditingValue& value) mutable { query = value; })
///       .OnSelected([query](std::size_t, const TextEditingValue& value) mutable { query = value; });
/// }
/// @endcode
class ComboBox final : public detail::TypedView<ComboBox> {
public:
  template <std::ranges::input_range Range>
    requires std::constructible_from<std::ranges::range_value_t<Range>, std::ranges::range_reference_t<Range>> &&
             std::constructible_from<StringVariant, std::ranges::range_reference_t<Range>>
  ComboBox(TextEditingValue value, Range&& suggestions)
      : ComboBox(
            std::move(value),
            detail::MakeComboBoxSuggestionSource(
                std::forward<Range>(suggestions),
                [](const auto& suggestion) { return StringVariant(suggestion); },
                [](const auto& suggestion) { return Text(StringVariant(suggestion)); }
            )
        ) {}

  template <std::ranges::input_range Range>
    requires std::constructible_from<std::ranges::range_value_t<Range>, std::ranges::range_reference_t<Range>> &&
             std::constructible_from<StringVariant, std::ranges::range_reference_t<Range>>
  ComboBox(const State<TextEditingValue>& value, Range&& suggestions)
      : ComboBox(value.Get(), std::forward<Range>(suggestions)) {}

  ComboBox(TextEditingValue value, std::initializer_list<StringVariant> suggestions)
      : ComboBox(std::move(value), std::vector<StringVariant>(suggestions)) {}

  ComboBox(const State<TextEditingValue>& value, std::initializer_list<StringVariant> suggestions)
      : ComboBox(value.Get(), std::vector<StringVariant>(suggestions)) {}

  template <std::ranges::input_range Range, class TextFactory, class ContentFactory>
    requires std::constructible_from<std::ranges::range_value_t<Range>, std::ranges::range_reference_t<Range>> &&
             std::invocable<TextFactory&, std::ranges::range_reference_t<Range>> &&
             std::constructible_from<
                 StringVariant,
                 std::invoke_result_t<TextFactory&, std::ranges::range_reference_t<Range>>> &&
             std::invocable<ContentFactory&, std::ranges::range_reference_t<Range>> &&
             std::convertible_to<
                 std::invoke_result_t<ContentFactory&, std::ranges::range_reference_t<Range>>,
                 View>
  ComboBox(TextEditingValue value, Range&& suggestions, TextFactory&& text, ContentFactory&& content)
      : ComboBox(
            std::move(value),
            detail::MakeComboBoxSuggestionSource(
                std::forward<Range>(suggestions), std::forward<TextFactory>(text), std::forward<ContentFactory>(content)
            )
        ) {}

  template <std::ranges::input_range Range, class TextFactory, class ContentFactory>
    requires std::constructible_from<std::ranges::range_value_t<Range>, std::ranges::range_reference_t<Range>> &&
             std::invocable<TextFactory&, std::ranges::range_reference_t<Range>> &&
             std::constructible_from<
                 StringVariant,
                 std::invoke_result_t<TextFactory&, std::ranges::range_reference_t<Range>>> &&
             std::invocable<ContentFactory&, std::ranges::range_reference_t<Range>> &&
             std::convertible_to<
                 std::invoke_result_t<ContentFactory&, std::ranges::range_reference_t<Range>>,
                 View>
  ComboBox(const State<TextEditingValue>& value, Range&& suggestions, TextFactory&& text, ContentFactory&& content)
      : ComboBox(
            value.Get(), std::forward<Range>(suggestions), std::forward<TextFactory>(text),
            std::forward<ContentFactory>(content)
        ) {}

  /// Sets the field label and accessible name.
  ComboBox Label(StringVariant value) &&;
  /// Sets text shown when the controlled value is empty.
  ComboBox Placeholder(StringVariant value) &&;
  /// Adds an image before the editable text.
  ComboBox LeadingIcon(ImageVariant icon) &&;
  /// Selects a TextField visual variant without changing popup behavior.
  ComboBox Variant(TextFieldVariant value) &&;
  /// Sets horizontal text alignment inside the field.
  ComboBox Align(TextAlign value) &&;
  /// Limits direct edits to a maximum number of grapheme clusters.
  ComboBox MaxLength(std::size_t value) &&;
  /// Presents application-owned validation state without filtering suggestions.
  ComboBox Validation(ValidationResult value) &&;
  /// Configures the single-line, non-secure, editable platform input session.
  ComboBox InputConfiguration(TextInputConfiguration configuration) &&;
  /// Sets non-interactive popup content used only when the suggestion range is empty.
  ComboBox EmptyContent(std::function<View()> content) &&;

  /// Handles a proposed complete editing value after direct text input.
  template <class Function> ComboBox OnChanged(Function&& function) && {
    return std::move(*this).On<ComboBoxEvents::Changed>(std::forward<Function>(function));
  }

  /// Handles an accepted suggestion index and its proposed complete replacement value.
  template <class Function> ComboBox OnSelected(Function&& function) && {
    return std::move(*this).On<ComboBoxEvents::Selected>(std::forward<Function>(function));
  }

  /// Handles submission when no active suggestion is accepted.
  template <class Function> ComboBox OnSubmitted(Function&& function) && {
    return std::move(*this).On<ComboBoxEvents::Submitted>(std::forward<Function>(function));
  }

private:
  ComboBox(TextEditingValue value, detail::ComboBoxSuggestionSource source)
      : detail::TypedView<ComboBox>(detail::MakeComboBoxSpec(source, value)), value_(std::move(value)),
        source_(std::move(source)) {
    UpdateModifier();
  }

  void UpdateModifier();

  TextEditingValue value_;
  detail::ComboBoxSuggestionSource source_;
  StringVariant label_;
  StringVariant placeholder_;
  std::optional<ImageVariant> leading_icon_;
  std::optional<TextFieldVariant> variant_;
  TextAlign text_align_ = TextAlign::Leading;
  std::optional<std::size_t> max_length_;
  ValidationResult validation_;
  TextInputConfiguration configuration_;
  std::function<View()> empty_content_;
};

/// Presents an independently controlled checked value, optionally with a label.
/// @code
/// return Checkbox("Remember me", remembered)
///     .OnChanged([remembered](bool checked) mutable { remembered = checked; });
/// @endcode
class Checkbox final : public detail::TypedView<Checkbox> {
public:
  explicit Checkbox(bool checked);
  explicit Checkbox(const State<bool>& checked) : Checkbox(checked.Get()) {}
  Checkbox(StringVariant label, bool checked);
  Checkbox(StringVariant label, const State<bool>& checked) : Checkbox(std::move(label), checked.Get()) {}

  /// Handles a requested controlled checked-state change.
  template <class Function> Checkbox OnChanged(Function&& function) && {
    return std::move(*this).On<ToggleEvents::Changed>(std::forward<Function>(function));
  }
};

/// Presents one controlled choice intended for an application-owned mutually exclusive group.
///
/// RadioButton emits a boolean proposal but does not create or own a group selection model.
class RadioButton final : public detail::TypedView<RadioButton> {
public:
  explicit RadioButton(bool selected);
  explicit RadioButton(const State<bool>& selected) : RadioButton(selected.Get()) {}
  RadioButton(StringVariant label, bool selected);
  RadioButton(StringVariant label, const State<bool>& selected) : RadioButton(std::move(label), selected.Get()) {}

  /// Handles a requested controlled selected-state change.
  template <class Function> RadioButton OnChanged(Function&& function) && {
    return std::move(*this).On<ToggleEvents::Changed>(std::forward<Function>(function));
  }
};

/// Presents a controlled on/off setting, optionally with a label.
class Switch final : public detail::TypedView<Switch> {
public:
  explicit Switch(bool checked);
  explicit Switch(const State<bool>& checked) : Switch(checked.Get()) {}
  Switch(StringVariant label, bool checked);
  Switch(StringVariant label, const State<bool>& checked) : Switch(std::move(label), checked.Get()) {}

  /// Handles a requested controlled checked-state change.
  template <class Function> Switch OnChanged(Function&& function) && {
    return std::move(*this).On<ToggleEvents::Changed>(std::forward<Function>(function));
  }
};

/// Displays circular progress.
///
/// The parameterless form is indeterminate. A float or State value is determinate and is normalized by the component.
class ProgressCircle final : public detail::TypedView<ProgressCircle> {
public:
  ProgressCircle();
  explicit ProgressCircle(float progress);
  explicit ProgressCircle(const State<float>& progress) : ProgressCircle(progress.Get()) {}
};

/// Displays linear progress.
///
/// The parameterless form is indeterminate. A float or State value is determinate and is normalized by the component.
class ProgressBar final : public detail::TypedView<ProgressBar> {
public:
  ProgressBar();
  explicit ProgressBar(float progress);
  explicit ProgressBar(const State<float>& progress) : ProgressBar(progress.Get()) {}
};

/// Presents a controlled continuous value with optional range and step constraints.
/// @code
/// return Slider(volume)
///     .Range(0.0F, 100.0F)
///     .Step(5.0F)
///     .OnChanged([volume](float value) mutable { volume = value; });
/// @endcode
class Slider final : public detail::TypedView<Slider> {
public:
  explicit Slider(float value);
  explicit Slider(const State<float>& value) : Slider(value.Get()) {}

  /// Sets the finite increasing value range.
  Slider Range(float minimum, float maximum) &&;
  /// Quantizes interaction proposals to a positive finite step from the range minimum.
  Slider Step(float step) &&;

  /// Handles a requested controlled value change.
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

/// Establishes an independently recomposable child declaration from a copyable factory and captured arguments.
///
/// Prefer ordinary composable functions for reusable UI. Scope is useful when a declaration explicitly needs a child
/// composition boundary or must defer creation until that boundary composes.
/// @code
/// return Scope([](UserId id) { return UserCard(id); }, user_id);
/// @endcode
class Scope final : public View {
public:
  explicit Scope(std::function<View()> factory);

  template <class Factory, class... Arguments>
    requires detail::ViewFactoryFor<Factory, Arguments...>
  explicit Scope(Factory&& factory, Arguments&&... arguments)
      : Scope(detail::BindViewFactory(std::forward<Factory>(factory), std::forward<Arguments>(arguments)...)) {}
};

/// Enables retained selection, Copy, and Select All across descendant static Text nodes.
///
/// SelectionArea does not create an editable text input session.
/// @code
/// return SelectionArea(Column {Text("First paragraph"), Text("Second paragraph")});
/// @endcode
class SelectionArea final : public View {
public:
  explicit SelectionArea(View content);
};

/// Consumes remaining main-axis space inside compatible Row and Column layouts.
class Spacer final : public View {
public:
  Spacer();
};

/// Measures and places children vertically in declaration order.
class Column final : public Layout<Column> {
public:
  using Layout::Layout;

  /// Implements Column measurement for Runtime and custom layout composition.
  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints);
};

/// Measures and places children horizontally in declaration order.
class Row final : public Layout<Row> {
public:
  using Layout::Layout;

  /// Implements Row measurement for Runtime and custom layout composition.
  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints);
};

/// Places children in wrapping lines according to the configured Flow modifiers.
class Flow final : public Layout<Flow> {
public:
  using Layout::Layout;

  /// Implements Flow measurement for Runtime and custom layout composition.
  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints);
};

/// Overlays children in declaration order within shared bounds.
class Stack final : public Layout<Stack> {
public:
  using Layout::Layout;

  /// Implements Stack measurement for Runtime and custom layout composition.
  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints);
};

/// Retains all declared pages while measuring and presenting only the controlled selected page.
///
/// Use stable page order and keep the selected index within the non-empty page range.
class IndexedPages final : public Layout<IndexedPages> {
public:
  IndexedPages(std::initializer_list<View> pages, std::size_t selected_index)
      : IndexedPages(std::vector<View>(pages), selected_index) {}
  IndexedPages(std::initializer_list<View> pages, const State<std::size_t>& selected_index)
      : IndexedPages(std::vector<View>(pages), selected_index.Get()) {}
  IndexedPages(std::vector<View> pages, std::size_t selected_index);
  IndexedPages(std::vector<View> pages, const State<std::size_t>& selected_index)
      : IndexedPages(std::move(pages), selected_index.Get()) {}

  /// Implements selected-page measurement for Runtime.
  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints);
};

/// Makes one ordinary content subtree scrollable along a configured axis.
///
/// ScrollView owns clipping, direct drag, wheel and trackpad consumption, nested scrolling, fling, overscroll, focus
/// reveal, and semantic scrolling through the shared scroll state.
/// @code
/// return ScrollView(Column {Header(), Content()}.With(Spacing(12.0F)))
///     .ScrollAxis(Axis::Vertical)
///     .Controller(scroll_controller);
/// @endcode
class ScrollView final : public detail::TypedView<ScrollView> {
public:
  explicit ScrollView(View content);

  /// Selects the scrolling axis.
  ScrollView ScrollAxis(Axis axis) &&;
  /// Connects a stable controller for programmatic scrolling and metric observation.
  ScrollView Controller(huxerui::ScrollController controller) &&;
};

/// Virtualizes a one-dimensional logical item source with fixed, estimated, or measured item extents.
/// @code
/// return VirtualList(records, [](const Record& record) {
///   return RecordRow(record).Key(record.id);
/// });
/// @endcode
class VirtualList final : public VirtualLayout<VirtualList> {
public:
  using VirtualLayout::VirtualLayout;

  /// Selects the scrolling and placement axis.
  VirtualList ScrollAxis(Axis axis) &&;
  /// Uses one positive fixed extent for every logical item.
  VirtualList ItemExtent(float extent) &&;
  /// Supplies a positive initial extent estimate for variable-size items.
  VirtualList EstimatedItemExtent(float extent) &&;
  /// Sets the non-negative offscreen extent retained around the viewport.
  VirtualList CacheExtent(float extent) &&;

  /// Implements virtual range measurement and placement for Runtime.
  static VirtualLayoutResult Measure(VirtualLayoutContext& context, MountedNode& node, Constraints constraints);
  /// Resolves the content offset required to align a logical item in the viewport.
  static std::optional<float>
  ScrollOffsetForItem(MountedNode& node, std::size_t index, ScrollAlignment alignment, float viewport_extent);
};

/// Virtualizes a vertically scrolling grid with adaptive or fixed columns and optional item spans.
/// @code
/// return VirtualGrid(items, [](const Item& item) { return Tile(item).Key(item.id); })
///     .Columns(GridColumns::Adaptive(160.0F))
///     .EstimatedRowExtent(120.0F)
///     .ColumnSpacing(12.0F)
///     .RowSpacing(12.0F);
/// @endcode
class VirtualGrid final : public VirtualLayout<VirtualGrid> {
public:
  using VirtualLayout::VirtualLayout;

  /// Selects fixed-count or adaptive column geometry.
  VirtualGrid Columns(GridColumns columns) &&;
  /// Uses one positive fixed extent for every grid row.
  VirtualGrid RowExtent(float extent) &&;
  /// Supplies a positive initial estimate for variable row extents.
  VirtualGrid EstimatedRowExtent(float extent) &&;
  /// Sets non-negative spacing between rows.
  VirtualGrid RowSpacing(float spacing) &&;
  /// Sets non-negative spacing between columns.
  VirtualGrid ColumnSpacing(float spacing) &&;
  /// Sets the non-negative offscreen extent retained around the viewport.
  VirtualGrid CacheExtent(float extent) &&;
  /// Assigns each item a positive span that is clamped to the resolved column count.
  VirtualGrid ItemSpans(std::vector<std::size_t> spans) &&;

  /// Implements virtual grid range measurement and placement for Runtime.
  static VirtualLayoutResult Measure(VirtualLayoutContext& context, MountedNode& node, Constraints constraints);
  /// Resolves the content offset required to align a logical item row in the viewport.
  static std::optional<float>
  ScrollOffsetForItem(MountedNode& node, std::size_t index, ScrollAlignment alignment, float viewport_extent);
};

} // namespace huxerui

// clang-format off
/// Returns a Scope from the current function using one expression-style factory body.
#define HUXERUI_SCOPE(...) return ::huxerui::Scope([=]() -> ::huxerui::View __VA_ARGS__)

/// Begins a multiline Scope return when an expression-style HUXERUI_SCOPE body is not practical.
#define HUXERUI_SCOPE_BEGIN \
  return ::huxerui::Scope([=]() -> ::huxerui::View {

/// Ends a multiline Scope started by HUXERUI_SCOPE_BEGIN.
#define HUXERUI_SCOPE_END \
  });
// clang-format on
