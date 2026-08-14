# App Resources, Images, and Localization Design

Status: initial implementation with ordered target resource merging

This document defines application resource identity, packaging, resolution, immutable raster and vector image assets, raw assets, the Image component, image painting, locale propagation, and formatted localized strings.

The current implementation includes typed keys, exact generated namespaces, optional generated-header filenames, ordered package merging, target staging, the installed built-in framework resource package, localized Dialog, selection-menu, validation, and window-caption defaults, component-owned check and submenu-chevron vectors, PlatformResources on Android, iOS, macOS, Windows, and Web, Runtime-owned resolution, raw assets, positional localized strings, deferred StringVariant inputs, ImageAsset, VectorAsset, SVG compilation, Image, image painting, native raster caches, and generated-assets wiring for the repository Android demo.
Reusable installed-Android integration, inherited Locale shaping for ordinary application text, localized image discovery, and future platform adapters remain planned.

## Goals

- Give application, framework, and module resources stable typed identities that do not expose platform paths or native resource identifiers.
- Package one resource model into Android assets, macOS application bundles, and Windows distributions.
- Support Image as a built-in View and image replay from Canvas without introducing another rendering surface.
- Keep encoded image bytes available to application and module code while retaining native decoding and bitmap caches in platform renderers.
- Resolve localized strings through the existing Environment and root-service model.
- Support translatable positional formatting whose arguments may be reordered or repeated by each locale.
- Preserve retained PaintSequence reuse, local invalidation, and platform-neutral RenderScene data.
- Leave room for module resources, custom fonts, plural messages, and additional image formats without exposing a generic resource hierarchy.

## Non-goals

The initial implementation does not provide network loading, URI loading, animated images, runtime SVG DOM decoding, image filters, editable pixel buffers, date or currency formatting, plural rules, resource hot reload, or a runtime module registry.

Network and native picker modules may produce encoded bytes and construct an ImageAsset.
Android `content://` values, Apple security-scoped URLs, and other native handles remain platform-service concerns rather than cross-platform file paths.

## Ownership

Resource ownership follows the existing Runtime, Environment, PlatformAdapter, and PaintCommand boundaries:

| Layer | Responsibility |
|---|---|
| Resource tool and CMake | Validate source resources, generate typed keys and a versioned index, and stage target resources |
| PlatformResources | Read immutable bytes from the installed platform package and report the current resource configuration |
| AppResources | Resolve typed keys, locale fallback, density variants, and shared immutable assets |
| Runtime | Own AppResources for each runtime root, seed the resource Environment, and coordinate resource-configuration invalidation |
| Image | Measure from intrinsic logical size and translate fit and alignment into raster or vector painting |
| PaintContext | Record raster image commands or expand immutable vector paths with source geometry, tint, and opacity |
| Platform renderer | Decode and cache raster images, then replay the common platform-neutral path and image commands |

Runtime and application state never retain `Bitmap`, `CGImage`, `ID2D1Bitmap`, platform paths, or platform resource identifiers.
Platform renderers never resolve localized strings or decide ImageFit behavior.

## Typed resource identity

The public surface uses distinct resource-key types:

```cpp
class ResourceId;
class ImageResource;
class StringResource;
class RawResource;
```

The typed keys derive from ResourceId, so shared identity operations remain available without another storage wrapper.
They are not implicitly convertible to one another.
A string key cannot therefore be passed to Image, and an image key cannot be read as arbitrary raw data by accident.

A ResourceId contains a domain and a key:

```text
app:images/logo
app:strings/save
huxerui:strings/cut
sweet_editor:images/diagnostic_icon
```

The domains have stable ownership:

- `app` belongs to the current application.
- `huxerui` belongs to framework resources.
- A module owns each domain explicitly declared by its resource `NAMESPACE`.
- Multiple target resource roots may contribute to the same domain.
- For one resource variant in one domain, a later target resource root overrides an earlier root.

Resource keys compare by value and include enough readable identity for diagnostics.
The package index uses mandatory content hashes to verify payloads, while native caches use a private stable identity carried by the resolved immutable ImageAsset rather than a ResourceId alone.

The CMake `NAMESPACE` value is both the resource domain and the exact generated C++ namespace.
The default generated header name adds the `_resources` suffix, while single-root `hapt` generation may select another `.h` filename without changing the domain or C++ namespace.
For `NAMESPACE app`, the tool generates `app_resources.h` into the build directory:

```cpp
namespace app::images {
inline const huxerui::ImageResource logo{"app", "images/logo"};
}

namespace app::strings {
inline const huxerui::StringResource save{"app", "strings/save"};
}
```

Generated resource headers are build products and are never edited or committed.
There is no generated `app_resources` C++ namespace or compatibility alias.

## Runtime resource ownership

Each Runtime root automatically owns AppResources.
Applications do not install it manually through AppOptions or a RootHook.

The initial AppResources implementation owns the application index generated for its target.
Ordered resource merging extends this owner to one final target index without changing the public lookup API.
Runtime publishes the service into the root Environment so captured Layer environments preserve the same resources and Locale as their source content.

The public composition operations remain narrow:

