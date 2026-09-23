#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <locale>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/data.h>
#include <huxerui/geometry.h>
#include <huxerui/stream.h>
#include <huxerui/vector.h>

namespace huxerui {

class StringVariant;
/// Resolves direct text or a localized template into an owned string.
/// @param value Literal text or a resource identity with already captured formatting arguments.
/// @return A copy of literal text, or text resolved using the effective Locale during composition.
/// @throws std::logic_error If a resource requires an unavailable composition context, service, or installed key.
/// @throws std::invalid_argument If the supplied argument count differs from the resource template.
/// Literal-only values do not require a Runtime or read resource configuration.
std::string UseString(const StringVariant& value);
/// Resolves an expendable text value, reusing literal storage when possible.
/// @param value Literal text or a deferred resource value whose storage may be consumed.
/// @return Owned text with the same locale, argument, and error rules as the const-reference overload.
/// @see UseString(const StringVariant&)
std::string UseString(StringVariant&& value);

namespace detail {
struct InternalAccess;
class AppResources;
struct ApplicationRuntimeState;
bool IsEmptyStringVariantLiteral(const StringVariant& value) noexcept;
} // namespace detail

/// An owned resource domain and key, identifying an entry rather than opening its payload.
/// Prefer generated typed keys in application code. Domains use ASCII letters, digits, '.', '_', or '-';
/// keys are normalized package-relative paths without traversal. Invalid identities are rejected.
/// @code{.cpp}
/// ResourceId id("app", "images/logo");
/// std::string diagnostic = id.ToString(); // "app:images/logo".
/// @endcode
class ResourceId {
public:
  ResourceId(std::string_view domain, std::string_view key);

  /// Returns the namespace that owns this resource, such as an application or library domain.
  /// @return A view borrowing this identity's storage; use it only while that storage remains unchanged and alive.
  [[nodiscard]] std::string_view Domain() const noexcept {
    return domain_;
  }

  /// Returns the logical resource key, not a native path or a selected locale/density payload path.
  /// @return A view borrowing this identity's storage; use it only while that storage remains unchanged and alive.
  [[nodiscard]] std::string_view Key() const noexcept {
    return key_;
  }

  /// Serializes the identity for diagnostics.
  /// @return An owned string in "domain:key" form; it is not a filesystem path.
  [[nodiscard]] std::string ToString() const;

  bool operator==(const ResourceId&) const = default;

private:
  std::string domain_;
  std::string key_;
};

/// A typed key for a packaged raster or compiled vector image.
/// Resolving selects the effective locale and display density; the key itself retains no decoded image.
/// Generated keys use resource_namespace::images::name for both raster and vector resources.
class ImageResource final : public ResourceId {
public:
  using ResourceId::ResourceId;

  bool operator==(const ImageResource&) const = default;
};

/// A typed key for a localized string or positional string template.
/// Use StringVariant::Format() for deferred formatting or UseString() for explicit resolution during composition.
/// Generated keys use resource_namespace::strings::name.
class StringResource final : public ResourceId {
public:
  using ResourceId::ResourceId;

  bool operator==(const StringResource&) const = default;
};

/// Owned literal text or a deferred localized template accepted by text-consuming APIs.
/// Literal inputs are retained as text; resource inputs retain their identity and captured argument strings.
/// Resolving a resource is deferred until UseString() or a mounted consumer has an effective Locale.
/// Examples use app as the configured resource namespace; use the names from your generated resource header.
/// @code{.cpp}
/// StringVariant label = StringVariant::Format(app::strings::file_count, 3);
/// // An installed template such as "{0} files" resolves in the consumer's locale.
/// @endcode
class StringVariant {
public:
  StringVariant() = default;
  StringVariant(std::string value);
  StringVariant(std::string_view value);
  StringVariant(const char* value);
  StringVariant(StringResource resource);

