#include "win32_file_internal.h"

#include <ole2.h>
#include <shellapi.h>

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

namespace huxerui::detail {
namespace {

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
          completion(FileResult<std::vector<FileReference>>(
              FileError{FileErrorCode::Io, "HuxerUI could not retain every dropped ordinary file"}
          ));
          return;
        }
        files.push_back(std::move(*file));
      }
      if (!*canceled) {
        completion(FileResult<std::vector<FileReference>>(std::move(files)));
      }
    });
    return [canceled] { *canceled = true; };
  }};
}

} // namespace

class Win32FileDrop::Target final : public IDropTarget {
public:
  Target(HWND window, Runtime& runtime, std::function<float()> scale)
      : window_(window), runtime_(&runtime), scale_(std::move(scale)) {}

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
    FORMATETC format = ShellFileFormat();
    offered_ = object && SUCCEEDED(object->QueryGetData(&format));
    if (!runtime_ || !offered_ || !copy) {
      return S_OK;
    }
    ++session_;
    hovering_ = true;
    try {
      const bool accepted = runtime_->HandleFileDragEntered(session_, {}, Position(point));
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
    if (!copy) {
      EndHover();
      return S_OK;
    }
    if (runtime_ && offered_) {
      try {
        const bool entered = !hovering_;
        if (entered) {
          ++session_;
          hovering_ = true;
        }
        const bool accepted = entered ? runtime_->HandleFileDragEntered(session_, {}, Position(point))
                                      : runtime_->HandleFileDragMoved(session_, {}, Position(point));
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
    if (runtime_ && offered_ && copy) {
      try {
        auto source = CaptureFiles(object);
        if (runtime_->HandleFileDrop(session_, {}, Position(point), std::move(source))) {
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
    runtime_ = nullptr;
    scale_ = {};
  }

private:
  Point Position(POINTL point) const {
    POINT client{point.x, point.y};
    if (!ScreenToClient(window_, &client)) {
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
    if (runtime_ && hovering) {
      try {
        runtime_->HandleFileDragExited(session_);
      } catch (...) {
      }
    }
  }

  void Leave() noexcept {
    offered_ = false;
    EndHover();
  }

  std::atomic<ULONG> references_{1};
  HWND window_;
  Runtime* runtime_;
  std::function<float()> scale_;
  std::uint64_t session_ = 0;
  bool offered_ = false;
  bool hovering_ = false;
};

Win32FileDrop::Win32FileDrop(HWND window, Runtime& runtime, std::function<float()> scale)
    : window_(window), target_(new Target(window, runtime, std::move(scale))) {
  if (FAILED(RegisterDragDrop(window_, target_))) {
    target_->Release();
    throw std::runtime_error("HuxerUI could not register Windows file drop reception");
  }
}

Win32FileDrop::~Win32FileDrop() {
  target_->Detach();
  static_cast<void>(RevokeDragDrop(window_));
  target_->Release();
}

} // namespace huxerui::detail
