#include "project.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace huxerui::cli {
namespace {

class TemporaryTree final {
public:
  explicit TemporaryTree(std::filesystem::path path) : path_(std::move(path)) {}

  ~TemporaryTree() {
    if (!committed_) {
      std::error_code error;
      std::filesystem::remove_all(path_, error);
    }
  }

  TemporaryTree(const TemporaryTree&) = delete;
  TemporaryTree& operator=(const TemporaryTree&) = delete;

  void Commit() noexcept {
    committed_ = true;
  }

private:
  std::filesystem::path path_;
  bool committed_ = false;
};

std::filesystem::path TemporaryPath(const std::filesystem::path& parent, std::string_view stem) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  return parent / ("." + std::string(stem) + ".huxerui-tmp-" + std::to_string(nonce));
}

void ValidateRelativePath(const std::filesystem::path& path) {
  if (path.empty() || path.is_absolute()) {
    throw std::logic_error("HuxerUI CLI generated an invalid absolute path");
  }
  for (const auto& component : path) {
    if (component == "..") {
      throw std::logic_error("HuxerUI CLI generated a path outside its project root");
    }
  }
}

void WriteFile(const std::filesystem::path& root, const GeneratedFile& file) {
  ValidateRelativePath(file.path);
  const std::filesystem::path output_path = root / file.path;
  std::filesystem::create_directories(output_path.parent_path());
  std::ofstream stream(output_path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("cannot create " + output_path.string());
  }
  stream.write(file.content.data(), static_cast<std::streamsize>(file.content.size()));
  if (!stream) {
    throw std::runtime_error("cannot write " + output_path.string());
  }
}

void WriteFiles(const std::filesystem::path& root, std::span<const GeneratedFile> files) {
  for (const GeneratedFile& file : files) {
    WriteFile(root, file);
  }
}

