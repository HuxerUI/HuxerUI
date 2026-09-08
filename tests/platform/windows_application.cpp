#include <catch2/catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include "win32_application_internal.h"
#include "application/application_internal.h"

#if !defined(HUXERUI_WINDOWS_7_COMPAT)
#include <roapi.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/base.h>
#endif

namespace huxerui::test {

namespace {

namespace fs = std::filesystem;

class ActivationTemporaryDirectory final {
public:
  ActivationTemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence = 0;
    path_ = fs::temp_directory_path() / (L"huxerui-windows-activation-tests-" +
                                         std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                         L"-" + std::to_wstring(sequence.fetch_add(1)));
    REQUIRE(fs::create_directories(path_));
  }

  ~ActivationTemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  [[nodiscard]] const fs::path& Path() const noexcept {
    return path_;
  }

private:
  fs::path path_;
};

} // namespace

TEST_CASE("Win32ApplicationActivationPreservesLaunchAndUrlInputs") {
  REQUIRE(
      std::holds_alternative<LaunchActivation>(detail::ParseWin32ApplicationActivation(std::span<const std::wstring>{}))
  );

  const std::vector<std::wstring> url_arguments{L"huxerui://documents/%E6%B5%8B%E8%AF%95"};
  const ApplicationActivation url_activation = detail::ParseWin32ApplicationActivation(url_arguments);
  REQUIRE(std::get<UrlActivation>(url_activation).url.ToString() == "huxerui://documents/%E6%B5%8B%E8%AF%95");

  const std::vector<wchar_t> url_payload = detail::EncodeWin32ApplicationArguments(url_arguments);
  const std::optional<ApplicationActivation> decoded_url = detail::DecodeWin32ApplicationActivation(url_payload);
  REQUIRE(decoded_url.has_value());
  REQUIRE(std::get<UrlActivation>(*decoded_url).url.ToString() == "huxerui://documents/%E6%B5%8B%E8%AF%95");

  const std::vector<wchar_t> incomplete_payload{L'h', L'u', L'x', L'\0'};
  REQUIRE_FALSE(detail::DecodeWin32ApplicationActivation(incomplete_payload).has_value());

  const std::vector<std::wstring> option_arguments{L"--workspace", L"Design"};
  REQUIRE(std::holds_alternative<LaunchActivation>(detail::ParseWin32ApplicationActivation(option_arguments)));

  const std::vector<std::wstring> drive_relative_argument{LR"(C:missing.txt)"};
  REQUIRE(std::holds_alternative<LaunchActivation>(detail::ParseWin32ApplicationActivation(drive_relative_argument)));

  const std::vector<std::wstring> invalid_url_argument{L"huxerui://documents/测试"};
  REQUIRE(std::holds_alternative<LaunchActivation>(detail::ParseWin32ApplicationActivation(invalid_url_argument)));
  const std::vector<wchar_t> invalid_url_payload = detail::EncodeWin32ApplicationArguments(invalid_url_argument);
  REQUIRE_FALSE(detail::DecodeWin32ApplicationActivation(invalid_url_payload).has_value());
}

TEST_CASE("Win32ApplicationActivationCreatesCapabilitiesOnlyForCompleteFileInputs") {
  ActivationTemporaryDirectory temporary;
  const fs::path first_path = temporary.Path() / L"first.txt";
  const fs::path second_path = temporary.Path() / L"第二.txt";
  std::ofstream(first_path, std::ios::binary) << "first";
  std::ofstream(second_path, std::ios::binary) << "second";

  const std::vector<std::wstring> file_arguments{first_path.wstring(), second_path.wstring()};
  const ApplicationActivation activation = detail::ParseWin32ApplicationActivation(file_arguments);
  const FileActivation& files = std::get<FileActivation>(activation);
  REQUIRE(files.files.size() == 2);
  REQUIRE(files.files[0].Name() == "first.txt");
  REQUIRE(files.files[1].Name() == "第二.txt");

  const std::vector<wchar_t> file_payload = detail::EncodeWin32ApplicationArguments(file_arguments);
  const std::optional<ApplicationActivation> decoded_files = detail::DecodeWin32ApplicationActivation(file_payload);
  REQUIRE(decoded_files.has_value());
  REQUIRE(std::get<FileActivation>(*decoded_files).files.size() == 2);

  const std::vector<std::wstring> incomplete_arguments{
      first_path.wstring(),
      (temporary.Path() / L"missing.txt").wstring()
  };
  REQUIRE(std::holds_alternative<LaunchActivation>(detail::ParseWin32ApplicationActivation(incomplete_arguments)));

  const std::vector<std::wstring> directory_argument{temporary.Path().wstring()};
  REQUIRE(std::holds_alternative<LaunchActivation>(detail::ParseWin32ApplicationActivation(directory_argument)));
}

