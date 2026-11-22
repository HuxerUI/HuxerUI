#include "ohos_platform_registry.h"

#include <arkui/native_node_napi.h>
#include <hilog/log.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include <huxerui/ohos/platform_registry.h>

#include "application/platform_registry_internal.h"
#include "ohos_file_picker.h"

namespace huxerui::detail {
namespace {

void Check(napi_env env, napi_status status) {
  if (status == napi_ok) return;
  if (status == napi_pending_exception) {
    napi_value ignored = nullptr;
    napi_get_and_clear_last_exception(env, &ignored);
  }
  throw std::runtime_error("HuxerUI failed to communicate with the ArkTS extension");
}

class HandleScope {
public:
  explicit HandleScope(napi_env env) : env_(env) { Check(env_, napi_open_handle_scope(env_, &scope_)); }
  ~HandleScope() { napi_close_handle_scope(env_, scope_); }

private:
  napi_env env_;
  napi_handle_scope scope_ = nullptr;
};

napi_valuetype Type(napi_env env, napi_value value) {
  if (!value) return napi_undefined;
  napi_valuetype type;
  Check(env, napi_typeof(env, value, &type));
  return type;
}

napi_value Undefined(napi_env env) {
  napi_value result;
  Check(env, napi_get_undefined(env, &result));
  return result;
}

napi_value Object(napi_env env) {
  napi_value result;
  Check(env, napi_create_object(env, &result));
  return result;
}

napi_value Property(napi_env env, napi_value object, const char* name) {
  napi_value value;
  Check(env, napi_get_named_property(env, object, name, &value));
  return value;
}

napi_value String(napi_env env, std::string_view value) {
  napi_value result;
  Check(env, napi_create_string_utf8(env, value.empty() ? "" : value.data(), value.size(), &result));
  return result;
}

std::string ReadString(napi_env env, napi_value value) {
  if (Type(env, value) != napi_string) throw std::invalid_argument("HuxerUI expected an ArkTS string");
  std::size_t length = 0;
  Check(env, napi_get_value_string_utf16(env, value, nullptr, 0, &length));
  std::u16string utf16(length + 1, u'\0');
  Check(env, napi_get_value_string_utf16(env, value, utf16.data(), utf16.size(), &length));
  for (std::size_t index = 0; index < length; ++index) {
    const auto unit = utf16[index];
    if (unit < 0xD800 || unit > 0xDFFF) continue;
    if (unit <= 0xDBFF && index + 1 < length && utf16[index + 1] >= 0xDC00 && utf16[index + 1] <= 0xDFFF) {
      ++index;
    } else {
      throw std::invalid_argument("HuxerUI ArkTS text contains an unpaired UTF-16 surrogate");
    }
  }
  Check(env, napi_get_value_string_utf8(env, value, nullptr, 0, &length));
  std::string result(length + 1, '\0');
  Check(env, napi_get_value_string_utf8(env, value, result.data(), result.size(), &length));
  result.resize(length);
  return result;
}

napi_value Number(napi_env env, double number) {
  napi_value result;
  Check(env, napi_create_double(env, number, &result));
  return result;
}

std::size_t ReadIndex(napi_env env, napi_value value) {
  double result = 0;
  Check(env, napi_get_value_double(env, value, &result));
  if (result < 0 || result > 9007199254740991.0 || std::trunc(result) != result) {
    throw std::invalid_argument("HuxerUI expected a nonnegative ArkTS integer index");
  }
  return static_cast<std::size_t>(result);
}

napi_value Boolean(napi_env env, bool value) {
  napi_value result;
  Check(env, napi_get_boolean(env, value, &result));
  return result;
}

napi_value ByteArray(napi_env env, std::span<const std::byte> bytes) {
  napi_value result;
  void* data = nullptr;
  Check(env, napi_create_arraybuffer(env, bytes.size(), &data, &result));
  if (!bytes.empty()) std::memcpy(data, bytes.data(), bytes.size());
  return result;
}

Bytes ReadBytes(napi_env env, napi_value value) {
  void* data = nullptr;
  std::size_t length = 0;
  Check(env, napi_get_arraybuffer_info(env, value, &data, &length));
  Bytes result(length);
  if (length != 0) std::memcpy(result.data(), data, length);
  return result;
}

std::uint32_t ArrayLength(napi_env env, napi_value value) {
  std::uint32_t length = 0;
  Check(env, napi_get_array_length(env, value, &length));
  return length;
}

napi_value Element(napi_env env, napi_value list, std::uint32_t index) {
  napi_value value;
  Check(env, napi_get_element(env, list, index, &value));
  return value;
}

template <class Range, class Convert> napi_value Array(napi_env env, const Range& range, Convert convert) {
  napi_value result;
  Check(env, napi_create_array_with_length(env, range.size(), &result));
  std::uint32_t index = 0;
  for (const auto& item : range) Check(env, napi_set_element(env, result, index++, convert(item)));
  return result;
}

napi_value Call(napi_env env, napi_value object, const char* method, std::span<const napi_value> arguments = {}) {
  napi_value result;
  const auto function = Property(env, object, method);
  if (Type(env, function) != napi_function) throw std::invalid_argument("HuxerUI ArkTS extension is missing a method");
  Check(env, napi_call_function(env, object, function, arguments.size(), arguments.data(), &result));
  return result;
}

napi_ref Retain(napi_env env, napi_value value) {
  napi_ref result = nullptr;
  if (Type(env, value) != napi_undefined) Check(env, napi_create_reference(env, value, 1, &result));
  return result;
}

napi_value Get(napi_env env, napi_ref reference) {
  if (!reference) return Undefined(env);
  napi_value result;
  Check(env, napi_get_reference_value(env, reference, &result));
  return result;
}

void Release(napi_env env, napi_ref& reference) noexcept {
  if (reference) napi_delete_reference(env, std::exchange(reference, nullptr));
}

struct CallInfo {
  napi_value self = nullptr;
  std::array<napi_value, 3> args{};
  std::size_t count = args.size();

