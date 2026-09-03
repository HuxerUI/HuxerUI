#include <huxerui/windows/external_texture.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "win32_external_texture_internal.h"

namespace huxerui::detail {
namespace {

using Microsoft::WRL::ComPtr;

void ThrowIfFailed(HRESULT result, const char* message) {
  if (FAILED(result)) {
    throw std::runtime_error(message);
  }
}

class SharedHandle final {
public:
  SharedHandle() noexcept = default;
  ~SharedHandle() {
    if (value_ != nullptr) {
      CloseHandle(value_);
    }
  }

  SharedHandle(const SharedHandle&) = delete;
  SharedHandle& operator=(const SharedHandle&) = delete;

  [[nodiscard]] HANDLE* Address() noexcept {
    return &value_;
  }

  [[nodiscard]] HANDLE Get() const noexcept {
    return value_;
  }

  [[nodiscard]] HANDLE Release() noexcept {
    return std::exchange(value_, nullptr);
  }

private:
  HANDLE value_ = nullptr;
};

class KeyedMutexLease final {
public:
  explicit KeyedMutexLease(ComPtr<IDXGIKeyedMutex> mutex) : mutex_(std::move(mutex)) {}

  ~KeyedMutexLease() {
    if (mutex_) {
      static_cast<void>(mutex_->ReleaseSync(release_key_));
    }
  }

  KeyedMutexLease(const KeyedMutexLease&) = delete;
  KeyedMutexLease& operator=(const KeyedMutexLease&) = delete;

  void SetReleaseKey(UINT64 key) noexcept {
    release_key_ = key;
  }

