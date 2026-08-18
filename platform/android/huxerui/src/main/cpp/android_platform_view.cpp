#include "android_platform_view.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/android/platform_view.h>

#include "android_renderer.h"
#include "internal.h"

namespace huxerui::detail {

namespace {

struct EventRoute {
  Runtime* runtime = nullptr;
  std::uint64_t identity = 0;
  bool active = false;
};

struct HostedPlatformView {
  std::uint64_t properties_revision = 0;
  std::string type;
  std::shared_ptr<EventRoute> event_route;
  android::PlatformViewFactory factory;
  jobject view = nullptr;
  bool mounted = false;
};

Rect VisibleBounds(const PlatformViewPlacement& placement) {
  return placement.clip.has_value() ? placement.world_bounds.Intersection(*placement.clip) : placement.world_bounds;
}

void ClearJavaException(JNIEnv* environment) {
  if (environment->ExceptionCheck()) {
    environment->ExceptionClear();
  }
}

} // namespace

struct AndroidPlatformViews::State {
  State(
      JNIEnv* environment,
      jobject root_value,
      jobject context_value,
      AndroidRenderer& renderer_value,
      PlatformModules& modules_value,
      Runtime& runtime_value,
      UIThreadDispatcher dispatcher
  )
      : context(context_value), renderer(&renderer_value), modules(&modules_value), runtime(&runtime_value),
        dispatch_to_ui_thread(std::move(dispatcher)) {
    if (environment->GetJavaVM(&virtual_machine) != JNI_OK) {
      throw std::runtime_error("HuxerUI could not access the Android Java VM for PlatformView hosting");
    }
    root = environment->NewGlobalRef(root_value);
    if (root == nullptr) {
      throw std::runtime_error("HuxerUI could not retain the Android PlatformView host");
    }
    jclass root_class = environment->GetObjectClass(root_value);
    if (root_class == nullptr) {
      environment->DeleteGlobalRef(root);
      root = nullptr;
      throw std::runtime_error("HuxerUI could not inspect the Android PlatformView host");
    }
    validate_platform_view = environment->GetMethodID(root_class, "validatePlatformView", "(Ljava/lang/Object;)I");
    mount_platform_view = environment->GetMethodID(root_class, "mountPlatformView", "(JLjava/lang/Object;)I");
    place_platform_view = environment->GetMethodID(root_class, "placePlatformView", "(JFFFFFFFFZ)V");
    remove_platform_view = environment->GetMethodID(root_class, "removePlatformView", "(J)V");
    commit_composition = environment->GetMethodID(root_class, "commitPlatformComposition", "([J)V");
    apply_platform_view_focus = environment->GetMethodID(root_class, "applyPlatformViewFocus", "(J)Z");
    environment->DeleteLocalRef(root_class);
    if (validate_platform_view == nullptr || mount_platform_view == nullptr || place_platform_view == nullptr ||
        remove_platform_view == nullptr || commit_composition == nullptr || apply_platform_view_focus == nullptr) {
      ClearJavaException(environment);
      environment->DeleteGlobalRef(root);
      root = nullptr;
      throw std::runtime_error("HuxerUI Android PlatformView host methods do not match the platform backend");
    }
  }

  JNIEnv* Environment() const {
    if (virtual_machine == nullptr) {
      return nullptr;
    }
    JNIEnv* environment = nullptr;
    const jint result = virtual_machine->GetEnv(reinterpret_cast<void**>(&environment), JNI_VERSION_1_6);
    if (result == JNI_OK) {
      return environment;
    }
    return nullptr;
  }

