#include <catch2/catch_amalgamated.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include <huxerui/paint.h>
#include <huxerui/resource.h>

#include "compiler.h"
#include "image_test_support.h"
#include "path_internal.h"
#include "resource_internal.h"
#include "runtime_test_support.h"
#include "vector_internal.h"

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("hrc-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& Path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void Write(const std::filesystem::path& path, std::string_view value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary);
  REQUIRE(stream);
  stream << value;
  REQUIRE(stream);
}

void Write(const std::filesystem::path& path, std::span<const std::byte> value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary);
  REQUIRE(stream);
  if (!value.empty()) {
    stream.write(reinterpret_cast<const char*>(value.data()), static_cast<std::streamsize>(value.size()));
  }
  REQUIRE(stream);
}

std::string Read(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  REQUIRE(stream);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::filesystem::path PathFromUtf8(std::string_view value) {
  std::u8string utf8(value.size(), u8'\0');
  if (!value.empty()) {
    std::memcpy(utf8.data(), value.data(), value.size());
  }
  return std::filesystem::path(std::move(utf8));
}

class DirectoryResources final : public huxerui::PlatformResources {
public:
  explicit DirectoryResources(std::filesystem::path root) : root_(std::move(root)) {}

  huxerui::ResourceConfiguration Configuration() const override {
    return {};
  }

  huxerui::RawAsset Read(std::string_view package_path) override {
    const std::filesystem::path path = root_ / PathFromUtf8(package_path);
    if (!std::filesystem::is_regular_file(path)) {
      return {};
    }
    std::ifstream stream(path, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    return huxerui::RawAsset::CopyBytes(std::as_bytes(std::span(bytes)));
  }

private:
  std::filesystem::path root_;
};

huxerui::View VectorResourceApp() {
  return huxerui::Image(huxerui::ImageResource("test_app", "images/mark"));
}

huxerui::VectorAsset CompileVectorResource(
    const std::filesystem::path& temporary, std::string_view svg, std::string_view output_name = "vector-output"
) {
  const std::filesystem::path root = temporary / "vector-assets";
  const std::filesystem::path output = temporary / output_name;
  Write(root / "images" / "mark.svg", svg);
  huxerui::resource_compiler::Compile({root, output, "test_app"});
  DirectoryResources resources(output / "package");
  huxerui::detail::AppResources app_resources(&resources);
  return app_resources.ResolveVector(
      huxerui::ImageResource("test_app", "images/mark"),
      huxerui::Locale::Default()
  );
}

TEST_CASE("ResourceCompilerGeneratesTypedKeysIndexAndPayloads") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  const std::filesystem::path output = temporary.Path() / "output";
  Write(root / "raw" / "config.json", "{\"enabled\":true}\n");
  Write(root / "strings" / "default.properties", "title = \"Hello {0}\"\n");
  Write(root / "strings" / "zh.properties", "title = \"你好，{0}\"\n");

  huxerui::resource_compiler::Compile({root, output, "test_app"});

  const std::string header = Read(output / "include" / "test_app_resources.h");
  REQUIRE(header.find("namespace test_app {") != std::string::npos);
  REQUIRE(header.find("namespace test_app_resources") == std::string::npos);
  REQUIRE(header.find("huxerui::RawResource config_json") != std::string::npos);
  REQUIRE(header.find("huxerui::StringResource title") != std::string::npos);
  REQUIRE(Read(output / "package" / "huxerui" / "test_app" / "raw" / "config.json") == "{\"enabled\":true}\n");
  REQUIRE(std::filesystem::file_size(output / "package" / "huxerui" / "resources.bin") > 16);
  REQUIRE_FALSE(std::filesystem::exists(output / "resources.stamp"));
}

TEST_CASE("ResourceCompilerRejectsAnEmptyResourceRoot") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  const std::filesystem::path output = temporary.Path() / "output";
  Write(root / "strings" / "default.properties", "");

  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, output, "test_app"}),
      Catch::Matchers::ContainsSubstring("resource root does not contain any supported resources")
  );
}

TEST_CASE("ResourceCompilerAcceptsACustomGeneratedHeaderName") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  const std::filesystem::path output = temporary.Path() / "output";
  Write(root / "strings" / "default.properties", "title = Hello\n");

  huxerui::resource_compiler::Compile({root, output, "test_app", "builtin_resources.h"});

  const std::string header = Read(output / "include" / "builtin_resources.h");
  REQUIRE(header.find("namespace test_app {") != std::string::npos);
  REQUIRE_FALSE(std::filesystem::exists(output / "include" / "test_app_resources.h"));
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, output, "test_app", "nested/builtin_resources.h"}),
      "resource header name must be a .h filename without a directory"
  );
}

TEST_CASE("ResourceCompilerRejectsInvalidMessagePlaceholders") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  Write(root / "strings" / "default.properties", "title = \"Hello {name}\"\n");

  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "output", "test_app"}),
      "localized string contains an invalid positional placeholder"
  );
}

TEST_CASE("ResourceCompilerRejectsLegacyTextCatalogs") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  Write(root / "strings" / "default.txt", "title = Legacy\n");

  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "output", "test_app"}),
      Catch::Matchers::ContainsSubstring("string catalog must use the .properties extension")
  );
}

TEST_CASE("ResourceCompilerRejectsGeneratedIdentifierCollisions") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  Write(root / "raw" / "a-b.txt", "first");
  Write(root / "raw" / "a_b.txt", "second");

  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "output", "test_app"}),
      "resource keys generate the same C++ identifier: raw/a-b.txt and raw/a_b.txt"
  );
}

TEST_CASE("ResourceCompilerEscapesReservedGeneratedIdentifiers") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  const std::filesystem::path output = temporary.Path() / "output";
  Write(root / "raw" / "class", "reserved");

  huxerui::resource_compiler::Compile({root, output, "test_app"});

  REQUIRE(Read(output / "include" / "test_app_resources.h").find("resource_class") != std::string::npos);
}

