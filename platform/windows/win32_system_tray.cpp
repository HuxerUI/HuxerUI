#include "win32_system_tray.h"

#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "win32_internal.h"

namespace huxerui::detail {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT tray_callback_message = WM_APP + 5;
constexpr UINT tray_icon_id = 1;

HBITMAP DecodeBitmap(const ImageAsset& image, UINT width, UINT height) {
  const std::span<const std::byte> bytes = image.EncodedBytes();
  if (bytes.empty() || bytes.size() > std::numeric_limits<DWORD>::max()) {
    throw std::invalid_argument("HuxerUI system tray icon data is invalid");
  }

  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICBitmapScaler> scaler;
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(
          CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()))
      ) ||
      FAILED(factory->CreateStream(stream.GetAddressOf())) ||
      FAILED(stream->InitializeFromMemory(
          reinterpret_cast<BYTE*>(const_cast<std::byte*>(bytes.data())),
          static_cast<DWORD>(bytes.size())
      )) ||
      FAILED(
          factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf())
      ) ||
      FAILED(decoder->GetFrame(0, frame.GetAddressOf())) ||
      FAILED(factory->CreateBitmapScaler(scaler.GetAddressOf()))) {
    throw std::runtime_error("HuxerUI could not decode the Windows system tray icon");
  }

  if (FAILED(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant)) ||
      FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
      FAILED(converter->Initialize(
          scaler.Get(),
          GUID_WICPixelFormat32bppPBGRA,
          WICBitmapDitherTypeNone,
          nullptr,
          0.0,
          WICBitmapPaletteTypeMedianCut
      ))) {
    throw std::runtime_error("HuxerUI could not scale the Windows system tray icon");
  }

  const UINT stride = width * 4U;
  std::vector<BYTE> pixels(static_cast<std::size_t>(stride) * height);
  if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data()))) {
    throw std::runtime_error("HuxerUI could not read the Windows system tray icon pixels");
  }

  BITMAPV5HEADER header{};
  header.bV5Size = sizeof(header);
  header.bV5Width = static_cast<LONG>(width);
  header.bV5Height = -static_cast<LONG>(height);
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;

  void* bitmap_pixels = nullptr;
  HDC screen = GetDC(nullptr);
  HBITMAP color =
      CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bitmap_pixels, nullptr, 0);
  if (screen != nullptr) {
    ReleaseDC(nullptr, screen);
  }
  if (color == nullptr || bitmap_pixels == nullptr) {
    if (color != nullptr) {
      DeleteObject(color);
    }
    throw std::runtime_error("HuxerUI could not create a Windows system tray bitmap");
  }
  std::memcpy(bitmap_pixels, pixels.data(), pixels.size());
  return color;
}

HICON DecodeTrayIcon(const ImageAsset& image) {
  const UINT width = static_cast<UINT>(std::max(1, GetSystemMetrics(SM_CXSMICON)));
  const UINT height = static_cast<UINT>(std::max(1, GetSystemMetrics(SM_CYSMICON)));
  HBITMAP color = DecodeBitmap(image, width, height);
  HBITMAP mask = CreateBitmap(static_cast<int>(width), static_cast<int>(height), 1, 1, nullptr);
  if (mask == nullptr) {
    DeleteObject(color);
    throw std::runtime_error("HuxerUI could not create the Windows system tray icon mask");
  }

  ICONINFO icon_info{
      .fIcon = TRUE,
      .hbmMask = mask,
      .hbmColor = color,
  };
  HICON icon = CreateIconIndirect(&icon_info);
  DeleteObject(mask);
  DeleteObject(color);
  if (icon == nullptr) {
    throw std::runtime_error("HuxerUI could not create the Windows system tray icon");
  }
  return icon;
}

} // namespace

struct Win32SystemTrayTransport::State {
  struct Command {
    UINT menu_id = 0;
    std::uint64_t command = 0;
  };

  State() : taskbar_created(RegisterWindowMessageW(L"TaskbarCreated")) {}

  ~State() {
    Hide();
  }

