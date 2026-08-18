#include <huxerui/app.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "platform_frame_internal.h"
#include "resource_internal.h"
#include "text_layout_internal.h"
#include "web_renderer.h"
#include "web_text_input.h"

namespace huxerui::detail {

namespace {

using emscripten::val;

constexpr std::array web_key_order{
    Key::Unknown,   Key::Tab,        Key::Enter,   Key::Space,     Key::Escape, Key::Backspace, Key::Delete,
    Key::ArrowLeft, Key::ArrowRight, Key::ArrowUp, Key::ArrowDown, Key::Home,   Key::End,       Key::PageUp,
    Key::PageDown,  Key::A,          Key::C,       Key::V,         Key::X,      Key::Y,         Key::Z,
    Key::Shift,     Key::Control,    Key::Alt,     Key::Meta,
};

static_assert(
    [] {
      for (std::size_t index = 0; index < web_key_order.size(); ++index) {
        if (static_cast<std::size_t>(web_key_order[index]) != index) {
          return false;
        }
      }
      return true;
    }(),
    "HuxerUI Web key mapping must match the Key enum order"
);

class WebSession;

std::unordered_map<std::uintptr_t, std::unique_ptr<WebSession>>& Sessions() {
  static std::unordered_map<std::uintptr_t, std::unique_ptr<WebSession>> sessions;
  return sessions;
}

std::uintptr_t NextSessionId() noexcept {
  static std::uintptr_t next_id = 1;
  return next_id++;
}

WebSession* FindSession(std::uintptr_t session_id) noexcept {
  const auto found = Sessions().find(session_id);
  return found == Sessions().end() ? nullptr : found->second.get();
}

void RunWebUIThreadTask(void* context) noexcept {
  std::unique_ptr<std::function<void()>> task(static_cast<std::function<void()>*>(context));
  try {
    (*task)();
  } catch (...) {
  }
}

void DispatchToWebUIThread(std::function<void()> task) {
  auto pending = std::make_unique<std::function<void()>>(std::move(task));
  emscripten_async_call(RunWebUIThreadTask, pending.get(), 0);
  static_cast<void>(pending.release());
}

// clang-format off
EM_JS(
    bool,
    InstallWebSession,
    (std::uintptr_t session_id, const char* selector, float fallback_width, float fallback_height, const char* title),
    {
      let session = null;
      try {
        const canvas = document.querySelector(UTF8ToString(selector));
        if (!(canvas instanceof HTMLCanvasElement)) {
          return false;
        }

        Module.huxerUIWebSessions ||= new Map();
        if (Module.huxerUIWebSessions.has(session_id)) {
          return false;
        }
        for (const session of Module.huxerUIWebSessions.values()) {
          if (session.canvas === canvas) {
            return false;
          }
        }

        if (canvas.getBoundingClientRect().width <= 0) {
          canvas.style.width = String(fallback_width) + "px";
        }
        if (canvas.getBoundingClientRect().height <= 0) {
          canvas.style.height = String(fallback_height) + "px";
        }
        if (UTF8ToString(title)) {
          document.title = UTF8ToString(title);
        }

        session = {
          canvas,
          images : new Map(),
          imageSizes : new Map(),
          imageBytes : 0,
          imageFailures : new Map(),
          listeners : [],
          activePointers : new Set(),
          resizeObserver : null,
          resolutionQuery : null,
          frameTimer : 0,
          frameDeadline : Infinity,
          animationFrame : 0,
        };
        session.dispose = () => {
          try {
            if (session.frameTimer) {
              clearTimeout(session.frameTimer);
            }
            if (session.animationFrame) {
              cancelAnimationFrame(session.animationFrame);
            }
            if (session.resizeObserver) {
              session.resizeObserver.disconnect();
            }
          } catch (error) {
            console.error("HuxerUI Web session scheduling cleanup failed", error);
          }
          for (const remove of session.listeners) {
            try {
              remove();
            } catch (error) {
              console.error("HuxerUI Web listener cleanup failed", error);
            }
          }
          for (const image of session.images.values()) {
            try {
              if (image && image.close) {
                image.close();
              }
            } catch (error) {
              console.error("HuxerUI Web image cleanup failed", error);
            }
          }
          session.listeners.length = 0;
          session.images.clear();
          session.imageSizes.clear();
          session.imageFailures.clear();
          session.imageBytes = 0;
        };
        Module.huxerUIWebSessions.set(session_id, session);

        const listen = (target, type, listener, options) => {
          target.addEventListener(type, listener, options);
          session.listeners.push(() => target.removeEventListener(type, listener, options));
        };
        const position = (event) => {
          const bounds = canvas.getBoundingClientRect();
          return [ event.clientX - bounds.left, event.clientY - bounds.top ];
        };
        const pointerKind = (value) => value === "touch" ? 1 : value === "pen" ? 2 : 0;
        const sendPointer = (event, type) => {
          const point = position(event);
          Module._huxerui_web_pointer(
              session_id,
              type,
              event.pointerId,
              point[0],
              point[1],
              pointerKind(event.pointerType),
              Math.max(1, event.detail || 1)
          );
        };

        listen(
            canvas,
            "pointerdown",
            (event) =>
                      {
                        if (!session.activeTextInput) {
                          canvas.focus({preventScroll : true});
                        }
                        session.activePointers.add(event.pointerId);
                        try {
                          canvas.setPointerCapture(event.pointerId);
                        } catch (_) {
                        }
                        sendPointer(event, 0);
                        event.preventDefault();
                      }
        );
        listen(
            canvas,
            "pointermove",
            (event) =>
                      {
                        sendPointer(event, 2);
                        event.preventDefault();
                      }
        );
        listen(
            canvas,
            "pointerleave",
            (event) =>
                      {
                        if (!session.activePointers.has(event.pointerId)) {
                          sendPointer(event, 3);
                        }
                      }
        );
        listen(
            canvas,
            "pointerup",
            (event) =>
                      {
                        sendPointer(event, 1);
                        session.activePointers.delete(event.pointerId);
                        queueMicrotask(() => {
                          if (Module.huxerUIWebSessions.get(session_id) === session && session.activeTextInput) {
                            session.activeTextInput.focus({preventScroll : true});
                          }
                        });
                        event.preventDefault();
                      }
        );
        listen(
            canvas,
            "pointercancel",
            (event) =>
                      {
                        sendPointer(event, 3);
                        session.activePointers.delete(event.pointerId);
                        event.preventDefault();
                      }
        );
        listen(
            canvas,
            "lostpointercapture",
            (event) =>
                      {
                        if (session.activePointers.delete(event.pointerId)) {
                          sendPointer(event, 3);
                        }
                      }
        );
        listen(
            canvas,
            "wheel",
            (event) =>
                      {
                        const point = position(event);
                        const unit = event.deltaMode === WheelEvent.DOM_DELTA_LINE
                                                              ? 16
                                                              : event.deltaMode === WheelEvent.DOM_DELTA_PAGE
                                                                  ? canvas.clientHeight
                                                                  : 1;
                        Module._huxerui_web_wheel(
                            session_id, point[0], point[1], event.deltaX * unit, event.deltaY * unit
                        );
                        event.preventDefault();
                      },
            {passive : false}
        );

        const keyValue = (key) => {
          switch (key) {
          case "Tab":
            return 1;
          case "Enter":
            return 2;
          case " ":
            return 3;
          case "Escape":
            return 4;
          case "Backspace":
            return 5;
          case "Delete":
            return 6;
          case "ArrowLeft":
            return 7;
          case "ArrowRight":
            return 8;
          case "ArrowUp":
            return 9;
          case "ArrowDown":
            return 10;
          case "Home":
            return 11;
          case "End":
            return 12;
          case "PageUp":
            return 13;
          case "PageDown":
            return 14;
          case "a":
          case "A":
            return 15;
          case "c":
          case "C":
            return 16;
          case "v":
          case "V":
            return 17;
          case "x":
          case "X":
            return 18;
          case "y":
          case "Y":
            return 19;
          case "z":
          case "Z":
            return 20;
          case "Shift":
            return 21;
          case "Control":
            return 22;
          case "Alt":
            return 23;
          case "Meta":
            return 24;
          default:
            return 0;
          }
        };
        const sendKey = (event, type) => {
          const text =
              !event.ctrlKey && !event.metaKey && !event.altKey && Array.from(event.key).length === 1 ? event.key : "";
          const textPointer = Module.stringToNewUTF8(text);
          try {
            Module._huxerui_web_key(
                session_id,
                type,
                keyValue(event.key),
                textPointer,
                event.shiftKey,
                event.ctrlKey,
                event.altKey,
                event.metaKey,
                event.repeat
            );
          } finally {
            _free(textPointer);
          }
        };
        session.sendKey = sendKey;
        listen(
            canvas,
            "keydown",
            (event) =>
                      {
                        sendKey(event, 0);
                        if ([
                              "Tab",
                              "Enter",
                              " ",
                              "Escape",
                              "Backspace",
                              "Delete",
                              "ArrowLeft",
                              "ArrowRight",
                              "ArrowUp",
                              "ArrowDown",
                              "Home",
                              "End",
                              "PageUp",
                              "PageDown"
                            ]
                                .includes(event.key)) {
                          event.preventDefault();
                        }
                      }
        );
        listen(canvas, "keyup", (event) => sendKey(event, 1));

        const resize = () => {
          if (!canvas.isConnected) {
            return;
          }
          const bounds = canvas.getBoundingClientRect();
          Module._huxerui_web_resize(
              session_id,
              Math.max(0, bounds.width),
              Math.max(0, bounds.height),
              Math.max(1, window.devicePixelRatio || 1)
          );
        };
        session.resizeObserver = new ResizeObserver(resize);
        session.resizeObserver.observe(canvas);
        const observeResolution = () => {
          if (session.resolutionQuery) {
            session.resolutionQuery.removeEventListener("change", observeResolution);
          }
          session.resolutionQuery = matchMedia(
              "(resolution: " + String(Math.max(1, window.devicePixelRatio || 1)) + "dppx)"
          );
          session.resolutionQuery.addEventListener("change", observeResolution);
          resize();
        };
        session.listeners.push(() => {
          if (session.resolutionQuery) {
            session.resolutionQuery.removeEventListener("change", observeResolution);
          }
        });
        listen(window, "resize", resize);
        listen(
            document,
            "visibilitychange",
            () =>
                 {
                   if (!document.hidden) {
                     Module._huxerui_web_visible(session_id);
                     resize();
                   }
                 }
        );
        observeResolution();
        return true;
      } catch (error) {
        if (session) {
          if (Module.huxerUIWebSessions) {
            Module.huxerUIWebSessions.delete(session_id);
          }
          if (session.dispose) {
            session.dispose();
          }
        }
        console.error("HuxerUI Web session installation failed", error);
        return false;
      }
    }
);

EM_JS(void, UninstallWebSession, (std::uintptr_t session_id), {
  try {
    const sessions = Module.huxerUIWebSessions;
    const session = sessions && sessions.get(session_id);
    if (!session) {
      return;
    }
    sessions.delete(session_id);
    session.dispose();
  } catch (error) {
    console.error("HuxerUI Web session removal failed", error);
  }
});

EM_JS(void, ScheduleWebFrame, (std::uintptr_t session_id, double deadline), {
  const sessions = Module.huxerUIWebSessions;
  const session = sessions && sessions.get(session_id);
  if (!session || !session.canvas.isConnected || !Number.isFinite(deadline)) {
    return;
  }
  const request = () => {
    session.frameTimer = 0;
    session.frameDeadline = Infinity;
    if (!session.animationFrame) {
      session.animationFrame = requestAnimationFrame(() => {
        session.animationFrame = 0;
        if (sessions.get(session_id) === session && session.canvas.isConnected) {
          Module._huxerui_web_frame(session_id);
        }
      });
    }
  };
  const delay = Math.max(0, deadline * 1000 - performance.now());
  if (delay <= 0) {
    if (session.frameTimer) {
      clearTimeout(session.frameTimer);
      session.frameTimer = 0;
      session.frameDeadline = Infinity;
    }
    request();
    return;
  }
  if (session.animationFrame || session.frameDeadline <= deadline) {
    return;
  }
  if (session.frameTimer) {
    clearTimeout(session.frameTimer);
  }
  session.frameDeadline = deadline;
  session.frameTimer = setTimeout(request, Math.min(delay, 2147483647));
});

EM_JS(double, WebNow, (), { return performance.now() / 1000.0; });

// clang-format on

class WebResources final : public PlatformResources {
public:
  explicit WebResources(ResourceConfiguration configuration) : configuration_(std::move(configuration)) {}

