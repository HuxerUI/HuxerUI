#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ios_http_internal.h"
#include "io/http_internal.h"

namespace huxerui::detail {
class IosHttpRequest;
}

@interface HuxerUIIosHttpDelegate : NSObject <NSURLSessionDataDelegate> {
  std::weak_ptr<huxerui::detail::IosHttpRequest> request_;
}

- (instancetype)initWithRequest:(std::weak_ptr<huxerui::detail::IosHttpRequest>)request;

@end

namespace huxerui::detail {

namespace {

NSString* HttpMethodName(HttpMethod method) {
  switch (method) {
  case HttpMethod::Get:
    return @"GET";
  case HttpMethod::Head:
    return @"HEAD";
  case HttpMethod::Post:
    return @"POST";
  case HttpMethod::Put:
    return @"PUT";
  case HttpMethod::Patch:
    return @"PATCH";
  case HttpMethod::Delete:
    return @"DELETE";
  case HttpMethod::Options:
    return @"OPTIONS";
  }
  return @"GET";
}

NSString* MakeString(std::string_view value) {
  return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
}

std::string MakeString(NSString* value) {
  if (value == nil) {
    return {};
  }
  NSData* data = [value dataUsingEncoding:NSUTF8StringEncoding];
  if (data == nil || data.length == 0) {
    return {};
  }
  return std::string(static_cast<const char*>(data.bytes), data.length);
}

std::string ErrorMessage(NSError* error) {
  const std::string description = MakeString(error.localizedDescription);
  return description.empty() ? "HuxerUI HTTP request failed" : "HuxerUI HTTP request failed: " + description;
}

bool EqualsAsciiCaseInsensitive(std::string_view first, std::string_view second) noexcept {
  if (first.size() != second.size()) {
    return false;
  }
  for (std::size_t index = 0; index < first.size(); ++index) {
    unsigned char left = first[index];
    unsigned char right = second[index];
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

} // namespace

class IosHttpRequest final : public HttpTransportOperation, public std::enable_shared_from_this<IosHttpRequest> {
public:
  IosHttpRequest(HttpRequest request, HttpTransportCallbacks callbacks)
      : request_(std::move(request)), callbacks_(std::move(callbacks)) {}

  ~IosHttpRequest() override {
    Cancel();
  }

  void Start() noexcept {
    @autoreleasepool {
      try {
        NSString* url_text = MakeString(request_.url);
        NSURL* url = url_text == nil ? nil : [NSURL URLWithString:url_text];
        if (url == nil) {
          FinishError(HttpError{HttpErrorCode::Transport, "HuxerUI HTTP URL is not valid UTF-8"}, false);
          return;
        }

        const NSTimeInterval native_timeout = request_.timeout.has_value()
                                                    ? std::chrono::duration<double>(*request_.timeout).count()
                                                    : std::numeric_limits<NSTimeInterval>::max();
        NSMutableURLRequest* native_request = [[NSMutableURLRequest alloc]
                initWithURL:url
                cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
            timeoutInterval:native_timeout];
        native_request.HTTPMethod = HttpMethodName(request_.method);
        for (const HttpHeader& header : request_.headers) {
          NSString* name = MakeString(header.name);
          NSString* value = MakeString(header.value);
          if (name == nil || value == nil) {
            FinishError(HttpError{HttpErrorCode::Transport, "HuxerUI HTTP header is not valid UTF-8"}, false);
            return;
          }
          [native_request addValue:value forHTTPHeaderField:name];
        }
        if (!request_.body.empty()) {
          native_request.HTTPBody = [NSData dataWithBytes:request_.body.data() length:request_.body.size()];
        }

        NSURLSessionConfiguration* configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        configuration.URLCache = nil;
        configuration.HTTPCookieStorage = nil;
        configuration.HTTPShouldSetCookies = NO;
        configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
        configuration.timeoutIntervalForRequest = native_timeout;
        configuration.timeoutIntervalForResource = native_timeout;

        std::weak_ptr<IosHttpRequest> weak = shared_from_this();
        HuxerUIIosHttpDelegate* delegate = [[HuxerUIIosHttpDelegate alloc] initWithRequest:weak];
        NSURLSession* session =
            [NSURLSession sessionWithConfiguration:configuration delegate:delegate delegateQueue:nil];
        NSURLSessionDataTask* task = [session dataTaskWithRequest:native_request];

        __strong dispatch_source_t timeout = nil;
        if (request_.timeout.has_value()) {
          const double seconds = std::chrono::duration<double>(*request_.timeout).count();
          const double maximum_seconds =
              static_cast<double>(std::numeric_limits<std::int64_t>::max()) / static_cast<double>(NSEC_PER_SEC);
          const auto nanoseconds =
              static_cast<std::int64_t>(std::min(seconds, maximum_seconds) * static_cast<double>(NSEC_PER_SEC));
          timeout =
              dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
          dispatch_source_set_timer(timeout, dispatch_time(DISPATCH_TIME_NOW, nanoseconds), DISPATCH_TIME_FOREVER, 0);
          dispatch_source_set_event_handler(timeout, ^{
            if (auto request = weak.lock()) {
              request->Timeout();
            }
          });
          dispatch_resume(timeout);
        }

        bool cancel = false;
        {
          std::scoped_lock lock(mutex_);
          if (finished_) {
            cancel = true;
          } else {
            delegate_ = delegate;
            session_ = session;
            task_ = task;
            timeout_ = timeout;
          }
        }
        if (cancel) {
          if (timeout != nil) {
            dispatch_source_cancel(timeout);
          }
          [session invalidateAndCancel];
          return;
        }
        [task resume];
      } catch (const std::exception& exception) {
        FinishError(HttpError{
            HttpErrorCode::Transport,
            "HuxerUI iOS HTTP request could not be started: " + std::string(exception.what()),
        }, true);
      } catch (...) {
        FinishError(HttpError{HttpErrorCode::Transport, "HuxerUI iOS HTTP request could not be started"}, true);
      }
    }
  }

  void RequestRead() override {
    __strong NSURLSessionDataTask* task = nil;
    {
      std::scoped_lock lock(mutex_);
      if (!finished_ && response_published_) {
        task = task_;
      }
    }
    [task resume];
  }

  void Cancel() noexcept override {
    __strong NSURLSession* session = nil;
    __strong dispatch_source_t timeout = nil;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      callbacks_ = {};
      session = std::exchange(session_, nil);
      task_ = nil;
      delegate_ = nil;
      timeout = std::exchange(timeout_, nil);
    }
    if (timeout != nil) {
      dispatch_source_cancel(timeout);
    }
    [session invalidateAndCancel];
  }

  bool ReceiveResponse(NSURLSessionDataTask* task, NSURLResponse* response) noexcept {
    @autoreleasepool {
      if (![response isKindOfClass:[NSHTTPURLResponse class]]) {
        FinishError(HttpError{HttpErrorCode::Transport, "HuxerUI HTTP response is invalid"}, true);
        return false;
      }
      [task suspend];
      NSHTTPURLResponse* http_response = static_cast<NSHTTPURLResponse*>(response);
      HttpTransportResponse result{
          .url = MakeString(http_response.URL.absoluteString),
          .status_code = static_cast<int>(http_response.statusCode),
          .headers = {},
          .body_size = std::nullopt,
      };
      result.headers.reserve(http_response.allHeaderFields.count);
      bool encoded = false;
      bool transfer_encoded = false;
      for (id key in http_response.allHeaderFields) {
        id value = http_response.allHeaderFields[key];
        const std::string name = MakeString([key description]);
        const std::string header_value = MakeString([value description]);
        if (EqualsAsciiCaseInsensitive(name, "Content-Encoding") && !header_value.empty()) {
          encoded = true;
        }
        if (EqualsAsciiCaseInsensitive(name, "Transfer-Encoding") && !header_value.empty()) {
          transfer_encoded = true;
        }
        result.headers.push_back({name, header_value});
      }
      if (request_.method == HttpMethod::Head || (result.status_code >= 100 && result.status_code < 200) ||
          result.status_code == 204 || result.status_code == 304) {
        result.body_size = 0;
      } else if (!encoded && !transfer_encoded && response.expectedContentLength >= 0) {
        result.body_size = static_cast<std::uint64_t>(response.expectedContentLength);
      }

      std::function<void(HttpTransportResponse)> callback;
      {
        std::scoped_lock lock(mutex_);
        if (finished_ || response_published_) {
          return false;
        }
        response_published_ = true;
        callback = callbacks_.response;
      }
      if (callback) {
        callback(std::move(result));
      }
      return true;
    }
  }

  void ReceiveBody(NSURLSessionDataTask* task, NSData* data) noexcept {
    @autoreleasepool {
      [task suspend];
      if (data == nil || data.length == 0) {
        FinishError(HttpError{HttpErrorCode::Transport, "HuxerUI iOS HTTP response body is invalid"}, true);
        return;
      }
      std::function<void(Bytes)> callback;
      {
        std::scoped_lock lock(mutex_);
        if (finished_) {
          return;
        }
        callback = callbacks_.body;
      }
      if (callback) {
        const auto* bytes = static_cast<const std::byte*>(data.bytes);
        callback(Bytes(bytes, bytes + data.length));
      }
    }
  }

  void Upload(std::uint64_t transferred_bytes) noexcept {
    std::function<void(std::uint64_t)> callback;
    {
      std::scoped_lock lock(mutex_);
      if (!finished_ && !response_published_) {
        callback = callbacks_.upload_progress;
      }
    }
    if (callback) {
      callback(transferred_bytes);
    }
  }

  void Complete(NSError* error) noexcept {
    if (error != nil) {
      const HttpErrorCode code = error.code == NSURLErrorTimedOut ? HttpErrorCode::Timeout : HttpErrorCode::Transport;
      FinishError(HttpError{code, ErrorMessage(error)}, false);
      return;
    }
    FinishComplete();
  }

  void Timeout() noexcept {
    FinishError(HttpError{HttpErrorCode::Timeout, "HuxerUI HTTP request timed out"}, true);
  }

private:
  void FinishComplete() noexcept {
    __strong NSURLSession* session = nil;
    __strong dispatch_source_t timeout = nil;
    HttpTransportCallbacks callbacks;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      callbacks = std::move(callbacks_);
      session = std::exchange(session_, nil);
      task_ = nil;
      delegate_ = nil;
      timeout = std::exchange(timeout_, nil);
    }
    if (timeout != nil) {
      dispatch_source_cancel(timeout);
    }
    [session finishTasksAndInvalidate];
    if (callbacks.complete) {
      callbacks.complete();
    }
  }

