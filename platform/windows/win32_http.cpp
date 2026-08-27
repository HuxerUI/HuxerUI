#include "win32_http_internal.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "http_internal.h"
#include "win32_internal.h"

namespace huxerui::detail {
namespace {

constexpr std::size_t kReadBufferSize = 64U * 1024U;

const wchar_t* HttpMethodName(HttpMethod method) noexcept {
  switch (method) {
  case HttpMethod::Get:
    return L"GET";
  case HttpMethod::Head:
    return L"HEAD";
  case HttpMethod::Post:
    return L"POST";
  case HttpMethod::Put:
    return L"PUT";
  case HttpMethod::Patch:
    return L"PATCH";
  case HttpMethod::Delete:
    return L"DELETE";
  case HttpMethod::Options:
    return L"OPTIONS";
  }
  return L"GET";
}

bool EqualsAsciiCaseInsensitive(std::string_view first, std::string_view second) noexcept {
  if (first.size() != second.size()) {
    return false;
  }
  for (std::size_t index = 0; index < first.size(); ++index) {
    unsigned char left = static_cast<unsigned char>(first[index]);
    unsigned char right = static_cast<unsigned char>(second[index]);
    if (left >= 'A' && left <= 'Z') {
      left = static_cast<unsigned char>(left + ('a' - 'A'));
    }
    if (right >= 'A' && right <= 'Z') {
      right = static_cast<unsigned char>(right + ('a' - 'A'));
    }
    if (left != right) {
      return false;
    }
  }
  return true;
}

std::string WinHttpErrorMessage(DWORD error) {
  return "HuxerUI Windows HTTP request failed with WinHTTP error " + std::to_string(error);
}

HttpErrorCode WinHttpErrorCode(DWORD error) noexcept {
  return error == ERROR_WINHTTP_TIMEOUT ? HttpErrorCode::Timeout : HttpErrorCode::Transport;
}

class Win32HttpSession final {
public:
  Win32HttpSession() {
#if defined(HUXERUI_WINDOWS_7_COMPAT)
    constexpr DWORD access_type = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
#else
    constexpr DWORD access_type = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
#endif
    handle_ =
        WinHttpOpen(L"HuxerUI/1.0", access_type, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (handle_ == nullptr) {
      error_ = GetLastError();
    }
  }

  ~Win32HttpSession() {
    if (handle_ != nullptr) {
      WinHttpCloseHandle(handle_);
    }
  }

  Win32HttpSession(const Win32HttpSession&) = delete;
  Win32HttpSession& operator=(const Win32HttpSession&) = delete;

  [[nodiscard]] HINTERNET Handle() const noexcept {
    return handle_;
  }

  [[nodiscard]] DWORD Error() const noexcept {
    return error_;
  }

private:
  HINTERNET handle_ = nullptr;
  DWORD error_ = ERROR_SUCCESS;
};

class Win32HttpRequest;

class Win32HttpDeadline final {
public:
  static std::shared_ptr<Win32HttpDeadline>
  Start(const std::shared_ptr<Win32HttpRequest>& request, std::chrono::milliseconds timeout) {
    auto deadline = std::shared_ptr<Win32HttpDeadline>(new Win32HttpDeadline(request));
    auto owner = std::make_unique<std::shared_ptr<Win32HttpDeadline>>(deadline);
    PTP_TIMER timer = CreateThreadpoolTimer(Callback, owner.get(), nullptr);
    if (timer == nullptr) {
      throw std::runtime_error("HuxerUI Windows HTTP could not create its deadline timer");
    }
    {
      std::scoped_lock lock(deadline->mutex_);
      deadline->timer_ = timer;
      deadline->owner_ = owner.release();
    }

    constexpr std::int64_t ticks_per_millisecond = 10'000;
    const std::int64_t maximum_milliseconds = std::numeric_limits<std::int64_t>::max() / ticks_per_millisecond;
    const std::int64_t bounded_milliseconds = std::min(timeout.count(), maximum_milliseconds);
    LARGE_INTEGER due_time{};
    due_time.QuadPart = -bounded_milliseconds * ticks_per_millisecond;
    FILETIME due{};
    due.dwLowDateTime = due_time.LowPart;
    due.dwHighDateTime = static_cast<DWORD>(due_time.HighPart);
    SetThreadpoolTimer(timer, &due, 0, 0);
    return deadline;
  }

  ~Win32HttpDeadline() {
    Cancel();
  }

