#include "ios_project.h"

#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

#include "project.h"

namespace huxerui::cli {
namespace {

void ReplaceAll(std::string& value, std::string_view token, std::string_view replacement) {
  std::size_t offset = 0;
  while ((offset = value.find(token, offset)) != std::string::npos) {
    value.replace(offset, token.size(), replacement);
    offset += replacement.size();
  }
}

std::string EscapePbxString(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    switch (character) {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      break;
    default:
      escaped += character;
      break;
    }
  }
  return escaped;
}

void SkipJsonWhitespace(std::string_view json, std::size_t& offset) {
  while (offset < json.size() && std::isspace(static_cast<unsigned char>(json[offset]))) {
    ++offset;
  }
}

void ExpectJsonCharacter(std::string_view json, std::size_t& offset, char expected) {
  SkipJsonWhitespace(json, offset);
  if (offset >= json.size() || json[offset] != expected) {
    throw std::runtime_error("HuxerUI iOS module graph is malformed");
  }
  ++offset;
}

std::string ParseJsonString(std::string_view json, std::size_t& offset) {
  SkipJsonWhitespace(json, offset);
  if (offset >= json.size() || json[offset++] != '"') {
    throw std::runtime_error("HuxerUI iOS module graph contains a non-string value");
  }

  std::string value;
  while (offset < json.size()) {
    const char character = json[offset++];
    if (character == '"') {
      return value;
    }
    if (character != '\\') {
      if (static_cast<unsigned char>(character) < 0x20) {
        throw std::runtime_error("HuxerUI iOS module graph contains an unescaped control character");
      }
      value += character;
      continue;
    }
    if (offset >= json.size()) {
      break;
    }
    switch (json[offset++]) {
    case '"':
      value += '"';
      break;
    case '\\':
      value += '\\';
      break;
    case '/':
      value += '/';
      break;
    case 'b':
      value += '\b';
      break;
    case 'f':
      value += '\f';
      break;
    case 'n':
      value += '\n';
      break;
    case 'r':
      value += '\r';
      break;
    case 't':
      value += '\t';
      break;
    default:
      throw std::runtime_error("HuxerUI iOS module graph contains an unsupported string escape");
    }
  }
  throw std::runtime_error("HuxerUI iOS module graph contains an unterminated string");
}

void ExpectJsonKey(std::string_view json, std::size_t& offset, std::string_view expected) {
  if (ParseJsonString(json, offset) != expected) {
    throw std::runtime_error("HuxerUI iOS module graph contains an unexpected field");
  }
  ExpectJsonCharacter(json, offset, ':');
}

struct IosModulePackage {
  std::string target;
  std::filesystem::path path;
  std::string product;
};

std::string ModuleProductName(std::string_view target) {
  const std::size_t separator = target.rfind("::");
  if (separator == std::string_view::npos) {
    return MakeModuleProductName(target);
  }
  const std::string_view product = target.substr(separator + 2);
  if (product.empty()) {
    throw std::runtime_error("HuxerUI iOS module target has an empty product name: " + std::string(target));
  }
  return std::string(product);
}

std::vector<IosModulePackage> ParseIosModulePackages(std::string_view json) {
  std::size_t offset = 0;
  ExpectJsonCharacter(json, offset, '{');
  ExpectJsonKey(json, offset, "schema");
  SkipJsonWhitespace(json, offset);
  if (offset >= json.size() || json[offset++] != '1') {
    throw std::runtime_error("HuxerUI iOS module graph has an unsupported schema");
  }
  ExpectJsonCharacter(json, offset, ',');
  ExpectJsonKey(json, offset, "modules");
  ExpectJsonCharacter(json, offset, '[');

  std::vector<IosModulePackage> modules;
  std::set<std::string> targets;
  std::set<std::string> products;
  SkipJsonWhitespace(json, offset);
  while (offset < json.size() && json[offset] != ']') {
    ExpectJsonCharacter(json, offset, '{');
    ExpectJsonKey(json, offset, "target");
    std::string target = ParseJsonString(json, offset);
    ExpectJsonCharacter(json, offset, ',');
    ExpectJsonKey(json, offset, "sourceRoot");
    const std::filesystem::path source_root = ParseJsonString(json, offset);
    ExpectJsonCharacter(json, offset, '}');

    const std::filesystem::path package = source_root / "platform/ios";
    if (std::filesystem::is_directory(package)) {
      if (!std::filesystem::is_regular_file(package / "Package.swift")) {
        throw std::runtime_error("HuxerUI iOS module package is missing Package.swift: " + package.string());
      }
      std::string product = ModuleProductName(target);
      if (!targets.insert(target).second) {
        throw std::runtime_error("HuxerUI iOS module target is duplicated: " + target);
      }
      if (!products.insert(product).second) {
        throw std::runtime_error("HuxerUI iOS module product is duplicated: " + product);
      }
      modules.push_back({std::move(target), package, std::move(product)});
    }

    SkipJsonWhitespace(json, offset);
    if (offset < json.size() && json[offset] == ',') {
      ++offset;
      SkipJsonWhitespace(json, offset);
    } else {
      break;
    }
  }
  ExpectJsonCharacter(json, offset, ']');
  ExpectJsonCharacter(json, offset, '}');
  SkipJsonWhitespace(json, offset);
  if (offset != json.size()) {
    throw std::runtime_error("HuxerUI iOS module graph contains trailing data");
  }
  return modules;
}

std::string EscapeSwiftString(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    switch (character) {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(character) < 0x20) {
        throw std::runtime_error("HuxerUI iOS module path contains an unsupported control character");
      }
      escaped += character;
      break;
    }
  }
  return escaped;
}

