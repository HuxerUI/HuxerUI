#include "timer.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <huxerui/android/jni.h>
#include <huxerui/android/platform_module.h>

namespace {

constexpr char native_timer_class[] = "org/huxerui/demo/NativeTimer";

huxerui::PlatformError TimerError(std::string code, std::string message) {
  return {
      std::move(code),
      std::move(message),
      {},
  };
}

bool ClearJavaException(JNIEnv* environment) {
  if (!environment->ExceptionCheck()) {
    return false;
  }
  environment->ExceptionClear();
  return true;
}

struct PendingStart {
  std::uint64_t generation = 0;
  huxerui::PlatformResultSink result;
};

struct AndroidTimerState : std::enable_shared_from_this<AndroidTimerState> {
  explicit AndroidTimerState(JNIEnv* environment_value, huxerui::PlatformEventSink event_sink)
      : environment(environment_value), events(std::move(event_sink)) {}

  void Tick(std::uint64_t timer_generation) {
    huxerui::PlatformResultSink first_result;
    huxerui::PlatformEventSink event_sink;
    std::uint64_t next_tick = 0;
    {
      std::lock_guard lock(mutex);
      if (closed || generation != timer_generation) {
        return;
      }
      next_tick = ++tick;
      if (pending_start && pending_start->generation == timer_generation) {
        first_result = std::move(pending_start->result);
        pending_start.reset();
      }
      event_sink = events;
    }
    if (first_result) {
      first_result(huxerui::PlatformPayload(next_tick));
    }
    if (event_sink) {
      event_sink(huxerui::example::timer::tick_event, huxerui::PlatformPayload(next_tick));
    }
  }

  std::function<void()> Start(std::int64_t milliseconds, huxerui::PlatformResultSink result) {
    if (milliseconds <= 0) {
      result(TimerError("example/invalid-interval", "The timer interval must be greater than zero"));
      return {};
    }

    huxerui::PlatformResultSink replaced_result;
    std::uint64_t timer_generation = 0;
    {
      std::lock_guard lock(mutex);
      if (closed) {
        result(TimerError("example/timer-closed", "The native timer is closed"));
        return {};
      }
      if (generation == static_cast<std::uint64_t>(std::numeric_limits<jlong>::max())) {
        result(TimerError("example/timer-exhausted", "The native timer generation space is exhausted"));
        return {};
      }
      if (pending_start) {
        replaced_result = std::move(pending_start->result);
        pending_start.reset();
      }
      timer_generation = ++generation;
      tick = 0;
      pending_start = PendingStart{timer_generation, std::move(result)};
    }
    if (replaced_result) {
      replaced_result(TimerError("example/timer-replaced", "The timer was replaced by a newer start call"));
    }

    environment->CallVoidMethod(timer, start, static_cast<jlong>(milliseconds), static_cast<jlong>(timer_generation));
    if (ClearJavaException(environment)) {
      huxerui::PlatformResultSink failed_result;
      {
        std::lock_guard lock(mutex);
        if (pending_start && pending_start->generation == timer_generation) {
          failed_result = std::move(pending_start->result);
          pending_start.reset();
        }
        if (generation == timer_generation) {
          ++generation;
        }
      }
      if (failed_result) {
        failed_result(TimerError("example/timer-java", "The Android timer could not be started"));
      }
      return {};
    }

    const std::weak_ptr<AndroidTimerState> weak_state = weak_from_this();
    return [weak_state, timer_generation] {
      if (const std::shared_ptr<AndroidTimerState> state = weak_state.lock()) {
        state->CancelStart(timer_generation);
      }
    };
  }

  void CancelStart(std::uint64_t timer_generation) {
    {
      std::lock_guard lock(mutex);
      if (closed || generation != timer_generation) {
        return;
      }
      pending_start.reset();
      ++generation;
    }
    environment->CallVoidMethod(timer, cancel, static_cast<jlong>(timer_generation));
    static_cast<void>(ClearJavaException(environment));
  }

  void Stop(huxerui::PlatformResultSink result) {
    huxerui::PlatformResultSink pending_result;
    {
      std::lock_guard lock(mutex);
      if (closed) {
        result(TimerError("example/timer-closed", "The native timer is closed"));
        return;
      }
      if (pending_start) {
        pending_result = std::move(pending_start->result);
        pending_start.reset();
      }
      ++generation;
    }
    if (pending_result) {
      pending_result(TimerError("example/timer-stopped", "The timer stopped before its first tick"));
    }

    environment->CallVoidMethod(timer, stop);
    if (ClearJavaException(environment)) {
      result(TimerError("example/timer-java", "The Android timer could not be stopped"));
      return;
    }
    result(huxerui::PlatformPayload());
  }