  /// Captures positional arguments without resolving the resource or requiring a composition context.
  /// @tparam Arguments Types insertable into std::ostringstream using the classic locale.
  /// @param resource Generated string-template key, such as app::strings::file_count.
  /// @param arguments Values converted to owned strings now, in zero-based placeholder order.
  /// @return A deferred value; later resolution validates the exact argument count.
  /// Templates use {0}, {1}, and so on, with '{{' and '}}' for literal braces.
  /// This is not std::format syntax or locale-aware number/plural formatting.
  template <class... Arguments> static StringVariant Format(StringResource resource, Arguments&&... arguments);

  bool operator==(const StringVariant&) const = default;

private:
  StringVariant(StringResource resource, std::vector<std::string> arguments);

  std::variant<std::string, StringResource> value_;
  std::vector<std::string> arguments_;

  friend struct detail::InternalAccess;
};

/// A typed key for arbitrary packaged bytes with no implied text or image interpretation.
/// UseRawResource() resolves it lazily; raw payload selection does not depend on Locale or display density.
/// Generated keys use resource_namespace::raw::name.
class RawResource final : public ResourceId {
public:
  using ResourceId::ResourceId;

  bool operator==(const RawResource&) const = default;
};

/// A language-tag value used by resource lookup and inherited text/UI localization.
/// Providing a Locale in Environment overrides the platform's resource locale for that subtree.
/// It does not change the process locale or the numeric conversion used by StringVariant::Format().
/// @code{.cpp}
/// Locale locale = Locale::FromLanguageTag("ZH_hans_cn");
/// std::string normalized(locale.LanguageTag()); // "zh-Hans-CN".
/// @endcode
class Locale {
public:
  /// Normalizes common language, script, and region subtags for resource matching.
  /// @param language_tag Nonempty language tag, such as "en-US" or "zh-Hans-CN"; underscores are accepted as separators.
  /// @return An owned tag with hyphen separators, lowercase language, title-case script, and uppercase region.
  /// @throws std::invalid_argument If a tag violates the supported subtag character or length rules.
  /// This performs structural normalization, not language-registry lookup or complete BCP-47 canonicalization.
  static Locale FromLanguageTag(std::string language_tag);
  /// Returns the framework's explicit default locale.
  /// @return The language tag "en"; this does not query the current operating-system locale.
  static Locale Default();

  /// Returns the stored normalized tag.
  /// @return A borrowed view valid while this Locale's storage remains unchanged and alive.
  [[nodiscard]] std::string_view LanguageTag() const noexcept {
    return language_tag_;
  }

  bool operator==(const Locale&) const = default;

private:
  explicit Locale(std::string language_tag) : language_tag_(std::move(language_tag)) {}

  std::string language_tag_;
};

/// Platform-supplied application defaults and per-UiWindow resource configuration.
/// The platform reports changes to the corresponding Runtime or UiWindow; local Locale overrides apply per subtree.
struct ResourceConfiguration {
  /// Default locale for resource resolution and inherited localization unless Environment overrides it.
  Locale locale = Locale::Default();
  /// Finite, positive pixels-per-logical-unit density used to choose image variants.
  /// Runtime and UiWindow reject zero, negative, or non-finite configuration values.
  float display_scale = 1.0F;

  bool operator==(const ResourceConfiguration&) const = default;
};

/// A copyable raw-resource value backed by immutable memory or a lazily opened installed-package entry.
/// UseRawResource() resolves identity and metadata without reading payload bytes. Copies share optional cached bytes.
/// Reads may run on workers. Uncached package access requires a connected Runtime resource service; retained memory
/// and already opened streams remain usable after Runtime destruction. Cache retention lasts as long as shared data.
/// @code{.cpp}
/// RawAsset asset = RawAsset::FromBytes({std::byte{'o'}, std::byte{'k'}}, "text/plain");
/// std::string text = asset.ReadString();
/// InputStream input = asset.OpenRead(); // Independent cursor; it does not consume asset.
/// @endcode
class RawAsset {
public:
  RawAsset() = default;

