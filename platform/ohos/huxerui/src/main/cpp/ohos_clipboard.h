#pragma once

#include <huxerui/clipboard.h>

namespace huxerui::detail {

class OhosFileAccess;

class OhosClipboard final : public PlatformClipboard {
public:
  explicit OhosClipboard(std::weak_ptr<OhosFileAccess> file_access);

  std::optional<std::string> ReadText() override;
  bool WriteText(std::string_view text) override;

private:
  bool WriteItem(ClipboardItem item) override;
  Task<IoResult<ClipboardItem>> ReadItemAsync(ClipboardReadOptions options) override;
  bool WriteFiles(ClipboardFiles files) override;
  Task<IoResult<std::vector<FileReference>>> ReadFilesAsync() override;
  bool MayHaveText() const noexcept override;

  std::weak_ptr<OhosFileAccess> file_access_;
};

} // namespace huxerui::detail
