#include <huxerui/android/jni.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/android/platform_registry.h>

#include "application/platform_registry_internal.h"
#include "android_file_internal.h"

namespace huxerui::android {

namespace {

std::u16string Utf8ToUtf16(std::string_view value) {
  std::u16string result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size();) {
    const auto first = static_cast<std::uint8_t>(value[index]);
    std::size_t length = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    if (first <= 0x7FU) {
      length = 1;
      code_point = first;
    } else if ((first & 0xE0U) == 0xC0U) {
      length = 2;
      code_point = first & 0x1FU;
      minimum = 0x80U;
    } else if ((first & 0xF0U) == 0xE0U) {
      length = 3;
      code_point = first & 0x0FU;
      minimum = 0x800U;
    } else if ((first & 0xF8U) == 0xF0U) {
      length = 4;
      code_point = first & 0x07U;
      minimum = 0x10000U;
    } else {
      throw std::invalid_argument("HuxerUI Android JNI string must contain valid UTF-8");
    }
    if (index + length > value.size()) {
      throw std::invalid_argument("HuxerUI Android JNI string must contain valid UTF-8");
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto continuation = static_cast<std::uint8_t>(value[index + offset]);
      if ((continuation & 0xC0U) != 0x80U) {
        throw std::invalid_argument("HuxerUI Android JNI string must contain valid UTF-8");
      }
      code_point = (code_point << 6U) | (continuation & 0x3FU);
    }
    if (code_point < minimum || code_point > 0x10FFFFU || (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
      throw std::invalid_argument("HuxerUI Android JNI string must contain valid UTF-8");
    }
    if (code_point <= 0xFFFFU) {
      result.push_back(static_cast<char16_t>(code_point));
    } else {
      const std::uint32_t surrogate = code_point - 0x10000U;
      result.push_back(static_cast<char16_t>(0xD800U + (surrogate >> 10U)));
      result.push_back(static_cast<char16_t>(0xDC00U + (surrogate & 0x3FFU)));
    }
    index += length;
  }
  return result;
}

void AppendUtf8(std::string& result, std::uint32_t code_point) {
  if (code_point <= 0x7FU) {
    result.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FFU) {
    result.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
    result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else if (code_point <= 0xFFFFU) {
    result.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
    result.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
    result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else {
    result.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
    result.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
    result.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
    result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  }
}

std::string Utf16ToUtf8(std::u16string_view value) {
  std::string result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    std::uint32_t code_point = value[index];
    if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
      if (index + 1 >= value.size()) {
        throw std::invalid_argument("HuxerUI Android Java string contains an unpaired UTF-16 surrogate");
      }
      const std::uint32_t low = value[++index];
      if (low < 0xDC00U || low > 0xDFFFU) {
        throw std::invalid_argument("HuxerUI Android Java string contains an unpaired UTF-16 surrogate");
      }
      code_point = 0x10000U + ((code_point - 0xD800U) << 10U) + (low - 0xDC00U);
    } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
      throw std::invalid_argument("HuxerUI Android Java string contains an unpaired UTF-16 surrogate");
    }
    AppendUtf8(result, code_point);
  }
  return result;
}

void RequireEnvironment(JNIEnv* environment) {
  if (environment == nullptr) {
    throw std::invalid_argument("HuxerUI Android JNI environment must not be null");
  }
}

} // namespace

namespace detail {

void DeleteLocalReference(JNIEnv* environment, jobject reference) noexcept {
  if (environment != nullptr && reference != nullptr) {
    environment->DeleteLocalRef(reference);
  }
}

} // namespace detail

LocalRef<jstring> Utf8ToJavaString(JNIEnv* environment, std::string_view value) {
  RequireEnvironment(environment);
  const std::u16string utf16 = Utf8ToUtf16(value);
  if (utf16.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max())) {
    throw std::overflow_error("HuxerUI Android Java string exceeds the JNI length range");
  }
  return LocalRef<jstring>(
      environment,
      environment->NewString(reinterpret_cast<const jchar*>(utf16.data()), static_cast<jsize>(utf16.size()))
  );
}

std::string JavaStringToUtf8(JNIEnv* environment, jstring value) {
  RequireEnvironment(environment);
  if (value == nullptr) {
    throw std::invalid_argument("HuxerUI Android Java string must not be null");
  }
  const jsize length = environment->GetStringLength(value);
  std::u16string utf16(static_cast<std::size_t>(length), u'\0');
  if (length > 0) {
    environment->GetStringRegion(value, 0, length, reinterpret_cast<jchar*>(utf16.data()));
  }
  return Utf16ToUtf8(utf16);
}

LocalRef<jbyteArray> BytesToJavaByteArray(JNIEnv* environment, std::span<const std::byte> value) {
  RequireEnvironment(environment);
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max())) {
    throw std::overflow_error("HuxerUI Android byte array exceeds the JNI length range");
  }
  jbyteArray result = environment->NewByteArray(static_cast<jsize>(value.size()));
  if (result != nullptr && !value.empty()) {
    environment
        ->SetByteArrayRegion(result, 0, static_cast<jsize>(value.size()), reinterpret_cast<const jbyte*>(value.data()));
  }
  return LocalRef<jbyteArray>(environment, result);
}

Bytes JavaByteArrayToBytes(JNIEnv* environment, jbyteArray value) {
  RequireEnvironment(environment);
  if (value == nullptr) {
    throw std::invalid_argument("HuxerUI Android Java byte array must not be null");
  }
  const jsize length = environment->GetArrayLength(value);
  Bytes result(static_cast<std::size_t>(length));
  if (length > 0) {
    environment->GetByteArrayRegion(value, 0, length, reinterpret_cast<jbyte*>(result.data()));
  }
  return result;
}

