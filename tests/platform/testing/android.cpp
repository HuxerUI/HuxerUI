#include "smoke.h"

#include <memory>

#include <jni.h>

extern "C" JNIEXPORT jstring JNICALL Java_org_huxerui_HuxerUIUiTestingTest_runNative(
    JNIEnv* environment, jclass, jstring package_root) {
  if (!package_root) return environment->NewStringUTF("HuxerUI testing requires packagePath");
  const char* path = environment->GetStringUTFChars(package_root, nullptr);
  if (!path) return nullptr;
  auto release = [environment, package_root](const char* value) {
    environment->ReleaseStringUTFChars(package_root, value);
  };
  std::unique_ptr<const char, decltype(release)> retained(path, release);
  try {
    auto error = RunUiTestingSmoke(path);
    // Keep failure transport ASCII-safe rather than passing arbitrary UTF-8 as JNI modified UTF-8.
    for (char& byte : error) if (static_cast<unsigned char>(byte) >= 128) byte = '?';
    return environment->NewStringUTF(error.c_str());
  } catch (...) {
    return environment->NewStringUTF("HuxerUI testing carrier failed");
  }
}