  Win32HttpDeadline(const Win32HttpDeadline&) = delete;
  Win32HttpDeadline& operator=(const Win32HttpDeadline&) = delete;

  void Cancel() noexcept {
    PTP_TIMER timer = nullptr;
    std::shared_ptr<Win32HttpDeadline>* owner = nullptr;
    {
      std::scoped_lock lock(mutex_);
      if (timer_ == nullptr) {
        return;
      }
      timer = std::exchange(timer_, nullptr);
      owner = std::exchange(owner_, nullptr);
      SetThreadpoolTimer(timer, nullptr, 0, 0);
    }
    WaitForThreadpoolTimerCallbacks(timer, TRUE);
    CloseThreadpoolTimer(timer);
    delete owner;
  }

private:
  explicit Win32HttpDeadline(const std::shared_ptr<Win32HttpRequest>& request) : request_(request) {}

  static void CALLBACK Callback(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER timer) noexcept {
    auto* owner = static_cast<std::shared_ptr<Win32HttpDeadline>*>(context);
    const std::shared_ptr<Win32HttpDeadline> deadline = *owner;
    deadline->Fire(timer, owner);
  }

  void Fire(PTP_TIMER timer, std::shared_ptr<Win32HttpDeadline>* owner) noexcept;

  std::mutex mutex_;
  std::weak_ptr<Win32HttpRequest> request_;
  PTP_TIMER timer_ = nullptr;
  std::shared_ptr<Win32HttpDeadline>* owner_ = nullptr;
};

class Win32HttpRequest final : public std::enable_shared_from_this<Win32HttpRequest> {
public:
  Win32HttpRequest(std::shared_ptr<Win32HttpSession> session, HttpRequest request, HttpTransportCompletion completion)
      : session_(std::move(session)), request_(std::move(request)), completion_(std::move(completion)) {}

  ~Win32HttpRequest() {
    if (request_handle_ != nullptr) {
      WinHttpCloseHandle(request_handle_);
    }
    if (connection_handle_ != nullptr) {
      WinHttpCloseHandle(connection_handle_);
    }
  }

  Win32HttpRequest(const Win32HttpRequest&) = delete;
  Win32HttpRequest& operator=(const Win32HttpRequest&) = delete;