  CallInfo(napi_env env, napi_callback_info info) {
    Check(env, napi_get_cb_info(env, info, &count, args.data(), &self, nullptr));
  }

  napi_value Argument(std::size_t index) const {
    if (index >= count || index >= args.size()) {
      throw std::invalid_argument("HuxerUI ArkTS extension is missing an argument");
    }
    return args[index];
  }
};

template <auto Callback> napi_value Invoke(napi_env env, napi_callback_info info) noexcept {
  try {
    return Callback(env, CallInfo(env, info));
  } catch (const std::exception& error) {
    napi_throw_error(env, nullptr, error.what());
    return nullptr;
  }
}

napi_property_descriptor Method(const char* name, napi_callback callback) {
  return {name, nullptr, callback, nullptr, nullptr, nullptr, napi_default, nullptr};
}

template <class Value> void DeleteWrapped(napi_env, void* value, void*) noexcept { delete static_cast<Value*>(value); }

template <class Value> napi_value Wrap(napi_env env, Value value, const napi_type_tag& tag,
                                      std::initializer_list<napi_property_descriptor> methods) {
  auto data = std::make_unique<Value>(std::move(value));
  const auto result = Object(env);
  Check(env, napi_type_tag_object(env, result, &tag));
  Check(env, napi_wrap(env, result, data.get(), DeleteWrapped<Value>, nullptr, nullptr));
  data.release();
  Check(env, napi_define_properties(env, result, methods.size(), methods.begin()));
  return result;
}

template <class Value> Value& Unwrap(napi_env env, napi_value value, const napi_type_tag& tag) {
  bool matches = false;
  void* data = nullptr;
  if (Type(env, value) != napi_object || napi_check_object_type_tag(env, value, &tag, &matches) != napi_ok ||
      !matches || napi_unwrap(env, value, &data) != napi_ok || !data) {
    throw std::invalid_argument("HuxerUI expected a platform value from the ArkTS bridge");
  }
  return *static_cast<Value*>(data);
}

constexpr napi_type_tag payload_tag{0x48555845524F484FULL, 1};
constexpr napi_type_tag texture_tag{0x48555845524F484FULL, 2};
constexpr napi_type_tag file_tag{0x48555845524F484FULL, 3};
constexpr napi_type_tag buffer_tag{0x48555845524F484FULL, 4};

template <class T> constexpr const napi_type_tag& CapabilityTag() {
  if constexpr (std::same_as<T, FileReference>) return file_tag;
  else if constexpr (std::same_as<T, BufferReference>) return buffer_tag;
  else return texture_tag;
}

template <class T> T& GetCapability(napi_env env, napi_value value) {
  auto& capability = Unwrap<std::optional<T>>(env, value, CapabilityTag<T>());
  if (!capability) throw std::logic_error("HuxerUI ArkTS capability is closed");
  return *capability;
}

template <class T> napi_value CloseCapability(napi_env env, const CallInfo& call) {
  Unwrap<std::optional<T>>(env, call.self, CapabilityTag<T>()).reset();
  return Undefined(env);
}

napi_value FileUri(napi_env env, const CallInfo& call) {
  const auto uri = OhosFileUri(GetCapability<FileReference>(env, call.self));
  return uri ? String(env, *uri) : Undefined(env);
}

napi_value WrapFile(napi_env env, FileReference value) {
  return Wrap(env, std::optional(std::move(value)), file_tag, {
      Method("uri", Invoke<FileUri>), Method("close", Invoke<CloseCapability<FileReference>>),
  });
}

napi_value RetainGrantedFile(napi_env env, const CallInfo& call) {
  auto files = ReadOhosGrantedFiles(env, call.Argument(0));
  if (!files || files->size() != 1) return Undefined(env);
  return WrapFile(env, std::move(files->front()));
}

napi_value FileFromUri(napi_env env, const CallInfo& call) {
  const auto uri = String(env, ReadString(env, call.Argument(0)));
  bool writable = false;
  if (Type(env, call.args[1]) != napi_undefined) Check(env, napi_get_value_bool(env, call.args[1], &writable));
  napi_value callback;
  Check(env, napi_create_function(env, nullptr, 0, Invoke<RetainGrantedFile>, nullptr, &callback));
  const std::array arguments{uri, Boolean(env, writable), callback};
  return Call(env, Property(env, call.self, "files"), "retainPlatformFile", arguments);
}

napi_value BufferLength(napi_env env, const CallInfo& call) {
  return Number(env, static_cast<double>(GetCapability<BufferReference>(env, call.self).AsBytes().size()));
}

napi_value CopyBuffer(napi_env env, const CallInfo& call) {
  return ByteArray(env, GetCapability<BufferReference>(env, call.self).AsBytes());
}

napi_value ReadBufferInto(napi_env env, const CallInfo& call) {
  const auto source = GetCapability<BufferReference>(env, call.self).AsBytes();
  void* destination = nullptr;
  std::size_t size = 0;
  Check(env, napi_get_arraybuffer_info(env, call.Argument(0), &destination, &size));
  const auto offset = ReadIndex(env, call.Argument(1));
  if (offset > source.size() || size > source.size() - offset) {
    throw std::invalid_argument("HuxerUI ArkTS buffer read exceeds the retained range");
  }
  if (size != 0) std::memmove(destination, source.data() + offset, size);
  return Undefined(env);
}

napi_value WrapBuffer(napi_env env, BufferReference value);

napi_value SliceBuffer(napi_env env, const CallInfo& call) {
  return WrapBuffer(env, GetCapability<BufferReference>(env, call.self).Slice(
      ReadIndex(env, call.Argument(0)), ReadIndex(env, call.Argument(1))));
}

napi_value WrapBuffer(napi_env env, BufferReference value) {
  return Wrap(env, std::optional(std::move(value)), buffer_tag, {
      Method("byteLength", Invoke<BufferLength>), Method("copyBytes", Invoke<CopyBuffer>),
      Method("readInto", Invoke<ReadBufferInto>),
      Method("slice", Invoke<SliceBuffer>), Method("close", Invoke<CloseCapability<BufferReference>>),
  });
}

napi_value BufferFromBytes(napi_env env, const CallInfo& call) {
  const auto storage = std::make_shared<const Bytes>(ReadBytes(env, call.Argument(0)));
  return WrapBuffer(env, BufferReference(*storage, storage));
}

struct ArkTsPayload {
  PlatformPayload value;
  std::string path;
};

napi_value WrapPayload(napi_env env, PlatformPayload value, std::string path);

PlatformPayload ReadPayload(napi_env env, napi_value value) {
  return PlatformPayload::Decode(Unwrap<ArkTsPayload>(env, value, payload_tag).value.Encode());
}

napi_value SendPayload(napi_env env, PlatformPayload value, std::string path) {
  return WrapPayload(env, PlatformPayload::Decode(value.Encode()), std::move(path));
}

enum class PayloadOperation {
  Kind, IsNull, Null, Boolean, Integer, Double, String, Bytes, Texture, File, Buffer,
  Field, RequireField, Fields, RejectUnknown, Element, Elements,
};

template <PayloadOperation Operation> napi_value AccessPayload(napi_env env, const CallInfo& call) {
  const auto& payload = Unwrap<ArkTsPayload>(env, call.self, payload_tag);
  const auto& value = payload.value;
  try {
    if constexpr (Operation == PayloadOperation::Kind) return Number(env, static_cast<int>(value.Kind()));
    if constexpr (Operation == PayloadOperation::IsNull) return Boolean(env, value.IsNull());
    if constexpr (Operation == PayloadOperation::Null) {
      if (!value.IsNull()) throw std::invalid_argument("HuxerUI PlatformPayload must be Null");
      return Undefined(env);
    }
    if constexpr (Operation == PayloadOperation::Boolean) return Boolean(env, value.AsBoolean());
    if constexpr (Operation == PayloadOperation::Integer) {
      napi_value result;
      Check(env, napi_create_bigint_int64(env, value.AsInteger(), &result));
      return result;
    }
    if constexpr (Operation == PayloadOperation::Double) return Number(env, value.AsDouble());
    if constexpr (Operation == PayloadOperation::String) return String(env, value.AsString());
    if constexpr (Operation == PayloadOperation::Bytes) return ByteArray(env, value.AsBytes());
    if constexpr (Operation == PayloadOperation::Texture) {
      return Wrap(env, std::optional(value.AsExternalTexture()), texture_tag, {
          Method("close", Invoke<CloseCapability<std::shared_ptr<ExternalTexture>>>),
      });
    }
    if constexpr (Operation == PayloadOperation::File) return WrapFile(env, value.AsFileReference());
    if constexpr (Operation == PayloadOperation::Buffer) return WrapBuffer(env, value.AsBufferReference());
    if constexpr (Operation == PayloadOperation::Field || Operation == PayloadOperation::RequireField) {
      const auto name = ReadString(env, call.Argument(0));
      const auto& fields = value.AsObject();
      if (const auto found = fields.find(name); found != fields.end()) {
        return WrapPayload(env, found->second, payload.path + "." + name);
      }
      if constexpr (Operation == PayloadOperation::RequireField) {
        throw std::invalid_argument("HuxerUI missing field " + name);
      }
      return Undefined(env);
    }
    if constexpr (Operation == PayloadOperation::Fields) {
      return Array(env, value.AsObject(), [&](const auto& entry) {
        const auto result = Object(env);
        Check(env, napi_set_named_property(env, result, "name", String(env, entry.first)));
        Check(env, napi_set_named_property(env, result, "value",
            WrapPayload(env, entry.second, payload.path + "." + entry.first)));
        return result;
      });
    }
    if constexpr (Operation == PayloadOperation::RejectUnknown) {
      const auto names = call.Argument(0);
      std::vector<std::string> fields;
      for (std::uint32_t index = 0, size = ArrayLength(env, names); index < size; ++index) {
        fields.push_back(ReadString(env, Element(env, names, index)));
      }
      for (const auto& [name, field] : value.AsObject()) {
        if (std::ranges::find(fields, name) == fields.end()) {
          throw std::invalid_argument("HuxerUI unknown field " + name);
        }
      }
      return Undefined(env);
    }
    if constexpr (Operation == PayloadOperation::Element) {
      const auto index = ReadIndex(env, call.Argument(0));
      const auto& elements = value.AsList();
      if (index >= elements.size()) {
        throw std::invalid_argument("HuxerUI PlatformPayload index out of bounds: " + std::to_string(index));
      }
      return WrapPayload(env, elements[index], payload.path + "[" + std::to_string(index) + "]");
    }
    if constexpr (Operation == PayloadOperation::Elements) {
      std::size_t index = 0;
      return Array(env, value.AsList(), [&](const auto& entry) {
        return WrapPayload(env, entry, payload.path + "[" + std::to_string(index++) + "]");
      });
    }
  } catch (const std::invalid_argument& error) {
    throw std::invalid_argument(std::string(error.what()) + " at " + payload.path);
  }
}

napi_value WrapPayload(napi_env env, PlatformPayload value, std::string path) {
  return Wrap(env, ArkTsPayload{std::move(value), std::move(path)}, payload_tag, {
      Method("kind", Invoke<AccessPayload<PayloadOperation::Kind>>),
      Method("isNull", Invoke<AccessPayload<PayloadOperation::IsNull>>),
      Method("requireNull", Invoke<AccessPayload<PayloadOperation::Null>>),
      Method("requireBoolean", Invoke<AccessPayload<PayloadOperation::Boolean>>),
      Method("requireInt64", Invoke<AccessPayload<PayloadOperation::Integer>>),
      Method("requireDouble", Invoke<AccessPayload<PayloadOperation::Double>>),
      Method("requireString", Invoke<AccessPayload<PayloadOperation::String>>),
      Method("requireBytes", Invoke<AccessPayload<PayloadOperation::Bytes>>),
      Method("requireExternalTexture", Invoke<AccessPayload<PayloadOperation::Texture>>),
      Method("requireFileReference", Invoke<AccessPayload<PayloadOperation::File>>),
      Method("requireBufferReference", Invoke<AccessPayload<PayloadOperation::Buffer>>),
      Method("field", Invoke<AccessPayload<PayloadOperation::Field>>),
      Method("requireField", Invoke<AccessPayload<PayloadOperation::RequireField>>),
      Method("fields", Invoke<AccessPayload<PayloadOperation::Fields>>),
      Method("rejectUnknownFields", Invoke<AccessPayload<PayloadOperation::RejectUnknown>>),
      Method("element", Invoke<AccessPayload<PayloadOperation::Element>>),
      Method("elements", Invoke<AccessPayload<PayloadOperation::Elements>>),
  });
}

template <PayloadOperation Kind> napi_value CreatePayload(napi_env env, const CallInfo& call) {
  PlatformPayload result;
  if constexpr (Kind != PayloadOperation::Null) {
    const auto value = call.Argument(0);
    if constexpr (Kind == PayloadOperation::Boolean) {
      bool flag = false;
      Check(env, napi_get_value_bool(env, value, &flag));
      result = PlatformPayload(flag);
    } else if constexpr (Kind == PayloadOperation::Integer) {
      std::int64_t integer = 0;
      bool lossless = false;
      Check(env, napi_get_value_bigint_int64(env, value, &integer, &lossless));
      if (!lossless) throw std::invalid_argument("HuxerUI ArkTS payload integer exceeds Int64");
      result = PlatformPayload(integer);
    } else if constexpr (Kind == PayloadOperation::Double) {
      double number = 0;
      Check(env, napi_get_value_double(env, value, &number));
      result = PlatformPayload(number);
    } else if constexpr (Kind == PayloadOperation::String) {
      result = PlatformPayload(ReadString(env, value));
    } else if constexpr (Kind == PayloadOperation::Bytes) {
      result = PlatformPayload(ReadBytes(env, value));
    } else if constexpr (Kind == PayloadOperation::Texture) {
      result = PlatformPayload(GetCapability<std::shared_ptr<ExternalTexture>>(env, value));
    } else if constexpr (Kind == PayloadOperation::File) {
      result = PlatformPayload(GetCapability<FileReference>(env, value));
    } else if constexpr (Kind == PayloadOperation::Buffer) {
      result = PlatformPayload(GetCapability<BufferReference>(env, value));
    } else if constexpr (Kind == PayloadOperation::Elements) {
      PlatformPayload::List list;
      const auto size = ArrayLength(env, value);
      list.reserve(size);
      for (std::uint32_t index = 0; index < size; ++index) {
        list.push_back(Unwrap<ArkTsPayload>(env, Element(env, value, index), payload_tag).value);
      }
      result = PlatformPayload(std::move(list));
    } else if constexpr (Kind == PayloadOperation::Fields) {
      PlatformPayload::Object fields;
      for (std::uint32_t index = 0, size = ArrayLength(env, value); index < size; ++index) {
        const auto field = Element(env, value, index);
        const auto name = ReadString(env, Property(env, field, "name"));
        if (!fields.emplace(name, Unwrap<ArkTsPayload>(env, Property(env, field, "value"), payload_tag).value).second) {
          throw std::invalid_argument("HuxerUI duplicate ArkTS payload field " + name);
        }
      }
      result = PlatformPayload(std::move(fields));
    }
  }
  return WrapPayload(env, std::move(result), "value");
}

napi_value PayloadFactory(napi_env env, napi_value files) {
  const auto value = Object(env);
  const napi_property_descriptor methods[]{
      {"files", nullptr, nullptr, nullptr, nullptr, files, napi_default, nullptr},
      Method("nullValue", Invoke<CreatePayload<PayloadOperation::Null>>),
      Method("booleanValue", Invoke<CreatePayload<PayloadOperation::Boolean>>),
      Method("int64", Invoke<CreatePayload<PayloadOperation::Integer>>),
      Method("doubleValue", Invoke<CreatePayload<PayloadOperation::Double>>),
      Method("string", Invoke<CreatePayload<PayloadOperation::String>>),
      Method("bytes", Invoke<CreatePayload<PayloadOperation::Bytes>>),
      Method("list", Invoke<CreatePayload<PayloadOperation::Elements>>),
      Method("object", Invoke<CreatePayload<PayloadOperation::Fields>>),
      Method("externalTexture", Invoke<CreatePayload<PayloadOperation::Texture>>),
      Method("fileReference", Invoke<CreatePayload<PayloadOperation::File>>),
      Method("fileFromUri", Invoke<FileFromUri>),
      Method("bufferReference", Invoke<CreatePayload<PayloadOperation::Buffer>>),
      Method("bufferFromBytes", Invoke<BufferFromBytes>),
  };
  Check(env, napi_define_properties(env, value, std::size(methods), methods));
  return value;
}

using Completion = std::function<void(PlatformResult<PlatformPayload>)>;

class PendingRequest final {
public:
  PendingRequest(napi_env env, Completion complete) : env_(env), complete_(std::move(complete)) {}
  ~PendingRequest() { Release(env_, cancel_); }

