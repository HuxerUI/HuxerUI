#include "runtime_test_support.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "image_test_support.h"
#include "resource_internal.h"
#include "text_field_internal.h"

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

std::uint8_t WireKind(detail::ResourceEntryKind kind) {
  switch (kind) {
  case detail::ResourceEntryKind::Raw:
    return 1;
  case detail::ResourceEntryKind::Image:
    return 2;
  case detail::ResourceEntryKind::String:
    return 3;
  }
  throw std::logic_error("test resource entry has an unknown kind");
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
    bytes.push_back(static_cast<std::byte>(WireKind(entry.kind)));
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
    const float intrinsic_width =
        entry.kind == detail::ResourceEntryKind::Image ? static_cast<float>(entry.width) / entry.scale : 0.0F;
    const float intrinsic_height =
        entry.kind == detail::ResourceEntryKind::Image ? static_cast<float>(entry.height) / entry.scale : 0.0F;
    AppendU32(bytes, std::bit_cast<std::uint32_t>(intrinsic_width));
    AppendU32(bytes, std::bit_cast<std::uint32_t>(intrinsic_height));
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
int density_image_compositions = 0;
int density_unrelated_compositions = 0;
int virtual_density_compositions = 0;
int direct_literal_resource_compositions = 0;
int literal_text_field_compositions = 0;
int literal_semantics_compositions = 0;
int resource_semantics_compositions = 0;
int literal_tooltip_compositions = 0;
int resource_tooltip_compositions = 0;
State<bool> missing_text_field_placeholder;
ImageAsset direct_image_asset;
State<bool> alternate_nested_shaping_locale;
int inherited_shaping_compositions = 0;
int explicit_shaping_compositions = 0;
int resource_explicit_shaping_compositions = 0;
int nested_inherited_shaping_compositions = 0;
int nested_explicit_shaping_compositions = 0;
int shaping_unrelated_compositions = 0;
int text_measurer_shaping_compositions = 0;

class ShapingRecordingPlatform final : public TestPlatform {
public:
  struct Measurement {
    std::string text;
    TextLayoutOptions options;
  };

  TextLayoutMetrics MeasureText(const huxerui::AttributedText& text, const TextStyle& style, float max_width,
      const TextLayoutOptions& options) override {
    measurements.push_back({text.PlainText(), options});
    return TestPlatform::MeasureText(text, style, max_width, options);
  }

  std::vector<Measurement> measurements;
};

View InheritedShapingContent() {
  ++inherited_shaping_compositions;
  return Column {
    Text("root inherited"),
    Text(StringResource("test", "strings/shaped")),
    Button("button inherited"),
    Chip("chip inherited"),
    RadioButton("radio inherited", false),
    Switch("switch inherited", false),
    Canvas([](PaintContext& paint, Size) {
      const TextStyle style = TextStyle::Default();
      paint.DrawText({0.0F, 0.0F, 120.0F, 20.0F}, "canvas paragraph", style);
      paint.DrawTextRun({0.0F, 20.0F, 70.0F, 20.0F}, {0.0F, 35.0F}, "canvas run", style);
      paint.DrawTextRuns({
          TextRun{{0.0F, 40.0F, 80.0F, 20.0F}, {0.0F, 55.0F}, "canvas batch", style, {}},
          TextRun{
              {0.0F, 60.0F, 90.0F, 20.0F},
              {0.0F, 75.0F},
              "canvas explicit",
              style,
              {.locale = "ko-KR"},
          },
      });
    }).With(huxerui::Frame{120.0F, 80.0F}),
  };
}

View ExplicitShapingContent() {
  ++explicit_shaping_compositions;
  return Text("root explicit").Shaping({.direction = TextDirection::RightToLeft, .locale = "fa-IR"});
}

View ResourceExplicitShapingContent() {
  ++resource_explicit_shaping_compositions;
  return Text(StringResource("test", "strings/explicit_shaped")).Shaping({.locale = "fa-IR"});
}

View NestedInheritedShapingContent() {
  ++nested_inherited_shaping_compositions;
  auto alternate_nested = UseState(false);
  alternate_nested_shaping_locale = alternate_nested;
  const Locale locale = Locale::FromLanguageTag(alternate_nested.Get() ? "de-DE" : "ar-EG");
  return ProvideEnvironment(locale, Text("nested inherited"));
}

View NestedExplicitShapingContent() {
  ++nested_explicit_shaping_compositions;
  return Text("nested explicit").Shaping({.locale = "ja-JP"});
}

View ShapingUnrelatedContent() {
  ++shaping_unrelated_compositions;
  return Spacer();
}

View ExplicitTextMeasurerContent() {
  ++text_measurer_shaping_compositions;
  static_cast<void>(UseTextMeasurer().MeasureText("explicit measurer", TextStyle::Default()));
  return Spacer();
}

View InheritedTextShapingApp() {
  return Column {
    Scope(InheritedShapingContent),
    Scope(ExplicitShapingContent),
    Scope(ResourceExplicitShapingContent),
    Scope(NestedInheritedShapingContent),
    ProvideEnvironment(Locale::FromLanguageTag("ar-EG"), Scope(NestedExplicitShapingContent)),
    Scope(ShapingUnrelatedContent),
    Scope(ExplicitTextMeasurerContent),
  };
}

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
  return ProvideEnvironment(
      TextSelectionMenuLabels{.copy = "Duplicate"},
      TextField(TextEditingValue::FromText("alpha beta")).With(huxerui::Frame{180.0F, 40.0F})
  );
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

View TextFieldResourceFailureApp() {
  auto missing = UseState(false);
  missing_text_field_placeholder = missing;
  const StringResource placeholder("test", missing.Get() ? "strings/missing_placeholder" : "strings/placeholder");
  return TextField(TextEditingValue::FromText("")).Placeholder(placeholder);
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

View DensityImageResourceContent() {
  ++density_image_compositions;
  return Image(ImageResource("test", "images/density"));
}

View DensityUnrelatedResourceContent() {
  ++density_unrelated_compositions;
  return Text("unrelated").Shaping({.locale = "en-US"});
}

View ResourceConfigurationDependencyApp() {
  return Column {
    Scope(DensityImageResourceContent),
    Scope(DensityUnrelatedResourceContent),
  };
}

View DirectLiteralResourceHelperApp() {
  ++direct_literal_resource_compositions;
  const detail::ResolvedImageAsset resolved = detail::UseImageVariant(ImageVariant{direct_image_asset});
  REQUIRE(std::get<ImageAsset>(resolved) == direct_image_asset);
  return Text(UseString(StringVariant{"literal"})).Shaping({.locale = "en-US"});
}

View VirtualDensityResourceContent() {
  ++virtual_density_compositions;
  return VirtualList(std::size_t{1}, [](std::size_t) {
    return Image(ImageResource("test", "images/density")).With(huxerui::Frame{20.0F, 10.0F});
  }).ItemExtent(10.0F);
}

View VirtualResourceConfigurationDependencyApp() {
  return Column {
    Scope(VirtualDensityResourceContent),
    Scope(DensityUnrelatedResourceContent),
  };
}

View LiteralTextFieldResourceContent() {
  ++literal_text_field_compositions;
  return TextField(TextEditingValue::FromText("literal")).Placeholder("placeholder");
}

View LiteralTextFieldResourceDependencyApp() {
  return Scope(LiteralTextFieldResourceContent);
}

View LiteralSemanticsResourceContent() {
  ++literal_semantics_compositions;
  return Text("semantic").Shaping({.locale = "en-US"}).With(Semantics{.label = "literal"});
}

View LiteralSemanticsResourceDependencyApp() {
  return Scope(LiteralSemanticsResourceContent);
}

View ResourceSemanticsContent() {
  ++resource_semantics_compositions;
  return Text("semantic").With(Semantics{.label = StringResource("test", "strings/semantic_label")});
}

View ResourceSemanticsDependencyApp() {
  return Scope(ResourceSemanticsContent);
}

View LiteralTooltipContent() {
  ++literal_tooltip_compositions;
  return Text("tooltip target").Shaping({.locale = "en-US"}).With(Tooltip("literal hint"));
}

View LiteralTooltipDependencyApp() {
  return Scope(LiteralTooltipContent);
}

View ResourceTooltipContent() {
  ++resource_tooltip_compositions;
  return Text("tooltip target").With(Tooltip(StringResource("test", "strings/tooltip_hint")));
}

View ResourceTooltipDependencyApp() {
  return Scope(ResourceTooltipContent);
}

} // namespace

TEST_CASE("BuiltinStringCatalogsContainEveryDefaultKeyWithoutFallback") {
  const auto entries = detail::ParseResourceIndex(BuiltinTestResources()->Read(detail::resource_index_path));
  std::map<std::string, std::set<std::string>> catalogs;
  for (const auto& entry : entries) {
    if (entry.kind != detail::ResourceEntryKind::String || entry.id.Domain() != "huxerui") {
      continue;
    }
    CAPTURE(entry.locale, entry.id.Key());
    REQUIRE(entry.value.find_first_not_of(" \t\r\n") != std::string::npos);
    REQUIRE(catalogs[entry.locale].emplace(entry.id.Key()).second);
  }
  REQUIRE(catalogs.contains(""));
  REQUIRE(catalogs.size() > 1);
  const auto& default_keys = catalogs.at("");
  REQUIRE_FALSE(default_keys.empty());
  for (const auto& [locale, keys] : catalogs) {
    CAPTURE(locale);
    REQUIRE(keys == default_keys);
  }
}

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

TEST_CASE("ImageResourceScopesObserveDisplayScalePrecisely") {
  density_image_compositions = 0;
  density_unrelated_compositions = 0;
  TestResources resources;
  const RawAsset image = TestPng(20, 10);
  const RawAsset image_2x = TestPng(40, 20);
  resources.assets.emplace(
      detail::resource_index_path,
      EncodeIndex({
          {detail::ResourceEntryKind::Image,
           "images/density",
           "huxerui/test/images/density.png",
           "image/png",
           {},
           {},
           1.0F,
           20,
           10,
           Hash(image.Bytes())},
          {detail::ResourceEntryKind::Image,
           "images/density",
           "huxerui/test/images/density@2x.png",
           "image/png",
           {},
           {},
           2.0F,
           40,
           20,
           Hash(image_2x.Bytes())},
      })
  );
  resources.assets.emplace("huxerui/test/images/density.png", image);
  resources.assets.emplace("huxerui/test/images/density@2x.png", image_2x);
  TestPlatform platform;
  platform.platform_resources = &resources;
  Runtime runtime{ResourceConfigurationDependencyApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});

  runtime.BuildFrame();
  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(std::get<ImageAsset>(root->children[0]->children[0]->image_properties.source).Scale() == 2.0F);
  REQUIRE(density_image_compositions == 1);
  REQUIRE(density_unrelated_compositions == 1);

  resources.configuration.display_scale = 1.0F;
  runtime.UpdateResourceConfiguration(resources.configuration);
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(std::get<ImageAsset>(root->children[0]->children[0]->image_properties.source).Scale() == 1.0F);
  REQUIRE(density_image_compositions == 2);
  REQUIRE(density_unrelated_compositions == 1);

  resources.configuration.locale = Locale::FromLanguageTag("en-US");
  runtime.UpdateResourceConfiguration(resources.configuration);
  runtime.BuildFrame();
  REQUIRE(density_image_compositions == 3);
  REQUIRE(density_unrelated_compositions == 1);

  runtime.UpdateResourceConfiguration(resources.configuration);
  runtime.BuildFrame();
  REQUIRE(density_image_compositions == 3);
  REQUIRE(density_unrelated_compositions == 1);
}