  [[nodiscard]] ResourceConfiguration Configuration() const override {
    return configuration_;
  }

  void SetConfiguration(ResourceConfiguration configuration) {
    configuration_ = std::move(configuration);
  }

  [[nodiscard]] RawAsset Read(std::string_view package_path) override {
    if (!IsValidResourcePackagePath(package_path)) {
      throw std::logic_error("HuxerUI Web resource path is invalid");
    }
    std::ifstream stream(std::filesystem::path("/") / std::string(package_path), std::ios::binary);
    if (!stream) {
      return {};
    }
    std::vector<char> source{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    std::vector<std::byte> bytes(source.size());
    std::transform(source.begin(), source.end(), bytes.begin(), [](char value) {
      return static_cast<std::byte>(static_cast<unsigned char>(value));
    });
    return RawAsset::FromBytes(std::move(bytes));
  }

private:
  ResourceConfiguration configuration_;
};

class WebPlatformAdapter final : public PlatformAdapter {
public:
  WebPlatformAdapter(std::uintptr_t session_id, val canvas, ResourceConfiguration configuration)
      : PlatformAdapter(DispatchToWebUIThread), session_id_(session_id), renderer_(session_id, std::move(canvas)),
        resources_(std::move(configuration)), text_input_(session_id) {}

  void Attach(Runtime& runtime) noexcept {
    runtime_ = &runtime;
    text_input_.SetRuntime(runtime_);
  }

