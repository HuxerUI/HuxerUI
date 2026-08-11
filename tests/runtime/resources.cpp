#include "runtime_test_support.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
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
  std::string domain = "test";
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
    AppendString(bytes, entry.domain);
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

class TestClipboard final : public PlatformClipboard {
public:
  std::optional<std::string> ReadText() override {
    return text;
  }

  bool WriteText(std::string_view value) override {
    text = std::string(value);
    return true;
  }

  std::optional<std::string> text = "paste";
};

std::optional<MenuHandle> resource_menu;
std::optional<DialogHandle> resource_dialog;

View ResourceMenuApp() {
  auto menu = UseMenu();
  resource_menu = menu;
  return Button("resource menu").With(huxerui::Frame{120.0F, 36.0F}, menu.Anchor());
}

View FrameworkDialogResourceApp() {
  resource_dialog = UseDialog();
  return Spacer();
}

View FrameworkValidationResourceApp() {
  return TextField(TextEditingValue::FromText(""))
      .Validation(Validate("", Required()))
      .With(huxerui::Frame{220.0F, 80.0F});
}

View FrameworkSelectionResourceApp() {
  return TextField(TextEditingValue::FromText("alpha beta")).With(huxerui::Frame{180.0F, 40.0F});
}

View PartialSelectionLabelsResourceApp() {
  return ProvideEnvironment(TextSelectionMenuLabels{.copy = "Duplicate"}, [] {
    return TextField(TextEditingValue::FromText("alpha beta")).With(huxerui::Frame{180.0F, 40.0F});
  });
}

View LocalizedResourceApp() {
  const StringVariant greeting = StringVariant::Format(StringResource("test", "strings/greeting"), "Ada");
  return Text(UseString(greeting));
}

View DirectLocalizedResourceApp() {
  return Column {
    Text(StringResource("test", "strings/title"), TextRole::Title),
    Button(StringResource("test", "strings/action")),
    TextField(TextEditingValue::FromText("")).Placeholder(StringResource("test", "strings/placeholder")),
  };
}

View LocalizedWindowControlsApp() {
  return Spacer();
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

TEST_CASE("AppResourcesPrefersRegionBeforeScriptDuringLocaleFallback") {
  TestResources resources;
  resources.assets.emplace(
      detail::resource_index_path,
      EncodeIndex({
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/greeting",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "Language",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/greeting",
              .mime_type = "text/plain",
              .locale = "zh-Hant",
              .value = "Script",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/greeting",
              .mime_type = "text/plain",
              .locale = "zh-TW",
              .value = "Region",
          },
      })
  );

  detail::AppResources service(&resources);
  const StringResource greeting("test", "strings/greeting");
  REQUIRE(service.Resolve(greeting, Locale::FromLanguageTag("zh-Hant-TW")).value == "Region");
  REQUIRE(service.Resolve(greeting, Locale::FromLanguageTag("zh-Hant")).value == "Script");
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
  runtime.SetWindowMetrics({.viewport = {200.0F, 60.0F}});

  REQUIRE(FirstText(runtime.BuildFrame()) == "你好，Ada");
  resources.configuration.locale = Locale::FromLanguageTag("en-US");
  const int requests_before_update = platform.requested_frames;
  runtime.UpdateResourceConfiguration(resources.configuration);
  REQUIRE(platform.requested_frames == requests_before_update + 1);
  runtime.UpdateResourceConfiguration(resources.configuration);
  REQUIRE(platform.requested_frames == requests_before_update + 1);
  REQUIRE(FirstText(runtime.BuildFrame()) == "Hello Ada");
}

TEST_CASE("DialogDefaultsResolveFrameworkResourcesAndTrackLocale") {
  resource_dialog.reset();
  TestResources resources;
  resources.assets.emplace(
      detail::resource_index_path,
      EncodeIndex({
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/dialog_ok",
              .mime_type = "text/plain",
              .value = "OK",
              .domain = "huxerui",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/dialog_ok",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "确定",
              .domain = "huxerui",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/dialog_cancel",
              .mime_type = "text/plain",
              .value = "Cancel",
              .domain = "huxerui",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/dialog_cancel",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "取消",
              .domain = "huxerui",
          },
      })
  );
  TestPlatform platform;
  platform.platform_resources = &resources;
  Runtime runtime{FrameworkDialogResourceApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  REQUIRE(resource_dialog.has_value());

  resource_dialog->Show("Title", "Message", StringVariant{}, StringVariant{});
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  REQUIRE(ContainsText(runtime.BuildFrame(), "确定"));
  REQUIRE(ContainsText(runtime.BuildFrame(), "取消"));

  resources.configuration.locale = Locale::FromLanguageTag("en-US");
  runtime.UpdateResourceConfiguration(resources.configuration);
  REQUIRE(ContainsText(runtime.BuildFrame(), "OK"));
  REQUIRE(ContainsText(runtime.BuildFrame(), "Cancel"));
}