  bool Pending() const noexcept { return static_cast<bool>(complete_); }

  void SetCancellation(napi_value value) {
    const auto type = Type(env_, value);
    if (type == napi_function && Pending()) cancel_ = Retain(env_, value);
    else if (type != napi_function && type != napi_undefined) {
      throw std::invalid_argument("HuxerUI ArkTS invocation must return a cancellation function or undefined");
    }
  }

  void Complete(PlatformResult<PlatformPayload> result) {
    auto complete = std::exchange(complete_, {});
    Release(env_, cancel_);
    if (complete) complete(std::move(result));
  }

  void Cancel(bool notify_host = true) noexcept {
    complete_ = {};
    if (cancel_ && notify_host) {
      try {
        HandleScope scope(env_);
        const auto callback = Get(env_, cancel_);
        Release(env_, cancel_);
        napi_value ignored;
        Check(env_, napi_call_function(env_, Undefined(env_), callback, 0, nullptr, &ignored));
      } catch (...) {
      }
    }
    Release(env_, cancel_);
  }

private:
  napi_env env_;
  Completion complete_;
  napi_ref cancel_ = nullptr;
};

constexpr napi_type_tag result_tag{0x48555845524F484FULL, 5};
constexpr napi_type_tag events_tag{0x48555845524F484FULL, 6};

template <bool Failed> napi_value CompleteRequest(napi_env env, const CallInfo& call) {
  const auto request = Unwrap<std::weak_ptr<PendingRequest>>(env, call.self, result_tag).lock();
  if (!request || !request->Pending()) return Undefined(env);
  try {
    if constexpr (Failed) {
      const auto code = ReadString(env, call.Argument(0));
      const auto message = ReadString(env, call.Argument(1));
      auto details = Type(env, call.args[2]) == napi_undefined ? PlatformPayload{} : ReadPayload(env, call.args[2]);
      request->Complete(PlatformError{code, message, std::move(details)});
    } else {
      request->Complete(ReadPayload(env, call.Argument(0)));
    }
  } catch (const std::exception& error) {
    request->Complete(PlatformError{"huxerui/invalid-result", error.what(), {}});
  }
  return Undefined(env);
}

struct InstanceEvents {
  PlatformEventEmitter emitter;
  bool committed = false;
  bool closed = false;
  std::vector<std::pair<std::string, PlatformPayload>> pending;

