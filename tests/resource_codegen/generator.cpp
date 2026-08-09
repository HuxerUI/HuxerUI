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

#include "generator.h"
#include "image_test_support.h"
#include "path_internal.h"
#include "resource_internal.h"
#include "runtime_test_support.h"

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("hapt-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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

TEST_CASE("ResourceCodegenGeneratesTypedKeysIndexAndPayloads") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  const std::filesystem::path output = temporary.Path() / "output";
  Write(root / "raw" / "config.json", "{\"enabled\":true}\n");
  Write(root / "strings" / "default.properties", "title = \"Hello {0}\"\n");
  Write(root / "strings" / "zh.properties", "title = \"你好，{0}\"\n");

  huxerui::resource_codegen::Generate({root, output, "test_app"});

  const std::string header = Read(output / "include" / "test_app_resources.h");
  REQUIRE(header.find("huxerui::RawResource config_json") != std::string::npos);
  REQUIRE(header.find("huxerui::StringResource title") != std::string::npos);
  REQUIRE(Read(output / "package" / "huxerui" / "test_app" / "raw" / "config.json") == "{\"enabled\":true}\n");
  REQUIRE(std::filesystem::file_size(output / "package" / "huxerui" / "resources.bin") > 16);
}

TEST_CASE("ResourceCodegenAcceptsAnEmptyStringCatalog") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  const std::filesystem::path output = temporary.Path() / "output";
  Write(root / "strings" / "default.properties", "");

  huxerui::resource_codegen::Generate({root, output, "test_app"});

  REQUIRE(std::filesystem::exists(output / "include" / "test_app_resources.h"));
  REQUIRE(std::filesystem::file_size(output / "package" / "huxerui" / "resources.bin") == 16);
}

TEST_CASE("ResourceCodegenRejectsInvalidMessagePlaceholders") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  Write(root / "strings" / "default.properties", "title = \"Hello {name}\"\n");

  REQUIRE_THROWS_WITH(
      huxerui::resource_codegen::Generate({root, temporary.Path() / "output", "test_app"}),
      "localized string contains an invalid positional placeholder"
  );
}

TEST_CASE("ResourceCodegenRejectsLegacyTextCatalogs") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  Write(root / "strings" / "default.txt", "title = Legacy\n");

  REQUIRE_THROWS_WITH(
      huxerui::resource_codegen::Generate({root, temporary.Path() / "output", "test_app"}),
      Catch::Matchers::ContainsSubstring("string catalog must use the .properties extension")
  );
}

TEST_CASE("ResourceCodegenRejectsGeneratedIdentifierCollisions") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  Write(root / "raw" / "a-b.txt", "first");
  Write(root / "raw" / "a_b.txt", "second");

  REQUIRE_THROWS_WITH(
      huxerui::resource_codegen::Generate({root, temporary.Path() / "output", "test_app"}),
      "resource keys generate the same C++ identifier: raw/a-b.txt and raw/a_b.txt"
  );
}

TEST_CASE("ResourceCodegenEscapesReservedGeneratedIdentifiers") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  const std::filesystem::path output = temporary.Path() / "output";
  Write(root / "raw" / "class", "reserved");

  huxerui::resource_codegen::Generate({root, output, "test_app"});

  REQUIRE(Read(output / "include" / "test_app_resources.h").find("resource_class") != std::string::npos);
}

TEST_CASE("ResourceCodegenRejectsReservedNamespacesAndInvalidKeys") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  Write(root / "raw" / "config.txt", "value");
  REQUIRE_THROWS_WITH(
      huxerui::resource_codegen::Generate({root, temporary.Path() / "reserved", "class"}),
      "resource namespace must be a non-reserved C++ identifier"
  );

  Write(root / "strings" / "default.properties", "bad:key = value\n");
  REQUIRE_THROWS_WITH(
      huxerui::resource_codegen::Generate({root, temporary.Path() / "invalid-key", "test_app"}),
      Catch::Matchers::ContainsSubstring("resource path is not package-relative")
  );

  REQUIRE_THROWS_WITH(
      huxerui::resource_codegen::Generate({root, temporary.Path() / "invalid-namespace", "test_app_"}),
      "resource namespace must be a non-reserved C++ identifier"
  );
}

TEST_CASE("ResourceCodegenRemovesStalePackagedPayloads") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  const std::filesystem::path output = temporary.Path() / "output";
  const std::filesystem::path old_source = root / "raw" / "old.txt";
  const std::filesystem::path old_output = output / "package" / "huxerui" / "test_app" / "raw" / "old.txt";
  Write(old_source, "old");
  huxerui::resource_codegen::Generate({root, output, "test_app"});
  REQUIRE(std::filesystem::exists(old_output));

  REQUIRE(std::filesystem::remove(old_source));
  Write(root / "raw" / "new.txt", "new");
  huxerui::resource_codegen::Generate({root, output, "test_app"});

  REQUIRE_FALSE(std::filesystem::exists(old_output));
  REQUIRE(Read(output / "package" / "huxerui" / "test_app" / "raw" / "new.txt") == "new");
}

