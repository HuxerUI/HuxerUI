#pragma once

namespace huxerui::detail {

// Shared numeric tolerances. File-local constants live next to their only use site.
inline constexpr float transform_epsilon = 0.000001F;   // Inverse-matrix determinant singularity
inline constexpr float extrema_epsilon = 0.000001F;     // Bezier extrema root singularity
inline constexpr float scroll_delta_epsilon = 0.001F;   // Scroll delta consumed/comparison epsilon
inline constexpr float progress_epsilon = 0.001F;       // Progress/opacity zero-test epsilon

// Curve approximation constants.
inline constexpr float cubic_circle_kappa = 0.5522847498F;  // Bezier control for a quarter circle
inline constexpr float square_cap_scale = 0.70710678118F;   // sqrt(0.5), square stroke cap outset
inline constexpr float square_cap_outset = 1.41421356237F;  // sqrt(2), square stroke cap corner
inline constexpr float degrees_to_radians_factor = 3.14159265358979323846F / 180.0F;
inline constexpr float two_pi = 6.28318530717958647692F;

// Root-finding iteration bounds.
inline constexpr int newton_max_iterations = 8;         // Cubic bezier Newton solve (double precision)
inline constexpr int bezier_bisection_iterations = 16;  // Cubic bezier bisection solve (float precision)
inline constexpr int bisection_max_iterations = 12;     // Cubic bezier bisection refine (double precision)
inline constexpr double bezier_convergence_epsilon = 1.0e-7;
inline constexpr double critical_damping_epsilon = 1.0e-5;  // Damping-ratio neighborhood of 1.0

// Gesture and animation timing defaults.
inline constexpr double double_tap_interval = 0.4;          // Text double-tap window
inline constexpr double long_press_delay = 0.5;             // Text long-press delay
inline constexpr double long_press_minimum_interval = 0.3;  // Multi-tap fallback interval
inline constexpr float multi_tap_movement_slop = 18.0F;     // Multi-tap movement tolerance
inline constexpr float velocity_sample_max_age = 0.1F;      // Scroll velocity sampling window
inline constexpr float fling_stop_velocity_fraction = 0.3F; // Stop velocity = min fling * fraction
inline constexpr double fling_frame_clamp = 0.25;           // Single-frame time clamp
inline constexpr double maximum_extrapolation_ratio = 2.0;  // Velocity extrapolation ratio cap

// Visual thresholds.
inline constexpr float luminance_threshold = 0.45F;         // System bar brightness boundary
inline constexpr float tick_max_interval_count = 512.0F;    // Slider tick interval cap
inline constexpr float tick_min_spacing_scale = 1.5F;        // Slider tick minimum spacing

} // namespace huxerui::detail