TEST_CASE("ResourceCompilerRejectsReservedNamespacesAndInvalidKeys") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  Write(root / "raw" / "config.txt", "value");
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "reserved", "class"}),
      "resource namespace must be a non-reserved C++ identifier"
  );

  Write(root / "strings" / "default.properties", "bad:key = value\n");
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "invalid-key", "test_app"}),
      Catch::Matchers::ContainsSubstring("resource path is not package-relative")
  );

  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "invalid-namespace", "test_app_"}),
      "resource namespace must be a non-reserved C++ identifier"
  );
}

TEST_CASE("ResourceCompilerRemovesStalePackagedPayloads") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  const std::filesystem::path output = temporary.Path() / "output";
  const std::filesystem::path old_source = root / "raw" / "old.txt";
  const std::filesystem::path old_output = output / "package" / "huxerui" / "test_app" / "raw" / "old.txt";
  Write(old_source, "old");
  huxerui::resource_compiler::Compile({root, output, "test_app"});
  REQUIRE(std::filesystem::exists(old_output));

  REQUIRE(std::filesystem::remove(old_source));
  Write(root / "raw" / "new.txt", "new");
  huxerui::resource_compiler::Compile({root, output, "test_app"});

  REQUIRE_FALSE(std::filesystem::exists(old_output));
  REQUIRE(Read(output / "package" / "huxerui" / "test_app" / "raw" / "new.txt") == "new");
}

TEST_CASE("ResourceCompilerRemovesOutputsFromThePreviousNamespace") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  const std::filesystem::path output = temporary.Path() / "output";
  Write(root / "raw" / "value.txt", "value");

  huxerui::resource_compiler::Compile({root, output, "old_app"});
  REQUIRE(std::filesystem::exists(output / "include" / "old_app_resources.h"));
  REQUIRE(std::filesystem::exists(output / "package" / "huxerui" / "old_app"));

  huxerui::resource_compiler::Compile({root, output, "new_app"});
  REQUIRE_FALSE(std::filesystem::exists(output / "include" / "old_app_resources.h"));
  REQUIRE_FALSE(std::filesystem::exists(output / "package" / "huxerui" / "old_app"));
  REQUIRE(std::filesystem::exists(output / "include" / "new_app_resources.h"));
}

TEST_CASE("ResourceCompilerMergesPackagesInDeclarationOrder") {
  TemporaryDirectory temporary;
  const std::filesystem::path base_root = temporary.Path() / "base";
  const std::filesystem::path library_root = temporary.Path() / "library";
  const std::filesystem::path override_root = temporary.Path() / "override";
  const std::filesystem::path base_output = temporary.Path() / "base-output";
  const std::filesystem::path library_output = temporary.Path() / "library-output";
  const std::filesystem::path override_output = temporary.Path() / "override-output";
  const std::filesystem::path merged_output = temporary.Path() / "merged-output";

  Write(base_root / "raw" / "config.txt", "base");
  Write(base_root / "raw" / "kept.txt", "kept");
  Write(base_root / "strings" / "default.properties", "title = Hello {0}\n");
  Write(base_root / "strings" / "zh.properties", "title = 你好，{0}\n");
  Write(base_root / "images" / "icon.png", huxerui::test::MakeTestPng(8, 4));
  Write(base_root / "images" / "mark.svg", R"(<svg viewBox="0 0 8 4"><path d="M0 0L8 4"/></svg>)");
  Write(library_root / "raw" / "tool.txt", "library");
  Write(override_root / "raw" / "config.txt", "override");
  Write(override_root / "strings" / "default.properties", "title = Welcome {0}\n");
  Write(override_root / "images" / "icon@2x.png", huxerui::test::MakeTestPng(16, 8));
  Write(override_root / "images" / "mark.png", huxerui::test::MakeTestPng(8, 4));

  huxerui::resource_compiler::Compile({base_root, base_output, "app"});
  huxerui::resource_compiler::Compile({library_root, library_output, "editor"});
  huxerui::resource_compiler::Compile({override_root, override_output, "app"});
  huxerui::resource_compiler::Merge(
      {{base_output / "package", library_output / "package", override_output / "package"}, merged_output}
  );

  REQUIRE(std::filesystem::exists(merged_output / "include" / "app_resources.h"));
  REQUIRE(std::filesystem::exists(merged_output / "include" / "editor_resources.h"));
  REQUIRE_FALSE(std::filesystem::exists(merged_output / "package" / "huxerui" / "app" / "images" / "mark.huxv"));
  REQUIRE(std::filesystem::exists(merged_output / "package" / "huxerui" / "app" / "images" / "mark.png"));
  REQUIRE(std::filesystem::exists(merged_output / "package" / "huxerui" / "app" / "images" / "icon.png"));
  REQUIRE(std::filesystem::exists(merged_output / "package" / "huxerui" / "app" / "images" / "icon@2x.png"));

  DirectoryResources platform(merged_output / "package");
  huxerui::detail::AppResources resources(&platform);
  REQUIRE(resources.Resolve(huxerui::RawResource("app", "raw/config.txt")).AsStringView() == "override");
  REQUIRE(resources.Resolve(huxerui::RawResource("app", "raw/kept.txt")).AsStringView() == "kept");
  REQUIRE(resources.Resolve(huxerui::RawResource("editor", "raw/tool.txt")).AsStringView() == "library");
  REQUIRE(
      resources.Resolve(huxerui::StringResource("app", "strings/title"), huxerui::Locale::Default()).value ==
      "Welcome {0}"
  );
  REQUIRE(
      resources.Resolve(huxerui::StringResource("app", "strings/title"), huxerui::Locale::FromLanguageTag("zh"))
          .value == "你好，{0}"
  );
  const huxerui::ImageAsset image =
      resources.Resolve(huxerui::ImageResource("app", "images/mark"), huxerui::Locale::Default());
  REQUIRE(image.PixelWidth() == 8);
  REQUIRE(image.PixelHeight() == 4);

  const std::filesystem::path reversed_output = temporary.Path() / "reversed-output";
  huxerui::resource_compiler::Merge({{override_output / "package", base_output / "package"}, reversed_output});
  DirectoryResources reversed_platform(reversed_output / "package");
  huxerui::detail::AppResources reversed_resources(&reversed_platform);
  REQUIRE(reversed_resources.Resolve(huxerui::RawResource("app", "raw/config.txt")).AsStringView() == "base");
  REQUIRE(std::filesystem::exists(reversed_output / "package" / "huxerui" / "app" / "images" / "mark.huxv"));
  REQUIRE_FALSE(std::filesystem::exists(reversed_output / "package" / "huxerui" / "app" / "images" / "mark.png"));
}

