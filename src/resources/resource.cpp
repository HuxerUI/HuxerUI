#include <huxerui/resource.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <ranges>
#include <stdexcept>

#include <huxerui/paint.h>
#include <huxerui/file.h>

#include "resource_internal.h"
#include "internal_access.h"
#include "io/stream_internal.h"

namespace huxerui {

StringVariant::StringVariant(std::string value) : value_(std::move(value)) {}

StringVariant::StringVariant(std::string_view value) : value_(std::string(value)) {}

StringVariant::StringVariant(const char* value) : value_(value == nullptr ? std::string{} : std::string(value)) {}

StringVariant::StringVariant(StringResource resource) : value_(std::move(resource)) {}

StringVariant::StringVariant(StringResource resource, std::vector<std::string> arguments)
    : value_(std::move(resource)), arguments_(std::move(arguments)) {}

bool detail::NeedsResourceResolution(const StringVariant& value) noexcept {
  return std::holds_alternative<StringResource>(detail::InternalAccess::StringValue(value));
}

bool detail::NeedsResourceResolution(const ImageVariant& value) noexcept {
  return std::holds_alternative<ImageResource>(value);
}

bool detail::NeedsResourceResolution(const VisualFill& fill) noexcept {
  const auto* image = std::get_if<ImageFill>(&fill.Get());
  return image != nullptr && NeedsResourceResolution(image->source);
}

bool detail::IsEmptyStringVariantLiteral(const StringVariant& value) noexcept {
  const auto* literal = std::get_if<std::string>(&detail::InternalAccess::StringValue(value));
  return literal != nullptr && literal->empty();
}

bool detail::IsBlankStringVariantLiteral(const StringVariant& value) noexcept {
  const auto* literal = std::get_if<std::string>(&detail::InternalAccess::StringValue(value));
  return literal != nullptr && std::ranges::all_of(*literal, [](unsigned char character) {
           return std::isspace(character) != 0;
         });
}

void detail::ValidateImageVariant(const ImageVariant& image) {
  std::visit(
      [](const auto& value) {
        using Image = std::decay_t<decltype(value)>;
        if constexpr (!std::same_as<Image, ImageResource>) {
          if (!value.HasValue()) {
            throw std::invalid_argument("HuxerUI image asset must not be empty");
          }
        }
      },
      image
  );
}

namespace {

bool IsAsciiAlpha(char value) noexcept {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

bool IsAsciiDigit(char value) noexcept {
  return value >= '0' && value <= '9';
}

bool IsAsciiAlphanumeric(char value) noexcept {
  return IsAsciiAlpha(value) || IsAsciiDigit(value);
}

std::uint16_t ReadBigEndian16(const std::byte* bytes) noexcept {
  return static_cast<std::uint16_t>(
      (std::to_integer<std::uint8_t>(bytes[0]) << 8U) | std::to_integer<std::uint8_t>(bytes[1])
  );
}

std::uint32_t ReadBigEndian32(const std::byte* bytes) noexcept {
  return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[0])) << 24U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[1])) << 16U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[2])) << 8U) |
         static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[3]));
}

struct ImageMetadata {
  ImageFormat format = ImageFormat::Png;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::string_view mime_type;
};

bool IsPngChunk(std::span<const std::byte> bytes, std::size_t offset, std::string_view type) noexcept {
  return type.size() == 4 && offset + 8 <= bytes.size() &&
         std::equal(
             type.begin(),
             type.end(),
             bytes.begin() + static_cast<std::ptrdiff_t>(offset + 4),
             [](char left, std::byte right) { return static_cast<std::byte>(left) == right; }
         );
}

