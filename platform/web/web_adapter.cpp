#include <huxerui/app.h>

#include <algorithm>
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
#include <emscripten/eventloop.h>
#include <emscripten/val.h>

#include "application/platform_frame_internal.h"
#include "resources/resource_internal.h"
#include "text/text_internal.h"
#include "web_application_internal.h"
#include "web_file.h"
#include "web_http_internal.h"
#include "web_platform_view.h"
#include "web_renderer.h"
#include "web_text_input.h"

namespace huxerui::detail {

namespace {

using emscripten::val;

static_assert(
    static_cast<int>(Key::Unknown) == 0 && static_cast<int>(Key::Backspace) == 1 &&
        static_cast<int>(Key::ArrowDown) == 15 && static_cast<int>(Key::A) == 16 &&
        static_cast<int>(Key::Z) == 41 && static_cast<int>(Key::Digit0) == 42 &&
        static_cast<int>(Key::Digit9) == 51 && static_cast<int>(Key::Backquote) == 52 &&
        static_cast<int>(Key::ScrollLock) == 76 && static_cast<int>(Key::F1) == 77 &&
        static_cast<int>(Key::F24) == 100 && static_cast<int>(Key::PrintScreen) == 101 &&
        static_cast<int>(Key::Help) == 104 && static_cast<int>(Key::Numpad0) == 105 &&
        static_cast<int>(Key::NumpadClear) == 123,
    "HuxerUI Web key mapping must match the Key enum order"
);

class WebUiWindow;

std::unordered_map<std::uintptr_t, std::unique_ptr<WebUiWindow>>& Sessions() {
  static std::unordered_map<std::uintptr_t, std::unique_ptr<WebUiWindow>> sessions;
  return sessions;
}

std::uintptr_t NextSessionId() noexcept {
  static std::uintptr_t next_id = 1;
  return next_id++;
}

WebUiWindow* FindSession(std::uintptr_t session_id) noexcept {
  const auto found = Sessions().find(session_id);
  return found == Sessions().end() ? nullptr : found->second.get();
}

/// Consumes the callback allocation passed through Emscripten's asynchronous C callback boundary.
/// @param context Heap-owned std::function<void()> transferred by DispatchToWebUiThread.
void RunWebUiThreadTask(void* context) noexcept {
  std::unique_ptr<std::function<void()>> task(static_cast<std::function<void()>*>(context));
  try {
    (*task)();
  } catch (...) {
  }
}

/// Schedules application work through the browser's asynchronous callback queue.
/// @param task Owned callback transferred to RunWebUiThreadTask; never invoked inline.
void DispatchToWebUiThread(std::function<void()> task) {
  auto pending = std::make_unique<std::function<void()>>(std::move(task));
  emscripten_async_call(RunWebUiThreadTask, pending.get(), 0);
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
          mousePointerId : 1,
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
        const changedButton = (button) => [1, 4, 2, 8, 16][button] || 0;
        const sendPointer = (event, type) => {
          const point = position(event);
          Module._huxerui_web_pointer(session_id, type, event.pointerId, point[0], point[1],
              pointerKind(event.pointerType), type === 0 || type === 1 ? changedButton(event.button) : 0,
              type === 3 ? 0 : event.buttons & 31, !!event.shiftKey, !!event.ctrlKey, !!event.altKey, !!event.metaKey);
        };
        const sendMouseButton = (event, type) => {
          const point = position(event);
          Module._huxerui_web_pointer(session_id, type, session.mousePointerId, point[0], point[1], 0,
              changedButton(event.button), event.buttons & 31, !!event.shiftKey, !!event.ctrlKey, !!event.altKey,
              !!event.metaKey);
        };

        let fileDrag = 0;
        let fileDragSequence = 0;
        let fileDragDepth = 0;
        const leaveFiles = () => {
          if (fileDrag) {
            const previous = fileDrag;
            fileDrag = 0;
            Module._huxerui_web_file_drag(session_id, previous, 2, 0, 0, Emval.toHandle(null));
          }
        };
        const offerFiles = (event) => {
          const transfer = event.dataTransfer;
          const point = position(event);
          const token = platformToken(event.target);
          const allowed = transfer && (transfer.effectAllowed === "all" || transfer.effectAllowed === "uninitialized" ||
              transfer.effectAllowed.toLowerCase().includes("copy"));
          if (!allowed || !Array.from(transfer.types || []).includes("Files") ||
              (token && platformViewWins(token, point))) {
            leaveFiles();
            return false;
          }
          const phase = fileDrag ? 1 : 0;
          if (!fileDrag) {
            fileDrag = ++fileDragSequence;
          }
          return Module._huxerui_web_file_drag(
              session_id, fileDrag, phase, point[0], point[1], Emval.toHandle(transfer)
          );
        };
        listen(root, "dragenter", (event) => {
          ++fileDragDepth;
          if (offerFiles(event)) {
            event.preventDefault();
            event.dataTransfer.dropEffect = "copy";
          }
        });
        listen(root, "dragover", (event) => {
          if (offerFiles(event)) {
            event.preventDefault();
            event.dataTransfer.dropEffect = "copy";
          }
        });
        listen(root, "dragleave", (event) => {
          fileDragDepth = Math.max(0, fileDragDepth - 1);
          if (!fileDragDepth && !(event.relatedTarget instanceof Node && root.contains(event.relatedTarget))) {
            leaveFiles();
          }
        });
        listen(root, "drop", (event) => {
          if (offerFiles(event)) {
            const point = position(event);
            event.preventDefault();
            const accepted = Module._huxerui_web_file_drag(
                session_id, fileDrag, 3, point[0], point[1], Emval.toHandle(event.dataTransfer)
            );
            event.dataTransfer.dropEffect = accepted ? "copy" : "none";
          }
          fileDragDepth = 0;
          leaveFiles();
        });
        listen(window, "dragend", () => { fileDragDepth = 0; leaveFiles(); });
        listen(window, "blur", () => { fileDragDepth = 0; leaveFiles(); });

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
                        if (event.pointerType === "mouse") {
                          session.mousePointerId = event.pointerId;
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
                        if (owner === false) {
                          return;
                        }
                        if (owner === undefined && token && platformViewWins(token, position(event))) {
                          sendPointer(event, 3);
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
        listen(root, "contextmenu", (event) => {
          const point = position(event);
          const token = platformToken(event.target);
          if ((token && platformViewWins(token, point)) ||
              !Module._huxerui_web_context_menu_handler(session_id, point[0], point[1])) {
            return;
          }
          event.preventDefault();
          event.stopPropagation();
        }, true);
        listen(root, "mousedown", (event) => {
          const changed = changedButton(event.button);
          if (session.activePointers.get(session.mousePointerId) !== true ||
              ((event.buttons & 31) & ~changed) === 0) {
            return;
          }
          sendMouseButton(event, 0);
          event.preventDefault();
          event.stopPropagation();
        }, true);
        listen(root, "mouseup", (event) => {
          if (session.activePointers.get(session.mousePointerId) !== true || (event.buttons & 31) === 0) {
            return;
          }
          sendMouseButton(event, 1);
          event.preventDefault();
          event.stopPropagation();
        }, true);
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
                        if (Module._huxerui_web_wheel(
                                session_id, point[0], point[1], event.deltaX * unit, event.deltaY * unit,
                                event.shiftKey, event.ctrlKey, event.altKey, event.metaKey
                            )) {
                          event.preventDefault();
                          event.stopPropagation();
                        }
                      },
            {capture : true, passive : false}
        );

        const keyValues = {
          Backspace : 1,
          Tab : 2,
          Enter : 3,
          Escape : 4,
          Space : 5,
          Insert : 6,
          Delete : 7,
          Home : 8,
          End : 9,
          PageUp : 10,
          PageDown : 11,
          ArrowLeft : 12,
          ArrowRight : 13,
          ArrowUp : 14,
          ArrowDown : 15,
          Backquote : 52,
          Minus : 53,
          Equal : 54,
          BracketLeft : 55,
          BracketRight : 56,
          Backslash : 57,
          Semicolon : 58,
          Quote : 59,
          Comma : 60,
          Period : 61,
          Slash : 62,
          IntlBackslash : 63,
          IntlRo : 64,
          IntlYen : 65,
          ShiftLeft : 66,
          ShiftRight : 67,
          ControlLeft : 68,
          ControlRight : 69,
          AltLeft : 70,
          AltRight : 71,
          MetaLeft : 72,
          MetaRight : 73,
          CapsLock : 74,
          NumLock : 75,
          ScrollLock : 76,
          PrintScreen : 101,
          Pause : 102,
          ContextMenu : 103,
          Help : 104,
          NumpadDecimal : 115,
          NumpadDivide : 116,
          NumpadMultiply : 117,
          NumpadSubtract : 118,
          NumpadAdd : 119,
          NumpadEnter : 120,
          NumpadEqual : 121,
          NumpadComma : 122,
          NumpadClear : 123,
        };
        const keyValue = (code, key) => {
          const upperKey = key.toUpperCase();
          if (upperKey.length === 1) {
            const letter = upperKey.charCodeAt(0) - "A".charCodeAt(0);
            if (letter >= 0 && letter < 26) {
              return 16 + letter;
            }
          }
          if (code.length === 6 && code.startsWith("Digit") && code[5] >= "0" && code[5] <= "9") {
            return 42 + Number(code[5]);
          }
          if (code[0] === "F") {
            const number = Number(code.slice(1));
            if (Number.isInteger(number) && number >= 1 && number <= 24) {
              return 76 + number;
            }
          }
          if (code.length === 7 && code.startsWith("Numpad") && code[6] >= "0" && code[6] <= "9") {
            return 105 + Number(code[6]);
          }
          return keyValues[code] || 0;
        };
        const sendKey = (event, type, includeText = true) => {
          const text =
              includeText && type === 0 && !event.ctrlKey && !event.metaKey && !event.altKey &&
                      Array.from(event.key).length === 1
                  ? event.key
                  : "";
          const altGraph = event.getModifierState && event.getModifierState("AltGraph");
          const textPointer = Module.stringToNewUTF8(text);
          try {
            return Module._huxerui_web_key(
                session_id,
                type,
                keyValue(event.code || "", event.key || ""),
                textPointer,
                event.shiftKey,
                event.ctrlKey && !altGraph,
                event.altKey,
                event.metaKey,
                type === 0 && event.repeat
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
                        if (sendKey(event, 0)) {
                          event.preventDefault();
                          event.stopPropagation();
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
            if (sendKey(event, 1)) {
              event.preventDefault();
              event.stopPropagation();
            }
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
        const updateLifecycle = () => {
          const state = document.hidden ? 2 : document.hasFocus() ? 0 : 1;
          Module._huxerui_web_application_lifecycle(session_id, state);
          if (!document.hidden) {
            resize();
          }
        };
        listen(document, "visibilitychange", updateLifecycle);
        listen(window, "focus", updateLifecycle);
        listen(window, "blur", updateLifecycle);
        updateLifecycle();
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

EM_JS(void, SetWebPointerCursor, (std::uintptr_t session_id, int kind), {
  const sessions = Module.huxerUIWebSessions;
  const session = sessions && sessions.get(session_id);
  if (!session || !session.root.isConnected) {
    return;
  }
  const cursors = [
    "default",
    "text",
    "pointer",
    "crosshair",
    "move",
    "grab",
    "grabbing",
    "ew-resize",
    "ns-resize",
    "nesw-resize",
    "nwse-resize",
    "not-allowed",
    "wait",
  ];
  session.root.style.cursor = cursors[kind] || "default";
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

  [[nodiscard]] std::optional<InputStream> OpenRead(std::string_view package_path) override {
    if (!IsValidResourcePackagePath(package_path)) {
      throw std::logic_error("HuxerUI Web resource path is invalid");
    }
    return OpenPackageFile(std::filesystem::path("/") / std::string(package_path));
  }

private:
  ResourceConfiguration configuration_;
};

ResourceConfiguration BrowserResourceConfiguration();
ApplicationLifecycleState BrowserLifecycleState();
/// Retires attached browser surfaces and removes process lifecycle listeners after shared Runtime shutdown.
void StopWebApplicationHost();

/// Owns browser application services and timers independently of mounted DOM window surfaces.
/// Native facilities are prepared before shared startup and remain alive until Runtime::Retire completes.
class WebRuntime final : public Runtime {
public:
  WebRuntime() : Runtime(DispatchToWebUiThread), resources_(BrowserResourceConfiguration()) {}

  /// Initializes application services with the current browser visibility/focus state on the browser thread.
  void Start() { InitializeApplication(CurrentApplication(), LaunchActivation{}, BrowserLifecycleState()); }

  ~WebRuntime() override {
    Retire();
    for (const auto& [identity, timer] : timers_) {
      emscripten_clear_timeout(identity);
      timer->owner = nullptr;
      timer->callback = {};
    }
  }

  PlatformResources* Resources() noexcept override { return &resources_; }
  std::shared_ptr<HttpTransport> CreateHttpTransport() override { return CreateWebHttpTransport(); }
  std::shared_ptr<PermissionTransport> CreatePermissionTransport() override { return CreateWebPermissionTransport(); }
  std::optional<AppDirectories> CreateAppDirectories() override { return CreateWebAppDirectories(); }

  std::function<void()> ScheduleTimerAt(std::chrono::steady_clock::time_point deadline,
                                        std::function<void()> callback) override {
    auto timer = std::make_shared<Timer>();
    timer->owner = this;
    timer->deadline = deadline;
    timer->callback = std::move(callback);
    Arm(timer);
    return [timer] {
      if (timer->owner) {
        emscripten_clear_timeout(timer->identity);
        timer->owner->timers_.erase(timer->identity);
        timer->owner = nullptr;
        timer->callback = {};
      }
    };
  }

private:
  /// Retained ordinary timer entry shared with its cancellation closure.
  /// owner is cleared before callback delivery or retirement so cancellation cannot target a replacement Runtime.
  struct Timer {
    WebRuntime* owner = nullptr;
    std::chrono::steady_clock::time_point deadline;
    std::function<void()> callback;
    int identity = 0;
  };

  /// Schedules the next browser timeout, splitting deadlines beyond the JavaScript timeout range.
  /// @param timer Live entry owned by this Runtime; its deadline is measured on TimerNow's steady clock.
  /// Called only on the browser thread. Hidden-tab throttling remains subject to browser policy.
  void Arm(const std::shared_ptr<Timer>& timer) {
    const double delay = std::clamp(std::chrono::duration<double, std::milli>(timer->deadline - TimerNow()).count(),
                                    0.0, 2147483647.0);
    timer->identity = emscripten_set_timeout([](void* context) {
      auto* entry = static_cast<Timer*>(context);
      auto timer = entry->owner->timers_.extract(entry->identity).mapped();
      if (timer->owner->TimerNow() < timer->deadline) {
        timer->owner->Arm(timer);
        return;
      }
      timer->owner = nullptr;
      auto callback = std::move(timer->callback);
      try { callback(); } catch (const std::exception& error) {
        emscripten_log(EM_LOG_ERROR, "HuxerUI application timer failed: %s", error.what());
      } catch (...) {
        emscripten_log(EM_LOG_ERROR, "HuxerUI application timer failed with an unknown exception");
      }
    }, delay, timer.get());
    try {
      timers_.emplace(timer->identity, timer);
    } catch (...) {
      emscripten_clear_timeout(timer->identity);
      timer->owner = nullptr;
      throw;
    }
  }

  void OnRuntimeStopped() override { StopWebApplicationHost(); }
  WebResources resources_;
  std::unordered_map<int, std::shared_ptr<Timer>> timers_;
};

/// Reads document visibility and window focus without requiring a HuxerUI surface.
/// @return Background for a hidden document, otherwise Active when focused or Inactive when unfocused.
ApplicationLifecycleState BrowserLifecycleState() {
  const auto document = val::global("document");
  if (document["hidden"].as<bool>()) return ApplicationLifecycleState::Background;
  return document.call<bool>("hasFocus") ? ApplicationLifecycleState::Active : ApplicationLifecycleState::Inactive;
}

/// Accesses the browser-thread owner of the single application Runtime.
/// @return The owning slot, which remains empty before initialization and after host retirement.
std::unique_ptr<WebRuntime>& WebApplication() {
  static std::unique_ptr<WebRuntime> application;
  return application;
}

/// Creates the browser Runtime once and installs process lifecycle listeners after successful startup.
void InitializeWebApplication() {
  if (WebApplication()) return;
  WebApplication() = std::make_unique<WebRuntime>();
  try { WebApplication()->Start(); } catch (...) { WebApplication().reset(); throw; }
  EM_ASM({
    const changed = () => Module._huxerui_web_application_state_changed();
    document.addEventListener("visibilitychange", changed);
    window.addEventListener("focus", changed);
    window.addEventListener("blur", changed);
    Module.huxerUIApplicationLifecycle = () => {
      document.removeEventListener("visibilitychange", changed);
      window.removeEventListener("focus", changed);
      window.removeEventListener("blur", changed);
    };
  });
}

/// Owns one DOM surface and its text/rendering state; deferred frames wait until the JavaScript session is ready.
class WebUiWindow final : public UiWindow {
public:
  WebUiWindow(std::uintptr_t session_id, val host, val root, val canvas, ResourceConfiguration configuration)
      : session_id_(session_id), renderer_(session_id, canvas), configuration_(configuration),
        text_input_(session_id), host_(std::move(host)), root_(std::move(root)), base_canvas_(std::move(canvas)) {
    try {
      InitializeWindow(*WebApplication(), configuration, static_cast<WindowLifecycleState>(BrowserLifecycleState()));
      text_input_.SetUiWindow(this);
      platform_views_ = std::make_unique<WebPlatformViews>(renderer_, PlatformRegistry(), *this, root_, base_canvas_);
    } catch (...) {
      Retire();
      throw;
    }
  }

  ~WebUiWindow() override {
    Shutdown();
    UninstallWebSession(session_id_);
    RemoveWebSurface(root_.as_handle());
  }

  bool Initialize() {
    const WindowOptions& options = CurrentApplication().options.window;
    if (!InstallWebSession(
            session_id_,
            host_.as_handle(),
            root_.as_handle(),
            base_canvas_.as_handle(),
            options.title.c_str()
        )) {
      return false;
    }
    Ready();
    return true;
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
    Retire();
    platform_ready_ = false;
    if (platform_views_) {
      platform_views_->Shutdown();
      platform_views_.reset();
    }
    text_input_.Reset();
  }

  void RequestFrameAt(double deadline) override {
    if (const std::optional<double> scheduled = frame_state_.Request(deadline, Now(), platform_ready_)) {
      Schedule(*scheduled);
    }
  }

  double Now() const noexcept override {
    return WebNow();
  }

  void SetPointerCursor(PointerCursorKind kind) override {
    SetWebPointerCursor(session_id_, static_cast<int>(kind));
  }

  FontMetrics Metrics(const Font& font) override {
    return renderer_.Metrics(font);
  }

  TextRunMetrics
  MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options = {}) override {
    return renderer_.MeasureRun(text, style, options);
  }

  TextLayoutMetrics MeasureText(const huxerui::AttributedText& text, const TextStyle& style, float max_width,
      const TextLayoutOptions& options = {}) override {
    return renderer_.MeasureText(text, style, max_width, options);
  }

  std::unique_ptr<TextLayout> CreateTextLayout(const huxerui::AttributedText& text, const TextStyle& style,
      float max_width, const TextLayoutOptions& options = {}) override {
    return renderer_.CreateTextLayout(text, style, max_width, options);
  }

  PlatformTextInput* TextInput() noexcept override {
    return &text_input_;
  }

  std::shared_ptr<FilePickerTransport> CreateFilePickerTransport() override {
    return CreateWebFilePickerTransport();
  }

  void Resize(float width, float height, float display_scale) {
    const Size viewport{std::max(0.0F, width), std::max(0.0F, height)};
    display_scale = std::max(1.0F, display_scale);
    if (viewport == viewport_ && display_scale == configuration_.display_scale) {
      return;
    }
    viewport_ = viewport;
    renderer_.SetViewport(viewport_, display_scale);
    if (platform_views_) {
      platform_views_->SetViewport(viewport_, display_scale);
    }
    ResourceConfiguration configuration = configuration_;
    configuration.display_scale = display_scale;
    configuration_ = configuration;
    if (IsInitialized()) {
      UiWindow::SetWindowMetrics({.viewport = viewport_});
      UiWindow::UpdateResourceConfiguration(configuration);
    }
  }

  void Frame() {
    if (!IsInitialized() || !frame_state_.BeginCommit()) {
      return;
    }
    const FrameCommit& commit = UiWindow::BuildFrame();
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
    if (IsInitialized()) {
      UiWindow::HandlePointerEvent(event);
    }
  }

  bool HandleFileDrag(std::uint32_t session, int phase, Point position, const val& transfer) {
    if (!IsInitialized()) {
      return false;
    }
    if (phase == 2) {
      UiWindow::HandleFileDragExited(session);
      return false;
    }
    if (transfer.isNull() || transfer.isUndefined()) {
      return false;
    }
    auto offer = ReadWebFileDropOffer(transfer);
    switch (phase) {
    case 0:
      return UiWindow::HandleFileDragEntered(session, std::move(offer), position);
    case 1:
      return UiWindow::HandleFileDragMoved(session, std::move(offer), position);
    case 3:
      return UiWindow::HandleFileDrop(session, std::move(offer), position, CaptureWebFileDrop(transfer));
    default:
      return false;
    }
  }

  bool HandleWheel(ScrollInputEvent event) {
    const Point consumed = IsInitialized() ? UiWindow::HandleScrollInput(event) : Point{};
    return consumed.x != 0.0F || consumed.y != 0.0F;
  }

  bool HandleKey(KeyEvent event) {
    return IsInitialized() && UiWindow::HandleKeyEvent(event);
  }

  void UpdateApplicationLifecycleState(int state) {
    if (!IsInitialized()) {
      return;
    }
    switch (state) {
    case 0:
      UiWindow::UpdateWindowLifecycleState(WindowLifecycleState::Active);
      RequestFrameAt(Now());
      return;
    case 1:
      UiWindow::UpdateWindowLifecycleState(WindowLifecycleState::Inactive);
      RequestFrameAt(Now());
      return;
    case 2:
      UiWindow::UpdateWindowLifecycleState(WindowLifecycleState::Background);
      return;
    default:
      throw std::invalid_argument("HuxerUI Web application lifecycle state is invalid");
    }
  }

  bool PlatformViewHit(std::uint32_t token, Point point) const {
    return platform_views_ && platform_views_->HitTest(token, point);
  }

  bool HasContextMenuHandler(Point point) const {
    return IsInitialized() && UiWindow::HasContextMenuHandler(point);
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
  WebRenderer renderer_;
  ResourceConfiguration configuration_;
  WebTextInput text_input_;
  val host_;
  val root_;
  val base_canvas_;
  std::unique_ptr<WebPlatformViews> platform_views_;
  PlatformFrameState frame_state_;
  Size viewport_;
  bool platform_ready_ = false;
};

template <typename Callback>
void DispatchWebSession(std::uintptr_t session_id, const char* operation, Callback&& callback) noexcept {
  WebUiWindow* session = FindSession(session_id);
  if (session == nullptr) {
    return;
  }
  try {
    callback(*session);
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
    InitializeWebApplication();
    auto session = std::make_unique<WebUiWindow>(
        session_id,
        std::move(host),
        std::move(root),
        std::move(canvas),
        BrowserResourceConfiguration()
    );
    WebUiWindow* inserted = session.get();
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

void StopWebApplicationHost() {
  EM_ASM({
    Module.huxerUIApplicationLifecycle?.();
    delete Module.huxerUIApplicationLifecycle;
  });
  Sessions().clear();
  WebApplication().reset();
}

/// Requests application shutdown; StopWebApplicationHost releases the owner when shared retirement completes.
void ShutdownWebApplication() {
  if (WebApplication()) WebApplication()->RequestShutdown();
}

} // namespace

void EnsureWebPlatformLinked() {}

} // namespace huxerui::detail

extern "C" {

EMSCRIPTEN_KEEPALIVE void huxerui_web_application_state_changed() {
  try {
    if (const auto& application = huxerui::detail::WebApplication()) {
      application->UpdateApplicationLifecycleState(huxerui::detail::BrowserLifecycleState());
    }
  } catch (const std::exception& error) {
    emscripten_log(EM_LOG_ERROR, "HuxerUI application lifecycle failed: %s", error.what());
  }
}

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

EMSCRIPTEN_KEEPALIVE bool huxerui_web_context_menu_handler(std::uintptr_t session_id, float x, float y) {
  bool handled = false;
  huxerui::detail::DispatchWebSession(session_id, "context menu hit test", [&](auto& platform) {
    handled = platform.HasContextMenuHandler({x, y});
  });
  return handled;
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

EMSCRIPTEN_KEEPALIVE void huxerui_web_application_lifecycle(std::uintptr_t session_id, int state) {
  huxerui::detail::DispatchWebSession(session_id, "application lifecycle update", [=](auto& platform) {
    platform.UpdateApplicationLifecycleState(state);
  });
}

EMSCRIPTEN_KEEPALIVE void huxerui_web_image_ready(std::uintptr_t session_id) {
  huxerui::detail::DispatchWebSession(session_id, "image update", [](auto& platform) { platform.ImageReady(); });
}

EMSCRIPTEN_KEEPALIVE void huxerui_web_pointer(std::uintptr_t session_id, int type, std::int32_t pointer_id, float x,
    float y, int device_kind, std::uint32_t changed_button, std::uint32_t pressed_buttons, bool shift, bool control,
    bool alt, bool meta) {
  huxerui::detail::DispatchWebSession(session_id, "pointer input", [=](auto& platform) {
    platform.HandlePointer({
        static_cast<huxerui::PointerEventType>(std::clamp(type, 0, 3)),
        pointer_id,
        {x, y},
        static_cast<huxerui::PointerDeviceKind>(std::clamp(device_kind, 0, 2)),
        static_cast<huxerui::PointerButton>(changed_button & 31U),
        static_cast<huxerui::PointerButton>(pressed_buttons & 31U),
        {shift, control, alt, meta},
    });
  });
}

EMSCRIPTEN_KEEPALIVE bool huxerui_web_wheel(std::uintptr_t session_id, float x, float y, float delta_x, float delta_y,
                                           bool shift, bool control, bool alt, bool meta) {
  bool handled = false;
  huxerui::detail::DispatchWebSession(session_id, "wheel input", [&](auto& platform) {
    handled = platform.HandleWheel({{x, y}, delta_x, delta_y, {shift, control, alt, meta}});
  });
  return handled;
}

EMSCRIPTEN_KEEPALIVE bool huxerui_web_file_drag(std::uintptr_t session_id, std::uint32_t drag, int phase,
                                              float x, float y, emscripten::EM_VAL transfer_handle) {
  const auto transfer = emscripten::val::take_ownership(transfer_handle);
  bool accepted = false;
  huxerui::detail::DispatchWebSession(session_id, "file drop", [&](auto& platform) {
    accepted = platform.HandleFileDrag(drag, phase, {x, y}, transfer);
  });
  return accepted;
}

EMSCRIPTEN_KEEPALIVE bool huxerui_web_key(
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
  bool handled = false;
  huxerui::detail::DispatchWebSession(session_id, "key input", [&](auto& platform) {
    handled = platform.HandleKey({
        static_cast<huxerui::KeyEventType>(std::clamp(type, 0, 1)),
        static_cast<huxerui::Key>(std::clamp(key, 0, static_cast<int>(huxerui::Key::NumpadClear))),
        text == nullptr ? std::string{} : std::string{text},
        {shift, control, alt, meta},
        repeat,
    });
  });
  return handled;
}
}

EMSCRIPTEN_BINDINGS(huxerui_web) {
  emscripten::function("initializeHuxerUI", &huxerui::detail::InitializeWebApplication);
  emscripten::function("shutdownHuxerUI", &huxerui::detail::ShutdownWebApplication);
  emscripten::function("mountHuxerUI", &huxerui::detail::MountWebSession);
  emscripten::function("disposeHuxerUI", &huxerui::detail::DisposeWebSession);
}