```cpp
ImageAsset UseImage(ImageResource resource);
VectorAsset UseVectorImage(ImageResource resource);
RawAsset UseRawResource(RawResource resource);

template <class... Arguments>
std::string UseString(StringResource resource, Arguments&&... arguments);

std::string UseString(const StringVariant& value);
std::string UseString(StringVariant&& value);
```

These operations read the current root service and Environment but do not allocate ordered UseState slots.
There is no public mutable ResourceManager, global resource singleton, or second context system.
Deferred display-string APIs use `StringVariant`, which stores direct text or a `StringResource` and its positional arguments until composition occurs in the captured Environment.
The `UseString` overloads form its single resolution boundary; components do not use a parallel private resolver.
Text also accepts a zero-argument StringResource directly and mirrors its existing Format surface for localized arguments:

```cpp
Text(app::strings::title);
Text(app::strings::title, TextRole::Title);
Text::Format(app::strings::file_count, user_name, file_count);
Text::Format(TextRole::Label, app::strings::file_count, user_name, file_count);
```

The constructors keep the common static-string case compact.
The named Format operation avoids a variadic constructor that could conflict with TextRole or future Text configuration.
Button and TextField placeholder provide direct StringResource overloads.
UseString remains the explicit operation for application logic that needs resolved UTF-8 immediately, while StringVariant lets component, validation, semantics, and presentation APIs retain direct text or a StringResource until their composition boundary.

## Build and package model

The recommended source root is named `resources` and contains resource kinds directly rather than another domain directory:

```text
resources/
  images/
  strings/
    default.properties
  raw/
```

Platform-native assets such as an Apple `Assets.xcassets` catalog remain in their native shell and are not HuxerUI resource roots.

Application resources are target-scoped CMake inputs.
The current implementation permits repeated calls for the same target and preserves their declaration order:

```cmake
huxerui_add_resources(
    application_target
    ROOT "${CMAKE_CURRENT_SOURCE_DIR}/modules/editor/resources"
    NAMESPACE "editor"
)

huxerui_add_resources(
    application_target
    ROOT "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    NAMESPACE "app"
)
```

`huxerui_add_app` keeps a compact primary application root:

```cmake
huxerui_add_app(application_target
    SOURCES src/app.cpp
    RESOURCES resources
    RESOURCE_NAMESPACE app
)
```

`huxerui_add_module` provides the same compact form for a module's primary resource root:

```cmake
huxerui_add_module(camera_kit
    SOURCES src/camera_kit.cpp
    RESOURCES resources
    RESOURCE_NAMESPACE camera_kit
)
```

The application helper registers the precompiled built-in `huxerui` package before the primary application root, while the module helper attaches its primary package to the module target.
Advanced consumers that need more than one root omit the compact resource arguments and call `huxerui_add_resources` in the required order.
Later roots override earlier roots only when they declare the same resource variant; all other variants coexist.
This permits application roots to override selected `huxerui` or module resources without a separate override declaration.
Each registered root must contain at least one supported resource and must not equal, contain, or be contained by another root registered for the same target.
All roots configure one final resource package for the target.
Native platform shells consume its generated staging projection and do not redeclare asset roots.

Resource processing is distinct from C++ scope transformation.
The HuxerUI Asset Packaging Tool (`hapt`) belongs in the existing `tools/prebuilt/<host>/<architecture>` layout rather than expanding the HuxerUI Code Generator (`hcg`) into an unrelated packager.

The resource tool:

- Discovers declared image, string, and raw resources.
- Normalizes package-relative paths and rejects absolute paths, `..` traversal, control characters, quotes, and
  platform separators that cannot be represented consistently in generated C++ and native packages.
- Validates domains, keys, locale tags, scales, referenced files, and final merged variants.
- Reads image format and dimensions without performing a full pixel decode.
- Verifies that scale variants have consistent intrinsic logical dimensions.
- Parses and validates localized format templates.
- Generates typed C++ resource keys.
- Generates a versioned binary resource index with content hashes and image metadata.
- Produces a target-specific staging directory.
- Emits dependency metadata so additions, removals, and content changes rebuild and restage the package.
- Removes stale payloads from generated and staged output before publishing the current package.

The framework source root is processed once as an ordinary resource package while HuxerUI itself is built, and that package is installed with the SDK.
Each module or application root is likewise processed as an ordinary package when its application target is built.
The `hapt merge` operation reads the precompiled framework package first and the target's packages in declaration order, then writes one final `resources.bin` plus only the payloads referenced by that final index.
It does not introduce bundle metadata, base and overlay roles, or a runtime package stack; the existing binary index already contains the domain, key, kind, locale, scale, package path, content hash, and image metadata required for merging.
CMake removes the per-root compilation workspace after a successful merge so the final package and generated headers are the only retained target resource output.
CMake attaches generation and staging outputs directly to the application target; framework compilation, individual roots, merging, and platform staging do not create auxiliary resource targets.

Variant identity is defined by resource kind:

- Raw resources use domain and key.
- String resources use domain, key, and locale.
- Image resources use domain, key, locale, and scale.

A later entry with the same variant identity replaces the earlier entry.
Entries with different locales or scales remain independently addressable.
After replacement, `hapt` validates the complete merged resource family, including kind consistency, localized argument schemas, raster and vector consistency, intrinsic logical dimensions, package paths, and content hashes.