  std::unique_ptr<HostedPlatformView> Create(JNIEnv* environment, const PlatformViewPlacement& placement) {
    const PlacePlatformViewCommand& command = *placement.command;
    const auto* factory = modules->Find<android::PlatformViewFactory>(command.Type());
    if (factory == nullptr) {
      throw std::logic_error("HuxerUI Android PlatformView type is not registered: " + std::string(command.Type()));
    }
    if (!factory->create) {
      throw std::logic_error("HuxerUI Android PlatformView factory must provide create");
    }

    auto route = std::make_shared<EventRoute>(EventRoute{runtime, command.Identity(), false});
    const std::weak_ptr<EventRoute> weak_route = route;
    const UIThreadDispatcher dispatcher = dispatch_to_ui_thread;
    PlatformEventSink event_sink = [weak_route, dispatcher](std::string name, PlatformPayload payload) mutable {
      if (!dispatcher) {
        return;
      }
      dispatcher([weak_route, name = std::move(name), payload = std::move(payload)]() mutable {
        const std::shared_ptr<EventRoute> route = weak_route.lock();
        if (!route || !route->active || route->runtime == nullptr) {
          return;
        }
        static_cast<void>(RuntimeAccess::DispatchPlatformViewEvent(*route->runtime, route->identity, name, payload));
      });
    };

    jobject local_view = nullptr;
    local_view = factory->create(environment, context, command.Properties(), std::move(event_sink));
    if (environment->ExceptionCheck()) {
      ClearJavaException(environment);
      if (local_view != nullptr && factory->dispose) {
        try {
          factory->dispose(environment, local_view);
        } catch (...) {
        }
        ClearJavaException(environment);
      }
      if (local_view != nullptr) {
        environment->DeleteLocalRef(local_view);
      }
      throw std::logic_error("HuxerUI Android PlatformView factory raised a Java exception while creating");
    }
    if (local_view == nullptr) {
      throw std::logic_error("HuxerUI Android PlatformView factory returned a null View");
    }

    const jint validation_result = environment->CallIntMethod(root, validate_platform_view, local_view);
    if (environment->ExceptionCheck()) {
      ClearJavaException(environment);
      if (factory->dispose) {
        try {
          factory->dispose(environment, local_view);
        } catch (...) {
        }
        ClearJavaException(environment);
      }
      environment->DeleteLocalRef(local_view);
      throw std::logic_error("HuxerUI Android PlatformView host failed to validate the factory result");
    }
    if (validation_result != 0) {
      if (factory->dispose) {
        try {
          factory->dispose(environment, local_view);
        } catch (...) {
        }
        ClearJavaException(environment);
      }
      environment->DeleteLocalRef(local_view);
      if (validation_result == 1) {
        throw std::logic_error("HuxerUI Android PlatformView factory returned an object that is not a View");
      }
      if (validation_result == 2) {
        throw std::logic_error(
            "HuxerUI Android PlatformView type uses a SurfaceView that cannot preserve RenderComposition order: " +
            std::string(command.Type())
        );
      }
      throw std::logic_error("HuxerUI Android PlatformView factory returned a View that already has a parent");
    }

    jobject retained_view = environment->NewGlobalRef(local_view);
    if (retained_view == nullptr) {
      if (factory->dispose) {
        try {
          factory->dispose(environment, local_view);
        } catch (...) {
        }
        ClearJavaException(environment);
      }
      environment->DeleteLocalRef(local_view);
      throw std::runtime_error("HuxerUI could not retain the Android PlatformView instance");
    }
    environment->DeleteLocalRef(local_view);

    auto hosted_view = std::make_unique<HostedPlatformView>();
    hosted_view->properties_revision = command.PropertiesRevision();
    hosted_view->type = command.Type();
    hosted_view->event_route = std::move(route);
    hosted_view->factory = *factory;
    hosted_view->view = retained_view;
    return hosted_view;
  }

  void Mount(JNIEnv* environment, std::uint64_t identity, HostedPlatformView& hosted_view) {
    const jint mount_result =
        environment->CallIntMethod(root, mount_platform_view, static_cast<jlong>(identity), hosted_view.view);
    if (environment->ExceptionCheck()) {
      ClearJavaException(environment);
      throw std::logic_error("HuxerUI Android PlatformView host failed to mount the factory result");
    }
    if (mount_result != 0) {
      throw std::logic_error("HuxerUI Android PlatformView factory result could not be mounted");
    }
    hosted_view.mounted = true;
  }

  void Update(JNIEnv* environment, HostedPlatformView& hosted_view, const PlatformPayload& properties) {
    if (!hosted_view.factory.update) {
      throw std::logic_error("HuxerUI Android PlatformView factory does not support property updates");
    }
    hosted_view.factory.update(environment, hosted_view.view, properties);
    if (environment->ExceptionCheck()) {
      ClearJavaException(environment);
      throw std::logic_error("HuxerUI Android PlatformView factory raised a Java exception while updating");
    }
  }

  void Dispose(JNIEnv* environment, std::uint64_t identity, HostedPlatformView& hosted_view) noexcept {
    hosted_view.event_route->active = false;
    if (hosted_view.mounted) {
      environment->CallVoidMethod(root, remove_platform_view, static_cast<jlong>(identity));
      ClearJavaException(environment);
      hosted_view.mounted = false;
    }
    if (hosted_view.view != nullptr && hosted_view.factory.dispose) {
      try {
        hosted_view.factory.dispose(environment, hosted_view.view);
      } catch (...) {
      }
      ClearJavaException(environment);
    }
    if (hosted_view.view != nullptr) {
      environment->DeleteGlobalRef(hosted_view.view);
      hosted_view.view = nullptr;
    }
  }

