# Platform Views

`PlatformView` embeds a platform-owned leaf control in HuxerUI layout. It is a View, not a modifier, layout, canvas command, or module.

## Declarative contract

Construct `PlatformView(type, properties)` with a stable type string and a complete `PlatformPayload` property snapshot. Register typed events with `.Events<Key...>()` and handlers with `.On<Key>(...)`.

Wrap raw construction in an app-side component such as `PlatformTextField(value, on_changed)`. The wrapper owns type names, payload keys, event decoding, constraints, semantics, and controlled update rules. Callers should not manipulate raw payloads.

The platform factory owns create, update, and dispose:

- Android: Java object factory receiving `JNIEnv*` and host object.
- iOS: `UIView*` factory.
- macOS: `NSView*` factory.
- Web: `emscripten::val` element factory.
- Windows: child `HWND` factory receiving its parent window.

Use only the installed platform-specific public factory header. Register through the platform integration point generated or documented by the active SDK; do not reach into an adapter or runtime private API.

## Geometry and behavior

Provide a bounded size or parent `Frame`; a platform control has no portable intrinsic size. The public contract supports HuxerUI layout placement, rectangular visibility/clipping, focus/input integration, update, event dispatch, and lifecycle according to each backend.

Do not promise rotation, arbitrary path clipping, group opacity, backdrop filters, or transparent composition unless the active platform header/user contract explicitly does. A platform control cannot be placed inside Canvas drawing commands.

## Controlled properties

Properties are a full authoritative snapshot. On application change, emit a typed event, update owner state, and supply the new snapshot on composition. Keep a stable key/type when the same platform instance should update; change identity only when replacement is intended.

## PlatformView versus ExternalTexture

Choose `PlatformView` for platform-owned input, IME, accessibility, focus, and control lifecycle. Choose `ExternalTexture` when the platform only produces frames and HuxerUI should own layout, effects, interaction, and surrounding semantics.

## Review points

- concrete wrapper instead of raw type strings in page code;
- validated payloads and typed events;
- full controlled property snapshot;
- bounded geometry;
- factory create/update/dispose symmetry;
- focus, IME, accessibility, and unmount behavior;
- documented platform limitations rather than cross-platform assumptions.
