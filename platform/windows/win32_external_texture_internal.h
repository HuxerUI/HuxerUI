#pragma once

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include <huxerui/windows/external_texture.h>

namespace huxerui::detail {

class Win32D3D11Frame final {
public:
  Win32D3D11Frame(
      int pixel_width, int pixel_height, windows::D3D11Texture::Alpha alpha, HANDLE shared_handle,
      Microsoft::WRL::ComPtr<ID3D11Texture2D> texture
  ) noexcept;
  ~Win32D3D11Frame();

  Win32D3D11Frame(const Win32D3D11Frame&) = delete;
  Win32D3D11Frame& operator=(const Win32D3D11Frame&) = delete;

  [[nodiscard]] int PixelWidth() const noexcept {
    return pixel_width_;
  }

  [[nodiscard]] int PixelHeight() const noexcept {
    return pixel_height_;
  }

  [[nodiscard]] windows::D3D11Texture::Alpha Alpha() const noexcept {
    return alpha_;
  }

  [[nodiscard]] HANDLE SharedHandle() const noexcept {
    return shared_handle_;
  }

  [[nodiscard]] ID3D11Texture2D* Texture() const noexcept {
    return texture_.Get();
  }

private:
  int pixel_width_ = 0;
  int pixel_height_ = 0;
  windows::D3D11Texture::Alpha alpha_ = windows::D3D11Texture::Alpha::Premultiplied;
  HANDLE shared_handle_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
};

class Win32PixelFrame final {
public:
  Win32PixelFrame() noexcept = default;
  Win32PixelFrame(int pixel_width, int pixel_height, std::vector<std::byte> pixels)
      : pixel_width_(pixel_width), pixel_height_(pixel_height), pixels_(std::move(pixels)) {}

  [[nodiscard]] int PixelWidth() const noexcept {
    return pixel_width_;
  }

  [[nodiscard]] int PixelHeight() const noexcept {
    return pixel_height_;
  }

  [[nodiscard]] std::size_t BytesPerRow() const noexcept {
    return static_cast<std::size_t>(pixel_width_) * 4U;
  }

  [[nodiscard]] std::span<const std::byte> Pixels() const noexcept {
    return pixels_;
  }

private:
  int pixel_width_ = 0;
  int pixel_height_ = 0;
  std::vector<std::byte> pixels_;
};

} // namespace huxerui::detail
