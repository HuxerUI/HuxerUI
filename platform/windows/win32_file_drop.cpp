#include "win32_file_internal.h"

#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/file_drop.h>

#include "io/file_internal.h"
#include "internal_access.h"
#include "macros_internal.h"
#include "runtime/gesture_internal.h"

namespace huxerui::detail {
namespace {

UINT ApplicationDragFormat() {
  static const UINT format = RegisterClipboardFormatW(L"HuxerUI.ApplicationDrag");
  return format;
}

class Win32ApplicationDrag final : public IDataObject, public IDropSource {
public:
  Win32ApplicationDrag(std::shared_ptr<WindowDrag> drag, bool swapped_buttons)
      : drag_(std::move(drag)), button_(swapped_buttons ? MK_RBUTTON : MK_LBUTTON) {}

  const std::shared_ptr<WindowDrag>& Drag() const noexcept { return drag_; }
  bool Released() const noexcept { return released_; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
    if (!value) return E_POINTER;
    *value = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDataObject)) {
      *value = static_cast<IDataObject*>(this);
    } else if (IsEqualIID(iid, IID_IDropSource)) {
      *value = static_cast<IDropSource*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = --references_;
    if (!remaining) delete this;
    return remaining;
  }

  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
    if (escape || !drag_->Active()) return DRAGDROP_S_CANCEL;
    if (!(keys & button_)) {
      released_ = true;
      return DRAGDROP_S_DROP;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

  HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
    if (!medium) return E_POINTER;
    *medium = {};
    const HRESULT match = QueryGetData(format);
    if (FAILED(match)) return match;
    medium->hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, 1);
    if (!medium->hGlobal) return E_OUTOFMEMORY;
    medium->tymed = TYMED_HGLOBAL;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return DV_E_FORMATETC; }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
    if (!format) return E_POINTER;
    return format->cfFormat == ApplicationDragFormat() && (format->tymed & TYMED_HGLOBAL) &&
           format->dwAspect == DVASPECT_CONTENT && format->lindex == -1 ? S_OK : DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) override {
    if (!output) return E_POINTER;
    *output = {};
    return DATA_S_SAMEFORMATETC;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** value) override {
    if (!value) return E_POINTER;
    *value = nullptr;
    if (direction != DATADIR_GET) return E_NOTIMPL;
    const FORMATETC format{static_cast<CLIPFORMAT>(ApplicationDragFormat()), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return SHCreateStdEnumFmtEtc(1, &format, value);
  }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
  std::atomic<ULONG> references_{1};
  std::shared_ptr<WindowDrag> drag_;
  DWORD button_;
  bool released_ = false;
};

// OLE presents the source IDataObject directly to targets in the same thread.
thread_local Win32ApplicationDrag* current_application_drag = nullptr;

std::shared_ptr<WindowDrag> LocalApplicationDrag(IDataObject* object) {
  if (current_application_drag && object == static_cast<IDataObject*>(current_application_drag)) {
    return current_application_drag->Drag();
  }
  return {};
}