LocalRef<jobject> PlatformPayloadToJava(JNIEnv* environment, const PlatformPayload& payload) {
  RequireEnvironment(environment);
  PlatformPayload::Envelope envelope = payload.Encode();
  LocalRef<jbyteArray> bytes = BytesToJavaByteArray(environment, envelope.bytes);
  LocalRef<jclass> payload_class(environment, environment->FindClass("org/huxerui/PlatformPayload"));
  LocalRef<jclass> texture_class(environment, environment->FindClass("org/huxerui/HuxerUIExternalTexture"));
  LocalRef<jclass> reference_class(environment, environment->FindClass("org/huxerui/HuxerUIFileReference"));
  LocalRef<jclass> buffer_class(environment, environment->FindClass("org/huxerui/HuxerUIBufferReference"));
  LocalRef<jclass> list_class(environment, environment->FindClass("java/util/ArrayList"));
  if (!bytes || !payload_class || !texture_class || !reference_class || !buffer_class || !list_class ||
      environment->ExceptionCheck()) {
    throw std::runtime_error("HuxerUI could not resolve the Android PlatformPayload bridge");
  }
  const jmethodID buffer_constructor = environment->GetMethodID(buffer_class.Get(), "<init>", "(J)V");
  const jmethodID buffer_close = environment->GetMethodID(buffer_class.Get(), "close", "()V");
  const jmethodID texture_constructor = environment->GetMethodID(texture_class.Get(), "<init>", "(J)V");
  const jmethodID reference_constructor =
      environment->GetMethodID(reference_class.Get(), "<init>", "(JJLjava/lang/String;)V");
  const jmethodID list_constructor = environment->GetMethodID(list_class.Get(), "<init>", "(I)V");
  const jmethodID list_add = environment->GetMethodID(list_class.Get(), "add", "(Ljava/lang/Object;)Z");
  const jmethodID list_get = environment->GetMethodID(list_class.Get(), "get", "(I)Ljava/lang/Object;");
  const jmethodID decode = environment->GetStaticMethodID(
      payload_class.Get(), "decodeEnvelope",
      "([BLjava/util/List;Ljava/util/List;Ljava/util/List;)Lorg/huxerui/PlatformPayload;");
  if (texture_constructor == nullptr || reference_constructor == nullptr || buffer_constructor == nullptr ||
      buffer_close == nullptr || list_constructor == nullptr || list_add == nullptr || list_get == nullptr ||
      decode == nullptr || environment->ExceptionCheck()) {
    throw std::runtime_error("HuxerUI Android PlatformPayload bridge methods do not match the SDK");
  }
  LocalRef<jobject> textures(environment, environment->NewObject(list_class.Get(), list_constructor,
                                                                 static_cast<jint>(envelope.external_textures.size())));
  LocalRef<jobject> references(environment, environment->NewObject(list_class.Get(), list_constructor,
                                                                   static_cast<jint>(envelope.file_references.size())));
  LocalRef<jobject> buffers(environment, environment->NewObject(list_class.Get(), list_constructor,
                                                              static_cast<jint>(envelope.buffer_references.size())));
  if (!textures || !references || !buffers || environment->ExceptionCheck()) {
    throw std::runtime_error("HuxerUI could not allocate the Android PlatformPayload capability table");
  }
  // Decoded payloads retain independent shares; temporary wrappers must not hold storage until Java finalization.
  struct TemporaryBufferTable {
    JNIEnv* environment;
    jobject list;
    jmethodID get;
    jmethodID close;
    jint count = 0;
    LocalRef<jobject> current;

    void Close(jobject reference) noexcept {
      if (reference != nullptr) {
        environment->CallVoidMethod(reference, close);
      }
      if (environment->ExceptionCheck()) {
        environment->ExceptionDescribe();
        environment->ExceptionClear();
      }
    }

    ~TemporaryBufferTable() {
      LocalRef<jthrowable> pending(environment, environment->ExceptionOccurred());
      if (pending) {
        environment->ExceptionClear();
      }
      Close(current.Get());
      for (jint index = 0; index < count; ++index) {
        LocalRef<jobject> reference(environment, environment->CallObjectMethod(list, get, index));
        Close(reference.Get());
      }
      if (pending) {
        environment->Throw(pending.Get());
      }
    }
  } buffer_table{environment, buffers.Get(), list_get, buffer_close, 0, {}};
  for (const std::shared_ptr<ExternalTexture>& texture : envelope.external_textures) {
    auto handle = std::make_unique<std::shared_ptr<ExternalTexture>>(texture);
    LocalRef<jobject> java_texture(
        environment, environment->NewObject(texture_class.Get(), texture_constructor,
                                            static_cast<jlong>(reinterpret_cast<std::uintptr_t>(handle.get()))));
    if (!java_texture || environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI could not create an Android external texture capability");
    }
    handle.release();
    environment->CallBooleanMethod(textures.Get(), list_add, java_texture.Get());
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI could not populate the Android PlatformPayload capability table");
    }
  }
  for (const FileReference& reference : envelope.file_references) {
    const huxerui::detail::AndroidFileReferenceProjection projection =
        huxerui::detail::ProjectAndroidFileReference(reference);
    auto handle = std::make_unique<FileReference>(reference);
    LocalRef<jstring> uri = Utf8ToJavaString(environment, projection.uri);
    const jlong native_handle = static_cast<jlong>(reinterpret_cast<std::uintptr_t>(handle.get()));
    LocalRef<jobject> java_reference(environment, environment->NewObject(reference_class.Get(), reference_constructor,
                                                                         native_handle,
                                                                         static_cast<jlong>(projection.capability_key),
                                                                         uri.Get()));
    if (!java_reference || environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI could not create an Android file reference capability");
    }
    handle.release();
    environment->CallBooleanMethod(references.Get(), list_add, java_reference.Get());
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI could not populate the Android PlatformPayload capability table");
    }
  }
  for (const BufferReference& reference : envelope.buffer_references) {
    if (reference.AsBytes().size() > static_cast<std::size_t>(std::numeric_limits<jint>::max())) {
      throw std::invalid_argument("HuxerUI Android buffer reference exceeds the ByteBuffer capacity range");
    }
    auto handle = std::make_unique<BufferReference>(reference);
    LocalRef<jobject> wrapper(environment, environment->NewObject(
        buffer_class.Get(), buffer_constructor, static_cast<jlong>(reinterpret_cast<std::uintptr_t>(handle.get()))));
    if (!wrapper || environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI could not create an Android buffer reference capability");
    }
    handle.release();
    buffer_table.current = std::move(wrapper);
    environment->CallBooleanMethod(buffers.Get(), list_add, buffer_table.current.Get());
    if (environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI could not populate the Android buffer reference table");
    }
    ++buffer_table.count;
    buffer_table.current.Reset();
  }
  jobject result = environment->CallStaticObjectMethod(
      payload_class.Get(), decode, bytes.Get(), textures.Get(), references.Get(), buffers.Get());
  if (result == nullptr || environment->ExceptionCheck()) {
    throw std::runtime_error("HuxerUI could not decode PlatformPayload for Android");
  }
  return LocalRef<jobject>(environment, result);
}