  void Start() noexcept {
    try {
      if (request_.body.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
        FinishUnattached(HttpResult(
            HttpError{
                HttpErrorCode::Transport,
                "HuxerUI Windows HTTP request body exceeds the WinHTTP size range",
            }
        ));
        return;
      }

      const std::optional<std::wstring> url = StrictUtf8ToWide(request_.url);
      if (!url.has_value()) {
        FinishUnattached(HttpResult(
            HttpError{
                HttpErrorCode::Transport,
                "HuxerUI Windows HTTP URL is not valid UTF-8",
            }
        ));
        return;
      }
      URL_COMPONENTS components{};
      components.dwStructSize = sizeof(components);
      components.dwSchemeLength = static_cast<DWORD>(-1);
      components.dwHostNameLength = static_cast<DWORD>(-1);
      components.dwUrlPathLength = static_cast<DWORD>(-1);
      components.dwExtraInfoLength = static_cast<DWORD>(-1);
      if (WinHttpCrackUrl(url->data(), static_cast<DWORD>(url->size()), 0, &components) == FALSE ||
          (components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS) ||
          components.lpszHostName == nullptr || components.dwHostNameLength == 0) {
        FinishUnattached(HttpResult(
            HttpError{
                HttpErrorCode::Transport,
                "HuxerUI Windows HTTP URL could not be parsed",
            }
        ));
        return;
      }

      const std::wstring host(components.lpszHostName, components.dwHostNameLength);
      std::wstring object_name;
      if (components.lpszUrlPath != nullptr && components.dwUrlPathLength != 0) {
        object_name.append(components.lpszUrlPath, components.dwUrlPathLength);
      }
      if (components.lpszExtraInfo != nullptr && components.dwExtraInfoLength != 0) {
        object_name.append(components.lpszExtraInfo, components.dwExtraInfoLength);
      }
      if (const std::size_t fragment = object_name.find(L'#'); fragment != std::wstring::npos) {
        object_name.resize(fragment);
      }
      if (object_name.empty()) {
        object_name = L"/";
      }

      connection_handle_ = WinHttpConnect(session_->Handle(), host.c_str(), components.nPort, 0);
      if (connection_handle_ == nullptr) {
        FinishUnattached(WinHttpError(GetLastError()));
        return;
      }
      const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
      request_handle_ = WinHttpOpenRequest(
          connection_handle_,
          HttpMethodName(request_.method),
          object_name.c_str(),
          nullptr,
          WINHTTP_NO_REFERER,
          WINHTTP_DEFAULT_ACCEPT_TYPES,
          flags
      );
      if (request_handle_ == nullptr) {
        FinishUnattached(WinHttpError(GetLastError()));
        return;
      }
      if (!AttachCallback()) {
        return;
      }
      if (const DWORD option_error = ConfigureRequest(); option_error != ERROR_SUCCESS) {
        Finish(WinHttpError(option_error));
        return;
      }
      if (request_.timeout.has_value()) {
        // WinHTTP timeouts apply to individual phases. This deadline also covers callbacks and body buffering.
        std::shared_ptr<Win32HttpDeadline> deadline = Win32HttpDeadline::Start(shared_from_this(), *request_.timeout);
        bool cancel_deadline = false;
        {
          std::scoped_lock lock(mutex_);
          if (finished_) {
            cancel_deadline = true;
          } else {
            deadline_ = deadline;
          }
        }
        if (cancel_deadline) {
          deadline->Cancel();
        }
      }

      const DWORD send_error = WithRequestHandle([this](HINTERNET request_handle) {
        const DWORD body_size = static_cast<DWORD>(request_.body.size());
        // WinHTTP may resend after authentication or redirects, so request_ owns the body through HANDLE_CLOSING.
        void* body = request_.body.empty() ? WINHTTP_NO_REQUEST_DATA : request_.body.data();
        const BOOL started = WinHttpSendRequest(
            request_handle,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            body,
            body_size,
            body_size,
            reinterpret_cast<DWORD_PTR>(request_owner_)
        );
        return started != FALSE ? ERROR_SUCCESS : GetLastError();
      });
      if (send_error != ERROR_SUCCESS) {
        Finish(WinHttpError(send_error));
      }
    } catch (const std::exception& exception) {
      Finish(HttpResult(HttpError{HttpErrorCode::Transport, exception.what()}));
    } catch (...) {
      Finish(HttpResult(
          HttpError{
              HttpErrorCode::Transport,
              "HuxerUI Windows HTTP request could not be started",
          }
      ));
    }
  }

  void Cancel() noexcept {
    Finish(std::nullopt);
  }

  void Timeout() noexcept {
    Finish(HttpResult(HttpError{HttpErrorCode::Timeout, "HuxerUI HTTP request timed out"}));
  }

private:
  static void CALLBACK StatusCallback(
      HINTERNET, DWORD_PTR context, DWORD status, void* status_information, DWORD status_information_length
  ) noexcept {
    if (context == 0) {
      return;
    }
    auto* owner = reinterpret_cast<std::shared_ptr<Win32HttpRequest>*>(context);
    const std::shared_ptr<Win32HttpRequest> request = *owner;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
      request->HandleClosed(owner);
      delete owner;
      return;
    }
    try {
      request->HandleStatus(status, status_information, status_information_length);
    } catch (const std::exception& exception) {
      request->Finish(HttpResult(HttpError{HttpErrorCode::Transport, exception.what()}));
    } catch (...) {
      request->Finish(HttpResult(
          HttpError{
              HttpErrorCode::Transport,
              "HuxerUI Windows HTTP response could not be processed",
          }
      ));
    }
  }

  bool AttachCallback() noexcept {
    auto owner = std::make_unique<std::shared_ptr<Win32HttpRequest>>(shared_from_this());
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(owner.get());
    if (WinHttpSetOption(request_handle_, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) == FALSE) {
      FinishUnattached(WinHttpError(GetLastError()));
      return false;
    }
    constexpr DWORD callbacks = WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE | WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE |
                                WINHTTP_CALLBACK_FLAG_READ_COMPLETE | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR |
                                WINHTTP_CALLBACK_FLAG_HANDLES;
    if (WinHttpSetStatusCallback(request_handle_, StatusCallback, callbacks, 0) == WINHTTP_INVALID_STATUS_CALLBACK) {
      FinishUnattached(WinHttpError(GetLastError()));
      return false;
    }
    // WinHTTP may report cancellation after WinHttpCloseHandle returns. HANDLE_CLOSING releases this owner.
    request_owner_ = owner.release();
    return true;
  }