void WriteFile(const std::filesystem::path& path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output || !output.write(content.data(), static_cast<std::streamsize>(content.size()))) {
    throw std::runtime_error("HuxerUI cannot write iOS module integration: " + path.string());
  }
}

} // namespace

std::vector<GeneratedFile> CreateIosModulePackage(const ProjectTemplateContext& context) {
  const std::string product = MakeModuleProductName(context.project_name);
  return {
      {".gitignore", ".build/\n.swiftpm/\nxcuserdata/\n"},
      {"Package.swift",
       "// swift-tools-version: 5.9\n\nimport PackageDescription\n\nlet package = Package(\n    name: \"" +
           context.project_name +
           "\",\n    platforms: [\n        .iOS(.v13),\n    ],\n    products: [\n        .library(\n            name: "
           "\"" +
           product + "\",\n            targets: [\"" + product +
           "\"]\n        ),\n    ],\n    targets: [\n        .target(name: \"" + product + "\"),\n    ]\n)\n"},
      {"Sources/" + product + "/" + product + ".swift", ""},
  };
}

std::vector<GeneratedFile> CreateIosProject(const ProjectTemplateContext& context) {
  const std::string build_script = context.Render(R"TEMPLATE(set -eu
if [ -z "${HUXERUI_HOME:-}" ]; then
  echo "error: HUXERUI_HOME is not configured; set it in Config/Local.xcconfig" >&2
  exit 1
fi
HUXERUI_CMAKE_ARCHS=$(printf '%s' "$ARCHS" | tr ' ' ';')
cmake -S "$HUXERUI_PROJECT_ROOT" -B "$HUXERUI_CORE_BUILD_DIR" \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT="$SDKROOT" \
  -DCMAKE_OSX_ARCHITECTURES="$HUXERUI_CMAKE_ARCHS" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$IPHONEOS_DEPLOYMENT_TARGET" \
  -DCMAKE_BUILD_TYPE="$CONFIGURATION" \
  -DHUXERUI_HOME="$HUXERUI_HOME" \
  -DHUXERUI_BUILD_SHARED=OFF \
  -DHUXERUI_BUILD_TESTS=OFF \
  -DHUXERUI_BUILD_EXAMPLES=OFF \
  -DHUXERUI_BUILD_CLI=OFF
cmake --build "$HUXERUI_CORE_BUILD_DIR" --target @TARGET_NAME@_huxerui_ios_core --parallel
)TEMPLATE");
  const std::string stage_script = context.Render(R"TEMPLATE(set -eu
HUXERUI_RESOURCE_SOURCE="$HUXERUI_CORE_BUILD_DIR/huxerui-ios/@TARGET_NAME@/resources/package"
HUXERUI_RESOURCE_DESTINATION="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/HuxerUI"
if [ ! -f "$HUXERUI_RESOURCE_SOURCE/huxerui/resources.bin" ]; then
  echo "error: HuxerUI resource package is missing: $HUXERUI_RESOURCE_SOURCE" >&2
  exit 1
fi
cmake -E remove_directory "$HUXERUI_RESOURCE_DESTINATION"
cmake -E make_directory "$HUXERUI_RESOURCE_DESTINATION"
cmake -E copy_directory "$HUXERUI_RESOURCE_SOURCE" "$HUXERUI_RESOURCE_DESTINATION"
if [ -n "${HUXERUI_INTEGRATION_PLAN:-}" ]; then
  mkdir -p "$(dirname "$HUXERUI_INTEGRATION_PLAN")"
  {
    printf '{\n'
    printf '  "schema": 1,\n'
    printf '  "target": "%s",\n' "@TARGET_NAME@"
    printf '  "platform": "ios",\n'
    printf '  "artifact": "%s",\n' "$TARGET_BUILD_DIR/$EXECUTABLE_PATH"
    printf '  "bundle": "%s",\n' "$TARGET_BUILD_DIR/$WRAPPER_NAME"
    printf '  "bundleIdentifier": "%s"\n' "$PRODUCT_BUNDLE_IDENTIFIER"
    printf '}\n'
  } > "$HUXERUI_INTEGRATION_PLAN"
fi
)TEMPLATE");
  std::string project = context.Render(R"TEMPLATE(// !$*UTF8*$!
{
	archiveVersion = 1;
	classes = {};
	objectVersion = 56;
	objects = {

/* Begin PBXBuildFile section */
		000000000000000000000101 /* main.mm in Sources */ = {
			isa = PBXBuildFile;
			fileRef = 000000000000000000000201 /* main.mm */;
		};
		000000000000000000000102 /* Assets.xcassets in Resources */ = {
			isa = PBXBuildFile;
			fileRef = 000000000000000000000203 /* Assets.xcassets */;
		};
		000000000000000000000103 /* LaunchScreen.storyboard in Resources */ = {
			isa = PBXBuildFile;
			fileRef = 000000000000000000000204 /* LaunchScreen.storyboard */;
		};
		000000000000000000000104 /* HuxerUIModules in Frameworks */ = {
			isa = PBXBuildFile;
			productRef = 000000000000000000000901 /* HuxerUIModules */;
		};
/* End PBXBuildFile section */

/* Begin PBXFileReference section */
		000000000000000000000200 /* @PROJECT_NAME@.app */ = {
			isa = PBXFileReference;
			explicitFileType = wrapper.application;
			includeInIndex = 0;
			path = "@PROJECT_NAME@.app";
			sourceTree = BUILT_PRODUCTS_DIR;
		};
		000000000000000000000201 /* main.mm */ = {
			isa = PBXFileReference;
			lastKnownFileType = sourcecode.cpp.objcpp;
			path = main.mm;
			sourceTree = "<group>";
		};
		000000000000000000000202 /* Info.plist */ = {
			isa = PBXFileReference;
			lastKnownFileType = text.plist.xml;
			path = Info.plist;
			sourceTree = "<group>";
		};
		000000000000000000000203 /* Assets.xcassets */ = {
			isa = PBXFileReference;
			lastKnownFileType = folder.assetcatalog;
			path = Assets.xcassets;
			sourceTree = "<group>";
		};
		000000000000000000000204 /* LaunchScreen.storyboard */ = {
			isa = PBXFileReference;
			lastKnownFileType = file.storyboard;
			path = LaunchScreen.storyboard;
			sourceTree = "<group>";
		};
		000000000000000000000205 /* Base.xcconfig */ = {
			isa = PBXFileReference;
			lastKnownFileType = text.xcconfig;
			path = Base.xcconfig;
			sourceTree = "<group>";
		};
		000000000000000000000206 /* Debug.xcconfig */ = {
			isa = PBXFileReference;
			lastKnownFileType = text.xcconfig;
			path = Debug.xcconfig;
			sourceTree = "<group>";
		};
		000000000000000000000207 /* Release.xcconfig */ = {
			isa = PBXFileReference;
			lastKnownFileType = text.xcconfig;
			path = Release.xcconfig;
			sourceTree = "<group>";
		};
		000000000000000000000208 /* Local.xcconfig.example */ = {
			isa = PBXFileReference;
			lastKnownFileType = text.xcconfig;
			path = Local.xcconfig.example;
			sourceTree = "<group>";
		};
/* End PBXFileReference section */

/* Begin PBXFrameworksBuildPhase section */
		000000000000000000000302 /* Frameworks */ = {
			isa = PBXFrameworksBuildPhase;
			buildActionMask = 2147483647;
			files = (
				000000000000000000000104 /* HuxerUIModules in Frameworks */,
			);
			runOnlyForDeploymentPostprocessing = 0;
		};
/* End PBXFrameworksBuildPhase section */

/* Begin PBXGroup section */
		000000000000000000000400 = {
			isa = PBXGroup;
			children = (
				000000000000000000000401 /* App */,
				000000000000000000000402 /* Config */,
				000000000000000000000403 /* Products */,
			);
			sourceTree = "<group>";
		};
		000000000000000000000401 /* App */ = {
			isa = PBXGroup;
			children = (
				000000000000000000000201 /* main.mm */,
				000000000000000000000202 /* Info.plist */,
				000000000000000000000203 /* Assets.xcassets */,
				000000000000000000000204 /* LaunchScreen.storyboard */,
			);
			path = App;
			sourceTree = "<group>";
		};
		000000000000000000000402 /* Config */ = {
			isa = PBXGroup;
			children = (
				000000000000000000000205 /* Base.xcconfig */,
				000000000000000000000206 /* Debug.xcconfig */,
				000000000000000000000207 /* Release.xcconfig */,
				000000000000000000000208 /* Local.xcconfig.example */,
			);
			path = Config;
			sourceTree = "<group>";
		};
		000000000000000000000403 /* Products */ = {
			isa = PBXGroup;
			children = (
				000000000000000000000200 /* @PROJECT_NAME@.app */,
			);
			name = Products;
			sourceTree = "<group>";
		};
/* End PBXGroup section */

/* Begin PBXNativeTarget section */
		000000000000000000000500 /* @TARGET_NAME@ */ = {
			isa = PBXNativeTarget;
			buildConfigurationList = 000000000000000000000702;
			buildPhases = (
				000000000000000000000304 /* Build HuxerUI Core */,
				000000000000000000000301 /* Sources */,
				000000000000000000000302 /* Frameworks */,
				000000000000000000000303 /* Resources */,
				000000000000000000000305 /* Stage HuxerUI Resources */,
			);
			buildRules = ();
			dependencies = ();
			name = @TARGET_NAME@;
			packageProductDependencies = (
				000000000000000000000901 /* HuxerUIModules */,
			);
			productName = "@PROJECT_NAME@";
			productReference = 000000000000000000000200 /* @PROJECT_NAME@.app */;
			productType = "com.apple.product-type.application";
		};
/* End PBXNativeTarget section */

/* Begin PBXProject section */
		000000000000000000000600 /* Project object */ = {
			isa = PBXProject;
			attributes = {
				BuildIndependentTargetsInParallel = 1;
				LastUpgradeCheck = 1600;
				TargetAttributes = {
					000000000000000000000500 = {
						CreatedOnToolsVersion = 16.0;
					};
				};
			};
			buildConfigurationList = 000000000000000000000701;
			compatibilityVersion = "Xcode 14.0";
			developmentRegion = en;
			hasScannedForEncodings = 0;
			knownRegions = (
				en,
				Base,
			);
			mainGroup = 000000000000000000000400;
			packageReferences = (
				000000000000000000000900 /* XCLocalSwiftPackageReference "HuxerUIModules" */,
			);
			productRefGroup = 000000000000000000000403 /* Products */;
			projectDirPath = "";
			projectRoot = "";
			targets = (
				000000000000000000000500 /* @TARGET_NAME@ */,
			);
		};
/* End PBXProject section */

/* Begin PBXResourcesBuildPhase section */
		000000000000000000000303 /* Resources */ = {
			isa = PBXResourcesBuildPhase;
			buildActionMask = 2147483647;
			files = (
				000000000000000000000102 /* Assets.xcassets in Resources */,
				000000000000000000000103 /* LaunchScreen.storyboard in Resources */,
			);
			runOnlyForDeploymentPostprocessing = 0;
		};
/* End PBXResourcesBuildPhase section */

/* Begin PBXShellScriptBuildPhase section */
		000000000000000000000304 /* Build HuxerUI Core */ = {
			isa = PBXShellScriptBuildPhase;
			alwaysOutOfDate = 1;
			buildActionMask = 2147483647;
			files = ();
			inputFileListPaths = ();
			inputPaths = ();
			name = "Build HuxerUI Core";
			outputFileListPaths = ();
			outputPaths = ();
			runOnlyForDeploymentPostprocessing = 0;
			shellPath = /bin/sh;
			shellScript = "@BUILD_SCRIPT@";
		};
		000000000000000000000305 /* Stage HuxerUI Resources */ = {
			isa = PBXShellScriptBuildPhase;
			alwaysOutOfDate = 1;
			buildActionMask = 2147483647;
			files = ();
			inputFileListPaths = ();
			inputPaths = ();
			name = "Stage HuxerUI Resources";
			outputFileListPaths = ();
			outputPaths = ();
			runOnlyForDeploymentPostprocessing = 0;
			shellPath = /bin/sh;
			shellScript = "@STAGE_SCRIPT@";
		};
/* End PBXShellScriptBuildPhase section */

/* Begin PBXSourcesBuildPhase section */
		000000000000000000000301 /* Sources */ = {
			isa = PBXSourcesBuildPhase;
			buildActionMask = 2147483647;
			files = (
				000000000000000000000101 /* main.mm in Sources */,
			);
			runOnlyForDeploymentPostprocessing = 0;
		};
/* End PBXSourcesBuildPhase section */

/* Begin XCBuildConfiguration section */
		000000000000000000000801 /* Project Debug */ = {
			isa = XCBuildConfiguration;
			buildSettings = {
				CLANG_ENABLE_MODULES = YES;
			};
			name = Debug;
		};
		000000000000000000000802 /* Project Release */ = {
			isa = XCBuildConfiguration;
			buildSettings = {
				CLANG_ENABLE_MODULES = YES;
			};
			name = Release;
		};
		000000000000000000000803 /* Target Debug */ = {
			isa = XCBuildConfiguration;
			baseConfigurationReference = 000000000000000000000206 /* Debug.xcconfig */;
			buildSettings = {
				INFOPLIST_FILE = App/Info.plist;
				PRODUCT_NAME = "$(inherited)";
				SDKROOT = iphoneos;
			};
			name = Debug;
		};
		000000000000000000000804 /* Target Release */ = {
			isa = XCBuildConfiguration;
			baseConfigurationReference = 000000000000000000000207 /* Release.xcconfig */;
			buildSettings = {
				INFOPLIST_FILE = App/Info.plist;
				PRODUCT_NAME = "$(inherited)";
				SDKROOT = iphoneos;
			};
			name = Release;
		};
/* End XCBuildConfiguration section */

/* Begin XCConfigurationList section */
		000000000000000000000701 /* Project configuration list */ = {
			isa = XCConfigurationList;
			buildConfigurations = (
				000000000000000000000801 /* Project Debug */,
				000000000000000000000802 /* Project Release */,
			);
			defaultConfigurationIsVisible = 0;
			defaultConfigurationName = Release;
		};
		000000000000000000000702 /* Target configuration list */ = {
			isa = XCConfigurationList;
			buildConfigurations = (
				000000000000000000000803 /* Target Debug */,
				000000000000000000000804 /* Target Release */,
			);
			defaultConfigurationIsVisible = 0;
			defaultConfigurationName = Release;
		};
/* End XCConfigurationList section */

/* Begin XCLocalSwiftPackageReference section */
		000000000000000000000900 /* XCLocalSwiftPackageReference "HuxerUIModules" */ = {
			isa = XCLocalSwiftPackageReference;
			relativePath = ../../.huxerui/generated/ios/modules;
		};
/* End XCLocalSwiftPackageReference section */

/* Begin XCSwiftPackageProductDependency section */
		000000000000000000000901 /* HuxerUIModules */ = {
			isa = XCSwiftPackageProductDependency;
			package = 000000000000000000000900 /* XCLocalSwiftPackageReference "HuxerUIModules" */;
			productName = HuxerUIModules;
		};
/* End XCSwiftPackageProductDependency section */
	};
	rootObject = 000000000000000000000600 /* Project object */;
}
)TEMPLATE");
  ReplaceAll(project, "@BUILD_SCRIPT@", EscapePbxString(build_script));
  ReplaceAll(project, "@STAGE_SCRIPT@", EscapePbxString(stage_script));
  return {
      {".gitignore", "DerivedData/\nxcuserdata/\n*.xcuserstate\narchives/\nConfig/Local.xcconfig\n"},
      {"Config/Base.xcconfig", context.Render(R"TEMPLATE(PRODUCT_NAME = @PROJECT_NAME@
PRODUCT_BUNDLE_IDENTIFIER = @PROJECT_ID@
MARKETING_VERSION = 0.1.0
CURRENT_PROJECT_VERSION = 1
IPHONEOS_DEPLOYMENT_TARGET = 13.0
TARGETED_DEVICE_FAMILY = 1,2
CODE_SIGN_STYLE = Automatic
ASSETCATALOG_COMPILER_APPICON_NAME = AppIcon
CLANG_CXX_LANGUAGE_STANDARD = c++20
ENABLE_BITCODE = NO
ENABLE_USER_SCRIPT_SANDBOXING = NO

HUXERUI_PROJECT_ROOT = $(PROJECT_DIR)/../..
HUXERUI_CORE_BUILD_DIR = $(DERIVED_FILE_DIR)/huxerui-core
HUXERUI_LINK_OPTIONS_FILE = $(HUXERUI_CORE_BUILD_DIR)/huxerui-ios/@TARGET_NAME@/link.rsp
HEADER_SEARCH_PATHS = $(inherited) "$(HUXERUI_HOME)/include"
OTHER_LDFLAGS = $(inherited) @"$(HUXERUI_LINK_OPTIONS_FILE)"

#include? "Local.xcconfig"
)TEMPLATE")},
      {"Config/Debug.xcconfig", R"TEMPLATE(#include "Base.xcconfig"

GCC_PREPROCESSOR_DEFINITIONS = $(inherited) DEBUG=1
ONLY_ACTIVE_ARCH = YES
)TEMPLATE"},
      {"Config/Release.xcconfig", R"TEMPLATE(#include "Base.xcconfig"

SWIFT_COMPILATION_MODE = wholemodule
)TEMPLATE"},
      {"Config/Local.xcconfig", R"TEMPLATE(DEVELOPMENT_TEAM =
)TEMPLATE"},
      {"Config/Local.xcconfig.example", R"TEMPLATE(DEVELOPMENT_TEAM = YOUR_TEAM_ID
)TEMPLATE"},
      {"App/main.mm", R"TEMPLATE(#import <UIKit/UIKit.h>

#include <huxerui/app.h>

int main() {
  @autoreleasepool {
    return huxerui::RunApplication();
  }
}
)TEMPLATE"},
      {"App/Info.plist", context.Render(R"TEMPLATE(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "https://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>
  <string>$(DEVELOPMENT_LANGUAGE)</string>
  <key>CFBundleDisplayName</key>
  <string>@PROJECT_NAME@</string>
  <key>CFBundleExecutable</key>
  <string>$(EXECUTABLE_NAME)</string>
  <key>CFBundleIdentifier</key>
  <string>$(PRODUCT_BUNDLE_IDENTIFIER)</string>
  <key>CFBundleInfoDictionaryVersion</key>
  <string>6.0</string>
  <key>CFBundleName</key>
  <string>$(PRODUCT_NAME)</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>$(MARKETING_VERSION)</string>
  <key>CFBundleVersion</key>
  <string>$(CURRENT_PROJECT_VERSION)</string>
  <key>UILaunchStoryboardName</key>
  <string>LaunchScreen</string>
  <key>UISupportedInterfaceOrientations</key>
  <array>
    <string>UIInterfaceOrientationPortrait</string>
    <string>UIInterfaceOrientationLandscapeLeft</string>
    <string>UIInterfaceOrientationLandscapeRight</string>
  </array>
  <key>UISupportedInterfaceOrientations~ipad</key>
  <array>
    <string>UIInterfaceOrientationPortrait</string>
    <string>UIInterfaceOrientationPortraitUpsideDown</string>
    <string>UIInterfaceOrientationLandscapeLeft</string>
    <string>UIInterfaceOrientationLandscapeRight</string>
  </array>
</dict>
</plist>
)TEMPLATE")},
      {"App/LaunchScreen.storyboard", R"TEMPLATE(<?xml version="1.0" encoding="UTF-8"?>
<document type="com.apple.InterfaceBuilder3.CocoaTouch.Storyboard.XIB"
          version="3.0"
          toolsVersion="23094"
          targetRuntime="iOS.CocoaTouch"
          propertyAccessControl="none"
          useAutolayout="YES"
          launchScreen="YES"
          useTraitCollections="YES"
          useSafeAreas="YES"
          initialViewController="hux-controller"
          colorMatched="YES">
  <device id="retina6_12" orientation="portrait" appearance="light"/>
  <dependencies>
    <plugIn identifier="com.apple.InterfaceBuilder.IBCocoaTouchPlugin" version="23084"/>
    <capability name="Safe area layout guides" minToolsVersion="9.0"/>
    <capability name="System colors in document resources" minToolsVersion="11.0"/>
  </dependencies>
  <scenes>
    <scene sceneID="hux-scene">
      <objects>
        <viewController id="hux-controller" sceneMemberID="viewController">
          <view key="view" contentMode="scaleToFill" id="hux-view">
            <rect key="frame" x="0.0" y="0.0" width="393" height="852"/>
            <viewLayoutGuide key="safeArea" id="hux-safe-area"/>
            <color key="backgroundColor" systemColor="systemBackgroundColor"/>
          </view>
        </viewController>
        <placeholder placeholderIdentifier="IBFirstResponder"
                     id="hux-responder"
                     userLabel="First Responder"
                     sceneMemberID="firstResponder"/>
      </objects>
    </scene>
  </scenes>
  <resources>
    <systemColor name="systemBackgroundColor">
      <color white="1" alpha="1" colorSpace="custom" customColorSpace="genericGamma22GrayColorSpace"/>
    </systemColor>
  </resources>
</document>
)TEMPLATE"},
      {"App/Assets.xcassets/Contents.json", R"TEMPLATE({
  "info" : {
    "author" : "xcode",
    "version" : 1
  }
}
)TEMPLATE"},
      {"App/Assets.xcassets/AppIcon.appiconset/Contents.json", R"TEMPLATE({
  "images" : [
    { "idiom" : "iphone", "scale" : "2x", "size" : "20x20" },
    { "idiom" : "iphone", "scale" : "3x", "size" : "20x20" },
    { "idiom" : "iphone", "scale" : "2x", "size" : "29x29" },
    { "idiom" : "iphone", "scale" : "3x", "size" : "29x29" },
    { "idiom" : "iphone", "scale" : "2x", "size" : "40x40" },
    { "idiom" : "iphone", "scale" : "3x", "size" : "40x40" },
    { "idiom" : "iphone", "scale" : "2x", "size" : "60x60" },
    { "idiom" : "iphone", "scale" : "3x", "size" : "60x60" },
    { "idiom" : "ipad", "scale" : "1x", "size" : "20x20" },
    { "idiom" : "ipad", "scale" : "2x", "size" : "20x20" },
    { "idiom" : "ipad", "scale" : "1x", "size" : "29x29" },
    { "idiom" : "ipad", "scale" : "2x", "size" : "29x29" },
    { "idiom" : "ipad", "scale" : "1x", "size" : "40x40" },
    { "idiom" : "ipad", "scale" : "2x", "size" : "40x40" },
    { "idiom" : "ipad", "scale" : "1x", "size" : "76x76" },
    { "idiom" : "ipad", "scale" : "2x", "size" : "76x76" },
    { "idiom" : "ipad", "scale" : "2x", "size" : "83.5x83.5" },
    { "idiom" : "ios-marketing", "scale" : "1x", "size" : "1024x1024" }
  ],
  "info" : {
    "author" : "xcode",
    "version" : 1
  }
}
)TEMPLATE"},
      {context.Render("@TARGET_NAME@.xcodeproj/project.pbxproj"), std::move(project)},
      {context.Render("@TARGET_NAME@.xcodeproj/xcshareddata/xcschemes/@TARGET_NAME@.xcscheme"),
       context.Render(R"TEMPLATE(<?xml version="1.0" encoding="UTF-8"?>
<Scheme LastUpgradeVersion="1600" version="1.7">
  <BuildAction parallelizeBuildables="YES" buildImplicitDependencies="YES">
    <BuildActionEntries>
      <BuildActionEntry buildForTesting="YES"
                        buildForRunning="YES"
                        buildForProfiling="YES"
                        buildForArchiving="YES"
                        buildForAnalyzing="YES">
        <BuildableReference BuildableIdentifier="primary"
                            BlueprintIdentifier="000000000000000000000500"
                            BuildableName="@PROJECT_NAME@.app"
                            BlueprintName="@TARGET_NAME@"
                            ReferencedContainer="container:@TARGET_NAME@.xcodeproj"/>
      </BuildActionEntry>
    </BuildActionEntries>
  </BuildAction>
  <TestAction buildConfiguration="Debug"
              selectedDebuggerIdentifier="Xcode.DebuggerFoundation.Debugger.LLDB"
              selectedLauncherIdentifier="Xcode.DebuggerFoundation.Launcher.LLDB"
              shouldUseLaunchSchemeArgsEnv="YES"/>
  <LaunchAction buildConfiguration="Debug"
                selectedDebuggerIdentifier="Xcode.DebuggerFoundation.Debugger.LLDB"
                selectedLauncherIdentifier="Xcode.DebuggerFoundation.Launcher.LLDB"
                launchStyle="0"
                useCustomWorkingDirectory="NO"
                ignoresPersistentStateOnLaunch="NO"
                debugDocumentVersioning="YES"
                debugServiceExtension="internal"
                allowLocationSimulation="YES">
    <BuildableProductRunnable runnableDebuggingMode="0">
      <BuildableReference BuildableIdentifier="primary"
                          BlueprintIdentifier="000000000000000000000500"
                          BuildableName="@PROJECT_NAME@.app"
                          BlueprintName="@TARGET_NAME@"
                          ReferencedContainer="container:@TARGET_NAME@.xcodeproj"/>
    </BuildableProductRunnable>
  </LaunchAction>
  <ProfileAction buildConfiguration="Release"
                 shouldUseLaunchSchemeArgsEnv="YES"
                 savedToolIdentifier=""
                 useCustomWorkingDirectory="NO"
                 debugDocumentVersioning="YES">
    <BuildableProductRunnable runnableDebuggingMode="0">
      <BuildableReference BuildableIdentifier="primary"
                          BlueprintIdentifier="000000000000000000000500"
                          BuildableName="@PROJECT_NAME@.app"
                          BlueprintName="@TARGET_NAME@"
                          ReferencedContainer="container:@TARGET_NAME@.xcodeproj"/>
    </BuildableProductRunnable>
  </ProfileAction>
  <AnalyzeAction buildConfiguration="Debug"/>
  <ArchiveAction buildConfiguration="Release" revealArchiveInOrganizer="YES"/>
</Scheme>
)TEMPLATE")},
  };
}

void UpdateIosModuleIntegration(const std::filesystem::path& project_root) {
  const std::filesystem::path graph = project_root / ".huxerui/generated/modules.json";
  std::ifstream input(graph, std::ios::binary);
  if (!input) {
    throw std::runtime_error("HuxerUI iOS module graph is missing: " + graph.string());
  }
  const std::string json{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  const std::vector<IosModulePackage> modules = ParseIosModulePackages(json);

  std::string manifest = R"TEMPLATE(// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "HuxerUIModules",
    platforms: [
        .iOS(.v13),
    ],
    products: [
        .library(
            name: "HuxerUIModules",
            targets: ["HuxerUIModules"]
        ),
    ],
    dependencies: [
)TEMPLATE";
  for (const IosModulePackage& module : modules) {
    manifest += "        .package(name: \"" + EscapeSwiftString(module.product) + "\", path: \"" +
                EscapeSwiftString(module.path.generic_string()) + "\"),\n";
  }
  manifest += R"TEMPLATE(    ],
    targets: [
        .target(
            name: "HuxerUIModules",
            dependencies: [
)TEMPLATE";
  for (const IosModulePackage& module : modules) {
    manifest += "                .product(name: \"" + EscapeSwiftString(module.product) + "\", package: \"" +
                EscapeSwiftString(module.product) + "\"),\n";
  }
  manifest += R"TEMPLATE(            ]
        ),
    ]
)
)TEMPLATE";

  const std::filesystem::path output = project_root / ".huxerui/generated/ios/modules";
  WriteFile(output / "Package.swift", manifest);
  WriteFile(output / "Sources/HuxerUIModules/HuxerUIModules.swift", "enum HuxerUIModulesAnchor {}\n");
}

