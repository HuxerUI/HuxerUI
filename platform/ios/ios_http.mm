#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "http_internal.h"
#include "ios_http_internal.h"

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

class IosHttpRequest final : public std::enable_shared_from_this<IosHttpRequest> {
public:
  IosHttpRequest(NSURLSession* session, HttpRequest request, HttpTransportCompletion completion)
      : session_(session), request_(std::move(request)), completion_(std::move(completion)) {}

  void Start() {
    @autoreleasepool {
      NSString* url_text = MakeString(request_.url);
      NSURL* url = url_text == nil ? nil : [NSURL URLWithString:url_text];
      if (url == nil) {
        Finish(HttpResult(HttpError{HttpErrorCode::Transport, "HuxerUI HTTP URL is not valid UTF-8"}), false);
        return;
      }

      NSMutableURLRequest* native_request = [[NSMutableURLRequest alloc]
              initWithURL:url
              cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
          timeoutInterval:request_.timeout.has_value() ? std::chrono::duration<double>(*request_.timeout).count()
                                                       : 60.0];
      native_request.HTTPMethod = HttpMethodName(request_.method);
      for (const HttpHeader& header : request_.headers) {
        NSString* name = MakeString(header.name);
        NSString* value = MakeString(header.value);
        if (name == nil || value == nil) {
          Finish(HttpResult(HttpError{HttpErrorCode::Transport, "HuxerUI HTTP header is not valid UTF-8"}), false);
          return;
        }
        [native_request addValue:value forHTTPHeaderField:name];
      }
      if (!request_.body.empty()) {
        native_request.HTTPBody = [NSData dataWithBytes:request_.body.data() length:request_.body.size()];
      }

      std::weak_ptr<IosHttpRequest> weak = shared_from_this();
      NSURLSessionDataTask* task =
          [session_ dataTaskWithRequest:native_request
                      completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                        if (auto request = weak.lock()) {
                          request->Complete(data, response, error);
                        }
                      }];
      {
        std::scoped_lock lock(mutex_);
        if (finished_) {
          [task cancel];
          return;
        }
        task_ = task;
      }
      [task resume];

      if (request_.timeout.has_value()) {
        const double seconds = std::chrono::duration<double>(*request_.timeout).count();
        const double maximum_seconds =
            static_cast<double>(std::numeric_limits<std::int64_t>::max()) / static_cast<double>(NSEC_PER_SEC);
        const auto nanoseconds =
            static_cast<std::int64_t>(std::min(seconds, maximum_seconds) * static_cast<double>(NSEC_PER_SEC));
        dispatch_source_t timeout =
            dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
        dispatch_source_set_timer(timeout, dispatch_time(DISPATCH_TIME_NOW, nanoseconds), DISPATCH_TIME_FOREVER, 0);
        dispatch_source_set_event_handler(timeout, ^{
          if (auto request = weak.lock()) {
            request->Timeout();
          }
        });
        dispatch_resume(timeout);

        bool cancel_timeout = false;
        {
          std::scoped_lock lock(mutex_);
          if (finished_) {
            cancel_timeout = true;
          } else {
            timeout_ = timeout;
          }
        }
        if (cancel_timeout) {
          dispatch_source_cancel(timeout);
        }
      }
    }
  }

  void Cancel() noexcept {
    __strong NSURLSessionDataTask* task = nil;
    __strong dispatch_source_t timeout = nil;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      completion_ = {};
      task = task_;
      task_ = nil;
      timeout = timeout_;
      timeout_ = nil;
    }
    if (timeout != nil) {
      dispatch_source_cancel(timeout);
    }
    [task cancel];
  }

private:
  void Complete(NSData* data, NSURLResponse* response, NSError* error) noexcept {
    @autoreleasepool {
      if (error != nil) {
        const HttpErrorCode code = error.code == NSURLErrorTimedOut ? HttpErrorCode::Timeout : HttpErrorCode::Transport;
        Finish(HttpResult(HttpError{code, ErrorMessage(error)}), false);
        return;
      }
      if (![response isKindOfClass:[NSHTTPURLResponse class]]) {
        Finish(HttpResult(HttpError{HttpErrorCode::Transport, "HuxerUI HTTP response is invalid"}), false);
        return;
      }

      NSHTTPURLResponse* http_response = static_cast<NSHTTPURLResponse*>(response);
      HttpResponse result{
          .url = MakeString(http_response.URL.absoluteString),
          .status_code = static_cast<int>(http_response.statusCode),
      };
      result.headers.reserve(http_response.allHeaderFields.count);
      for (id key in http_response.allHeaderFields) {
        id value = http_response.allHeaderFields[key];
        result.headers.push_back({
            MakeString([key description]),
            MakeString([value description]),
        });
      }
      if (data != nil && data.length != 0) {
        const auto* bytes = static_cast<const std::byte*>(data.bytes);
        result.body.assign(bytes, bytes + data.length);
      }
      Finish(HttpResult(std::move(result)), false);
    }
  }

  void Timeout() noexcept {
    Finish(HttpResult(HttpError{HttpErrorCode::Timeout, "HuxerUI HTTP request timed out"}), true);
  }

  void Finish(HttpResult result, bool cancel_task) noexcept {
    __strong NSURLSessionDataTask* task = nil;
    __strong dispatch_source_t timeout = nil;
    HttpTransportCompletion completion;
    {
      std::scoped_lock lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
      if (cancel_task) {
        task = task_;
      }
      task_ = nil;
      timeout = timeout_;
      timeout_ = nil;
      completion = std::move(completion_);
    }
    if (timeout != nil) {
      dispatch_source_cancel(timeout);
    }
    [task cancel];
    if (completion) {
      completion(std::move(result));
    }
  }

  std::mutex mutex_;
  __strong NSURLSession* session_;
  HttpRequest request_;
  HttpTransportCompletion completion_;
  __strong NSURLSessionDataTask* task_ = nil;
  __strong dispatch_source_t timeout_ = nil;
  bool finished_ = false;
};

class IosHttpTransport final : public HttpTransport {
public:
  IosHttpTransport() {
    NSURLSessionConfiguration* configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    configuration.URLCache = nil;
    configuration.HTTPCookieStorage = nil;
    configuration.HTTPShouldSetCookies = NO;
    configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    session_ = [NSURLSession sessionWithConfiguration:configuration];
  }

  ~IosHttpTransport() override {
    [session_ invalidateAndCancel];
  }

  std::function<void()> Start(HttpRequest request, HttpTransportCompletion completion) override {
    auto operation = std::make_shared<IosHttpRequest>(session_, std::move(request), std::move(completion));
    operation->Start();
    return [operation] { operation->Cancel(); };
  }

private:
  __strong NSURLSession* session_ = nil;
};

} // namespace

std::shared_ptr<HttpTransport> CreateIosHttpTransport() {
  return std::make_shared<IosHttpTransport>();
}

} // namespace huxerui::detail