  DWORD ConfigureRequest() {
    return WithRequestHandle([this](HINTERNET request_handle) {
      if (request_.timeout.has_value()) {
        const auto maximum_timeout = static_cast<std::int64_t>(std::numeric_limits<int>::max());
        const int timeout = static_cast<int>(std::min(request_.timeout->count(), maximum_timeout));
        if (WinHttpSetTimeouts(request_handle, timeout, timeout, timeout, timeout) == FALSE) {
          return GetLastError();
        }
      }

      DWORD disabled_features = WINHTTP_DISABLE_COOKIES;
      if (WinHttpSetOption(
              request_handle,
              WINHTTP_OPTION_DISABLE_FEATURE,
              &disabled_features,
              sizeof(disabled_features)
          ) == FALSE) {
        return GetLastError();
      }

      bool accepts_encoded_response = false;
      for (const HttpHeader& header : request_.headers) {
        if (EqualsAsciiCaseInsensitive(header.name, "Accept-Encoding")) {
          accepts_encoded_response = true;
        }
        const std::optional<std::wstring> name = StrictUtf8ToWide(header.name);
        const std::optional<std::wstring> value = StrictUtf8ToWide(header.value);
        if (!name.has_value() || !value.has_value()) {
          return static_cast<DWORD>(ERROR_NO_UNICODE_TRANSLATION);
        }
        std::wstring line = *name + L": " + *value;
        if (line.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
          return static_cast<DWORD>(ERROR_ARITHMETIC_OVERFLOW);
        }
        if (WinHttpAddRequestHeaders(
                request_handle,
                line.c_str(),
                static_cast<DWORD>(line.size()),
                WINHTTP_ADDREQ_FLAG_ADD
            ) == FALSE) {
          return GetLastError();
        }
      }

      if (!accepts_encoded_response) {
        DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
        if (WinHttpSetOption(request_handle, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression)) ==
            FALSE) {
          const DWORD error = GetLastError();
          if (error != ERROR_WINHTTP_INVALID_OPTION) {
            return error;
          }
        }
      }
      return static_cast<DWORD>(ERROR_SUCCESS);
    });
  }

  template <class Operation> DWORD WithRequestHandle(Operation&& operation) {
    HINTERNET request_handle = nullptr;
    {
      std::scoped_lock lock(mutex_);
      if (finished_ || request_handle_ == nullptr) {
        return ERROR_WINHTTP_OPERATION_CANCELLED;
      }
      // A concurrent cancellation defers WinHttpCloseHandle until this native call has returned.
      ++native_calls_;
      request_handle = request_handle_;
    }

    DWORD result = ERROR_SUCCESS;
    try {
      result = std::forward<Operation>(operation)(request_handle);
    } catch (...) {
      EndNativeCall();
      throw;
    }
    EndNativeCall();
    return result;
  }

  void EndNativeCall() noexcept {
    HINTERNET request_handle = nullptr;
    {
      std::scoped_lock lock(mutex_);
      --native_calls_;
      if (native_calls_ == 0 && close_requested_) {
        request_handle = std::exchange(request_handle_, nullptr);
      }
    }
    if (request_handle != nullptr) {
      WinHttpCloseHandle(request_handle);
    }
  }

  void HandleStatus(DWORD status, void* status_information, DWORD status_information_length) {
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
      ReceiveResponse();
      break;
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
      ReceiveHeaders();
      break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
      ReceiveBody(status_information, status_information_length);
      break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
      if (status_information == nullptr || status_information_length != sizeof(WINHTTP_ASYNC_RESULT)) {
        Finish(HttpResult(
            HttpError{
                HttpErrorCode::Transport,
                "HuxerUI Windows HTTP request returned an invalid asynchronous error",
            }
        ));
        return;
      }
      Finish(WinHttpError(static_cast<const WINHTTP_ASYNC_RESULT*>(status_information)->dwError));
      break;
    default:
      break;
    }
  }

  void ReceiveResponse() {
    const DWORD error = WithRequestHandle([](HINTERNET request_handle) {
      return WinHttpReceiveResponse(request_handle, nullptr) != FALSE ? ERROR_SUCCESS : GetLastError();
    });
    if (error != ERROR_SUCCESS) {
      Finish(WinHttpError(error));
    }
  }