  void Release() {
    const HRESULT result = mutex_->ReleaseSync(release_key_);
    mutex_.Reset();
    ThrowIfFailed(result, "HuxerUI could not release a Windows D3D11 texture snapshot");
  }

private:
  ComPtr<IDXGIKeyedMutex> mutex_;
  UINT64 release_key_ = 0;
};

void WaitForCopy(ID3D11Device& device, ID3D11DeviceContext& context) {
  D3D11_QUERY_DESC query_description{};
  query_description.Query = D3D11_QUERY_EVENT;
  ComPtr<ID3D11Query> query;
  ThrowIfFailed(
      device.CreateQuery(&query_description, query.GetAddressOf()),
      "HuxerUI could not create a Windows D3D11 texture completion query"
  );
  context.End(query.Get());
  context.Flush();
  HRESULT result = context.GetData(query.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
  while (result == S_FALSE) {
    std::this_thread::yield();
    result = context.GetData(query.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
  }
  ThrowIfFailed(result, "HuxerUI could not complete a Windows D3D11 texture snapshot");
}

std::shared_ptr<const Win32D3D11Frame> CopyD3D11Frame(const windows::D3D11Texture::Frame& frame) {
#if defined(HUXERUI_WINDOWS_7_COMPAT)
  static_cast<void>(frame);
  throw std::runtime_error("HuxerUI Windows D3D11 textures require Windows 10 or later");
#else
  if (frame.texture == nullptr) {
    throw std::invalid_argument("HuxerUI Windows D3D11 texture source must not be null");
  }
  switch (frame.alpha) {
  case windows::D3D11Texture::Alpha::Opaque:
  case windows::D3D11Texture::Alpha::Premultiplied:
    break;
  default:
    throw std::invalid_argument("HuxerUI Windows D3D11 texture alpha mode is not supported");
  }

  D3D11_TEXTURE2D_DESC source_description{};
  frame.texture->GetDesc(&source_description);
  if (source_description.Width == 0 || source_description.Height == 0 || source_description.MipLevels != 1 ||
      source_description.ArraySize != 1 || source_description.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
      source_description.SampleDesc.Count != 1 || source_description.SampleDesc.Quality != 0 ||
      source_description.Usage != D3D11_USAGE_DEFAULT || source_description.CPUAccessFlags != 0) {
    throw std::invalid_argument(
        "HuxerUI Windows D3D11 texture source must be a single-sampled, one-mip, one-slice "
        "DXGI_FORMAT_B8G8R8A8_UNORM D3D11_USAGE_DEFAULT texture without CPU access"
    );
  }
  if (source_description.Width > static_cast<UINT>(std::numeric_limits<int>::max()) ||
      source_description.Height > static_cast<UINT>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("HuxerUI Windows D3D11 texture dimensions are too large");
  }

  ComPtr<ID3D11Device> device;
  frame.texture->GetDevice(device.GetAddressOf());
  if (!device) {
    throw std::runtime_error("HuxerUI could not access the Windows D3D11 texture device");
  }
  ThrowIfFailed(
      device->GetDeviceRemovedReason(),
      "HuxerUI cannot publish a Windows D3D11 texture from a removed device"
  );

  D3D11_TEXTURE2D_DESC snapshot_description = source_description;
  snapshot_description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  snapshot_description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  ComPtr<ID3D11Texture2D> snapshot;
  ThrowIfFailed(
      device->CreateTexture2D(&snapshot_description, nullptr, snapshot.GetAddressOf()),
      "HuxerUI could not create a shared Windows D3D11 texture snapshot"
  );

  ComPtr<IDXGIKeyedMutex> keyed_mutex;
  ThrowIfFailed(snapshot.As(&keyed_mutex), "HuxerUI could not synchronize a shared Windows D3D11 texture snapshot");
  ThrowIfFailed(
      keyed_mutex->AcquireSync(0, INFINITE),
      "HuxerUI could not acquire a shared Windows D3D11 texture snapshot"
  );
  KeyedMutexLease lease(std::move(keyed_mutex));

  ComPtr<ID3D11DeviceContext> context;
  device->GetImmediateContext(context.GetAddressOf());
  if (!context) {
    throw std::runtime_error("HuxerUI could not access the Windows D3D11 immediate context");
  }
  context->CopyResource(snapshot.Get(), frame.texture);
  WaitForCopy(*device.Get(), *context.Get());

  ComPtr<IDXGIResource1> resource;
  ThrowIfFailed(snapshot.As(&resource), "HuxerUI could not share a Windows D3D11 texture snapshot");
  SharedHandle shared_handle;
  ThrowIfFailed(
      resource->CreateSharedHandle(
          nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, shared_handle.Address()
      ),
      "HuxerUI could not create a Windows D3D11 texture shared handle"
  );

  lease.SetReleaseKey(1);
  lease.Release();
  auto copied = std::make_shared<Win32D3D11Frame>(
      static_cast<int>(source_description.Width), static_cast<int>(source_description.Height), frame.alpha,
      shared_handle.Get(), std::move(snapshot)
  );
  static_cast<void>(shared_handle.Release());
  return copied;
#endif
}

Win32PixelFrame CopyFrame(const windows::PixelFrame& frame) {
  if (frame.pixel_width <= 0 || frame.pixel_height <= 0) {
    throw std::invalid_argument("HuxerUI Windows external texture frame dimensions must be positive");
  }
  switch (frame.format) {
  case windows::PixelFormat::Rgba8888:
  case windows::PixelFormat::Bgra8888:
    break;
  default:
    throw std::invalid_argument("HuxerUI Windows external texture pixel format is not supported");
  }

  const std::size_t width = static_cast<std::size_t>(frame.pixel_width);
  const std::size_t height = static_cast<std::size_t>(frame.pixel_height);
  if (width > std::numeric_limits<std::size_t>::max() / 4U) {
    throw std::invalid_argument("HuxerUI Windows external texture frame dimensions are too large");
  }
  const std::size_t row_bytes = width * 4U;
  if (row_bytes > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::invalid_argument("HuxerUI Windows external texture frame dimensions are too large");
  }
  if (frame.bytes_per_row < row_bytes) {
    throw std::invalid_argument("HuxerUI Windows external texture row stride is too small");
  }
  if (height > 1U && frame.bytes_per_row > (std::numeric_limits<std::size_t>::max() - row_bytes) / (height - 1U)) {
    throw std::invalid_argument("HuxerUI Windows external texture frame dimensions are too large");
  }
  const std::size_t required_bytes = (height - 1U) * frame.bytes_per_row + row_bytes;
  if (frame.pixels.size() < required_bytes) {
    throw std::invalid_argument("HuxerUI Windows external texture pixel buffer is too small");
  }
  if (height > std::numeric_limits<std::size_t>::max() / row_bytes) {
    throw std::invalid_argument("HuxerUI Windows external texture frame dimensions are too large");
  }

  std::vector<std::byte> pixels(row_bytes * height);
  for (std::size_t y = 0; y < height; ++y) {
    const std::byte* source_row = frame.pixels.data() + y * frame.bytes_per_row;
    std::byte* destination_row = pixels.data() + y * row_bytes;
    for (std::size_t x = 0; x < width; ++x) {
      const std::byte* source = source_row + x * 4U;
      const auto first = static_cast<std::uint8_t>(source[0]);
      const auto green = static_cast<std::uint8_t>(source[1]);
      const auto third = static_cast<std::uint8_t>(source[2]);
      const auto alpha = static_cast<std::uint8_t>(source[3]);
      const std::uint8_t red = frame.format == windows::PixelFormat::Rgba8888 ? first : third;
      const std::uint8_t blue = frame.format == windows::PixelFormat::Rgba8888 ? third : first;
      const auto premultiply = [alpha](std::uint8_t channel) {
        return static_cast<std::uint8_t>((static_cast<std::uint32_t>(channel) * alpha + 127U) / 255U);
      };
      std::byte* destination = destination_row + x * 4U;
      destination[0] = static_cast<std::byte>(premultiply(blue));
      destination[1] = static_cast<std::byte>(premultiply(green));
      destination[2] = static_cast<std::byte>(premultiply(red));
      destination[3] = static_cast<std::byte>(alpha);
    }
  }
  return Win32PixelFrame(frame.pixel_width, frame.pixel_height, std::move(pixels));
}

} // namespace

Win32D3D11Frame::Win32D3D11Frame(
    int pixel_width, int pixel_height, windows::D3D11Texture::Alpha alpha, HANDLE shared_handle,
    ComPtr<ID3D11Texture2D> texture
) noexcept
    : pixel_width_(pixel_width), pixel_height_(pixel_height), alpha_(alpha), shared_handle_(shared_handle),
      texture_(std::move(texture)) {}

Win32D3D11Frame::~Win32D3D11Frame() {
  if (shared_handle_ != nullptr) {
    CloseHandle(shared_handle_);
  }
}

} // namespace huxerui::detail

namespace huxerui::windows {

struct PixelTexture::Storage {
  std::mutex mutex;
  std::shared_ptr<const detail::Win32PixelFrame> frame;
  bool finished = false;
};

struct D3D11Texture::Storage {
  std::mutex mutex;
  std::shared_ptr<const detail::Win32D3D11Frame> frame;
  bool finished = false;
};

PixelTexture::PixelTexture(Size intrinsic_size)
    : huxerui::ExternalTexture(intrinsic_size), storage_(std::make_unique<Storage>()) {}

PixelTexture::~PixelTexture() {
  Finish();
}

void PixelTexture::Publish(const PixelFrame& frame) {
  auto copied = std::make_shared<const detail::Win32PixelFrame>(detail::CopyFrame(frame));
  {
    std::lock_guard lock(storage_->mutex);
    if (storage_->finished) {
      throw std::logic_error("HuxerUI Windows external texture is finished");
    }
    storage_->frame = std::move(copied);
  }
  NotifyFrameAvailable();
}

void PixelTexture::Finish() noexcept {
  std::lock_guard lock(storage_->mutex);
  storage_->finished = true;
}

std::shared_ptr<const detail::Win32PixelFrame> PixelTexture::AcquireFrame() const noexcept {
  std::lock_guard lock(storage_->mutex);
  return storage_->frame;
}

D3D11Texture::D3D11Texture(Size intrinsic_size)
    : huxerui::ExternalTexture(intrinsic_size), storage_(std::make_unique<Storage>()) {}

D3D11Texture::~D3D11Texture() {
  Finish();
}

void D3D11Texture::Publish(Frame frame) {
  {
    std::lock_guard lock(storage_->mutex);
    if (storage_->finished) {
      throw std::logic_error("HuxerUI Windows D3D11 texture is finished");
    }
  }
  std::shared_ptr<const detail::Win32D3D11Frame> copied = detail::CopyD3D11Frame(frame);
  {
    std::lock_guard lock(storage_->mutex);
    if (storage_->finished) {
      throw std::logic_error("HuxerUI Windows D3D11 texture is finished");
    }
    storage_->frame = std::move(copied);
  }
  NotifyFrameAvailable();
}

void D3D11Texture::Finish() noexcept {
  std::lock_guard lock(storage_->mutex);
  storage_->finished = true;
}

std::shared_ptr<const detail::Win32D3D11Frame> D3D11Texture::AcquireFrame() const noexcept {
  std::lock_guard lock(storage_->mutex);
  return storage_->frame;
}

} // namespace huxerui::windows

namespace huxerui::detail {

std::shared_ptr<const Win32PixelFrame> GetPixelFrame(const windows::PixelTexture& texture) noexcept {
  return texture.AcquireFrame();
}

std::shared_ptr<const Win32D3D11Frame> GetD3D11Frame(const windows::D3D11Texture& texture) noexcept {
  return texture.AcquireFrame();
}

} // namespace huxerui::detail