TEST_CASE("ResourceCompilerValidatesMergedResourceFamilies") {
  TemporaryDirectory temporary;
  const std::filesystem::path first_root = temporary.Path() / "first";
  const std::filesystem::path second_root = temporary.Path() / "second";
  const std::filesystem::path first_output = temporary.Path() / "first-output";
  const std::filesystem::path second_output = temporary.Path() / "second-output";
  Write(first_root / "images" / "logo.png", huxerui::test::MakeTestPng(8, 4));
  Write(second_root / "images" / "logo@2x.png", huxerui::test::MakeTestPng(18, 8));
  Write(first_root / "strings" / "default.properties", "title = {0} {1}\n");
  Write(first_root / "strings" / "zh.properties", "title = {1}\n");
  Write(second_root / "strings" / "default.properties", "title = {0}\n");
  huxerui::resource_compiler::Compile({first_root, first_output, "app"});
  huxerui::resource_compiler::Compile({second_root, second_output, "app"});

  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Merge(
          {{first_output / "package", second_output / "package"}, temporary.Path() / "merged"}
      ),
      Catch::Matchers::ContainsSubstring("image scale variants must have the same intrinsic logical size")
  );

  REQUIRE(std::filesystem::remove(second_root / "images" / "logo@2x.png"));
  huxerui::resource_compiler::Compile({second_root, second_output, "app"});
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Merge(
          {{first_output / "package", second_output / "package"}, temporary.Path() / "merged"}
      ),
      Catch::Matchers::ContainsSubstring("localized string references an undeclared argument")
  );
}

TEST_CASE("ResourceCompilerRejectsMergedPayloadHashMismatches") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "resources";
  const std::filesystem::path generated = temporary.Path() / "generated";
  Write(root / "raw" / "config.txt", "original");
  huxerui::resource_compiler::Compile({root, generated, "app"});
  Write(generated / "package" / "huxerui" / "app" / "raw" / "config.txt", "modified");

  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Merge({{generated / "package"}, temporary.Path() / "merged"}),
      Catch::Matchers::ContainsSubstring("payload hash does not match")
  );
}

TEST_CASE("ResourceCompilerRejectsUnsafeGeneratedStringLiteralsAndTruncatedImages") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  Write(root / "strings" / "default.properties", "bad\"key = value\n");
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "unsafe", "test_app"}),
      Catch::Matchers::ContainsSubstring("resource path is not package-relative")
  );

  REQUIRE(std::filesystem::remove(root / "strings" / "default.properties"));
  std::vector<std::byte> truncated = huxerui::test::MakeTestPng(2, 2);
  truncated.resize(24);
  Write(root / "images" / "broken.png", truncated);
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "truncated", "test_app"}),
      Catch::Matchers::ContainsSubstring("invalid IHDR")
  );
}

TEST_CASE("GeneratedPackagesRoundTripThroughRuntimeResolution") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  const std::filesystem::path output = temporary.Path() / "output";
  Write(root / "raw" / "config.txt", "enabled");
  const std::string unicode_name = "\xE9\x85\x8D\xE7\xBD\xAE.txt";
  Write(root / "raw" / PathFromUtf8(unicode_name), "unicode");
  Write(root / "strings" / "default.properties", "title = Hello {0}\n");
  const std::vector<std::byte> image = huxerui::test::MakeTestPng(8, 4);
  Write(root / "images" / "logo.png", image);

  huxerui::resource_compiler::Compile({root, output, "test_app"});

  DirectoryResources platform(output / "package");
  huxerui::detail::AppResources resources(&platform);
  REQUIRE(resources.Resolve(huxerui::RawResource("test_app", "raw/config.txt")).AsStringView() == "enabled");
  REQUIRE(resources.Resolve(huxerui::RawResource("test_app", "raw/" + unicode_name)).AsStringView() == "unicode");
  const huxerui::detail::ResolvedStringResource title =
      resources.Resolve(huxerui::StringResource("test_app", "strings/title"), huxerui::Locale::Default());
  REQUIRE(title.value == "Hello {0}");
  REQUIRE(title.argument_count == 1);
  const huxerui::ImageAsset logo =
      resources.Resolve(huxerui::ImageResource("test_app", "images/logo"), huxerui::Locale::Default());
  REQUIRE(logo.PixelWidth() == 8);
  REQUIRE(logo.PixelHeight() == 4);
  REQUIRE_THROWS_AS(
      resources.ResolveVector(huxerui::ImageResource("test_app", "images/logo"), huxerui::Locale::Default()),
      std::invalid_argument
  );
}

TEST_CASE("SvgResourcesCompileToTypedVectorPayloads") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  const std::filesystem::path output = temporary.Path() / "output";
  Write(
      root / "images" / "mark.svg",
      R"(<svg width="24" height="16" viewBox="0 0 24 16" xmlns="http://www.w3.org/2000/svg">
  <path d="M 2 2 L 22 2 L 12 14 Z" fill="#336699"/>
</svg>)"
  );

  huxerui::resource_compiler::Compile({root, output, "test_app"});

  const std::string header = Read(output / "include" / "test_app_resources.h");
  REQUIRE(header.find("huxerui::ImageResource mark") != std::string::npos);
  const std::string payload = Read(output / "package" / "huxerui" / "test_app" / "images" / "mark.huxv");
  REQUIRE(payload.starts_with("HUXVEC"));
  REQUIRE(payload.size() >= 12);
  REQUIRE(static_cast<unsigned char>(payload[8]) == 1U);

  DirectoryResources platform_resources(output / "package");
  huxerui::detail::AppResources resources(&platform_resources);
  const huxerui::ImageResource resource("test_app", "images/mark");
  const huxerui::VectorAsset vector = resources.ResolveVector(resource, huxerui::Locale::Default());
  REQUIRE(vector.IntrinsicSize() == huxerui::Size{24.0F, 16.0F});
  REQUIRE_THROWS_AS(resources.Resolve(resource, huxerui::Locale::Default()), std::invalid_argument);

  huxerui::test::TestPlatform platform;
  platform.platform_resources = &platform_resources;
  huxerui::test::Runtime runtime{VectorResourceApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  const huxerui::test::FlattenedScene& scene = runtime.BuildFrame();
  REQUIRE(std::ranges::any_of(scene.Commands(), [](const huxerui::PaintCommand& command) {
    return std::holds_alternative<huxerui::FillPathCommand>(command);
  }));
  REQUIRE(std::ranges::none_of(scene.Commands(), [](const huxerui::PaintCommand& command) {
    return std::holds_alternative<huxerui::DrawImageCommand>(command);
  }));
}