  void Ready() {
    native_ready_ = true;
    if (const std::optional<double> deadline = frame_state_.TakeDeferred(true)) {
      Schedule(*deadline);
    } else {
      RequestFrameAt(Now());
    }
  }

  void Shutdown() noexcept {
    native_ready_ = false;
    text_input_.Reset();
    runtime_ = nullptr;
  }

  void RequestFrameAt(double deadline) override {
    if (const std::optional<double> scheduled = frame_state_.Request(deadline, Now(), native_ready_)) {
      Schedule(*scheduled);
    }
  }

  double Now() const noexcept override {
    return WebNow();
  }

  FontMetrics Metrics(const Font& font) override {
    return renderer_.Metrics(font);
  }

  TextRunMetrics
  MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options = {}) override {
    return renderer_.MeasureRun(text, style, options);
  }

  TextLayoutMetrics MeasureText(
      std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options = {}
  ) override {
    return renderer_.MeasureText(text, style, max_width, options);
  }

  std::unique_ptr<TextLayout> CreateTextLayout(
      std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options = {}
  ) override {
    return renderer_.CreateTextLayout(text, style, max_width, options);
  }

  PlatformClipboard* Clipboard() noexcept override {
    return nullptr;
  }

  PlatformResources* Resources() noexcept override {
    return &resources_;
  }