TEST_CASE("ValidationDefaultsResolveFrameworkResourcesAndTrackLocale") {
  TestResources resources;
  resources.assets.emplace(
      detail::resource_index_path,
      EncodeIndex({
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/validation_required",
              .mime_type = "text/plain",
              .value = "This field is required",
              .domain = "huxerui",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/validation_required",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "此字段为必填项",
              .domain = "huxerui",
          },
      })
  );
  TestPlatform platform;
  platform.platform_resources = &resources;
  Runtime runtime{FrameworkValidationResourceApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 100.0F}});

  REQUIRE(ContainsText(runtime.BuildFrame(), "此字段为必填项"));
  resources.configuration.locale = Locale::FromLanguageTag("en-US");
  runtime.UpdateResourceConfiguration(resources.configuration);
  REQUIRE(ContainsText(runtime.BuildFrame(), "This field is required"));
}

TEST_CASE("TextSelectionLabelsResolveFrameworkResourcesAndTrackLocale") {
  TestResources resources;
  resources.assets.emplace(
      detail::resource_index_path,
      EncodeIndex({
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/text_selection_cut",
              .mime_type = "text/plain",
              .value = "Cut",
              .domain = "huxerui",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/text_selection_cut",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "剪切",
              .domain = "huxerui",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/text_selection_copy",
              .mime_type = "text/plain",
              .value = "Copy",
              .domain = "huxerui",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/text_selection_copy",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "复制",
              .domain = "huxerui",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/text_selection_paste",
              .mime_type = "text/plain",
              .value = "Paste",
              .domain = "huxerui",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/text_selection_paste",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "粘贴",
              .domain = "huxerui",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/text_selection_select_all",
              .mime_type = "text/plain",
              .value = "Select all",
              .domain = "huxerui",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/text_selection_select_all",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "全选",
              .domain = "huxerui",
          },
      })
  );
  TestClipboard clipboard;
  TestPlatform platform;
  platform.platform_clipboard = &clipboard;
  platform.platform_resources = &resources;
  Runtime runtime{FrameworkSelectionResourceApp, platform};
  runtime.SetWindowMetrics({.viewport = {280.0F, 120.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 901, {20.0F, 20.0F}, PointerDeviceKind::Touch});
  platform.AdvanceTime(0.5);
  REQUIRE(ContainsText(runtime.BuildFrame(), "复制"));

  resources.configuration.locale = Locale::FromLanguageTag("en-US");
  runtime.UpdateResourceConfiguration(resources.configuration);
  const FlattenedScene& updated = runtime.BuildFrame();
  REQUIRE_FALSE(ContainsText(updated, "复制"));
  REQUIRE(ContainsText(updated, "Copy"));

  resources.configuration.locale = Locale::FromLanguageTag("zh-CN");
  TestPlatform partial_platform;
  partial_platform.platform_clipboard = &clipboard;
  partial_platform.platform_resources = &resources;
  Runtime partial_runtime{PartialSelectionLabelsResourceApp, partial_platform};
  partial_runtime.SetWindowMetrics({.viewport = {280.0F, 120.0F}});
  partial_runtime.BuildFrame();
  partial_runtime.HandlePointerEvent({PointerEventType::Down, 902, {20.0F, 20.0F}, PointerDeviceKind::Touch});
  partial_platform.AdvanceTime(0.5);
  const FlattenedScene& partial = partial_runtime.BuildFrame();
  REQUIRE(ContainsText(partial, "剪切"));
  REQUIRE(ContainsText(partial, "Duplicate"));
}

TEST_CASE("TextAndControlsResolveStringResourcesDirectly") {
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
              .value = "Localized title",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/action",
              .mime_type = "text/plain",
              .value = "Action",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/action",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "Localized action",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/placeholder",
              .mime_type = "text/plain",
              .value = "Placeholder",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/placeholder",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "Localized placeholder",
          },
      })
  );
  TestPlatform platform;
  platform.platform_resources = &resources;
  Runtime runtime{DirectLocalizedResourceApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 160.0F}});

  const FlattenedScene& scene = runtime.BuildFrame();
  REQUIRE(ContainsText(scene, "Localized title"));
  REQUIRE(ContainsText(scene, "Localized action"));
  REQUIRE(ContainsText(scene, "Localized placeholder"));
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
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/greeting",
              .mime_type = "text/plain",
              .value = "Hello {0}",
              .argument_count = 1,
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
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  REQUIRE(resource_menu.has_value());

  resource_menu->Show({
      MenuItem(ImageResource("test", "images/menu_icon"), StringResource("test", "strings/menu_item"), [] {}),
      MenuItem(StringVariant::Format(StringResource("test", "strings/greeting"), "Ada"), [] {}),
  });
  const FlattenedScene& shown = runtime.BuildFrame();
  REQUIRE(ContainsText(shown, "Resource item"));
  REQUIRE(ContainsText(shown, "Hello Ada"));
  REQUIRE(std::ranges::any_of(shown.Commands(), [](const PaintCommand& command) {
    return std::holds_alternative<huxerui::DrawImageCommand>(command);
  }));
}

