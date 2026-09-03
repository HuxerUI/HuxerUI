#include <huxerui/android/external_texture.h>

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "android_external_texture_internal.h"
#include "android_renderer.h"

namespace huxerui::detail {

namespace {

constexpr std::array<GLfloat, 8> kPositions{
    -1.0F,
    -1.0F,
    1.0F,
    -1.0F,
    -1.0F,
    1.0F,
    1.0F,
    1.0F,
};

constexpr std::array<GLfloat, 16> kIdentityMatrix{
    1.0F,
    0.0F,
    0.0F,
    0.0F,
    0.0F,
    1.0F,
    0.0F,
    0.0F,
    0.0F,
    0.0F,
    1.0F,
    0.0F,
    0.0F,
    0.0F,
    0.0F,
    1.0F,
};

constexpr const char* kVertexShader = R"(
attribute vec2 position;
attribute vec2 textureCoordinate;
uniform mat4 textureMatrix;
varying vec2 sampledCoordinate;

void main() {
  gl_Position = vec4(position, 0.0, 1.0);
  sampledCoordinate = (textureMatrix * vec4(textureCoordinate, 0.0, 1.0)).xy;
}
)";

constexpr const char* kTextureFragmentShader = R"(
precision mediump float;
uniform sampler2D sampledTexture;
uniform int alphaMode;
varying vec2 sampledCoordinate;

void main() {
  vec4 color = texture2D(sampledTexture, sampledCoordinate);
  if (alphaMode == 0) {
    color.a = 1.0;
  } else if (alphaMode == 2) {
    color.rgb *= color.a;
  }
  gl_FragColor = color;
}
)";

constexpr const char* kExternalFragmentShader = R"(
#extension GL_OES_EGL_image_external : require
precision mediump float;
uniform samplerExternalOES sampledTexture;
varying vec2 sampledCoordinate;

void main() {
  vec4 color = texture2D(sampledTexture, sampledCoordinate);
  gl_FragColor = color;
}
)";

[[noreturn]] void ThrowEgl(const char* operation) {
  throw std::runtime_error(
      std::string("HuxerUI Android texture compositor failed to ") + operation + " (EGL error " +
      std::to_string(eglGetError()) + ")"
  );
}

bool ClearPendingJavaException(JNIEnv* environment) noexcept {
  if (environment == nullptr || !environment->ExceptionCheck()) {
    return false;
  }
  environment->ExceptionClear();
  return true;
}

jmethodID
GetRequiredMethod(JNIEnv* environment, jclass type, const char* name, const char* signature, const char* failure) {
  const jmethodID method = environment->GetMethodID(type, name, signature);
  const bool lookup_failed = ClearPendingJavaException(environment);
  if (method == nullptr || lookup_failed) {
    throw std::runtime_error(failure);
  }
  return method;
}

void LogTextureError(const char* message) noexcept {
  __android_log_write(ANDROID_LOG_ERROR, "HuxerUI", message);
}

GLuint CompileShader(GLenum kind, const char* source) {
  const GLuint shader = glCreateShader(kind);
  if (shader == 0) {
    throw std::runtime_error("HuxerUI Android texture compositor could not create a GL shader");
  }
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_TRUE) {
    return shader;
  }
  GLint length = 0;
  glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
  std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
  glGetShaderInfoLog(shader, length, nullptr, log.data());
  glDeleteShader(shader);
  throw std::runtime_error("HuxerUI Android texture compositor shader compilation failed: " + log);
}

GLuint LinkProgram(const char* fragment_source) {
  const GLuint vertex = CompileShader(GL_VERTEX_SHADER, kVertexShader);
  GLuint fragment = 0;
  GLuint program = 0;
  try {
    fragment = CompileShader(GL_FRAGMENT_SHADER, fragment_source);
    program = glCreateProgram();
    if (program == 0) {
      throw std::runtime_error("HuxerUI Android texture compositor could not create a GL program");
    }
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "position");
    glBindAttribLocation(program, 1, "textureCoordinate");
    glLinkProgram(program);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
      GLint length = 0;
      glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
      std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
      glGetProgramInfoLog(program, length, nullptr, log.data());
      throw std::runtime_error("HuxerUI Android texture compositor program link failed: " + log);
    }
  } catch (...) {
    if (program != 0) {
      glDeleteProgram(program);
    }
    if (fragment != 0) {
      glDeleteShader(fragment);
    }
    glDeleteShader(vertex);
    throw;
  }
  glDetachShader(program, vertex);
  glDetachShader(program, fragment);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  return program;
}

void WaitForFence(int& fence_fd) {
  if (fence_fd < 0) {
    return;
  }
  const int owned_fd = std::exchange(fence_fd, -1);
  pollfd descriptor{.fd = owned_fd, .events = POLLIN, .revents = 0};
  int result = 0;
  do {
    result = poll(&descriptor, 1, 5000);
  } while (result < 0 && errno == EINTR);
  close(owned_fd);
  if (result <= 0 || (descriptor.revents & POLLIN) == 0) {
    throw std::runtime_error("HuxerUI Android GL texture acquire fence did not signal");
  }
}

std::atomic<std::uint64_t> next_surface_stream_token = 1;
std::mutex surface_stream_mutex;
std::unordered_map<std::uint64_t, std::function<void()>> surface_stream_callbacks;

std::uint64_t AllocateSurfaceStreamToken() noexcept {
  std::uint64_t token = next_surface_stream_token.fetch_add(1, std::memory_order_relaxed);
  if (token == 0) {
    token = next_surface_stream_token.fetch_add(1, std::memory_order_relaxed);
  }
  return token;
}

void RegisterSurfaceStream(std::uint64_t token, std::function<void()> callback) {
  std::lock_guard lock(surface_stream_mutex);
  surface_stream_callbacks[token] = std::move(callback);
}

void UnregisterSurfaceStream(std::uint64_t token) noexcept {
  std::lock_guard lock(surface_stream_mutex);
  surface_stream_callbacks.erase(token);
}

void DispatchSurfaceStream(std::uint64_t token) noexcept {
  try {
    std::function<void()> callback;
    {
      std::lock_guard lock(surface_stream_mutex);
      const auto found = surface_stream_callbacks.find(token);
      if (found != surface_stream_callbacks.end()) {
        callback = found->second;
      }
    }
    if (callback) {
      callback();
    }
  } catch (const std::exception& error) {
    __android_log_print(ANDROID_LOG_ERROR, "HuxerUI", "Android Surface stream frame callback failed: %s", error.what());
  } catch (...) {
    LogTextureError("Android Surface stream frame callback failed with an unknown error");
  }
}

class OutputSurface final {
public:
  ~OutputSurface();

  OutputSurface(const OutputSurface&) = delete;
  OutputSurface& operator=(const OutputSurface&) = delete;

private:
  struct State;

  explicit OutputSurface(std::unique_ptr<State> state);

  std::unique_ptr<State> state_;

  friend class GpuContext;
};

class SurfaceStream final {
public:
  ~SurfaceStream();

  SurfaceStream(const SurfaceStream&) = delete;
  SurfaceStream& operator=(const SurfaceStream&) = delete;

  [[nodiscard]] android::LocalRef<jobject> NewSurfaceLocalRef(JNIEnv* environment) const;
  void SetDefaultBufferSize(JNIEnv* environment, int pixel_width, int pixel_height);

private:
  struct State;