ImageMetadata ReadImageMetadata(std::span<const std::byte> bytes) {
  constexpr std::byte png_signature[] = {
      std::byte{0x89},
      std::byte{'P'},
      std::byte{'N'},
      std::byte{'G'},
      std::byte{0x0D},
      std::byte{0x0A},
      std::byte{0x1A},
      std::byte{0x0A},
  };
  if (bytes.size() >= std::size(png_signature) &&
      std::equal(std::begin(png_signature), std::end(png_signature), bytes.begin())) {
    if (bytes.size() < 33 || ReadBigEndian32(bytes.data() + 8) != 13 || !IsPngChunk(bytes, 8, "IHDR") ||
        bytes[26] != std::byte{0} || bytes[27] != std::byte{0} || std::to_integer<std::uint8_t>(bytes[28]) > 1) {
      throw std::invalid_argument("HuxerUI encoded PNG image has an invalid IHDR chunk");
    }
    const std::uint32_t width = ReadBigEndian32(bytes.data() + 16);
    const std::uint32_t height = ReadBigEndian32(bytes.data() + 20);
    if (width == 0 || height == 0) {
      throw std::invalid_argument("HuxerUI PNG image dimensions must be positive");
    }
    bool has_image_data = false;
    std::size_t offset = 8;
    while (offset + 12 <= bytes.size()) {
      const std::uint32_t length = ReadBigEndian32(bytes.data() + offset);
      if (length > bytes.size() - offset - 12) {
        break;
      }
      const std::size_t end = offset + 12 + length;
      if (IsPngChunk(bytes, offset, "IDAT")) {
        has_image_data = true;
      } else if (IsPngChunk(bytes, offset, "IEND")) {
        if (length == 0 && has_image_data && end == bytes.size()) {
          return {ImageFormat::Png, width, height, "image/png"};
        }
        break;
      }
      offset = end;
    }
    throw std::invalid_argument("HuxerUI encoded PNG image must contain complete IDAT and IEND chunks");
  }

  if (bytes.size() >= 4 && bytes[0] == std::byte{0xFF} && bytes[1] == std::byte{0xD8}) {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t offset = 2;
    while (offset + 3 < bytes.size()) {
      while (offset < bytes.size() && bytes[offset] == std::byte{0xFF}) {
        ++offset;
      }
      if (offset >= bytes.size()) {
        break;
      }
      const std::uint8_t marker = std::to_integer<std::uint8_t>(bytes[offset++]);
      if (marker == 0x01 || marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7)) {
        continue;
      }
      if (offset + 2 > bytes.size()) {
        break;
      }
      const std::uint16_t segment_size = ReadBigEndian16(bytes.data() + offset);
      if (segment_size < 2 || offset + segment_size > bytes.size()) {
        break;
      }
      const bool size_of_frame = (marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
                                 (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF);
      if (size_of_frame && segment_size >= 7) {
        height = ReadBigEndian16(bytes.data() + offset + 3);
        width = ReadBigEndian16(bytes.data() + offset + 5);
        if (width == 0 || height == 0) {
          throw std::invalid_argument("HuxerUI JPEG image dimensions must be positive");
        }
      }
      if (marker == 0xDA) {
        if (width != 0 && height != 0 && bytes.size() >= 2 && bytes[bytes.size() - 2] == std::byte{0xFF} &&
            bytes.back() == std::byte{0xD9}) {
          return {ImageFormat::Jpeg, width, height, "image/jpeg"};
        }
        break;
      }
      offset += segment_size;
    }
  }
  throw std::invalid_argument("HuxerUI encoded image must be a supported PNG or JPEG");
}

void ValidateScale(float scale) {
  if (!std::isfinite(scale) || scale <= 0.0F) {
    throw std::invalid_argument("HuxerUI image scale must be finite and positive");
  }
}

std::uint64_t NextImageIdentity() noexcept {
  static std::atomic_uint64_t next_identity{1};
  return next_identity.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

struct RawAsset::Data {
  Data(std::shared_ptr<const void> storage, const std::byte* data, std::size_t length, std::string mime)
      : owner(std::move(storage)), bytes(data), size(length), mime_type(std::move(mime)) {}
  Data(std::weak_ptr<detail::AppResources> source, std::string path, std::string mime)
      : mime_type(std::move(mime)), resources(std::move(source)), package_path(std::move(path)) {}

  std::shared_ptr<const void> owner;
  const std::byte* bytes = nullptr;
  std::size_t size = 0;
  std::string mime_type;
  std::weak_ptr<detail::AppResources> resources;
  std::string package_path;
  mutable std::mutex cache_mutex;
  mutable std::shared_ptr<const huxerui::Bytes> cached;
};

namespace {

class RawAssetInputStreamBackend final : public detail::InputStreamBackend {
public:
  RawAssetInputStreamBackend(std::shared_ptr<const void> owner, std::span<const std::byte> bytes)
      : owner_(std::move(owner)), bytes_(bytes) {}

  IoResult<std::size_t> Read(std::span<std::byte> buffer) override {
    const std::size_t size = std::min(buffer.size(), bytes_.size() - offset_);
    if (size != 0) {
      std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_), size, buffer.begin());
    }
    offset_ += size;
    return IoResult<std::size_t>(size);
  }

  void Release() noexcept override {
    bytes_ = {};
    owner_.reset();
  }

private:
  std::shared_ptr<const void> owner_;
  std::span<const std::byte> bytes_;
  std::size_t offset_ = 0;
};

} // namespace