void ConfigureIosLocalHome(const std::filesystem::path& project_root, const std::filesystem::path& huxerui_home) {
  if (huxerui_home.empty()) {
    throw std::invalid_argument("HuxerUI iOS local configuration requires HUXERUI_HOME");
  }
  const std::filesystem::path configuration = project_root / "platform/ios/Config/Local.xcconfig";
  if (!std::filesystem::is_directory(configuration.parent_path())) {
    throw std::runtime_error("HuxerUI iOS configuration directory is missing: " + configuration.parent_path().string());
  }

  std::string content;
  if (std::ifstream input(configuration, std::ios::binary); input) {
    content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }

  constexpr std::string_view setting_name = "HUXERUI_HOME";
  const std::string setting = std::string(setting_name) + " = " + huxerui_home.generic_string();
  bool replaced = false;
  std::size_t line_start = 0;
  while (line_start < content.size()) {
    const std::size_t line_end = content.find('\n', line_start);
    const std::size_t assignment = content.find('=', line_start);
    if (assignment != std::string::npos && (line_end == std::string::npos || assignment < line_end)) {
      std::string_view name(content.data() + line_start, assignment - line_start);
      while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
        name.remove_suffix(1);
      }
      if (name == setting_name) {
        const std::size_t replace_end = line_end == std::string::npos ? content.size() : line_end;
        content.replace(line_start, replace_end - line_start, setting);
        replaced = true;
        break;
      }
    }
    if (line_end == std::string::npos) {
      break;
    }
    line_start = line_end + 1;
  }
  if (!replaced) {
    if (!content.empty() && content.back() != '\n') {
      content += '\n';
    }
    content += setting + '\n';
  }

  std::ofstream output(configuration, std::ios::binary | std::ios::trunc);
  if (!output || !output.write(content.data(), static_cast<std::streamsize>(content.size()))) {
    throw std::runtime_error("HuxerUI cannot update iOS local configuration: " + configuration.string());
  }
}

} // namespace huxerui::cli
