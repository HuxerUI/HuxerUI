#include "runtime_test_support.h"

#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <huxerui/file_drop.h>

#include "io/file_internal.h"

namespace huxerui::test {
namespace {

std::vector<std::string> file_drop_events;
std::vector<FileReference> received_files;
std::optional<FileDropEvent> received_position;
std::optional<FileError> received_error;
State<bool> file_drop_present;
State<bool> file_drop_enabled;
State<FileDropOptions> file_drop_options;
State<int> file_drop_handler;
State<Point> file_drop_translation;
bool file_drop_throw_predicate = false;
bool file_drop_throw_entered = false;
bool file_drop_throw_exited = false;
bool file_drop_nested = false;
bool file_drop_child_accepts = true;

class DroppedFileState final : public detail::FileReferenceState {
public:
  std::function<void()> ReadBytes(detail::FileReferenceBytesCompletion completion) override {
    ++reads;
    completion(FileResult<Bytes>(Bytes{}));
    return {};
  }
  std::function<void()> ImportTo(File, bool, detail::FileReferenceCompletion<std::uint64_t> completion) override {
    completion(FileResult<std::uint64_t>(0));
    return {};
  }
  std::function<void()> ReplaceWith(File, detail::FileReferenceBoolCompletion completion) override {
    completion(false);
    return {};
  }
  int reads = 0;
};

FileReference DroppedFile(std::string name = "photo.PNG", std::optional<std::string> content_type = "image/png",
                          FileType type = FileType::File,
                          std::shared_ptr<DroppedFileState> access = std::make_shared<DroppedFileState>()) {
  return detail::MakeFileReference({.name = std::move(name), .content_type = std::move(content_type), .type = type},
                                   std::move(access));
}

struct ManualFileDrop {
  detail::FileDropCompletion completion;
  int starts = 0;
  int cancellations = 0;

  detail::FileDropPreparation Source() {
    return {[this](detail::FileDropCompletion callback) {
      ++starts;
      completion = std::move(callback);
      return [this] { ++cancellations; };
    }};
  }