  void Commit() {
    if (closed || committed) return;
    committed = true;
    auto initial = std::exchange(pending, {});
    for (auto& [name, payload] : initial) {
      if (closed) break;
      auto emit = emitter;
      emit.Emit(std::move(name), std::move(payload));
    }
  }

  void Close() noexcept {
    closed = true;
    pending.clear();
    emitter = {};
  }
};

napi_value EmitArkTsEvent(napi_env env, const CallInfo& call) {
  const auto state = Unwrap<std::weak_ptr<InstanceEvents>>(env, call.self, events_tag).lock();
  if (!state || state->closed) return Undefined(env);
  auto name = ReadString(env, call.Argument(0));
  auto payload = ReadPayload(env, call.Argument(1));
  if (!state->committed) {
    // A candidate view has no committed event target yet. Failed mounts discard this queue.
    state->pending.emplace_back(std::move(name), std::move(payload));
    return Undefined(env);
  }
  auto emit = state->emitter;
  auto reply = emit.Emit(std::move(name), std::move(payload));
  return reply ? SendPayload(env, std::move(*reply), "event result") : Undefined(env);
}

napi_value ResolveFactory(napi_env env, ArkTsPlatformHost& host, const char* kind, const std::string& name) {
  if (name.empty() || !IsValidUtf8(name)) throw std::invalid_argument("HuxerUI ArkTS factory name must be valid UTF-8");
  const auto extensions = host.Extensions();
  if (Type(env, extensions) != napi_object) throw std::logic_error("HuxerUI has no ArkTS platform extensions");
  const std::array args{String(env, name)};
  const auto value = Call(env, extensions, kind, args);
  if (Type(env, value) != napi_object || Type(env, Property(env, value, "create")) != napi_function) {
    throw std::logic_error("HuxerUI ArkTS factory could not be resolved: " + name);
  }
  return value;
}

void Report(const std::exception& error) noexcept {
  OH_LOG_Print(LOG_APP, LOG_ERROR, 0xCAFE, "HuxerUI", "%{public}s", error.what());
}

} // namespace

class ArkTsPlatformInstanceBridge final {
public:
  ArkTsPlatformInstanceBridge(napi_env env, const std::shared_ptr<InstanceEvents>& events)
      : env_(env), events_(events) {}
  ~ArkTsPlatformInstanceBridge() { Dispose(); }

