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
#include "web_file.h"
#include "web_http_internal.h"
#include "web_platform_view.h"
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
EM_JS(emscripten::EM_VAL, CreateWebSurface, (const char* selector, float fallback_width, float fallback_height), {
  const host = document.querySelector(UTF8ToString(selector));
  if (!(host instanceof HTMLElement) || host.childNodes.length !== 0) {
    return Emval.toHandle(null);
  }
  if (host.getBoundingClientRect().width <= 0) {
    host.style.width = String(fallback_width) + "px";
  }
  if (host.getBoundingClientRect().height <= 0) {
    host.style.height = String(fallback_height) + "px";
  }

  Module.huxerUIWebPlatformViewFocusables ||= (container) => {
    if (!(container instanceof HTMLElement)) {
      return [];
    }
    const candidates = [];
    let order = 0;
    const visit = (element) => {
      if (!(element instanceof HTMLElement)) {
        return;
      }
      const currentOrder = order++;
      if (element.tabIndex >= 0 && !element.matches(":disabled") &&
          element.getClientRects().length > 0) {
        candidates.push({element, order : currentOrder, tabIndex : element.tabIndex});
      }
      if (element.shadowRoot) {
        for (const child of element.shadowRoot.children) {
          visit(child);
        }
      }
      for (const child of element.children) {
        visit(child);
      }
    };
    visit(container);
    candidates.sort((left, right) => {
      const leftPositive = left.tabIndex > 0;
      const rightPositive = right.tabIndex > 0;
      if (leftPositive !== rightPositive) {
        return leftPositive ? -1 : 1;
      }
      if (leftPositive && left.tabIndex !== right.tabIndex) {
        return left.tabIndex - right.tabIndex;
      }
      return left.order - right.order;
    });
    return candidates.map((candidate) => candidate.element);
  };

  const root = document.createElement("div");
  root.dataset.huxeruiRoot = "";
  root.tabIndex = 0;
  root.style.position = "relative";
  root.style.display = "block";
  root.style.width = "100%";
  root.style.height = "100%";
  root.style.overflow = "hidden";
  root.style.isolation = "isolate";
  root.style.contain = "layout paint style";
  root.style.outline = "none";
  root.style.touchAction = "none";

  const canvas = document.createElement("canvas");
  canvas.setAttribute("aria-hidden", "true");
  canvas.style.position = "absolute";
  canvas.style.inset = "0";
  canvas.style.width = "100%";
  canvas.style.height = "100%";
  canvas.style.pointerEvents = "none";
  canvas.style.zIndex = "0";
  root.appendChild(canvas);
  host.appendChild(root);
  return Emval.toHandle([host, root, canvas]);
});

EM_JS(void, RemoveWebSurface, (emscripten::EM_VAL root_handle), {
  const root = Emval.toValue(root_handle);
  if (root instanceof HTMLElement) {
    root.remove();
  }
});

