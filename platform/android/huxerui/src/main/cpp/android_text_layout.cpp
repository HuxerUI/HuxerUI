#include "android_text_layout.h"

#include <stdexcept>
#include <vector>

#include "text_internal.h"

namespace huxerui::detail {

class AndroidTextLayout final : public TextLayout {
public:
  AndroidTextLayout(JavaVM* virtual_machine, JNIEnv* environment, jobject layout)
      : virtual_machine_(virtual_machine), layout_(environment->NewGlobalRef(layout)) {
    if (layout_ == nullptr) {
      throw std::runtime_error("HuxerUI could not retain its Android text layout");
    }
    jclass layout_class = environment->GetObjectClass(layout);
    measure_ = environment->GetMethodID(layout_class, "measure", "()[F");
    hit_test_ = environment->GetMethodID(layout_class, "hitTest", "(FF)J");
    caret_ = environment->GetMethodID(layout_class, "caret", "(JZ)[F");
    range_ = environment->GetMethodID(layout_class, "range", "(JJ)[F");
    previous_ = environment->GetMethodID(layout_class, "previous", "(J)J");
    next_ = environment->GetMethodID(layout_class, "next", "(J)J");
    environment->DeleteLocalRef(layout_class);
    if (measure_ == nullptr || hit_test_ == nullptr || caret_ == nullptr || range_ == nullptr || previous_ == nullptr ||
        next_ == nullptr) {
      environment->DeleteGlobalRef(layout_);
      layout_ = nullptr;
      throw std::runtime_error("HuxerUI Android text layout methods do not match the platform backend");
    }
  }

  ~AndroidTextLayout() override {
    if (JNIEnv* environment = Environment(); environment != nullptr && layout_ != nullptr) {
      environment->DeleteGlobalRef(layout_);
    }
  }

  Size Measure() const override {
    const std::vector<float> values = FloatArray(measure_);
    return values.size() >= 2 ? Size{values[0], values[1]} : Size{};
  }

  TextPosition HitTest(Point point) const override {
    JNIEnv* environment = Environment();
    if (environment == nullptr) {
      return {};
    }
    const jlong encoded = environment->CallLongMethod(layout_, hit_test_, point.x, point.y);
    // Java packs affinity into the sign: -(offset + 1) keeps upstream offset zero distinct from downstream zero.
    const bool upstream = encoded < 0;
    return {
        static_cast<TextOffset>(upstream ? -encoded - 1 : encoded),
        upstream ? TextAffinity::Upstream : TextAffinity::Downstream,
    };
  }

  Rect CaretRect(TextOffset offset, TextAffinity affinity) const override {
    const std::vector<float> values =
        FloatArray(caret_, static_cast<jlong>(offset), affinity == TextAffinity::Upstream ? JNI_TRUE : JNI_FALSE);
    return values.size() >= 4 ? Rect{values[0], values[1], values[2], values[3]} : Rect{};
  }

  std::vector<Rect> RangeRects(TextRange range) const override {
    const std::vector<float> values =
        FloatArray(range_, static_cast<jlong>(range.start), static_cast<jlong>(range.end));
    std::vector<Rect> rects;
    rects.reserve(values.size() / 4);
    for (std::size_t index = 0; index + 3 < values.size(); index += 4) {
      rects.push_back({
          values[index],
          values[index + 1],
          values[index + 2],
          values[index + 3],
      });
    }
    return rects;
  }

  TextOffset PreviousCaretOffset(TextOffset offset) const override {
    JNIEnv* environment = Environment();
    return environment == nullptr
               ? offset
               : static_cast<TextOffset>(environment->CallLongMethod(layout_, previous_, static_cast<jlong>(offset)));
  }

  TextOffset NextCaretOffset(TextOffset offset) const override {
    JNIEnv* environment = Environment();
    return environment == nullptr
               ? offset
               : static_cast<TextOffset>(environment->CallLongMethod(layout_, next_, static_cast<jlong>(offset)));
  }

private:
  JNIEnv* Environment() const noexcept {
    if (virtual_machine_ == nullptr) {
      return nullptr;
    }
    JNIEnv* environment = nullptr;
    return virtual_machine_->GetEnv(reinterpret_cast<void**>(&environment), JNI_VERSION_1_6) == JNI_OK ? environment
                                                                                                       : nullptr;
  }

  template <class... Arguments> std::vector<float> FloatArray(jmethodID method, Arguments... arguments) const {
    JNIEnv* environment = Environment();
    if (environment == nullptr) {
      return {};
    }
    auto* result = static_cast<jfloatArray>(environment->CallObjectMethod(layout_, method, arguments...));
    if (result == nullptr) {
      return {};
    }
    const jsize size = environment->GetArrayLength(result);
    std::vector<float> values(static_cast<std::size_t>(size));
    if (size > 0) {
      environment->GetFloatArrayRegion(result, 0, size, values.data());
    }
    environment->DeleteLocalRef(result);
    return values;
  }

  JavaVM* virtual_machine_ = nullptr;
  jobject layout_ = nullptr;
  jmethodID measure_ = nullptr;
  jmethodID hit_test_ = nullptr;
  jmethodID caret_ = nullptr;
  jmethodID range_ = nullptr;
  jmethodID previous_ = nullptr;
  jmethodID next_ = nullptr;
};

std::unique_ptr<TextLayout> CreateAndroidTextLayout(JavaVM* virtual_machine, JNIEnv* environment, jobject layout) {
  return std::make_unique<AndroidTextLayout>(virtual_machine, environment, layout);
}

} // namespace huxerui::detail