  void Create(napi_value factory, std::span<const napi_value> host, napi_value files, PlatformPayload options,
              bool view, bool update, bool invoke) {
    std::vector<napi_value> args(host.begin(), host.end());
    args.push_back(SendPayload(env_, std::move(options), view ? "properties" : "options"));
    args.push_back(Wrap(env_, std::weak_ptr(events_), events_tag, {Method("emit", Invoke<EmitArkTsEvent>)}));
    args.push_back(PayloadFactory(env_, files));
    const auto instance = Call(env_, factory, "create", args);
    if (Type(env_, instance) != napi_object) {
      throw std::logic_error("HuxerUI ArkTS factory returned an invalid instance");
    }
    instance_ = Retain(env_, instance);
    if (Type(env_, Property(env_, instance, "dispose")) != napi_function ||
        (update && Type(env_, Property(env_, instance, "update")) != napi_function) ||
        (invoke && Type(env_, Property(env_, instance, "invoke")) != napi_function)) {
      throw std::logic_error("HuxerUI ArkTS platform instance is missing required methods");
    }
  }

  void AttachView(ArkUI_NativeNodeAPI_1& api) {
    const auto content = Property(env_, Get(env_, instance_), "content");
    api_ = &api;
    if (Type(env_, content) != napi_object ||
        OH_ArkUI_GetNodeHandleFromNapiValue(env_, content, &child_) != ARKUI_ERROR_CODE_NO_ERROR || !child_) {
      throw std::logic_error("HuxerUI ArkTS view did not return detached ComponentContent");
    }
    view_ = api.createNode(ARKUI_NODE_STACK);
    if (!view_) throw std::runtime_error("HuxerUI could not create the ArkTS view container");
    const ArkUI_NumberValue alignment{.i32 = ARKUI_ALIGNMENT_TOP_START};
    const ArkUI_AttributeItem align_item{.value = &alignment, .size = 1, .string = nullptr, .object = nullptr};
    if (api.setAttribute(view_, NODE_STACK_ALIGN_CONTENT, &align_item) != ARKUI_ERROR_CODE_NO_ERROR ||
        api.addChild(view_, child_) != ARKUI_ERROR_CODE_NO_ERROR) {
      throw std::runtime_error("HuxerUI could not attach ArkTS ComponentContent");
    }
    child_attached_ = true;
  }

