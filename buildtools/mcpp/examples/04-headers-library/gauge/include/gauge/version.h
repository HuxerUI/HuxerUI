#pragma once

// The version stays a macro because a unit that includes the headers tests it
// with #if. A module carries no macro, so modules/macros.cppm maps it to
// constants for a unit that imports `gauge`.
#define GAUGE_VERSION_MAJOR 1
#define GAUGE_VERSION_MINOR 0