TEST_CASE("DirectLiteralResourceHelpersDoNotObserveResourceConfiguration") {
  direct_literal_resource_compositions = 0;
  TestResources resources;
  resources.assets.emplace(detail::resource_index_path, EncodeIndex({}));
  TestPlatform platform;
  platform.platform_resources = &resources;
  direct_image_asset = ImageAsset::FromEncoded(MakeTestPng(20, 10));
  Runtime runtime{DirectLiteralResourceHelperApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});

  REQUIRE(FirstText(runtime.BuildFrame()) == "literal");
  REQUIRE(direct_literal_resource_compositions == 1);

  resources.configuration = {Locale::FromLanguageTag("fr-FR"), 2.0F};
  runtime.UpdateResourceConfiguration(resources.configuration);
  runtime.BuildFrame();
  REQUIRE(direct_literal_resource_compositions == 1);
}

TEST_CASE("MountedTextAndCanvasInheritLocaleWithoutSubscribingExplicitShaping") {
  inherited_shaping_compositions = 0;
  explicit_shaping_compositions = 0;
  resource_explicit_shaping_compositions = 0;
  nested_inherited_shaping_compositions = 0;
  nested_explicit_shaping_compositions = 0;
  shaping_unrelated_compositions = 0;
  text_measurer_shaping_compositions = 0;
  TestResources resources;
  resources.assets.emplace(
      detail::resource_index_path,
      EncodeIndex({
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/shaped",
              .mime_type = "text/plain",
              .value = "resource default",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/shaped",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "resource inherited",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/explicit_shaped",
              .mime_type = "text/plain",
              .value = "explicit resource default",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/explicit_shaped",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "explicit resource localized",
          },
      })
  );
  ShapingRecordingPlatform platform;
  platform.platform_resources = &resources;
  Runtime runtime{InheritedTextShapingApp, platform};
  runtime.SetWindowMetrics({.viewport = {400.0F, 700.0F}});

  const auto find_paragraph = [](const FlattenedScene& scene, std::string_view text) {
    for (const PaintCommand& command : scene.Commands()) {
      if (const auto* paragraph = std::get_if<DrawTextCommand>(&command);
          paragraph != nullptr && paragraph->text.PlainText() == text) {
        return paragraph;
      }
    }
    return static_cast<const DrawTextCommand*>(nullptr);
  };
  const auto find_run = [](const FlattenedScene& scene, std::string_view text) {
    for (const PaintCommand& command : scene.Commands()) {
      if (const auto* batch = std::get_if<DrawTextRunsCommand>(&command)) {
        const auto found = std::ranges::find(batch->runs, text, &TextRun::text);
        if (found != batch->runs.end()) {
          return &*found;
        }
      }
    }
    return static_cast<const TextRun*>(nullptr);
  };
  const auto last_measurement = [&platform](std::string_view text) {
    const auto found = std::find_if(
        platform.measurements.rbegin(),
        platform.measurements.rend(),
        [text](const ShapingRecordingPlatform::Measurement& measurement) {
          return measurement.text == text;
        }
    );
    return found == platform.measurements.rend() ? nullptr : &*found;
  };
  const auto require_measurement_matches = [&last_measurement](
                                               std::string_view text,
                                               const DrawTextCommand& command
                                           ) {
    const ShapingRecordingPlatform::Measurement* measurement = last_measurement(text);
    REQUIRE(measurement != nullptr);
    REQUIRE(measurement->options == command.options);
  };

  const FlattenedScene& initial = runtime.BuildFrame();
  const DrawTextCommand* root_text = find_paragraph(initial, "root inherited");
  const DrawTextCommand* resource_text = find_paragraph(initial, "resource inherited");
  const DrawTextCommand* root_button = find_paragraph(initial, "button inherited");
  const DrawTextCommand* root_chip = find_paragraph(initial, "chip inherited");
  const DrawTextCommand* root_radio = find_paragraph(initial, "radio inherited");
  const DrawTextCommand* root_switch = find_paragraph(initial, "switch inherited");
  const DrawTextCommand* explicit_text = find_paragraph(initial, "root explicit");
  const DrawTextCommand* explicit_resource_text = find_paragraph(initial, "explicit resource localized");
  const DrawTextCommand* nested_text = find_paragraph(initial, "nested inherited");
  const DrawTextCommand* nested_explicit = find_paragraph(initial, "nested explicit");
  REQUIRE(root_text != nullptr);
  REQUIRE(resource_text != nullptr);
  REQUIRE(root_button != nullptr);
  REQUIRE(root_chip != nullptr);
  REQUIRE(root_radio != nullptr);
  REQUIRE(root_switch != nullptr);
  REQUIRE(explicit_text != nullptr);
  REQUIRE(explicit_resource_text != nullptr);
  REQUIRE(nested_text != nullptr);
  REQUIRE(nested_explicit != nullptr);
  const DrawTextCommand* canvas_paragraph = find_paragraph(initial, "canvas paragraph");
  const TextRun* canvas_run = find_run(initial, "canvas run");
  const TextRun* canvas_batch = find_run(initial, "canvas batch");
  const TextRun* canvas_explicit = find_run(initial, "canvas explicit");
  const ShapingRecordingPlatform::Measurement* root_measurement = last_measurement("root inherited");
  const ShapingRecordingPlatform::Measurement* measurer_measurement = last_measurement("explicit measurer");
  REQUIRE(canvas_paragraph != nullptr);
  REQUIRE(canvas_run != nullptr);
  REQUIRE(canvas_batch != nullptr);
  REQUIRE(canvas_explicit != nullptr);
  REQUIRE(root_measurement != nullptr);
  REQUIRE(measurer_measurement != nullptr);
  REQUIRE(root_text->options.shaping.locale == "zh-Hans-CN");
  REQUIRE(resource_text->options.shaping.locale == "zh-Hans-CN");
  REQUIRE(root_button->options.shaping.locale == "zh-Hans-CN");
  REQUIRE(root_chip->options.shaping.locale == "zh-Hans-CN");
  REQUIRE(root_radio->options.shaping.locale == "zh-Hans-CN");
  REQUIRE(root_switch->options.shaping.locale == "zh-Hans-CN");
  const TextShapingOptions explicit_options{.direction = TextDirection::RightToLeft, .locale = "fa-IR"};
  REQUIRE(explicit_text->options.shaping == explicit_options);
  REQUIRE(explicit_resource_text->options.shaping.locale == "fa-IR");
  REQUIRE(nested_text->options.shaping.locale == "ar-EG");
  REQUIRE(nested_explicit->options.shaping.locale == "ja-JP");
  REQUIRE(canvas_paragraph->options.shaping.locale == "zh-Hans-CN");
  REQUIRE(canvas_run->shaping.locale == "zh-Hans-CN");
  REQUIRE(canvas_batch->shaping.locale == "zh-Hans-CN");
  REQUIRE(canvas_explicit->shaping.locale == "ko-KR");
  REQUIRE(root_measurement->options == root_text->options);
  require_measurement_matches("button inherited", *root_button);
  require_measurement_matches("chip inherited", *root_chip);
  require_measurement_matches("radio inherited", *root_radio);
  require_measurement_matches("switch inherited", *root_switch);
  REQUIRE(measurer_measurement->options.shaping.locale.empty());
  REQUIRE(inherited_shaping_compositions == 1);
  REQUIRE(explicit_shaping_compositions == 1);
  REQUIRE(resource_explicit_shaping_compositions == 1);
  REQUIRE(nested_inherited_shaping_compositions == 1);
  REQUIRE(nested_explicit_shaping_compositions == 1);
  REQUIRE(shaping_unrelated_compositions == 1);
  REQUIRE(text_measurer_shaping_compositions == 1);

  resources.configuration.locale = Locale::FromLanguageTag("fr-FR");
  runtime.UpdateResourceConfiguration(resources.configuration);
  const FlattenedScene& root_updated = runtime.BuildFrame();
  root_text = find_paragraph(root_updated, "root inherited");
  resource_text = find_paragraph(root_updated, "resource default");
  canvas_paragraph = find_paragraph(root_updated, "canvas paragraph");
  explicit_text = find_paragraph(root_updated, "root explicit");
  explicit_resource_text = find_paragraph(root_updated, "explicit resource default");
  nested_text = find_paragraph(root_updated, "nested inherited");
  REQUIRE(root_text != nullptr);
  REQUIRE(resource_text != nullptr);
  REQUIRE(canvas_paragraph != nullptr);
  REQUIRE(explicit_text != nullptr);
  REQUIRE(explicit_resource_text != nullptr);
  REQUIRE(nested_text != nullptr);
  REQUIRE(root_text->options.shaping.locale == "fr-FR");
  REQUIRE(resource_text->options.shaping.locale == "fr-FR");
  REQUIRE(canvas_paragraph->options.shaping.locale == "fr-FR");
  REQUIRE(explicit_text->options.shaping.locale == "fa-IR");
  REQUIRE(explicit_resource_text->options.shaping.locale == "fa-IR");
  REQUIRE(nested_text->options.shaping.locale == "ar-EG");
  REQUIRE(inherited_shaping_compositions == 2);
  REQUIRE(explicit_shaping_compositions == 1);
  REQUIRE(resource_explicit_shaping_compositions == 2);
  REQUIRE(nested_inherited_shaping_compositions == 1);
  REQUIRE(nested_explicit_shaping_compositions == 1);
  REQUIRE(shaping_unrelated_compositions == 1);
  REQUIRE(text_measurer_shaping_compositions == 1);

  alternate_nested_shaping_locale = true;
  const FlattenedScene& nested_updated = runtime.BuildFrame();
  nested_text = find_paragraph(nested_updated, "nested inherited");
  nested_explicit = find_paragraph(nested_updated, "nested explicit");
  REQUIRE(nested_text != nullptr);
  REQUIRE(nested_explicit != nullptr);
  REQUIRE(nested_text->options.shaping.locale == "de-DE");
  REQUIRE(nested_explicit->options.shaping.locale == "ja-JP");
  REQUIRE(inherited_shaping_compositions == 2);
  REQUIRE(explicit_shaping_compositions == 1);
  REQUIRE(resource_explicit_shaping_compositions == 2);
  REQUIRE(nested_inherited_shaping_compositions == 2);
  REQUIRE(nested_explicit_shaping_compositions == 1);
  REQUIRE(shaping_unrelated_compositions == 1);
  REQUIRE(text_measurer_shaping_compositions == 1);
}