  ArkUI_NodeHandle View() const noexcept { return view_; }

  void Update(PlatformPayload properties) {
    HandleScope scope(env_);
    const std::array args{SendPayload(env_, std::move(properties), "properties")};
    Call(env_, Get(env_, instance_), "update", args);
  }

  void SetViewChannel(PlatformChannel channel) { view_channel_ = std::move(channel); }
  PlatformChannel ViewChannel() const { return view_channel_; }
  void Commit() { events_->Commit(); }

  std::function<void()> CallMethod(std::string method, PlatformPayload args, Completion complete) {
    if (disposed_) throw std::logic_error("HuxerUI ArkTS instance has closed");
    HandleScope scope(env_);
    auto request = std::make_shared<PendingRequest>(env_, std::move(complete));
    const auto reply = Wrap(env_, std::weak_ptr(request), result_tag, {
        Method("complete", Invoke<CompleteRequest<false>>), Method("fail", Invoke<CompleteRequest<true>>),
    });
    const std::array arguments{String(env_, method), SendPayload(env_, std::move(args), "arguments"), reply};
    std::erase_if(requests_, [](const auto& weak) { return weak.expired(); });
    requests_.push_back(request);
    try {
      request->SetCancellation(Call(env_, Get(env_, instance_), "invoke", arguments));
    } catch (...) {
      request->Cancel();
      throw;
    }
    return [request] { request->Cancel(); };
  }