  explicit SurfaceStream(std::unique_ptr<State> state);

  std::unique_ptr<State> state_;

  friend class GpuContext;
};

class GpuContext final {
public:
  [[nodiscard]] static GpuContext& Instance();

  GpuContext(const GpuContext&) = delete;
  GpuContext& operator=(const GpuContext&) = delete;

  [[nodiscard]] std::shared_ptr<const AndroidGpuFrame> ImportCurrentGlFrame(const android::GlTexture::Frame& frame);
  [[nodiscard]] std::shared_ptr<SurfaceStream>
  CreateSurfaceStream(JNIEnv* environment, std::uint64_t callback_token, int pixel_width, int pixel_height);
  [[nodiscard]] std::shared_ptr<const AndroidGpuFrame>
  LatchSurfaceStream(const std::shared_ptr<SurfaceStream>& stream, int pixel_width, int pixel_height);
  [[nodiscard]] std::shared_ptr<OutputSurface>
  CreateOutputSurface(JNIEnv* environment, jobject surface, int pixel_width, int pixel_height);
  [[nodiscard]] bool Render(
      const std::shared_ptr<OutputSurface>& output, const std::shared_ptr<const AndroidGpuFrame>& frame, Rect source,
      ImageSampling sampling
  );
  void DeleteTexture(unsigned int texture_name) noexcept;

private:
  struct State;

  GpuContext();
  ~GpuContext();

  std::unique_ptr<State> state_;

  friend class OutputSurface;
  friend class SurfaceStream;
};

} // namespace

struct OutputSurface::State {
  ANativeWindow* window = nullptr;
  EGLSurface surface = EGL_NO_SURFACE;
  int pixel_width = 0;
  int pixel_height = 0;
};

struct SurfaceStream::State {
  JavaVM* virtual_machine = nullptr;
  jobject wrapper = nullptr;
  jobject producer_surface = nullptr;
  jfloatArray transform = nullptr;
  jmethodID update_texture = nullptr;
  jmethodID set_default_buffer_size = nullptr;
  jmethodID release = nullptr;
  GLuint external_texture = 0;
};

struct GpuContext::State {
  State() {
    std::promise<void> initialized;
    std::future<void> ready = initialized.get_future();
    worker = std::thread([this, promise = std::move(initialized)]() mutable {
      worker_id = std::this_thread::get_id();
      try {
        Initialize();
        promise.set_value();
      } catch (...) {
        const std::exception_ptr failure = std::current_exception();
        Shutdown();
        promise.set_exception(failure);
        return;
      }
      Run();
      Shutdown();
    });
    try {
      ready.get();
    } catch (...) {
      if (worker.joinable()) {
        worker.join();
      }
      throw;
    }
  }

  ~State() {
    {
      std::lock_guard lock(mutex);
      stopping = true;
    }
    condition.notify_one();
    if (worker.joinable()) {
      worker.join();
    }
  }

  template <class Callback> auto Invoke(Callback&& callback) -> std::invoke_result_t<Callback> {
    using Result = std::invoke_result_t<Callback>;
    if (std::this_thread::get_id() == worker_id) {
      return callback();
    }
    auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Callback>(callback));
    std::future<Result> future = task->get_future();
    {
      std::lock_guard lock(mutex);
      if (stopping) {
        throw std::runtime_error("HuxerUI Android texture compositor is shutting down");
      }
      tasks.emplace_back([task]() { (*task)(); });
    }
    condition.notify_one();
    if constexpr (std::is_void_v<Result>) {
      future.get();
    } else {
      return future.get();
    }
  }

  void Post(std::function<void()> callback) noexcept {
    try {
      {
        std::lock_guard lock(mutex);
        if (stopping) {
          return;
        }
        tasks.push_back(std::move(callback));
      }
      condition.notify_one();
    } catch (...) {
    }
  }

  void Initialize() {
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || eglInitialize(display, nullptr, nullptr) != EGL_TRUE) {
      ThrowEgl("initialize EGL");
    }
    if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
      ThrowEgl("bind the OpenGL ES API");
    }
    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE,
        EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_ALPHA_SIZE,
        8,
        EGL_NONE,
    };
    EGLint config_count = 0;
    if (eglChooseConfig(display, config_attributes, &config, 1, &config_count) != EGL_TRUE || config_count != 1) {
      ThrowEgl("choose an RGBA window configuration");
    }
    if (eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &native_visual_id) != EGL_TRUE ||
        native_visual_id == 0) {
      ThrowEgl("query the native window format");
    }
    const EGLint pbuffer_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    pbuffer = eglCreatePbufferSurface(display, config, pbuffer_attributes);
    if (pbuffer == EGL_NO_SURFACE) {
      ThrowEgl("create its utility surface");
    }
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT) {
      ThrowEgl("create its OpenGL ES context");
    }
    MakeUtilityCurrent();
    texture_program = LinkProgram(kTextureFragmentShader);
    external_program = LinkProgram(kExternalFragmentShader);
    create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    image_target_texture =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  }

  void Run() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock lock(mutex);
        condition.wait(lock, [this]() { return stopping || !tasks.empty(); });
        if (tasks.empty()) {
          if (stopping) {
            return;
          }
          continue;
        }
        task = std::move(tasks.front());
        tasks.pop_front();
      }
      task();
    }
  }

  void Shutdown() noexcept {
    if (display == EGL_NO_DISPLAY) {
      return;
    }
    if (texture_program != 0) {
      glDeleteProgram(texture_program);
    }
    if (external_program != 0) {
      glDeleteProgram(external_program);
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (context != EGL_NO_CONTEXT) {
      eglDestroyContext(display, context);
    }
    if (pbuffer != EGL_NO_SURFACE) {
      eglDestroySurface(display, pbuffer);
    }
    eglTerminate(display);
    if (worker_attached && virtual_machine != nullptr) {
      virtual_machine->DetachCurrentThread();
    }
  }

  void MakeUtilityCurrent() {
    if (eglMakeCurrent(display, pbuffer, pbuffer, context) != EGL_TRUE) {
      ThrowEgl("make its utility context current");
    }
  }

  JNIEnv* Environment(JavaVM* requested_vm) {
    if (requested_vm == nullptr) {
      throw std::invalid_argument("HuxerUI Android texture compositor requires a Java VM");
    }
    if (virtual_machine != nullptr && virtual_machine != requested_vm) {
      throw std::invalid_argument("HuxerUI Android textures must belong to the same Java VM");
    }
    virtual_machine = requested_vm;
    JNIEnv* environment = nullptr;
    const jint result = virtual_machine->GetEnv(reinterpret_cast<void**>(&environment), JNI_VERSION_1_6);
    if (result == JNI_OK) {
      return environment;
    }
    if (result != JNI_EDETACHED || virtual_machine->AttachCurrentThread(&environment, nullptr) != JNI_OK ||
        environment == nullptr) {
      throw std::runtime_error("HuxerUI Android texture compositor could not attach to the Java VM");
    }
    worker_attached = true;
    return environment;
  }

  GLuint CreateTexture(int pixel_width, int pixel_height) {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pixel_width, pixel_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    if (texture == 0 || glGetError() != GL_NO_ERROR) {
      if (texture != 0) {
        glDeleteTextures(1, &texture);
      }
      throw std::runtime_error("HuxerUI Android texture compositor could not allocate an imported frame");
    }
    return texture;
  }

  void Draw(
      GLuint program, GLenum target, GLuint source_texture, const std::array<GLfloat, 8>& coordinates,
      const GLfloat* matrix, int alpha_mode
  ) {
    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(target, source_texture);
    glUniform1i(glGetUniformLocation(program, "sampledTexture"), 0);
    glUniformMatrix4fv(glGetUniformLocation(program, "textureMatrix"), 1, GL_FALSE, matrix);
    const GLint alpha_location = glGetUniformLocation(program, "alphaMode");
    if (alpha_location >= 0) {
      glUniform1i(alpha_location, alpha_mode);
    }
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, kPositions.data());
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, coordinates.data());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindTexture(target, 0);
    glUseProgram(0);
  }

  GLuint CopyToCanonical(
      GLuint source_texture, GLenum source_target, int pixel_width, int pixel_height,
      const std::array<GLfloat, 8>& coordinates, const GLfloat* matrix, int alpha_mode
  ) {
    const GLuint destination = CreateTexture(pixel_width, pixel_height);
    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, destination, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glDeleteFramebuffers(1, &framebuffer);
      glDeleteTextures(1, &destination);
      throw std::runtime_error("HuxerUI Android texture compositor could not create a frame target");
    }
    glViewport(0, 0, pixel_width, pixel_height);
    glDisable(GL_BLEND);
    glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    Draw(
        source_target == GL_TEXTURE_EXTERNAL_OES ? external_program : texture_program, source_target, source_texture,
        coordinates, matrix, alpha_mode
    );
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    if (glGetError() != GL_NO_ERROR) {
      glDeleteTextures(1, &destination);
      throw std::runtime_error("HuxerUI Android texture compositor failed while copying an external frame");
    }
    glFinish();
    return destination;
  }

  std::mutex mutex;
  std::condition_variable condition;
  std::deque<std::function<void()>> tasks;
  std::thread worker;
  std::thread::id worker_id;
  bool stopping = false;
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLConfig config = nullptr;
  EGLint native_visual_id = 0;
  EGLSurface pbuffer = EGL_NO_SURFACE;
  EGLContext context = EGL_NO_CONTEXT;
  GLuint texture_program = 0;
  GLuint external_program = 0;
  PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture = nullptr;
  JavaVM* virtual_machine = nullptr;
  bool worker_attached = false;
};