struct ImageAsset::Data {
  RawAsset encoded;
  ImageFormat format = ImageFormat::Png;
  std::uint32_t pixel_width = 0;
  std::uint32_t pixel_height = 0;
  float scale = 1.0F;
  // Copies share this process-local native decode-cache identity; it is neither value equality nor a ResourceId.
  std::uint64_t identity = 0;
};

ResourceId::ResourceId(std::string_view domain, std::string_view key) : domain_(domain), key_(key) {
  if (domain_.empty() || !std::ranges::all_of(domain_, [](char value) {
        return IsAsciiAlphanumeric(value) || value == '_' || value == '-' || value == '.';
      })) {
    throw std::invalid_argument("HuxerUI resource domain must contain only ASCII letters, digits, '.', '_', or '-'");
  }
  if (!detail::IsValidResourcePackagePath(key_)) {
    throw std::invalid_argument("HuxerUI resource key must be a normalized package-relative path");
  }
}

std::string ResourceId::ToString() const {
  return domain_ + ':' + key_;
}

Locale Locale::FromLanguageTag(std::string language_tag) {
  if (language_tag.empty()) {
    throw std::invalid_argument("HuxerUI locale language tag must not be empty");
  }
  std::replace(language_tag.begin(), language_tag.end(), '_', '-');
  std::size_t start = 0;
  std::size_t subtag_index = 0;
  while (start < language_tag.size()) {
    const std::size_t end = language_tag.find('-', start);
    const std::size_t length = (end == std::string::npos ? language_tag.size() : end) - start;
    if (length == 0 || length > 8 ||
        !std::ranges::all_of(std::string_view(language_tag).substr(start, length), IsAsciiAlphanumeric)) {
      throw std::invalid_argument("HuxerUI locale must be a valid normalized BCP-47 language tag");
    }
    const bool script = subtag_index > 0 && length == 4 &&
                        std::ranges::all_of(std::string_view(language_tag).substr(start, length), IsAsciiAlpha);
    const bool region =
        subtag_index > 0 &&
        ((length == 2 && std::ranges::all_of(std::string_view(language_tag).substr(start, length), IsAsciiAlpha)) ||
         (length == 3 && std::ranges::all_of(std::string_view(language_tag).substr(start, length), IsAsciiDigit)));
    for (std::size_t index = start; index < start + length; ++index) {
      const unsigned char value = static_cast<unsigned char>(language_tag[index]);
      language_tag[index] = static_cast<char>(region ? std::toupper(value) : std::tolower(value));
    }
    if (script) {
      language_tag[start] = static_cast<char>(std::toupper(static_cast<unsigned char>(language_tag[start])));
    }
    ++subtag_index;
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return Locale(std::move(language_tag));
}

Locale Locale::Default() {
  return Locale("en");
}

RawAsset RawAsset::FromBytes(huxerui::Bytes bytes, std::string mime_type) {
  auto storage = std::make_shared<const huxerui::Bytes>(std::move(bytes));
  return FromSharedBytes(storage, storage->data(), storage->size(), std::move(mime_type));
}

RawAsset RawAsset::CopyBytes(std::span<const std::byte> bytes, std::string mime_type) {
  return FromBytes(huxerui::Bytes(bytes.begin(), bytes.end()), std::move(mime_type));
}

RawAsset RawAsset::FromSharedBytes(
    std::shared_ptr<const void> owner, const std::byte* data, std::size_t size, std::string mime_type
) {
  if (size != 0 && (!owner || data == nullptr)) {
    throw std::invalid_argument("HuxerUI shared raw asset bytes require a non-empty owner and data pointer");
  }
  return RawAsset(std::make_shared<const Data>(std::move(owner), data, size, std::move(mime_type)));
}

std::shared_ptr<const huxerui::Bytes> RawAsset::CachedBytes() const {
  if (!data_ || data_->package_path.empty()) {
    return {};
  }
  std::scoped_lock lock(data_->cache_mutex);
  return data_->cached;
}

std::shared_ptr<const huxerui::Bytes> RawAsset::EnsureCachedBytes() const {
  if (auto bytes = CachedBytes()) {
    return bytes;
  }
  auto bytes = std::make_shared<const huxerui::Bytes>(detail::ReadStreamBytes(OpenRead()));
  std::scoped_lock lock(data_->cache_mutex);
  if (!data_->cached) {
    data_->cached = std::move(bytes);
  }
  return data_->cached;
}

huxerui::Bytes RawAsset::ReadBytes(bool cache) const {
  if (!data_) {
    return {};
  }
  if (data_->package_path.empty()) {
    const auto bytes = detail::InternalAccess::RawBytes(*this);
    return {bytes.begin(), bytes.end()};
  }
  if (auto bytes = cache ? EnsureCachedBytes() : CachedBytes()) {
    return *bytes;
  }
  return detail::ReadStreamBytes(OpenRead());
}

InputStream RawAsset::OpenRead() const {
  if (!data_) {
    throw std::logic_error("HuxerUI cannot open a missing raw asset");
  }
  if (data_->package_path.empty()) {
    return detail::StreamAccess::MakeInputStream(
        std::make_shared<RawAssetInputStreamBackend>(data_->owner, detail::InternalAccess::RawBytes(*this)));
  }
  if (auto bytes = CachedBytes()) {
    return detail::StreamAccess::MakeInputStream(std::make_shared<RawAssetInputStreamBackend>(bytes, *bytes));
  }
  const auto resources = data_->resources.lock();
  if (!resources) {
    throw std::logic_error("HuxerUI raw asset resource service has expired");
  }
  return resources->OpenRead(data_->package_path);
}

namespace {

Task<AsyncInputStream> OpenMemoryAsync(RawAsset asset) {
  co_return detail::MakeMemoryAsyncInput(asset.OpenRead());
}

} // namespace

Task<AsyncInputStream> RawAsset::OpenReadAsync() const {
  if (data_ && (data_->package_path.empty() || CachedBytes())) {
    return OpenMemoryAsync(*this);
  }
  return detail::OpenWorkerInput([asset = *this] { return asset.OpenRead(); });
}

std::string RawAsset::ReadString(bool cache) const {
  if (!data_) {
    return {};
  }
  if (data_->package_path.empty()) {
    const auto bytes = detail::InternalAccess::RawBytes(*this);
    return bytes.empty() ? std::string{} : std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  if (auto bytes = cache ? EnsureCachedBytes() : CachedBytes()) {
    return bytes->empty() ? std::string{} : std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  }
  InputStream input = OpenRead();
  std::array<std::byte, 16384> buffer;
  std::string result;
  while (true) {
    auto read = input.Read(buffer);
    if (!read.Succeeded()) {
      throw std::runtime_error(read.Error().message);
    }
    if (read.Value() == 0) {
      break;
    }
    result.append(reinterpret_cast<const char*>(buffer.data()), read.Value());
  }
  return result;
}

std::string_view RawAsset::MimeType() const noexcept {
  return data_ ? std::string_view(data_->mime_type) : std::string_view{};
}

bool RawAsset::HasValue() const noexcept {
  return static_cast<bool>(data_);
}

bool RawAsset::operator==(const RawAsset& other) const noexcept {
  if (data_ == other.data_) {
    return true;
  }
  if (!data_ || !other.data_ || MimeType() != other.MimeType()) {
    return false;
  }
  if (!data_->package_path.empty() || !other.data_->package_path.empty()) {
    return data_->package_path == other.data_->package_path &&
           !data_->resources.owner_before(other.data_->resources) &&
           !other.data_->resources.owner_before(data_->resources);
  }
  return std::ranges::equal(detail::InternalAccess::RawBytes(*this), detail::InternalAccess::RawBytes(other));
}

ImageAsset ImageAsset::FromFile(const std::filesystem::path& path, float scale) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::invalid_argument("HuxerUI image file could not be opened: " + path.string());
  }
  stream.seekg(0, std::ios::end);
  const std::streamoff length = stream.tellg();
  if (length < 0 || static_cast<std::uintmax_t>(length) > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument("HuxerUI image file size is invalid: " + path.string());
  }
  stream.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(length));
  if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), length)) {
    throw std::invalid_argument("HuxerUI image file could not be read: " + path.string());
  }
  return FromEncoded(std::move(bytes), scale);
}