  void ReceiveHeaders() {
    HttpResponse response;
    const DWORD error = WithRequestHandle([&response](HINTERNET request_handle) {
      DWORD status_code = 0;
      DWORD status_size = sizeof(status_code);
      if (WinHttpQueryHeaders(
              request_handle,
              WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
              WINHTTP_HEADER_NAME_BY_INDEX,
              &status_code,
              &status_size,
              WINHTTP_NO_HEADER_INDEX
          ) == FALSE) {
        return GetLastError();
      }
      response.status_code = static_cast<int>(status_code);

      DWORD url_bytes = 0;
      if (WinHttpQueryOption(request_handle, WINHTTP_OPTION_URL, nullptr, &url_bytes) != FALSE) {
        return static_cast<DWORD>(ERROR_INVALID_DATA);
      }
      const DWORD url_size_error = GetLastError();
      if (url_size_error != ERROR_INSUFFICIENT_BUFFER || url_bytes < sizeof(wchar_t)) {
        return url_size_error;
      }
      std::vector<wchar_t> url(url_bytes / sizeof(wchar_t));
      if (WinHttpQueryOption(request_handle, WINHTTP_OPTION_URL, url.data(), &url_bytes) == FALSE) {
        return GetLastError();
      }
      const std::size_t url_length = std::char_traits<wchar_t>::length(url.data());
      response.url = WideToUtf8(std::wstring_view(url.data(), url_length));
      if (url_length != 0 && response.url.empty()) {
        return static_cast<DWORD>(ERROR_NO_UNICODE_TRANSLATION);
      }

      DWORD header_bytes = 0;
      if (WinHttpQueryHeaders(
              request_handle,
              WINHTTP_QUERY_RAW_HEADERS,
              WINHTTP_HEADER_NAME_BY_INDEX,
              nullptr,
              &header_bytes,
              WINHTTP_NO_HEADER_INDEX
          ) != FALSE) {
        return static_cast<DWORD>(ERROR_INVALID_DATA);
      }
      const DWORD header_size_error = GetLastError();
      if (header_size_error != ERROR_INSUFFICIENT_BUFFER || header_bytes < sizeof(wchar_t) * 2U) {
        return header_size_error;
      }
      std::vector<wchar_t> headers(header_bytes / sizeof(wchar_t));
      if (WinHttpQueryHeaders(
              request_handle,
              WINHTTP_QUERY_RAW_HEADERS,
              WINHTTP_HEADER_NAME_BY_INDEX,
              headers.data(),
              &header_bytes,
              WINHTTP_NO_HEADER_INDEX
          ) == FALSE) {
        return GetLastError();
      }

      const std::size_t header_characters = header_bytes / sizeof(wchar_t);
      std::size_t offset = 0;
      bool status_line = true;
      while (offset < header_characters && headers[offset] != L'\0') {
        const std::wstring_view line(headers.data() + offset);
        offset += line.size() + 1U;
        if (status_line) {
          status_line = false;
          continue;
        }
        const std::size_t separator = line.find(L':');
        if (separator == std::wstring_view::npos || separator == 0) {
          continue;
        }
        std::wstring_view value = line.substr(separator + 1U);
        while (!value.empty() && (value.front() == L' ' || value.front() == L'\t')) {
          value.remove_prefix(1U);
        }
        HttpHeader header{
            WideToUtf8(line.substr(0, separator)),
            WideToUtf8(value),
        };
        if (header.name.empty() || (!value.empty() && header.value.empty())) {
          return static_cast<DWORD>(ERROR_NO_UNICODE_TRANSLATION);
        }
        response.headers.push_back(std::move(header));
      }
      return static_cast<DWORD>(ERROR_SUCCESS);
    });
    if (error != ERROR_SUCCESS) {
      Finish(WinHttpError(error));
      return;
    }
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      response_ = std::move(response);
    }
    ReadBody();
  }

  void ReadBody() {
    const DWORD error = WithRequestHandle([this](HINTERNET request_handle) {
      return WinHttpReadData(request_handle, read_buffer_.data(), static_cast<DWORD>(read_buffer_.size()), nullptr) !=
                     FALSE
                 ? ERROR_SUCCESS
                 : GetLastError();
    });
    if (error != ERROR_SUCCESS) {
      Finish(WinHttpError(error));
    }
  }

  void ReceiveBody(void* bytes, DWORD byte_count) {
    if (byte_count == 0) {
      HttpResponse response;
      {
        std::scoped_lock lock(mutex_);
        if (finished_) {
          return;
        }
        response = std::move(response_);
      }
      Finish(HttpResult(std::move(response)));
      return;
    }
    if (bytes == nullptr) {
      Finish(HttpResult(
          HttpError{
              HttpErrorCode::Transport,
              "HuxerUI Windows HTTP response body is invalid",
          }
      ));
      return;
    }
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      const auto* body = static_cast<const std::byte*>(bytes);
      response_.body.insert(response_.body.end(), body, body + byte_count);
    }
    ReadBody();
  }

  HttpResult WinHttpError(DWORD error) const {
    return HttpResult(HttpError{WinHttpErrorCode(error), WinHttpErrorMessage(error)});
  }

  void Finish(std::optional<HttpResult> result) noexcept {
    HttpTransportCompletion completion;
    std::shared_ptr<Win32HttpDeadline> deadline;
    HINTERNET request_handle = nullptr;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      completion = std::move(completion_);
      deadline = std::move(deadline_);
      close_requested_ = true;
      if (native_calls_ == 0) {
        request_handle = std::exchange(request_handle_, nullptr);
      }
    }
    if (deadline) {
      deadline->Cancel();
    }
    if (request_handle != nullptr) {
      WinHttpCloseHandle(request_handle);
    }
    if (result.has_value() && completion) {
      completion(std::move(*result));
    }
  }

  void FinishUnattached(HttpResult result) noexcept {
    HINTERNET request_handle = std::exchange(request_handle_, nullptr);
    HINTERNET connection_handle = std::exchange(connection_handle_, nullptr);
    if (request_handle != nullptr) {
      WinHttpCloseHandle(request_handle);
    }
    if (connection_handle != nullptr) {
      WinHttpCloseHandle(connection_handle);
    }
    HttpTransportCompletion completion = std::move(completion_);
    finished_ = true;
    if (completion) {
      completion(std::move(result));
    }
  }

  void HandleClosed(std::shared_ptr<Win32HttpRequest>* owner) noexcept {
    HINTERNET connection_handle = nullptr;
    {
      std::scoped_lock lock(mutex_);
      if (request_owner_ == owner) {
        request_owner_ = nullptr;
      }
      request_handle_ = nullptr;
      connection_handle = std::exchange(connection_handle_, nullptr);
    }
    if (connection_handle != nullptr) {
      WinHttpCloseHandle(connection_handle);
    }
  }

  std::shared_ptr<Win32HttpSession> session_;
  HttpRequest request_;
  HttpTransportCompletion completion_;
  std::mutex mutex_;
  HINTERNET connection_handle_ = nullptr;
  HINTERNET request_handle_ = nullptr;
  std::shared_ptr<Win32HttpRequest>* request_owner_ = nullptr;
  std::shared_ptr<Win32HttpDeadline> deadline_;
  HttpResponse response_;
  std::array<std::byte, kReadBufferSize> read_buffer_{};
  std::size_t native_calls_ = 0;
  bool close_requested_ = false;
  bool finished_ = false;
};