AndroidGpuFrame::AndroidGpuFrame(GLuint texture_name, int pixel_width, int pixel_height) noexcept
    : texture_name_(texture_name), pixel_width_(pixel_width), pixel_height_(pixel_height) {}

AndroidGpuFrame::~AndroidGpuFrame() {
  if (texture_name_ != 0) {
    GpuContext::Instance().DeleteTexture(texture_name_);
  }
}

AndroidGpuFrame::operator bool() const noexcept {
  return texture_name_ != 0 && pixel_width_ > 0 && pixel_height_ > 0;
}

GLuint AndroidGpuFrame::TextureName() const noexcept {
  return texture_name_;
}

int AndroidGpuFrame::PixelWidth() const noexcept {
  return pixel_width_;
}

int AndroidGpuFrame::PixelHeight() const noexcept {
  return pixel_height_;
}

OutputSurface::OutputSurface(std::unique_ptr<State> state) : state_(std::move(state)) {}

OutputSurface::~OutputSurface() {
  if (!state_) {
    return;
  }
  auto state = std::shared_ptr<State>(state_.release());
  GpuContext::State* service = GpuContext::Instance().state_.get();
  service->Post([service, state]() {
    if (state->surface != EGL_NO_SURFACE) {
      eglDestroySurface(service->display, state->surface);
    }
    if (state->window != nullptr) {
      ANativeWindow_release(state->window);
    }
  });
}

SurfaceStream::SurfaceStream(std::unique_ptr<State> state) : state_(std::move(state)) {}

SurfaceStream::~SurfaceStream() {
  if (!state_) {
    return;
  }
  auto state = std::shared_ptr<State>(state_.release());
  GpuContext::State* service = GpuContext::Instance().state_.get();
  service->Post([service, state]() {
    JNIEnv* environment = nullptr;
    try {
      environment = service->Environment(state->virtual_machine);
    } catch (...) {
    }
    if (environment != nullptr && state->wrapper != nullptr && state->release != nullptr) {
      environment->CallVoidMethod(state->wrapper, state->release);
      if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
      }
    }
    if (state->external_texture != 0) {
      glDeleteTextures(1, &state->external_texture);
    }
    if (environment != nullptr) {
      if (state->transform != nullptr) {
        environment->DeleteGlobalRef(state->transform);
      }
      if (state->producer_surface != nullptr) {
        environment->DeleteGlobalRef(state->producer_surface);
      }
      if (state->wrapper != nullptr) {
        environment->DeleteGlobalRef(state->wrapper);
      }
    }
  });
}

android::LocalRef<jobject> SurfaceStream::NewSurfaceLocalRef(JNIEnv* environment) const {
  if (environment == nullptr) {
    throw std::invalid_argument("HuxerUI Android Surface stream JNI environment must not be null");
  }
  JavaVM* virtual_machine = nullptr;
  if (environment->GetJavaVM(&virtual_machine) != JNI_OK || virtual_machine != state_->virtual_machine) {
    throw std::invalid_argument("HuxerUI Android Surface stream must use its creating Java VM");
  }
  jobject surface = environment->NewLocalRef(state_->producer_surface);
  const bool retain_surface_failed = ClearPendingJavaException(environment);
  if (surface == nullptr || retain_surface_failed) {
    if (surface != nullptr) {
      environment->DeleteLocalRef(surface);
    }
    throw std::runtime_error("HuxerUI could not create a local reference to the Android producer Surface");
  }
  return {environment, surface};
}

void SurfaceStream::SetDefaultBufferSize(JNIEnv* environment, int pixel_width, int pixel_height) {
  if (environment == nullptr) {
    throw std::invalid_argument("HuxerUI Android Surface stream JNI environment must not be null");
  }
  JavaVM* virtual_machine = nullptr;
  if (environment->GetJavaVM(&virtual_machine) != JNI_OK || virtual_machine != state_->virtual_machine) {
    throw std::invalid_argument("HuxerUI Android Surface stream must use its creating Java VM");
  }
  environment->CallVoidMethod(state_->wrapper, state_->set_default_buffer_size, pixel_width, pixel_height);
  if (ClearPendingJavaException(environment)) {
    throw std::runtime_error("HuxerUI could not resize the Android producer Surface");
  }
}

GpuContext& GpuContext::Instance() {
  static GpuContext context;
  return context;
}

GpuContext::GpuContext() : state_(std::make_unique<State>()) {}

GpuContext::~GpuContext() = default;