PlatformPayload JavaPlatformPayloadToCpp(JNIEnv* environment, jobject payload) {
  RequireEnvironment(environment);
  if (payload == nullptr) {
    throw std::invalid_argument("HuxerUI Android PlatformPayload must not be null");
  }
  LocalRef<jclass> payload_class(environment, environment->GetObjectClass(payload));
  LocalRef<jclass> envelope_class(environment, environment->FindClass("org/huxerui/PlatformPayload$Envelope"));
  LocalRef<jclass> texture_class(environment, environment->FindClass("org/huxerui/HuxerUIExternalTexture"));
  LocalRef<jclass> reference_class(environment, environment->FindClass("org/huxerui/HuxerUIFileReference"));
  LocalRef<jclass> buffer_class(environment, environment->FindClass("org/huxerui/HuxerUIBufferReference"));
  LocalRef<jclass> list_class(environment, environment->FindClass("java/util/List"));
  if (!payload_class || !envelope_class || !texture_class || !reference_class || !buffer_class || !list_class ||
      environment->ExceptionCheck()) {
    throw std::runtime_error("HuxerUI could not resolve the Android PlatformPayload bridge");
  }
  const jmethodID encode = environment->GetStaticMethodID(
      payload_class.Get(), "encodeEnvelope", "(Lorg/huxerui/PlatformPayload;)Lorg/huxerui/PlatformPayload$Envelope;");
  const jfieldID bytes_field = environment->GetFieldID(envelope_class.Get(), "bytes", "[B");
  const jfieldID textures_field = environment->GetFieldID(envelope_class.Get(), "externalTextures", "Ljava/util/List;");
  const jfieldID references_field = environment->GetFieldID(envelope_class.Get(), "fileReferences", "Ljava/util/List;");
  const jmethodID list_size = environment->GetMethodID(list_class.Get(), "size", "()I");
  const jmethodID list_get = environment->GetMethodID(list_class.Get(), "get", "(I)Ljava/lang/Object;");
  const jmethodID retain_texture = environment->GetMethodID(texture_class.Get(), "retainHandle", "()J");
  const jmethodID retain_reference = environment->GetMethodID(reference_class.Get(), "retainHandle", "()J");
  const jmethodID retain_buffer = environment->GetMethodID(buffer_class.Get(), "retainHandle", "()J");
  const jfieldID buffers_field = environment->GetFieldID(envelope_class.Get(), "bufferReferences", "Ljava/util/List;");
  if (encode == nullptr || bytes_field == nullptr || textures_field == nullptr || references_field == nullptr ||
      list_size == nullptr || list_get == nullptr || retain_texture == nullptr || retain_reference == nullptr ||
      retain_buffer == nullptr || buffers_field == nullptr ||
      environment->ExceptionCheck()) {
    throw std::runtime_error("HuxerUI Android PlatformPayload bridge methods do not match the SDK");
  }
  LocalRef<jobject> envelope(environment, environment->CallStaticObjectMethod(payload_class.Get(), encode, payload));
  if (!envelope || environment->ExceptionCheck()) {
    throw std::runtime_error("HuxerUI could not encode PlatformPayload from Android");
  }
  LocalRef<jbyteArray> bytes(environment,
                             static_cast<jbyteArray>(environment->GetObjectField(envelope.Get(), bytes_field)));
  LocalRef<jobject> textures(environment, environment->GetObjectField(envelope.Get(), textures_field));
  LocalRef<jobject> references(environment, environment->GetObjectField(envelope.Get(), references_field));
  LocalRef<jobject> buffers(environment, environment->GetObjectField(envelope.Get(), buffers_field));
  if (!bytes || !textures || !references || !buffers || environment->ExceptionCheck()) {
    throw std::runtime_error("HuxerUI Android PlatformPayload envelope is invalid");
  }
  const jint texture_count = environment->CallIntMethod(textures.Get(), list_size);
  if (texture_count < 0 || environment->ExceptionCheck()) {
    throw std::runtime_error("HuxerUI Android PlatformPayload capability table is invalid");
  }
  std::vector<std::shared_ptr<ExternalTexture>> external_textures;
  external_textures.reserve(static_cast<std::size_t>(texture_count));
  for (jint index = 0; index < texture_count; ++index) {
    LocalRef<jobject> texture(environment, environment->CallObjectMethod(textures.Get(), list_get, index));
    if (!texture || !environment->IsInstanceOf(texture.Get(), texture_class.Get()) || environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI Android PlatformPayload capability is not an external texture");
    }
    const jlong handle = environment->CallLongMethod(texture.Get(), retain_texture);
    if (handle == 0 || environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI Android PlatformPayload external texture is closed");
    }
    const std::unique_ptr<std::shared_ptr<ExternalTexture>> value(
        reinterpret_cast<std::shared_ptr<ExternalTexture>*>(static_cast<std::uintptr_t>(handle)));
    external_textures.push_back(*value);
  }
  const jint reference_count = environment->CallIntMethod(references.Get(), list_size);
  if (reference_count < 0 || environment->ExceptionCheck()) {
    throw std::runtime_error("HuxerUI Android PlatformPayload capability table is invalid");
  }
  std::vector<FileReference> file_references;
  file_references.reserve(static_cast<std::size_t>(reference_count));
  for (jint index = 0; index < reference_count; ++index) {
    LocalRef<jobject> reference(environment, environment->CallObjectMethod(references.Get(), list_get, index));
    if (!reference || !environment->IsInstanceOf(reference.Get(), reference_class.Get()) ||
        environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI Android PlatformPayload capability is not a file reference");
    }
    const jlong handle = environment->CallLongMethod(reference.Get(), retain_reference);
    if (handle == 0 || environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI Android PlatformPayload file reference is closed");
    }
    const std::unique_ptr<FileReference> value(reinterpret_cast<FileReference*>(static_cast<std::uintptr_t>(handle)));
    file_references.push_back(*value);
  }
  PlatformPayload::Envelope decoded;
  decoded.bytes = JavaByteArrayToBytes(environment, bytes.Get());
  decoded.external_textures = std::move(external_textures);
  decoded.file_references = std::move(file_references);
  const jint buffer_count = environment->CallIntMethod(buffers.Get(), list_size);
  if (buffer_count < 0 || environment->ExceptionCheck()) {
    throw std::runtime_error("HuxerUI Android buffer reference table is invalid");
  }
  decoded.buffer_references.reserve(static_cast<std::size_t>(buffer_count));
  for (jint index = 0; index < buffer_count; ++index) {
    LocalRef<jobject> buffer(environment, environment->CallObjectMethod(buffers.Get(), list_get, index));
    if (!buffer || !environment->IsInstanceOf(buffer.Get(), buffer_class.Get()) || environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI Android PlatformPayload capability is not a buffer reference");
    }
    const jlong handle = environment->CallLongMethod(buffer.Get(), retain_buffer);
    if (handle == 0 || environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI Android PlatformPayload buffer reference is closed");
    }
    const std::unique_ptr<BufferReference> reference(
        reinterpret_cast<BufferReference*>(static_cast<std::uintptr_t>(handle)));
    decoded.buffer_references.push_back(*reference);
  }
  return PlatformPayload::Decode(decoded);
}

} // namespace huxerui::android

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIExternalTexture_release(JNIEnv*, jclass, jlong handle) {
  delete reinterpret_cast<std::shared_ptr<huxerui::ExternalTexture>*>(static_cast<std::uintptr_t>(handle));
}

extern "C" JNIEXPORT jlong JNICALL Java_org_huxerui_HuxerUIExternalTexture_retain(JNIEnv*, jclass, jlong handle) {
  if (handle == 0) {
    return 0;
  }
  const auto* texture =
      reinterpret_cast<const std::shared_ptr<huxerui::ExternalTexture>*>(static_cast<std::uintptr_t>(handle));
  return static_cast<jlong>(
      reinterpret_cast<std::uintptr_t>(new std::shared_ptr<huxerui::ExternalTexture>(*texture)));
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIFileReference_release(JNIEnv*, jclass, jlong handle) {
  delete reinterpret_cast<huxerui::FileReference*>(static_cast<std::uintptr_t>(handle));
}

extern "C" JNIEXPORT jlong JNICALL Java_org_huxerui_HuxerUIFileReference_retain(JNIEnv*, jclass, jlong handle) {
  if (handle == 0) {
    return 0;
  }
  const auto* reference = reinterpret_cast<const huxerui::FileReference*>(static_cast<std::uintptr_t>(handle));
  return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(new huxerui::FileReference(*reference)));
}

namespace huxerui::android::detail {

namespace {

void ClearException(JNIEnv* environment) noexcept {
  if (environment != nullptr && environment->ExceptionCheck()) {
    environment->ExceptionClear();
  }
}

class JavaEnvironment final {
public:
  explicit JavaEnvironment(JavaVM* virtual_machine) : virtual_machine_(virtual_machine) {
    if (virtual_machine_ == nullptr) {
      return;
    }
    const jint result = virtual_machine_->GetEnv(reinterpret_cast<void**>(&environment_), JNI_VERSION_1_6);
    if (result == JNI_EDETACHED && virtual_machine_->AttachCurrentThread(&environment_, nullptr) == JNI_OK) {
      attached_ = true;
    } else if (result != JNI_OK) {
      environment_ = nullptr;
    }
  }

