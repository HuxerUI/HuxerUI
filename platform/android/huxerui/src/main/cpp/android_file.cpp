#include "android_file_internal.h"

#include <jni.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include <huxerui/android/jni.h>

#include "file_internal.h"

namespace huxerui::detail {

namespace {

[[noreturn]] void ThrowJniFailure(JNIEnv* environment, const char* message) {
  if (environment != nullptr && environment->ExceptionCheck()) {
    environment->ExceptionClear();
  }
  throw std::runtime_error(message);
}

std::string AbsolutePath(JNIEnv* environment, jobject file, jmethodID get_absolute_path, const char* description) {
  if (file == nullptr) {
    ThrowJniFailure(environment, description);
  }
  android::LocalRef<jstring> path(
      environment,
      static_cast<jstring>(environment->CallObjectMethod(file, get_absolute_path))
  );
  if (!path || environment->ExceptionCheck()) {
    ThrowJniFailure(environment, description);
  }
  return android::JavaStringToUtf8(environment, path.Get());
}

std::string TemporaryPath(std::string cache_path) {
  if (!cache_path.empty() && cache_path.back() != '/') {
    cache_path.push_back('/');
  }
  cache_path += "huxerui_tmp";
  return cache_path;
}

} // namespace

std::shared_ptr<FileSystem> CreateAndroidFileSystem(JNIEnv* environment, jobject context) {
  if (environment == nullptr || context == nullptr) {
    throw std::runtime_error("HuxerUI Android file system host is unavailable");
  }

  android::LocalRef<jclass> context_class(environment, environment->GetObjectClass(context));
  android::LocalRef<jclass> file_class(environment, environment->FindClass("java/io/File"));
  android::LocalRef<jclass> application_info_class(
      environment,
      environment->FindClass("android/content/pm/ApplicationInfo")
  );
  if (!context_class || !file_class || !application_info_class || environment->ExceptionCheck()) {
    ThrowJniFailure(environment, "HuxerUI Android file system classes are unavailable");
  }

  const jmethodID get_files_directory =
      environment->GetMethodID(context_class.Get(), "getFilesDir", "()Ljava/io/File;");
  const jmethodID get_cache_directory =
      environment->GetMethodID(context_class.Get(), "getCacheDir", "()Ljava/io/File;");
  const jmethodID get_application_info =
      environment->GetMethodID(context_class.Get(), "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
  const jmethodID get_absolute_path =
      environment->GetMethodID(file_class.Get(), "getAbsolutePath", "()Ljava/lang/String;");
  const jfieldID native_library_directory =
      environment->GetFieldID(application_info_class.Get(), "nativeLibraryDir", "Ljava/lang/String;");
  if (get_files_directory == nullptr || get_cache_directory == nullptr || get_application_info == nullptr ||
      get_absolute_path == nullptr || native_library_directory == nullptr || environment->ExceptionCheck()) {
    ThrowJniFailure(environment, "HuxerUI Android file system methods do not match the platform backend");
  }

  android::LocalRef<jobject> data(environment, environment->CallObjectMethod(context, get_files_directory));
  android::LocalRef<jobject> cache(environment, environment->CallObjectMethod(context, get_cache_directory));
  android::LocalRef<jobject> application_info(
      environment,
      environment->CallObjectMethod(context, get_application_info)
  );
  if (!data || !cache || !application_info || environment->ExceptionCheck()) {
    ThrowJniFailure(environment, "HuxerUI Android application directories are unavailable");
  }

  android::LocalRef<jstring> executable(
      environment,
      static_cast<jstring>(environment->GetObjectField(application_info.Get(), native_library_directory))
  );
  if (environment->ExceptionCheck()) {
    ThrowJniFailure(environment, "HuxerUI Android native library directory is unavailable");
  }

  const std::string cache_path =
      AbsolutePath(environment, cache.Get(), get_absolute_path, "HuxerUI Android cache directory is unavailable");
  return MakeFileSystem({
      .executable_directory = executable
                                  ? std::optional<std::string>{android::JavaStringToUtf8(environment, executable.Get())}
                                  : std::nullopt,
      .data_directory =
          AbsolutePath(environment, data.Get(), get_absolute_path, "HuxerUI Android data directory is unavailable"),
      .cache_directory = cache_path,
      .temporary_directory = TemporaryPath(cache_path),
  });
}

} // namespace huxerui::detail