std::shared_ptr<const AndroidGpuFrame> GpuContext::ImportCurrentGlFrame(const android::GlTexture::Frame& frame) {
  if (frame.texture_name == 0 || frame.pixel_width <= 0 || frame.pixel_height <= 0) {
    throw std::invalid_argument("HuxerUI Android GL texture frame dimensions and texture name must be valid");
  }
  if (frame.acquire_fence_fd < -1) {
    throw std::invalid_argument("HuxerUI Android GL texture acquire fence must be -1 or a valid descriptor");
  }
  if (state_->create_image == nullptr || state_->destroy_image == nullptr || state_->image_target_texture == nullptr) {
    throw std::runtime_error("HuxerUI Android GL texture publication requires EGL image import support");
  }
  switch (frame.origin) {
  case android::GlTexture::Origin::TopLeft:
  case android::GlTexture::Origin::BottomLeft:
    break;
  default:
    throw std::invalid_argument("HuxerUI Android GL texture origin is invalid");
  }
  switch (frame.alpha) {
  case android::GlTexture::Alpha::Opaque:
  case android::GlTexture::Alpha::Premultiplied:
  case android::GlTexture::Alpha::Straight:
    break;
  default:
    throw std::invalid_argument("HuxerUI Android GL texture alpha representation is invalid");
  }
  const EGLDisplay producer_display = eglGetCurrentDisplay();
  const EGLContext producer_context = eglGetCurrentContext();
  if (producer_display == EGL_NO_DISPLAY || producer_context == EGL_NO_CONTEXT) {
    throw std::logic_error("HuxerUI Android GL texture publication requires a current EGL context");
  }
  if (producer_display != state_->display) {
    throw std::invalid_argument("HuxerUI Android GL texture uses an EGLDisplay incompatible with the renderer");
  }
  if (glIsTexture(static_cast<GLuint>(frame.texture_name)) != GL_TRUE) {
    throw std::invalid_argument("HuxerUI Android GL texture name does not identify a current GL texture");
  }

  int fence_fd = -1;
  if (frame.acquire_fence_fd >= 0) {
    fence_fd = fcntl(frame.acquire_fence_fd, F_DUPFD_CLOEXEC, 0);
    if (fence_fd < 0) {
      throw std::runtime_error("HuxerUI could not duplicate the Android GL texture acquire fence");
    }
  } else {
    glFinish();
  }

  const EGLint image_attributes[] = {
      EGL_GL_TEXTURE_LEVEL_KHR,
      0,
      EGL_IMAGE_PRESERVED_KHR,
      EGL_TRUE,
      EGL_NONE,
  };
  const EGLImageKHR image = state_->create_image(
      producer_display, producer_context, EGL_GL_TEXTURE_2D_KHR,
      reinterpret_cast<EGLClientBuffer>(static_cast<std::uintptr_t>(frame.texture_name)), image_attributes
  );
  if (image == EGL_NO_IMAGE_KHR) {
    if (fence_fd >= 0) {
      close(fence_fd);
    }
    ThrowEgl("capture a producer GL texture");
  }

  GLuint canonical = 0;
  try {
    canonical = state_->Invoke([this, image, &fence_fd, &frame]() {
      WaitForFence(fence_fd);
      GLuint imported = 0;
      glGenTextures(1, &imported);
      glBindTexture(GL_TEXTURE_2D, imported);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      state_->image_target_texture(GL_TEXTURE_2D, image);
      const std::array<GLfloat, 8> coordinates = frame.origin == android::GlTexture::Origin::BottomLeft
                                                     ? std::array<GLfloat, 8>{0, 0, 1, 0, 0, 1, 1, 1}
                                                     : std::array<GLfloat, 8>{0, 1, 1, 1, 0, 0, 1, 0};
      const int alpha_mode = frame.alpha == android::GlTexture::Alpha::Opaque
                                 ? 0
                                 : (frame.alpha == android::GlTexture::Alpha::Premultiplied ? 1 : 2);
      GLuint result = 0;
      try {
        result = state_->CopyToCanonical(
            imported, GL_TEXTURE_2D, frame.pixel_width, frame.pixel_height, coordinates, kIdentityMatrix.data(),
            alpha_mode
        );
      } catch (...) {
        glDeleteTextures(1, &imported);
        throw;
      }
      glDeleteTextures(1, &imported);
      return result;
    });
  } catch (...) {
    if (fence_fd >= 0) {
      close(fence_fd);
    }
    state_->destroy_image(producer_display, image);
    throw;
  }

  if (state_->destroy_image(producer_display, image) != EGL_TRUE) {
    DeleteTexture(canonical);
    ThrowEgl("release a captured producer GL texture");
  }
  return std::make_shared<AndroidGpuFrame>(canonical, frame.pixel_width, frame.pixel_height);
}