TEST_CASE("SvgResourcesPreserveNormalizedDashStyles") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  const std::filesystem::path output = temporary.Path() / "output";
  Write(
      root / "images" / "dashed.svg",
      R"(<svg width="24" height="16" viewBox="0 0 24 16" xmlns="http://www.w3.org/2000/svg">
  <g fill="none" stroke="#336699" stroke-width="2" stroke-linecap="round" stroke-linejoin="bevel"
     stroke-miterlimit="1" stroke-dasharray="3px, 1 2px" stroke-dashoffset="-2">
    <path d="M 2 8 L 22 8"/>
  </g>
</svg>)"
  );

  huxerui::resource_compiler::Compile({root, output, "test_app"});
  DirectoryResources platform(output / "package");
  huxerui::detail::AppResources resources(&platform);
  const huxerui::VectorAsset vector = resources.ResolveVector(
      huxerui::ImageResource("test_app", "images/dashed"),
      huxerui::Locale::Default()
  );
  huxerui::PaintSequence sequence;
  huxerui::PaintContext context(sequence, {0.0F, 0.0F, 24.0F, 16.0F});
  context.DrawImage(vector, {0.0F, 0.0F, 24.0F, 16.0F});
  context.Finish();

  const auto stroke = std::ranges::find_if(sequence.Commands(), [](const huxerui::PaintCommand& command) {
    return std::holds_alternative<huxerui::StrokePathCommand>(command);
  });
  REQUIRE(stroke != sequence.Commands().end());
  const huxerui::StrokeStyle& style = std::get<huxerui::StrokePathCommand>(*stroke).style;
  REQUIRE(style.width == 2.0F);
  REQUIRE(style.cap == huxerui::StrokeCap::Round);
  REQUIRE(style.join == huxerui::StrokeJoin::Bevel);
  REQUIRE(style.miter_limit == 1.0F);
  REQUIRE(style.dash_pattern == std::vector<float>{3.0F, 1.0F, 2.0F, 3.0F, 1.0F, 2.0F});
  REQUIRE(style.dash_offset == 10.0F);
}

TEST_CASE("SvgResourcesRejectMalformedStrokeStyles") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  const std::filesystem::path source = root / "images" / "dashed.svg";

  Write(source, R"(<svg viewBox="0 0 10 10"><path stroke="#000" stroke-dasharray="1px2px" d="M0 0L10 10"/></svg>)");
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "joined", "test_app"}),
      Catch::Matchers::ContainsSubstring("stroke-dasharray lengths must be separated")
  );

  Write(source, R"(<svg viewBox="0 0 10 10"><path stroke="#000" stroke-dasharray="2 -1" d="M0 0L10 10"/></svg>)");
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "negative", "test_app"}),
      Catch::Matchers::ContainsSubstring("stroke-dasharray lengths must be non-negative")
  );

  Write(source, R"(<svg viewBox="0 0 10 10"><path stroke="#000" stroke-dasharray=",1 2" d="M0 0L10 10"/></svg>)");
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "leading-empty", "test_app"}),
      Catch::Matchers::ContainsSubstring("stroke-dasharray must not contain empty lengths")
  );

  Write(source, R"(<svg viewBox="0 0 10 10"><path stroke="#000" stroke-dasharray="1,,2" d="M0 0L10 10"/></svg>)");
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "middle-empty", "test_app"}),
      Catch::Matchers::ContainsSubstring("stroke-dasharray must not contain empty lengths")
  );

  Write(source, R"(<svg viewBox="0 0 10 10"><path stroke="#000" stroke-dasharray="1 2," d="M0 0L10 10"/></svg>)");
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "trailing-empty", "test_app"}),
      Catch::Matchers::ContainsSubstring("stroke-dasharray must not contain empty lengths")
  );

  Write(source, R"(<svg viewBox="0 0 10 10"><path stroke="#000" stroke-miterlimit="0.5" d="M0 0L10 10"/></svg>)");
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "small-miter", "test_app"}),
      Catch::Matchers::ContainsSubstring("stroke-miterlimit must be at least one")
  );
}

TEST_CASE("SvgResourcesRejectElementsOutsideTheRootAndCompileStaticPresentationSemantics") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  const std::filesystem::path source = root / "images" / "mark.svg";

  Write(source, R"(<path d="M0 0L1 1"/><svg viewBox="0 0 10 10"/>)");
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "outside", "test_app"}),
      Catch::Matchers::ContainsSubstring("inside the root svg element")
  );

  Write(source, R"(<svg viewBox="0 0 10 10" preserveAspectRatio="none"/>)");
  REQUIRE_NOTHROW(huxerui::resource_compiler::Compile({root, temporary.Path() / "aspect", "test_app"}));

  Write(source, R"(<svg viewBox="0 0 10 10"><g visibility="hidden"/></svg>)");
  REQUIRE_NOTHROW(huxerui::resource_compiler::Compile({root, temporary.Path() / "visibility", "test_app"}));

  Write(source, R"(<svg viewBox="0 0 10 10"><g style="display: none"/></svg>)");
  REQUIRE_NOTHROW(huxerui::resource_compiler::Compile({root, temporary.Path() / "display", "test_app"}));

  Write(source, R"(<svg viewBox="0 0 10 10"><g opacity="0.5"/></svg>)");
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "opacity", "test_app"}),
      Catch::Matchers::ContainsSubstring("opacity must be exactly zero or one")
  );
}