  void FinishError(HttpError error, bool cancel_task) noexcept {
    __strong NSURLSession* session = nil;
    __strong dispatch_source_t timeout = nil;
    HttpTransportCallbacks callbacks;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      callbacks = std::move(callbacks_);
      session = std::exchange(session_, nil);
      task_ = nil;
      delegate_ = nil;
      timeout = std::exchange(timeout_, nil);
    }
    if (timeout != nil) {
      dispatch_source_cancel(timeout);
    }
    if (cancel_task) {
      [session invalidateAndCancel];
    } else {
      [session finishTasksAndInvalidate];
    }
    if (callbacks.error) {
      callbacks.error(std::move(error));
    }
  }

  std::mutex mutex_;
  HttpRequest request_;
  HttpTransportCallbacks callbacks_;
  __strong HuxerUIIosHttpDelegate* delegate_ = nil;
  __strong NSURLSession* session_ = nil;
  __strong NSURLSessionDataTask* task_ = nil;
  __strong dispatch_source_t timeout_ = nil;
  bool response_published_ = false;
  bool finished_ = false;
};

namespace {

class IosHttpTransport final : public HttpTransport {
public:
  std::shared_ptr<HttpTransportOperation>
  Start(HttpRequest request, bool, HttpTransportCallbacks callbacks) override {
    auto operation = std::make_shared<IosHttpRequest>(std::move(request), std::move(callbacks));
    operation->Start();
    return operation;
  }
};

} // namespace