std::shared_ptr<SurfaceStream>
GpuContext::CreateSurfaceStream(JNIEnv* environment, std::uint64_t callback_token, int pixel_width, int pixel_height) {
  if (environment == nullptr || pixel_width <= 0 || pixel_height <= 0) {
    throw std::invalid_argument("HuxerUI Android Surface stream arguments must be valid");
  }
  JavaVM* virtual_machine = nullptr;
  if (environment->GetJavaVM(&virtual_machine) != JNI_OK || virtual_machine == nullptr) {
    throw std::runtime_error("HuxerUI could not access the Java VM for an Android Surface stream");
  }
  jclass local_class = environment->FindClass("org/huxerui/HuxerUISurfaceStream");
  if (local_class == nullptr) {
    ClearPendingJavaException(environment);
    throw std::runtime_error("HuxerUI could not resolve its Android Surface stream bridge");
  }
  jclass stream_class = static_cast<jclass>(environment->NewGlobalRef(local_class));
  environment->DeleteLocalRef(local_class);
  const bool retain_class_failed = ClearPendingJavaException(environment);
  if (stream_class == nullptr || retain_class_failed) {
    if (stream_class != nullptr) {
      environment->DeleteGlobalRef(stream_class);
    }
    throw std::runtime_error("HuxerUI could not retain its Android Surface stream bridge");
  }
  constexpr const char* method_failure =
      "HuxerUI Android Surface stream bridge methods do not match the platform backend";
  jmethodID constructor = nullptr;
  jmethodID producer_surface = nullptr;
  jmethodID update_texture = nullptr;
  jmethodID set_default_buffer_size = nullptr;
  jmethodID release = nullptr;
  try {
    constructor = GetRequiredMethod(environment, stream_class, "<init>", "(IJII)V", method_failure);
    producer_surface =
        GetRequiredMethod(environment, stream_class, "producerSurface", "()Landroid/view/Surface;", method_failure);
    update_texture = GetRequiredMethod(environment, stream_class, "updateTexture", "([F)V", method_failure);
    set_default_buffer_size =
        GetRequiredMethod(environment, stream_class, "setDefaultBufferSize", "(II)V", method_failure);
    release = GetRequiredMethod(environment, stream_class, "release", "()V", method_failure);
  } catch (...) {
    environment->DeleteGlobalRef(stream_class);
    throw;
  }

  try {
    std::shared_ptr<SurfaceStream> result = state_->Invoke([=, this]() {
      JNIEnv* worker_environment = state_->Environment(virtual_machine);
      GLuint texture = 0;
      glGenTextures(1, &texture);
      glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
      glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      jobject local_wrapper = worker_environment->NewObject(
          stream_class, constructor, static_cast<jint>(texture), static_cast<jlong>(callback_token), pixel_width,
          pixel_height
      );
      if (local_wrapper == nullptr || worker_environment->ExceptionCheck()) {
        if (worker_environment->ExceptionCheck()) {
          worker_environment->ExceptionClear();
        }
        glDeleteTextures(1, &texture);
        throw std::runtime_error("HuxerUI could not create its Android Surface stream bridge");
      }
      jobject local_surface = worker_environment->CallObjectMethod(local_wrapper, producer_surface);
      if (local_surface == nullptr || worker_environment->ExceptionCheck()) {
        if (worker_environment->ExceptionCheck()) {
          worker_environment->ExceptionClear();
        }
        worker_environment->CallVoidMethod(local_wrapper, release);
        if (worker_environment->ExceptionCheck()) {
          worker_environment->ExceptionClear();
        }
        worker_environment->DeleteLocalRef(local_wrapper);
        glDeleteTextures(1, &texture);
        throw std::runtime_error("HuxerUI could not create the Android producer Surface");
      }
      auto stream_state = std::make_unique<SurfaceStream::State>();
      stream_state->virtual_machine = virtual_machine;
      stream_state->update_texture = update_texture;
      stream_state->set_default_buffer_size = set_default_buffer_size;
      stream_state->release = release;
      stream_state->external_texture = texture;
      jfloatArray local_transform = nullptr;
      const auto release_resources = [&]() noexcept {
        if (worker_environment->ExceptionCheck()) {
          worker_environment->ExceptionClear();
        }
        worker_environment->CallVoidMethod(local_wrapper, release);
        if (worker_environment->ExceptionCheck()) {
          worker_environment->ExceptionClear();
        }
        if (stream_state->transform != nullptr) {
          worker_environment->DeleteGlobalRef(stream_state->transform);
        }
        if (stream_state->producer_surface != nullptr) {
          worker_environment->DeleteGlobalRef(stream_state->producer_surface);
        }
        if (stream_state->wrapper != nullptr) {
          worker_environment->DeleteGlobalRef(stream_state->wrapper);
        }
        if (local_transform != nullptr) {
          worker_environment->DeleteLocalRef(local_transform);
        }
        worker_environment->DeleteLocalRef(local_surface);
        worker_environment->DeleteLocalRef(local_wrapper);
        glDeleteTextures(1, &texture);
      };
      stream_state->wrapper = worker_environment->NewGlobalRef(local_wrapper);
      if (stream_state->wrapper == nullptr || worker_environment->ExceptionCheck()) {
        release_resources();
        throw std::runtime_error("HuxerUI could not retain its Android Surface stream resources");
      }
      stream_state->producer_surface = worker_environment->NewGlobalRef(local_surface);
      if (stream_state->producer_surface == nullptr || worker_environment->ExceptionCheck()) {
        release_resources();
        throw std::runtime_error("HuxerUI could not retain its Android Surface stream resources");
      }
      local_transform = worker_environment->NewFloatArray(16);
      if (local_transform == nullptr || worker_environment->ExceptionCheck()) {
        release_resources();
        throw std::runtime_error("HuxerUI could not retain its Android Surface stream resources");
      }
      stream_state->transform = static_cast<jfloatArray>(worker_environment->NewGlobalRef(local_transform));
      if (stream_state->transform == nullptr || worker_environment->ExceptionCheck()) {
        release_resources();
        throw std::runtime_error("HuxerUI could not retain its Android Surface stream resources");
      }
      worker_environment->DeleteLocalRef(local_transform);
      worker_environment->DeleteLocalRef(local_surface);
      worker_environment->DeleteLocalRef(local_wrapper);
      return std::shared_ptr<SurfaceStream>(new SurfaceStream(std::move(stream_state)));
    });
    environment->DeleteGlobalRef(stream_class);
    return result;
  } catch (...) {
    if (stream_class != nullptr) {
      environment->DeleteGlobalRef(stream_class);
    }
    throw;
  }
}

std::shared_ptr<const AndroidGpuFrame>
GpuContext::LatchSurfaceStream(const std::shared_ptr<SurfaceStream>& stream, int pixel_width, int pixel_height) {
  if (!stream || pixel_width <= 0 || pixel_height <= 0) {
    throw std::invalid_argument("HuxerUI Android Surface stream frame arguments must be valid");
  }
  return state_->Invoke([this, stream, pixel_width, pixel_height]() {
    SurfaceStream::State& source = *stream->state_;
    JNIEnv* environment = state_->Environment(source.virtual_machine);
    environment->CallVoidMethod(source.wrapper, source.update_texture, source.transform);
    if (environment->ExceptionCheck()) {
      environment->ExceptionClear();
      throw std::runtime_error("HuxerUI could not latch an Android Surface stream frame");
    }
    std::array<GLfloat, 16> transform{};
    environment->GetFloatArrayRegion(source.transform, 0, 16, transform.data());
    if (environment->ExceptionCheck()) {
      environment->ExceptionClear();
      throw std::runtime_error("HuxerUI could not read an Android Surface stream transform");
    }
    const std::array<GLfloat, 8> coordinates{0, 0, 1, 0, 0, 1, 1, 1};
    const GLuint texture = state_->CopyToCanonical(
        source.external_texture, GL_TEXTURE_EXTERNAL_OES, pixel_width, pixel_height, coordinates, transform.data(), 0
    );
    return std::make_shared<AndroidGpuFrame>(texture, pixel_width, pixel_height);
  });
}

std::shared_ptr<OutputSurface>
GpuContext::CreateOutputSurface(JNIEnv* environment, jobject surface, int pixel_width, int pixel_height) {
  if (environment == nullptr || surface == nullptr || pixel_width <= 0 || pixel_height <= 0) {
    throw std::invalid_argument("HuxerUI Android texture output Surface arguments must be valid");
  }
  ANativeWindow* window = ANativeWindow_fromSurface(environment, surface);
  const bool native_window_failed = ClearPendingJavaException(environment);
  if (window == nullptr || native_window_failed) {
    if (window != nullptr) {
      ANativeWindow_release(window);
    }
    throw std::runtime_error("HuxerUI could not obtain an Android native window for a texture layer");
  }
  try {
    return state_->Invoke([this, window, pixel_width, pixel_height]() {
      if (ANativeWindow_setBuffersGeometry(window, 0, 0, state_->native_visual_id) != 0) {
        throw std::runtime_error("HuxerUI could not configure an Android native window for a texture layer");
      }
      const EGLint attributes[] = {EGL_NONE};
      const EGLSurface surface = eglCreateWindowSurface(state_->display, state_->config, window, attributes);
      if (surface == EGL_NO_SURFACE) {
        ThrowEgl("create a TextureView output surface");
      }
      auto output_state = std::make_unique<OutputSurface::State>();
      output_state->window = window;
      output_state->surface = surface;
      output_state->pixel_width = pixel_width;
      output_state->pixel_height = pixel_height;
      return std::shared_ptr<OutputSurface>(new OutputSurface(std::move(output_state)));
    });
  } catch (...) {
    ANativeWindow_release(window);
    throw;
  }
}