  JavaVM* virtual_machine = nullptr;
  jobject root = nullptr;
  jobject context = nullptr;
  AndroidRenderer* renderer = nullptr;
  PlatformModules* modules = nullptr;
  Runtime* runtime = nullptr;
  UIThreadDispatcher dispatch_to_ui_thread;
  const RenderFrame* frame = nullptr;
  std::optional<RenderSlice> base_slice;
  std::unordered_map<std::uint64_t, std::unique_ptr<HostedPlatformView>> hosted;
  jmethodID validate_platform_view = nullptr;
  jmethodID mount_platform_view = nullptr;
  jmethodID place_platform_view = nullptr;
  jmethodID remove_platform_view = nullptr;
  jmethodID commit_composition = nullptr;
  jmethodID apply_platform_view_focus = nullptr;
};

AndroidPlatformViews::AndroidPlatformViews(
    JNIEnv* environment,
    jobject root,
    jobject context,
    AndroidRenderer& renderer,
    PlatformModules& modules,
    Runtime& runtime,
    UIThreadDispatcher dispatch_to_ui_thread
)
    : state_(std::make_unique<State>(
          environment, root, context, renderer, modules, runtime, std::move(dispatch_to_ui_thread)
      )) {}

AndroidPlatformViews::~AndroidPlatformViews() {
  if (state_) {
    Shutdown(state_->Environment());
  }
}

void AndroidPlatformViews::Commit(JNIEnv* environment, const RenderFrame& frame) {
  RenderComposition composition = BuildRenderComposition(frame.scene);
  std::unordered_set<std::uint64_t> retained_identities;
  std::vector<std::pair<std::uint64_t, std::unique_ptr<HostedPlatformView>>> pending;
  try {
    for (const RenderCompositionLayer& layer : composition.layers) {
      const auto* placement = std::get_if<PlatformViewPlacement>(&layer);
      if (placement == nullptr) {
        continue;
      }
      const PlacePlatformViewCommand& command = *placement->command;
      retained_identities.insert(command.Identity());
      const auto found = state_->hosted.find(command.Identity());
      if (found == state_->hosted.end() || found->second->type != command.Type()) {
        pending.emplace_back(command.Identity(), state_->Create(environment, *placement));
        continue;
      }
      if (found->second->properties_revision != command.PropertiesRevision()) {
        state_->Update(environment, *found->second, command.Properties());
        found->second->properties_revision = command.PropertiesRevision();
      }
    }
  } catch (...) {
    for (auto& [identity, hosted_view] : pending) {
      state_->Dispose(environment, identity, *hosted_view);
    }
    throw;
  }

  for (auto& [identity, hosted_view] : pending) {
    const auto existing = state_->hosted.find(identity);
    if (existing != state_->hosted.end()) {
      state_->Dispose(environment, identity, *existing->second);
      state_->hosted.erase(existing);
    }
    try {
      state_->Mount(environment, identity, *hosted_view);
    } catch (...) {
      state_->Dispose(environment, identity, *hosted_view);
      throw;
    }
    state_->hosted.emplace(identity, std::move(hosted_view));
  }
  for (auto iterator = state_->hosted.begin(); iterator != state_->hosted.end();) {
    if (retained_identities.contains(iterator->first)) {
      ++iterator;
      continue;
    }
    state_->Dispose(environment, iterator->first, *iterator->second);
    iterator = state_->hosted.erase(iterator);
  }

  state_->base_slice.reset();
  bool passed_platform_view = false;
  std::vector<jlong> encoded_layers;
  encoded_layers.reserve(composition.layers.size() * 3);
  for (const RenderCompositionLayer& layer : composition.layers) {
    if (const auto* slice = std::get_if<RenderSlice>(&layer)) {
      if (!passed_platform_view && !state_->base_slice.has_value()) {
        state_->base_slice = *slice;
        continue;
      }
      if (slice->first_command > static_cast<std::size_t>(std::numeric_limits<jlong>::max()) ||
          slice->command_count > static_cast<std::size_t>(std::numeric_limits<jlong>::max())) {
        throw std::overflow_error("HuxerUI Android RenderComposition slice exceeds the JNI index range");
      }
      encoded_layers.push_back(0);
      encoded_layers.push_back(static_cast<jlong>(slice->first_command));
      encoded_layers.push_back(static_cast<jlong>(slice->command_count));
      continue;
    }

    const PlatformViewPlacement& placement = std::get<PlatformViewPlacement>(layer);
    passed_platform_view = true;
    const std::uint64_t identity = placement.command->Identity();
    const Rect visible_bounds = VisibleBounds(placement);
    const bool visible = placement.visible && !visible_bounds.IsEmpty();
    environment->CallVoidMethod(
        state_->root,
        state_->place_platform_view,
        static_cast<jlong>(identity),
        placement.world_bounds.x,
        placement.world_bounds.y,
        placement.world_bounds.width,
        placement.world_bounds.height,
        visible_bounds.x,
        visible_bounds.y,
        visible_bounds.width,
        visible_bounds.height,
        visible ? JNI_TRUE : JNI_FALSE
    );
    if (environment->ExceptionCheck()) {
      ClearJavaException(environment);
      throw std::logic_error("HuxerUI Android PlatformView host failed to apply a placement");
    }
    encoded_layers.push_back(1);
    encoded_layers.push_back(static_cast<jlong>(identity));
    encoded_layers.push_back(0);
  }

  if (encoded_layers.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max())) {
    throw std::overflow_error("HuxerUI Android RenderComposition descriptor exceeds the JNI array range");
  }
  const jsize layer_count = static_cast<jsize>(encoded_layers.size());
  jlongArray layers = environment->NewLongArray(layer_count);
  if (layers == nullptr) {
    throw std::runtime_error("HuxerUI could not allocate the Android RenderComposition descriptor");
  }
  if (!encoded_layers.empty()) {
    environment->SetLongArrayRegion(layers, 0, layer_count, encoded_layers.data());
  }
  environment->CallVoidMethod(state_->root, state_->commit_composition, layers);
  environment->DeleteLocalRef(layers);
  if (environment->ExceptionCheck()) {
    ClearJavaException(environment);
    throw std::logic_error("HuxerUI Android PlatformView host failed to commit RenderComposition");
  }

