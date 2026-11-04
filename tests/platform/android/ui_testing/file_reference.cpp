#include <huxerui/android/jni.h>
#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>

#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
void Require(bool condition, const char* message) {
  if (!condition) { throw std::runtime_error(message); }
}
} // namespace

extern "C" JNIEXPORT jstring JNICALL Java_org_huxerui_HuxerUIFileReferenceTest_verifyNative(
    JNIEnv* environment, jclass, jobject payload, jobject after_capture, jstring destination,
    jboolean unknown, jboolean revoked) {
  using namespace huxerui;
  try {
    TaskScope tasks;
    Application application([] { return View{}; }, {
        .application_hooks = {[&](ApplicationContext&) { tasks = UseApplicationTaskScope(); }},
    });
    testing::UiTest ui(application);
    Task<IoResult<AsyncInputStream>> open;
    Task<bool> import;
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
    }
    android::LocalRef<jclass> runnable(environment, environment->GetObjectClass(after_capture));
    const jmethodID run = environment->GetMethodID(runnable.Get(), "run", "()V");
    environment->CallVoidMethod(after_capture, run);
    if (environment->ExceptionCheck()) { return nullptr; }
    bool finished = false;
    std::exception_ptr failure;
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
          Require(first.Succeeded() && first.Value().size() == 3 && rest.Succeeded() && rest.Value().size() == 4 &&
                      eof.Succeeded() && eof.Value().empty(), "The native reference must support incremental reads");
          Require(co_await std::move(import), "A pending import must survive wrapper closure");
          auto contents = target.ReadString();
          Require(contents.Succeeded() && contents.Value() == "content", "Import must preserve file contents");
        }
      } catch (...) { failure = std::current_exception(); }
      finished = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!finished && std::chrono::steady_clock::now() < deadline) {
      ui.Pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(finished, "The file reference test timed out");
    if (failure) { std::rethrow_exception(failure); }
    return environment->NewStringUTF("");
  } catch (const std::exception& error) {
    return huxerui::android::Utf8ToJavaString(environment, error.what()).Release();
  }
}
