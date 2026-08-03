#include "runtime_test_support.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "image_test_support.h"
#include "resource_internal.h"

namespace huxerui::test {

namespace {

struct IndexEntry {
  detail::ResourceEntryKind kind;
  std::string key;
  std::string path;
  std::string mime_type;
  std::string locale;
  std::string value;
  float scale = 1.0F;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint64_t content_hash = 0;
  std::uint32_t argument_count = 0;
};

void AppendU32(std::vector<std::byte>& bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void AppendU64(std::vector<std::byte>& bytes, std::uint64_t value) {
  AppendU32(bytes, static_cast<std::uint32_t>(value));
  AppendU32(bytes, static_cast<std::uint32_t>(value >> 32U));
}

void AppendString(std::vector<std::byte>& bytes, std::string_view value) {
  AppendU32(bytes, static_cast<std::uint32_t>(value.size()));
  for (char character : value) {
    bytes.push_back(static_cast<std::byte>(character));
  }
}

RawAsset EncodeIndex(const std::vector<IndexEntry>& entries) {
  std::vector<std::byte> bytes{
      std::byte{'H'},
      std::byte{'U'},
      std::byte{'X'},
      std::byte{'R'},
      std::byte{'E'},
      std::byte{'S'},
      std::byte{0},
      std::byte{0},
  };
  AppendU32(bytes, 1);
  AppendU32(bytes, static_cast<std::uint32_t>(entries.size()));
  for (const IndexEntry& entry : entries) {
    bytes.push_back(static_cast<std::byte>(entry.kind));
    AppendString(bytes, "test");
    AppendString(bytes, entry.key);
    AppendString(bytes, entry.path);
    AppendString(bytes, entry.mime_type);
    AppendString(bytes, entry.locale);
    AppendString(bytes, entry.value);
    AppendU32(bytes, std::bit_cast<std::uint32_t>(entry.scale));
    AppendU32(bytes, entry.width);
    AppendU32(bytes, entry.height);
    AppendU64(bytes, entry.content_hash);
    AppendU32(bytes, entry.kind == detail::ResourceEntryKind::String ? entry.argument_count : 0);
  }
  return RawAsset::FromBytes(std::move(bytes));
}

RawAsset TestPng(std::uint32_t width, std::uint32_t height) {
  return RawAsset::FromBytes(MakeTestPng(width, height), "image/png");
}

std::uint64_t Hash(std::span<const std::byte> bytes) {
  std::uint64_t result = 14695981039346656037ULL;
  for (std::byte byte : bytes) {
    result ^= std::to_integer<std::uint8_t>(byte);
    result *= 1099511628211ULL;
  }
  return result;
}

class TestResources final : public PlatformResources {
public:
  ResourceConfiguration Configuration() const override {
    return configuration;
  }

  RawAsset Read(std::string_view package_path) override {
    const auto found = assets.find(std::string(package_path));
    return found == assets.end() ? RawAsset{} : found->second;
  }

  ResourceConfiguration configuration{Locale::FromLanguageTag("zh-Hans-CN"), 1.5F};
  std::unordered_map<std::string, RawAsset> assets;
};

std::optional<MenuHandle> resource_menu;

View ResourceMenuApp() {
  auto menu = UseMenu();
  resource_menu = menu;
  return Button("resource menu").With(huxerui::Frame{120.0F, 36.0F}, menu.Anchor());
}

View LocalizedResourceApp() {
  return Text::Format(StringResource("test", "strings/greeting"), "Ada");
}

View DirectLocalizedResourceApp() {
  return Text(StringResource("test", "strings/title"), TextRole::Title);
}

View MissingResourceArgumentsApp() {
  return Text(UseString(StringResource("test", "strings/greeting")));
}

View ExtraResourceArgumentsApp() {
  return Text(UseString(StringResource("test", "strings/greeting"), "Ada", "extra"));
}

} // namespace

TEST_CASE("AppResourcesResolveLocaleScaleAndRawPayloads") {
  TestResources resources;
  const RawAsset config = RawAsset::CopyBytes(std::as_bytes(std::span("enabled", std::size("enabled") - 1)));
  const RawAsset logo = TestPng(20, 10);
  const RawAsset logo_2x = TestPng(40, 20);
  resources.assets.emplace(
      detail::resource_index_path,
      EncodeIndex({
          {detail::ResourceEntryKind::Raw,
           "raw/config.txt",
           "huxerui/test/raw/config.txt",
           "text/plain",
           {},
           {},
           1.0F,
           0,
           0,
           Hash(config.Bytes())},
          {detail::ResourceEntryKind::Image,
           "images/logo",
           "huxerui/test/images/logo.png",
           "image/png",
           {},
           {},
           1.0F,
           20,
           10,
           Hash(logo.Bytes())},
          {detail::ResourceEntryKind::Image,
           "images/logo",
           "huxerui/test/images/logo@2x.png",
           "image/png",
           {},
           {},
           2.0F,
           40,
           20,
           Hash(logo_2x.Bytes())},
      })
  );
  resources.assets.emplace("huxerui/test/raw/config.txt", config);
  resources.assets.emplace("huxerui/test/images/logo.png", logo);
  resources.assets.emplace("huxerui/test/images/logo@2x.png", logo_2x);

  detail::AppResources service(&resources);
  const RawAsset resolved_config = service.Resolve(RawResource("test", "raw/config.txt"));
  REQUIRE(resolved_config.Bytes().size() == 7);
  REQUIRE(resolved_config.MimeType() == "text/plain");
  const ImageAsset image = service.Resolve(ImageResource("test", "images/logo"), Locale::Default());
  REQUIRE(image.Scale() == 2.0F);
  REQUIRE(image.IntrinsicSize() == Size{20.0F, 10.0F});
}

TEST_CASE("RuntimeRefreshesLocalizedResourcesWhenPlatformContextChanges") {
  TestResources resources;
  resources.assets.emplace(
      detail::resource_index_path,
      EncodeIndex({
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/greeting",
              .mime_type = "text/plain",
              .value = "Hello {0}",
              .argument_count = 1,
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/greeting",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "你好，{0}",
              .argument_count = 1,
          },
      })
  );
  TestPlatform platform;
  platform.platform_resources = &resources;
  Runtime runtime{LocalizedResourceApp, platform};
  runtime.SetViewport({200.0F, 60.0F});