  void DisableCallbacks() noexcept { callbacks_enabled_ = false; }

  void Dispose() noexcept {
    if (std::exchange(disposed_, true)) return;
    events_->Close();
    // Cancel app callbacks before releasing the ArkTS instance, including requests whose results are still pending.
    auto requests = std::exchange(requests_, {});
    for (const auto& weak : requests) {
      if (auto request = weak.lock()) request->Cancel(callbacks_enabled_);
    }
    view_channel_.Close();
    if (view_) {
      if (child_attached_) api_->removeChild(view_, child_);
      api_->disposeNode(std::exchange(view_, nullptr));
    }
    // The Node-API conversion owns a separate reference to the ComponentContent root.
    if (child_) api_->disposeNode(std::exchange(child_, nullptr));
    if (instance_ && callbacks_enabled_) {
      try {
        HandleScope scope(env_);
        Call(env_, Get(env_, instance_), "dispose");
      } catch (const std::exception& error) {
        Report(error);
      }
    }
    Release(env_, instance_);
  }

private:
  napi_env env_;
  std::shared_ptr<InstanceEvents> events_;
  napi_ref instance_ = nullptr;
  ArkUI_NativeNodeAPI_1* api_ = nullptr;
  ArkUI_NodeHandle view_ = nullptr;
  ArkUI_NodeHandle child_ = nullptr;
  bool child_attached_ = false;
  bool disposed_ = false;
  bool callbacks_enabled_ = true;
  PlatformChannel view_channel_;
  std::vector<std::weak_ptr<PendingRequest>> requests_;
};

ArkTsPlatformHost::ArkTsPlatformHost(napi_env env, napi_value context, napi_value files, napi_value extensions,
                                     napi_value window)
    : env_(env) {
  try {
    context_ = Retain(env_, context);
    files_ = Retain(env_, files);
    extensions_ = Retain(env_, extensions);
    window_ = Retain(env_, window);
  } catch (...) {
    Close();
    throw;
  }
}

ArkTsPlatformHost::~ArkTsPlatformHost() { Close(); }

napi_value ArkTsPlatformHost::Context() const { return Get(env_, context_); }
napi_value ArkTsPlatformHost::Files() const { return Get(env_, files_); }
napi_value ArkTsPlatformHost::Extensions() const { return Get(env_, extensions_); }
napi_value ArkTsPlatformHost::Window() const { return Get(env_, window_); }

void ArkTsPlatformHost::TrackInstance(const std::shared_ptr<ArkTsPlatformInstanceBridge>& instance) {
  std::erase_if(instances_, [](const auto& weak) { return weak.expired(); });
  instances_.push_back(instance);
}

void ArkTsPlatformHost::DisableCallbacks() noexcept {
  for (const auto& weak : instances_) {
    if (const auto instance = weak.lock()) instance->DisableCallbacks();
  }
}

void ArkTsPlatformHost::Close() noexcept {
  auto instances = std::exchange(instances_, {});
  for (const auto& weak : instances) {
    if (auto instance = weak.lock()) instance->Dispose();
  }
  Release(env_, window_);
  Release(env_, extensions_);
  Release(env_, files_);
  Release(env_, context_);
}

namespace {

PlatformChannel OpenModule(ArkTsPlatformHost& owner, napi_value factory, const PlatformChannelEndpoint& endpoint,
                           std::span<const napi_value> host, PlatformPayload options) {
  auto events = std::make_shared<InstanceEvents>(InstanceEvents{endpoint.Events(), true, false, {}});
  auto instance = std::make_shared<ArkTsPlatformInstanceBridge>(owner.Environment(), events);
  instance->Create(factory, host, owner.Files(), std::move(options), false, false, true);
  owner.TrackInstance(instance);
  endpoint.Connect({
      .invoke = [instance](std::string method, PlatformPayload args, Completion result) {
        return instance->CallMethod(std::move(method), std::move(args), std::move(result));
      },
      .dispose = [instance] { instance->Dispose(); },
  });
  return endpoint.Channel();
}

} // namespace
} // namespace huxerui::detail