TEST_CASE("SvgStylesAllowTrailingWhitespaceAndSmoothCurvesDoNotReflectArcControls") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  const std::filesystem::path output = temporary.Path() / "output";
  Write(
      root / "images" / "mark.svg",
      R"(<svg width="40" height="20" viewBox="0 0 40 20"><path style="fill: #fff; " d="M0 10A10 10 0 0 1 20 10S30 20 40 10"/></svg>)"
  );

  huxerui::resource_compiler::Compile({root, output, "test_app"});
  DirectoryResources platform(output / "package");
  huxerui::detail::AppResources resources(&platform);
  const huxerui::VectorAsset vector =
      resources.ResolveVector(huxerui::ImageResource("test_app", "images/mark"), huxerui::Locale::Default());
  huxerui::PaintSequence sequence;
  huxerui::PaintContext context(sequence, {0.0F, 0.0F, 40.0F, 20.0F});
  context.DrawImage(vector, {0.0F, 0.0F, 40.0F, 20.0F});
  context.Finish();

  const auto fill = std::ranges::find_if(sequence.Commands(), [](const huxerui::PaintCommand& command) {
    return std::holds_alternative<huxerui::FillPathCommand>(command);
  });
  REQUIRE(fill != sequence.Commands().end());
  const std::span<const huxerui::detail::PathElement> elements =
      huxerui::detail::PathAccess::Elements(std::get<huxerui::FillPathCommand>(*fill).path);
  REQUIRE_FALSE(elements.empty());
  REQUIRE(elements.back().verb == huxerui::detail::PathVerb::CubicTo);
  REQUIRE(elements.back().points[0] == huxerui::Point{20.0F, 10.0F});
}

TEST_CASE("SvgResourcesExpandForwardUseReferencesAndCompilePathClips") {
  TemporaryDirectory temporary;
  const huxerui::VectorAsset vector = CompileVectorResource(
      temporary.Path(),
      R"svg(<svg width="20" height="10" viewBox="0 0 20 10" color="#336699" xmlns:xlink="http://www.w3.org/1999/xlink">
  <defs>
    <clipPath id="bounds" clip-rule="nonzero"><rect x="0" y="0" width="8" height="8"/></clipPath>
  </defs>
  <use href="#mark" x="2" clip-path="url(#bounds)"/>
  <use xlink:href="#mark" x="4"/>
  <path id="mark" d="M0 0L10 0L10 10Z" fill="red" style="fill: currentColor; fill-rule: evenodd"/>
</svg>)svg"
  );

  const std::vector<huxerui::PaintCommand>& commands = huxerui::detail::VectorAccess::Sequence(vector).Commands();
  REQUIRE(std::ranges::count_if(commands, [](const huxerui::PaintCommand& command) {
    return std::holds_alternative<huxerui::FillPathCommand>(command);
  }) == 3);
  REQUIRE(std::ranges::count_if(commands, [](const huxerui::PaintCommand& command) {
    return std::holds_alternative<huxerui::PushPathClipCommand>(command);
  }) == 1);
  const auto fill = std::ranges::find_if(commands, [](const huxerui::PaintCommand& command) {
    return std::holds_alternative<huxerui::FillPathCommand>(command);
  });
  REQUIRE(fill != commands.end());
  REQUIRE(std::get<huxerui::FillPathCommand>(*fill).color == huxerui::Color::Rgb(51, 102, 153));
  REQUIRE(std::get<huxerui::FillPathCommand>(*fill).fill_rule == huxerui::PathFillRule::EvenOdd);
  const auto clip = std::ranges::find_if(commands, [](const huxerui::PaintCommand& command) {
    return std::holds_alternative<huxerui::PushPathClipCommand>(command);
  });
  REQUIRE(clip != commands.end());
  REQUIRE(std::get<huxerui::PushPathClipCommand>(*clip).fill_rule == huxerui::PathFillRule::NonZero);
}

TEST_CASE("SvgResourcesValidateFileLocalReferenceIdentity") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  const std::filesystem::path source = root / "images" / "mark.svg";

  Write(source, R"(<svg viewBox="0 0 10 10"><use href="#missing"/></svg>)");
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "missing", "test_app"}),
      Catch::Matchers::ContainsSubstring("references a missing id: missing")
  );

  Write(source, R"(<svg viewBox="0 0 10 10"><path id="same" d="M0 0"/><g id="same"/></svg>)");
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "duplicate", "test_app"}),
      Catch::Matchers::ContainsSubstring("duplicate id: same")
  );

  Write(
      source,
      R"(<svg viewBox="0 0 10 10"><g id="a"><use href="#b"/></g><g id="b"><use href="#a"/></g><use href="#a"/></svg>)"
  );
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "cycle", "test_app"}),
      Catch::Matchers::ContainsSubstring("references form a cycle")
  );

  Write(
      source,
      R"svg(<svg viewBox="0 0 10 10"><clipPath id="clip"><rect width="2" height="2"/></clipPath>
  <path clip-path="url(#missing)" d="M0 0L1 1"/>
</svg>)svg"
  );
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "missing-clip", "test_app"}),
      Catch::Matchers::ContainsSubstring("clip-path references a missing id")
  );
}

TEST_CASE("SvgResourcesCompileAspectRatioVisibilityUnitsColorsAndSkew") {
  TemporaryDirectory temporary;
  const huxerui::VectorAsset vector = CompileVectorResource(
      temporary.Path(),
      R"svg(<svg width="1in" height="2.54cm" viewBox="0 0 10 5">
  <g visibility="hidden"><rect width="1" height="1"/><rect visibility="visible" width="2" height="2" fill="#0f08"/></g>
  <path display="none" d="M0 0L1 1"/>
  <path transform="skewX(45)" fill="rgba(255, 0, 0, 50%)" d="M0 0L1 0L1 1Z"/>
</svg>)svg"
  );

  REQUIRE(vector.IntrinsicSize() == huxerui::Size{96.0F, 96.0F});
  const std::vector<huxerui::PaintCommand>& commands = huxerui::detail::VectorAccess::Sequence(vector).Commands();
  REQUIRE(std::ranges::count_if(commands, [](const huxerui::PaintCommand& command) {
    return std::holds_alternative<huxerui::FillPathCommand>(command);
  }) == 2);
  const auto skew = std::ranges::find_if(commands, [](const huxerui::PaintCommand& command) {
    return std::holds_alternative<huxerui::PushTransformCommand>(command) &&
           std::get<huxerui::PushTransformCommand>(command).transform.m21 > 0.99F;
  });
  REQUIRE(skew != commands.end());
  const auto aspect = std::ranges::find_if(commands, [](const huxerui::PaintCommand& command) {
    if (!std::holds_alternative<huxerui::PushTransformCommand>(command)) {
      return false;
    }
    const huxerui::Transform2D transform = std::get<huxerui::PushTransformCommand>(command).transform;
    return std::abs(transform.m11 - 1.0F) < 0.001F && std::abs(transform.m22 - 0.5F) < 0.001F;
  });
  REQUIRE(aspect != commands.end());
}

