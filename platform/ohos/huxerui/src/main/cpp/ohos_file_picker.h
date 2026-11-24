#pragma once

#include <napi/native_api.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <huxerui/file_drop.h>

#include "io/file_internal.h"

struct ArkUI_DragEvent;

namespace huxerui::detail {

std::optional<std::string> OhosFileUri(const File& file);
std::optional<std::string> OhosFileUri(const FileReference& reference);
std::optional<std::vector<FileReference>> ReadOhosGrantedFiles(napi_env env, napi_value files);

class OhosFileAccess final : public FileAccessStore, public std::enable_shared_from_this<OhosFileAccess> {
public:
  OhosFileAccess(napi_env env, napi_value host);
  ~OhosFileAccess() override;

  Task<IoResult<void>> Remember(std::string key, std::shared_ptr<FileReferenceBackend> reference) override;
  Task<IoResult<FileReference>> Restore(std::string key) override;
  Task<IoResult<void>> Forget(std::string key) override;
  Task<IoResult<std::vector<FileReference>>> OpenUris(std::vector<std::string> uris);
  FileDropPreparation PrepareDrop(ArkUI_DragEvent& event);

  napi_value Complete(napi_callback_info info);
  void Disconnect() noexcept;

private:
  using Files = IoResult<std::vector<FileReference>>;
  Task<Files> Start(std::string operation, std::string key, std::vector<std::string> uris = {});
  std::function<void()> Start(std::string_view operation, std::string_view key, std::vector<std::string> uris,
                              FileDropCompletion completion);

  napi_env env_;
  napi_ref host_ = nullptr;
  std::map<std::string, FileDropCompletion, std::less<>> pending_;
};

class OhosFilePicker final : public FilePickerTransport {
public:
  OhosFilePicker(napi_env env, napi_value host);
  ~OhosFilePicker() override;

  bool CanOpenFiles() const noexcept override;
  bool CanSaveFiles() const noexcept override;
  bool CanOpenDirectories(bool writable) const noexcept override;
  std::function<void()>
  OpenFiles(FilePickerFilter filter, bool multiple, FilePickerOpenCompletion completion) override;
  std::function<void()> SaveFile(File source, SaveFileOptions options, FilePickerSaveCompletion completion) override;
  std::function<void()> OpenDirectory(bool writable, FilePickerOpenCompletion completion) override;

  void CompleteOpen(napi_value token, napi_value selections);
  void CompleteSave(napi_value token, napi_value saved);
  void ReleaseHost() noexcept;

private:
  bool Matches(napi_value value) const;

  napi_env env_;
  napi_ref host_ = nullptr;
  bool can_select_directory_ = false;
  std::string request_;
  FilePickerOpenCompletion open_completion_;
  FilePickerSaveCompletion save_completion_;
};

} // namespace huxerui::detail