Generated headers are emitted from the final merged index and grouped by namespace so repeated roots with the same `NAMESPACE` produce one header instead of competing intermediate headers.
Intermediate root headers are not placed on the application target's include path.

Generated indexes and `PlatformResources` package paths use normalized UTF-8 with `/` separators.
Native adapters convert that representation only at the platform filesystem boundary.

The package keeps the index separate from resource payload files.
Embedding or archive packing can be added later without changing ResourceId, ImageAsset, or StringResource.

Target packaging maps the staging directory as follows:

- Android CMake builds generate one package per ABI, then a Gradle generated-assets task selects one deterministic
  package after native builds complete and synchronizes it into APK assets.
- macOS copies it into `.app/Contents/Resources/HuxerUI`.
- Windows copies it beside the executable under `<executable-name>.resources` so multiple applications can share one output directory without colliding.
- iOS copies it into the application bundle's reserved HuxerUI resources directory.
- OHOS includes it in the HAP rawfile payload.
- Linux copies it beside the executable under `<executable-name>.resources`.
- Web preloads it into WASM-owned memory or a virtual filesystem before Runtime is created.

Shared application code never uses Android `R` identifiers, `NSBundle` paths, or Win32 resource identifiers.
Future backends likewise keep native bundle handles, rawfile handles, installation paths, and browser URLs behind PlatformResources.

## Platform resource boundary

PlatformAdapter exposes one optional native resource capability:

```cpp
struct ResourceConfiguration {
  Locale locale;
  float display_scale = 1.0F;

  bool operator==(const ResourceConfiguration&) const = default;
};

class RawAsset;

class PlatformResources {
public:
  virtual ~PlatformResources() = default;

  [[nodiscard]] virtual ResourceConfiguration Configuration() const = 0;
  [[nodiscard]] virtual RawAsset Read(std::string_view package_path) = 0;
};
```

PlatformAdapter returns the capability when packaged resources are available.
Using a packaged resource without an installed capability is a framework configuration error.

Returning RawAsset lets a backend retain owned memory, a mapped file, a package buffer, or a WASM memory range without forcing an intermediate vector copy.

The resource index is already filtered for the build target, so shared Runtime code does not branch on a platform identifier.
ResourceConfiguration supplies only values that vary at runtime and affect resolution.

Packaged resources must be synchronously readable before Runtime is created.
A platform whose package transport is asynchronous completes that transport during platform startup and exposes the resulting immutable payload through PlatformResources.
In particular, the Web entry integration loads the resource index and payload before creating a Runtime from the registered `Application`.
Remote URLs remain application or module inputs and do not become package paths.

When system locale or display scale changes, the platform integration calls `Runtime::UpdateResourceConfiguration()` with the new value.
Runtime ignores an equal value; otherwise it updates AppResources and the inherited Locale, invalidates root composition, and requests a frame.
`BuildFrame()` does not poll native state.
The initial implementation invalidates root composition, then normal reconciliation limits changed ImageAsset geometry and paint work to the affected nodes.
Dependency-recorded resource reads and inherited Locale text shaping may later narrow the initial root invalidation without changing the public API.

An explicit Locale Environment value overrides the system locale for its subtree.
Display scale remains a platform property.

## Resource variants

The initial implementation supports localized string variants and image scale variants.

Locale fallback removes trailing variants first.
When a tag includes both script and region, the language-region catalog takes precedence over the language-script catalog before lookup falls back to the language and bundle default:

```text
zh-Hant-TW
zh-TW
zh-Hant
zh
default locale
```

Image density selection prefers the smallest declared scale that is not lower than the display scale.
If no such variant exists, it selects the largest available scale.

For example, a display scale of 1.5 selects a 2x image when 1x, 2x, and 3x variants exist.

The image example packages a real matching set:

```text
logo.png       418 x 418
logo@2x.png    836 x 836
logo@3x.png   1254 x 1254
```

It displays the selected ImageAsset scale and encoded pixel dimensions so density selection is observable on each platform.

Scale variants for one ImageResource must have the same intrinsic logical size:

```text
pixel size / declared scale
```

Localized image discovery remains planned.
The index and resolver reserve locale metadata so that capability can be added without changing ImageResource.

The versioned index may later add appearance or contrast qualifiers.
The initial public API does not expose speculative theme-resource abstractions.

## ImageAsset

ImageResource is a logical key.
ImageAsset is a resolved immutable encoded image value.

The public value provides encoded data and metadata:

```cpp
enum class ImageFormat {
  Png,
  Jpeg,
};

class ImageAsset {
public:
  ImageAsset() = default;

  static ImageAsset FromFile(const std::filesystem::path& path, float scale = 1.0F);
  static ImageAsset FromEncoded(std::vector<std::byte> bytes, float scale = 1.0F);
  static ImageAsset CopyEncoded(std::span<const std::byte> bytes, float scale = 1.0F);

  [[nodiscard]] std::span<const std::byte> EncodedBytes() const noexcept;
  [[nodiscard]] ImageFormat Format() const noexcept;
  [[nodiscard]] std::string_view MimeType() const noexcept;
  [[nodiscard]] std::uint32_t PixelWidth() const noexcept;
  [[nodiscard]] std::uint32_t PixelHeight() const noexcept;
  [[nodiscard]] float Scale() const noexcept;
  [[nodiscard]] Size IntrinsicSize() const noexcept;
  [[nodiscard]] bool HasValue() const noexcept;

  bool operator==(const ImageAsset& other) const noexcept;
};
```