FORMATETC ShellFileFormat() {
  return {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
}

FileDropPreparation CaptureFiles(IDataObject* object) {
  FORMATETC format = ShellFileFormat();
  STGMEDIUM medium{};
  if (!object || FAILED(object->GetData(&format, &medium))) {
    throw std::runtime_error("HuxerUI could not obtain Windows dropped files");
  }
  struct ReleaseMedium {
    STGMEDIUM& medium;
    ~ReleaseMedium() { ReleaseStgMedium(&medium); }
  } release{medium};
  if (medium.tymed != TYMED_HGLOBAL || !medium.hGlobal) {
    throw std::runtime_error("HuxerUI received an invalid Windows file drop");
  }
  const auto drop = static_cast<HDROP>(medium.hGlobal);
  const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  std::vector<std::wstring> paths;
  paths.reserve(count);
  for (UINT index = 0; index < count; ++index) {
    const UINT length = DragQueryFileW(drop, index, nullptr, 0);
    if (length == 0) {
      throw std::runtime_error("HuxerUI received an empty Windows file drop path");
    }
    std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
    if (DragQueryFileW(drop, index, path.data(), length + 1) != length) {
      throw std::runtime_error("HuxerUI could not read a Windows file drop path");
    }
    path.resize(length);
    paths.push_back(std::move(path));
  }
  return {[paths = std::move(paths)](FileDropCompletion completion) {
    auto canceled = std::make_shared<std::atomic<bool>>(false);
    EnqueueFileOperation([paths, canceled, completion = std::move(completion)]() mutable {
      std::vector<FileReference> files;
      for (const auto& path : paths) {
        if (*canceled) {
          return;
        }
        auto file = MakeWin32FileReference(path, false);
        if (!file) {
          completion(IoResult<std::vector<FileReference>>(
              IoError{IoErrorCode::Io, "HuxerUI could not retain every dropped ordinary file"}));
          return;
        }
        files.push_back(std::move(*file));
      }
      if (!*canceled) {
        completion(IoResult<std::vector<FileReference>>(std::move(files)));
      }
    });
    return [canceled] { *canceled = true; };
  }};
}

} // namespace

void RunWin32ApplicationDrag(const std::shared_ptr<WindowDrag>& drag, bool swapped_buttons) {
  if (current_application_drag || !drag->Active()) {
    drag->Finish(true);
    return;
  }
  struct ReleaseDrag {
    void operator()(Win32ApplicationDrag* drag) const noexcept { drag->Release(); }
  };
  std::unique_ptr<Win32ApplicationDrag, ReleaseDrag> operation(new Win32ApplicationDrag(drag, swapped_buttons));
  current_application_drag = operation.get();
  DWORD effect = DROPEFFECT_NONE;
  const HRESULT result = DoDragDrop(operation.get(), operation.get(), DROPEFFECT_COPY, &effect);
  current_application_drag = nullptr;
  drag->Finish(FAILED(result) || (result == DRAGDROP_S_CANCEL && !operation->Released()));
}