  void Dispose() noexcept {
    {
      std::lock_guard lock(mutex);
      if (closed) {
        return;
      }
      closed = true;
      ++generation;
      pending_start.reset();
      events = {};
    }
    if (timer != nullptr) {
      environment->CallVoidMethod(timer, dispose_bridge);
      static_cast<void>(ClearJavaException(environment));
      environment->DeleteGlobalRef(timer);
      timer = nullptr;
    }
    delete bridge;
    bridge = nullptr;
  }

  JNIEnv* environment = nullptr;
  jobject timer = nullptr;
  jmethodID start = nullptr;
  jmethodID cancel = nullptr;
  jmethodID stop = nullptr;
  jmethodID dispose_bridge = nullptr;
  std::weak_ptr<AndroidTimerState>* bridge = nullptr;
  std::mutex mutex;
  huxerui::PlatformEventSink events;
  std::optional<PendingStart> pending_start;
  std::uint64_t generation = 0;
  std::uint64_t tick = 0;
  bool closed = false;
};

huxerui::PlatformModuleFactory::Instance CreateAndroidTimer(
    JNIEnv* environment, jobject context, const huxerui::PlatformPayload& options, huxerui::PlatformEventSink events
) {
  static_cast<void>(options);
  huxerui::android::LocalRef<jclass> timer_class(environment, environment->FindClass(native_timer_class));
  if (!timer_class) {
    ClearJavaException(environment);
    throw std::logic_error("HuxerUI example could not find the Android native timer class");
  }
  const jmethodID constructor = environment->GetMethodID(timer_class.Get(), "<init>", "(Landroid/content/Context;J)V");
  const jmethodID start = environment->GetMethodID(timer_class.Get(), "start", "(JJ)V");
  const jmethodID cancel = environment->GetMethodID(timer_class.Get(), "cancel", "(J)V");
  const jmethodID stop = environment->GetMethodID(timer_class.Get(), "stop", "()V");
  const jmethodID dispose_bridge = environment->GetMethodID(timer_class.Get(), "disposeNativeBridge", "()V");
  if (constructor == nullptr || start == nullptr || cancel == nullptr || stop == nullptr || dispose_bridge == nullptr) {
    ClearJavaException(environment);
    throw std::logic_error("HuxerUI example Android timer methods do not match the native bridge");
  }

  auto state = std::make_shared<AndroidTimerState>(environment, std::move(events));
  auto bridge = std::make_unique<std::weak_ptr<AndroidTimerState>>(state);
  huxerui::android::LocalRef<jobject> local_timer(
      environment,
      environment->NewObject(
          timer_class.Get(),
          constructor,
          context,
          static_cast<jlong>(reinterpret_cast<std::uintptr_t>(bridge.get()))
      )
  );
  if (!local_timer || environment->ExceptionCheck()) {
    ClearJavaException(environment);
    throw std::logic_error("HuxerUI example could not create the Android native timer");
  }
  state->timer = environment->NewGlobalRef(local_timer.Get());
  if (state->timer == nullptr) {
    ClearJavaException(environment);
    throw std::runtime_error("HuxerUI example could not retain the Android native timer");
  }
  state->start = start;
  state->cancel = cancel;
  state->stop = stop;
  state->dispose_bridge = dispose_bridge;
  state->bridge = bridge.release();

  huxerui::PlatformModuleFactory::Instance instance;
  instance.call = [state](std::string method, huxerui::PlatformPayload arguments, huxerui::PlatformResultSink result)
      -> std::function<void()> {
    if (method == huxerui::example::timer::start_method) {
      std::int64_t milliseconds = 0;
      try {
        milliseconds = arguments.AsInteger();
      } catch (...) {
        result(TimerError("example/invalid-interval", "The timer interval payload is invalid"));
        return {};
      }
      return state->Start(milliseconds, std::move(result));
    }
    if (method == huxerui::example::timer::stop_method) {
      state->Stop(std::move(result));
      return {};
    }
    result(TimerError("example/unknown-method", "The native timer method is not supported"));
    return {};
  };
  instance.dispose = [state] { state->Dispose(); };
  return instance;
}

huxerui::android::PlatformModuleFactory AndroidTimerFactory() {
  return {.create = CreateAndroidTimer};
}

} // namespace

namespace huxerui::example {

void InstallTimer(RootContext& root) {
  root.Modules().Register(timer::type, AndroidTimerFactory());
  root.Provide(std::make_shared<TimerService>(root.Modules().Open(timer::type)));
}

} // namespace huxerui::example

extern "C" JNIEXPORT void JNICALL
Java_org_huxerui_demo_NativeTimer_nativeTick(JNIEnv*, jclass, jlong bridge, jlong generation) {
  auto* state = reinterpret_cast<std::weak_ptr<AndroidTimerState>*>(static_cast<std::uintptr_t>(bridge));
  if (state != nullptr && generation > 0) {
    try {
      if (const std::shared_ptr<AndroidTimerState> locked_state = state->lock()) {
        locked_state->Tick(static_cast<std::uint64_t>(generation));
      }
    } catch (...) {
    }
  }
}