TEST_CASE("SvgResourcesCompileNoneAndSliceAspectRatioModes") {
  TemporaryDirectory temporary;
  const huxerui::VectorAsset unscaled = CompileVectorResource(
      temporary.Path(),
      R"svg(<svg width="20" height="20" viewBox="0 0 10 5" preserveAspectRatio="none">
  <rect width="10" height="5"/>
</svg>)svg",
      "aspect-none"
  );
  REQUIRE(std::ranges::none_of(
      huxerui::detail::VectorAccess::Sequence(unscaled).Commands(),
      [](const huxerui::PaintCommand& command) {
        return std::holds_alternative<huxerui::PushTransformCommand>(command);
      }
  ));

  const huxerui::VectorAsset sliced = CompileVectorResource(
      temporary.Path(),
      R"svg(<svg width="20" height="20" viewBox="0 0 10 5" preserveAspectRatio="xMidYMid slice">
  <rect width="10" height="5"/>
</svg>)svg",
      "aspect-slice"
  );
  const auto transform = std::ranges::find_if(
      huxerui::detail::VectorAccess::Sequence(sliced).Commands(),
      [](const huxerui::PaintCommand& command) {
        if (!std::holds_alternative<huxerui::PushTransformCommand>(command)) {
          return false;
        }
        const huxerui::Transform2D value = std::get<huxerui::PushTransformCommand>(command).transform;
        return value.m11 == 2.0F && value.m22 == 1.0F && value.translate_x == -5.0F;
      }
  );
  REQUIRE(transform != huxerui::detail::VectorAccess::Sequence(sliced).Commands().end());
}

TEST_CASE("SvgResourcesCompileGradientPathFillsInHuxvecVersionOne") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "resources";
  const std::filesystem::path output = temporary.Path() / "output";
  Write(
      root / "images" / "gradient.svg",
      R"svg(<svg width="40" height="20" viewBox="0 0 40 20" color="#00ff00" xmlns:xlink="http://www.w3.org/1999/xlink">
  <defs>
    <linearGradient id="linear" xlink:href="#base" x1="25%" x2="75%"/>
    <linearGradient id="base"><stop offset="0" stop-color="currentColor" stop-opacity="0.5"/>
      <stop offset="100%" style="stop-color: #0000ff"/></linearGradient>
    <radialGradient id="radial" gradientUnits="userSpaceOnUse" cx="20" cy="10" r="5">
      <stop stop-color="white"/><stop offset="1" stop-color="black"/>
    </radialGradient>
  </defs>
  <path fill="url(#linear)" fill-opacity="0.5" fill-rule="evenodd" d="M5 2C5 18 25 18 25 2Z"/>
  <rect x="28" y="2" width="10" height="16" fill="url(#radial)"/>
</svg>)svg"
  );

  huxerui::resource_compiler::Compile({root, output, "test_app"});
  const std::string payload = Read(output / "package" / "huxerui" / "test_app" / "images" / "gradient.huxv");
  REQUIRE(payload.starts_with("HUXVEC"));
  REQUIRE(static_cast<unsigned char>(payload[8]) == 1U);
  REQUIRE(static_cast<unsigned char>(payload[40]) == 7U);
  std::vector<std::byte> truncated(payload.size() - 1);
  std::memcpy(truncated.data(), payload.data(), truncated.size());
  REQUIRE_THROWS_AS(
      huxerui::detail::ResourceAccess::VectorFromRaw(huxerui::RawAsset::FromBytes(std::move(truncated))),
      std::logic_error
  );

  DirectoryResources platform(output / "package");
  huxerui::detail::AppResources resources(&platform);
  const huxerui::VectorAsset vector = resources.ResolveVector(
      huxerui::ImageResource("test_app", "images/gradient"), huxerui::Locale::Default()
  );
  const std::vector<huxerui::PaintCommand>& commands = huxerui::detail::VectorAccess::Sequence(vector).Commands();
  const auto linear = std::ranges::find_if(commands, [](const huxerui::PaintCommand& command) {
    return std::holds_alternative<huxerui::FillLinearGradientPathCommand>(command);
  });
  const auto radial = std::ranges::find_if(commands, [](const huxerui::PaintCommand& command) {
    return std::holds_alternative<huxerui::FillRadialGradientPathCommand>(command);
  });
  REQUIRE(linear != commands.end());
  REQUIRE(radial != commands.end());
  const auto& linear_fill = std::get<huxerui::FillLinearGradientPathCommand>(*linear);
  REQUIRE(linear_fill.fill_rule == huxerui::PathFillRule::EvenOdd);
  REQUIRE(linear_fill.gradient_rect == huxerui::Rect{5.0F, 2.0F, 20.0F, 12.0F});
  REQUIRE(linear_fill.gradient.start == huxerui::Point{0.25F, 0.0F});
  REQUIRE(linear_fill.gradient.end == huxerui::Point{0.75F, 0.0F});
  REQUIRE(linear_fill.gradient.transform.IsIdentity());
  REQUIRE(linear_fill.gradient.stops[0].color == huxerui::Color::Rgb(0, 255, 0, 0.25F));
  const auto& radial_fill = std::get<huxerui::FillRadialGradientPathCommand>(*radial);
  REQUIRE(radial_fill.gradient_rect == huxerui::Rect{0.0F, 0.0F, 40.0F, 20.0F});
  REQUIRE(radial_fill.gradient.center == huxerui::Point{0.5F, 0.5F});
  REQUIRE(radial_fill.gradient.radius == huxerui::Size{0.125F, 0.25F});
  REQUIRE(radial_fill.gradient.transform.IsIdentity());
}