bool GpuContext::Render(
    const std::shared_ptr<OutputSurface>& output, const std::shared_ptr<const AndroidGpuFrame>& frame, Rect source,
    ImageSampling sampling
) {
  if (!output || !frame || !*frame || source.IsEmpty()) {
    return false;
  }
  return state_->Invoke([this, output, frame, source, sampling]() {
    OutputSurface::State& target = *output->state_;
    if (target.surface == EGL_NO_SURFACE ||
        eglMakeCurrent(state_->display, target.surface, target.surface, state_->context) != EGL_TRUE) {
      state_->MakeUtilityCurrent();
      return false;
    }
    EGLint width = target.pixel_width;
    EGLint height = target.pixel_height;
    static_cast<void>(eglQuerySurface(state_->display, target.surface, EGL_WIDTH, &width));
    static_cast<void>(eglQuerySurface(state_->display, target.surface, EGL_HEIGHT, &height));
    if (width <= 0 || height <= 0) {
      state_->MakeUtilityCurrent();
      return false;
    }
    const float frame_width = static_cast<float>(frame->PixelWidth());
    const float frame_height = static_cast<float>(frame->PixelHeight());
    const float left = source.x / frame_width;
    const float right = (source.x + source.width) / frame_width;
    const float top = 1.0F - source.y / frame_height;
    const float bottom = 1.0F - (source.y + source.height) / frame_height;
    const std::array<GLfloat, 8> coordinates{left, bottom, right, bottom, left, top, right, top};
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindTexture(GL_TEXTURE_2D, frame->TextureName());
    const GLint filter = sampling == ImageSampling::Nearest ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    state_->Draw(state_->texture_program, GL_TEXTURE_2D, frame->TextureName(), coordinates, kIdentityMatrix.data(), 1);
    const bool presented = eglSwapBuffers(state_->display, target.surface) == EGL_TRUE;
    state_->MakeUtilityCurrent();
    return presented;
  });
}

void GpuContext::DeleteTexture(unsigned int texture_name) noexcept {
  if (texture_name == 0) {
    return;
  }
  state_->Post([texture_name]() {
    const GLuint texture = texture_name;
    glDeleteTextures(1, &texture);
  });
}

namespace {

struct TextureLayerDescriptor {
  AndroidTextureLayerKey key;
  float width = 0.0F;
  float height = 0.0F;
  bool drawable = false;
};

struct AndroidTextureLayerKeyHash {
  std::size_t operator()(const AndroidTextureLayerKey& key) const noexcept {
    std::size_t result = std::hash<std::uint64_t>{}(key.node_identity);
    const auto combine = [&result](std::size_t value) {
      result ^= value + 0x9e3779b9U + (result << 6U) + (result >> 2U);
    };
    combine(std::hash<std::size_t>{}(key.texture_ordinal));
    combine(std::hash<bool>{}(key.foreground));
    return result;
  }
};

bool IsGpuTexture(const std::shared_ptr<ExternalTexture>& texture) {
  return std::dynamic_pointer_cast<android::GlTexture>(texture) != nullptr ||
         std::dynamic_pointer_cast<android::SurfaceStreamTexture>(texture) != nullptr;
}

void CollectSequence(
    const PaintSequence& sequence, std::uint64_t node_identity, bool foreground, bool drawable,
    std::vector<TextureLayerDescriptor>& descriptors
) {
  std::size_t texture_ordinal = 0;
  for (const PaintCommand& command : sequence.Commands()) {
    const auto* texture = std::get_if<DrawExternalTextureCommand>(&command);
    if (texture == nullptr) {
      continue;
    }
    const AndroidTextureLayerKey key{
        .node_identity = node_identity,
        .texture_ordinal = texture_ordinal,
        .foreground = foreground,
    };
    ++texture_ordinal;
    if (IsGpuTexture(texture->texture)) {
      descriptors.push_back({
          .key = key,
          .width = std::max(0.0F, texture->destination.width),
          .height = std::max(0.0F, texture->destination.height),
          .drawable = drawable,
      });
    }
  }
}

void CollectNode(const RenderNode& node, bool parent_drawable, std::vector<TextureLayerDescriptor>& descriptors) {
  const bool drawable = parent_drawable && node.visible && std::clamp(node.opacity, 0.0F, 1.0F) > 0.0F;
  CollectSequence(node.content, node.id, false, drawable, descriptors);
  for (const RenderNode* child : node.children) {
    if (child != nullptr) {
      CollectNode(*child, drawable, descriptors);
    }
  }
  CollectSequence(node.foreground, node.id, true, drawable, descriptors);
}

} // namespace

struct AndroidTextureLayers::State {
  struct LayerSlot {
    void ResetPresentation() noexcept {
      presented_texture.reset();
      presented_frame.reset();
      presented_source = {};
      presented_sampling = ImageSampling::Linear;
      has_presentation = false;
    }

    std::uint64_t java_identity = 0;
    std::uint64_t seen_generation = 0;
    std::shared_ptr<OutputSurface> output;
    std::weak_ptr<ExternalTexture> presented_texture;
    std::shared_ptr<const AndroidGpuFrame> presented_frame;
    Rect presented_source;
    ImageSampling presented_sampling = ImageSampling::Linear;
    bool has_presentation = false;
  };

  jobject root = nullptr;
  AndroidRenderer* renderer = nullptr;
  jmethodID commit_layers = nullptr;
  jmethodID draw_layer = nullptr;
  std::unordered_map<AndroidTextureLayerKey, LayerSlot, AndroidTextureLayerKeyHash> layers;
  std::uint64_t next_java_identity = 1;
  std::uint64_t commit_generation = 0;
  bool shutdown = false;

  std::uint64_t BeginCommit() noexcept {
    ++commit_generation;
    if (commit_generation != 0) {
      return commit_generation;
    }
    for (auto& entry : layers) {
      entry.second.seen_generation = 0;
    }
    commit_generation = 1;
    return commit_generation;
  }

  std::uint64_t AllocateJavaIdentity() {
    if (next_java_identity > static_cast<std::uint64_t>(std::numeric_limits<jlong>::max())) {
      throw std::overflow_error("HuxerUI Android texture layer identity exceeds the JNI range");
    }
    return next_java_identity++;
  }

  LayerSlot* FindLayerByJavaIdentity(std::uint64_t identity) noexcept {
    for (auto& entry : layers) {
      if (entry.second.java_identity == identity) {
        return &entry.second;
      }
    }
    return nullptr;
  }
};

AndroidTextureLayers::AndroidTextureLayers(JNIEnv* environment, jobject root, AndroidRenderer& renderer)
    : state_(std::make_unique<State>()) {
  state_->root = root;
  state_->renderer = &renderer;
  jclass root_class = environment->GetObjectClass(root);
  if (root_class == nullptr) {
    ClearPendingJavaException(environment);
    throw std::runtime_error("HuxerUI could not inspect its Android texture layer host");
  }
  constexpr const char* method_failure = "HuxerUI Android texture layer host methods do not match the platform backend";
  try {
    state_->commit_layers =
        GetRequiredMethod(environment, root_class, "commitTextureLayers", "([J[F)V", method_failure);
    state_->draw_layer = GetRequiredMethod(
        environment, root_class, "drawTextureLayer", "(Landroid/graphics/Canvas;JFFFFF)V", method_failure
    );
  } catch (...) {
    environment->DeleteLocalRef(root_class);
    throw;
  }
  environment->DeleteLocalRef(root_class);
  state_->renderer->SetTextureLayers(this);
}

AndroidTextureLayers::~AndroidTextureLayers() {
  if (state_ && state_->renderer != nullptr) {
    state_->renderer->SetTextureLayers(nullptr);
  }
}