#if !defined(HUXERUI_WINDOWS_7_COMPAT)
namespace {

class NotificationXmlApartment final {
public:
  NotificationXmlApartment() : result_(RoInitialize(RO_INIT_SINGLETHREADED)) {
    if (result_ != RPC_E_CHANGED_MODE) {
      winrt::check_hresult(result_);
    }
  }
  ~NotificationXmlApartment() {
    if (SUCCEEDED(result_)) {
      RoUninitialize();
    }
  }
  NotificationXmlApartment(const NotificationXmlApartment&) = delete;
  NotificationXmlApartment& operator=(const NotificationXmlApartment&) = delete;

private:
  HRESULT result_;
};

} // namespace

TEST_CASE("Win32NotificationActivationPreservesIdentifierAndData") {
  const PlatformPayload data(PlatformPayload::Object{{"file", "archive.zip"}, {"bytes", 12345}});
  const detail::ResolvedLocalNotification notification{.identifier = "download/测试 & =",
                                                       .title = "Download <ready>",
                                                       .body = "Open \"archive.zip\" & inspect",
                                                       .data = detail::EncodeLocalNotificationData(data)};
  const std::wstring encoded = detail::EncodeWin32NotificationActivation(notification);
  const auto decoded = detail::DecodeWin32NotificationActivation(encoded);
  REQUIRE(decoded.has_value());
  REQUIRE(decoded->identifier == notification.identifier);
  REQUIRE(decoded->data == data);
  const auto xml = detail::BuildWin32NotificationXml(notification);
  REQUIRE(xml.has_value());
  REQUIRE(xml->find(L"Download &lt;ready&gt;") != xml->npos);
  REQUIRE(xml->find(L"&quot;archive.zip&quot; &amp;") != xml->npos);
  const std::vector<std::wstring> arguments{std::wstring(detail::win32_notification_payload_flag), encoded};
  const auto activation = detail::DecodeWin32ApplicationActivation(detail::EncodeWin32ApplicationArguments(arguments));
  REQUIRE(activation.has_value());
  REQUIRE(std::get<NotificationActivation>(*activation) == *decoded);
}

TEST_CASE("Win32NotificationRejectsMalformedOrOversizedContent") {
  const detail::ResolvedLocalNotification notification{
      .identifier = "example", .title = "Title", .data = detail::EncodeLocalNotificationData({})};
  const auto encoded = detail::EncodeWin32NotificationActivation(notification);
  REQUIRE_FALSE(detail::DecodeWin32NotificationActivation(L"huxerui.notification.v2:QQ==.QQ=="));
  REQUIRE_FALSE(detail::DecodeWin32NotificationActivation(encoded + L" "));
  REQUIRE_FALSE(detail::DecodeWin32NotificationActivation(encoded.substr(0, encoded.size() - 1)));
  REQUIRE_FALSE(detail::DecodeWin32NotificationActivation(std::wstring(5121, L'x')));
  auto invalid = notification;
  invalid.body = std::string(5120, 'x');
  REQUIRE_THROWS_AS(detail::BuildWin32NotificationXml(invalid), std::invalid_argument);
  invalid.body = std::string("bad\0text", 8);
  REQUIRE_THROWS_AS(detail::BuildWin32NotificationXml(invalid), std::invalid_argument);
  invalid = notification;
  invalid.identifier.clear();
  REQUIRE_THROWS_AS(detail::EncodeWin32NotificationActivation(invalid), std::invalid_argument);
  invalid = notification;
  invalid.data.clear();
  REQUIRE_THROWS_AS(detail::EncodeWin32NotificationActivation(invalid), std::invalid_argument);
}

TEST_CASE("Win32NotificationTemplateReceivesResolvedContentAndPreservesActivation") {
  const NotificationXmlApartment apartment;
  const PlatformPayload data(PlatformPayload::Object{{"file", "测试 & archive.zip"}, {"progress", 0.42}});
  const detail::ResolvedLocalNotification notification{
      .identifier = "download/测试", .title = "Resolved title", .body = "Resolved body",
      .presentation = TemplateNotificationPresentation{.identifier = "download"},
      .data = detail::EncodeLocalNotificationData(data)};
  int calls = 0;
  const windows::LocalNotificationTemplateProvider provider =
      [&](std::string_view identifier, std::string_view title, std::string_view body,
          const PlatformPayload& supplied_data) -> std::optional<std::string> {
    ++calls;
    REQUIRE(identifier == "download");
    REQUIRE(title == notification.title);
    REQUIRE(body == notification.body);
    REQUIRE(supplied_data == data);
    return "<toast><visual><binding template='ToastGeneric'><text>测试 &amp; archive.zip</text>"
           "<progress value='0.42' status='Downloading'/></binding></visual><audio silent='true'/></toast>";
  };
  const auto xml = detail::BuildWin32NotificationXml(notification, provider);
  REQUIRE(calls == 1);
  REQUIRE(xml.has_value());
  winrt::Windows::Data::Xml::Dom::XmlDocument document;
  document.LoadXml(*xml);
  REQUIRE(document.SelectSingleNode(L"/toast/visual/binding/text").InnerText() == L"测试 & archive.zip");
  REQUIRE(document.SelectSingleNode(L"/toast/visual/binding/progress/@value").InnerText() == L"0.42");
  REQUIRE(document.SelectSingleNode(L"/toast/audio/@silent").InnerText() == L"true");
  const auto activation = detail::DecodeWin32NotificationActivation(document.DocumentElement().GetAttribute(L"launch"));
  REQUIRE(activation.has_value());
  REQUIRE(activation->identifier == notification.identifier);
  REQUIRE(activation->data == data);
  const auto forwarded = detail::DecodeWin32ApplicationActivation(detail::EncodeWin32ApplicationArguments(
      std::vector<std::wstring>{std::wstring(detail::win32_notification_payload_flag),
                               std::wstring(document.DocumentElement().GetAttribute(L"launch"))}));
  REQUIRE(forwarded.has_value());
  REQUIRE(std::get<NotificationActivation>(*forwarded) == *activation);

  auto ordinary = notification;
  ordinary.presentation = DefaultNotificationPresentation{};
  REQUIRE(detail::BuildWin32NotificationXml(ordinary, provider).has_value());
  REQUIRE(calls == 1);
}

