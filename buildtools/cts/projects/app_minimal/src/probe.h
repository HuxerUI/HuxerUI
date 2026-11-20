#pragma once

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#if defined(__ANDROID__)
#include <android/log.h>
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace cts {

/// Appends observed runtime facts to the file named by HUXERUI_CTS_PROBE so the conformance runner can read what a
/// started application resolved; recording is inert when the variable is absent. A Web page, an Android application,
/// and an iOS application have no environment or file system the runner can name, so there each fact is a line the
/// runner reads: from the browser console, the `HUXERUI_CTS_PROBE` log tag, or the application's standard output.
class Probe {
public:
  void Record(std::string_view key, const std::string& value) const {
#if defined(__ANDROID__)
    const std::string line = std::string(key) + '=' + value;
    __android_log_write(ANDROID_LOG_INFO, "HUXERUI_CTS_PROBE", line.c_str());
#elif defined(__EMSCRIPTEN__) || (defined(TARGET_OS_IOS) && TARGET_OS_IOS)
    std::cout << "HUXERUI_CTS_PROBE " << key << '=' << value << std::endl;
#else
    const char* path = std::getenv("HUXERUI_CTS_PROBE");
    if (path == nullptr || *path == '\0') return;
    std::ofstream output(path, std::ios::app);
    output << key << '=' << value << '\n';
#endif
  }
};

} // namespace cts