`EncodedBytes()` returns the original PNG or JPEG representation, not decoded RGBA pixels.
Its span remains valid while the ImageAsset or one of its copies remains alive.

ImageAsset uses shared immutable storage.
Copying it does not copy encoded bytes, and recording it in a PaintCommand retains a stable resource snapshot.
`HasValue()` distinguishes a default-constructed missing asset from a resolved image.

`FromEncoded()` moves caller-owned bytes into shared storage.
`CopyEncoded()` explicitly copies a borrowed span.
Both detect the image format and read dimensions from the encoded header without performing a native pixel decode.

`FromFile()` is a synchronous convenience for real filesystem paths.
It reads the file once and delegates to the encoded-data path.
It is not a cross-platform URI loader and must not interpret Android content URIs or Apple security-scoped URLs.
On Web it addresses only files already mounted in the WASM virtual filesystem and never treats an HTTP URL as a file path.
Callers retain the resulting ImageAsset rather than recreating it during every composition.

AppResources verifies the selected payload against the index and caches its shared ImageAsset storage by immutable package path and content hash.
The encoded bytes are therefore already present when a renderer first decodes the image.

The initial guaranteed formats are PNG and JPEG.
Additional formats extend ImageFormat and every renderer together.

## VectorAsset and SVG resources

VectorAsset is an immutable platform-neutral vector image value.
It retains a view box, intrinsic logical size, and a restricted sequence of filled paths, stroked paths, clips, and transforms.
It does not retain native paths, callbacks, Environment values, Theme values, or mutable component state.

Applications may construct vector images directly:

```cpp
const VectorAsset mark = VectorAsset::Create({24.0F, 24.0F}, [](VectorBuilder& vector) {
  Path path;
  path.MoveTo({2.0F, 12.0F}).LineTo({10.0F, 20.0F}).LineTo({22.0F, 4.0F});
  vector.StrokePath(std::move(path), Color::Black(), 2.0F, StrokeCap::Round, StrokeJoin::Round);
});
```

Packaged SVG files remain ImageResource values and live under `resources/images` with raster images.
The resource generator validates them and compiles supported geometry into the versioned `HUXV` payload.
Runtime detects that payload signature when it first resolves the ImageResource; ResourceId does not encode the storage format and there is no public ImageKind.

The initial SVG compiler supports `svg`, `g`, `path`, `rect`, `circle`, `ellipse`, `line`, `polyline`, and `polygon`; view boxes and intrinsic sizes; solid fill and stroke colors; fill rules; stroke widths, caps, joins, and miter limits; element transforms; and path arcs converted to cubic curves.
It rejects scripts, external entities, text rendering, embedded bitmaps, CSS stylesheets, gradients, filters, masks, clip paths, animation, and unsupported units with a source-file diagnostic.
Unimplemented presentation semantics such as `preserveAspectRatio`, `display`, `visibility`, and group `opacity` are rejected rather than approximated.

`Image(ImageResource)` automatically accepts either a raster or vector payload.
`UseImage()` and `UseVectorImage()` are explicit type-checked resolvers for code that needs the concrete asset value.
Calling the wrong resolver throws `std::invalid_argument`.

## RawResource and RawAsset

Arbitrary bytes use a distinct key and value rather than abusing ImageAsset:

```cpp
class RawAsset {
public:
  RawAsset() = default;

  static RawAsset FromBytes(std::vector<std::byte> bytes, std::string mime_type = {});
  static RawAsset CopyBytes(std::span<const std::byte> bytes, std::string mime_type = {});
  static RawAsset FromSharedBytes(
      std::shared_ptr<const void> owner,
      const std::byte* data,
      std::size_t size,
      std::string mime_type = {}
  );

  [[nodiscard]] std::span<const std::byte> Bytes() const noexcept;
  [[nodiscard]] std::string_view AsStringView() const noexcept;
  [[nodiscard]] std::string ToString() const;
  [[nodiscard]] std::string_view MimeType() const noexcept;
  [[nodiscard]] bool HasValue() const noexcept;

  bool operator==(const RawAsset& other) const noexcept;
};
```

`UseRawResource(RawResource)` returns a shared immutable RawAsset.
ImageAsset and RawAsset may share private byte-storage machinery but do not inherit from a public Asset base class.

RawAsset storage retains a shared owner rather than requiring a vector allocation.
The owner may represent ordinary allocated bytes, a memory-mapped Linux file, an Apple data buffer, an OHOS rawfile-backed region, or a WASM preloaded-memory slice.
AppResources can construct ImageAsset storage from the same owner without copying encoded bytes.

`HasValue()` reports whether storage is present, not whether the byte range has content.
A zero-length RawAsset created with `FromBytes({})` is valid; callers use `Bytes().empty()` when byte length is what matters.