  void Show(const ResolvedSystemTrayPresentation& value) {
    if (window == nullptr) {
      throw std::logic_error("HuxerUI Windows system tray requires an active application window");
    }
    ResolvedSystemTrayPresentation replacement_presentation = value;
    HICON replacement = DecodeTrayIcon(value.icon);
    if (!Commit(window, added ? NIM_MODIFY : NIM_ADD, replacement, &replacement_presentation)) {
      DestroyIcon(replacement);
      throw std::runtime_error("HuxerUI could not publish the Windows system tray icon");
    }
    const HICON previous = icon;
    icon = replacement;
    presentation = std::move(replacement_presentation);
    if (!added) {
      NOTIFYICONDATAW version = Data(window, icon, &*presentation);
      version.uVersion = NOTIFYICON_VERSION_4;
      static_cast<void>(Shell_NotifyIconW(NIM_SETVERSION, &version));
      added = true;
    }
    if (previous != nullptr) {
      DestroyIcon(previous);
    }
  }

  void Hide() noexcept {
    if (added && window != nullptr) {
      NOTIFYICONDATAW data = Data(window, icon, presentation ? &*presentation : nullptr);
      static_cast<void>(Shell_NotifyIconW(NIM_DELETE, &data));
    }
    added = false;
    presentation.reset();
    commands.clear();
    if (icon != nullptr) {
      DestroyIcon(icon);
      icon = nullptr;
    }
  }

  static NOTIFYICONDATAW Data(HWND target, HICON icon, const ResolvedSystemTrayPresentation* presentation) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = target;
    data.uID = tray_icon_id;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = tray_callback_message;
    data.hIcon = icon;
    if (presentation != nullptr) {
      const std::wstring tooltip = Utf8ToWide(presentation->tooltip);
      wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
    }
    return data;
  }

  static bool Commit(HWND target, DWORD operation, HICON icon, const ResolvedSystemTrayPresentation* presentation) {
    NOTIFYICONDATAW data = Data(target, icon, presentation);
    return Shell_NotifyIconW(operation, &data) != FALSE;
  }

  HMENU BuildMenu(const std::vector<ResolvedSystemTrayMenuEntry>& entries, std::vector<HBITMAP>& bitmaps) {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
      throw std::runtime_error("HuxerUI could not create the Windows system tray menu");
    }
    try {
      for (const ResolvedSystemTrayMenuEntry& entry : entries) {
        if (entry.section) {
          if (AppendMenuW(menu, MF_SEPARATOR, 0, nullptr) == FALSE) {
            throw std::runtime_error("HuxerUI could not append a Windows system tray menu section");
          }
          continue;
        }
        UINT flags = MF_STRING;
        if (!entry.enabled) {
          flags |= MF_GRAYED;
        }
        if (entry.checked.value_or(false)) {
          flags |= MF_CHECKED;
        }
        const std::wstring label = Utf8ToWide(entry.label);
        if (!entry.children.empty()) {
          HMENU child = BuildMenu(entry.children, bitmaps);
          if (AppendMenuW(menu, flags | MF_POPUP, reinterpret_cast<UINT_PTR>(child), label.c_str()) == FALSE) {
            DestroyMenu(child);
            throw std::runtime_error("HuxerUI could not append a Windows system tray submenu");
          }
        } else {
          if (next_menu_id == 0 || next_menu_id > std::numeric_limits<UINT>::max()) {
            throw std::overflow_error("HuxerUI Windows system tray menu contains too many commands");
          }
          const UINT menu_id = static_cast<UINT>(next_menu_id++);
          if (AppendMenuW(menu, flags, menu_id, label.c_str()) == FALSE) {
            throw std::runtime_error("HuxerUI could not append a Windows system tray menu item");
          }
          commands.push_back({menu_id, entry.command});
        }
        if (entry.icon.has_value()) {
          const UINT width = static_cast<UINT>(std::max(1, GetSystemMetrics(SM_CXMENUCHECK)));
          const UINT height = static_cast<UINT>(std::max(1, GetSystemMetrics(SM_CYMENUCHECK)));
          HBITMAP bitmap = DecodeBitmap(*entry.icon, width, height);
          bitmaps.push_back(bitmap);
          MENUITEMINFOW info{};
          info.cbSize = sizeof(info);
          info.fMask = MIIM_BITMAP;
          info.hbmpItem = bitmap;
          const int position = GetMenuItemCount(menu) - 1;
          if (position < 0 || SetMenuItemInfoW(menu, static_cast<UINT>(position), TRUE, &info) == FALSE) {
            throw std::runtime_error("HuxerUI could not set a Windows system tray menu icon");
          }
        }
      }
      return menu;
    } catch (...) {
      DestroyMenu(menu);
      throw;
    }
  }

  void ShowMenu(HWND target) {
    if (!presentation.has_value() || presentation->menu.empty()) {
      return;
    }
    commands.clear();
    next_menu_id = 1;
    std::vector<HBITMAP> bitmaps;
    HMENU menu = nullptr;
    try {
      menu = BuildMenu(presentation->menu, bitmaps);
    } catch (...) {
      for (HBITMAP bitmap : bitmaps) {
        DeleteObject(bitmap);
      }
      throw;
    }
    POINT position{};
    GetCursorPos(&position);
    SetForegroundWindow(target);
    const UINT selected =
        TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, position.x, position.y, target, nullptr);
    DestroyMenu(menu);
    for (HBITMAP bitmap : bitmaps) {
      DeleteObject(bitmap);
    }
    PostMessageW(target, WM_NULL, 0, 0);
    const auto found = std::ranges::find(commands, selected, &Command::menu_id);
    if (found != commands.end() && event_handler) {
      event_handler({
          .type = SystemTrayEventType::Command,
          .generation = presentation->generation,
          .command = found->command,
      });
    }
  }

  HWND window = nullptr;
  std::function<void(SystemTrayEvent)> event_handler;
  std::optional<ResolvedSystemTrayPresentation> presentation;
  std::vector<Command> commands;
  HICON icon = nullptr;
  UINT taskbar_created = 0;
  std::uint64_t next_menu_id = 1;
  bool added = false;
};