TEST_CASE("SvgResourcesCompileInheritedObjectAndUserSpaceGradientTransforms") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "resources";
  const std::filesystem::path output = temporary.Path() / "output";
  Write(
      root / "images" / "transformed.svg",
      R"svg(<svg width="40" height="20" viewBox="0 0 40 20">
  <defs>
    <linearGradient id="base" gradientTransform="matrix(1 0.25 -0.5 1 0.2 -0.1)">
      <stop stop-color="black"/><stop offset="1" stop-color="white"/>
    </linearGradient>
    <linearGradient id="inherited" href="#base"/>
    <linearGradient id="overridden" href="#base" gradientTransform="translate(0.25 0.5)"/>
    <radialGradient id="user" gradientUnits="userSpaceOnUse"
        gradientTransform="matrix(1 0.25 -0.5 1 4 2)" cx="20" cy="10" r="5">
      <stop stop-color="white"/><stop offset="1" stop-color="black"/>
    </radialGradient>
  </defs>
  <rect width="10" height="10" fill="url(#inherited)"/>
  <rect x="10" width="10" height="10" fill="url(#overridden)"/>
  <rect x="20" width="10" height="10" fill="url(#user)"/>
</svg>)svg"
  );

  huxerui::resource_compiler::Compile({root, output, "test_app"});
  const std::string payload = Read(output / "package" / "huxerui" / "test_app" / "images" / "transformed.huxv");
  REQUIRE(static_cast<unsigned char>(payload[40]) == 7U);
  std::vector<std::byte> truncated(payload.size() - 1);
  std::memcpy(truncated.data(), payload.data(), truncated.size());
  REQUIRE_THROWS_AS(
      huxerui::detail::ResourceAccess::VectorFromRaw(huxerui::RawAsset::FromBytes(std::move(truncated))),
      std::logic_error
  );

  DirectoryResources platform(output / "package");
  huxerui::detail::AppResources resources(&platform);
  const huxerui::VectorAsset vector = resources.ResolveVector(
      huxerui::ImageResource("test_app", "images/transformed"), huxerui::Locale::Default()
  );
  const std::vector<huxerui::PaintCommand>& commands = huxerui::detail::VectorAccess::Sequence(vector).Commands();
  REQUIRE(commands.size() == 3);
  const auto& inherited = std::get<huxerui::FillLinearGradientPathCommand>(commands[0]).gradient.transform;
  REQUIRE(inherited == huxerui::Transform2D{1.0F, 0.25F, -0.5F, 1.0F, 0.2F, -0.1F});
  const auto& overridden = std::get<huxerui::FillLinearGradientPathCommand>(commands[1]).gradient.transform;
  REQUIRE(overridden == huxerui::Transform2D{1.0F, 0.0F, 0.0F, 1.0F, 0.25F, 0.5F});
  const auto& user = std::get<huxerui::FillRadialGradientPathCommand>(commands[2]).gradient.transform;
  REQUIRE(user.m11 == Catch::Approx(1.0F));
  REQUIRE(user.m12 == Catch::Approx(0.5F));
  REQUIRE(user.m21 == Catch::Approx(-0.25F));
  REQUIRE(user.m22 == Catch::Approx(1.0F));
  REQUIRE(user.translate_x == Catch::Approx(0.1F));
  REQUIRE(user.translate_y == Catch::Approx(0.1F));
}

TEST_CASE("SvgResourcesCompileGradientPathStrokesInHuxvecVersionOne") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "resources";
  const std::filesystem::path output = temporary.Path() / "output";
  Write(
      root / "images" / "gradient_stroke.svg",
      R"svg(<svg width="40" height="20" viewBox="0 0 40 20">
  <defs>
    <linearGradient id="base" gradientTransform="matrix(1 0.25 -0.5 1 0.2 -0.1)">
      <stop stop-color="black" stop-opacity="0.5"/><stop offset="1" stop-color="white"/>
    </linearGradient>
    <linearGradient id="inherited" href="#base"/>
    <radialGradient id="radial"><stop stop-color="white"/><stop offset="1" stop-color="black"/></radialGradient>
  </defs>
  <g fill="none" stroke-width="3" stroke-linecap="round" stroke-linejoin="bevel"
     stroke-dasharray="3 1 2" stroke-dashoffset="-2">
    <rect x="5" y="2" width="20" height="12" stroke="url(#inherited)" stroke-opacity="0.5"/>
    <path d="M25 2L38 2L32 16Z" stroke="url(#radial)"/>
  </g>
</svg>)svg"
  );

  huxerui::resource_compiler::Compile({root, output, "test_app"});
  const std::string payload =
      Read(output / "package" / "huxerui" / "test_app" / "images" / "gradient_stroke.huxv");
  REQUIRE(payload.starts_with("HUXVEC"));
  REQUIRE(static_cast<unsigned char>(payload[8]) == 1U);
  REQUIRE(static_cast<unsigned char>(payload[40]) == 9U);
  std::vector<std::byte> truncated(payload.size() - 1);
  std::memcpy(truncated.data(), payload.data(), truncated.size());
  REQUIRE_THROWS_AS(
      huxerui::detail::ResourceAccess::VectorFromRaw(huxerui::RawAsset::FromBytes(std::move(truncated))),
      std::logic_error
  );

  DirectoryResources platform(output / "package");
  huxerui::detail::AppResources resources(&platform);
  const huxerui::VectorAsset vector = resources.ResolveVector(
      huxerui::ImageResource("test_app", "images/gradient_stroke"), huxerui::Locale::Default()
  );
  const std::vector<huxerui::PaintCommand>& commands = huxerui::detail::VectorAccess::Sequence(vector).Commands();
  REQUIRE(commands.size() == 2);
  const auto& linear = std::get<huxerui::StrokeLinearGradientPathCommand>(commands[0]);
  REQUIRE(linear.gradient_rect == huxerui::Rect{5.0F, 2.0F, 20.0F, 12.0F});
  REQUIRE(linear.gradient.transform == huxerui::Transform2D{1.0F, 0.25F, -0.5F, 1.0F, 0.2F, -0.1F});
  REQUIRE(linear.gradient.stops[0].color == huxerui::Color::Rgb(0, 0, 0, 0.25F));
  REQUIRE(linear.style.width == 3.0F);
  REQUIRE(linear.style.cap == huxerui::StrokeCap::Round);
  REQUIRE(linear.style.join == huxerui::StrokeJoin::Bevel);
  REQUIRE(linear.style.dash_pattern == std::vector<float>{3.0F, 1.0F, 2.0F, 3.0F, 1.0F, 2.0F});
  REQUIRE(linear.style.dash_offset == 10.0F);
  const auto& radial = std::get<huxerui::StrokeRadialGradientPathCommand>(commands[1]);
  REQUIRE(radial.gradient_rect == huxerui::Rect{25.0F, 2.0F, 13.0F, 14.0F});
  REQUIRE(radial.gradient.transform.IsIdentity());
  REQUIRE(radial.style == linear.style);
}