  PlatformTextInput* TextInput() noexcept override {
    return &text_input_;
  }

  void Resize(float width, float height, float display_scale) {
    const Size viewport{std::max(0.0F, width), std::max(0.0F, height)};
    display_scale = std::max(1.0F, display_scale);
    if (viewport == viewport_ && display_scale == resources_.Configuration().display_scale) {
      return;
    }
    viewport_ = viewport;
    renderer_.SetViewport(viewport_, display_scale);
    ResourceConfiguration configuration = resources_.Configuration();
    configuration.display_scale = display_scale;
    resources_.SetConfiguration(configuration);
    if (runtime_ != nullptr) {
      runtime_->SetWindowMetrics({.viewport = viewport_});
      runtime_->UpdateResourceConfiguration(configuration);
    }
  }

  void Frame() {
    if (runtime_ == nullptr || !frame_state_.BeginCommit()) {
      return;
    }
    const FrameCommit& commit = runtime_->BuildFrame();
    frame_state_.BeginPaint();
    renderer_.Draw(commit.render_frame);
    if (const std::optional<double> deadline = frame_state_.EndPaint(native_ready_)) {
      Schedule(*deadline);
    }
    if (commit.next_frame_deadline.has_value()) {
      RequestFrameAt(*commit.next_frame_deadline);
    }
  }