AsStringView exposes the complete byte range as characters without copying.
The view remains valid while the RawAsset or another value sharing its storage remains alive.
ToString returns an independently owned copy.
Neither operation checks the MIME type, validates UTF-8, strips a byte-order mark, or stops at an embedded null byte.
RawAsset deliberately provides no implicit string conversion because binary resources must not become text accidentally.

Custom fonts may later use FontResource and the same bundle storage without turning every resource into a public template specialization.

## Image component

Image is a built-in leaf View:

```cpp
enum class ImageFit {
  None,
  Contain,
  Cover,
  Fill,
  ScaleDown,
};

enum class ImageSampling {
  Nearest,
  Linear,
};

class Image final : public View {
public:
  explicit Image(ImageResource resource);
  explicit Image(ImageAsset asset);
  explicit Image(VectorAsset asset);

  Image Fit(ImageFit fit) &&;
  Image Align(HorizontalAlignment horizontal, VerticalAlignment vertical) &&;
  Image Sampling(ImageSampling sampling) &&;
  Image Tint(Color tint) &&;
};
```

The ImageResource constructor lets Runtime resolve raster scale variants or a compiled vector payload from the node's Environment and PlatformResources configuration.
The ImageAsset constructor supports files, network results, native picker modules, generated images, and explicitly shared application data.
Packaged SVG resources resolve to VectorAsset values through ImageResource, while VectorAsset::Create constructs programmatic vector geometry.

Sampling configures raster filtering and is invalid for a VectorAsset.
Tint replaces the RGB channels of vector fills and strokes while preserving their per-layer alpha, then multiplies the supplied tint alpha.
Tint is invalid for an ImageAsset; a future raster color-filter API remains a distinct operation.

Image does not add component-specific opacity.
The existing Opacity presentation modifier applies to the node as a whole.

`HorizontalAlignment::Stretch` and `VerticalAlignment::Stretch` are invalid Image content alignments because ImageFit owns scaling.
Image validates them when configured.

### Measurement

ImageFit does not determine the node's measured size.

- Without tight dimensions, Image uses the resolved asset's IntrinsicSize() as its desired size.
- If the intrinsic size exceeds finite maximum constraints, Image scales the desired size down uniformly to fit.
- Minimum constraints may enlarge the Image layout box without changing the image's intrinsic aspect ratio.
- Frame, Grow, and parent layout policy may provide a larger or tighter final box.
- Padding reduces the content rectangle in which the image is painted.

This keeps layout deterministic while letting the paint policy decide how an image occupies an explicitly sized box.

### Fit and alignment

- `None` uses intrinsic logical size without scaling.
- `Contain` scales uniformly so the complete image fits inside the content rectangle.
- `Cover` scales uniformly and crops the source so the destination is filled.
- `Fill` scales each axis independently and may change aspect ratio.
- `ScaleDown` chooses the smaller result of None and Contain.

Alignment positions unused destination space or chooses the cropped source region.
Image computes source and destination geometry before recording a command, so native renderers do not implement ImageFit independently.

## Image painting

PaintCommand adds one immutable image command:

```cpp
struct DrawImageCommand {
  ImageAsset image;
  Rect source;
  Rect destination;
  ImageSampling sampling = ImageSampling::Linear;
  float opacity = 1.0F;

  bool operator==(const DrawImageCommand&) const = default;
};
```

Source geometry uses intrinsic logical image coordinates rather than encoded pixels.
The renderer converts source coordinates using ImageAsset::Scale().

PaintContext exposes separate whole-image and cropped-image methods:

```cpp
void DrawImage(
    ImageAsset image,
    Rect destination,
    ImageSampling sampling = ImageSampling::Linear,
    float opacity = 1.0F
);

void DrawImageRect(
    ImageAsset image,
    Rect source,
    Rect destination,
    ImageSampling sampling = ImageSampling::Linear,
    float opacity = 1.0F
);

void DrawImage(
    VectorAsset image,
    Rect destination,
    std::optional<Color> tint = {},
    float opacity = 1.0F
);

void DrawImageRect(
    VectorAsset image,
    Rect source,
    Rect destination,
    std::optional<Color> tint = {},
    float opacity = 1.0F
);
```

The explicit method names avoid a single overload whose optional source geometry is difficult to read at call sites.

DrawImageCommand destination geometry supplies culling, damage, and conservative paint bounds.
The renderer does not measure the image or recalculate fit.
Existing transforms, clips, retained opacity, and PaintSequence reuse apply without image-specific traversal.

Vector drawing is expanded while PaintContext records the sequence.
The expansion adds a destination clip and a view-box transform, then reuses the existing immutable FillPathCommand, StrokePathCommand, clip, and transform commands.
Every renderer therefore uses its established path implementation instead of maintaining a second vector interpreter.

Canvas resolves an application resource during composition and captures the cheap ImageAsset value:

```cpp
const ImageAsset logo = UseImage(app::images::logo);

return Canvas([logo](PaintContext& paint, Size size) {
  paint.DrawImage(logo, Rect{0.0F, 0.0F, size.width, size.height});
});
```

## Native image caches

Each renderer caches decoded native images by the resolved immutable ImageAsset's private stable identity.

