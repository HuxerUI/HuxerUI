#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <huxerui/resource.h>
#include <huxerui/state.h>
#include <huxerui/vector.h>

namespace huxerui {
class Environment;
class VisualFill;
}

namespace huxerui::detail {

inline constexpr std::string_view resource_index_path = "huxerui/resources.bin";

enum class ResourceEntryKind {
  Raw,
  Image,
  String,
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
  Size intrinsic_size;
};

struct ResolvedStringResource {
  std::string value;
  std::uint32_t argument_count = 0;
};

std::vector<ResourceIndexEntry> ParseResourceIndex(RawAsset index);
bool IsValidResourcePackagePath(std::string_view path) noexcept;

using ResolvedImageAsset = std::variant<ImageAsset, VectorAsset>;

class AppResources {
public:
  explicit AppResources(PlatformResources* platform_resources);

  void UpdateConfiguration(ResourceConfiguration configuration);
  [[nodiscard]] ResourceConfiguration Configuration() const;
  [[nodiscard]] RawAsset Resolve(RawResource resource);
  [[nodiscard]] ResolvedImageAsset ResolveImage(ImageResource resource, const Locale& locale);
  [[nodiscard]] ImageAsset Resolve(ImageResource resource, const Locale& locale);
  [[nodiscard]] VectorAsset ResolveVector(ImageResource resource, const Locale& locale);
  [[nodiscard]] ResolvedStringResource Resolve(const StringResource& resource, const Locale& locale) const;

private:
  [[nodiscard]] const ResourceIndexEntry&
  ResolveLocalized(const ResourceId& id, ResourceEntryKind kind, const Locale& locale) const;
  [[nodiscard]] RawAsset ReadEntry(const ResourceIndexEntry& entry);

  PlatformResources* platform_resources_ = nullptr;
  ResourceConfiguration configuration_;
  std::shared_ptr<CompositionDependency> configuration_dependency_ = std::make_shared<CompositionDependency>();
  std::vector<ResourceIndexEntry> entries_;
  std::unordered_map<std::string, RawAsset> raw_cache_;
  std::unordered_map<std::string, ResolvedImageAsset> image_cache_;
};

std::string ResolveString(const StringVariant& value, AppResources& resources, const Locale& locale);
std::string ResolveString(StringVariant&& value, AppResources& resources, const Locale& locale);
const std::string& StringLiteral(const StringVariant& value);
std::string StringLiteral(StringVariant&& value);
bool NeedsResourceResolution(const StringVariant& value) noexcept;
bool NeedsResourceResolution(const ImageVariant& value) noexcept;
bool NeedsResourceResolution(const VisualFill& fill) noexcept;
bool IsBlankStringVariantLiteral(const StringVariant& value) noexcept;
void ValidateImageVariant(const ImageVariant& image);
ResolvedImageAsset ResolveImage(const ImageVariant& image, AppResources& resources, const Locale& locale);
ResolvedImageAsset UseImageVariant(const ImageVariant& image);
std::shared_ptr<AppResources> RequireAppResources(std::shared_ptr<const Environment> environment);
Locale ResolveResourceLocale(std::shared_ptr<const Environment> environment, const AppResources& resources);

} // namespace huxerui::detail