class Win32FileDrop::Target final : public IDropTarget {
public:
  Target(HWND platform_window, UiWindow& ui_window, std::function<float()> scale)
      : platform_window_(platform_window), ui_window_(&ui_window), scale_(std::move(scale)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (!object) {
      return E_POINTER;
    }
    *object = nullptr;
    if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IDropTarget)) {
      return E_NOINTERFACE;
    }
    *object = static_cast<IDropTarget*>(this);
    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = --references_;
    if (remaining == 0) {
      delete this;
    }
    return remaining;
  }

  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* object, DWORD, POINTL point, DWORD* effect) override {
    if (!effect) {
      return E_POINTER;
    }
    const bool copy = (*effect & DROPEFFECT_COPY) != 0;
    *effect = DROPEFFECT_NONE;
    Leave();
    if (auto drag = LocalApplicationDrag(object)) {
      application_drag_ = std::move(drag);
      MoveApplicationDrag(point, copy, effect);
      return S_OK;
    }
    FORMATETC format = ShellFileFormat();
    offered_ = object && SUCCEEDED(object->QueryGetData(&format));
    if (!ui_window_ || !offered_ || !copy) {
      return S_OK;
    }
    ++session_;
    hovering_ = true;
    try {
      const bool accepted = ui_window_->HandleFileDragEntered(session_, {}, Position(point));
      *effect = copy && accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    } catch (...) {
      Leave();
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override {
    if (!effect) {
      return E_POINTER;
    }
    const bool copy = (*effect & DROPEFFECT_COPY) != 0;
    *effect = DROPEFFECT_NONE;
    if (application_drag_) {
      MoveApplicationDrag(point, copy, effect);
      return S_OK;
    }
    if (!copy) {
      EndHover();
      return S_OK;
    }
    if (ui_window_ && offered_) {
      try {
        const bool entered = !hovering_;
        if (entered) {
          ++session_;
          hovering_ = true;
        }
        const bool accepted = entered ? ui_window_->HandleFileDragEntered(session_, {}, Position(point))
                                      : ui_window_->HandleFileDragMoved(session_, {}, Position(point));
        *effect = copy && accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
      } catch (...) {
        Leave();
      }
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DragLeave() override {
    Leave();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Drop(IDataObject* object, DWORD, POINTL point, DWORD* effect) override {
    if (!effect) {
      return E_POINTER;
    }
    const bool copy = (*effect & DROPEFFECT_COPY) != 0;
    *effect = DROPEFFECT_NONE;
    if (auto drag = std::exchange(application_drag_, {})) {
      if (ui_window_ && copy && LocalApplicationDrag(object) == drag) {
        ExecutionGuard guard(InternalAccess::WindowExecution(*ui_window_));
        try {
          if (drag->Drop(*ui_window_, Position(point))) *effect = DROPEFFECT_COPY;
        } catch (...) {}
      }
      return S_OK;
    }
    if (ui_window_ && offered_ && copy) {
      try {
        auto source = CaptureFiles(object);
        if (ui_window_->HandleFileDrop(session_, {}, Position(point), std::move(source))) {
          *effect = DROPEFFECT_COPY;
        }
      } catch (...) {
        Leave();
      }
    }
    Leave();
    return S_OK;
  }

  void Detach() noexcept {
    Leave();
    ui_window_ = nullptr;
    scale_ = {};
  }

private:
  void MoveApplicationDrag(POINTL point, bool copy, DWORD* effect) noexcept {
    if (!ui_window_) return;
    const auto drag = application_drag_;
    ExecutionGuard guard(InternalAccess::WindowExecution(*ui_window_));
    try {
      if (copy) {
        if (drag->Move(*ui_window_, Position(point))) *effect = DROPEFFECT_COPY;
      } else {
        drag->Leave(*ui_window_);
      }
    } catch (...) {}
  }

  Point Position(POINTL point) const {
    POINT client{point.x, point.y};
    if (!ScreenToClient(platform_window_, &client)) {
      throw std::runtime_error("HuxerUI could not map the Windows file drop position");
    }
    const float scale = scale_();
    if (!std::isfinite(scale) || scale <= 0.0F) {
      throw std::runtime_error("HuxerUI Windows file drop has an invalid DPI scale");
    }
    return {static_cast<float>(client.x) / scale, static_cast<float>(client.y) / scale};
  }

  void EndHover() noexcept {
    const bool hovering = std::exchange(hovering_, false);
    if (ui_window_ && hovering) {
      try {
        ui_window_->HandleFileDragExited(session_);
      } catch (...) {
      }
    }
  }

  void Leave() noexcept {
    if (auto drag = std::exchange(application_drag_, {}); drag && ui_window_) {
      ExecutionGuard guard(InternalAccess::WindowExecution(*ui_window_));
      try { drag->Leave(*ui_window_); } catch (...) {}
    }
    offered_ = false;
    EndHover();
  }

  std::atomic<ULONG> references_{1};
  HWND platform_window_;
  UiWindow* ui_window_;
  std::function<float()> scale_;
  std::shared_ptr<WindowDrag> application_drag_;
  std::uint64_t session_ = 0;
  bool offered_ = false;
  bool hovering_ = false;
};

Win32FileDrop::Win32FileDrop(HWND platform_window, UiWindow& ui_window, std::function<float()> scale)
    : platform_window_(platform_window), target_(new Target(platform_window, ui_window, std::move(scale))) {
  if (FAILED(RegisterDragDrop(platform_window_, target_))) {
    target_->Release();
    throw std::runtime_error("HuxerUI could not register Windows file drop reception");
  }
}

Win32FileDrop::~Win32FileDrop() {
  target_->Detach();
  HUXERUI_DISCARD(RevokeDragDrop(platform_window_));
  target_->Release();
}

} // namespace huxerui::detail
