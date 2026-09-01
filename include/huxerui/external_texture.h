#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <huxerui/geometry.h>

namespace huxerui {

namespace detail {
class ExternalTextureFrameRequester;
} // namespace detail

/// Identifies one live platform texture shared by application code, RenderScene, and the matching platform renderer.
///
/// ExternalTexture owns the platform-neutral identity, immutable logical size, frame revision, and committed-visibility
/// state. A platform-specific subclass owns the actual frame mailbox. Share an instance through std::shared_ptr; the
/// object itself is intentionally non-copyable.
///
/// Publishing a frame does not recompose the View or rerecord its PaintSequence. Active Runtimes observe Revision(),
/// damage the visible destination, and let their renderer acquire the newest platform frame.
/// Subclassing alone does not add rendering support: the active platform renderer must explicitly recognize the
/// concrete texture type and its mailbox contract.
///
/// @code
/// std::shared_ptr<ExternalTexture> preview = camera->PreviewTexture();
/// return Image(preview)
///     .Fit(ImageFit::Cover);
/// @endcode
class ExternalTexture {
public:
  /// Releases the shared texture identity after its final std::shared_ptr owner is released.
  virtual ~ExternalTexture() = 0;

  /// ExternalTexture identities cannot be copied; copy their std::shared_ptr instead.
  ExternalTexture(const ExternalTexture&) = delete;
  ExternalTexture& operator=(const ExternalTexture&) = delete;

  /// Returns the immutable logical size used by Image measurement and source rectangles, expressed in DIPs.
  ///
  /// Pixel dimensions may differ from this value. A producer whose logical size changes creates a new texture.
  [[nodiscard]] Size IntrinsicSize() const noexcept {
    return intrinsic_size_;
  }
  /// Returns the monotonically increasing publication revision.
  ///
  /// The value advances after each frame has been committed to the concrete texture's mailbox. It does not identify a
  /// platform resource and should only be used to detect a newer publication from the same texture identity.
  [[nodiscard]] std::uint64_t Revision() const noexcept {
    return revision_.load(std::memory_order_acquire);
  }
  /// Returns whether at least one live Runtime currently displays this texture in its committed RenderScene.
  ///
  /// This is a production hint, not an ownership or lifecycle signal. It may change after any frame commit, and the
  /// producer remains responsible for starting, pausing, and finishing its own work.
  [[nodiscard]] bool IsActive() const noexcept;

protected:
  /// Creates a texture with a finite, strictly positive immutable logical size.
  ///
  /// Platform subclasses call this once from their constructor. Invalid dimensions throw std::invalid_argument.
  explicit ExternalTexture(Size intrinsic_size);
  /// Commits one already-published mailbox frame to the shared revision and schedules every active Runtime.
  ///
  /// A concrete Publish operation first stores a complete stable frame, then calls this function exactly once. The
  /// function may be called from the producer context supported by that concrete texture.
  void NotifyFrameAvailable();

private:
  void SetActive(const std::shared_ptr<detail::ExternalTextureFrameRequester>& requester, bool active);

  Size intrinsic_size_;
  std::atomic<std::uint64_t> revision_ = 0;
  mutable std::mutex mutex_;
  std::vector<std::weak_ptr<detail::ExternalTextureFrameRequester>> active_requesters_;

  friend class detail::ExternalTextureFrameRequester;
};

} // namespace huxerui
