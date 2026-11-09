#pragma once

#include <cstdint>

#include <emscripten/val.h>

#include <huxerui/file.h>

namespace huxerui::detail {

// JavaScript receives the existing provider source while the native handle retains its FileReference capability.
// The capability key is a process-local comparison key derived from FileReferenceBackend, never an owning handle.
struct WebFileReferenceProjection {
  emscripten::val source;
  std::uintptr_t capability_key = 0;
  bool is_file = false;
};

[[nodiscard]] WebFileReferenceProjection ProjectWebFileReference(const FileReference& reference);

} // namespace huxerui::detail