TEST_CASE("LiteralTextFieldsDoNotObserveResourceConfiguration") {
  literal_text_field_compositions = 0;
  TestResources resources;
  resources.assets.emplace(detail::resource_index_path, EncodeIndex({}));
  TestPlatform platform;
  platform.platform_resources = &resources;
  Runtime runtime{LiteralTextFieldResourceDependencyApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 80.0F}});

  runtime.BuildFrame();
  REQUIRE(literal_text_field_compositions == 1);

  resources.configuration.display_scale = 2.0F;
  runtime.UpdateResourceConfiguration(resources.configuration);
  runtime.BuildFrame();
  REQUIRE(literal_text_field_compositions == 1);
}

TEST_CASE("LiteralSemanticsDoNotObserveResourceConfiguration") {
  literal_semantics_compositions = 0;
  TestResources resources;
  resources.assets.emplace(detail::resource_index_path, EncodeIndex({}));
  TestPlatform platform;
  platform.platform_resources = &resources;
  Runtime runtime{LiteralSemanticsResourceDependencyApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 80.0F}});

  runtime.BuildFrame();
  REQUIRE(literal_semantics_compositions == 1);

  resources.configuration.locale = Locale::FromLanguageTag("fr-FR");
  runtime.UpdateResourceConfiguration(resources.configuration);
  runtime.BuildFrame();
  REQUIRE(literal_semantics_compositions == 1);
}

