# Resources and Images

## Package application resources

The generated app registers its `resources` directory with `huxerui_add_app(... RESOURCES resources RESOURCE_NAMESPACE app)`. Put static resources in these directories.

```text
resources/
  images/
    logo.png
    logo@2x.png
    mark.svg
  raw/
    about.txt
  strings/
    default.properties
    zh.properties
```

Include the generated `<app_resources.h>` for keys in `app::images`, `app::raw`, and `app::strings`. `Image(app::images::logo)` and `Text(app::strings::welcome)` resolve their resources under the mounted Environment. Apple asset catalogs, Android manifests, and other shell assets stay in their platform projects.

Libraries register a primary resource root through `huxerui_add_library`. For additional roots, call `huxerui_add_resources` on the target in precedence order.

```cmake
huxerui_add_resources(my_app
        ROOT "${CMAKE_CURRENT_SOURCE_DIR}/branding"
        NAMESPACE app
)
```

The namespace identifies both the resource domain and generated C++ namespace. A later root replaces matching variants and keeps the others. Roots on the same target must be distinct and nonoverlapping. Library dependency resources enter the application package through `huxerui_use_library`.

Static PNG, JPEG, and SVG files under `images` produce image keys. Name density variants with an `@2x` or `@3x` suffix and give them the same logical size at their declared scales. SVGs use one scale-independent variant. See the [compiled SVG subset](../design/resources.md#vectorasset-and-svg-resources) when preparing vector assets.

## Localized strings

Declare keys and their positional formatting arguments in `strings/default.properties`. Add language or regional overrides in files such as `strings/zh.properties` or `strings/zh-TW.properties`.

```properties
welcome = Hello, {0}
file_count = "{0} has {1} files"
```

Translations may reorder, repeat, or omit arguments from the default catalog. Callers supply the full default argument list. Use `{{` and `}}` to render literal braces. `Text::Format(app::strings::file_count, user_name, count)` defers resolution until the Text is mounted. Use `UseString()` if a composable needs the resolved string immediately.

To override the locale for a subtree, wrap its content in `ProvideEnvironment(Locale::FromLanguageTag("zh-Hans-CN"), content)`. For application-thread callbacks, capture the service obtained from `UseService<Resources>()` in composition. Its `GetString()`, `GetImage()`, `GetVectorImage()`, and `GetRawResource()` methods resolve values without subscribing to changes.

## Read raw assets and independent packages

`UseRawResource()` returns a lightweight reference without loading its payload. `ReadBytes()` and `ReadString()` return owned contents. Pass `true` to keep successfully read package bytes for reuse across copies of that asset. An existing cache remains available even when the argument is `false`. `ReadString()` preserves bytes, including BOMs and embedded nulls.

Use `OpenRead()` for a synchronous cursor or `OpenReadAsync()` in a task to consume the package incrementally. Each open starts from byte zero, and stream reads do not fill the whole-asset cache. Use `RawAsset::FromBytes()` for owned memory, `CopyBytes()` for borrowed memory, or `FromSharedBytes()` to retain an externally owned immutable range.

`Resources::OpenPackage(File("path/to/package"))` opens the `hrc` output package directory containing `huxerui/resources.bin` and its payloads. The resolver can be shared across threads and used outside Runtime. It uses English and 1x by default. Pass a `ResourceConfiguration` for another locale or display scale. Keep the resolver alive through uncached raw access and keep its package directory stable. Already opened streams own their sources.

## Display and draw images

Pass an `ImageResource`, `ImageAsset`, `VectorAsset`, or `Bitmap` to `Image`. Use `Fit()` and `Align()` to place the image inside its layout box. `Sampling()` selects raster filtering. `Tint()` applies to vector content. A live `ExternalTexture` uses the separate `Image(std::shared_ptr<ExternalTexture>)` constructor.

Resolve a concrete asset in composition before capturing it for custom drawing. Use `UseImage()` for raster resources or `UseVectorImage()` for vector resources.

```cpp
[[huxerui::composable]]
View LogoCanvas() {
  const auto logo = UseImage(app::images::logo);

  return Canvas([logo](PaintContext& paint, Size size) {
    paint.DrawImage(logo, {0.0F, 0.0F, size.width, size.height});
  });
}
```

## Load, edit, and save pixels

`ImageAsset` retains encoded PNG or JPEG bytes. `ImageAsset::FromFile(file)` reads a local `File` synchronously. For a [FileReference](files.md#external-files-and-directories), an asynchronous local read, or a network response, pass the successfully read bytes to `ImageAsset::FromEncoded()`. Use `CopyEncoded()` when the bytes are borrowed.

Await `DecodeAsync()` to get a `Bitmap` with upright RGBA8 pixels at the asset's scale. Orientation metadata can swap the pixel axes. `PixelWidth()` and `PixelHeight()` describe the stored grid, while `IntrinsicSize()` divides those dimensions by `Scale()`. A 400 × 200 bitmap at scale 2 has a natural 200 × 100 logical size.

`Crop()`, `Resize()`, and `Transform()` return bitmaps and preserve scale. Use `Bitmap::FromRgba8()` to take ownership of tightly packed pixels or `CopyPixels()` to import borrowed RGBA, BGRA, or ARGB rows with a stride.

```cpp
Task<bool> SaveSquareThumbnail(ImageAsset image, File destination) {
  const Bitmap pixels = co_await image.DecodeAsync();
  const Bitmap thumbnail = pixels.Resize(160, 160);
  co_return co_await thumbnail.SaveAsync(std::move(destination), {.format = ImageFormat::Png});
}
```

`ImageAsset::SaveAsync()` writes the original bytes and metadata. `Bitmap::SaveAsync()` encodes before writing to `File`. PNG preserves transparency. JPEG composites against the opaque background in `ImageEncodeOptions`. Use `EncodeAsync()` to keep the output bytes in memory. For a complete picker, editing, and export flow, see the [Bitmap example](../../examples/bitmap/main.cpp).

Call `ExternalTexture::SnapshotAsync()` on the owning UI thread to capture the current frame. Await the task to receive a scale-1 `Bitmap`, or an empty optional before the first frame. A finished producer can still supply its last frame.

On HarmonyOS, include `<huxerui/ohos/external_texture.h>` and publish straight-alpha sRGB rows through `ohos::PixelTexture`. The texture copies the rows during publication and can receive frames from a producer thread. On API 22 or later, check `ohos::NativeBufferTexture::IsSupported()` to publish an immutable premultiplied sRGB RGBA8 buffer with the system's CPU read and write usage flags. Finish producer writes before publishing it. The texture retains the shared buffer as a system PixelMap, and the platform drawing backend controls pixel upload.

See [Resource and image design](../design/resources.md) for package resolution, cache ownership, and renderer behavior.