  /// Retains an owned binary buffer without interpreting its contents.
  /// @param bytes Payload transferred into immutable shared storage; empty bytes still form a valid asset.
  /// @param mime_type Optional descriptive MIME type, retained verbatim without validation.
  /// @return A memory-backed asset that does not depend on a Runtime resource service.
  static RawAsset FromBytes(Bytes bytes, std::string mime_type = {});
  /// Copies a borrowed binary range into immutable owned storage.
  /// @param bytes Payload readable for this call only; it need not outlive the returned asset.
  /// @param mime_type Optional descriptive MIME type, retained verbatim without validation.
  /// @return A memory-backed asset independent of the original buffer, including for empty input.
  static RawAsset CopyBytes(std::span<const std::byte> bytes, std::string mime_type = {});
  /// Retains an externally owned immutable byte range without copying it.
  /// @param owner Lifetime owner that keeps the entire range alive for all assets and streams sharing it.
  /// @param data Start of the readable range; may point into a larger allocation retained by owner.
  /// @param size Number of readable bytes. Zero creates a valid empty asset and permits null owner/data.
  /// @param mime_type Optional descriptive MIME type, retained verbatim without validation.
  /// @return A memory-backed asset sharing the supplied lifetime owner.
  /// @throws std::invalid_argument If size is nonzero and owner or data is null.
  /// The caller must keep the range immutable; retaining a const owner does not prevent writes through other aliases.
  /// @code{.cpp}
  /// auto storage = std::make_shared<const Bytes>(Bytes{std::byte{1}, std::byte{2}, std::byte{3}});
  /// RawAsset tail = RawAsset::FromSharedBytes(storage, storage->data() + 1, 2);
  /// @endcode
  static RawAsset FromSharedBytes(std::shared_ptr<const void> owner, const std::byte* data, std::size_t size,
                                  std::string mime_type = {});

  /// Reads the complete payload into an independent buffer, reusing existing storage as the source.
  /// @param cache Retain successfully read package bytes for all copies of this asset. False does not evict a cache.
  /// @return Owned bytes; a missing default asset and a valid empty asset both return an empty buffer.
  /// @throws std::logic_error If uncached package access has a missing payload or disconnected/expired service.
  /// @throws std::runtime_error If an opened stream reports an operational read failure.
  /// Platform-open and allocation exceptions may also propagate. No partial read is published to the shared cache.
  /// This can block; use OpenReadAsync() for incremental asynchronous consumption or RunWorker() for a complete read.
  [[nodiscard]] Bytes ReadBytes(bool cache = false) const;
  /// Reads the complete payload as text without decoding or validating its encoding.
  /// @param cache Retain complete package bytes in the same shared cache used by ReadBytes().
  /// @return An owned string preserving BOMs, embedded nulls, arbitrary bytes, and original newlines.
  /// A missing default asset returns an empty string. Blocking, cache, and exception rules match ReadBytes().
  /// @see ReadBytes
  [[nodiscard]] std::string ReadString(bool cache = false) const;
  /// Opens a fresh synchronous cursor without materializing an uncached package payload.
  /// @return A stream starting at byte zero, retaining its memory or native source independently of this asset.
  /// @throws std::logic_error If this asset has no value, its payload is missing, or uncached access lacks a live service.
  /// Platform-open exceptions may propagate. Later Read()/CopyTo() failures use IoResult rather than resource exceptions.
  /// Opening and reading run on the calling thread and never populate the complete-content cache.
  [[nodiscard]] InputStream OpenRead() const;
  /// Opens an independent cursor with asynchronous byte reads.
  /// @return A lazy task retaining this asset until opening completes and yielding a stream starting at byte zero.
  /// Opening has the same resource exceptions as OpenRead(), delivered when the task runs; the result is not IoResult.
  /// Blocking package I/O uses workers or the Web file queue. Memory reads may complete immediately.
  /// Later stream operations return IoResult. Neither opening nor incremental reading fills the complete-content cache.
  /// Cancellation suppresses delivery; an active native operation may finish before its retained source is released.
  [[nodiscard]] Task<AsyncInputStream> OpenReadAsync() const;
  /// Returns descriptive MIME metadata without opening or inspecting the payload.
  /// @return A view borrowing the shared asset storage; empty when absent or unspecified.
  [[nodiscard]] std::string_view MimeType() const noexcept;
  /// Tests whether this value has memory storage or a resolved package entry.
  /// @return False only for a missing default asset; true also for valid zero-length payloads.
  /// This performs no I/O and does not prove that a packaged payload still exists or can be opened.
  [[nodiscard]] bool HasValue() const noexcept;