TEST_CASE("ResourceSemanticsObserveLocalePrecisely") {
  resource_semantics_compositions = 0;
  TestResources resources;
  resources.assets.emplace(
      detail::resource_index_path,
      EncodeIndex({
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/semantic_label",
              .mime_type = "text/plain",
              .value = "English label",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/semantic_label",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "Chinese label",
          },
      })
  );
  TestPlatform platform;
  platform.platform_resources = &resources;
  Runtime runtime{ResourceSemanticsDependencyApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 80.0F}});

  const auto contains_label = [](const FrameCommit& frame, std::string_view label) {
    return frame.semantic_frame != nullptr &&
           std::ranges::any_of(frame.semantic_frame->nodes, [label](const SemanticNode& node) {
             return node.label == label;
           });
  };
  REQUIRE(contains_label(runtime.BuildCommit(), "Chinese label"));
  REQUIRE(resource_semantics_compositions == 1);

  resources.configuration.locale = Locale::FromLanguageTag("en-US");
  runtime.UpdateResourceConfiguration(resources.configuration);
  REQUIRE(contains_label(runtime.BuildCommit(), "English label"));
  REQUIRE(resource_semantics_compositions == 2);
}

TEST_CASE("TooltipCompileObservesOnlyResourceBackedMessages") {
  literal_tooltip_compositions = 0;
  resource_tooltip_compositions = 0;
  TestResources resources;
  resources.assets.emplace(
      detail::resource_index_path,
      EncodeIndex({
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/tooltip_hint",
              .mime_type = "text/plain",
              .value = "English hint",
          },
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/tooltip_hint",
              .mime_type = "text/plain",
              .locale = "zh",
              .value = "Chinese hint",
          },
      })
  );
  TestPlatform platform;
  platform.platform_resources = &resources;
  Runtime literal{LiteralTooltipDependencyApp, platform};
  Runtime localized{ResourceTooltipDependencyApp, platform};
  literal.SetWindowMetrics({.viewport = {240.0F, 80.0F}});
  localized.SetWindowMetrics({.viewport = {240.0F, 80.0F}});

  const auto contains_hint = [](const FrameCommit& frame, std::string_view hint) {
    return frame.semantic_frame != nullptr &&
           std::ranges::any_of(frame.semantic_frame->nodes, [hint](const SemanticNode& node) {
             return node.hint == hint;
           });
  };
  REQUIRE(contains_hint(literal.BuildCommit(), "literal hint"));
  REQUIRE(contains_hint(localized.BuildCommit(), "Chinese hint"));
  REQUIRE(literal_tooltip_compositions == 1);
  REQUIRE(resource_tooltip_compositions == 1);

  resources.configuration.locale = Locale::FromLanguageTag("en-US");
  literal.UpdateResourceConfiguration(resources.configuration);
  localized.UpdateResourceConfiguration(resources.configuration);
  REQUIRE(contains_hint(literal.BuildCommit(), "literal hint"));
  REQUIRE(contains_hint(localized.BuildCommit(), "English hint"));
  REQUIRE(literal_tooltip_compositions == 1);
  REQUIRE(resource_tooltip_compositions == 2);
}