std::vector<GeneratedFile> CommonProjectFiles(const ProjectTemplateContext& context) {
  return {
      {".gitignore",
       R"TEMPLATE(/.huxerui/
/dist/
/build/
/cmake-build-*/
/.cache/

/.idea/
/.vscode/
/.vs/

.DS_Store
Thumbs.db
)TEMPLATE"},
      {"CMakeLists.txt", context.Render(R"TEMPLATE(cmake_minimum_required(VERSION 3.20)
project(@TARGET_NAME@ VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(HUXERUI_SDK_ROOT "$ENV{HUXERUI_SDK_ROOT}" CACHE PATH "HuxerUI SDK or source directory")
if (HUXERUI_SDK_ROOT AND EXISTS "${HUXERUI_SDK_ROOT}/CMakeLists.txt"
        AND EXISTS "${HUXERUI_SDK_ROOT}/include/huxerui/huxerui.h")
    set(HUXERUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(HUXERUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory("${HUXERUI_SDK_ROOT}" "${CMAKE_BINARY_DIR}/huxerui-sdk" EXCLUDE_FROM_ALL)
else ()
    if (HUXERUI_SDK_ROOT)
        find_package(HuxerUI CONFIG REQUIRED
                PATHS "${HUXERUI_SDK_ROOT}"
                NO_DEFAULT_PATH
                NO_CMAKE_FIND_ROOT_PATH
        )
    else ()
        find_package(HuxerUI CONFIG REQUIRED)
    endif ()
endif ()

if (WIN32)
    include("${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/huxerui.cmake" OPTIONAL)
elseif (APPLE AND NOT IOS)
    include("${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/huxerui.cmake" OPTIONAL)
elseif (EMSCRIPTEN)
    include("${CMAKE_CURRENT_SOURCE_DIR}/platform/web/huxerui.cmake" OPTIONAL)
endif ()

set(HUXERUI_APP_BUNDLE_IDENTIFIER "@PACKAGE_NAME@")
if (APPLE AND NOT IOS AND HUXERUI_MACOS_BUNDLE_IDENTIFIER)
    set(HUXERUI_APP_BUNDLE_IDENTIFIER "${HUXERUI_MACOS_BUNDLE_IDENTIFIER}")
endif ()

file(GLOB_RECURSE APP_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)

huxerui_add_app(@TARGET_NAME@
        SOURCES
            ${APP_SOURCE_FILES}
        RESOURCES
            assets
        RESOURCE_NAMESPACE
            app
        BUNDLE_NAME
            "@PROJECT_NAME@"
        BUNDLE_IDENTIFIER
            "${HUXERUI_APP_BUNDLE_IDENTIFIER}"
)

if (WIN32 AND HUXERUI_WINDOWS_MANIFEST)
    target_sources(@TARGET_NAME@ PRIVATE "${HUXERUI_WINDOWS_MANIFEST}")
endif ()
if (APPLE AND NOT IOS AND HUXERUI_MACOS_INFO_PLIST)
    set_target_properties(@TARGET_NAME@ PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${HUXERUI_MACOS_INFO_PLIST}"
    )
endif ()
if (EMSCRIPTEN AND COMMAND huxerui_configure_web_app)
    huxerui_configure_web_app(@TARGET_NAME@)
endif ()
)TEMPLATE")},
      {"src/main.cpp", context.Render(R"TEMPLATE(#include <huxerui/huxerui.h>

using namespace huxerui;

View App() {
  return MaterialTheme([] {
    return Text("Hello, HuxerUI");
  });
}

HUXERUI_APP(App, {.title = "@PROJECT_NAME@"})
)TEMPLATE")},
      {"assets/strings/default.properties", context.Render("app_name = \"@PROJECT_NAME@\"\n")},
  };
}

} // namespace

bool IsValidProjectName(std::string_view name) noexcept {
  if (name.empty() || !std::isalpha(static_cast<unsigned char>(name.front()))) {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](char character) {
    const unsigned char value = static_cast<unsigned char>(character);
    return std::isalnum(value) || character == '_' || character == '-';
  });
}

ProjectTemplateContext MakeProjectTemplateContext(std::string_view project_name) {
  if (!IsValidProjectName(project_name)) {
    throw std::invalid_argument(
        "project name must start with a letter and contain only letters, digits, underscores, or hyphens"
    );
  }

  std::string target_name;
  target_name.reserve(project_name.size());
  for (const char character : project_name) {
    const unsigned char value = static_cast<unsigned char>(character);
    target_name.push_back(character == '-' ? '_' : static_cast<char>(std::tolower(value)));
  }
  return {std::string(project_name), target_name, "com.example." + target_name};
}

Project DiscoverProject(const std::filesystem::path& start) {
  std::error_code error;
  std::filesystem::path current = std::filesystem::absolute(start, error);
  if (error) {
    throw std::runtime_error("cannot resolve working directory: " + start.string());
  }
  if (std::filesystem::is_regular_file(current)) {
    current = current.parent_path();
  }

  while (!current.empty()) {
    if (std::filesystem::is_regular_file(current / "CMakeLists.txt") &&
        std::filesystem::is_directory(current / "platform")) {
      Project project{current, {}, {}};
      for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(current / "platform")) {
        if (!entry.is_directory()) {
          continue;
        }
        const std::string id = entry.path().filename().string();
        if (FindPlatformDriver(id)) {
          project.platforms.push_back(id);
        } else {
          project.unknown_platforms.push_back(id);
        }
      }
      std::sort(project.platforms.begin(), project.platforms.end());
      std::sort(project.unknown_platforms.begin(), project.unknown_platforms.end());
      return project;
    }

    const std::filesystem::path parent = current.parent_path();
    if (parent == current) {
      break;
    }
    current = parent;
  }
  throw std::runtime_error("no HuxerUI project found from " + start.string());
}

void CreateProject(
    const std::filesystem::path& destination,
    const ProjectTemplateContext& context,
    std::span<const PlatformDriver* const> platforms
) {
  if (std::filesystem::exists(destination)) {
    throw std::runtime_error("destination already exists: " + destination.string());
  }
  if (platforms.empty()) {
    throw std::invalid_argument("at least one platform is required");
  }

  const std::filesystem::path parent = destination.has_parent_path() ? destination.parent_path() : ".";
  std::filesystem::create_directories(parent);
  const std::filesystem::path temporary = TemporaryPath(parent, destination.filename().string());
  if (std::filesystem::exists(temporary)) {
    throw std::runtime_error("temporary project path already exists: " + temporary.string());
  }

  TemporaryTree cleanup(temporary);
  std::filesystem::create_directories(temporary);
  const std::vector<GeneratedFile> common_files = CommonProjectFiles(context);
  WriteFiles(temporary, common_files);
  std::filesystem::create_directories(temporary / "assets/images");
  std::filesystem::create_directories(temporary / "assets/raw");
  for (const PlatformDriver* platform : platforms) {
    const std::vector<GeneratedFile> files = platform->CreateShell(context);
    WriteFiles(temporary / "platform" / platform->Id(), files);
  }

  std::filesystem::rename(temporary, destination);
  cleanup.Commit();
}

void AddProjectPlatforms(
    const Project& project, const ProjectTemplateContext& context, std::span<const PlatformDriver* const> platforms
) {
  if (platforms.empty()) {
    throw std::invalid_argument("at least one platform is required");
  }
  for (const PlatformDriver* platform : platforms) {
    const std::filesystem::path destination = project.root / "platform" / platform->Id();
    if (std::filesystem::exists(destination)) {
      throw std::runtime_error("platform already exists: " + std::string(platform->Id()));
    }
  }

  std::vector<std::filesystem::path> created;
  try {
    for (const PlatformDriver* platform : platforms) {
      const std::filesystem::path platform_root = project.root / "platform";
      const std::filesystem::path temporary = TemporaryPath(platform_root, platform->Id());
      TemporaryTree cleanup(temporary);
      std::filesystem::create_directories(temporary);
      const std::vector<GeneratedFile> files = platform->CreateShell(context);
      WriteFiles(temporary, files);

      const std::filesystem::path destination = platform_root / platform->Id();
      std::filesystem::rename(temporary, destination);
      cleanup.Commit();
      created.push_back(destination);
    }
  } catch (...) {
    for (const std::filesystem::path& path : created) {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
    throw;
  }
}

} // namespace huxerui::cli
