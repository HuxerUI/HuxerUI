#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <huxerui/huxerui.h>

namespace {

using namespace huxerui;

constexpr std::string_view contents = "HuxerUI persisted bytes";

View Root() { return Spacer(); }

std::string CheckRandomAccess(const std::string& persistent_path, const std::string& memory_path) {
  const File persistent(persistent_path);
  for (const auto access : {FileAccessMode::WriteOnly, FileAccessMode::ReadWrite}) {
    auto opened = persistent.OpenRandomAccess(access, FileOpenMode::CreateNew);
    if (opened.Succeeded() || opened.Error().code != IoErrorCode::Unsupported || persistent.Exists()) {
      return "persistent random-access open did not reject the write without creating a file";
    }
  }

  const File memory(memory_path);
  auto opened = memory.OpenRandomAccess(FileAccessMode::ReadWrite, FileOpenMode::CreateNew);
  if (!opened.Succeeded()) {
    return "MEMFS random-access open failed";
  }
  auto& stream = opened.Value();
  const auto bytes = std::as_bytes(std::span(contents));
  if (!stream.WriteAt(0, bytes).Succeeded() || !stream.Close().Succeeded()) {
    return "MEMFS random-access write or close failed";
  }
  const auto result = memory.ReadString();
  return result.Succeeded() && result.Value() == contents ? "" : "MEMFS random-access write changed the bytes";
}

Task<IoResult<void>> WritePersistentFile(File file) {
  auto opened = co_await file.OpenWriteAsync();
  if (!opened.Succeeded()) {
    co_return IoResult<void>(std::move(opened).Error());
  }
  auto& stream = opened.Value();
  const auto bytes = std::as_bytes(std::span(contents));
  auto written = co_await stream.WriteAsync(Bytes(bytes.begin(), bytes.end()));
  if (!written.Succeeded()) {
    co_return written;
  }
  co_return co_await stream.CloseAsync();
}

Task<void> ReportPersistentWrite(File file, emscripten::val report) {
  auto result = co_await WritePersistentFile(std::move(file));
  report(result.Succeeded() ? std::string{} : "async output failed: " + result.Error().message);
}

void WritePersistent(const std::string& path, emscripten::val report) {
  UseApplicationTaskScope().Launch(ReportPersistentWrite(File(path), std::move(report)));
}

bool ReadPersistent(const std::string& path) {
  const auto result = File(path).ReadString();
  return result.Succeeded() && result.Value() == contents;
}

} // namespace

const huxerui::Application application{Root, {.show_debug_overlay = false}};

EMSCRIPTEN_BINDINGS(huxerui_web_file_storage_test) {
  emscripten::function("checkRandomAccess", &CheckRandomAccess);
  emscripten::function("writePersistent", &WritePersistent);
  emscripten::function("readPersistent", &ReadPersistent);
}
