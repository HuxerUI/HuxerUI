# App Resources, Images, and Localization Design

This document defines package construction, resource resolution, and image rendering ownership. See [Resources and Images](../guide/resources.md) for application usage and [Platform Content Integration](platform-content.md#externaltexture) for live texture publication.

## Typed resource identity

`ImageResource`, `RawResource`, and `StringResource` are typed `ResourceId` values with a domain and a key. Density, locale, package path, and storage format are properties of index entries. One image key can resolve to a raster or vector asset. Mounted `ImageVariant` consumers resolve the key to either format, while the direct raster and vector resolvers require their respective formats.

## Build and package model

The host resource compiler (`hrc`) validates each declared root, generates payloads and metadata, and merges the resulting packages into one target index. Framework resources are built once and installed with the SDK. The application merge reads the framework package first, then the declared library packages, then application roots in declaration order. Library merges include their dependency packages before their own roots.

A later entry replaces the same variant identity. Raw entries use domain and key. Strings add locale, and images add locale and density. Other variants survive the merge. The compiler validates the merged resource families for kind and format consistency, image dimensions, and localization schemas. It rejects duplicate variants within one root, generated identifier collisions, invalid paths and metadata, and unsupported SVG constructs.

HUXRES version 1 stores typed entries, normalized UTF-8 package paths, content hashes, and image metadata. The final staging tree contains `huxerui/resources.bin` and the payloads reachable from it. Generated headers reflect the final index and are grouped by registered namespace. The build tracks changes to the root list and file list and removes stale staged files.

Windows and Linux stage a sidecar beside the executable. macOS and iOS stage the package in the application bundle. Android synchronizes the completed CMake package into APK assets through Gradle. Web preloads the package into the virtual filesystem before Runtime starts. Each host consumes the same package layout and keeps its native asset paths private.

## Runtime resource ownership

Runtime installs one `AppResources` resolver for its final index. A `UiWindow` can supply a resource configuration that overrides the application defaults. An inherited `Locale` can override locale for a subtree. Captured layer environments preserve their originating resources and locale.

The composition hooks observe the effective configuration they use, including on cache hits, without allocating ordered state slots. `Resources` service methods use the effective configuration or an explicit snapshot without subscribing composition. The Runtime-owned service requires the original application lifetime and thread. Reconciliation confines layout and paint invalidation to the affected consumers.

`PlatformResources` provides the host configuration and opens a fresh input stream for each package payload. Configuration is read on the Runtime thread. Payload opening may run on a worker, and the returned stream owns what it needs to outlive that call and Runtime. A missing payload returns an empty optional, and an empty payload returns a stream already at EOF. Android asset-open failures have no detailed native error category. Hosts finish asynchronous package transport before publishing a synchronously readable package.

### Independent resource packages

`Resources::OpenPackage()` uses the same resolver and caches with an independent directory source. It validates the index at open and reads payloads on demand. Concurrent lookups synchronize cache access, with file reads and parsing outside the cache lock. Package authenticity and payload integrity belong to the directory owner.

## Resource variants

`AppResources` sorts the private index once by kind, domain, key, locale, and scale, then binary-searches the range for the requested resource. It chooses the smallest image scale at least as large as the display scale, or the largest available scale if all are smaller. Density variants share an intrinsic logical size equal to encoded dimensions divided by scale. SVG resources have one scale-independent variant. Localized images and appearance or contrast qualifiers are unsupported.

Locale fallback tries the exact tag, then removes trailing variants. For a language with both script and region, the language-region catalog precedes the language-script catalog. For example, `zh-Hant-TW` falls back to `zh-TW`, `zh-Hant`, `zh`, then the default catalog.

## Localized strings and formatting

The host publishes locale and display-scale changes through the resource configuration. Changes invalidate recorded readers and schedule a frame. The effective Environment locale also supplies the shaping locale for mounted text, built-in labels, TextField, and Canvas text when their shaping locale is empty. An explicit shaping locale leaves resource selection independent.

`StringVariant` retains literal text or a resource and its formatting arguments until resolution in the effective Environment. Resolvers select and format the catalog entry before layout. Renderers receive resolved text and shaping options.

Catalogs are a strict UTF-8 properties subset. They ignore blank lines and `#` comments, split on the first `=`, trim unquoted values, and support `\n`, `\t`, `\"`, and `\\` escapes in quoted values. The default catalog declares keys and contiguous zero-based argument indices. Translations can reorder, repeat, or omit those indices, and the caller always supplies the complete default schema. `{{` and `}}` are literal braces. Value conversion uses the classic locale. General plural, select, and locale-aware numeric formatting are unsupported.

The built-in `huxerui` domain uses the same package and lookup model for framework strings, including text selection actions. A missing generated key or unsupported index version is a packaging or configuration error. Template argument mismatches and malformed or incorrectly typed image payloads are invalid arguments.

## RawResource and RawAsset

A resolved `RawAsset` contains immutable owned bytes or a package entry with MIME metadata and a weak resolver reference. Looking up the asset records the selected path without opening it. Whole-buffer reads return copies. A successful cached read publishes the complete immutable bytes once for shared reuse and never publishes a partial read. String reads preserve the original bytes, including a BOM or embedded null.

Streams own their current source and position. Opening a stream never populates the whole-asset cache. File and resource inputs share the worker adapter for blocking reads. Memory reads can complete immediately, while Web uses its virtual filesystem queue. Cancellation suppresses delivery and lets a retained native read finish safely.

Runtime disconnects its resource adapter under the same lock used to open payloads. After disconnect or independent resolver destruction, uncached package access fails. Already opened streams and retained cached or memory assets remain usable. An empty asset value and a valid zero-length payload remain distinct. Packaged fonts reuse raw-resource payloads and have their matching and native loading contracts in [Text](text.md#font-and-style).

## ImageAsset and Bitmap

`ImageAsset` owns the original immutable PNG or JPEG bytes and their metadata. Factory construction reads dimensions and EXIF orientation without decoding pixels. The asset reports the encoded dimensions and logical size. Full decoding applies orientation to produce upright pixels and may swap the axes. Saving the asset writes the original bytes and metadata. The package resolver caches encoded values by selected path and content hash.

`Bitmap` owns immutable, top-down, tightly packed sRGB RGBA8 pixels with straight alpha. Bitmap operations keep the logical scale. Resize extends edge samples, affine transforms sample outside the source as transparent, and linear interpolation uses premultiplied alpha. Encoding preserves alpha in PNG and composites JPEG against the selected opaque background. Saving delegates to the file writer after encoding succeeds. The Linux decoder supplies RGB samples without converting embedded ICC profiles to sRGB.

## VectorAsset and SVG resources

`VectorAsset` owns a view box, intrinsic size, and an immutable sequence of platform-neutral path, clip, and transform operations. `hrc` compiles SVG to HUXVEC version 1. Runtime identifies vector payloads by their signature and does not parse source SVG.

The compiler supports `svg`, `g`, `defs`, `use`, `clipPath`, `linearGradient`, `radialGradient`, `stop`, `path`, `rect`, `circle`, `ellipse`, `line`, `polyline`, and `polygon`. It supports viewport mapping with `none`, `meet`, and `slice`, element transforms, stroke and dash properties, inherited color and `currentColor`, static visibility, and arc conversion to cubics. A clip can contain one drawable `userSpaceOnUse` path with multiple contours. File-local references are resolved at compile time with missing-reference and cycle checks.

Gradients support local inheritance, object-bounds or user-space units, transforms, ordered stops, opacity, and pad extension. Each operation stores its complete affine matrix in normalized gradient coordinates. Local transforms override inherited transforms, single-stop gradients become constant gradients, and repeated stops preserve hard transitions. Singular transforms, repeat or reflect extension, and nonconcentric radial focal geometry are rejected.

Unsupported constructs include external references, stylesheets, scripts, text, embedded bitmaps, filters, masks, animation, fractionally opaque groups, and lengths requiring a browser layout context. Compiler errors identify the source file.

## Image measurement and painting

`Image` measures from intrinsic logical size, shrinking uniformly to finite maximum constraints before applying the parent constraints. Padding determines the paint content box. Fit and alignment determine destination geometry and the visible source region during recording. Renderers replay that geometry without recalculating fit.

`DrawImageCommand` retains the immutable raster value and geometry. Live textures use `DrawExternalTextureCommand` and can publish new frames while retaining the same paint sequence. Destination bounds contribute to culling and damage. Vector recording expands its content into existing path operations under a destination clip and view-box transform.

### Platform image caches

Each renderer caches platform images by the stable storage identity shared across copies. A miss decodes an encoded asset or uploads a bitmap. Native renderers use WIC on Windows, ImageIO on Apple hosts, BitmapFactory on Android, and GdkPixbuf on Linux. Web starts an asynchronous ImageBitmap decode, draws no image until it is ready, and requests another frame on completion.

Renderer caches use a decoded-byte LRU budget. Linux uses 32 MiB and caps its entry count, while the other current renderers use 64 MiB. An oversized entry can remain the only cached image until the next distinct image is needed. Native cache entries are released with their renderer or device resources. Retained paint commands and immutable image values can be replayed with a new renderer cache.

## Validation

[Resource compiler tests](../../tests/resource_compiler/compiler.cpp) and [CMake resource merge tests](../../tests/cmake/resource_merge.cmake) cover package generation and merging. [Resource unit tests](../../tests/unit/resource.cpp) and [Runtime resource tests](../../tests/runtime/resources.cpp) cover resolution, retained assets, and invalidation. The [Bitmap example](../../examples/bitmap/main.cpp) exercises codec and file integration.