EM_JS(
    bool,
    InstallWebSession,
    (std::uintptr_t session_id, emscripten::EM_VAL host_handle, emscripten::EM_VAL root_handle,
     emscripten::EM_VAL canvas_handle, const char* title),
    {
      let session = null;
      try {
        const host = Emval.toValue(host_handle);
        const root = Emval.toValue(root_handle);
        const canvas = Emval.toValue(canvas_handle);
        if (!(host instanceof HTMLElement) || !(root instanceof HTMLElement) ||
            !(canvas instanceof HTMLCanvasElement) || root.parentNode !== host || canvas.parentNode !== root) {
          return false;
        }

        Module.huxerUIWebSessions ||= new Map();
        if (Module.huxerUIWebSessions.has(session_id)) {
          return false;
        }
        for (const session of Module.huxerUIWebSessions.values()) {
          if (session.host === host) {
            return false;
          }
        }

        if (UTF8ToString(title)) {
          document.title = UTF8ToString(title);
        }

        session = {
          host,
          root,
          canvas,
          images : new Map(),
          imageSizes : new Map(),
          imageBytes : 0,
          imageFailures : new Map(),
          listeners : [],
          activePointers : new Map(),
          suppressedKeyUps : new Set(),
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
          session.suppressedKeyUps.clear();
          session.images.clear();
          session.imageSizes.clear();
          session.imageFailures.clear();
          session.imageBytes = 0;
          root.remove();
        };
        Module.huxerUIWebSessions.set(session_id, session);

        const listen = (target, type, listener, options) => {
          target.addEventListener(type, listener, options);
          session.listeners.push(() => target.removeEventListener(type, listener, options));
        };
        const position = (event) => {
          const bounds = root.getBoundingClientRect();
          const scaleX = bounds.width > 0 ? root.clientWidth / bounds.width : 1;
          const scaleY = bounds.height > 0 ? root.clientHeight / bounds.height : 1;
          return [ (event.clientX - bounds.left) * scaleX, (event.clientY - bounds.top) * scaleY ];
        };
        const platformToken = (target) => {
          const element = target instanceof Element ? target.closest("[data-huxerui-platform-view]") : null;
          if (!(element instanceof HTMLElement) || !root.contains(element)) {
            return 0;
          }
          const token = Number(element.dataset.huxeruiPlatformView);
          return Number.isSafeInteger(token) && token > 0 ? token : 0;
        };
        const platformViewWins = (token, point) =>
            Module._huxerui_web_platform_view_hit(session_id, token, point[0], point[1]);
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
            root,
            "pointerdown",
            (event) =>
                      {
                        const point = position(event);
                        const token = platformToken(event.target);
                        if (token && platformViewWins(token, point)) {
                          session.activePointers.set(event.pointerId, false);
                          return;
                        }
                        if (!session.activeTextInput) {
                          root.focus({preventScroll : true});
                        }
                        session.activePointers.set(event.pointerId, true);
                        try {
                          root.setPointerCapture(event.pointerId);
                        } catch (_) {
                        }
                        sendPointer(event, 0);
                        event.preventDefault();
                        event.stopPropagation();
                      },
            true
        );
        listen(
            root,
            "pointermove",
            (event) =>
                      {
                        const owner = session.activePointers.get(event.pointerId);
                        const token = platformToken(event.target);
                        if (owner === false || (owner === undefined && token && platformViewWins(token, position(event)))) {
                          return;
                        }
                        sendPointer(event, 2);
                        event.preventDefault();
                        event.stopPropagation();
                      },
            true
        );
        listen(
            root,
            "pointerleave",
            (event) =>
                      {
                        if (!session.activePointers.has(event.pointerId)) {
                          sendPointer(event, 3);
                        }
                      },
            true
        );
        listen(
            root,
            "pointerup",
            (event) =>
                      {
                        const owner = session.activePointers.get(event.pointerId);
                        session.activePointers.delete(event.pointerId);
                        const token = platformToken(event.target);
                        if (owner === false || (owner === undefined && token && platformViewWins(token, position(event)))) {
                          return;
                        }
                        sendPointer(event, 1);
                        queueMicrotask(() => {
                          if (Module.huxerUIWebSessions.get(session_id) === session && session.activeTextInput) {
                            session.activeTextInput.focus({preventScroll : true});
                          }
                        });
                        event.preventDefault();
                        event.stopPropagation();
                      },
            true
        );
        listen(
            root,
            "pointercancel",
            (event) =>
                      {
                        const owner = session.activePointers.get(event.pointerId);
                        session.activePointers.delete(event.pointerId);
                        const token = platformToken(event.target);
                        if (owner === false || (owner === undefined && token && platformViewWins(token, position(event)))) {
                          return;
                        }
                        sendPointer(event, 3);
                        event.preventDefault();
                        event.stopPropagation();
                      },
            true
        );
        const releaseCapturedPointer = (event) => {
          if (session.activePointers.get(event.pointerId) === false) {
            session.activePointers.delete(event.pointerId);
          }
        };
        listen(window, "pointerup", releaseCapturedPointer);
        listen(window, "pointercancel", releaseCapturedPointer);
        listen(
            root,
            "lostpointercapture",
            (event) =>
                      {
                        const owner = session.activePointers.get(event.pointerId);
                        if (owner === true) {
                          sendPointer(event, 3);
                        }
                        session.activePointers.delete(event.pointerId);
                      },
            true
        );
        listen(
            root,
            "wheel",
            (event) =>
                      {
                        const point = position(event);
                        const token = platformToken(event.target);
                        if (token && platformViewWins(token, point)) {
                          return;
                        }
                        const unit = event.deltaMode === WheelEvent.DOM_DELTA_LINE
                                                              ? 16
                                                              : event.deltaMode === WheelEvent.DOM_DELTA_PAGE
                                                                  ? root.clientHeight
                                                                  : 1;
                        Module._huxerui_web_wheel(
                            session_id, point[0], point[1], event.deltaX * unit, event.deltaY * unit
                        );
                        event.preventDefault();
                        event.stopPropagation();
                      },
            {capture : true, passive : false}
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
        const hasInternalTabDestination = (container, reverse) => {
          const candidates = Module.huxerUIWebPlatformViewFocusables(container);
          let activeElement = document.activeElement;
          while (activeElement && activeElement.shadowRoot && activeElement.shadowRoot.activeElement) {
            activeElement = activeElement.shadowRoot.activeElement;
          }
          const index = candidates.indexOf(activeElement);
          return index >= 0 && (reverse ? index > 0 : index + 1 < candidates.length);
        };
        listen(
            root,
            "keydown",
            (event) =>
                      {
                        const keyIdentity = event.code || event.key;
                        session.suppressedKeyUps.delete(keyIdentity);
                        const token = platformToken(event.target);
                        if (token) {
                          if (event.key === "Tab" && !hasInternalTabDestination(event.target.closest(
                                  "[data-huxerui-platform-view]"
                              ), event.shiftKey)) {
                            event.preventDefault();
                            event.stopPropagation();
                            session.suppressedKeyUps.add(keyIdentity);
                            Module._huxerui_web_platform_view_move_focus(session_id, token, event.shiftKey);
                          }
                          return;
                        }
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
                      },
            true
        );
        listen(root, "keyup", (event) => {
          if (session.suppressedKeyUps.delete(event.code || event.key)) {
            event.preventDefault();
            event.stopPropagation();
            return;
          }
          if (!platformToken(event.target)) {
            sendKey(event, 1);
          }
        }, true);
        listen(window, "keyup", (event) => {
          session.suppressedKeyUps.delete(event.code || event.key);
        });
        listen(root, "focusin", (event) => {
          const token = platformToken(event.target);
          if (token) {
            const focusVisible = event.target instanceof Element && event.target.matches(":focus-visible");
            Module._huxerui_web_platform_view_focus(session_id, token, focusVisible);
          }
        }, true);
        listen(root, "focusout", (event) => {
          if (!platformToken(event.target)) {
            return;
          }
          queueMicrotask(() => {
            if (Module.huxerUIWebSessions.get(session_id) !== session) {
              return;
            }
            const token = platformToken(document.activeElement);
            const focusVisible = document.activeElement instanceof Element &&
                document.activeElement.matches(":focus-visible");
            Module._huxerui_web_platform_view_focus(session_id, token, focusVisible);
          });
        }, true);

        const resize = () => {
          if (!root.isConnected) {
            return;
          }
          const bounds = root.getBoundingClientRect();
          Module._huxerui_web_resize(
              session_id,
              Math.max(0, bounds.width),
              Math.max(0, bounds.height),
              Math.max(1, window.devicePixelRatio || 1)
          );
        };
        session.resizeObserver = new ResizeObserver(resize);
        session.resizeObserver.observe(host);
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
  WebPlatformAdapter(std::uintptr_t session_id, val root, val canvas, ResourceConfiguration configuration)
      : PlatformAdapter(DispatchToWebUIThread), session_id_(session_id), renderer_(session_id, canvas),
        resources_(std::move(configuration)), text_input_(session_id), root_(std::move(root)),
        base_canvas_(std::move(canvas)) {}

  void Attach(Runtime& runtime) {
    runtime_ = &runtime;
    text_input_.SetRuntime(runtime_);
    platform_views_ = std::make_unique<WebPlatformViews>(
        renderer_,
        Modules(),
        runtime,
        DispatchToWebUIThread,
        root_,
        base_canvas_
    );
  }

  void Ready() {
    platform_ready_ = true;
    if (const std::optional<double> deadline = frame_state_.TakeDeferred(true)) {
      Schedule(*deadline);
    } else {
      RequestFrameAt(Now());
    }
  }

  void Shutdown() noexcept {
    platform_ready_ = false;
    if (platform_views_) {
      platform_views_->Shutdown();
      platform_views_.reset();
    }
    text_input_.Reset();
    runtime_ = nullptr;
  }

  void RequestFrameAt(double deadline) override {
    if (const std::optional<double> scheduled = frame_state_.Request(deadline, Now(), platform_ready_)) {
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

  std::shared_ptr<HttpTransport> CreateHttpTransport() override {
    return CreateWebHttpTransport();
  }

  std::shared_ptr<FilePickerTransport> CreateFilePickerTransport() override {
    return CreateWebFilePickerTransport();
  }

  std::shared_ptr<FileSystem> CreateFileSystem() override {
    return CreateWebFileSystem();
  }

  void Resize(float width, float height, float display_scale) {
    const Size viewport{std::max(0.0F, width), std::max(0.0F, height)};
    display_scale = std::max(1.0F, display_scale);
    if (viewport == viewport_ && display_scale == resources_.Configuration().display_scale) {
      return;
    }
    viewport_ = viewport;
    renderer_.SetViewport(viewport_, display_scale);
    if (platform_views_) {
      platform_views_->SetViewport(viewport_, display_scale);
    }
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
    renderer_.BeginFrame();
    platform_views_->Commit(commit.render_frame);
    if (const std::optional<double> deadline = frame_state_.EndPaint(platform_ready_)) {
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

  bool PlatformViewHit(std::uint32_t token, Point point) const {
    return platform_views_ && platform_views_->HitTest(token, point);
  }

  void SynchronizePlatformViewFocus(std::uint32_t token, bool focus_visible) {
    if (platform_views_) {
      platform_views_->SynchronizeFocus(token, focus_visible);
    }
  }

  void MoveFocusFromPlatformView(std::uint32_t token, bool reverse) {
    if (platform_views_) {
      platform_views_->MoveFocus(token, reverse);
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
  val root_;
  val base_canvas_;
  std::unique_ptr<WebPlatformViews> platform_views_;
  PlatformFrameState frame_state_;
  Size viewport_;
  bool platform_ready_ = false;
};

class WebSession final {
public:
  WebSession(std::uintptr_t session_id, val host, val root, val canvas, ResourceConfiguration configuration)
      : session_id_(session_id), host_(std::move(host)), root_(std::move(root)), canvas_(std::move(canvas)),
        platform_(session_id, root_, canvas_, configuration), runtime_(CurrentApplication(), platform_) {
    platform_.Attach(runtime_);
  }

  ~WebSession() {
    platform_.Shutdown();
    UninstallWebSession(session_id_);
    RemoveWebSurface(root_.as_handle());
  }

  bool Initialize() {
    const WindowOptions& options = CurrentApplication().options.window;
    if (!InstallWebSession(
            session_id_,
            host_.as_handle(),
            root_.as_handle(),
            canvas_.as_handle(),
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
  val host_;
  val root_;
  val canvas_;
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
  val pending_root = val::undefined();
  try {
    const WindowOptions& options = CurrentApplication().options.window;
    val surface =
        val::take_ownership(CreateWebSurface(selector.c_str(), options.initial_size.width, options.initial_size.height)
        );
    if (surface.isNull() || surface.isUndefined()) {
      return 0;
    }
    val host = surface[0];
    val root = surface[1];
    val canvas = surface[2];
    pending_root = root;
    session_id = NextSessionId();
    auto session = std::make_unique<WebSession>(
        session_id,
        std::move(host),
        std::move(root),
        std::move(canvas),
        BrowserResourceConfiguration()
    );
    WebSession* inserted = session.get();
    Sessions().emplace(session_id, std::move(session));
    if (!inserted->Initialize()) {
      Sessions().erase(session_id);
      return 0;
    }
    return session_id;
  } catch (const std::exception& error) {
    Sessions().erase(session_id);
    if (!pending_root.isUndefined() && !pending_root.isNull()) {
      RemoveWebSurface(pending_root.as_handle());
    }
    emscripten_log(EM_LOG_ERROR, "HuxerUI Web mount failed: %s", error.what());
    return 0;
  } catch (...) {
    Sessions().erase(session_id);
    if (!pending_root.isUndefined() && !pending_root.isNull()) {
      RemoveWebSurface(pending_root.as_handle());
    }
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

EMSCRIPTEN_KEEPALIVE bool
huxerui_web_platform_view_hit(std::uintptr_t session_id, std::uint32_t token, float x, float y) {
  bool hit = false;
  huxerui::detail::DispatchWebSession(session_id, "PlatformView hit test", [&](auto& platform) {
    hit = platform.PlatformViewHit(token, {x, y});
  });
  return hit;
}

EMSCRIPTEN_KEEPALIVE void
huxerui_web_platform_view_focus(std::uintptr_t session_id, std::uint32_t token, bool focus_visible) {
  huxerui::detail::DispatchWebSession(session_id, "PlatformView focus", [=](auto& platform) {
    platform.SynchronizePlatformViewFocus(token, focus_visible);
  });
}

EMSCRIPTEN_KEEPALIVE void
huxerui_web_platform_view_move_focus(std::uintptr_t session_id, std::uint32_t token, bool reverse) {
  huxerui::detail::DispatchWebSession(session_id, "PlatformView focus traversal", [=](auto& platform) {
    platform.MoveFocusFromPlatformView(token, reverse);
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