TEST_CASE("VirtualItemsRetainResourceDependenciesOnTheirDeclaringScope") {
  virtual_density_compositions = 0;
  density_unrelated_compositions = 0;
  TestResources resources;
  const RawAsset image = TestPng(20, 10);
  const RawAsset image_2x = TestPng(40, 20);
  resources.assets.emplace(
      detail::resource_index_path,
      EncodeIndex({
          {detail::ResourceEntryKind::Image,
           "images/density",
           "huxerui/test/images/density.png",
           "image/png",
           {},
           {},
           1.0F,
           20,
           10,
           Hash(image.Bytes())},
          {detail::ResourceEntryKind::Image,
           "images/density",
           "huxerui/test/images/density@2x.png",
           "image/png",
           {},
           {},
           2.0F,
           40,
           20,
           Hash(image_2x.Bytes())},
      })
  );
  resources.assets.emplace("huxerui/test/images/density.png", image);
  resources.assets.emplace("huxerui/test/images/density@2x.png", image_2x);
  TestPlatform platform;
  platform.platform_resources = &resources;
  Runtime runtime{VirtualResourceConfigurationDependencyApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});

  runtime.BuildFrame();
  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(std::get<ImageAsset>(root->children[0]->children[0]->children[0]->image_properties.source).Scale() == 2.0F);
  REQUIRE(virtual_density_compositions == 1);
  REQUIRE(density_unrelated_compositions == 1);

  resources.configuration.display_scale = 1.0F;
  runtime.UpdateResourceConfiguration(resources.configuration);
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(std::get<ImageAsset>(root->children[0]->children[0]->children[0]->image_properties.source).Scale() == 1.0F);
  REQUIRE(virtual_density_compositions == 2);
  REQUIRE(density_unrelated_compositions == 1);
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

  resources.configuration.locale = Locale::FromLanguageTag("en-US");
  runtime.UpdateResourceConfiguration(resources.configuration);
  const FlattenedScene& updated = runtime.BuildFrame();
  REQUIRE(ContainsText(updated, "Title"));
  REQUIRE(ContainsText(updated, "Action"));
  REQUIRE(ContainsText(updated, "Placeholder"));
  REQUIRE_FALSE(ContainsText(updated, "Localized title"));
}

