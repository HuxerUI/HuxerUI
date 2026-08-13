#include "runtime_test_support.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace huxerui::test {

namespace {

class BuiltinResources final : public PlatformResources {
public:
  ResourceConfiguration Configuration() const override {
    return {};
  }

  RawAsset Read(std::string_view package_path) override {
    const std::filesystem::path path =
        std::filesystem::path(HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE) / std::filesystem::path(std::string(package_path));
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
      return {};
    }
    const std::streamoff length = stream.tellg();
    if (length < 0 || static_cast<std::uintmax_t>(length) > std::numeric_limits<std::size_t>::max()) {
      return {};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    stream.seekg(0, std::ios::beg);
    if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), length)) {
      return {};
    }
    return RawAsset::FromBytes(std::move(bytes));
  }
};

} // namespace

PlatformResources* BuiltinTestResources() {
  static BuiltinResources resources;
  return &resources;
}

} // namespace huxerui::test