- Android decodes with BitmapFactory and replays with `Canvas.drawBitmap`.
- macOS decodes through ImageIO and replays a retained CGImage.
- Windows decodes through WIC and creates a Direct2D bitmap.

Android transfers encoded bytes to Java only on a bitmap-cache miss.
Subsequent frames identify the same asset without recreating a byte array or decoding again.

Native image caches use a 64 MiB decoded-byte LRU budget and remain renderer-owned.
An individual decoded image larger than the budget is retained as the cache's sole entry so repeated frames do not
decode it again; the next distinct image evicts it normally.
Device loss clears Windows device-dependent bitmaps but does not invalidate ImageAsset, layout, or PaintSequence data.
Destroying an Android View releases its Bitmap cache, and macOS cache entries use balanced Core Foundation ownership.
macOS maps source and destination rectangles while drawing the retained full CGImage, avoiding a cropped CGImage allocation per command.

The current Android, iOS, macOS, and Windows backends decode synchronously on the first cache miss.
The Web backend keeps asynchronous loading and failure state inside its renderer, draws no image while loading, and asks its PlatformAdapter to schedule a frame when decoding completes.

A later preload service may warm native caches without changing ImageAsset, Image, or DrawImageCommand.

## Future platform mapping

The resource and image contracts treat supported platforms as an open-ended set.
New backends implement PlatformResources and native image replay without adding platform variants to shared application code.

### iOS

iOS reads the reserved resource directory from the application bundle and uses the same versioned index as macOS. The native Xcode shell stages the package generated by the CMake application helper into that directory for Simulator and physical-device application bundles.
ImageIO produces renderer-owned CGImage values, while the platform adapter reports system Locale and display scale through ResourceConfiguration.
Security-scoped URLs are native service inputs whose bytes may be converted to ImageAsset with FromEncoded; they are not package ResourceIds.

### OHOS

OHOS stages the resource index and payload into HAP rawfile storage.
Its PlatformResources implementation owns native resource-manager and rawfile lifetimes, converts reads into shared RawAsset storage, and never exposes native handles to Runtime.
The OHOS renderer owns decoded platform image values and native cache release.

### Linux

Linux resolves `<executable-name>.resources` beside the running executable instead of relying on the current working directory.
AppImage, Flatpak, Snap, or another distribution may relocate the executable and its sidecar together without changing resource identity.
The eventual Linux renderer chooses its own decoder and native cache without changing ImageAsset.

### Web

The Web entry integration loads the generated resource index and payload before creating Runtime, then exposes them through WASM memory or a virtual filesystem.
Resource lookup and localized string formatting therefore remain synchronous after application startup.

Browser-native image decoding may complete asynchronously.
The Web renderer keeps its loading entry, creates an ImageBitmap, and requests another frame through WebPlatformAdapter when the image becomes ready.
Decoded browser images follow the same 64 MiB renderer-owned LRU budget as native decoded-image caches.
The current PaintSequence remains valid because DrawImageCommand already retains immutable encoded bytes and complete geometry.

Network fetches are not ResourceIds.
Application or module code fetches bytes asynchronously, constructs ImageAsset with FromEncoded, updates controlled state, and lets ordinary recomposition replace the image.

## Locale

Locale becomes a formal Environment value:

```cpp
class Locale {
public:
  static Locale FromLanguageTag(std::string language_tag);
  static Locale Default();

  [[nodiscard]] std::string_view LanguageTag() const noexcept;

  bool operator==(const Locale&) const = default;
};
```

Runtime seeds the root Locale from ResourceConfiguration.
Applications may override it for any subtree with ProvideEnvironment.

```cpp
return ProvideEnvironment(
    Locale::FromLanguageTag("zh-Hans-CN"),
    [] {
      return Text(app::strings::save);
    }
);
```

The initial implementation publishes Locale for resource lookup, and the Runtime-owned selection overlay applies it to its localized toolbar text.
Propagating that inherited Locale into otherwise-unspecified Text, TextField, and Canvas shaping options remains planned.
When added, platform renderers will continue to receive resolved TextShapingOptions and will never read StringResource values.

## Localized strings and formatting

StringResource entries are localized message templates rather than unvalidated static strings.
Catalogs live under `strings/` and use locale filenames:

```text
strings/default.properties
strings/zh-TW.properties
strings/en-GB.properties
```

The catalog format is a strict UTF-8 HuxerUI properties subset rather than the legacy Java Properties encoding:

- Empty lines and lines beginning with `#` are ignored.
- Each entry uses `key = value`; the first `=` separates the key and value.
- Unquoted values are trimmed.
- Double-quoted values support `\n`, `\t`, `\"`, and `\\` escapes.
- Java-style ISO-8859-1 input, `\uXXXX` escapes, `!` comments, alternate `:` separators, and continued lines are not supported.

The default catalog declares every key and its formatting schema.
Locale catalogs may translate those keys but cannot extend their argument schemas.
The public API uses indexed positional arguments:

```cpp
template <class... Arguments>
std::string UseString(StringResource resource, Arguments&&... arguments);
```

Example catalogs may contain:

```properties
welcome = "Hello, {0}"
file_count = "{0} has {1} files"
```

A translation may reorder or repeat arguments:

```properties
welcome = "你好，{0}"
file_count = "共有 {1} 个文件，属于 {0}"
```

The call site remains compact:

```cpp
Text::Format(app::strings::file_count, user_name, file_count);
```

Indexed arguments are preferred over anonymous `{}` placeholders because translations may reorder values.
Named arguments are deliberately omitted because indexed positions provide the required translation freedom without adding public argument objects and helper functions.

Formatting rules are:

- Placeholders use zero-based indices such as `{0}` and `{1}`.
- A translation may reorder, repeat, or omit declared arguments.
- The default catalog declares a contiguous argument range starting at zero.
- A translation cannot reference an index outside the default schema.
- `{{` and `}}` produce literal braces.
- A call must provide exactly the contiguous argument schema declared by the default catalog.
- A translation may omit a declared value, but the call site still supplies the complete default schema.

The initial resource tool parses and validates templates and stores them in the index.
Runtime expands their indexed arguments during lookup.
A future plural or rich-message implementation may compile templates into instructions without changing UseString.

The standard supported argument values are strings, string views, C strings, signed and unsigned integers,
floating-point values, and booleans.
Other values are accepted when they support classic-locale stream insertion.
Formatting does not use the process-global C locale.

An unsupported argument type is rejected at compile time.
Missing or extra arguments produce `std::invalid_argument` diagnostics containing the full resource identity.

Text::Format with a string view remains the lightweight formatter for non-localized application text.
Its StringResource overload performs locale resolution and message validation before Text stores the final UTF-8 value.
UseString exposes the same resolution for non-Text consumers.
StringVariant::Format retains the resource identity and formatted arguments until a deferred consumer composes in its captured Environment.

### Deferred plural and select messages

Plural and select messages require real locale rules and are not approximated with an English singular-versus-plural branch.

The indexed message format reserves argument references for future syntax such as:

```text
{0, plural,
  one {# file}
  other {# files}
}
```

Adding CLDR-backed plural, number, date, currency, or select instructions does not change StringResource or the variadic UseString call surface.

## Framework strings

The built-in catalog owns framework-rendered default text in the `huxerui` resource domain:

```text
huxerui:strings/dialog_ok
huxerui:strings/dialog_cancel
huxerui:strings/text_selection_cut
huxerui:strings/text_selection_copy
huxerui:strings/text_selection_paste
huxerui:strings/text_selection_select_all
huxerui:strings/window_minimize
huxerui:strings/window_maximize
huxerui:strings/window_restore
huxerui:strings/window_close
huxerui:strings/validation_required
huxerui:strings/validation_email
```

The framework bundle provides the default English catalog plus `ar`, `bn`, `cs`, `da`, `de`, `el`, `es`, `fa`, `fi`, `fil`, `fr`, `he`, `hi`, `hu`, `id`, `it`, `ja`, `ko`, `ms`, `nb`, `nl`, `no`, `pl`, `pt`, `pt-BR`, `pt-PT`, `ro`, `ru`, `sv`, `th`, `tr`, `uk`, `ur`, `vi`, `zh`, `zh-HK`, `zh-MO`, and `zh-TW` catalogs.
Its precompiled package is the first merge input registered by `huxerui_add_app`, so a later application root may intentionally replace any matching `huxerui` variant by using `NAMESPACE huxerui`.
The framework does not require a separate override manifest or public resource-bundle API.

`TextSelectionMenuLabels` and `WindowCaptionLabels` remain explicit local overrides; absent values resolve the generated framework keys like any other StringResource.
Each empty `TextSelectionMenuLabels` field falls back independently, so an application may replace one action label without duplicating the other localized defaults.
`ValidationResult` retains a StringVariant until TextField composition so built-in rules and application rules use the same deferred localization path.
Direct Runtime integrations that render framework defaults provide the merged resource package just like applications using ordinary StringResource values; the Runtime does not special-case the `huxerui` domain or carry a duplicate fallback table.
The framework package also provides `images/check` and `images/chevron_right` as tintable vectors for Checkbox and Menu.
These fixed component glyphs resolve through the same generated resource keys as application images rather than relying on platform fonts or a duplicate code-defined shape.
Simple stateful glyphs that are clearer as programmatic paths, such as window controls, are not migrated solely for uniformity.

## Invalidation and retained rendering

Resource reads participate in the existing dependency and invalidation model:

- UseString, Text resource construction, Text resource formatting, UseImage, and UseVectorImage resolve from the Runtime-owned service during composition without allocating state slots.
- An Image constructed from ImageResource resolves that key during composition and retains the resulting immutable raster or vector asset.
- ResourceConfiguration changes currently invalidate the root composition so locale and density variants are selected consistently.
- A visible selection overlay is repainted and remeasured when ResourceConfiguration changes because it is Runtime-owned rather than part of the application composition tree.
- Reconciliation limits a changed image asset or intrinsic size to the affected node's measure, layout, and paint paths.
- An unchanged image asset compares equal and preserves its recorded PaintSequence.
- Presentation-only changes continue to reuse raster image commands, decoded bitmaps, and expanded vector paths.

Release resource bundles are immutable.
A future development hot-reload implementation may publish content revisions through the same dependency records without introducing a second invalidation path.