TEST_CASE("Win32NotificationTemplateDistinguishesUnavailableFromInvalidContent") {
  const NotificationXmlApartment apartment;
  const detail::ResolvedLocalNotification notification{
      .identifier = "download", .title = "Download",
      .presentation = TemplateNotificationPresentation{.identifier = "unknown"},
      .data = detail::EncodeLocalNotificationData({})};
  REQUIRE_FALSE(detail::BuildWin32NotificationXml(notification).has_value());
  const windows::LocalNotificationTemplateProvider unavailable =
      [](auto, auto, auto, const auto&) -> std::optional<std::string> { return std::nullopt; };
  REQUIRE_FALSE(detail::BuildWin32NotificationXml(notification, unavailable).has_value());
  const windows::LocalNotificationTemplateProvider failed =
      [](auto, auto, auto, const auto&) -> std::optional<std::string> {
    throw std::runtime_error("HuxerUI test template failure");
  };
  REQUIRE_THROWS_AS(detail::BuildWin32NotificationXml(notification, failed), std::runtime_error);

  const std::string layout = "<visual><binding template='ToastGeneric'><text>Download</text></binding></visual>";
  const std::vector<std::string> invalid{
      "", "<toast>", "<other>" + layout + "</other>", "<toast/>",
      "<toast xmlns='urn:other'>" + layout + "</toast>",
      "<toast>" + layout + layout + "</toast>",
      "<toast>" + layout + "<visual/></toast>",
      "<toast><visual><binding template='ToastText01'><text>Download</text></binding></visual></toast>",
      "<toast launch=''>" + layout + "</toast>",
      "<toast activationType='foreground'>" + layout + "</toast>",
      "<toast protocolActivationTargetApplicationPfn='other'>" + layout + "</toast>",
      "<toast>" + layout + "<actions><action content='Open' arguments='open'/></actions></toast>",
      "<toast>" + layout + "<input id='reply' type='text'/></toast>",
      "<toast>" + layout + "<header id='group' title='Group' arguments='open'/></toast>",
      "<!DOCTYPE toast [<!ENTITY value 'Download'>]><toast>" + layout + "</toast>",
      "<!DOCTYPE toast SYSTEM 'file:///huxerui-notification-test.dtd'><toast>" + layout + "</toast>",
      "<toast>" + layout + std::string(1, '\0') + "</toast>",
      "<toast>" + layout + std::string(1, '\xFF') + "</toast>",
      std::string(5121, 'x'),
  };
  for (const auto& xml : invalid) {
    CAPTURE(xml);
    const windows::LocalNotificationTemplateProvider provider =
        [&xml](auto, auto, auto, const auto&) -> std::optional<std::string> { return xml; };
    REQUIRE_THROWS(detail::BuildWin32NotificationXml(notification, provider));
  }
}

TEST_CASE("Win32NotificationTemplateChecksSizeAfterAddingActivationData") {
  const NotificationXmlApartment apartment;
  const detail::ResolvedLocalNotification notification{
      .identifier = "download", .title = "Download",
      .presentation = TemplateNotificationPresentation{.identifier = "download"},
      .data = detail::EncodeLocalNotificationData(std::string(1000, 'x'))};
  const std::string xml = "<toast><visual><binding template='ToastGeneric'><text>" + std::string(4000, 'x') +
                          "</text></binding></visual></toast>";
  REQUIRE(xml.size() < 5120);
  const windows::LocalNotificationTemplateProvider provider =
      [&xml](auto, auto, auto, const auto&) -> std::optional<std::string> { return xml; };
  REQUIRE_THROWS_AS(detail::BuildWin32NotificationXml(notification, provider), std::invalid_argument);
}
#endif

} // namespace huxerui::test