  ~JavaEnvironment() {
    if (attached_) {
      virtual_machine_->DetachCurrentThread();
    }
  }

  [[nodiscard]] JNIEnv* Get() const noexcept {
    return environment_;
  }

private:
  JavaVM* virtual_machine_ = nullptr;
  JNIEnv* environment_ = nullptr;
  bool attached_ = false;
};

void DeleteGlobalReference(JavaVM* virtual_machine, jobject reference) noexcept {
  if (reference == nullptr) {
    return;
  }
  JavaEnvironment attached(virtual_machine);
  if (JNIEnv* environment = attached.Get()) {
    environment->DeleteGlobalRef(reference);
  }
}

struct BufferOwner {
  JavaVM* virtual_machine = nullptr;
  jobject buffer = nullptr;
  jobject on_release = nullptr;
  jmethodID run = nullptr;
  bool published = false;

  ~BufferOwner() {
    JavaEnvironment attached(virtual_machine);
    if (JNIEnv* environment = attached.Get()) {
      // Destruction can occur during exception unwinding; a release callback must not replace that exception.
      LocalRef<jthrowable> pending(environment, environment->ExceptionOccurred());
      if (pending) {
        environment->ExceptionClear();
      }
      if (published && on_release != nullptr) {
        environment->CallVoidMethod(on_release, run);
        if (environment->ExceptionCheck()) {
          environment->ExceptionDescribe();
          environment->ExceptionClear();
        }
      }
      environment->DeleteGlobalRef(on_release);
      environment->DeleteGlobalRef(buffer);
      if (pending) {
        environment->Throw(pending.Get());
      }
    }
  }
};

jclass ResolveClass(JNIEnv* environment, jobject context, std::string_view class_name) {
  if (class_name.empty()) {
    throw std::invalid_argument("HuxerUI Android platform implementation class name must not be empty");
  }
  if (!huxerui::detail::IsValidUtf8(class_name)) {
    throw std::invalid_argument("HuxerUI Android platform implementation class name must contain valid UTF-8");
  }
  LocalRef<jclass> context_class(environment, environment->GetObjectClass(context));
  if (!context_class) {
    throw std::runtime_error("HuxerUI could not inspect the Android platform Context");
  }
  const jmethodID get_class_loader =
      environment->GetMethodID(context_class.Get(), "getClassLoader", "()Ljava/lang/ClassLoader;");
  if (get_class_loader == nullptr || environment->ExceptionCheck()) {
    ClearException(environment);
    throw std::runtime_error("HuxerUI could not resolve the Android host ClassLoader");
  }
  LocalRef<jobject> class_loader(environment, environment->CallObjectMethod(context, get_class_loader));
  if (!class_loader || environment->ExceptionCheck()) {
    ClearException(environment);
    throw std::runtime_error("HuxerUI could not obtain the Android host ClassLoader");
  }
  LocalRef<jclass> class_loader_class(environment, environment->GetObjectClass(class_loader.Get()));
  const jmethodID load_class =
      environment->GetMethodID(class_loader_class.Get(), "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
  LocalRef<jstring> java_name = Utf8ToJavaString(environment, class_name);
  if (load_class == nullptr || !java_name || environment->ExceptionCheck()) {
    ClearException(environment);
    throw std::runtime_error("HuxerUI Android host ClassLoader methods do not match the platform SDK");
  }
  jobject resolved = environment->CallObjectMethod(class_loader.Get(), load_class, java_name.Get());
  if (resolved == nullptr || environment->ExceptionCheck()) {
    ClearException(environment);
    throw std::logic_error("HuxerUI Android platform implementation class could not be loaded: " +
                           std::string(class_name));
  }
  return static_cast<jclass>(resolved);
}

jobject ConstructFactory(JNIEnv* environment, jclass implementation_class, const char* interface_name) {
  LocalRef<jclass> interface_class(environment, environment->FindClass(interface_name));
  const jmethodID constructor = environment->GetMethodID(implementation_class, "<init>", "()V");
  if (!interface_class || constructor == nullptr || environment->ExceptionCheck()) {
    ClearException(environment);
    throw std::logic_error("HuxerUI Android platform factory must provide a public no-argument constructor");
  }
  LocalRef<jobject> factory(environment, environment->NewObject(implementation_class, constructor));
  if (!factory || environment->ExceptionCheck()) {
    ClearException(environment);
    throw std::logic_error("HuxerUI Android platform factory constructor failed");
  }
  if (!environment->IsInstanceOf(factory.Get(), interface_class.Get())) {
    throw std::logic_error("HuxerUI Android platform factory does not implement the required SDK interface");
  }
  return factory.Release();
}

using PlatformResultCompletion = std::function<void(PlatformResult<PlatformPayload>)>;

class JavaBridgeSupport {
public:
  JavaBridgeSupport(PlatformAdapter& adapter, JNIEnv* environment, jobject context) : endpoint_adapter_(&adapter) {
    if (environment->GetJavaVM(&virtual_machine_) != JNI_OK) {
      throw std::runtime_error("HuxerUI could not access the Android Java VM for platform bridging");
    }
    LocalRef<jclass> emitter_class(environment, environment->FindClass("org/huxerui/HuxerUIPlatformChannel$Events"));
    LocalRef<jclass> result_class(environment, environment->FindClass("org/huxerui/HuxerUIPlatformChannel$Result"));
    LocalRef<jclass> cancellation_class(environment,
                                        environment->FindClass("org/huxerui/HuxerUIPlatformChannel$Cancellation"));
    if (!emitter_class || !result_class || !cancellation_class || environment->ExceptionCheck()) {
      ClearException(environment);
      throw std::runtime_error("HuxerUI Android platform bridge classes do not match the SDK");
    }
    emitter_constructor_ = environment->GetMethodID(emitter_class.Get(), "<init>", "(J)V");
    emitter_close_ = environment->GetMethodID(emitter_class.Get(), "close", "()V");
    result_constructor_ = environment->GetMethodID(result_class.Get(), "<init>", "(J)V");
    result_close_ = environment->GetMethodID(result_class.Get(), "close", "()V");
    cancel_ = environment->GetMethodID(cancellation_class.Get(), "cancel", "()V");
    if (emitter_constructor_ == nullptr || emitter_close_ == nullptr || result_constructor_ == nullptr ||
        result_close_ == nullptr || cancel_ == nullptr || environment->ExceptionCheck()) {
      ClearException(environment);
      throw std::runtime_error("HuxerUI Android platform bridge methods do not match the SDK");
    }
    context_ = environment->NewGlobalRef(context);
    emitter_class_ = static_cast<jclass>(environment->NewGlobalRef(emitter_class.Get()));
    result_class_ = static_cast<jclass>(environment->NewGlobalRef(result_class.Get()));
    cancellation_class_ = static_cast<jclass>(environment->NewGlobalRef(cancellation_class.Get()));
    if (context_ == nullptr || emitter_class_ == nullptr || result_class_ == nullptr ||
        cancellation_class_ == nullptr || environment->ExceptionCheck()) {
      ClearException(environment);
      DeleteGlobalReference(virtual_machine_, context_);
      DeleteGlobalReference(virtual_machine_, emitter_class_);
      DeleteGlobalReference(virtual_machine_, result_class_);
      DeleteGlobalReference(virtual_machine_, cancellation_class_);
      context_ = nullptr;
      emitter_class_ = nullptr;
      result_class_ = nullptr;
      cancellation_class_ = nullptr;
      throw std::runtime_error("HuxerUI could not retain the Android platform bridge classes");
    }
  }

  ~JavaBridgeSupport() {
    DeleteGlobalReference(virtual_machine_, context_);
    DeleteGlobalReference(virtual_machine_, emitter_class_);
    DeleteGlobalReference(virtual_machine_, result_class_);
    DeleteGlobalReference(virtual_machine_, cancellation_class_);
  }

  [[nodiscard]] huxerui::detail::PlatformChannelEndpoint NewEndpoint() const {
    return huxerui::detail::MakePlatformChannelEndpoint(*endpoint_adapter_);
  }

  [[nodiscard]] jobject Context() const noexcept {
    return context_;
  }

  [[nodiscard]] jobject NewEmitter(JNIEnv* environment, PlatformEventEmitter events) const {
    auto retained_events = std::make_unique<PlatformEventEmitter>(std::move(events));
    const jlong handle = static_cast<jlong>(reinterpret_cast<std::uintptr_t>(retained_events.get()));
    jobject emitter = environment->NewObject(emitter_class_, emitter_constructor_, handle);
    if (emitter == nullptr || environment->ExceptionCheck()) {
      ClearException(environment);
      throw std::runtime_error("HuxerUI could not create an Android platform event emitter");
    }
    retained_events.release();
    return emitter;
  }

  [[nodiscard]] jobject NewResult(JNIEnv* environment,
                                  std::function<void(PlatformResult<PlatformPayload>)> completion) const {
    auto retained_completion = std::make_unique<PlatformResultCompletion>(std::move(completion));
    const jlong handle = static_cast<jlong>(reinterpret_cast<std::uintptr_t>(retained_completion.get()));
    jobject result = environment->NewObject(result_class_, result_constructor_, handle);
    if (result == nullptr || environment->ExceptionCheck()) {
      ClearException(environment);
      throw std::runtime_error("HuxerUI could not create an Android platform result endpoint");
    }
    retained_completion.release();
    return result;
  }

  void CloseEmitter(JNIEnv* environment, jobject emitter) const noexcept {
    if (emitter != nullptr) {
      environment->CallVoidMethod(emitter, emitter_close_);
      ClearException(environment);
    }
  }

  void CloseResult(JNIEnv* environment, jobject result) const noexcept {
    if (result != nullptr) {
      environment->CallVoidMethod(result, result_close_);
      ClearException(environment);
    }
  }

  void Cancel(JNIEnv* environment, jobject cancellation) const noexcept {
    if (cancellation != nullptr) {
      environment->CallVoidMethod(cancellation, cancel_);
      ClearException(environment);
    }
  }

  [[nodiscard]] JavaVM* VirtualMachine() const noexcept {
    return virtual_machine_;
  }

private:
  PlatformAdapter* endpoint_adapter_ = nullptr;
  JavaVM* virtual_machine_ = nullptr;
  jobject context_ = nullptr;
  jclass emitter_class_ = nullptr;
  jclass result_class_ = nullptr;
  jclass cancellation_class_ = nullptr;
  jmethodID emitter_constructor_ = nullptr;
  jmethodID emitter_close_ = nullptr;
  jmethodID result_constructor_ = nullptr;
  jmethodID result_close_ = nullptr;
  jmethodID cancel_ = nullptr;
};

class JavaInvocation final {
public:
  JavaInvocation(std::shared_ptr<JavaBridgeSupport> bridge, JNIEnv* environment, jobject result, jobject cancellation)
      : bridge_(std::move(bridge)) {
    result_ = environment->NewGlobalRef(result);
    if (cancellation != nullptr) {
      cancellation_ = environment->NewGlobalRef(cancellation);
    }
    if (result_ == nullptr || environment->ExceptionCheck()) {
      ClearException(environment);
      throw std::runtime_error("HuxerUI could not retain an Android platform invocation");
    }
  }

  ~JavaInvocation() {
    JavaEnvironment attached(bridge_->VirtualMachine());
    JNIEnv* environment = attached.Get();
    if (environment == nullptr) {
      return;
    }
    bridge_->CloseResult(environment, result_);
    environment->DeleteGlobalRef(result_);
    if (cancellation_ != nullptr) {
      environment->DeleteGlobalRef(cancellation_);
    }
  }

  void Cancel() noexcept {
    JavaEnvironment attached(bridge_->VirtualMachine());
    if (JNIEnv* environment = attached.Get()) {
      bridge_->Cancel(environment, cancellation_);
      bridge_->CloseResult(environment, result_);
    }
  }

private:
  std::shared_ptr<JavaBridgeSupport> bridge_;
  jobject result_ = nullptr;
  jobject cancellation_ = nullptr;
};

struct JavaInstanceState {
  std::shared_ptr<JavaBridgeSupport> bridge;
  jobject instance = nullptr;
  jobject emitter = nullptr;
  jmethodID invoke = nullptr;
  jmethodID dispose = nullptr;
  bool disposed = false;

  ~JavaInstanceState() {
    Dispose();
  }

  std::function<void()> Invoke(std::string method, PlatformPayload arguments,
                               std::function<void(PlatformResult<PlatformPayload>)> completion) {
    JavaEnvironment attached(bridge->VirtualMachine());
    JNIEnv* environment = attached.Get();
    if (environment == nullptr || disposed) {
      throw std::logic_error("HuxerUI Android platform instance is disposed");
    }
    LocalRef<jstring> java_method = Utf8ToJavaString(environment, method);
    LocalRef<jobject> java_arguments = PlatformPayloadToJava(environment, arguments);
    LocalRef<jobject> result(environment, bridge->NewResult(environment, std::move(completion)));
    LocalRef<jobject> cancellation(environment, environment->CallObjectMethod(instance, invoke, java_method.Get(),
                                                                              java_arguments.Get(), result.Get()));
    if (environment->ExceptionCheck()) {
      ClearException(environment);
      bridge->CloseResult(environment, result.Get());
      throw std::logic_error("HuxerUI Android platform invoke raised a Java exception");
    }
    auto invocation = std::make_shared<JavaInvocation>(bridge, environment, result.Get(), cancellation.Get());
    return [invocation] { invocation->Cancel(); };
  }

  void Dispose(JNIEnv* preferred_environment = nullptr) noexcept {
    if (disposed) {
      return;
    }
    disposed = true;
    JavaEnvironment attached(preferred_environment == nullptr ? bridge->VirtualMachine() : nullptr);
    JNIEnv* environment = preferred_environment != nullptr ? preferred_environment : attached.Get();
    if (environment == nullptr) {
      return;
    }
    if (instance != nullptr) {
      if (dispose != nullptr) {
        environment->CallVoidMethod(instance, dispose);
      }
      ClearException(environment);
    }
    bridge->CloseEmitter(environment, emitter);
    if (emitter != nullptr) {
      environment->DeleteGlobalRef(emitter);
      emitter = nullptr;
    }
    if (instance != nullptr) {
      environment->DeleteGlobalRef(instance);
      instance = nullptr;
    }
  }
};

void ConnectInstance(const huxerui::detail::PlatformChannelEndpoint& endpoint,
                     const std::shared_ptr<JavaInstanceState>& instance) {
  endpoint.Connect({
      .invoke =
          [instance](std::string method, PlatformPayload arguments,
                     std::function<void(PlatformResult<PlatformPayload>)> completion) {
            return instance->Invoke(std::move(method), std::move(arguments), std::move(completion));
          },
      .dispose = [instance] { instance->Dispose(); },
  });
}

} // namespace

class JavaPlatformModuleFactoryState final {
public:
  JavaPlatformModuleFactoryState(PlatformAdapter& adapter, std::string class_name) {
    const PlatformEnv env = GetPlatformEnv(adapter);
    bridge = std::make_shared<JavaBridgeSupport>(adapter, env.jni, env.context);
    LocalRef<jclass> implementation_class(env.jni, ResolveClass(env.jni, env.context, class_name));
    LocalRef<jobject> local_factory(
        env.jni, ConstructFactory(env.jni, implementation_class.Get(), "org/huxerui/HuxerUIPlatformModule$Factory"));
    create = env.jni->GetMethodID(
        implementation_class.Get(), "create",
        "(Landroid/content/Context;Lorg/huxerui/PlatformPayload;Lorg/huxerui/HuxerUIPlatformChannel$Events;)"
        "Lorg/huxerui/HuxerUIPlatformModule;");
    if (create == nullptr || env.jni->ExceptionCheck()) {
      ClearException(env.jni);
      throw std::logic_error("HuxerUI Android PlatformModule factory methods do not match the SDK");
    }
    factory = env.jni->NewGlobalRef(local_factory.Get());
    if (factory == nullptr || env.jni->ExceptionCheck()) {
      ClearException(env.jni);
      throw std::runtime_error("HuxerUI could not retain the Android PlatformModule factory");
    }
  }