  void Complete(std::vector<FileReference> files = {DroppedFile()}) {
    completion(FileResult<std::vector<FileReference>>(std::move(files)));
  }
};

View FileDropApp() {
  file_drop_present = UseState(true);
  file_drop_enabled = UseState(true);
  file_drop_options = UseState(FileDropOptions{});
  file_drop_handler = UseState(0);
  file_drop_translation = UseState(Point{});
  auto target = Text("Files").With(huxerui::Frame{100.0F, 100.0F});
  if (file_drop_present.Get()) {
    target = std::move(target)
        .With(FileDropTarget::Accepts(file_drop_options.Get(), [](const FileDropOffer&) {
          if (file_drop_throw_predicate) {
            throw std::runtime_error("test predicate failure");
          }
          return !file_drop_nested || file_drop_child_accepts;
        }))
        .On<FileDropEvents::Entered>([](const FileDropOffer&, const FileDropEvent&) {
          file_drop_events.emplace_back("entered");
          if (file_drop_throw_entered) {
            throw std::runtime_error("test entered failure");
          }
        })
        .On<FileDropEvents::Moved>([](const FileDropOffer&, const FileDropEvent&) {
          file_drop_events.emplace_back("moved");
        })
        .On<FileDropEvents::Exited>([](const FileDropOffer&, const FileDropEvent&) {
          file_drop_events.emplace_back("exited");
          if (file_drop_throw_exited) {
            throw std::runtime_error("test exited failure");
          }
        })
        .On<FileDropEvents::Dropped>([version = file_drop_handler.Get()](const auto& files, const auto& event) {
          file_drop_events.push_back("dropped " + std::to_string(version));
          received_files.insert(received_files.end(), files.begin(), files.end());
          received_position = event;
        })
        .On<FileDropEvents::Failed>([](const FileError& error, const FileDropEvent& event) {
          file_drop_events.emplace_back("failed");
          received_error = error;
          received_position = event;
        });
  }
  target = std::move(target).With(Enabled(file_drop_enabled.Get()), Offset(file_drop_translation.Get()));
  if (file_drop_nested) {
    return Column {std::move(target)}
        .With(FileDropTarget::Accepts())
        .On<FileDropEvents::Entered>([](const auto&, const auto&) { file_drop_events.emplace_back("parent entered"); })
        .On<FileDropEvents::Exited>([](const auto&, const auto&) { file_drop_events.emplace_back("parent exited"); });
  }
  return Row {Text("Spacer").With(huxerui::Frame{50.0F, 100.0F}), std::move(target)};
}

struct FileDropFixture {
  FileDropFixture() {
    file_drop_events.clear();
    received_files.clear();
    received_position.reset();
    received_error.reset();
    file_drop_throw_predicate = false;
    file_drop_throw_entered = false;
    file_drop_throw_exited = false;
    file_drop_nested = false;
    file_drop_child_accepts = true;
  }
};

void MountDropApp(Runtime& runtime) {
  runtime.SetWindowMetrics({.viewport = {250.0F, 160.0F}});
  runtime.BuildFrame();
}

constexpr Point drop_point{75.0F, 40.0F};

} // namespace

TEST_CASE("File drop validates extension and MIME configuration before mounting") {
  for (const auto& extension : std::vector<std::string>{"", ".png", "*.png", "a/b", "a\\b", "a;b", "a?b",
                                                      std::string("a\0b", 3)}) {
    REQUIRE_THROWS_AS(FileDropTarget::Accepts({.extensions = {extension}}), std::invalid_argument);
  }
  for (const auto* mime : {"image", "/png", "image/", "image/png;x=y", "*/png", "im*age/png", "im*age/*"}) {
    REQUIRE_THROWS_AS(FileDropTarget::Accepts({.content_types = {mime}}), std::invalid_argument);
  }
  REQUIRE_NOTHROW(FileDropTarget::Accepts({.extensions = {"tar.gz"}, .content_types = {"IMAGE/*", "*/*"}}));
}

TEST_CASE_METHOD(FileDropFixture, "File drop final filters use suffix or MIME matching and reject whole batches") {
  FileDropOptions options{.extensions = {"tar.gz"}, .content_types = {"image/*"}};
  std::vector<FileReference> files;
  std::optional<FileErrorCode> error;
  SECTION("compound suffix ignores case") { files = {DroppedFile("ARCHIVE.TAR.GZ", {})}; }
  SECTION("long file names retain their storage") { files = {DroppedFile(std::string(256, 'a') + ".TAR.GZ", {})}; }
  SECTION("MIME matching ignores case") { files = {DroppedFile("image", "IMAGE/PNG")}; }
  SECTION("mixed eligibility rejects the complete batch") {
    files = {DroppedFile(), DroppedFile("notes.txt", "text/plain")};
    error = FileErrorCode::Unsupported;
  }
  SECTION("a suffix needs its leading separator") {
    files = {DroppedFile("tar.gz", {})};
    error = FileErrorCode::Unsupported;
  }
  SECTION("unknown metadata cannot satisfy a specific filter") {
    files = {DroppedFile("unknown", {})};
    error = FileErrorCode::Unsupported;
  }
  SECTION("empty batch is rejected without filters") {
    options = {};
    error = FileErrorCode::Unsupported;
  }
  SECTION("directories are not ordinary files") {
    options = {};
    files = {DroppedFile("folder", {}, FileType::Directory)};
    error = FileErrorCode::IsDirectory;
  }
  SECTION("other item kinds are rejected") {
    options = {};
    files = {DroppedFile("pipe", {}, FileType::Other)};
    error = FileErrorCode::Unsupported;
  }
  SECTION("single-file policy rejects the complete multiple-file batch") {
    options = {.allows_multiple = false};
    files = {DroppedFile(), DroppedFile()};
    error = FileErrorCode::Unsupported;
  }
  SECTION("unrestricted MIME wildcard accepts unknown metadata") {
    options = {.content_types = {"*/*"}};
    files = {DroppedFile("unknown", {})};
  }

  TestPlatform platform;
  ManualFileDrop source;
  Runtime runtime{FileDropApp, platform};
  MountDropApp(runtime);
  file_drop_options = std::move(options);
  runtime.BuildFrame();
  REQUIRE(runtime.CoreRuntime().HandleFileDragEntered(1, {}, drop_point));
  REQUIRE(runtime.CoreRuntime().HandleFileDrop(1, {}, drop_point, source.Source()));
  source.Complete(files);
  platform.RunPlatformModuleTasks();
  if (error) {
    REQUIRE(received_error.has_value());
    REQUIRE(received_error->code == *error);
    REQUIRE(received_files.empty());
    REQUIRE(file_drop_events.back() == "failed");
  } else {
    REQUIRE_FALSE(received_error.has_value());
    REQUIRE(received_files.size() == files.size());
    for (std::size_t index = 0; index < files.size(); ++index) {
      REQUIRE(received_files[index].Name() == files[index].Name());
    }
    REQUIRE(file_drop_events.back() == "dropped 0");
  }
}

TEST_CASE_METHOD(FileDropFixture, "File drop offers retain unknown metadata and enforce known count") {
  TestPlatform platform;
  Runtime runtime{FileDropApp, platform};
  MountDropApp(runtime);
  file_drop_options = FileDropOptions{.content_types = {"image/*"}, .allows_multiple = false};
  runtime.BuildFrame();
  REQUIRE(runtime.CoreRuntime().HandleFileDragEntered(1, {}, drop_point));
  REQUIRE_FALSE(runtime.CoreRuntime().HandleFileDragMoved(1, {.item_count = 2}, drop_point));
  REQUIRE(file_drop_events == std::vector<std::string>{"entered", "exited"});
  REQUIRE(runtime.CoreRuntime().HandleFileDragMoved(1, {.item_count = 1}, drop_point));
  runtime.CoreRuntime().HandleFileDragExited(1);
  REQUIRE_FALSE(runtime.CoreRuntime().HandleFileDragMoved(1, {}, drop_point));
  REQUIRE_FALSE(runtime.CoreRuntime().HandleFileDragEntered(1, {}, drop_point));
  REQUIRE_FALSE(runtime.CoreRuntime().HandleFileDragEntered(0, {}, drop_point));
}

TEST_CASE_METHOD(FileDropFixture, "File drop separates hover from deferred successful delivery") {
  TestPlatform platform;
  Runtime runtime{FileDropApp, platform};
  MountDropApp(runtime);
  auto access = std::make_shared<DroppedFileState>();
  REQUIRE(runtime.CoreRuntime().HandleFileDragEntered(1, {}, drop_point));
  const detail::FileDropPreparation source{[access](detail::FileDropCompletion completion) {
    completion(FileResult<std::vector<FileReference>>({DroppedFile("photo.png", "image/png", FileType::File, access)}));
    return std::function<void()>{};
  }};
  REQUIRE(runtime.CoreRuntime().HandleFileDrop(1, {}, drop_point, source));
  REQUIRE(file_drop_events == std::vector<std::string>{"entered", "moved", "exited"});
  REQUIRE(received_files.empty());
  platform.RunPlatformModuleTasks();
  REQUIRE(file_drop_events.back() == "dropped 0");
  REQUIRE(received_files.size() == 1);
  REQUIRE(received_position->position == Point{25.0F, 40.0F});
  REQUIRE(received_position->window_position == drop_point);
  REQUIRE(access->reads == 0);
  REQUIRE_FALSE(received_files.front().CanWrite());
}

TEST_CASE_METHOD(FileDropFixture, "File drop final rejection delivers failure without a partial batch") {
  TestPlatform platform;
  Runtime runtime{FileDropApp, platform};
  MountDropApp(runtime);
  file_drop_options = FileDropOptions{.extensions = {"png"}};
  runtime.BuildFrame();
  ManualFileDrop source;
  REQUIRE(runtime.CoreRuntime().HandleFileDragEntered(1, {}, drop_point));
  REQUIRE(runtime.CoreRuntime().HandleFileDrop(1, {}, drop_point, source.Source()));
  source.Complete({DroppedFile(), DroppedFile("notes.txt", "text/plain")});
  platform.RunPlatformModuleTasks();
  REQUIRE(received_files.empty());
  REQUIRE(received_error->code == FileErrorCode::Unsupported);
  REQUIRE(file_drop_events.back() == "failed");
  source.Complete();
  platform.RunPlatformModuleTasks();
  REQUIRE(received_files.empty());
}

TEST_CASE_METHOD(FileDropFixture, "File drop freezes acceptance and position but uses compatible current handlers") {
  TestPlatform platform;
  Runtime runtime{FileDropApp, platform};
  MountDropApp(runtime);
  file_drop_translation = Point{10.0F, 5.0F};
  file_drop_options = FileDropOptions{.extensions = {"png"}};
  runtime.BuildFrame();
  ManualFileDrop source;
  REQUIRE(runtime.CoreRuntime().HandleFileDragEntered(1, {}, drop_point));
  REQUIRE(runtime.CoreRuntime().HandleFileDrop(1, {}, drop_point, source.Source()));
  file_drop_handler = 1;
  file_drop_options = FileDropOptions{.extensions = {"txt"}};
  file_drop_translation = Point{40.0F, 15.0F};
  file_drop_enabled = false;
  runtime.BuildFrame();
  source.Complete();
  platform.RunPlatformModuleTasks();
  REQUIRE(file_drop_events.back() == "dropped 1");
  REQUIRE(received_position->position == Point{15.0F, 35.0F});
  REQUIRE(source.cancellations == 0);
}

TEST_CASE_METHOD(FileDropFixture, "File drop cancels removed targets and ignores late completion") {
  TestPlatform platform;
  ManualFileDrop source;
  Runtime runtime{FileDropApp, platform};
  MountDropApp(runtime);
  REQUIRE(runtime.CoreRuntime().HandleFileDragEntered(1, {}, drop_point));
  REQUIRE(runtime.CoreRuntime().HandleFileDrop(1, {}, drop_point, source.Source()));
  file_drop_present = false;
  runtime.BuildFrame();
  REQUIRE(source.cancellations == 1);
  file_drop_present = true;
  runtime.BuildFrame();
  source.Complete();
  platform.RunPlatformModuleTasks();
  REQUIRE(received_files.empty());
}

TEST_CASE_METHOD(FileDropFixture, "File drop teardown cancels preparation and discards already queued completion") {
  TestPlatform platform;
  ManualFileDrop source;
  {
    Runtime runtime{FileDropApp, platform};
    MountDropApp(runtime);
    REQUIRE(runtime.CoreRuntime().HandleFileDragEntered(1, {}, drop_point));
    REQUIRE(runtime.CoreRuntime().HandleFileDrop(1, {}, drop_point, source.Source()));
    source.Complete();
  }
  REQUIRE(source.cancellations == 1);
  platform.RunPlatformModuleTasks();
  REQUIRE(received_files.empty());
}

TEST_CASE_METHOD(FileDropFixture, "File drop concurrent pending deliveries do not affect newer hover") {
  TestPlatform platform;
  ManualFileDrop first;
  ManualFileDrop second;
  Runtime runtime{FileDropApp, platform};
  MountDropApp(runtime);
  REQUIRE(runtime.CoreRuntime().HandleFileDragEntered(1, {}, drop_point));
  REQUIRE(runtime.CoreRuntime().HandleFileDrop(1, {}, drop_point, first.Source()));
  REQUIRE(runtime.CoreRuntime().HandleFileDragEntered(2, {}, drop_point));
  REQUIRE(runtime.CoreRuntime().HandleFileDrop(2, {}, drop_point, second.Source()));
  REQUIRE(runtime.CoreRuntime().HandleFileDragEntered(3, {}, drop_point));
  runtime.CoreRuntime().HandleFileDragExited(2);
  second.Complete({DroppedFile("second.png")});
  first.Complete({DroppedFile("first.png")});
  platform.RunPlatformModuleTasks();
  REQUIRE(received_files.size() == 2);
  REQUIRE(received_files[0].Name() == "second.png");
  REQUIRE(received_files[1].Name() == "first.png");
  REQUIRE(runtime.CoreRuntime().HandleFileDragMoved(3, {}, drop_point));
  runtime.CoreRuntime().HandleFileDragExited(3);
  REQUIRE(file_drop_events.back() == "exited");
}

TEST_CASE_METHOD(FileDropFixture, "File drop preparation may complete off the Runtime thread") {
  std::vector<std::function<void()>> queued;
  TestPlatform platform{[&queued](auto task) { queued.push_back(std::move(task)); }};
  ManualFileDrop source;
  Runtime runtime{FileDropApp, platform};
  MountDropApp(runtime);
  REQUIRE(runtime.CoreRuntime().HandleFileDragEntered(1, {}, drop_point));
  REQUIRE(runtime.CoreRuntime().HandleFileDrop(1, {}, drop_point, source.Source()));
  std::thread worker([&source] { source.Complete(); });
  worker.join();
  REQUIRE(received_files.empty());
  REQUIRE(queued.size() == 1);
  queued.front()();
  REQUIRE(received_files.size() == 1);
}

TEST_CASE_METHOD(FileDropFixture, "File drop nested targets fall back and update stationary hover") {
  file_drop_nested = true;
  file_drop_child_accepts = false;
  TestPlatform platform;
  Runtime runtime{FileDropApp, platform};
  MountDropApp(runtime);
  REQUIRE(runtime.CoreRuntime().HandleFileDragEntered(1, {}, {30.0F, 30.0F}));
  REQUIRE(file_drop_events.back() == "parent entered");
  file_drop_child_accepts = true;
  runtime.BuildFrame();
  REQUIRE(file_drop_events == std::vector<std::string>{"parent entered", "parent exited", "entered"});
  file_drop_enabled = false;
  runtime.BuildFrame();
  REQUIRE(file_drop_events.back() == "parent entered");
}

TEST_CASE_METHOD(FileDropFixture, "File drop handler and predicate exceptions quarantine hover") {
  TestPlatform platform;
  Runtime runtime{FileDropApp, platform};
  MountDropApp(runtime);
  SECTION("predicate") {
    file_drop_throw_predicate = true;
    REQUIRE_THROWS(runtime.CoreRuntime().HandleFileDragEntered(1, {}, drop_point));
    REQUIRE(file_drop_events.empty());
  }
  SECTION("entered and cleanup") {
    file_drop_throw_entered = true;
    file_drop_throw_exited = true;
    REQUIRE_THROWS_WITH(runtime.CoreRuntime().HandleFileDragEntered(1, {}, drop_point), "test entered failure");
    REQUIRE(file_drop_events == std::vector<std::string>{"entered", "exited"});
  }
  REQUIRE_FALSE(runtime.CoreRuntime().HandleFileDragMoved(1, {}, drop_point));
}

TEST_CASE_METHOD(FileDropFixture, "File drop rejects invalid coordinates and does not start unaccepted sources") {
  TestPlatform platform;
  ManualFileDrop source;
  Runtime runtime{FileDropApp, platform};
  MountDropApp(runtime);
  REQUIRE_FALSE(runtime.CoreRuntime().HandleFileDragEntered(1, {}, {std::numeric_limits<float>::quiet_NaN(), 0.0F}));
  REQUIRE_FALSE(runtime.CoreRuntime().HandleFileDrop(1, {}, {240.0F, 140.0F}, source.Source()));
  REQUIRE(source.starts == 0);
  REQUIRE(file_drop_events.empty());
}

TEST_CASE_METHOD(FileDropFixture, "File drop auto-scroll follows the clipped route and stops on exit") {
  static std::optional<ScrollController> scroll;
  scroll.reset();
  TestPlatform platform;
  Runtime runtime{[]() -> View {
    scroll = UseScrollController();
    return ScrollView {
      Text("Files").With(huxerui::Frame{100.0F, 300.0F}, FileDropTarget::Accepts()),
    }.Controller(*scroll).With(huxerui::Frame{100.0F, 100.0F});
  }, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();
  REQUIRE_FALSE(runtime.CoreRuntime().HandleFileDragEntered(1, {}, {50.0F, 110.0F}));
  REQUIRE(runtime.CoreRuntime().HandleFileDragMoved(1, {}, {50.0F, 96.0F}));
  platform.AdvanceTime(0.05);
  runtime.BuildFrame();
  REQUIRE(scroll->Offset() > 0.0F);
  runtime.CoreRuntime().HandleFileDragExited(1);
  const float offset = scroll->Offset();
  platform.AdvanceTime(0.05);
  runtime.BuildFrame();
  REQUIRE(scroll->Offset() == offset);
}

TEST_CASE_METHOD(FileDropFixture, "File drop reports preparation exceptions and accepts subsequent drops") {
  TestPlatform platform;
  ManualFileDrop next;
  Runtime runtime{FileDropApp, platform};
  MountDropApp(runtime);
  REQUIRE(runtime.CoreRuntime().HandleFileDragEntered(1, {}, drop_point));
  REQUIRE(runtime.CoreRuntime().HandleFileDrop(1, {}, drop_point, [](auto) -> std::function<void()> {
    throw std::runtime_error("test preparation failure");
  }));
  REQUIRE_FALSE(received_error.has_value());
  platform.RunPlatformModuleTasks();
  REQUIRE(received_error->code == FileErrorCode::Io);
  REQUIRE(runtime.CoreRuntime().HandleFileDragEntered(2, {}, drop_point));
  REQUIRE(runtime.CoreRuntime().HandleFileDrop(2, {}, drop_point, next.Source()));
  next.Complete();
  next.Complete();
  platform.RunPlatformModuleTasks();
  REQUIRE(received_files.size() == 1);
}

} // namespace huxerui::test