TEST_CASE("SvgResourcesRejectUnsupportedAndCyclicGradients") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "resources";
  const std::filesystem::path source = root / "images" / "gradient.svg";

  Write(source, R"svg(<svg viewBox="0 0 10 10"><defs><linearGradient id="a" href="#b"/>
  <linearGradient id="b" href="#a"/></defs><rect width="10" height="10" fill="url(#a)"/></svg>)svg");
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "cycle", "test_app"}),
      Catch::Matchers::ContainsSubstring("gradient references form a cycle")
  );

  Write(source, R"svg(<svg viewBox="0 0 10 10"><radialGradient id="paint" fx="25%">
  <stop/><stop offset="1"/></radialGradient><rect width="10" height="10" fill="url(#paint)"/></svg>)svg");
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "focal", "test_app"}),
      Catch::Matchers::ContainsSubstring("non-concentric radial gradients are not supported")
  );

  Write(source, R"svg(<svg viewBox="0 0 10 10"><linearGradient id="paint" spreadMethod="repeat">
  <stop/><stop offset="1"/></linearGradient><rect width="10" height="10" fill="url(#paint)"/></svg>)svg");
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "repeat", "test_app"}),
      Catch::Matchers::ContainsSubstring("spreadMethod supports only pad")
  );
}

TEST_CASE("SvgResourcesRejectInexactClipAndPresentationFeatures") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  const std::filesystem::path source = root / "images" / "mark.svg";

  Write(
      source,
      R"svg(<svg viewBox="0 0 10 10">
  <clipPath id="clip" clipPathUnits="objectBoundingBox"><path d="M0 0L1 1"/></clipPath>
  <path clip-path="url(#clip)" d="M0 0L1 1"/>
</svg>)svg"
  );
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "clip-units", "test_app"}),
      Catch::Matchers::ContainsSubstring("supports only userSpaceOnUse")
  );

  Write(
      source,
      R"svg(<svg viewBox="0 0 10 10">
  <clipPath id="clip"><rect width="2" height="2"/><circle r="1"/></clipPath>
  <path clip-path="url(#clip)" d="M0 0L1 1"/>
</svg>)svg"
  );
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "compound-clip", "test_app"}),
      Catch::Matchers::ContainsSubstring("must resolve to one drawable path")
  );

  Write(source, R"svg(<svg viewBox="0 0 10 10"><linearGradient id="paint" gradientTransform="scale(0)">
  <stop/><stop offset="1"/></linearGradient><path fill="url(#paint)" d="M0 0L1 1"/></svg>)svg");
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "gradient", "test_app"}),
      Catch::Matchers::ContainsSubstring("gradientTransform must be finite and invertible")
  );

  Write(source, R"svg(<svg viewBox="0 0 10 10"><defs><path style="filter: blur(1px)" d="M0 0L1 1"/></defs></svg>)svg");
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "unused-style", "test_app"}),
      Catch::Matchers::ContainsSubstring("unsupported style property: filter")
  );

  Write(
      source,
      R"svg(<svg viewBox="0 0 10 10"><path transform="scale(3e38) scale(3e38)" d="M0 0L1 1"/></svg>)svg"
  );
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "overflowing-transform", "test_app"}),
      Catch::Matchers::ContainsSubstring("transform must remain finite")
  );
}

TEST_CASE("ResourceCompilerRejectsUnsupportedSvgFeaturesAndDensityVariants") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  Write(
      root / "images" / "filtered.svg",
      R"svg(<svg viewBox="0 0 10 10"><path filter="url(#x)" d="M0 0L1 1"/></svg>)svg"
  );
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "filtered", "test_app"}),
      Catch::Matchers::ContainsSubstring("unsupported attribute: filter")
  );

  REQUIRE(std::filesystem::remove(root / "images" / "filtered.svg"));
  Write(root / "images" / "mark@2x.svg", R"(<svg viewBox="0 0 10 10"><path d="M0 0L1 1"/></svg>)");
  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "scaled", "test_app"}),
      Catch::Matchers::ContainsSubstring("do not support density suffixes")
  );
}

TEST_CASE("ResourceCompilerRejectsMixedRasterAndVectorVariants") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  Write(root / "images" / "mark.svg", R"(<svg viewBox="0 0 8 4"><path d="M0 0L8 4"/></svg>)");
  Write(root / "images" / "mark@2x.png", huxerui::test::MakeTestPng(16, 8));

  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "output", "test_app"}),
      Catch::Matchers::ContainsSubstring("raster and vector variants must not share")
  );
}

TEST_CASE("ResourceCompilerRejectsMalformedPathDataAfterClose") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  Write(root / "images" / "mark.svg", R"(<svg viewBox="0 0 8 4"><path d="M0 0Z 1"/></svg>)");

  REQUIRE_THROWS_WITH(
      huxerui::resource_compiler::Compile({root, temporary.Path() / "output", "test_app"}),
      Catch::Matchers::ContainsSubstring("path data is malformed")
  );
}

} // namespace