Win32SystemTrayTransport::Win32SystemTrayTransport() : state_(std::make_unique<State>()) {}

Win32SystemTrayTransport::~Win32SystemTrayTransport() = default;

bool Win32SystemTrayTransport::IsAvailable() const noexcept {
  return true;
}

void Win32SystemTrayTransport::SetEventHandler(std::function<void(SystemTrayEvent)> handler) {
  state_->event_handler = std::move(handler);
}

void Win32SystemTrayTransport::Show(const ResolvedSystemTrayPresentation& presentation) {
  state_->Show(presentation);
}

void Win32SystemTrayTransport::Hide() noexcept {
  state_->Hide();
}

void Win32SystemTrayTransport::SetWindow(HWND window) noexcept {
  if (state_->window == window) {
    return;
  }
  state_->Hide();
  state_->window = window;
}

std::optional<LRESULT> Win32SystemTrayTransport::HandleMessage(UINT message, WPARAM w_param, LPARAM l_param) {
  static_cast<void>(w_param);
  if (state_->taskbar_created != 0 && message == state_->taskbar_created) {
    state_->added = false;
    if (state_->presentation.has_value()) {
      HWND target = state_->window;
      if (target != nullptr && state_->Commit(target, NIM_ADD, state_->icon, &*state_->presentation)) {
        NOTIFYICONDATAW version = state_->Data(target, state_->icon, &*state_->presentation);
        version.uVersion = NOTIFYICON_VERSION_4;
        static_cast<void>(Shell_NotifyIconW(NIM_SETVERSION, &version));
        state_->added = true;
      }
    }
    return 0;
  }
  if (message != tray_callback_message || !state_->presentation.has_value()) {
    return std::nullopt;
  }
  const UINT event = LOWORD(l_param);
  if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
    if (HWND target = state_->window) {
      state_->ShowMenu(target);
    }
    return 0;
  }
  if ((event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONUP) && state_->event_handler) {
    state_->event_handler({.type = SystemTrayEventType::Activate});
    return 0;
  }
  return 0;
}

} // namespace huxerui::detail
