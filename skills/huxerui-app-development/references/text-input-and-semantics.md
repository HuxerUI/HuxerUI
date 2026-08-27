# Text Input and Semantics

## Controlled editing

Store the complete `TextEditingValue`, not only `text`. It carries `TextSelection` and optional composition. Text offsets and platform input units are not interchangeable; let HuxerUI's text-input contract translate UTF-8, UTF-16, scalar, and grapheme boundaries.

```cpp
auto value = UseState(TextEditingValue::FromText(""));

return TextField(value)
    .Label("Email")
    .InputConfiguration({.type = TextInputType::Email, .action = TextInputAction::Next})
    .OnChanged([value](TextEditingValue next) {
      value = std::move(next);
    });
```

`TextField` is controlled: the callback requests the next value and the owner supplies it on recomposition. Use `TextFieldLineLimits` for single/multiline behavior, `MaxLength` for the component contract, and `ValidationResult` for application-owned validation state. `Required` and `EmailAddress` are reusable rules, and `Validate(value, rules...)` stops at the first invalid result. Validation reports domain state; it is not an edit filter.

`Secure()` configures secure entry. Avoid logging, retaining, or echoing secure editing values outside the necessary owner.

## Input configuration

Choose `TextInputType`, capitalization, action, multiline, secure, autocorrect, and read-only behavior through `TextInputConfiguration`. Handle `.OnSubmitted` for semantic completion, not every raw key.

## Semantics

Use the public `Semantics` modifier and typed semantic actions. Supply accessible labels for icon-only controls and meaningful roles/state for custom controls. Built-in controls already provide their normal semantics; add metadata only when application meaning is missing.

For collections, expose stable collection/item metadata when building a custom virtual layout. Disabled, checked, selected, expanded, value, range, and text-editing semantics should match the controlled UI value.

Custom `NodeExtension` semantics call `InvalidateSemantics()` after retained semantic state changes and handle only the local actions they declare.

## Review checklist

- full editing value preserved;
- focus and IME action make sense for the field order;
- validation message is perceivable and linked to the field meaning;
- icon-only actions have labels;
- keyboard and semantic activation match pointer activation;
- selection, clipboard, secure data, and read-only behavior are not conflated.