  state_->frame = &frame;
  for (auto& [identity, hosted_view] : state_->hosted) {
    static_cast<void>(identity);
    hosted_view->event_route->active = true;
  }

  const std::optional<std::uint64_t> focused_identity = RuntimeAccess::FocusedPlatformView(*state_->runtime);
  if (focused_identity.has_value() &&
      *focused_identity > static_cast<std::uint64_t>(std::numeric_limits<jlong>::max())) {
    throw std::overflow_error("HuxerUI Android PlatformView focus identity exceeds the JNI range");
  }
  const jlong requested_focus = focused_identity.has_value() ? static_cast<jlong>(*focused_identity) : 0;
  const jboolean focus_applied =
      environment->CallBooleanMethod(state_->root, state_->apply_platform_view_focus, requested_focus);
  if (environment->ExceptionCheck()) {
    ClearJavaException(environment);
    throw std::logic_error("HuxerUI Android PlatformView host failed to synchronize focus");
  }
  if (focused_identity.has_value() && focus_applied != JNI_TRUE) {
    RuntimeAccess::SynchronizePlatformViewFocus(*state_->runtime, std::nullopt, false);
  }
}

void AndroidPlatformViews::DrawBase(JNIEnv* environment, jobject canvas) {
  if (state_->frame == nullptr) {
    return;
  }
  if (state_->base_slice.has_value()) {
    state_->renderer->DrawSlice(
        environment,
        state_->root,
        canvas,
        *state_->frame,
        state_->base_slice->first_command,
        state_->base_slice->command_count
    );
    return;
  }
  state_->renderer->DrawSlice(environment, state_->root, canvas, *state_->frame, 0, 0);
}

void AndroidPlatformViews::DrawSlice(
    JNIEnv* environment, jobject canvas, std::size_t first_command, std::size_t command_count
) {
  if (state_->frame != nullptr) {
    state_->renderer->DrawSlice(environment, state_->root, canvas, *state_->frame, first_command, command_count);
  }
}

std::optional<std::uint64_t> AndroidPlatformViews::HitTest(Point point) const {
  return state_->runtime == nullptr ? std::nullopt : RuntimeAccess::HitTestPlatformView(*state_->runtime, point);
}

void AndroidPlatformViews::SynchronizeFocus(std::optional<std::uint64_t> identity, bool focus_visible) {
  if (state_->runtime != nullptr) {
    RuntimeAccess::SynchronizePlatformViewFocus(*state_->runtime, identity, focus_visible);
  }
}

bool AndroidPlatformViews::MoveFocus(std::uint64_t identity, bool reverse) {
  if (state_->runtime == nullptr || RuntimeAccess::FocusedPlatformView(*state_->runtime) != identity) {
    return false;
  }
  RuntimeAccess::MoveFocusFromPlatformView(*state_->runtime, identity, reverse);
  return true;
}

void AndroidPlatformViews::Shutdown(JNIEnv* environment) {
  if (!state_ || state_->root == nullptr || environment == nullptr) {
    return;
  }
  for (auto& [identity, hosted_view] : state_->hosted) {
    hosted_view->event_route->runtime = nullptr;
    state_->Dispose(environment, identity, *hosted_view);
  }
  state_->hosted.clear();
  state_->frame = nullptr;
  state_->base_slice.reset();
  state_->runtime = nullptr;
  state_->context = nullptr;
  environment->DeleteGlobalRef(state_->root);
  state_->root = nullptr;
}

} // namespace huxerui::detail