  bool operator==(const RawAsset& other) const noexcept;

private:
  struct Data;
  explicit RawAsset(std::shared_ptr<const Data> data) : data_(std::move(data)) {}

  std::shared_ptr<const Data> data_;

  [[nodiscard]] std::shared_ptr<const huxerui::Bytes> CachedBytes() const;
  [[nodiscard]] std::shared_ptr<const huxerui::Bytes> EnsureCachedBytes() const;

  friend class ImageAsset;
  friend struct detail::InternalAccess;
  friend class detail::AppResources;
};

/// Encoded raster formats recognized by ImageAsset metadata parsing.
enum class ImageFormat {
  Png,  ///< Portable Network Graphics payload.
  Jpeg, ///< JPEG payload.
};

/// Sampling policy used when a renderer scales an image.
enum class ImageSampling {
  Nearest, ///< Select nearby source pixels without interpolation; useful for pixel art.
  Linear,  ///< Interpolate nearby source pixels for smoother scaling.
};

/// Immutable encoded PNG or JPEG data with pixel dimensions and a logical density scale.
/// Factories read metadata rather than decoding pixels; the platform renderer owns pixel decoding.
/// A default asset has no image. Use generated ImageResource keys for packaged images that need locale/density selection.
/// @code{.cpp}
/// Size MeasureImage(Bytes encoded) {
///   ImageAsset image = ImageAsset::FromEncoded(std::move(encoded), 2.0F);
///   return image.IntrinsicSize(); // Pixel dimensions divided by 2.
/// }
/// @endcode
class ImageAsset {
public:
  ImageAsset() = default;

  /// Synchronously reads a local file into owned encoded-image storage.
  /// @param path Native filesystem path accessible to this process; this is not a resource key or provider URI.
  /// @param scale Finite, positive pixels per logical unit.
  /// @return An image with parsed format and dimensions, independent of the source file's later lifetime.
  /// @throws std::invalid_argument If the file cannot be read, its metadata/format is invalid, or scale is invalid.
  /// Full pixel decoding is deferred; successful metadata parsing does not guarantee platform decoding will succeed.
  static ImageAsset FromFile(const std::filesystem::path& path, float scale = 1.0F);
  /// Retains an owned encoded image and parses its metadata.
  /// @param bytes Complete PNG or JPEG payload, not raw pixels or SVG text.
  /// @param scale Finite, positive pixels per logical unit; dimensions divided by scale must remain finite.
  /// @return A valid image sharing immutable encoded storage across copies.
  /// @throws std::invalid_argument If the payload is empty, unsupported, or has invalid metadata, or scale is invalid.
  static ImageAsset FromEncoded(Bytes bytes, float scale = 1.0F);
  /// Copies borrowed encoded bytes before parsing their metadata.
  /// @param bytes Complete PNG or JPEG payload readable for the duration of this call.
  /// @param scale Finite, positive pixels per logical unit; dimensions divided by scale must remain finite.
  /// @return An image independent of the original buffer, with the same validation rules as FromEncoded().
  /// @throws std::invalid_argument If the payload metadata/format or scale is invalid.
  static ImageAsset CopyEncoded(std::span<const std::byte> bytes, float scale = 1.0F);

