#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <huxerui/file.h>
#include <huxerui/resource.h>
#include <huxerui/state.h>
#include <huxerui/text.h>
#include <huxerui/vector.h>

namespace huxerui {
class Environment;
class VisualFill;
}

namespace huxerui::detail {

inline constexpr std::string_view resource_index_path = "huxerui/resources.bin";

[[nodiscard]] Bytes ReadStreamBytes(InputStream input);
[[nodiscard]] std::optional<InputStream> OpenPackageFile(const File& file);
[[nodiscard]] std::optional<InputStream> OpenPackageFile(const std::filesystem::path& path);

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

class AppResources : public std::enable_shared_from_this<AppResources> {
public:
  explicit AppResources(PlatformResources* platform_resources);
  explicit AppResources(const File& directory);
  void Disconnect();
  [[nodiscard]] InputStream OpenRead(std::string_view package_path);
  bool IsIndependentPackage() const noexcept { return directory_.has_value(); }

  void UpdateConfiguration(ResourceConfiguration configuration);
  [[nodiscard]] ResourceConfiguration Configuration(bool observe = true) const;
  /// Resolves application defaults, original-window overrides, and local Locale, or the independent package defaults.
  /// @param observe Whether configuration dependencies should subscribe the active composition.
  /// @return An owned effective configuration; ordinary Resources Get methods pass false.
  [[nodiscard]] ResourceConfiguration EffectiveConfiguration(bool observe) const;
  [[nodiscard]] RawAsset Resolve(RawResource resource);
  [[nodiscard]] FontFamily ResolveFontFamily(std::vector<RawResource> resources);
  [[nodiscard]] ResolvedImageAsset ResolveImage(ImageResource resource, const Locale& locale,
                                                std::optional<float> display_scale = {});
  [[nodiscard]] ImageAsset Resolve(ImageResource resource, const Locale& locale, std::optional<float> display_scale = {});
  [[nodiscard]] VectorAsset ResolveVector(ImageResource resource, const Locale& locale,
                                          std::optional<float> display_scale = {});
  [[nodiscard]] ResolvedStringResource Resolve(const StringResource& resource, const Locale& locale) const;

private:
  void LoadIndex(Bytes bytes);
  [[nodiscard]] std::unique_lock<std::mutex> LockCache();
  [[nodiscard]] std::span<const ResourceIndexEntry> FindEntries(ResourceEntryKind kind, const ResourceId& id) const;
  [[nodiscard]] std::span<const ResourceIndexEntry>
  ResolveLocalized(const ResourceId& id, ResourceEntryKind kind, const Locale& locale) const;
  [[nodiscard]] ResolvedImageAsset ReadImage(const ResourceIndexEntry& entry);

  const std::optional<File> directory_;
  PlatformResources* platform_resources_ = nullptr;
  // Guards the borrowed platform source or the independent package's raw and image caches.
  std::mutex mutex_;
  ResourceConfiguration configuration_;
  std::shared_ptr<CompositionDependency> configuration_dependency_ = std::make_shared<CompositionDependency>();
  std::vector<ResourceIndexEntry> entries_;
  std::unordered_map<std::string, RawAsset> raw_cache_;
  std::unordered_map<std::string, ResolvedImageAsset> image_cache_;
  std::unordered_map<std::string, FontFamily> font_cache_;
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
ResolvedImageAsset ResolveImage(const ImageVariant& image, AppResources& resources, const Locale& locale,
                                 std::optional<float> display_scale = {});
ResolvedImageAsset UseImageVariant(const ImageVariant& image);
Locale ResolveResourceLocale(std::shared_ptr<const Environment> environment, const AppResources& resources);

} // namespace huxerui::detail