  void HandlePointer(PointerEvent event) {
    if (runtime_ != nullptr) {
      runtime_->HandlePointerEvent(event);
    }
  }

  void HandleWheel(ScrollEvent event) {
    if (runtime_ != nullptr) {
      runtime_->HandleScrollEvent(event);
    }
  }

  void HandleKey(KeyEvent event) {
    if (runtime_ != nullptr) {
      runtime_->HandleKeyEvent(event);
    }
  }

  void ImageReady() {
    renderer_.Invalidate();
    RequestFrameAt(Now());
  }

private:
  void Schedule(double deadline) {
    ScheduleWebFrame(session_id_, deadline);
  }

  std::uintptr_t session_id_ = 0;
  Runtime* runtime_ = nullptr;
  WebRenderer renderer_;
  WebResources resources_;
  WebTextInput text_input_;
  PlatformFrameState frame_state_;
  Size viewport_;
  bool native_ready_ = false;
};

class WebSession final {
public:
  WebSession(std::uintptr_t session_id, val canvas, ResourceConfiguration configuration)
      : session_id_(session_id), platform_(session_id, std::move(canvas), configuration),
        runtime_(CurrentApplication(), platform_) {
    platform_.Attach(runtime_);
  }

  ~WebSession() {
    platform_.Shutdown();
    UninstallWebSession(session_id_);
  }

  bool Initialize(std::string_view selector) {
    const WindowOptions& options = CurrentApplication().options.window;
    const std::string selector_copy{selector};
    if (!InstallWebSession(
            session_id_, selector_copy.c_str(), options.initial_size.width, options.initial_size.height,
            options.title.c_str()
        )) {
      return false;
    }
    platform_.Ready();
    return true;
  }

  WebPlatformAdapter& Platform() noexcept {
    return platform_;
  }

private:
  std::uintptr_t session_id_ = 0;
  WebPlatformAdapter platform_;
  Runtime runtime_;
};

template <typename Callback>
void DispatchWebSession(std::uintptr_t session_id, const char* operation, Callback&& callback) noexcept {
  WebSession* session = FindSession(session_id);
  if (session == nullptr) {
    return;
  }
  try {
    callback(session->Platform());
  } catch (const std::exception& error) {
    emscripten_log(EM_LOG_ERROR, "HuxerUI Web %s failed: %s", operation, error.what());
    Sessions().erase(session_id);
  } catch (...) {
    emscripten_log(EM_LOG_ERROR, "HuxerUI Web %s failed with an unknown exception", operation);
    Sessions().erase(session_id);
  }
}

ResourceConfiguration BrowserResourceConfiguration() {
  ResourceConfiguration configuration;
  const val navigator = val::global("navigator");
  if (!navigator.isUndefined() && !navigator["language"].isUndefined()) {
    try {
      configuration.locale = Locale::FromLanguageTag(navigator["language"].as<std::string>());
    } catch (const std::invalid_argument&) {
      configuration.locale = Locale::Default();
    }
  }
  const val window = val::global("window");
  if (!window.isUndefined() && !window["devicePixelRatio"].isUndefined()) {
    configuration.display_scale = std::max(1.0, window["devicePixelRatio"].as<double>());
  }
  return configuration;
}

std::uintptr_t MountWebSession(const std::string& selector) {
  std::uintptr_t session_id = 0;
  try {
    const val canvas = val::global("document").call<val>("querySelector", selector);
    if (canvas.isNull() || canvas.isUndefined()) {
      return 0;
    }
    session_id = NextSessionId();
    auto session = std::make_unique<WebSession>(session_id, canvas, BrowserResourceConfiguration());
    WebSession* inserted = session.get();
    Sessions().emplace(session_id, std::move(session));
    if (!inserted->Initialize(selector)) {
      Sessions().erase(session_id);
      return 0;
    }
    return session_id;
  } catch (const std::exception& error) {
    Sessions().erase(session_id);
    emscripten_log(EM_LOG_ERROR, "HuxerUI Web mount failed: %s", error.what());
    return 0;
  } catch (...) {
    Sessions().erase(session_id);
    emscripten_log(EM_LOG_ERROR, "HuxerUI Web mount failed with an unknown exception");
    return 0;
  }
}

void DisposeWebSession(std::uintptr_t session_id) {
  Sessions().erase(session_id);
}

} // namespace

void EnsureWebPlatformLinked() {}

} // namespace huxerui::detail

