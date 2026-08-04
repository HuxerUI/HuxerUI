#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <huxerui/resource.h>

namespace huxerui::detail {

inline constexpr std::string_view resource_index_path = "huxerui/resources.bin";

enum class ResourceEntryKind : std::uint8_t {
  Raw = 1,
  Image = 2,
  String = 3,
};

// ResourceEntryKind selects the active payload: raw and image entries use package_path and content_hash, image
// entries additionally use scale and pixel dimensions, and string entries use locale, value, and argument_count.
struct ResourceIndexEntry {
  ResourceEntryKind kind = ResourceEntryKind::Raw;
  ResourceId id{"app", "invalid"};
  std::string package_path;
  std::string mime_type;
  std::string locale;
  std::string value;
  float scale = 1.0F;
  std::uint32_t pixel_width = 0;
  std::uint32_t pixel_height = 0;
  std::uint64_t content_hash = 0;
  std::uint32_t argument_count = 0;
};

struct ResolvedStringResource {
  std::string value;
  std::uint32_t argument_count = 0;
};

std::vector<ResourceIndexEntry> ParseResourceIndex(RawAsset index);
bool IsValidResourcePackagePath(std::string_view path) noexcept;

class ResourceAccess {
public:
  static RawAsset WithMimeType(RawAsset asset, std::string mime_type);
  static ImageAsset ImageFromRaw(RawAsset asset, float scale);
  static std::uint64_t ImageIdentity(const ImageAsset& image) noexcept;
};

class AppResources {
public:
  explicit AppResources(PlatformResources* platform_resources);

  void UpdateConfiguration(ResourceConfiguration configuration);
  [[nodiscard]] ResourceConfiguration Configuration() const noexcept;
  [[nodiscard]] RawAsset Resolve(RawResource resource);
  [[nodiscard]] ImageAsset Resolve(ImageResource resource, const Locale& locale);
  [[nodiscard]] ResolvedStringResource Resolve(const StringResource& resource, const Locale& locale) const;

private:
  [[nodiscard]] const ResourceIndexEntry&
  ResolveLocalized(const ResourceId& id, ResourceEntryKind kind, const Locale& locale) const;
  [[nodiscard]] RawAsset ReadEntry(const ResourceIndexEntry& entry);

  PlatformResources* platform_resources_ = nullptr;
  ResourceConfiguration configuration_;
  std::vector<ResourceIndexEntry> entries_;
  std::unordered_map<std::string, RawAsset> raw_cache_;
  std::unordered_map<std::string, ImageAsset> image_cache_;
};

} // namespace huxerui::detail