TEST_CASE("PresentedStringVariantsRefreshWhenTheResourceConfigurationChanges") {
  resource_menu.reset();
  TestResources resources;
  resources.assets.emplace(
      detail::resource_index_path,
      EncodeIndex({
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/menu_item",
              .mime_type = "text/plain",
              .value = "English item",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/menu_item",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "Chinese item",
          },
      })
  );

  TestPlatform platform;
  platform.platform_resources = &resources;
  Runtime runtime{ResourceMenuApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  runtime.BuildFrame();
  REQUIRE(resource_menu.has_value());

  resource_menu->Show({MenuItem(StringResource("test", "strings/menu_item"), [] {})});
  REQUIRE(ContainsText(runtime.BuildFrame(), "Chinese item"));

  resources.configuration.locale = Locale::FromLanguageTag("en-US");
  runtime.UpdateResourceConfiguration(resources.configuration);
  const FlattenedScene& updated = runtime.BuildFrame();
  REQUIRE(!ContainsText(updated, "Chinese item"));
  REQUIRE(ContainsText(updated, "English item"));
}

TEST_CASE("WindowCaptionLabelsRefreshWhenTheResourceConfigurationChanges") {
  TestResources resources;
  resources.assets.emplace(
      detail::resource_index_path,
      EncodeIndex({
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/minimize_window",
              .mime_type = "text/plain",
              .value = "Minimize window",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/minimize_window",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "Minimize localized window",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/window_maximize",
              .mime_type = "text/plain",
              .value = "Maximize",
              .domain = "huxerui",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/window_maximize",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "最大化",
              .domain = "huxerui",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/window_restore",
              .mime_type = "text/plain",
              .value = "Restore",
              .domain = "huxerui",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/window_restore",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "还原",
              .domain = "huxerui",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/window_close",
              .mime_type = "text/plain",
              .value = "Close",
              .domain = "huxerui",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/window_close",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "关闭",
              .domain = "huxerui",
          },
      })
  );

  TestPlatform platform;
  platform.platform_resources = &resources;
  AppOptions options;
  options.show_debug_overlay = false;
  options.window.chrome_mode = WindowChromeMode::Custom;
  options.window.caption_labels.minimize = StringResource("test", "strings/minimize_window");
  Runtime runtime{LocalizedWindowControlsApp, platform, options};
  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 40.0F, .right_inset = 138.0F},
  });

  const auto contains_label = [](const FrameCommit& frame, std::string_view label) {
    return frame.semantic_frame != nullptr &&
           std::ranges::any_of(frame.semantic_frame->nodes, [label](const SemanticNode& node) {
             return node.label == label;
           });
  };
  const FrameCommit& localized = runtime.BuildCommit();
  REQUIRE(contains_label(localized, "Minimize localized window"));
  REQUIRE(contains_label(localized, "最大化"));
  REQUIRE(contains_label(localized, "关闭"));

  runtime.SetWindowMetrics({
      .viewport = {300.0F, 100.0F},
      .title_bar = WindowTitleBarMetrics{.height = 40.0F, .right_inset = 138.0F, .maximized = true},
  });
  REQUIRE(contains_label(runtime.BuildCommit(), "还原"));

  resources.configuration.locale = Locale::FromLanguageTag("en-US");
  runtime.UpdateResourceConfiguration(resources.configuration);
  const FrameCommit& updated = runtime.BuildCommit();
  REQUIRE(contains_label(updated, "Minimize window"));
  REQUIRE(contains_label(updated, "Restore"));
  REQUIRE(contains_label(updated, "Close"));
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
  missing.SetWindowMetrics({.viewport = {200.0F, 60.0F}});
  REQUIRE_THROWS_AS(missing.BuildFrame(), std::invalid_argument);

  Runtime extra{ExtraResourceArgumentsApp, platform};
  extra.SetWindowMetrics({.viewport = {200.0F, 60.0F}});
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