std::shared_ptr<HttpTransport> CreateIosHttpTransport() {
  return std::make_shared<IosHttpTransport>();
}

} // namespace huxerui::detail

@implementation HuxerUIIosHttpDelegate

- (instancetype)initWithRequest:(std::weak_ptr<huxerui::detail::IosHttpRequest>)request {
  self = [super init];
  if (self != nil) {
    request_ = std::move(request);
  }
  return self;
}

- (void)URLSession:(NSURLSession*)session
                 dataTask:(NSURLSessionDataTask*)dataTask
    didReceiveResponse:(NSURLResponse*)response
      completionHandler:(void (^)(NSURLSessionResponseDisposition disposition))completionHandler {
  (void)session;
  if (auto request = request_.lock(); request && request->ReceiveResponse(dataTask, response)) {
    completionHandler(NSURLSessionResponseAllow);
  } else {
    completionHandler(NSURLSessionResponseCancel);
  }
}

- (void)URLSession:(NSURLSession*)session
          dataTask:(NSURLSessionDataTask*)dataTask
    didReceiveData:(NSData*)data {
  (void)session;
  if (auto request = request_.lock()) {
    request->ReceiveBody(dataTask, data);
  }
}

- (void)URLSession:(NSURLSession*)session
                  task:(NSURLSessionTask*)task
       didSendBodyData:(int64_t)bytesSent
        totalBytesSent:(int64_t)totalBytesSent
totalBytesExpectedToSend:(int64_t)totalBytesExpectedToSend {
  (void)session;
  (void)task;
  (void)bytesSent;
  (void)totalBytesExpectedToSend;
  if (auto request = request_.lock(); request && totalBytesSent >= 0) {
    request->Upload(static_cast<std::uint64_t>(totalBytesSent));
  }
}

- (void)URLSession:(NSURLSession*)session
                    task:(NSURLSessionTask*)task
    didCompleteWithError:(NSError*)error {
  (void)session;
  (void)task;
  if (auto request = request_.lock()) {
    request->Complete(error);
  }
}

@end
