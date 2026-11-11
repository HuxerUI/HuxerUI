#pragma once

#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace huxerui::detail {

class FrameCoordinator {
public:
  [[nodiscard]] std::optional<double> Request(double deadline, double now, bool platform_ready) noexcept {
    frame_build_pending_ = true;
    deadline = NormalizeDeadline(deadline, now);
    if (commit_in_progress_ || paint_pending_ || paint_in_progress_ || !platform_ready) {
      if (!deferred_frame_deadline_.has_value() || deadline < *deferred_frame_deadline_) {
        deferred_frame_deadline_ = deadline;
      }
      return std::nullopt;
    }
    return deadline;
  }

  // Keep the borrowed scene protected until the host has installed the frame.
  template <class Publish>
  void Commit(Publish&& publish) {
    if (commit_in_progress_ || !frame_build_pending_) {
      return;
    }
    commit_in_progress_ = true;
    paint_interrupted_ = false;
    frame_build_pending_ = false;
    deferred_frame_deadline_.reset();
    try {
      std::forward<Publish>(publish)();
    } catch (...) {
      commit_in_progress_ = false;
      throw;
    }
    commit_in_progress_ = false;
  }

  // A commit can invalidate borrowed scene nodes before the platform has replaced every drawing route.
  [[nodiscard]] bool DeferPaintIfCommitting() noexcept {
    paint_interrupted_ = paint_interrupted_ || commit_in_progress_;
    return commit_in_progress_;
  }

  [[nodiscard]] bool PaintInterrupted() const noexcept {
    return paint_interrupted_;
  }

  void MarkPaintPending() noexcept {
    paint_pending_ = true;
  }

  void BeginPaint() noexcept {
    paint_in_progress_ = true;
  }

  [[nodiscard]] std::optional<double> EndPaint(bool platform_ready) noexcept {
    paint_in_progress_ = false;
    paint_pending_ = false;
    return TakeDeferred(platform_ready);
  }

  [[nodiscard]] std::optional<double> TakeDeferred(bool platform_ready) noexcept {
    if (commit_in_progress_ || paint_pending_ || paint_in_progress_ || !frame_build_pending_ ||
        !deferred_frame_deadline_.has_value() || !platform_ready) {
      return std::nullopt;
    }
    const double deadline = *deferred_frame_deadline_;
    deferred_frame_deadline_.reset();
    return deadline;
  }

  [[nodiscard]] bool FrameBuildPending() const noexcept {
    return frame_build_pending_;
  }

  [[nodiscard]] bool PaintPending() const noexcept {
    return paint_pending_;
  }

private:
  [[nodiscard]] static double NormalizeDeadline(double deadline, double now) noexcept {
    if (std::isnan(deadline) || deadline <= now) {
      return now;
    }
    return std::isfinite(deadline) ? deadline : std::numeric_limits<double>::max();
  }

  bool frame_build_pending_ = false;
  bool commit_in_progress_ = false;
  bool paint_pending_ = false;
  bool paint_in_progress_ = false;
  bool paint_interrupted_ = false;
  std::optional<double> deferred_frame_deadline_;
};

} // namespace huxerui::detail