  /// Borrows the retained encoded payload without decoding or copying it.
  /// @return A read-only span valid while its shared image storage remains alive; empty for a missing asset.
  [[nodiscard]] std::span<const std::byte> EncodedBytes() const noexcept;
  /// Reports the parsed encoded format.
  /// @return Png or Jpeg for a valid image; Png is the fallback for a missing asset, not proof of validity.
  [[nodiscard]] ImageFormat Format() const noexcept;
  /// Returns the MIME type derived from the encoded format.
  /// @return A view into shared storage containing "image/png" or "image/jpeg"; empty for a missing asset.
  [[nodiscard]] std::string_view MimeType() const noexcept;
  /// Returns the encoded image width before density scaling.
  /// @return Width in pixels, or zero for a missing asset.
  [[nodiscard]] std::uint32_t PixelWidth() const noexcept;
  /// Returns the encoded image height before density scaling.
  /// @return Height in pixels, or zero for a missing asset.
  [[nodiscard]] std::uint32_t PixelHeight() const noexcept;
  /// Returns the density used to convert pixel dimensions into logical units.
  /// @return Finite, positive pixels per logical unit; 1.0 for a missing asset.
  [[nodiscard]] float Scale() const noexcept;
  /// Returns the natural layout size without resampling the encoded image.
  /// @return PixelWidth()/Scale() by PixelHeight()/Scale(), or a zero size for a missing asset.
  [[nodiscard]] Size IntrinsicSize() const noexcept;
  /// Tests whether encoded storage and parsed metadata are present.
  /// @return True for an image value; false for a missing default asset. No platform decoding is performed.
  [[nodiscard]] bool HasValue() const noexcept;

  bool operator==(const ImageAsset& other) const noexcept;

private:
  struct Data;
  explicit ImageAsset(std::shared_ptr<const Data> data) : data_(std::move(data)) {}

  static ImageAsset FromRawAsset(RawAsset asset, float scale);

  std::shared_ptr<const Data> data_;

