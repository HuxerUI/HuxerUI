#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <locale>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/geometry.h>
#include <huxerui/vector.h>

namespace huxerui {

class StringVariant;
std::string UseString(const StringVariant& value);
std::string UseString(StringVariant&& value);

namespace detail {
class ResourceAccess;
bool IsEmptyStringVariantLiteral(const StringVariant& value) noexcept;
} // namespace detail

class ResourceId {
public:
  ResourceId(std::string_view domain, std::string_view key);

  [[nodiscard]] std::string_view Domain() const noexcept {
    return domain_;
  }

  [[nodiscard]] std::string_view Key() const noexcept {
    return key_;
  }

  [[nodiscard]] std::string ToString() const;

  bool operator==(const ResourceId&) const = default;

private:
  std::string domain_;
  std::string key_;
};

class ImageResource final : public ResourceId {
public:
  using ResourceId::ResourceId;

  bool operator==(const ImageResource&) const = default;
};

class StringResource final : public ResourceId {
public:
  using ResourceId::ResourceId;

  bool operator==(const StringResource&) const = default;
};

// Retains either direct text or a localized template for APIs that compose the value after the call returns.
class StringVariant {
public:
  StringVariant() = default;
  StringVariant(std::string value);
  StringVariant(std::string_view value);
  StringVariant(const char* value);
  StringVariant(StringResource resource);

  template <class... Arguments> static StringVariant Format(StringResource resource, Arguments&&... arguments);

  bool operator==(const StringVariant&) const = default;

private:
  StringVariant(StringResource resource, std::vector<std::string> arguments);

  std::variant<std::string, StringResource> value_;
  std::vector<std::string> arguments_;

  friend class detail::ResourceAccess;
};

class RawResource final : public ResourceId {
public:
  using ResourceId::ResourceId;

  bool operator==(const RawResource&) const = default;
};

class Locale {
public:
  static Locale FromLanguageTag(std::string language_tag);
  static Locale Default();

  [[nodiscard]] std::string_view LanguageTag() const noexcept {
    return language_tag_;
  }

  bool operator==(const Locale&) const = default;

private:
  explicit Locale(std::string language_tag) : language_tag_(std::move(language_tag)) {}

  std::string language_tag_;
};

struct ResourceConfiguration {
  Locale locale = Locale::Default();
  float display_scale = 1.0F;

  bool operator==(const ResourceConfiguration&) const = default;
};

class RawAsset {
public:
  RawAsset() = default;

  static RawAsset FromBytes(std::vector<std::byte> bytes, std::string mime_type = {});
  static RawAsset CopyBytes(std::span<const std::byte> bytes, std::string mime_type = {});
  static RawAsset FromSharedBytes(
      std::shared_ptr<const void> owner, const std::byte* data, std::size_t size, std::string mime_type = {}
  );

  [[nodiscard]] std::span<const std::byte> Bytes() const noexcept;
  [[nodiscard]] std::string_view AsStringView() const noexcept;
  [[nodiscard]] std::string ToString() const;
  [[nodiscard]] std::string_view MimeType() const noexcept;
  [[nodiscard]] bool HasValue() const noexcept;

  bool operator==(const RawAsset& other) const noexcept;

private:
  struct Data;
  explicit RawAsset(std::shared_ptr<const Data> data) : data_(std::move(data)) {}

  std::shared_ptr<const Data> data_;

  friend class ImageAsset;
  friend class detail::ResourceAccess;
};

enum class ImageFormat {
  Png,
  Jpeg,
};

enum class ImageSampling {
  Nearest,
  Linear,
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

private:
  struct Data;
  explicit ImageAsset(std::shared_ptr<const Data> data) : data_(std::move(data)) {}

  static ImageAsset FromRawAsset(RawAsset asset, float scale);

  std::shared_ptr<const Data> data_;

  friend class detail::ResourceAccess;
};

// ExternalTexture remains separate because it is a live platform surface rather than an application image value.
using ImageVariant = std::variant<ImageResource, ImageAsset, VectorAsset>;

class PlatformResources {
public:
  virtual ~PlatformResources() = default;

  [[nodiscard]] virtual ResourceConfiguration Configuration() const = 0;
  // Reads an immutable package-relative payload synchronously. A default RawAsset reports a missing payload.
  [[nodiscard]] virtual RawAsset Read(std::string_view package_path) = 0;
};

RawAsset UseRawResource(RawResource resource);
ImageAsset UseImage(ImageResource resource);
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

template <class... Arguments> std::string UseString(StringResource resource, Arguments&&... arguments) {
  return UseString(StringVariant::Format(std::move(resource), std::forward<Arguments>(arguments)...));
}

template <class... Arguments> StringVariant StringVariant::Format(StringResource resource, Arguments&&... arguments) {
  return StringVariant(std::move(resource), detail::FormatResourceArguments(std::forward<Arguments>(arguments)...));
}

} // namespace huxerui