  ~JavaPlatformModuleFactoryState() {
    DeleteGlobalReference(bridge->VirtualMachine(), factory);
  }

  std::shared_ptr<JavaBridgeSupport> bridge;
  jobject factory = nullptr;
  jmethodID create = nullptr;
};

class JavaPlatformViewFactoryState final {
public:
  JavaPlatformViewFactoryState(PlatformAdapter& adapter, std::string class_name) {
    const PlatformEnv env = GetPlatformEnv(adapter);
    bridge = std::make_shared<JavaBridgeSupport>(adapter, env.jni, env.context);
    LocalRef<jclass> implementation_class(env.jni, ResolveClass(env.jni, env.context, class_name));
    LocalRef<jobject> local_factory(
        env.jni, ConstructFactory(env.jni, implementation_class.Get(), "org/huxerui/HuxerUIPlatformView$Factory"));
    create = env.jni->GetMethodID(
        implementation_class.Get(), "create",
        "(Landroid/content/Context;Lorg/huxerui/PlatformPayload;Lorg/huxerui/HuxerUIPlatformChannel$Events;)"
        "Lorg/huxerui/HuxerUIPlatformView;");
    if (create == nullptr || env.jni->ExceptionCheck()) {
      ClearException(env.jni);
      throw std::logic_error("HuxerUI Android PlatformView factory methods do not match the SDK");
    }
    factory = env.jni->NewGlobalRef(local_factory.Get());
    if (factory == nullptr || env.jni->ExceptionCheck()) {
      ClearException(env.jni);
      throw std::runtime_error("HuxerUI could not retain the Android PlatformView factory");
    }
  }

