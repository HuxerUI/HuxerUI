#include "resource_internal.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string_view>

#include <huxerui/environment.h>
#include <huxerui/root.h>

#include "resource_format.h"

namespace huxerui::detail {

namespace {

bool IsAsciiAlpha(char value) {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

bool IsAsciiDigit(char value) {
  return value >= '0' && value <= '9';
}

bool IsScriptSubtag(std::string_view value) {
  return value.size() == 4 && std::ranges::all_of(value, IsAsciiAlpha);
}

bool IsRegionSubtag(std::string_view value) {
  return (value.size() == 2 && std::ranges::all_of(value, IsAsciiAlpha)) ||
         (value.size() == 3 && std::ranges::all_of(value, IsAsciiDigit));
}

std::string JoinLocalePrefix(const std::vector<std::string_view>& subtags, std::size_t count) {
  std::string result;
  for (std::size_t index = 0; index < count; ++index) {
    if (!result.empty()) {
      result.push_back('-');
    }
    result.append(subtags[index]);
  }
  return result;
}

void AppendLocaleFallback(std::vector<std::string>& fallbacks, std::string value) {
  if (std::ranges::find(fallbacks, value) == fallbacks.end()) {
    fallbacks.push_back(std::move(value));
  }
}

std::vector<std::string> LocaleFallbacks(const Locale& locale) {
  const std::string_view language_tag = locale.LanguageTag();
  std::vector<std::string_view> subtags;
  std::size_t start = 0;
  while (start < language_tag.size()) {
    const std::size_t end = language_tag.find('-', start);
    subtags.push_back(language_tag.substr(start, end == std::string_view::npos ? end : end - start));
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }

  constexpr std::size_t no_subtag = std::string_view::npos;
  std::size_t script_index = no_subtag;
  std::size_t region_index = no_subtag;
  std::size_t core_count = 1;
  if (core_count < subtags.size() && IsScriptSubtag(subtags[core_count])) {
    script_index = core_count++;
  }
  if (core_count < subtags.size() && IsRegionSubtag(subtags[core_count])) {
    region_index = core_count++;
  }

  std::vector<std::string> result;
  for (std::size_t count = subtags.size(); count >= core_count; --count) {
    AppendLocaleFallback(result, JoinLocalePrefix(subtags, count));
    if (count == core_count) {
      break;
    }
  }
  if (region_index != no_subtag) {
    AppendLocaleFallback(result, std::string(subtags.front()) + '-' + std::string(subtags[region_index]));
  }
  if (script_index != no_subtag) {
    AppendLocaleFallback(result, std::string(subtags.front()) + '-' + std::string(subtags[script_index]));
  }
  AppendLocaleFallback(result, std::string(subtags.front()));
  AppendLocaleFallback(result, {});
  return result;
}

std::string MissingResourceMessage(const ResourceId& id) {
  return "HuxerUI resource is missing from the installed package: " + id.ToString();
}

} // namespace

AppResources::AppResources(PlatformResources* platform_resources) : platform_resources_(platform_resources) {
  if (platform_resources_ == nullptr) {
    return;
  }
  configuration_ = platform_resources_->Configuration();
  if (!std::isfinite(configuration_.display_scale) || configuration_.display_scale <= 0.0F) {
    throw std::logic_error("HuxerUI platform resource display scale must be finite and positive");
  }
  entries_ = ParseResourceIndex(platform_resources_->Read(resource_index_path));
}

void AppResources::UpdateConfiguration(ResourceConfiguration configuration) {
  if (!std::isfinite(configuration.display_scale) || configuration.display_scale <= 0.0F) {
    throw std::logic_error("HuxerUI platform resource display scale must be finite and positive");
  }
  if (configuration_ == configuration) {
    return;
  }
  BeginDependencyChange(configuration_dependency_);
  configuration_ = std::move(configuration);
  CommitDependencyChange(configuration_dependency_);
}

ResourceConfiguration AppResources::Configuration() const {
  ObserveDependency(configuration_dependency_);
  return configuration_;
}

RawAsset AppResources::Resolve(RawResource resource) {
  const auto found = std::ranges::find_if(entries_, [&resource](const ResourceIndexEntry& entry) {
    return entry.kind == ResourceEntryKind::Raw && entry.id == resource;
  });
  if (found == entries_.end()) {
    throw std::logic_error(MissingResourceMessage(resource));
  }
  return ReadEntry(*found);
}

const ResourceIndexEntry&
AppResources::ResolveLocalized(const ResourceId& id, ResourceEntryKind kind, const Locale& locale) const {
  const std::vector<std::string> fallbacks = LocaleFallbacks(locale);
  for (const std::string& fallback : fallbacks) {
    const auto found = std::ranges::find_if(entries_, [&id, kind, &fallback](const ResourceIndexEntry& entry) {
      return entry.kind == kind && entry.id == id && entry.locale == fallback;
    });
    if (found != entries_.end()) {
      return *found;
    }
  }
  throw std::logic_error(MissingResourceMessage(id));
}

RawAsset AppResources::ReadEntry(const ResourceIndexEntry& entry) {
  if (platform_resources_ == nullptr) {
    throw std::logic_error("HuxerUI packaged resources require PlatformResources");
  }
  const auto cached = raw_cache_.find(entry.package_path);
  if (cached != raw_cache_.end()) {
    return cached->second;
  }
  RawAsset asset = platform_resources_->Read(entry.package_path);
  if (!asset.HasValue()) {
    throw std::logic_error("HuxerUI packaged resource payload is missing: " + entry.package_path);
  }
  if (resource_format::ContentHash(asset.Bytes()) != entry.content_hash) {
    throw std::logic_error("HuxerUI packaged resource payload does not match its index: " + entry.package_path);
  }
  asset = ResourceAccess::WithMimeType(std::move(asset), entry.mime_type);
  raw_cache_.emplace(entry.package_path, asset);
  return asset;
}

ResolvedImageAsset AppResources::ResolveImage(ImageResource resource, const Locale& locale) {
  ObserveDependency(configuration_dependency_);
  const std::vector<std::string> fallbacks = LocaleFallbacks(locale);
  std::vector<const ResourceIndexEntry*> candidates;
  for (const std::string& fallback : fallbacks) {
    for (const ResourceIndexEntry& entry : entries_) {
      if (entry.kind == ResourceEntryKind::Image && entry.id == resource && entry.locale == fallback) {
        candidates.push_back(&entry);
      }
    }
    if (!candidates.empty()) {
      break;
    }
  }
  if (candidates.empty()) {
    throw std::logic_error(MissingResourceMessage(resource));
  }
  std::ranges::sort(candidates, {}, [](const ResourceIndexEntry* entry) { return entry->scale; });
  const auto selected = std::ranges::find_if(candidates, [this](const ResourceIndexEntry* entry) {
    return entry->scale >= configuration_.display_scale;
  });
  const ResourceIndexEntry& entry = **(selected == candidates.end() ? std::prev(candidates.end()) : selected);
  const std::string cache_key = entry.package_path + '@' + std::to_string(entry.content_hash);
  const auto cached = image_cache_.find(cache_key);
  if (cached != image_cache_.end()) {
    return cached->second;
  }
  RawAsset raw = ReadEntry(entry);
  if (ResourceAccess::IsVectorPayload(raw)) {
    VectorAsset asset = ResourceAccess::VectorFromRaw(std::move(raw));
    if (asset.IntrinsicSize() != entry.intrinsic_size) {
      throw std::logic_error("HuxerUI vector metadata does not match the installed payload: " + entry.id.ToString());
    }
    image_cache_.emplace(cache_key, asset);
    return asset;
  }
  ImageAsset asset = ResourceAccess::ImageFromRaw(std::move(raw), entry.scale);
  if (asset.PixelWidth() != entry.pixel_width || asset.PixelHeight() != entry.pixel_height) {
    throw std::logic_error("HuxerUI image metadata does not match the installed payload: " + entry.id.ToString());
  }
  image_cache_.emplace(cache_key, asset);
  return asset;
}

ImageAsset AppResources::Resolve(ImageResource resource, const Locale& locale) {
  ResolvedImageAsset asset = ResolveImage(std::move(resource), locale);
  if (const auto* image = std::get_if<ImageAsset>(&asset)) {
    return *image;
  }
  throw std::invalid_argument("HuxerUI UseImage requires a raster image resource");
}

VectorAsset AppResources::ResolveVector(ImageResource resource, const Locale& locale) {
  ResolvedImageAsset asset = ResolveImage(std::move(resource), locale);
  if (const auto* image = std::get_if<VectorAsset>(&asset)) {
    return *image;
  }
  throw std::invalid_argument("HuxerUI UseVectorImage requires a vector image resource");
}

ResolvedStringResource AppResources::Resolve(const StringResource& resource, const Locale& locale) const {
  const ResourceIndexEntry& entry = ResolveLocalized(resource, ResourceEntryKind::String, locale);
  return {entry.value, entry.argument_count};
}

std::shared_ptr<AppResources> RequireAppResources(std::shared_ptr<const Environment> environment) {
  const std::any* value = FindEnvironmentValue(std::move(environment), typeid(AppResources));
  const auto* resources = value ? std::any_cast<std::shared_ptr<AppResources>>(value) : nullptr;
  if (resources == nullptr || !*resources) {
    throw std::logic_error("HuxerUI mounted resource service is not available");
  }
  return *resources;
}

Locale ResolveResourceLocale(std::shared_ptr<const Environment> environment, const AppResources& resources) {
  if (const std::any* value = FindEnvironmentValue(std::move(environment), typeid(Locale))) {
    if (const auto* locale = std::any_cast<Locale>(value)) {
      return *locale;
    }
    throw std::logic_error("HuxerUI Locale environment value has an invalid type");
  }
  return resources.Configuration().locale;
}

} // namespace huxerui::detail

namespace huxerui {

namespace {

std::shared_ptr<detail::AppResources> CurrentResources() {
  try {
    return UseService<detail::AppResources>();
  } catch (const std::logic_error&) {
    throw std::logic_error("HuxerUI resource lookup requires an active Runtime resource service");
  }
}

} // namespace

RawAsset UseRawResource(RawResource resource) {
  return CurrentResources()->Resolve(std::move(resource));
}

ImageAsset UseImage(ImageResource resource) {
  return CurrentResources()->Resolve(std::move(resource), UseEnvironment<Locale>());
}

VectorAsset UseVectorImage(ImageResource resource) {
  return CurrentResources()->ResolveVector(std::move(resource), UseEnvironment<Locale>());
}

namespace detail {

namespace {

std::string FormatResolvedString(
    const StringResource& resource, const ResolvedStringResource& resolved, std::span<const std::string> arguments
) {
  if (arguments.size() != resolved.argument_count) {
    throw std::invalid_argument(
        "HuxerUI localized string requires exactly " + std::to_string(resolved.argument_count) + " arguments for " +
        resource.ToString()
    );
  }
  const std::string& format = resolved.value;
  std::string result;
  result.reserve(format.size());
  for (std::size_t index = 0; index < format.size();) {
    if (format[index] == '{' && index + 1 < format.size() && format[index + 1] == '{') {
      result.push_back('{');
      index += 2;
      continue;
    }
    if (format[index] == '}' && index + 1 < format.size() && format[index + 1] == '}') {
      result.push_back('}');
      index += 2;
      continue;
    }
    if (format[index] != '{') {
      result.push_back(format[index++]);
      continue;
    }
    const std::size_t end = format.find('}', index + 1);
    if (end == std::string::npos || end == index + 1) {
      throw std::logic_error("HuxerUI localized string template is invalid: " + resource.ToString());
    }
    std::size_t argument_index = 0;
    for (std::size_t digit = index + 1; digit < end; ++digit) {
      if (format[digit] < '0' || format[digit] > '9') {
        throw std::logic_error("HuxerUI localized string template is invalid: " + resource.ToString());
      }
      argument_index = argument_index * 10 + static_cast<std::size_t>(format[digit] - '0');
    }
    if (argument_index >= arguments.size()) {
      throw std::invalid_argument(
          "HuxerUI localized string argument is missing for " + resource.ToString() + " at index " +
          std::to_string(argument_index)
      );
    }
    result += arguments[argument_index];
    index = end + 1;
  }
  return result;
}

} // namespace

std::string ResolveString(const StringVariant& value, AppResources& resources, const Locale& locale) {
  const auto& source = ResourceAccess::StringValue(value);
  if (const auto* literal = std::get_if<std::string>(&source)) {
    return *literal;
  }
  const StringResource& resource = std::get<StringResource>(source);
  return FormatResolvedString(resource, resources.Resolve(resource, locale), ResourceAccess::StringArguments(value));
}

std::string ResolveString(StringVariant&& value, AppResources& resources, const Locale& locale) {
  auto& source = ResourceAccess::StringValue(value);
  if (auto* literal = std::get_if<std::string>(&source)) {
    return std::move(*literal);
  }
  return ResolveString(value, resources, locale);
}

const std::string& StringLiteral(const StringVariant& value) {
  if (const auto* literal = std::get_if<std::string>(&ResourceAccess::StringValue(value))) {
    return *literal;
  }
  throw std::logic_error("HuxerUI unresolved StringVariant reached mounted state");
}

std::string StringLiteral(StringVariant&& value) {
  if (auto* literal = std::get_if<std::string>(&ResourceAccess::StringValue(value))) {
    return std::move(*literal);
  }
  throw std::logic_error("HuxerUI unresolved StringVariant reached mounted state");
}

ResolvedImageAsset ResolveImage(const ImageVariant& image, AppResources& resources, const Locale& locale) {
  ValidateImageVariant(image);
  return std::visit(
      [&resources, &locale](const auto& value) -> ResolvedImageAsset {
        using Image = std::decay_t<decltype(value)>;
        if constexpr (std::same_as<Image, ImageResource>) {
          return resources.ResolveImage(value, locale);
        } else {
          return value;
        }
      },
      image
  );
}

ResolvedImageAsset UseImageVariant(const ImageVariant& image) {
  if (!NeedsResourceResolution(image)) {
    ValidateImageVariant(image);
    if (const auto* raster = std::get_if<ImageAsset>(&image)) {
      return *raster;
    }
    return std::get<VectorAsset>(image);
  }
  std::shared_ptr<AppResources> resources = CurrentResources();
  return ResolveImage(image, *resources, UseEnvironment<Locale>());
}

} // namespace detail

std::string UseString(const StringVariant& value) {
  if (!detail::NeedsResourceResolution(value)) {
    return detail::StringLiteral(value);
  }
  std::shared_ptr<detail::AppResources> resources = CurrentResources();
  return detail::ResolveString(value, *resources, UseEnvironment<Locale>());
}

std::string UseString(StringVariant&& value) {
  if (!detail::NeedsResourceResolution(value)) {
    return detail::StringLiteral(std::move(value));
  }
  std::shared_ptr<detail::AppResources> resources = CurrentResources();
  return detail::ResolveString(std::move(value), *resources, UseEnvironment<Locale>());
}

} // namespace huxerui