void AndroidTextureLayers::Commit(JNIEnv* environment, const RenderFrame& frame) {
  std::vector<TextureLayerDescriptor> descriptors;
  if (frame.scene.root != nullptr) {
    CollectNode(*frame.scene.root, true, descriptors);
  }

  std::vector<jlong> identities;
  std::vector<jfloat> sizes;
  identities.reserve(descriptors.size());
  sizes.reserve(descriptors.size() * 2);
  const std::uint64_t generation = state_->BeginCommit();
  for (const TextureLayerDescriptor& descriptor : descriptors) {
    auto [found, inserted] = state_->layers.try_emplace(descriptor.key);
    static_cast<void>(inserted);
    State::LayerSlot& layer = found->second;
    layer.seen_generation = generation;
    if (!descriptor.drawable) {
      layer.presented_frame.reset();
    }
    if (descriptor.drawable && layer.java_identity == 0) {
      layer.java_identity = state_->AllocateJavaIdentity();
    }
    if (layer.java_identity == 0) {
      continue;
    }
    identities.push_back(static_cast<jlong>(layer.java_identity));
    sizes.push_back(descriptor.width);
    sizes.push_back(descriptor.height);
  }
  std::erase_if(state_->layers, [generation](const auto& entry) { return entry.second.seen_generation != generation; });

  if (identities.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max())) {
    throw std::overflow_error("HuxerUI Android texture layer descriptor exceeds the JNI array range");
  }

  jlongArray identity_array = environment->NewLongArray(static_cast<jsize>(identities.size()));
  const bool allocate_identities_failed = ClearPendingJavaException(environment);
  if (identity_array == nullptr || allocate_identities_failed) {
    if (identity_array != nullptr) {
      environment->DeleteLocalRef(identity_array);
    }
    throw std::runtime_error("HuxerUI could not allocate the Android texture layer descriptor");
  }
  jfloatArray size_array = environment->NewFloatArray(static_cast<jsize>(sizes.size()));
  const bool allocate_sizes_failed = ClearPendingJavaException(environment);
  if (size_array == nullptr || allocate_sizes_failed) {
    if (size_array != nullptr) {
      environment->DeleteLocalRef(size_array);
    }
    environment->DeleteLocalRef(identity_array);
    throw std::runtime_error("HuxerUI could not allocate the Android texture layer descriptor");
  }
  if (!identities.empty()) {
    environment->SetLongArrayRegion(identity_array, 0, static_cast<jsize>(identities.size()), identities.data());
    if (ClearPendingJavaException(environment)) {
      environment->DeleteLocalRef(size_array);
      environment->DeleteLocalRef(identity_array);
      throw std::runtime_error("HuxerUI could not populate the Android texture layer descriptor");
    }
    environment->SetFloatArrayRegion(size_array, 0, static_cast<jsize>(sizes.size()), sizes.data());
    if (ClearPendingJavaException(environment)) {
      environment->DeleteLocalRef(size_array);
      environment->DeleteLocalRef(identity_array);
      throw std::runtime_error("HuxerUI could not populate the Android texture layer descriptor");
    }
  }
  environment->CallVoidMethod(state_->root, state_->commit_layers, identity_array, size_array);
  const bool commit_failed = ClearPendingJavaException(environment);
  environment->DeleteLocalRef(size_array);
  environment->DeleteLocalRef(identity_array);
  if (commit_failed) {
    throw std::logic_error("HuxerUI Android texture layer host failed to commit its children");
  }
}

void AndroidTextureLayers::Draw(
    JNIEnv* environment, jobject canvas, const AndroidTextureLayerKey& key, const DrawExternalTextureCommand& command,
    const std::shared_ptr<const AndroidGpuFrame>& frame
) {
  const auto found = state_->layers.find(key);
  if (found == state_->layers.end() || found->second.java_identity == 0) {
    throw std::logic_error("HuxerUI Android texture layer is missing from the committed frame");
  }
  State::LayerSlot& layer = found->second;
  if (layer.has_presentation && layer.presented_texture.lock() != command.texture) {
    layer.ResetPresentation();
  }
  if (frame && layer.output) {
    const Size intrinsic_size = command.texture->IntrinsicSize();
    const float scale_x = static_cast<float>(frame->PixelWidth()) / intrinsic_size.width;
    const float scale_y = static_cast<float>(frame->PixelHeight()) / intrinsic_size.height;
    const Rect physical_source{
        command.source.x * scale_x,
        command.source.y * scale_y,
        command.source.width * scale_x,
        command.source.height * scale_y,
    };
    const bool is_presented = layer.has_presentation && layer.presented_texture.lock() == command.texture &&
                              layer.presented_frame == frame && layer.presented_source == physical_source &&
                              layer.presented_sampling == command.sampling;
    if (!is_presented) {
      if (!GpuContext::Instance().Render(layer.output, frame, physical_source, command.sampling)) {
        LogTextureError("Android texture layer failed to present its current frame");
      } else {
        layer.presented_texture = command.texture;
        layer.presented_frame = frame;
        layer.presented_source = physical_source;
        layer.presented_sampling = command.sampling;
        layer.has_presentation = true;
      }
    }
  }
  if (!layer.has_presentation) {
    return;
  }
  environment->CallVoidMethod(
      state_->root, state_->draw_layer, canvas, static_cast<jlong>(layer.java_identity), command.destination.x,
      command.destination.y, command.destination.width, command.destination.height, command.opacity
  );
  if (ClearPendingJavaException(environment)) {
    throw std::logic_error("HuxerUI Android texture layer host failed to draw its child");
  }
}

void AndroidTextureLayers::SetSurface(
    JNIEnv* environment, std::uint64_t identity, jobject surface, int pixel_width, int pixel_height
) {
  State::LayerSlot* layer = state_->FindLayerByJavaIdentity(identity);
  if (layer == nullptr) {
    return;
  }
  layer->output.reset();
  layer->ResetPresentation();
  layer->output = GpuContext::Instance().CreateOutputSurface(environment, surface, pixel_width, pixel_height);
}

void AndroidTextureLayers::ClearSurface(std::uint64_t identity) noexcept {
  State::LayerSlot* layer = state_->FindLayerByJavaIdentity(identity);
  if (layer != nullptr) {
    layer->output.reset();
    layer->ResetPresentation();
  }
}

void AndroidTextureLayers::Shutdown(JNIEnv* environment) noexcept {
  if (!state_ || state_->shutdown) {
    return;
  }
  state_->shutdown = true;
  state_->layers.clear();
  jlongArray identities = nullptr;
  jfloatArray sizes = nullptr;
  if (environment != nullptr) {
    identities = environment->NewLongArray(0);
    if (ClearPendingJavaException(environment)) {
      if (identities != nullptr) {
        environment->DeleteLocalRef(identities);
        identities = nullptr;
      }
    } else if (identities != nullptr) {
      sizes = environment->NewFloatArray(0);
      if (ClearPendingJavaException(environment)) {
        if (sizes != nullptr) {
          environment->DeleteLocalRef(sizes);
          sizes = nullptr;
        }
      } else if (sizes != nullptr) {
        environment->CallVoidMethod(state_->root, state_->commit_layers, identities, sizes);
        ClearPendingJavaException(environment);
      }
    }
  }
  if (environment != nullptr && sizes != nullptr) {
    environment->DeleteLocalRef(sizes);
  }
  if (environment != nullptr && identities != nullptr) {
    environment->DeleteLocalRef(identities);
  }
  if (state_->renderer != nullptr) {
    state_->renderer->SetTextureLayers(nullptr);
    state_->renderer = nullptr;
  }
}

} // namespace huxerui::detail

