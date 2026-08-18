#include "platform_text_field.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

#include <huxerui/android/jni.h>
#include <huxerui/android/platform_view.h>

namespace {

constexpr const char* platform_text_field_class = "org/huxerui/examples/platformview/PlatformTextField";

void ApplyProperties(JNIEnv* environment, jobject view, const huxerui::PlatformPayload& properties) {
  const std::string_view text =
      properties.AsObject().at(huxerui::example::platform_text_field::text_property).AsString();
  huxerui::android::LocalRef<jclass> view_class(environment, environment->GetObjectClass(view));
  jmethodID apply_text = environment->GetMethodID(view_class.Get(), "applyControlledText", "(Ljava/lang/String;)V");
  huxerui::android::LocalRef<jstring> java_text = huxerui::android::Utf8ToJavaString(environment, text);
  if (apply_text != nullptr && java_text) {
    environment->CallVoidMethod(view, apply_text, java_text.Get());
  }
}

} // namespace

namespace huxerui::example {

namespace {

jobject CreatePlatformTextField(
    JNIEnv* environment, jobject context, const PlatformPayload& properties, PlatformEventSink event_sink
) {
  android::LocalRef<jclass> view_class(environment, environment->FindClass(platform_text_field_class));
  if (!view_class) {
    return nullptr;
  }
  jmethodID constructor = environment->GetMethodID(view_class.Get(), "<init>", "(Landroid/content/Context;)V");
  jmethodID install_bridge = environment->GetMethodID(view_class.Get(), "installPlatformBridge", "(J)V");
  if (constructor == nullptr || install_bridge == nullptr) {
    return nullptr;
  }
  jobject view = environment->NewObject(view_class.Get(), constructor, context);
  if (view == nullptr || environment->ExceptionCheck()) {
    return view;
  }
  ApplyProperties(environment, view, properties);
  if (environment->ExceptionCheck()) {
    return view;
  }
  auto sink = std::make_unique<PlatformEventSink>(std::move(event_sink));
  environment->CallVoidMethod(view, install_bridge, static_cast<jlong>(reinterpret_cast<std::uintptr_t>(sink.get())));
  if (!environment->ExceptionCheck()) {
    sink.release();
  }
  return view;
}

void UpdatePlatformTextField(JNIEnv* environment, jobject view, const PlatformPayload& properties) {
  ApplyProperties(environment, view, properties);
}

void DisposePlatformTextField(JNIEnv* environment, jobject view) {
  android::LocalRef<jclass> view_class(environment, environment->GetObjectClass(view));
  jmethodID dispose = environment->GetMethodID(view_class.Get(), "disposePlatformBridge", "()J");
  jlong sink = dispose == nullptr ? 0 : environment->CallLongMethod(view, dispose);
  delete reinterpret_cast<PlatformEventSink*>(static_cast<std::uintptr_t>(sink));
}

android::PlatformViewFactory PlatformTextFieldFactory() {
  return {
      .create = CreatePlatformTextField,
      .update = UpdatePlatformTextField,
      .dispose = DisposePlatformTextField,
  };
}

} // namespace

void InstallPlatformTextField(RootContext& root) {
  root.Modules().Register(platform_text_field::type, PlatformTextFieldFactory());
}

} // namespace huxerui::example

extern "C" JNIEXPORT void JNICALL
Java_org_huxerui_examples_platformview_PlatformTextField_nativeChanged(
    JNIEnv* environment, jclass, jlong sink, jstring value
) {
  auto* event_sink = reinterpret_cast<huxerui::PlatformEventSink*>(static_cast<std::uintptr_t>(sink));
  if (event_sink != nullptr) {
    (*event_sink)(
        huxerui::example::PlatformTextFieldEvents::Changed::Name,
        huxerui::PlatformPayload(huxerui::android::JavaStringToUtf8(environment, value))
    );
  }
}