  REQUIRE(FirstText(runtime.BuildFrame()) == "你好，Ada");
  resources.configuration.locale = Locale::FromLanguageTag("en-US");
  const int requests_before_update = platform.requested_frames;
  runtime.UpdateResourceConfiguration(resources.configuration);
  REQUIRE(platform.requested_frames == requests_before_update + 1);
  runtime.UpdateResourceConfiguration(resources.configuration);
  REQUIRE(platform.requested_frames == requests_before_update + 1);
  REQUIRE(FirstText(runtime.BuildFrame()) == "Hello Ada");
}

TEST_CASE("TextResolvesStringResourcesDirectly") {
  TestResources resources;
  resources.assets.emplace(
      detail::resource_index_path,
      EncodeIndex({
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/title",
              .mime_type = "text/plain",
              .value = "Title",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/title",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "标题",
          },
      })
  );
  TestPlatform platform;
  platform.platform_resources = &resources;
  Runtime runtime{DirectLocalizedResourceApp, platform};
  runtime.SetViewport({200.0F, 60.0F});

  REQUIRE(FirstText(runtime.BuildFrame()) == "标题");
}

TEST_CASE("MenuItemsResolveStringAndImageResources") {
  resource_menu.reset();
  TestResources resources;
  const RawAsset icon = TestPng(16, 16);
  resources.assets.emplace(
      detail::resource_index_path,
      EncodeIndex({
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/menu_item",
              .mime_type = "text/plain",
              .value = "Resource item",
          },
          {
              .kind = detail::ResourceEntryKind::Image,
              .key = "images/menu_icon",
              .path = "huxerui/test/images/menu_icon.png",
              .mime_type = "image/png",
              .width = 16,
              .height = 16,
              .content_hash = Hash(icon.Bytes()),
          },
      })
  );
  resources.assets.emplace("huxerui/test/images/menu_icon.png", icon);

  TestPlatform platform;
  platform.platform_resources = &resources;
  Runtime runtime{ResourceMenuApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();
  REQUIRE(resource_menu.has_value());

  resource_menu->Show({
      MenuItem(
          ImageResource("test", "images/menu_icon"),
          StringResource("test", "strings/menu_item"),
          [] {}
      ),
  });
  const FlattenedScene& shown = runtime.BuildFrame();
  REQUIRE(ContainsText(shown, "Resource item"));
  REQUIRE(std::ranges::any_of(shown.Commands(), [](const PaintCommand& command) {
    return std::holds_alternative<huxerui::DrawImageCommand>(command);
  }));
}

TEST_CASE("LocalizedResourcesRequireTheDefaultArgumentSchema") {
  TestResources resources;
  resources.assets.emplace(
      detail::resource_index_path,
      EncodeIndex({
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/greeting",
              .mime_type = "text/plain",
              .value = "Hello {0}",
              .argument_count = 1,
          },
      })
  );
  TestPlatform platform;
  platform.platform_resources = &resources;

  Runtime missing{MissingResourceArgumentsApp, platform};
  missing.SetViewport({200.0F, 60.0F});
  REQUIRE_THROWS_AS(missing.BuildFrame(), std::invalid_argument);

  Runtime extra{ExtraResourceArgumentsApp, platform};
  extra.SetViewport({200.0F, 60.0F});
  REQUIRE_THROWS_AS(extra.BuildFrame(), std::invalid_argument);
}

TEST_CASE("AppResourcesRejectPayloadsThatDoNotMatchTheIndex") {
  TestResources resources;
  resources.assets.emplace(
      detail::resource_index_path,
      EncodeIndex({
          {detail::ResourceEntryKind::Raw,
           "raw/config.txt",
           "huxerui/test/raw/config.txt",
           "text/plain",
           {},
           {},
           1.0F,
           0,
           0,
           1},
      })
  );
  resources.assets.emplace(
      "huxerui/test/raw/config.txt",
      RawAsset::CopyBytes(std::as_bytes(std::span("enabled", std::size("enabled") - 1)), "text/plain")
  );

  detail::AppResources service(&resources);
  REQUIRE_THROWS_AS(service.Resolve(RawResource("test", "raw/config.txt")), std::logic_error);
}

} // namespace huxerui::test