namespace huxerui::android {

struct GlTexture::Storage {
  std::mutex publish_mutex;
  std::mutex frame_mutex;
  std::shared_ptr<const huxerui::detail::AndroidGpuFrame> frame;
  bool finished = false;
};

GlTexture::GlTexture(Size intrinsic_size)
    : huxerui::ExternalTexture(intrinsic_size), storage_(std::make_unique<Storage>()) {}

GlTexture::~GlTexture() {
  std::unique_lock publish_lock(storage_->publish_mutex);
  std::shared_ptr<const huxerui::detail::AndroidGpuFrame> frame;
  {
    std::lock_guard frame_lock(storage_->frame_mutex);
    storage_->finished = true;
    frame = std::move(storage_->frame);
  }
  publish_lock.unlock();
  frame.reset();
}

void GlTexture::PublishCurrent(Frame frame) {
  std::unique_lock publish_lock(storage_->publish_mutex);
  {
    std::lock_guard frame_lock(storage_->frame_mutex);
    if (storage_->finished) {
      throw std::logic_error("HuxerUI Android GL texture is finished");
    }
  }
  std::shared_ptr<const huxerui::detail::AndroidGpuFrame> imported =
      huxerui::detail::GpuContext::Instance().ImportCurrentGlFrame(frame);
  std::shared_ptr<const huxerui::detail::AndroidGpuFrame> replaced;
  {
    std::lock_guard frame_lock(storage_->frame_mutex);
    replaced = std::move(storage_->frame);
    storage_->frame = std::move(imported);
  }
  publish_lock.unlock();
  NotifyFrameAvailable();
  replaced.reset();
}

void GlTexture::Finish() noexcept {
  std::lock_guard publish_lock(storage_->publish_mutex);
  std::lock_guard frame_lock(storage_->frame_mutex);
  storage_->finished = true;
}

std::shared_ptr<const huxerui::detail::AndroidGpuFrame> GlTexture::AcquireFrame() const noexcept {
  std::lock_guard lock(storage_->frame_mutex);
  return storage_->frame;
}

struct SurfaceStreamTexture::Storage {
  std::mutex mutex;
  std::shared_ptr<huxerui::detail::SurfaceStream> stream;
  std::shared_ptr<const huxerui::detail::AndroidGpuFrame> frame;
  std::uint64_t callback_token = 0;
  int pixel_width = 0;
  int pixel_height = 0;
  bool finished = false;
};

std::shared_ptr<SurfaceStreamTexture>
SurfaceStreamTexture::Create(JNIEnv* environment, Size intrinsic_size, int pixel_width, int pixel_height) {
  if (environment == nullptr) {
    throw std::invalid_argument("HuxerUI Android Surface stream JNI environment must not be null");
  }
  if (pixel_width <= 0 || pixel_height <= 0) {
    throw std::invalid_argument("HuxerUI Android Surface stream pixel dimensions must be positive");
  }
  const std::uint64_t callback_token = huxerui::detail::AllocateSurfaceStreamToken();
  std::shared_ptr<huxerui::detail::SurfaceStream> stream = huxerui::detail::GpuContext::Instance().CreateSurfaceStream(
      environment, callback_token, pixel_width, pixel_height
  );
  auto storage = std::make_unique<Storage>();
  storage->stream = std::move(stream);
  storage->callback_token = callback_token;
  storage->pixel_width = pixel_width;
  storage->pixel_height = pixel_height;
  std::shared_ptr<SurfaceStreamTexture> texture(new SurfaceStreamTexture(intrinsic_size, std::move(storage)));
  huxerui::detail::RegisterSurfaceStream(callback_token, [weak = std::weak_ptr<SurfaceStreamTexture>(texture)] {
    if (const std::shared_ptr<SurfaceStreamTexture> retained = weak.lock()) {
      retained->FrameAvailable();
    }
  });
  return texture;
}

SurfaceStreamTexture::SurfaceStreamTexture(Size intrinsic_size, std::unique_ptr<Storage> storage)
    : huxerui::ExternalTexture(intrinsic_size), storage_(std::move(storage)) {}

SurfaceStreamTexture::~SurfaceStreamTexture() {
  Finish();
  std::shared_ptr<const huxerui::detail::AndroidGpuFrame> frame;
  {
    std::lock_guard lock(storage_->mutex);
    frame = std::move(storage_->frame);
  }
  frame.reset();
}

LocalRef<jobject> SurfaceStreamTexture::Surface(JNIEnv* environment) const {
  std::shared_ptr<huxerui::detail::SurfaceStream> stream;
  {
    std::lock_guard lock(storage_->mutex);
    stream = storage_->stream;
  }
  if (!stream) {
    throw std::logic_error("HuxerUI Android Surface stream is unavailable");
  }
  return stream->NewSurfaceLocalRef(environment);
}

void SurfaceStreamTexture::SetDefaultBufferSize(JNIEnv* environment, int pixel_width, int pixel_height) {
  if (pixel_width <= 0 || pixel_height <= 0) {
    throw std::invalid_argument("HuxerUI Android Surface stream pixel dimensions must be positive");
  }
  std::lock_guard lock(storage_->mutex);
  if (storage_->finished) {
    throw std::logic_error("HuxerUI Android Surface stream is finished");
  }
  storage_->stream->SetDefaultBufferSize(environment, pixel_width, pixel_height);
  storage_->pixel_width = pixel_width;
  storage_->pixel_height = pixel_height;
}

void SurfaceStreamTexture::Finish() noexcept {
  std::shared_ptr<huxerui::detail::SurfaceStream> stream;
  std::uint64_t callback_token = 0;
  {
    std::lock_guard lock(storage_->mutex);
    if (storage_->finished) {
      return;
    }
    storage_->finished = true;
    callback_token = std::exchange(storage_->callback_token, 0);
    stream = std::move(storage_->stream);
  }
  if (callback_token != 0) {
    huxerui::detail::UnregisterSurfaceStream(callback_token);
  }
  stream.reset();
}

void SurfaceStreamTexture::FrameAvailable() {
  std::shared_ptr<huxerui::detail::SurfaceStream> stream;
  int pixel_width = 0;
  int pixel_height = 0;
  {
    std::lock_guard lock(storage_->mutex);
    if (storage_->finished) {
      return;
    }
    stream = storage_->stream;
    pixel_width = storage_->pixel_width;
    pixel_height = storage_->pixel_height;
  }
  std::shared_ptr<const huxerui::detail::AndroidGpuFrame> frame =
      huxerui::detail::GpuContext::Instance().LatchSurfaceStream(stream, pixel_width, pixel_height);
  {
    std::lock_guard lock(storage_->mutex);
    if (storage_->finished) {
      return;
    }
    storage_->frame = std::move(frame);
  }
  NotifyFrameAvailable();
}

std::shared_ptr<const huxerui::detail::AndroidGpuFrame> SurfaceStreamTexture::AcquireFrame() const noexcept {
  std::lock_guard lock(storage_->mutex);
  return storage_->frame;
}

} // namespace huxerui::android

extern "C" JNIEXPORT void JNICALL
Java_org_huxerui_HuxerUISurfaceStream_nativeFrameAvailable(JNIEnv*, jclass, jlong callback_token) {
  if (callback_token > 0) {
    huxerui::detail::DispatchSurfaceStream(static_cast<std::uint64_t>(callback_token));
  }
}