ImageAsset ImageAsset::FromEncoded(Bytes bytes, float scale) {
  return FromRawAsset(RawAsset::FromBytes(std::move(bytes)), scale);
}

ImageAsset ImageAsset::CopyEncoded(std::span<const std::byte> bytes, float scale) {
  return FromRawAsset(RawAsset::CopyBytes(bytes), scale);
}

ImageAsset ImageAsset::FromRawAsset(RawAsset asset, float scale) {
  ValidateScale(scale);
  const ImageMetadata metadata = ReadImageMetadata(detail::InternalAccess::RawBytes(asset));
  if (!std::isfinite(static_cast<float>(metadata.width) / scale) ||
      !std::isfinite(static_cast<float>(metadata.height) / scale)) {
    throw std::invalid_argument("HuxerUI image intrinsic dimensions must be finite");
  }
  if (asset.data_ && asset.data_->mime_type.empty()) {
    auto raw_data = std::make_shared<const RawAsset::Data>(asset.data_->owner, asset.data_->bytes, asset.data_->size,
                                                           std::string(metadata.mime_type));
    asset = RawAsset(std::move(raw_data));
  }
  return ImageAsset(
      std::make_shared<const Data>(Data{
          std::move(asset),
          metadata.format,
          metadata.width,
          metadata.height,
          scale,
          NextImageIdentity(),
      })
  );
}

