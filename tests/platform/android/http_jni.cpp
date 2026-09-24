#include <jni.h>

extern "C" {

JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIHttpRequest_nativeUpload(JNIEnv*, jclass, jlong, jlong) {}

JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIHttpRequest_nativeResponse(
    JNIEnv* env, jclass, jlong handle, jstring, jint, jobjectArray, jobjectArray, jlong
) {
  jclass test = env->FindClass("org/huxerui/HuxerUIHttpRequestTest");
  if (test == nullptr) return;
  jmethodID callback = env->GetStaticMethodID(test, "onResponse", "(J)V");
  if (callback != nullptr) env->CallStaticVoidMethod(test, callback, handle);
  env->DeleteLocalRef(test);
}

JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIHttpRequest_nativeBody(
    JNIEnv* env, jclass, jlong handle, jbyteArray bytes
) {
  jclass test = env->FindClass("org/huxerui/HuxerUIHttpRequestTest");
  if (test == nullptr) return;
  jmethodID callback = env->GetStaticMethodID(test, "onBody", "(J[B)V");
  if (callback != nullptr) env->CallStaticVoidMethod(test, callback, handle, bytes);
  env->DeleteLocalRef(test);
}

JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIHttpRequest_nativeTerminal(
    JNIEnv* env, jclass, jlong handle, jint result, jstring
) {
  jclass test = env->FindClass("org/huxerui/HuxerUIHttpRequestTest");
  if (test == nullptr) return;
  jmethodID callback = env->GetStaticMethodID(test, "onTerminal", "(JI)V");
  if (callback != nullptr) env->CallStaticVoidMethod(test, callback, handle, result);
  env->DeleteLocalRef(test);
}

}
