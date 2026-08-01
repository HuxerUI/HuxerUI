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
#include <vector>

#include <huxerui/geometry.h>

namespace huxerui {

namespace detail {
class ResourceAccess;
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

struct ResourceContext {
  Locale locale = Locale::Default();
  float display_scale = 1.0F;

  bool operator==(const ResourceContext&) const = default;
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

class PlatformResources {
public:
  virtual ~PlatformResources() = default;

  [[nodiscard]] virtual ResourceContext Context() const = 0;
  // Reads an immutable package-relative payload synchronously. A default RawAsset reports a missing payload.
  [[nodiscard]] virtual RawAsset Read(std::string_view package_path) = 0;
};

RawAsset UseRawResource(RawResource resource);
ImageAsset UseImage(ImageResource resource);

namespace detail {

std::string UseStringArguments(StringResource resource, std::span<const std::string> arguments);

template <class Value> std::string FormatResourceArgument(Value&& value) {
  std::ostringstream stream;
  stream.imbue(std::locale::classic());
  stream << std::forward<Value>(value);
  return stream.str();
}

} // namespace detail

template <class... Arguments> std::string UseString(StringResource resource, Arguments&&... arguments) {
  const std::vector<std::string> formatted{
      detail::FormatResourceArgument(std::forward<Arguments>(arguments))...,
  };
  return detail::UseStringArguments(std::move(resource), formatted);
}

} // namespace huxerui