void Win32HttpDeadline::Fire(PTP_TIMER timer, std::shared_ptr<Win32HttpDeadline>* owner) noexcept {
  {
    std::scoped_lock lock(mutex_);
    if (timer_ != timer || owner_ != owner) {
      return;
    }
    timer_ = nullptr;
    owner_ = nullptr;
  }
  CloseThreadpoolTimer(timer);
  delete owner;
  if (const std::shared_ptr<Win32HttpRequest> request = request_.lock()) {
    request->Timeout();
  }
}

class Win32HttpTransport final : public HttpTransport {
public:
  Win32HttpTransport() : session_(std::make_shared<Win32HttpSession>()) {}

  std::function<void()> Start(HttpRequest request, HttpTransportCompletion completion) override {
    if (session_->Handle() == nullptr) {
      completion(HttpResult(
          HttpError{
              HttpErrorCode::Transport,
              "HuxerUI Windows HTTP could not create a WinHTTP session: error " + std::to_string(session_->Error()),
          }
      ));
      return {};
    }
    auto operation = std::make_shared<Win32HttpRequest>(session_, std::move(request), std::move(completion));
    operation->Start();
    return [operation] { operation->Cancel(); };
  }

private:
  std::shared_ptr<Win32HttpSession> session_;
};

} // namespace

std::shared_ptr<HttpTransport> CreateWin32HttpTransport() {
  return std::make_shared<Win32HttpTransport>();
}

} // namespace huxerui::detail
