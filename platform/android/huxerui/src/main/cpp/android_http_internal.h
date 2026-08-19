#pragma once

#include <jni.h>

#include <memory>

namespace huxerui::detail {

class HttpTransport;

std::shared_ptr<HttpTransport> CreateAndroidHttpTransport(JavaVM* virtual_machine, JNIEnv* environment);

} // namespace huxerui::detail