namespace huxerui::ohos::detail {

using namespace huxerui::detail;

PlatformChannel OpenArkTsModule(Runtime& runtime, const std::string& name, PlatformPayload options) {
  auto& host = GetArkTsPlatformHost(runtime);
  const auto env = host.Environment();
  HandleScope scope(env);
  const auto factory = ResolveFactory(env, host, "module", name);
  const std::array args{host.Context()};
  return OpenModule(host, factory, MakePlatformChannelEndpoint(runtime), args, std::move(options));
}

PlatformChannel OpenArkTsModule(UiWindow& window, const std::string& name, PlatformPayload options) {
  auto& host = GetArkTsPlatformHost(window);
  const auto env = host.Environment();
  HandleScope scope(env);
  const auto factory = ResolveFactory(env, GetApplicationArkTsPlatformHost(window), "windowModule", name);
  const std::array args{host.Window(), host.Context()};
  return OpenModule(host, factory, MakePlatformChannelEndpoint(window), args, std::move(options));
}

huxerui::detail::PlatformViewFactoryRegistration PrepareArkTsView(UiWindow& window, const std::string& name,
    std::function<PlatformPayload(const PlatformValue&)> encode,
    std::function<void(const PlatformValue&, PlatformChannel)> connect,
    std::function<void(const PlatformValue&)> disconnect) {
  auto factory = std::make_shared<ArkUiViewFactory>();
  const bool has_channel = static_cast<bool>(connect);
  factory->create = [&window, name, encode, has_channel](ArkUI_NativeNodeAPI_1& api, const PlatformValue& properties,
                                                       PlatformEventEmitter emitter) {
    auto& host = GetArkTsPlatformHost(window);
    const auto env = host.Environment();
    HandleScope scope(env);
    const auto object = ResolveFactory(env, GetApplicationArkTsPlatformHost(window), "view", name);
    auto events = std::make_shared<InstanceEvents>(InstanceEvents{std::move(emitter), false, false, {}});
    auto instance = std::make_shared<ArkTsPlatformInstanceBridge>(env, events);
    const std::array args{host.Context()};
    instance->Create(object, args, host.Files(), encode ? encode(properties) : PlatformPayload{},
                     true, static_cast<bool>(encode), has_channel);
    instance->AttachView(api);
    host.TrackInstance(instance);
    if (has_channel) {
      auto endpoint = MakePlatformChannelEndpoint(window);
      endpoint.Connect({
          .invoke = [weak = std::weak_ptr(instance)](std::string method, PlatformPayload args, Completion result) {
            const auto instance = weak.lock();
            if (!instance) throw std::logic_error("HuxerUI ArkTS view is closed");
            return instance->CallMethod(std::move(method), std::move(args), std::move(result));
          },
          .dispose = {},
      });
      instance->SetViewChannel(endpoint.Channel());
    }
    return std::static_pointer_cast<void>(instance);
  };
  factory->view = [](const std::shared_ptr<void>& instance) {
    return std::static_pointer_cast<ArkTsPlatformInstanceBridge>(instance)->View();
  };
  if (encode) {
    factory->update = [encode = std::move(encode)](const std::shared_ptr<void>& instance, const PlatformValue& value) {
      std::static_pointer_cast<ArkTsPlatformInstanceBridge>(instance)->Update(encode(value));
    };
  }
  factory->dispose = [](const std::shared_ptr<void>& instance) {
    std::static_pointer_cast<ArkTsPlatformInstanceBridge>(instance)->Dispose();
  };
  factory->commit = [](const std::shared_ptr<void>& instance) {
    std::static_pointer_cast<ArkTsPlatformInstanceBridge>(instance)->Commit();
  };
  if (has_channel) {
    factory->connect = [connect = std::move(connect)](const std::shared_ptr<void>& instance,
                                                       const PlatformValue& value) {
      connect(value, std::static_pointer_cast<ArkTsPlatformInstanceBridge>(instance)->ViewChannel());
    };
    factory->disconnect = [disconnect = std::move(disconnect)](const std::shared_ptr<void>&,
                                                                const PlatformValue& value) {
      disconnect(value);
    };
  }
  return MakePlatformViewFactoryRegistration(std::move(factory));
}

} // namespace huxerui::ohos::detail