extern "C" {

EMSCRIPTEN_KEEPALIVE void huxerui_web_frame(std::uintptr_t session_id) {
  huxerui::detail::DispatchWebSession(session_id, "frame", [](auto& platform) { platform.Frame(); });
}

EMSCRIPTEN_KEEPALIVE void
huxerui_web_resize(std::uintptr_t session_id, float width, float height, float display_scale) {
  huxerui::detail::DispatchWebSession(session_id, "resize", [=](auto& platform) {
    platform.Resize(width, height, display_scale);
  });
}

EMSCRIPTEN_KEEPALIVE void huxerui_web_visible(std::uintptr_t session_id) {
  huxerui::detail::DispatchWebSession(session_id, "visibility update", [](auto& platform) {
    platform.RequestFrameAt(platform.Now());
  });
}

EMSCRIPTEN_KEEPALIVE void huxerui_web_image_ready(std::uintptr_t session_id) {
  huxerui::detail::DispatchWebSession(session_id, "image update", [](auto& platform) { platform.ImageReady(); });
}

EMSCRIPTEN_KEEPALIVE void huxerui_web_pointer(
    std::uintptr_t session_id,
    int type,
    std::int32_t pointer_id,
    float x,
    float y,
    int device_kind,
    std::uint32_t click_count
) {
  huxerui::detail::DispatchWebSession(session_id, "pointer input", [=](auto& platform) {
    platform.HandlePointer({
        static_cast<huxerui::PointerEventType>(std::clamp(type, 0, 3)),
        pointer_id,
        {x, y},
        static_cast<huxerui::PointerDeviceKind>(std::clamp(device_kind, 0, 2)),
        click_count,
    });
  });
}

EMSCRIPTEN_KEEPALIVE void huxerui_web_wheel(std::uintptr_t session_id, float x, float y, float delta_x, float delta_y) {
  huxerui::detail::DispatchWebSession(session_id, "wheel input", [=](auto& platform) {
    platform.HandleWheel({{x, y}, delta_x, delta_y});
  });
}

EMSCRIPTEN_KEEPALIVE void huxerui_web_key(
    std::uintptr_t session_id,
    int type,
    int key,
    const char* text,
    bool shift,
    bool control,
    bool alt,
    bool meta,
    bool repeat
) {
  huxerui::detail::DispatchWebSession(session_id, "key input", [&](auto& platform) {
    platform.HandleKey({
        static_cast<huxerui::KeyEventType>(std::clamp(type, 0, 1)),
        static_cast<huxerui::Key>(std::clamp(key, 0, 20)),
        text == nullptr ? std::string{} : std::string{text},
        {shift, control, alt, meta},
        repeat,
    });
  });
}
}

EMSCRIPTEN_BINDINGS(huxerui_web) {
  emscripten::function("mountHuxerUI", &huxerui::detail::MountWebSession);
  emscripten::function("disposeHuxerUI", &huxerui::detail::DisposeWebSession);
}