  ~JavaPlatformViewFactoryState() {
    DeleteGlobalReference(bridge->VirtualMachine(), factory);
  }

  std::shared_ptr<JavaBridgeSupport> bridge;
  jobject factory = nullptr;
  jmethodID create = nullptr;
};

class JavaPlatformViewInstance final {
public:
  ~JavaPlatformViewInstance() {
    if (!state) {
      return;
    }
    JavaEnvironment attached(state->bridge->VirtualMachine());
    if (JNIEnv* environment = attached.Get()) {
      channel.Close();
      state->Dispose(environment);
      if (view != nullptr) {
        environment->DeleteGlobalRef(view);
      }
    }
  }

  std::shared_ptr<JavaInstanceState> state;
  PlatformChannel channel;
  jobject view = nullptr;
  jmethodID update = nullptr;
};

std::shared_ptr<JavaPlatformModuleFactoryState> PrepareJavaPlatformModuleFactory(PlatformAdapter& adapter,
                                                                                 std::string class_name) {
  return std::make_shared<JavaPlatformModuleFactoryState>(adapter, std::move(class_name));
}

PlatformChannel CreateJavaPlatformModule(const std::shared_ptr<JavaPlatformModuleFactoryState>& factory,
                                         PlatformPayload options) {
  if (!factory) {
    throw std::invalid_argument("HuxerUI Android Java PlatformModule factory must not be empty");
  }
  JavaEnvironment attached(factory->bridge->VirtualMachine());
  JNIEnv* environment = attached.Get();
  if (environment == nullptr) {
    throw std::runtime_error("HuxerUI could not access JNI while creating an Android PlatformModule");
  }
  auto state = std::make_shared<JavaInstanceState>();
  state->bridge = factory->bridge;
  const huxerui::detail::PlatformChannelEndpoint endpoint = factory->bridge->NewEndpoint();
  LocalRef<jobject> emitter(environment, factory->bridge->NewEmitter(environment, endpoint.Events()));
  LocalRef<jobject> java_options = PlatformPayloadToJava(environment, options);
  LocalRef<jobject> instance(environment, environment->CallObjectMethod(factory->factory, factory->create,
                                                                        factory->bridge->Context(), java_options.Get(),
                                                                        emitter.Get()));
  if (!instance || environment->ExceptionCheck()) {
    ClearException(environment);
    factory->bridge->CloseEmitter(environment, emitter.Get());
    throw std::logic_error("HuxerUI Android PlatformModule factory failed while creating");
  }
  LocalRef<jclass> instance_class(environment, environment->GetObjectClass(instance.Get()));
  state->invoke = environment->GetMethodID(
      instance_class.Get(), "invoke",
      "(Ljava/lang/String;Lorg/huxerui/PlatformPayload;Lorg/huxerui/HuxerUIPlatformChannel$Result;)"
      "Lorg/huxerui/HuxerUIPlatformChannel$Cancellation;");
  state->dispose = environment->GetMethodID(instance_class.Get(), "dispose", "()V");
  state->instance = environment->NewGlobalRef(instance.Get());
  state->emitter = environment->NewGlobalRef(emitter.Get());
  if (state->invoke == nullptr || state->dispose == nullptr || state->instance == nullptr ||
      state->emitter == nullptr || environment->ExceptionCheck()) {
    ClearException(environment);
    factory->bridge->CloseEmitter(environment, emitter.Get());
    throw std::logic_error("HuxerUI Android PlatformModule instance methods do not match the SDK");
  }
  ConnectInstance(endpoint, state);
  return endpoint.Channel();
}

std::shared_ptr<JavaPlatformViewFactoryState> PrepareJavaPlatformViewFactory(PlatformAdapter& adapter,
                                                                             std::string class_name) {
  return std::make_shared<JavaPlatformViewFactoryState>(adapter, std::move(class_name));
}

std::shared_ptr<JavaPlatformViewInstance>
CreateJavaPlatformView(const std::shared_ptr<JavaPlatformViewFactoryState>& factory, PlatformPayload properties,
                       PlatformEventEmitter events, bool channel_required) {
  if (!factory) {
    throw std::invalid_argument("HuxerUI Android Java PlatformView factory must not be empty");
  }
  JavaEnvironment attached(factory->bridge->VirtualMachine());
  JNIEnv* environment = attached.Get();
  if (environment == nullptr) {
    throw std::runtime_error("HuxerUI could not access JNI while creating an Android PlatformView");
  }
  auto instance = std::make_shared<JavaPlatformViewInstance>();
  instance->state = std::make_shared<JavaInstanceState>();
  instance->state->bridge = factory->bridge;
  const huxerui::detail::PlatformChannelEndpoint endpoint =
      channel_required ? factory->bridge->NewEndpoint() : huxerui::detail::PlatformChannelEndpoint{};
  LocalRef<jobject> emitter(environment, factory->bridge->NewEmitter(environment, std::move(events)));
  LocalRef<jobject> java_properties = PlatformPayloadToJava(environment, properties);
  LocalRef<jobject> java_instance(environment, environment->CallObjectMethod(factory->factory, factory->create,
                                                                             factory->bridge->Context(),
                                                                             java_properties.Get(), emitter.Get()));
  if (!java_instance || environment->ExceptionCheck()) {
    ClearException(environment);
    factory->bridge->CloseEmitter(environment, emitter.Get());
    throw std::logic_error("HuxerUI Android PlatformView factory failed while creating");
  }
  LocalRef<jclass> instance_class(environment, environment->GetObjectClass(java_instance.Get()));
  const jmethodID get_view = environment->GetMethodID(instance_class.Get(), "getView", "()Landroid/view/View;");
  instance->update = environment->GetMethodID(instance_class.Get(), "update", "(Lorg/huxerui/PlatformPayload;)V");
  if (channel_required) {
    instance->state->invoke = environment->GetMethodID(
        instance_class.Get(), "invoke",
        "(Ljava/lang/String;Lorg/huxerui/PlatformPayload;Lorg/huxerui/HuxerUIPlatformChannel$Result;)"
        "Lorg/huxerui/HuxerUIPlatformChannel$Cancellation;");
  }
  instance->state->dispose = environment->GetMethodID(instance_class.Get(), "dispose", "()V");
  if (get_view == nullptr || instance->update == nullptr || (channel_required && instance->state->invoke == nullptr) ||
      instance->state->dispose == nullptr || environment->ExceptionCheck()) {
    ClearException(environment);
    factory->bridge->CloseEmitter(environment, emitter.Get());
    throw std::logic_error("HuxerUI Android PlatformView instance methods do not match the SDK");
  }
  LocalRef<jobject> view(environment, environment->CallObjectMethod(java_instance.Get(), get_view));
  instance->state->instance = environment->NewGlobalRef(java_instance.Get());
  instance->state->emitter = environment->NewGlobalRef(emitter.Get());
  instance->view = environment->NewGlobalRef(view.Get());
  if (!view || instance->state->instance == nullptr || instance->state->emitter == nullptr ||
      instance->view == nullptr || environment->ExceptionCheck()) {
    ClearException(environment);
    factory->bridge->CloseEmitter(environment, emitter.Get());
    throw std::logic_error("HuxerUI Android PlatformView instance methods do not match the SDK");
  }
  if (channel_required) {
    ConnectInstance(endpoint, instance->state);
    instance->channel = endpoint.Channel();
  }
  return instance;
}

jobject GetJavaPlatformView(JNIEnv* environment, const std::shared_ptr<JavaPlatformViewInstance>& instance) {
  if (!instance || instance->view == nullptr) {
    return nullptr;
  }
  return environment->NewLocalRef(instance->view);
}

void UpdateJavaPlatformView(JNIEnv* environment, const std::shared_ptr<JavaPlatformViewInstance>& instance,
                            PlatformPayload properties) {
  if (!instance || instance->state->disposed) {
    throw std::logic_error("HuxerUI Android PlatformView instance is disposed");
  }
  LocalRef<jobject> java_properties = PlatformPayloadToJava(environment, properties);
  environment->CallVoidMethod(instance->state->instance, instance->update, java_properties.Get());
  if (environment->ExceptionCheck()) {
    ClearException(environment);
    throw std::logic_error("HuxerUI Android PlatformView update raised a Java exception");
  }
}

void DisposeJavaPlatformView(JNIEnv* environment, const std::shared_ptr<JavaPlatformViewInstance>& instance) noexcept {
  if (!instance) {
    return;
  }
  instance->channel.Close();
  instance->state->Dispose(environment);
  if (instance->view != nullptr) {
    environment->DeleteGlobalRef(instance->view);
    instance->view = nullptr;
  }
}

PlatformChannel GetJavaPlatformViewChannel(const std::shared_ptr<JavaPlatformViewInstance>& instance) {
  if (!instance || instance->state->disposed) {
    return {};
  }
  return instance->channel;
}

} // namespace huxerui::android::detail

extern "C" JNIEXPORT jobject JNICALL Java_org_huxerui_HuxerUIPlatformChannel_nativeEmit(
    JNIEnv* environment, jclass, jlong handle, jstring event, jobject payload
) {
  if (handle == 0 || event == nullptr || payload == nullptr) {
    return nullptr;
  }
  auto* retained = reinterpret_cast<huxerui::PlatformEventEmitter*>(static_cast<std::uintptr_t>(handle));
  try {
    // Emit may synchronously reenter Java and close the bridge, so retain a callable copy before invoking user code.
    const huxerui::PlatformEventEmitter emitter = *retained;
    std::optional<huxerui::PlatformPayload> result =
        emitter.Emit(huxerui::android::JavaStringToUtf8(environment, event),
                     huxerui::android::JavaPlatformPayloadToCpp(environment, payload));
    if (result.has_value()) {
      return huxerui::android::PlatformPayloadToJava(environment, *result).Release();
    }
  } catch (...) {
    huxerui::android::detail::ClearException(environment);
  }
  return nullptr;
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIPlatformChannel_nativeReleaseEvent(JNIEnv*, jclass,
                                                                                             jlong handle) {
  delete reinterpret_cast<huxerui::PlatformEventEmitter*>(static_cast<std::uintptr_t>(handle));
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIPlatformChannel_nativeComplete(JNIEnv* environment, jclass,
                                                                                         jlong handle,
                                                                                         jobject payload) {
  std::unique_ptr<huxerui::android::detail::PlatformResultCompletion> completion(
      reinterpret_cast<huxerui::android::detail::PlatformResultCompletion*>(static_cast<std::uintptr_t>(handle)));
  if (!completion || payload == nullptr) {
    return;
  }
  try {
    (*completion)(huxerui::android::JavaPlatformPayloadToCpp(environment, payload));
  } catch (...) {
    huxerui::android::detail::ClearException(environment);
    (*completion)(huxerui::PlatformError{"huxerui/invalid-result",
                                         "HuxerUI Android platform call returned an invalid result payload", {}});
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIPlatformChannel_nativeFail(JNIEnv* environment, jclass,
                                                                                     jlong handle, jstring code,
                                                                                     jstring message, jobject details) {
  std::unique_ptr<huxerui::android::detail::PlatformResultCompletion> completion(
      reinterpret_cast<huxerui::android::detail::PlatformResultCompletion*>(static_cast<std::uintptr_t>(handle)));
  if (!completion || code == nullptr || message == nullptr || details == nullptr) {
    return;
  }
  try {
    std::string error_code = huxerui::android::JavaStringToUtf8(environment, code);
    std::string error_message = huxerui::android::JavaStringToUtf8(environment, message);
    huxerui::PlatformPayload error_details = huxerui::android::JavaPlatformPayloadToCpp(environment, details);
    (*completion)(huxerui::PlatformError{std::move(error_code), std::move(error_message), std::move(error_details)});
  } catch (...) {
    huxerui::android::detail::ClearException(environment);
    (*completion)(huxerui::PlatformError{"huxerui/invalid-error",
                                         "HuxerUI Android platform call returned an invalid error payload", {}});
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIPlatformChannel_nativeReleaseResult(JNIEnv*, jclass,
                                                                                              jlong handle) {
  delete reinterpret_cast<huxerui::android::detail::PlatformResultCompletion*>(
      static_cast<std::uintptr_t>(handle));
}

namespace {

void ThrowBufferException(JNIEnv* environment, const std::exception& exception) {
  if (!environment->ExceptionCheck()) {
    const char* name = dynamic_cast<const std::bad_alloc*>(&exception) != nullptr
                           ? "java/lang/OutOfMemoryError"
                           : "java/lang/IllegalArgumentException";
    huxerui::android::LocalRef<jclass> type(environment, environment->FindClass(name));
    if (type) {
      environment->ThrowNew(type.Get(), exception.what());
    }
  }
}

const huxerui::BufferReference& RequireBuffer(jlong handle) {
  if (handle == 0) {
    throw std::invalid_argument("HuxerUI buffer reference is closed");
  }
  return *reinterpret_cast<const huxerui::BufferReference*>(static_cast<std::uintptr_t>(handle));
}

} // namespace

extern "C" JNIEXPORT jlong JNICALL Java_org_huxerui_HuxerUIBufferReference_create(
    JNIEnv* environment, jclass, jobject buffer, jobject on_release) {
  try {
    if (buffer == nullptr) {
      throw std::invalid_argument("HuxerUI buffer reference requires a direct ByteBuffer");
    }
    const jlong size = environment->GetDirectBufferCapacity(buffer);
    const auto* address = static_cast<const std::byte*>(environment->GetDirectBufferAddress(buffer));
    if (size < 0 || size > std::numeric_limits<jint>::max() || (size != 0 && address == nullptr)) {
      throw std::invalid_argument("HuxerUI buffer reference requires an accessible direct ByteBuffer");
    }
    auto owner = std::make_shared<huxerui::android::detail::BufferOwner>();
    if (environment->GetJavaVM(&owner->virtual_machine) != JNI_OK) {
      throw std::runtime_error("HuxerUI could not retain the Java VM for a buffer reference");
    }
    owner->buffer = environment->NewGlobalRef(buffer);
    if (owner->buffer == nullptr || environment->ExceptionCheck()) {
      throw std::runtime_error("HuxerUI could not retain direct buffer storage");
    }
    if (on_release != nullptr) {
      huxerui::android::LocalRef<jclass> type(environment, environment->GetObjectClass(on_release));
      owner->run = type ? environment->GetMethodID(type.Get(), "run", "()V") : nullptr;
      if (owner->run == nullptr || environment->ExceptionCheck()) {
        throw std::invalid_argument("HuxerUI buffer release callback must implement Runnable");
      }
      owner->on_release = environment->NewGlobalRef(on_release);
      if (owner->on_release == nullptr || environment->ExceptionCheck()) {
        throw std::runtime_error("HuxerUI could not retain the buffer release callback");
      }
    }
    auto reference = std::make_unique<huxerui::BufferReference>(
        std::span<const std::byte>(address, static_cast<std::size_t>(size)), owner);
    owner->published = true;
    return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(reference.release()));
  } catch (const std::exception& exception) {
    ThrowBufferException(environment, exception);
    return 0;
  }
}

extern "C" JNIEXPORT jlong JNICALL Java_org_huxerui_HuxerUIBufferReference_retain(
    JNIEnv* environment, jclass, jlong handle) {
  try {
    auto reference = std::make_unique<huxerui::BufferReference>(RequireBuffer(handle));
    return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(reference.release()));
  } catch (const std::exception& exception) {
    ThrowBufferException(environment, exception);
    return 0;
  }
}

extern "C" JNIEXPORT void JNICALL Java_org_huxerui_HuxerUIBufferReference_release(JNIEnv*, jclass, jlong handle) {
  delete reinterpret_cast<huxerui::BufferReference*>(static_cast<std::uintptr_t>(handle));
}

extern "C" JNIEXPORT jobject JNICALL Java_org_huxerui_HuxerUIBufferReference_view(
    JNIEnv* environment, jclass, jlong handle) {
  try {
    const auto bytes = RequireBuffer(handle).AsBytes();
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<jint>::max())) {
      throw std::invalid_argument("HuxerUI buffer reference exceeds the ByteBuffer capacity range");
    }
    static std::byte empty_buffer;
    void* address = bytes.data() == nullptr ? &empty_buffer : const_cast<std::byte*>(bytes.data());
    return environment->NewDirectByteBuffer(address, static_cast<jlong>(bytes.size()));
  } catch (const std::exception& exception) {
    ThrowBufferException(environment, exception);
    return nullptr;
  }
}

extern "C" JNIEXPORT jlong JNICALL Java_org_huxerui_HuxerUIBufferReference_slice(
    JNIEnv* environment, jclass, jlong handle, jint offset, jint size) {
  try {
    if (offset < 0 || size < 0) {
      throw std::invalid_argument("HuxerUI buffer slice requires nonnegative offset and size");
    }
    auto reference = std::make_unique<huxerui::BufferReference>(
        RequireBuffer(handle).Slice(static_cast<std::size_t>(offset), static_cast<std::size_t>(size)));
    return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(reference.release()));
  } catch (const std::exception& exception) {
    ThrowBufferException(environment, exception);
    return 0;
  }
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_huxerui_HuxerUIBufferReference_equal(
    JNIEnv* environment, jclass, jlong left, jlong right) {
  try {
    return RequireBuffer(left) == RequireBuffer(right);
  } catch (const std::exception& exception) {
    ThrowBufferException(environment, exception);
    return false;
  }
}

extern "C" JNIEXPORT jint JNICALL Java_org_huxerui_HuxerUIBufferReference_hash(
    JNIEnv* environment, jclass, jlong handle) {
  try {
    const auto bytes = RequireBuffer(handle).AsBytes();
    return static_cast<jint>(reinterpret_cast<std::uintptr_t>(bytes.data()) ^ bytes.size());
  } catch (const std::exception& exception) {
    ThrowBufferException(environment, exception);
    return 0;
  }
}
