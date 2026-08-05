#include "ios_project.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace huxerui::cli {
namespace {

ProjectTemplateContext AppleContext(const ProjectTemplateContext& context) {
  ProjectTemplateContext result = context;
  std::replace(result.package_name.begin(), result.package_name.end(), '_', '-');
  return result;
}

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

} // namespace

std::vector<GeneratedFile> CreateIosProject(const ProjectTemplateContext& context) {
  const ProjectTemplateContext apple_context = AppleContext(context);
  const std::string build_script = apple_context.Render(R"TEMPLATE(set -eu
if [ -z "${HUXERUI_SDK_ROOT:-}" ]; then
  echo "error: HUXERUI_SDK_ROOT is not configured; set it in Config/Local.xcconfig" >&2
  exit 1
fi
HUXERUI_CMAKE_ARCHS=$(printf '%s' "$ARCHS" | tr ' ' ';')
cmake -S "$HUXERUI_PROJECT_ROOT" -B "$HUXERUI_CORE_BUILD_DIR" \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT="$SDKROOT" \
  -DCMAKE_OSX_ARCHITECTURES="$HUXERUI_CMAKE_ARCHS" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$IPHONEOS_DEPLOYMENT_TARGET" \
  -DCMAKE_BUILD_TYPE="$CONFIGURATION" \
  -DHUXERUI_SDK_ROOT="$HUXERUI_SDK_ROOT" \
  -DHUXERUI_BUILD_SHARED=OFF \
  -DHUXERUI_BUILD_TESTS=OFF \
  -DHUXERUI_BUILD_EXAMPLES=OFF \
  -DHUXERUI_BUILD_CLI=OFF
cmake --build "$HUXERUI_CORE_BUILD_DIR" --target @TARGET_NAME@_huxerui_ios_core --parallel
)TEMPLATE");
  const std::string stage_script = apple_context.Render(R"TEMPLATE(set -eu
HUXERUI_RESOURCE_SOURCE="$HUXERUI_CORE_BUILD_DIR/huxerui-resources/@TARGET_NAME@/package"
HUXERUI_RESOURCE_DESTINATION="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/HuxerUI"
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
  std::string project = apple_context.Render(R"TEMPLATE(// !$*UTF8*$!
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
			files = ();
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
			productName = @PROJECT_NAME@;
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
	};
	rootObject = 000000000000000000000600 /* Project object */;
}
)TEMPLATE");
  ReplaceAll(project, "@BUILD_SCRIPT@", EscapePbxString(build_script));
  ReplaceAll(project, "@STAGE_SCRIPT@", EscapePbxString(stage_script));
  return {
      {".gitignore", "DerivedData/\nxcuserdata/\n*.xcuserstate\narchives/\nConfig/Local.xcconfig\n"},
      {"Config/Base.xcconfig", apple_context.Render(R"TEMPLATE(PRODUCT_NAME = @PROJECT_NAME@
PRODUCT_BUNDLE_IDENTIFIER = @PACKAGE_NAME@
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

extern "C" int HuxerUIRunApplication();

int main() {
  @autoreleasepool {
    return HuxerUIRunApplication();
  }
}
)TEMPLATE"},
      {"App/Info.plist", apple_context.Render(R"TEMPLATE(<?xml version="1.0" encoding="UTF-8"?>
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
      {apple_context.Render("@TARGET_NAME@.xcodeproj/project.pbxproj"), std::move(project)},
      {apple_context.Render("@TARGET_NAME@.xcodeproj/xcshareddata/xcschemes/@TARGET_NAME@.xcscheme"),
       apple_context.Render(R"TEMPLATE(<?xml version="1.0" encoding="UTF-8"?>
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

void ConfigureIosLocalSdk(const std::filesystem::path& project_root, const std::filesystem::path& sdk_root) {
  if (sdk_root.empty()) {
    throw std::invalid_argument("HuxerUI iOS local configuration requires an SDK root");
  }
  const std::filesystem::path configuration = project_root / "platform/ios/Config/Local.xcconfig";
  if (!std::filesystem::is_directory(configuration.parent_path())) {
    throw std::runtime_error("HuxerUI iOS configuration directory is missing: " + configuration.parent_path().string());
  }

  std::string content;
  if (std::ifstream input(configuration, std::ios::binary); input) {
    content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }

  constexpr std::string_view setting_name = "HUXERUI_SDK_ROOT";
  const std::string setting = std::string(setting_name) + " = " + sdk_root.generic_string();
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