TEST_CASE("ResourceCodegenRemovesOutputsFromThePreviousNamespace") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  const std::filesystem::path output = temporary.Path() / "output";
  Write(root / "raw" / "value.txt", "value");

  huxerui::resource_codegen::Generate({root, output, "old_app"});
  REQUIRE(std::filesystem::exists(output / "include" / "old_app_resources.h"));
  REQUIRE(std::filesystem::exists(output / "package" / "huxerui" / "old_app"));

  huxerui::resource_codegen::Generate({root, output, "new_app"});
  REQUIRE_FALSE(std::filesystem::exists(output / "include" / "old_app_resources.h"));
  REQUIRE_FALSE(std::filesystem::exists(output / "package" / "huxerui" / "old_app"));
  REQUIRE(std::filesystem::exists(output / "include" / "new_app_resources.h"));
}

TEST_CASE("ResourceCodegenRejectsUnsafeGeneratedStringLiteralsAndTruncatedImages") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  Write(root / "strings" / "default.properties", "bad\"key = value\n");
  REQUIRE_THROWS_WITH(
      huxerui::resource_codegen::Generate({root, temporary.Path() / "unsafe", "test_app"}),
      Catch::Matchers::ContainsSubstring("resource path is not package-relative")
  );

  REQUIRE(std::filesystem::remove(root / "strings" / "default.properties"));
  std::vector<std::byte> truncated = huxerui::test::MakeTestPng(2, 2);
  truncated.resize(24);
  Write(root / "images" / "broken.png", truncated);
  REQUIRE_THROWS_WITH(
      huxerui::resource_codegen::Generate({root, temporary.Path() / "truncated", "test_app"}),
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

  huxerui::resource_codegen::Generate({root, output, "test_app"});

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

  huxerui::resource_codegen::Generate({root, output, "test_app"});

  const std::string header = Read(output / "include" / "test_app_resources.h");
  REQUIRE(header.find("huxerui::ImageResource mark") != std::string::npos);
  const std::string payload = Read(output / "package" / "huxerui" / "test_app" / "images" / "mark.huxv");
  REQUIRE(payload.starts_with("HUXVEC"));

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

TEST_CASE("SvgResourcesRejectElementsOutsideTheRootAndUnsupportedPresentationSemantics") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  const std::filesystem::path source = root / "images" / "mark.svg";

  Write(source, R"(<path d="M0 0L1 1"/><svg viewBox="0 0 10 10"/>)");
  REQUIRE_THROWS_WITH(
      huxerui::resource_codegen::Generate({root, temporary.Path() / "outside", "test_app"}),
      Catch::Matchers::ContainsSubstring("inside the root svg element")
  );

  Write(source, R"(<svg viewBox="0 0 10 10" preserveAspectRatio="none"/>)");
  REQUIRE_THROWS_WITH(
      huxerui::resource_codegen::Generate({root, temporary.Path() / "aspect", "test_app"}),
      Catch::Matchers::ContainsSubstring("unsupported attribute: preserveAspectRatio")
  );

  Write(source, R"(<svg viewBox="0 0 10 10"><g visibility="hidden"/></svg>)");
  REQUIRE_THROWS_WITH(
      huxerui::resource_codegen::Generate({root, temporary.Path() / "visibility", "test_app"}),
      Catch::Matchers::ContainsSubstring("unsupported attribute: visibility")
  );

  Write(source, R"(<svg viewBox="0 0 10 10"><g style="display: none"/></svg>)");
  REQUIRE_THROWS_WITH(
      huxerui::resource_codegen::Generate({root, temporary.Path() / "display", "test_app"}),
      Catch::Matchers::ContainsSubstring("unsupported style property: display")
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

  huxerui::resource_codegen::Generate({root, output, "test_app"});
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

TEST_CASE("ResourceCodegenRejectsUnsupportedSvgFeaturesAndDensityVariants") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  Write(
      root / "images" / "filtered.svg",
      R"svg(<svg viewBox="0 0 10 10"><path filter="url(#x)" d="M0 0L1 1"/></svg>)svg"
  );
  REQUIRE_THROWS_WITH(
      huxerui::resource_codegen::Generate({root, temporary.Path() / "filtered", "test_app"}),
      Catch::Matchers::ContainsSubstring("unsupported attribute: filter")
  );

  REQUIRE(std::filesystem::remove(root / "images" / "filtered.svg"));
  Write(root / "images" / "mark@2x.svg", R"(<svg viewBox="0 0 10 10"><path d="M0 0L1 1"/></svg>)");
  REQUIRE_THROWS_WITH(
      huxerui::resource_codegen::Generate({root, temporary.Path() / "scaled", "test_app"}),
      Catch::Matchers::ContainsSubstring("do not support density suffixes")
  );
}

TEST_CASE("ResourceCodegenRejectsMixedRasterAndVectorVariants") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  Write(root / "images" / "mark.svg", R"(<svg viewBox="0 0 8 4"><path d="M0 0L8 4"/></svg>)");
  Write(root / "images" / "mark@2x.png", huxerui::test::MakeTestPng(16, 8));

  REQUIRE_THROWS_WITH(
      huxerui::resource_codegen::Generate({root, temporary.Path() / "output", "test_app"}),
      Catch::Matchers::ContainsSubstring("raster and vector variants must not share")
  );
}

TEST_CASE("ResourceCodegenRejectsMalformedPathDataAfterClose") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "assets";
  Write(root / "images" / "mark.svg", R"(<svg viewBox="0 0 8 4"><path d="M0 0Z 1"/></svg>)");

  REQUIRE_THROWS_WITH(
      huxerui::resource_codegen::Generate({root, temporary.Path() / "output", "test_app"}),
      Catch::Matchers::ContainsSubstring("path data is malformed")
  );
}

} // namespace
