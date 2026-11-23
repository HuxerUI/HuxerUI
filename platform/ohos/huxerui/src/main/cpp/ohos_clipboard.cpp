#include "ohos_clipboard.h"

#include <database/pasteboard/oh_pasteboard.h>
#include <database/pasteboard/oh_pasteboard_err_code.h>
#include <database/udmf/udmf.h>
#include <database/udmf/udmf_err_code.h>
#include <database/udmf/uds.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "application/application_internal.h"

#include "ohos_bitmap.h"
#include "ohos_file_picker.h"

namespace huxerui::detail {
namespace {

constexpr char plain_text_type[] = "general.plain-text";
constexpr char html_type[] = "general.html";
constexpr char pixelmap_type[] = "openharmony.pixel-map";
constexpr char file_uri_type[] = "general.file-uri";
constexpr std::array image_types{"image/png", "image/jpeg"};

using ClipboardData = std::unique_ptr<OH_UdmfData, decltype(&OH_UdmfData_Destroy)>;

template <class T>
auto Own(T* value, void (*destroy)(T*)) {
  return std::unique_ptr<T, decltype(destroy)>(value, destroy);
}

bool AddPlainText(OH_UdmfRecord* record, const std::string& text) {
  const auto value = Own(OH_UdsPlainText_Create(), &OH_UdsPlainText_Destroy);
  return value && OH_UdsPlainText_SetContent(value.get(), text.c_str()) == UDMF_E_OK &&
         OH_UdmfRecord_AddPlainText(record, value.get()) == UDMF_E_OK;
}

bool AddHtml(OH_UdmfRecord* record, const std::string& html, const std::optional<std::string>& plain) {
  const auto value = Own(OH_UdsHtml_Create(), &OH_UdsHtml_Destroy);
  return value && OH_UdsHtml_SetContent(value.get(), html.c_str()) == UDMF_E_OK &&
         (!plain || OH_UdsHtml_SetPlainContent(value.get(), plain->c_str()) == UDMF_E_OK) &&
         OH_UdmfRecord_AddHtml(record, value.get()) == UDMF_E_OK;
}

bool AddBytes(OH_UdmfRecord* record, const char* type, std::span<const std::byte> bytes) {
  auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
  // UDMF copies the input into its own record even though the parameter is mutable.
  return OH_UdmfRecord_AddGeneralEntry(record, type, const_cast<unsigned char*>(data), bytes.size()) == UDMF_E_OK;
}

bool AddImage(OH_UdmfRecord* record, const ImageAsset& image) {
  try {
    const auto value = Own(OH_UdsPixelMap_Create(), &OH_UdsPixelMap_Destroy);
    const auto pixelmap = DecodeOhosPixelmap(image);
    return value && OH_UdsPixelMap_SetPixelMap(value.get(), pixelmap.get()) == UDMF_E_OK &&
           OH_UdmfRecord_AddPixelMap(record, value.get()) == UDMF_E_OK &&
           AddBytes(record, image.MimeType().data(), image.EncodedBytes());
  } catch (const std::exception&) {
    return false;
  }
}

bool AddCustom(OH_UdmfRecord* record, const std::string& mime, const Bytes& bytes) {
  if (!bytes.empty()) {
    return AddBytes(record, mime.c_str(), bytes);
  }
  // UDMF rejects zero-length data. A separate type identifier preserves empty values without changing MIME payloads.
  constexpr std::byte empty_marker{1};
  return AddBytes(record, ClipboardTypeIdentifier(mime).c_str(), std::span(&empty_marker, 1));
}

IoResult<ClipboardData> ReadData() {
  const auto clipboard = Own(OH_Pasteboard_Create(), &OH_Pasteboard_Destroy);
  if (!clipboard) {
    return ClipboardFailure<ClipboardData>(IoErrorCode::Unsupported);
  }
  if (!OH_Pasteboard_HasData(clipboard.get())) {
    return ClipboardFailure<ClipboardData>(IoErrorCode::NotFound);
  }
  int status = ERR_OK;
  ClipboardData data(OH_Pasteboard_GetData(clipboard.get(), &status), &OH_UdmfData_Destroy);
  if (data) {
    return IoResult<ClipboardData>::Success(std::move(data));
  }
  if (status == ERR_PERMISSION_ERROR) {
    return ClipboardFailure<ClipboardData>(IoErrorCode::PermissionDenied);
  }
  if (status == ERR_DEVICE_NOT_SUPPORTED) {
    return ClipboardFailure<ClipboardData>(IoErrorCode::Unsupported);
  }
  return ClipboardFailure<ClipboardData>(status == ERR_OK ? IoErrorCode::NotFound : IoErrorCode::Io);
}

IoResult<std::string> ReadString(const char* text) {
  if (!text) {
    return ClipboardFailure<std::string>(IoErrorCode::Io);
  }
  const auto size = strnlen(text, clipboard_max_text_bytes + 1);
  if (size > clipboard_max_text_bytes) {
    return ClipboardFailure<std::string>(IoErrorCode::TooLarge);
  }
  return ClipboardText(std::string(text, size));
}

IoResult<Bytes> ReadBytes(OH_UdmfRecord* record, const char* type, std::size_t limit) {
  unsigned char* data = nullptr;
  unsigned int size = 0;
  if (OH_UdmfRecord_GetGeneralEntry(record, type, &data, &size) != UDMF_E_OK || (!data && size != 0)) {
    return ClipboardFailure<Bytes>(IoErrorCode::Io);
  }
  if (size > limit) {
    return ClipboardFailure<Bytes>(IoErrorCode::TooLarge);
  }
  if (size == 0) {
    return IoResult<Bytes>::Success({});
  }
  const auto* begin = reinterpret_cast<const std::byte*>(data);
  return IoResult<Bytes>::Success(Bytes(begin, begin + size));
}

bool HasType(std::span<char* const> types, std::string_view type) {
  return std::ranges::any_of(types, [type](const char* value) { return value && type == value; });
}

IoResult<ImageAsset> ReadImage(OH_UdmfRecord* record, std::span<char* const> types) {
  for (const auto* mime : image_types) {
    if (HasType(types, mime)) {
      auto bytes = ReadBytes(record, mime, clipboard_max_image_bytes);
      auto image = bytes.Succeeded() ? ClipboardImage(std::move(bytes).Value())
                                     : IoResult<ImageAsset>::Failure(std::move(bytes).Error());
      if (image.Succeeded() || !HasType(types, pixelmap_type)) {
        return image;
      }
      break;
    }
  }

  const auto value = Own(OH_UdsPixelMap_Create(), &OH_UdsPixelMap_Destroy);
  if (!value || OH_UdmfRecord_GetPixelMap(record, value.get()) != UDMF_E_OK) {
    return ClipboardFailure<ImageAsset>(IoErrorCode::Io);
  }
  try {
    const auto pixelmap = CreateOhosPixelmap(1, 1);
    // UDMF replaces the image held by the supplied PixelMap wrapper.
    OH_UdsPixelMap_GetPixelMap(value.get(), pixelmap.get());
    const auto [width, height] = OhosPixelmapDimensions(pixelmap.get());
    if (!ClipboardImageSizeAllowed(width, height)) {
      return ClipboardFailure<ImageAsset>(IoErrorCode::TooLarge);
    }
    return ClipboardImage(EncodeOhosPixelmap(pixelmap.get(), {}, clipboard_max_image_bytes));
  } catch (const std::length_error&) {
    return ClipboardFailure<ImageAsset>(IoErrorCode::TooLarge);
  } catch (const std::exception&) {
    return ClipboardFailure<ImageAsset>(IoErrorCode::Io);
  }
}

bool HasImage(std::span<char* const> types) {
  return HasType(types, pixelmap_type) ||
         std::ranges::any_of(image_types, [types](const char* type) { return HasType(types, type); });
}

IoResult<ClipboardItem> ReadRecord(OH_UdmfRecord* record, std::span<char* const> types,
                                   const ClipboardReadOptions& options) {
  ClipboardItemRead item;
  if (options.text && HasType(types, plain_text_type)) {
    const auto value = Own(OH_UdsPlainText_Create(), &OH_UdsPlainText_Destroy);
    if (value && OH_UdmfRecord_GetPlainText(record, value.get()) == UDMF_E_OK) {
      item.SetText(&ClipboardItem::text, ReadString(OH_UdsPlainText_GetContent(value.get())));
    }
  }
  if (options.html && HasType(types, html_type)) {
    const auto value = Own(OH_UdsHtml_Create(), &OH_UdsHtml_Destroy);
    if (value && OH_UdmfRecord_GetHtml(record, value.get()) == UDMF_E_OK) {
      item.SetText(&ClipboardItem::html, ReadString(OH_UdsHtml_GetContent(value.get())));
    }
  }
  if (options.image && HasImage(types)) {
    item.SetImage(ReadImage(record, types));
  }
  for (const auto& mime : options.custom_mime_types) {
    if (HasType(types, mime)) {
      item.SetCustom(mime, ReadBytes(record, mime.c_str(), item.RemainingCustomBytes()));
    } else if (const auto type = ClipboardTypeIdentifier(mime); HasType(types, type)) {
      auto marker = ReadBytes(record, type.c_str(), 1);
      if (marker.Succeeded() && marker.Value() == Bytes{std::byte{1}}) {
        item.SetCustom(mime, IoResult<Bytes>::Success({}));
      }
    }
  }
  return std::move(item).Finish();
}

IoResult<ClipboardItem> ReadItem(ClipboardReadOptions options) {
  auto data = ReadData();
  if (!data.Succeeded()) {
    return IoResult<ClipboardItem>::Failure(std::move(data).Error());
  }
  unsigned int count = 0;
  auto** records = OH_UdmfData_GetRecords(data.Value().get(), &count);
  for (unsigned int index = 0; records && index < count; ++index) {
    unsigned int type_count = 0;
    auto** type_names = OH_UdmfRecord_GetTypes(records[index], &type_count);
    if (!type_names) {
      continue;
    }
    const std::span<char* const> types(type_names, type_count);
    if ((options.text && HasType(types, plain_text_type)) || (options.html && HasType(types, html_type)) ||
        (options.image && HasImage(types)) ||
        std::ranges::any_of(options.custom_mime_types, [&](const auto& mime) {
          return HasType(types, mime) || HasType(types, ClipboardTypeIdentifier(mime));
        })) {
      return ReadRecord(records[index], types, options);
    }
  }
  return ClipboardFailure(IoErrorCode::NotFound);
}

IoResult<std::vector<std::string>> ReadUris() {
  auto data = ReadData();
  if (!data.Succeeded()) {
    return IoResult<std::vector<std::string>>::Failure(std::move(data).Error());
  }
  std::vector<std::string> uris;
  unsigned int count = 0;
  auto** records = OH_UdmfData_GetRecords(data.Value().get(), &count);
  for (unsigned int index = 0; records && index < count; ++index) {
    unsigned int type_count = 0;
    auto** types = OH_UdmfRecord_GetTypes(records[index], &type_count);
    if (!types || !HasType(std::span<char* const>(types, type_count), file_uri_type)) {
      continue;
    }
    if (uris.size() == clipboard_max_files) {
      return ClipboardFailure<std::vector<std::string>>(IoErrorCode::TooLarge);
    }
    const auto value = Own(OH_UdsFileUri_Create(), &OH_UdsFileUri_Destroy);
    if (!value || OH_UdmfRecord_GetFileUri(records[index], value.get()) != UDMF_E_OK) {
      return ClipboardFailure<std::vector<std::string>>(IoErrorCode::Io);
    }
    const auto* uri = OH_UdsFileUri_GetFileUri(value.get());
    if (!uri || !uri[0]) {
      return ClipboardFailure<std::vector<std::string>>(IoErrorCode::Io);
    }
    uris.emplace_back(uri);
  }
  if (uris.empty()) {
    return ClipboardFailure<std::vector<std::string>>(IoErrorCode::NotFound);
  }
  return IoResult<std::vector<std::string>>::Success(std::move(uris));
}

Task<IoResult<std::vector<FileReference>>> OpenClipboardFiles(std::weak_ptr<OhosFileAccess> file_access) {
  auto uris = co_await RunWorker(ReadUris);
  if (!uris.Succeeded()) {
    co_return std::move(uris).Error();
  }
  if (const auto access = file_access.lock()) {
    co_return co_await access->OpenUris(std::move(uris).Value());
  }
  co_return ClipboardFailure<std::vector<FileReference>>(IoErrorCode::Unsupported);
}

} // namespace

OhosClipboard::OhosClipboard(std::weak_ptr<OhosFileAccess> file_access) : file_access_(std::move(file_access)) {}

std::optional<std::string> OhosClipboard::ReadText() {
  auto data = ReadData();
  const auto value = Own(OH_UdsPlainText_Create(), &OH_UdsPlainText_Destroy);
  if (!data.Succeeded() || !value ||
      OH_UdmfData_GetPrimaryPlainText(data.Value().get(), value.get()) != UDMF_E_OK) {
    return std::nullopt;
  }
  auto text = ReadString(OH_UdsPlainText_GetContent(value.get()));
  if (text.Succeeded()) {
    return std::move(text).Value();
  }
  return std::nullopt;
}

bool OhosClipboard::WriteText(std::string_view text) {
  ClipboardItem item;
  item.text.emplace(text);
  return WriteItem(std::move(item));
}

bool OhosClipboard::WriteItem(ClipboardItem item) {
  const auto clipboard = Own(OH_Pasteboard_Create(), &OH_Pasteboard_Destroy);
  const ClipboardData data(OH_UdmfData_Create(), &OH_UdmfData_Destroy);
  const auto record = Own(OH_UdmfRecord_Create(), &OH_UdmfRecord_Destroy);
  if (!clipboard || !data || !record || (item.text && !AddPlainText(record.get(), *item.text)) ||
      (item.html && !AddHtml(record.get(), *item.html, item.text)) ||
      (item.image && !AddImage(record.get(), *item.image))) {
    return false;
  }
  for (const auto& [mime, bytes] : item.custom_formats) {
    if (!AddCustom(record.get(), mime, bytes)) {
      return false;
    }
  }
  return OH_UdmfData_AddRecord(data.get(), record.get()) == UDMF_E_OK &&
         OH_Pasteboard_SetData(clipboard.get(), data.get()) == ERR_OK;
}

Task<IoResult<ClipboardItem>> OhosClipboard::ReadItemAsync(ClipboardReadOptions options) {
  return RunWorker(ReadItem, std::move(options));
}

bool OhosClipboard::WriteFiles(ClipboardFiles files) {
  const auto clipboard = Own(OH_Pasteboard_Create(), &OH_Pasteboard_Destroy);
  const ClipboardData data(OH_UdmfData_Create(), &OH_UdmfData_Destroy);
  if (!clipboard || !data) {
    return false;
  }
  const bool added = std::visit([&](const auto& entries) {
    for (const auto& entry : entries) {
      const auto uri = OhosFileUri(entry);
      const auto value = Own(OH_UdsFileUri_Create(), &OH_UdsFileUri_Destroy);
      const auto record = Own(OH_UdmfRecord_Create(), &OH_UdmfRecord_Destroy);
      if (!uri || !value || !record || OH_UdsFileUri_SetFileUri(value.get(), uri->c_str()) != UDMF_E_OK ||
          OH_UdmfRecord_AddFileUri(record.get(), value.get()) != UDMF_E_OK ||
          OH_UdmfData_AddRecord(data.get(), record.get()) != UDMF_E_OK) {
        return false;
      }
    }
    return true;
  }, files);
  return added && OH_Pasteboard_SetData(clipboard.get(), data.get()) == ERR_OK;
}

Task<IoResult<std::vector<FileReference>>> OhosClipboard::ReadFilesAsync() {
  return OpenClipboardFiles(file_access_);
}

bool OhosClipboard::MayHaveText() const noexcept {
  const auto clipboard = Own(OH_Pasteboard_Create(), &OH_Pasteboard_Destroy);
  return clipboard && (OH_Pasteboard_HasType(clipboard.get(), plain_text_type) ||
                       OH_Pasteboard_HasType(clipboard.get(), html_type));
}

} // namespace huxerui::detail
