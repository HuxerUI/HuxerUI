#include <huxerui/huxerui.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

using namespace huxerui;

std::string Utf8Text(std::span<const std::byte> bytes) {
  return bytes.empty() ? std::string{} : std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

constexpr auto download_url = "https://httpbingo.org/bytes/262144";

struct RequestState {
  bool loading {false};
  std::string status {"Ready"};
  std::string body {"Press Send to perform an HTTPS GET request."};
};

struct DownloadState {
  bool downloading {false};
  bool complete {false};
  std::uint64_t transferred_bytes = 0;
  std::optional<std::uint64_t> total_bytes;
  std::string status {"Ready"};
  std::string detail {"Press Download to stream 256 KiB into application temporary storage."};
};

ProgressBar DownloadProgressBar(const DownloadState& download) {
  if (download.complete) {
    return ProgressBar(1.0F);
  }
  if (!download.total_bytes || *download.total_bytes == 0) {
    return download.downloading ? ProgressBar() : ProgressBar(0.0F);
  }
  const double fraction =
      static_cast<double>(download.transferred_bytes) / static_cast<double>(*download.total_bytes);
  return ProgressBar(static_cast<float>(std::clamp(fraction, 0.0, 1.0)));
}

std::string DownloadProgressText(const DownloadState& download) {
  if (!download.total_bytes || *download.total_bytes == 0) {
    return std::to_string(download.transferred_bytes) + " bytes downloaded";
  }
  const double fraction =
      static_cast<double>(download.transferred_bytes) / static_cast<double>(*download.total_bytes);
  const int percent = static_cast<int>(std::clamp(fraction, 0.0, 1.0) * 100.0);
  return std::to_string(download.transferred_bytes) + " / " + std::to_string(*download.total_bytes) + " bytes (" +
         std::to_string(percent) + "%)";
}

Task<void> DownloadFile(std::shared_ptr<HttpClient> http, File output, State<DownloadState> download) {
  download = {
    .downloading = true,
    .status = "Opening response",
    .detail = "Waiting for response headers...",
  };

  HttpStreamResult opened = co_await http->SendStream(
      {
          .url = download_url,
          .headers = {{"Accept", "application/octet-stream"}},
      },
      [download](HttpProgress progress) {
        if (progress.kind != HttpProgressKind::Download) {
          return;
        }
        download.Update([progress](DownloadState& state) {
          state.transferred_bytes = progress.transferred_bytes;
          state.total_bytes = progress.total_bytes;
        });
      }
  );
  if (!opened.HasResponse()) {
    download.Update([message = opened.Error().message](DownloadState& state) {
      state.downloading = false;
      state.status = "Request failed";
      state.detail = message;
    });
    co_return;
  }

  HttpResponseStream stream = std::move(opened).Response();
  const int status_code = stream.StatusCode();
  if (status_code < 200 || status_code >= 300) {
    download.Update([status_code](DownloadState& state) {
      state.downloading = false;
      state.status = "Download rejected";
      state.detail = "The server returned HTTP " + std::to_string(status_code) + ".";
    });
    co_return;
  }

  bool wrote_data = false;
  while (true) {
    HttpStreamReadResult read = co_await stream.Read();
    if (read.HasData()) {
      Bytes chunk = std::move(read).Data();
      const bool written = wrote_data ? co_await output.AppendBytesAsync(std::move(chunk))
                                      : co_await output.WriteBytesAsync(std::move(chunk));
      if (!written) {
        download.Update([](DownloadState& state) {
          state.downloading = false;
          state.status = "File write failed";
          state.detail = "The downloaded bytes could not be written to temporary storage.";
        });
        co_return;
      }
      wrote_data = true;
    } else if (read.HasError()) {
      download.Update([message = read.Error().message](DownloadState& state) {
        state.downloading = false;
        state.status = "Download failed";
        state.detail = message;
      });
      co_return;
    } else {
      if (!wrote_data && !co_await output.WriteBytesAsync({})) {
        download.Update([](DownloadState& state) {
          state.downloading = false;
          state.status = "File write failed";
          state.detail = "The empty response could not be written to temporary storage.";
        });
        co_return;
      }
      const std::string path = output.Path();
      download.Update([path, status_code](DownloadState& state) {
        state.downloading = false;
        state.complete = true;
        state.status = "HTTP " + std::to_string(status_code) + " · Download complete";
        state.detail = "Saved to " + path;
      });
      co_return;
    }
  }
}

[[huxerui::composable]]
View HttpContent() {
  auto http = UseService<HttpClient>();
  auto file_system = UseService<FileSystem>();
  auto tasks = UseTaskScope();
  auto request = UseState(RequestState{});
  auto download = UseState(DownloadState{});
  auto& theme = UseTheme();
  const File download_file = file_system->Directories().temporary_directory.Child("huxerui-http-download.bin");

  return ScrollView {
    Column {
      Text("HTTP", TextRole::Title),
      Text("HttpClient performs a platform request and resumes its Task on the owning UI thread."),
      Text("Buffered response", TextRole::Title),
      Text("https://httpbingo.org/get", TextRole::Label).With(Foreground(theme.colors.primary)),
      Button(request->loading ? "Loading..." : "Send").With(Enabled(!request->loading)).OnClick([=] {
        tasks.Launch([=]() -> Task<void> {
          request = {true, "Loading", "Waiting for the response..."};

          HttpResult result = co_await http->Send({
              .url = "https://httpbingo.org/get",
              .headers = {{"Accept", "application/json"}},
          });
          if (result.HasResponse()) {
            HttpResponse& response = result.Response();
            request = {
              false, "HTTP " + std::to_string(response.status_code) + " · " + response.url,
              response.body.empty() ? "(empty response body)" : Utf8Text(response.body)
            };
          } else {
            request = {false, "Request failed", result.Error().message};
          }
        });
      }),
      Text(request->status, TextRole::Label),
      SelectionArea {
        Text(request->body),
      }.With(
          Padding(theme.spacing.large),
          Background(theme.colors.surface_container_low),
          CornerRadius(theme.shapes.medium)
      ),
      Text("Streaming download", TextRole::Title),
      Text("SendStream reads a binary response in chunks and writes each chunk to temporary storage."),
      Text(download_url, TextRole::Label).With(Foreground(theme.colors.primary)),
      Button(download->downloading ? "Downloading..." : "Download")
          .With(Enabled(!download->downloading))
          .OnClick([=] { tasks.Launch(DownloadFile(http, download_file, download)); }),
      DownloadProgressBar(download.Get()),
      Text(DownloadProgressText(download.Get()), TextRole::Label),
      Text(download->status, TextRole::Label),
      SelectionArea {
        Text(download->detail),
      }.With(
          Padding(theme.spacing.large),
          Background(theme.colors.surface_container_low),
          CornerRadius(theme.shapes.medium)
      ),
    }.With(
        Padding(theme.spacing.extra_large),
        Spacing(theme.spacing.medium),
        CrossAlign(CrossAxisAlignment::Stretch)
    ),
  }.With(
      ScrollBar(),
      Background(theme.colors.background)
  );
}

View App() {
  return MaterialTheme {HttpContent()};
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI HTTP",
            .initial_size = {760.0F, 620.0F},
        },
    }
};
