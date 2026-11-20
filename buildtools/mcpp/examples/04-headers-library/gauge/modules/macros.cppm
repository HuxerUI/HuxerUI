// Written by hand: the macros of the public headers that an importer needs,
// each mapped to a constant under its own name in gauge::macros. A module
// carries no macro, so these are what `import gauge;` offers in their place;
// the generated interface re-exports this unit. A macro left out here stays
// reachable only by including the header.
module;

#include <gauge/version.h>

// The values, read while the macros are defined.
namespace gauge_macros_capture {
inline constexpr auto value_GAUGE_VERSION_MAJOR = GAUGE_VERSION_MAJOR;
inline constexpr auto value_GAUGE_VERSION_MINOR = GAUGE_VERSION_MINOR;
} // namespace gauge_macros_capture

// A macro the global module fragment defines stays defined to the end of the
// unit, where it would expand inside the constant's own name.
#undef GAUGE_VERSION_MAJOR
#undef GAUGE_VERSION_MINOR

export module gauge.macros;

export namespace gauge::macros {
inline constexpr auto GAUGE_VERSION_MAJOR = gauge_macros_capture::value_GAUGE_VERSION_MAJOR;
inline constexpr auto GAUGE_VERSION_MINOR = gauge_macros_capture::value_GAUGE_VERSION_MINOR;
} // namespace gauge::macros
