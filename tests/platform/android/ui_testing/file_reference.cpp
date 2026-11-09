#include <huxerui/android/jni.h>
#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>

#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#include "android_file_internal.h"
#include "io/file_internal.h"

namespace {
void Require(bool condition, const char* message) {
  if (!condition) { throw std::runtime_error(message); }
}

void PumpUntilFileTaskFinishes(huxerui::testing::UiTest& ui, const bool& finished) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!finished && std::chrono::steady_clock::now() < deadline) {
    ui.Pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Require(finished, "The file reference test timed out");
}

void VerifySynchronousReference(const huxerui::FileReference& reference, bool revoked, bool sequential,
                                bool read_error) {
  using namespace huxerui;
  auto input = reference.OpenRead();
  auto random = reference.OpenRandomAccess();
  if (revoked) {
    auto content = reference.ReadString();
    Require(!input.Succeeded() && input.Error().code == IoErrorCode::PermissionDenied &&
                !content.Succeeded() && content.Error().code == IoErrorCode::PermissionDenied &&
                !random.Succeeded() && random.Error().code == IoErrorCode::PermissionDenied,
            "Synchronous operations must observe revoked provider access");
    return;
  }
  if (!read_error) {
    auto content = reference.ReadString();
    Require(content.Succeeded() && content.Value() == "content", "Whole-file reads must use the retained provider");
  }
  Require(input.Succeeded(), "Synchronous input must use the retained provider");
  Bytes buffer(7);
  const Bytes expected{std::byte{'c'}, std::byte{'o'}, std::byte{'n'}, std::byte{'t'},
                       std::byte{'e'}, std::byte{'n'}, std::byte{'t'}};
  Require(input.Value().Read(buffer).Value() == buffer.size() && buffer == expected,
          "Synchronous input must read a provider stream incrementally");
  auto eof = input.Value().Read(buffer);
  Require(read_error ? !eof.Succeeded() && eof.Error().code == IoErrorCode::Io
                     : eof.Succeeded() && eof.Value() == 0,
          "Synchronous input must distinguish a provider failure from EOF");
  if (sequential) {
    Require(!random.Succeeded() && random.Error().code == IoErrorCode::Unsupported,
            "Synchronous random access must reject a sequential document");
  } else {
    buffer.resize(3);
    Require(random.Succeeded() && random.Value().Size().Value() == 7 &&
                random.Value().ReadAt(4, buffer).Value() == 3 &&
                buffer == Bytes{std::byte{'e'}, std::byte{'n'}, std::byte{'t'}} && random.Value().Close().Succeeded(),
            "Synchronous random access must read at the requested offset and close a seekable document");
  }
}
} // namespace