TEST_CASE("TextFieldResourceFailurePreservesTheCommittedExtensionValue") {
  TestResources resources;
  resources.assets.emplace(
      detail::resource_index_path,
      EncodeIndex({
          {
              .kind = detail::ResourceEntryKind::String,
              .key = "strings/placeholder",
              .mime_type = "text/plain",
              .value = "Placeholder",
          },
      })
  );
  TestPlatform platform;
  platform.platform_resources = &resources;
  Runtime runtime{TextFieldResourceFailureApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  runtime.BuildFrame();

  const auto find_text_field_modifier = [](const detail::MountedNode& node) -> const detail::TextFieldModifier* {
    const auto found = std::ranges::find_if(node.extensions, [](const detail::NodeExtensionEntry& entry) {
      return entry.descriptor == &detail::TextFieldModifier::Descriptor();
    });
    if (found == node.extensions.end()) {
      return nullptr;
    }
    return static_cast<const detail::TextFieldModifier*>(found->value.get());
  };
  const detail::MountedNode* field = runtime.RootNode();
  REQUIRE(field != nullptr);
  const auto* committed = find_text_field_modifier(*field);
  REQUIRE(committed != nullptr);
  REQUIRE(detail::StringLiteral(committed->placeholder) == "Placeholder");

  missing_text_field_placeholder = true;
  REQUIRE_THROWS(runtime.BuildFrame());

  field = runtime.RootNode();
  REQUIRE(field != nullptr);
  committed = find_text_field_modifier(*field);
  REQUIRE(committed != nullptr);
  REQUIRE(detail::StringLiteral(committed->placeholder) == "Placeholder");
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
