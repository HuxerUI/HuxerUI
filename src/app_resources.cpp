#include "resource_internal.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <huxerui/environment.h>
#include <huxerui/root.h>

namespace huxerui::detail {

namespace {

std::vector<std::string> LocaleFallbacks(const Locale& locale) {
  std::vector<std::string> result;
  std::string current(locale.LanguageTag());
  while (!current.empty()) {
    result.push_back(current);
    const std::size_t separator = current.rfind('-');
    if (separator == std::string::npos) {
      break;
    }
    current.erase(separator);
  }
  result.emplace_back();
  return result;
}

std::string MissingResourceMessage(const ResourceId& id) {
  return "HuxerUI resource is missing from the installed package: " + id.ToString();
}

std::uint64_t ContentHash(std::span<const std::byte> bytes) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
  for (std::byte byte : bytes) {
    hash ^= std::to_integer<std::uint8_t>(byte);
    hash *= 1099511628211ULL;
  }
  return hash;
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
  configuration_ = std::move(configuration);
}

ResourceConfiguration AppResources::Configuration() const noexcept {
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
  if (ContentHash(asset.Bytes()) != entry.content_hash) {
    throw std::logic_error("HuxerUI packaged resource payload does not match its index: " + entry.package_path);
  }
  asset = ResourceAccess::WithMimeType(std::move(asset), entry.mime_type);
  raw_cache_.emplace(entry.package_path, asset);
  return asset;
}

ImageAsset AppResources::Resolve(ImageResource resource, const Locale& locale) {
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
  ImageAsset asset = ResourceAccess::ImageFromRaw(ReadEntry(entry), entry.scale);
  if (asset.PixelWidth() != entry.pixel_width || asset.PixelHeight() != entry.pixel_height) {
    throw std::logic_error("HuxerUI image metadata does not match the installed payload: " + entry.id.ToString());
  }
  image_cache_.emplace(cache_key, asset);
  return asset;
}

ResolvedStringResource AppResources::Resolve(StringResource resource, const Locale& locale) const {
  const ResourceIndexEntry& entry = ResolveLocalized(resource, ResourceEntryKind::String, locale);
  return {entry.value, entry.argument_count};
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

namespace detail {

std::string UseStringArguments(StringResource resource, std::span<const std::string> arguments) {
  const ResolvedStringResource resolved = CurrentResources()->Resolve(resource, UseEnvironment<Locale>());
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

} // namespace detail

} // namespace huxerui