## Error behavior

The current resource tool rejects:

- Duplicate resource variants within one root and generated C++ identifier collisions.
- Invalid resource namespaces, paths, locale tags, and image scales.
- Image files whose metadata cannot be read.
- Scale variants with inconsistent intrinsic logical dimensions.
- SVG density suffixes, mixed raster/vector variants, and unsupported SVG elements, attributes, styles, or units.
- Malformed format templates.
- Non-contiguous default argument indices.
- Translations that reference undeclared indices.

Runtime treats an unsupported resource-index version or a generated key missing from the installed package as a framework or packaging failure.

ImageAsset factories use `std::invalid_argument` for empty or malformed encoded data, unsupported formats, non-finite scales, and non-positive scales.
ImageAsset::FromFile uses `std::invalid_argument` for inaccessible or unreadable input and includes the path in the diagnostic.

Image and PaintContext validate non-finite geometry, invalid source rectangles, invalid alignment values, and opacity outside `[0, 1]` at the earliest public boundary.

Public API and runtime diagnostics are English, begin with `HuxerUI`, and include the relevant ResourceId, path, locale, variant, or argument index.
Asset packaging diagnostics are English and use the `hapt:` CLI prefix.

## Validation

The initial implementation has focused shared coverage for:

- Typed keys, resource-index parsing, and payload-hash validation.
- Locale normalization and fallback.
- Scale-variant selection.
- ImageAsset moved-byte and copied-byte metadata, scale, equality, and byte-lifetime behavior.
- VectorAsset construction, immutable geometry, painting, tint, and validation.
- SVG compilation, payload signatures, runtime resolution, unsupported features, and density-suffix rejection.
- RawAsset byte, MIME, string-view lifetime, embedded-null, and owned-string behavior.
- Indexed string lookup, default argument-count enforcement, and invalid template generation.
- Direct Text resource construction and Text resource formatting.
- Intrinsic Image measurement and Contain geometry.
- DrawImageCommand recording, bounds, source validation, and opacity validation.
- Resource code generation, payload staging, and generated-identifier collision detection.
- Ordered package input precedence, variant coexistence, repeated namespaces, stale payload removal, final-family validation, and payload-hash validation.

Every renderer implements image decode, cropping, destination scaling, sampling, and bounded native caches.
Windows common builds and tests plus Android compilation are required for this implementation; macOS must be built on macOS before release.

Future platform and SDK work adds installed-package, ordered target integration, iOS archive export, OHOS, Linux, and Web release-packaging validation as those capabilities become available.

## Delivery sequence

The delivery sequence is:

- Land this design and align SDK, Canvas, Text, and roadmap documentation.
- Add ResourceId, typed keys, resource index generation, PlatformResources, AppResources, Locale, and package staging.
- Add RawResource and RawAsset as the smallest byte-loading path and use it to validate the package boundary.
- Add ImageAsset factories, ImageResource resolution, Image, DrawImageCommand, Canvas replay, and the current Android, iOS, macOS, and Windows native decoders.
- Add VectorAsset construction, compiled SVG resources, automatic ImageResource format detection, vector tint, and shared path replay.
- Add StringResource formatting and locale fallback.
- Extend `hapt` with exact namespace generation and ordered binary-index merging.
- Permit repeated target resource roots in CMake and update generated CLI projects to use `resources/images`, `resources/strings`, and `resources/raw`.
- Add the built-in `huxerui` resource package and migrate framework-rendered Dialog, selection-menu, validation, and window-caption defaults.
- Migrate the framework-owned check and submenu-chevron vectors required by Checkbox and Menu.
- Add inherited Locale text shaping. This remains planned.

Each slice updates public headers, standalone-header checks, common tests, platform builds, packaging metadata, examples, and the relevant documentation together.

## Final design constraints

- Resource keys are typed; a generic public Resource template is not introduced.
- `NAMESPACE` is the exact resource domain and generated C++ namespace; `_resources` is a generated-header suffix only.
- One target may declare multiple ordered resource roots, and later matching variants replace earlier variants.
- Runtime consumes one final merged index rather than a stack of bundles or an override registry.
- HuxerUI source resource roots use `resources/images`, `resources/strings`, and `resources/raw` directly.
- Resource payload values are immutable and cheap to copy.
- ImageAsset exposes encoded bytes but never native image objects or ambiguous decoded pixel data.
- VectorAsset exposes immutable platform-neutral geometry without a public ImageKind or native path objects.
- RawResource is the explicit arbitrary-byte resource kind.
- PlatformResources returns shared RawAsset storage and does not require an intermediate byte-vector copy.
- Packaged resources are synchronously readable before Runtime starts; the Web entry integration performs asynchronous transport during startup.
- Filesystem construction is synchronous and distinct from native URI services.
- Localized formatting uses indexed positional arguments and permits translation reordering.
- StringResource values resolve during composition and never enter PaintCommand as resources.
- ImageFit is resolved before native replay and does not change parent layout policy.
- Native renderers own decoding, asynchronous readiness when required, and bounded device-resource caches.
- Resource lookup reuses root services, Environment, Layer capture, and existing invalidation instead of adding another context or observer system.