  friend struct detail::InternalAccess;
};

/// Image input retaining either a deferred packaged key, encoded raster asset, or compiled/programmatic vector asset.
/// Components resolve a key in their effective resource environment; already resolved assets need no key lookup.
/// ExternalTexture is separate because it represents a live shared texture rather than an immutable image value.
using ImageVariant = std::variant<ImageResource, ImageAsset, VectorAsset>;

/// Resolves owned resource values on the original application thread without subscribing composition.
/// Implicit configuration follows the calling window and local Locale; application-only tasks use application defaults.
/// Methods reject a stopped or different application lifetime. Resolved values can be retained independently.
/// @code{.cpp}
/// auto resources = UseService<Resources>();
/// const ResourceConfiguration configuration = resources->Configuration();
/// const std::string title = resources->GetString(app::strings::title, configuration);
/// @endcode
/// The example assumes an installed generated resource key. Use the UseXxx resource hooks when composition must react
/// to configuration changes; Get methods return an immediate value and do not establish that subscription.
class Resources final {
public:
  /// Reads effective resource configuration without observing composition dependencies.
  /// @return An owned snapshot including application defaults, original-window overrides, and local Locale.
  /// @throws std::logic_error If the original application lifetime, thread, or context is invalid.
  [[nodiscard]] ResourceConfiguration Configuration() const;
  /// Resolves a raw-resource identity without reading its payload.
  /// @param resource Installed package raw-resource key.
  /// @return A lazy asset that can later open or read its own stream.
  /// @throws std::logic_error If the key is absent or the application lifetime/thread is invalid.
  [[nodiscard]] RawAsset GetRawResource(RawResource resource) const;
  /// Resolves a packaged raster image with this call's effective resource configuration.
  /// @param resource Installed image key whose selected payload is raster data.
  /// @return A retained asset; first resolution may synchronously read and parse package data.
  /// @throws std::invalid_argument If the selected payload is vector data or malformed.
  /// Missing keys and invalid application lifetime/thread raise std::logic_error; native I/O errors may propagate.
  [[nodiscard]] ImageAsset GetImage(ImageResource resource) const;
  /// Resolves a packaged raster image using an explicit configuration snapshot.
  /// @param resource Installed key selecting raster image data.
  /// @param configuration Locale and finite positive display scale used for variant selection; not retained by
  /// reference.
  /// @return An owned raster asset without composition subscriptions.
  /// Uses the same payload, missing-key, lifetime, and I/O error rules as the implicit-configuration overload.
  [[nodiscard]] ImageAsset GetImage(ImageResource resource, const ResourceConfiguration& configuration) const;
  /// Resolves a packaged vector image with this call's effective resource configuration.
  /// @param resource Installed image key whose selected payload is vector data.
  /// @return A retained asset; first resolution may synchronously read and parse package data.
  /// @throws std::invalid_argument If the selected payload is raster data or malformed.
  /// Missing keys and invalid application lifetime/thread raise std::logic_error; native I/O errors may propagate.
  [[nodiscard]] VectorAsset GetVectorImage(ImageResource resource) const;
  /// Resolves a packaged vector image using an explicit configuration snapshot.
  /// @param resource Installed key selecting vector image data.
  /// @param configuration Locale and finite positive display scale used for variant selection; not retained by
  /// reference.
  /// @return An owned vector asset without composition subscriptions.
  /// Uses the same payload, missing-key, lifetime, and I/O error rules as the implicit-configuration overload.
  [[nodiscard]] VectorAsset GetVectorImage(ImageResource resource, const ResourceConfiguration& configuration) const;
  /// Resolves text immediately using this call's effective Locale.
  /// @param value Literal text or an installed string key with owned formatting arguments.
  /// @return Owned resolved text with no composition dependency.
  /// @throws std::logic_error If a key is absent or the original application lifetime/thread is invalid.
  /// @throws std::invalid_argument If formatting arguments do not satisfy the selected template.
  [[nodiscard]] std::string GetString(const StringVariant& value) const;
  /// Resolves text with an explicit configuration snapshot.
  /// @param value Literal text or a packaged string with owned formatting arguments.
  /// @param configuration Snapshot whose locale selects the string variant; unrelated display fields do not affect
  /// text.
  /// @return Owned resolved text; uses the same lifetime/key/format validation as the implicit-configuration overload.
  [[nodiscard]] std::string GetString(const StringVariant& value, const ResourceConfiguration& configuration) const;

private:
  Resources(std::shared_ptr<detail::AppResources> resources, std::weak_ptr<detail::ApplicationRuntimeState> application)
      : resources_(std::move(resources)), application_(std::move(application)) {}
  /// Rejects access outside this resource service's original live application and its owning thread.
  void RequireLifetime() const;
  std::shared_ptr<detail::AppResources> resources_;
  std::weak_ptr<detail::ApplicationRuntimeState> application_;
  friend class Runtime;
};

/// Platform capability for installed-package access and current resource defaults.
/// Platform Runtime subclasses provide this service; applications normally use Resources or generated UseXxx() helpers.
/// Package streams must own their native lifetime independently so an opened stream survives capability destruction.
class PlatformResources {
public:
  virtual ~PlatformResources() = default;