extern "C" JNIEXPORT jstring JNICALL Java_org_huxerui_HuxerUIFileReferenceTest_verifyNative(
    JNIEnv* environment, jclass, jobject payload, jobject after_capture, jstring destination,
    jboolean unknown, jboolean revoked, jboolean sequential, jboolean read_error) {
  using namespace huxerui;
  try {
    TaskScope tasks;
    Application application([] { return View{}; }, {
        .application_hooks = {[&](ApplicationContext&) { tasks = UseApplicationTaskScope(); }},
    });
    testing::UiTest ui(application);
    Task<IoResult<AsyncInputStream>> open;
    Task<bool> import;
    std::optional<FileReference> sync_reference;
    const File target(android::JavaStringToUtf8(environment, destination));
    {
      auto reference = android::JavaPlatformPayloadToCpp(environment, payload).AsFileReference();
      Require(!reference.CanWrite(), "The factory must return a read-only capability");
      Require(!reference.AsFile(), "A content URI must not become a local path");
      Require(reference.Type() == FileType::File, "The factory must return a file");
      if (unknown) {
        Require(reference.Name().empty() && !reference.Size() && !reference.ContentType(),
                "Missing metadata must remain unknown");
      } else {
        Require(reference.Name() == "测试.txt" && reference.Size() == 7 && reference.ContentType() == "text/plain",
                "The factory must preserve provider metadata");
      }
      open = reference.OpenReadAsync();
      import = reference.ImportToAsync(target, true);
      sync_reference = reference;
    }
    android::LocalRef<jclass> runnable(environment, environment->GetObjectClass(after_capture));
    const jmethodID run = environment->GetMethodID(runnable.Get(), "run", "()V");
    environment->CallVoidMethod(after_capture, run);
    if (environment->ExceptionCheck()) { return nullptr; }
    std::exception_ptr failure;
    std::thread worker([&] {
      try {
        VerifySynchronousReference(*sync_reference, revoked, sequential, read_error);
      } catch (...) { failure = std::current_exception(); }
    });
    worker.join();
    sync_reference.reset();
    if (failure) { std::rethrow_exception(failure); }
    bool finished = false;
    tasks.Launch([&]() -> Task<void> {
      try {
        auto opened = co_await std::move(open);
        if (revoked) {
          Require(!opened.Succeeded() && opened.Error().code == IoErrorCode::PermissionDenied,
                  "Revoked access must report PermissionDenied");
        } else {
          Require(opened.Succeeded(), "A pending open must survive wrapper closure");
          auto first = co_await opened.Value().ReadAsync(3);
          auto rest = co_await opened.Value().ReadAsync(64);
          auto eof = co_await opened.Value().ReadAsync(64);
          Require(first.Succeeded() && first.Value().size() == 3 && rest.Succeeded() && rest.Value().size() == 4,
                  "The native reference must support incremental reads");
          Require(read_error ? !eof.Succeeded() && eof.Error().code == IoErrorCode::Io
                             : eof.Succeeded() && eof.Value().empty(),
                  "Asynchronous input must distinguish a provider failure from EOF");
          if (!read_error) {
            Require(co_await std::move(import), "A pending import must survive wrapper closure");
            auto contents = target.ReadString();
            Require(contents.Succeeded() && contents.Value() == "content", "Import must preserve file contents");
          }
        }
      } catch (...) { failure = std::current_exception(); }
      finished = true;
    });
    PumpUntilFileTaskFinishes(ui, finished);
    if (failure) { std::rethrow_exception(failure); }
    return environment->NewStringUTF("");
  } catch (const std::exception& error) {
    return huxerui::android::Utf8ToJavaString(environment, error.what()).Release();
  }
}

extern "C" JNIEXPORT jstring JNICALL Java_org_huxerui_HuxerUIFileReferenceTest_verifyReliableOutput(
    JNIEnv* environment, jclass, jobject context, jstring uri, jboolean asynchronous, jboolean finalize,
    jboolean write_error) {
  using namespace huxerui;
  try {
    JavaVM* vm = nullptr;
    Require(environment->GetJavaVM(&vm) == JNI_OK, "Android VM must be available");
    auto reference = detail::CreateAndroidFileReference(
        vm, environment, context, {.name = "output.txt", .can_write = true},
        android::JavaStringToUtf8(environment, uri));
    const Bytes bytes{std::byte{'n'}, std::byte{'e'}, std::byte{'w'}};
    if (!asynchronous) {
      auto opened = reference.OpenWrite();
      Require(opened.Succeeded(), "Reliable synchronous output must open");
      auto written = opened.Value().Write(bytes);
      Require(written.Succeeded() != write_error, "Reliable synchronous output must report the write outcome");
      if (finalize) {
        Require(opened.Value().Close().Succeeded(), "Reliable synchronous output must close normally");
      }
      return environment->NewStringUTF("");
    }

    TaskScope tasks;
    Application application([] { return View{}; }, {
        .application_hooks = {[&](ApplicationContext&) { tasks = UseApplicationTaskScope(); }},
    });
    testing::UiTest ui(application);
    std::exception_ptr failure;
    bool finished = false;
    tasks.Launch([&]() -> Task<void> {
      try {
        auto opened = co_await reference.OpenWriteAsync();
        Require(opened.Succeeded(), "Reliable asynchronous output must open");
        auto written = co_await opened.Value().WriteAsync(bytes);
        Require(written.Succeeded() != write_error, "Reliable asynchronous output must report the write outcome");
        if (finalize) {
          Require((co_await opened.Value().CloseAsync()).Succeeded(),
                  "Reliable asynchronous output must close normally");
        }
      } catch (...) { failure = std::current_exception(); }
      finished = true;
    });
    PumpUntilFileTaskFinishes(ui, finished);
    if (failure) { std::rethrow_exception(failure); }
    return environment->NewStringUTF("");
  } catch (const std::exception& error) {
    return android::Utf8ToJavaString(environment, error.what()).Release();
  }
}