std::span<const std::byte> ImageAsset::EncodedBytes() const noexcept {
  return data_ ? detail::InternalAccess::RawBytes(data_->encoded) : std::span<const std::byte>{};
}

ImageFormat ImageAsset::Format() const noexcept {
  return data_ ? data_->format : ImageFormat::Png;
}

std::string_view ImageAsset::MimeType() const noexcept {
  return data_ ? data_->encoded.MimeType() : std::string_view{};
}

std::uint32_t ImageAsset::PixelWidth() const noexcept {
  return data_ ? data_->pixel_width : 0;
}

std::uint32_t ImageAsset::PixelHeight() const noexcept {
  return data_ ? data_->pixel_height : 0;
}

float ImageAsset::Scale() const noexcept {
  return data_ ? data_->scale : 1.0F;
}

Size ImageAsset::IntrinsicSize() const noexcept {
  return data_ ? Size{static_cast<float>(data_->pixel_width) / data_->scale,
                      static_cast<float>(data_->pixel_height) / data_->scale}
               : Size{};
}

bool ImageAsset::HasValue() const noexcept {
  return static_cast<bool>(data_);
}

bool ImageAsset::operator==(const ImageAsset& other) const noexcept {
  return data_ == other.data_ ||
         (Format() == other.Format() && PixelWidth() == other.PixelWidth() && PixelHeight() == other.PixelHeight() &&
          Scale() == other.Scale() && EncodedBytes().size() == other.EncodedBytes().size() &&
          std::ranges::equal(EncodedBytes(), other.EncodedBytes()));
}

} // namespace huxerui

namespace huxerui::detail {

Bytes ReadStreamBytes(InputStream input) {
  Bytes result;
  std::array<std::byte, 16384> buffer;
  while (true) {
    auto read = input.Read(buffer);
    if (!read.Succeeded()) {
      throw std::runtime_error(read.Error().message);
    }
    const std::size_t count = read.Value();
    if (count == 0) {
      break;
    }
    result.insert(result.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count));
  }
  return result;
}

std::optional<InputStream> OpenPackageFile(const std::filesystem::path& path) {
  auto opened = File(std::u8string_view(path.u8string())).OpenRead();
  if (opened.Succeeded()) {
    return std::move(opened).Value();
  }
  if (opened.Error().code == IoErrorCode::NotFound) {
    return std::nullopt;
  }
  throw std::runtime_error(opened.Error().message);
}

const std::variant<std::string, StringResource>& InternalAccess::StringValue(const StringVariant& value) noexcept {
  return value.value_;
}

std::variant<std::string, StringResource>& InternalAccess::StringValue(StringVariant& value) noexcept {
  return value.value_;
}

std::span<const std::string> InternalAccess::StringArguments(const StringVariant& value) noexcept {
  return value.arguments_;
}

RawAsset InternalAccess::PackagedRaw(std::weak_ptr<AppResources> resources, std::string path, std::string mime_type) {
  if (resources.expired()) {
    throw std::logic_error("HuxerUI packaged raw assets require a shared resource service");
  }
  return RawAsset(std::make_shared<const RawAsset::Data>(std::move(resources), std::move(path), std::move(mime_type)));
}

std::span<const std::byte> InternalAccess::RawBytes(const RawAsset& asset) noexcept {
  return asset.data_ ? std::span<const std::byte>(asset.data_->bytes, asset.data_->size) : std::span<const std::byte>{};
}

ImageAsset InternalAccess::ImageFromRaw(RawAsset asset, float scale) {
  return ImageAsset::FromRawAsset(std::move(asset), scale);
}

std::uint64_t InternalAccess::ImageIdentity(const ImageAsset& image) noexcept {
  return image.data_ ? image.data_->identity : 0;
}

} // namespace huxerui::detail