  /// Reports the platform's current locale and density defaults on the application thread.
  /// @return A configuration whose display_scale is finite and positive.
  /// The platform remains responsible for notifying Runtime when these defaults change.
  [[nodiscard]] virtual ResourceConfiguration Configuration() const = 0;
  /// Opens an installed payload without materializing its complete contents.
  /// @param package_path Normalized, package-relative path supplied by the resource service, not a native absolute path.
  /// @return An independent cursor at byte zero, or std::nullopt for a missing payload.
  /// A present zero-length payload must return a valid stream that reports EOF, not std::nullopt.
  /// Opening may run on a worker; retain no borrowed path data after returning.
  /// The stream must retain its source independently of this capability. Other operational open failures throw.
  [[nodiscard]] virtual std::optional<InputStream> OpenRead(std::string_view package_path) = 0;
};

/// Resolves a packaged raw-resource entry during composition without reading its payload.
/// @param resource Generated raw key in the installed package, such as app::raw::config_json.
/// @return A lazy asset retaining the selected identity and metadata; repeated lookup reuses the lightweight value.
/// @throws std::logic_error If composition has no resource service or the key is missing.
/// Complete reads may block and should not be performed during composition for large payloads.
/// @code{.cpp}
/// // Inside a composable function, with the corresponding raw entry installed:
/// RawAsset config = UseRawResource(app::raw::config_json);
/// // Capture config into an event/task and choose ReadString(true) or OpenReadAsync() there.
/// @endcode
RawAsset UseRawResource(RawResource resource);
/// Resolves a packaged raster image using the effective Locale and current display density during composition.
/// @param resource Generated image key whose selected payload is raster data, such as app::images::logo.
/// @return A cached image; first resolution may synchronously read the selected payload and parse its metadata.
/// @throws std::logic_error If the resource service/key is missing or package metadata is inconsistent.
/// @throws std::invalid_argument If the selected payload is a vector or its raster metadata is invalid.
/// Platform-open/read failures may also propagate. Pass the key directly to Image when explicit resolution is unnecessary.
/// @code{.cpp}
/// // During composition, with a raster image in the generated resource namespace:
/// ImageAsset logo = UseImage(app::images::logo);
/// @endcode
ImageAsset UseImage(ImageResource resource);
/// Resolves a packaged vector image using the effective resource environment during composition.
/// @param resource Generated image key whose selected payload is compiled vector data, such as app::images::mark.
/// @return A cached vector asset, with first-resolution package I/O performed synchronously.
/// @throws std::logic_error If the resource service/key is missing or package metadata is inconsistent.
/// @throws std::invalid_argument If the selected payload is raster data or its vector data is invalid.
/// Platform-open/read failures may also propagate; this helper does not parse arbitrary SVG source text.
/// @code{.cpp}
/// // During composition, with a vector image in the generated resource namespace:
/// VectorAsset mark = UseVectorImage(app::images::mark);
/// @endcode
VectorAsset UseVectorImage(ImageResource resource);

namespace detail {

template <class Value> std::string FormatResourceArgument(Value&& value) {
  std::ostringstream stream;
  stream.imbue(std::locale::classic());
  stream << std::forward<Value>(value);
  return stream.str();
}

template <class... Arguments> std::vector<std::string> FormatResourceArguments(Arguments&&... arguments) {
  return {
      FormatResourceArgument(std::forward<Arguments>(arguments))...,
  };
}

} // namespace detail

/// Resolves and formats a packaged string immediately in the current composition's effective Locale.
/// @tparam Arguments Types insertable into std::ostringstream using the classic locale.
/// @param resource Generated string key in the installed package, such as app::strings::title.
/// @param arguments Values converted to owned strings in zero-based placeholder order.
/// @return Owned localized text; pass no arguments for a plain, unformatted string resource.
/// @throws std::logic_error If a composition resource service or installed key is unavailable.
/// @throws std::invalid_argument If the argument count does not match the installed template.
/// @code{.cpp}
/// // During composition, using keys from the generated resource namespace:
/// std::string title = UseString(app::strings::title);
/// std::string label = UseString(app::strings::file_count, 3); // Template: "{0} files".
/// @endcode
template <class... Arguments> std::string UseString(StringResource resource, Arguments&&... arguments) {
  return UseString(StringVariant::Format(std::move(resource), std::forward<Arguments>(arguments)...));
}

template <class... Arguments> StringVariant StringVariant::Format(StringResource resource, Arguments&&... arguments) {
  return StringVariant(std::move(resource), detail::FormatResourceArguments(std::forward<Arguments>(arguments)...));
}

} // namespace huxerui
