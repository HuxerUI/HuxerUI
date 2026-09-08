#include "notification_template.h"

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <system_error>

#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/base.h>

namespace local_notification_example {

std::optional<std::string> BuildNotificationTemplate(std::string_view template_identifier, std::string_view title,
                                                    std::string_view body, const huxerui::PlatformPayload& data) {
  if (template_identifier != "huxerui.local-notification.download") {
    return std::nullopt;
  }
  // Progress arrived in Windows 10 version 1703; the ordinary SDK backend also supports version 1607.
  if (!winrt::Windows::Foundation::Metadata::ApiInformation::IsApiContractPresent(
          L"Windows.Foundation.UniversalApiContract", 4)) {
    return std::nullopt;
  }
  const auto& fields = data.AsObject();
  const auto file_name = fields.at("file_name").AsString();
  const auto status = fields.at("status").AsString();
  const auto downloaded = fields.at("downloaded_bytes").AsInteger();
  const auto& total_value = fields.at("total_bytes");
  const auto total = total_value.IsNull() ? 0 : total_value.AsInteger();
  const bool expanded = fields.at("expanded").AsBoolean();
  if (downloaded < 0 || total < 0) {
    throw std::invalid_argument("HuxerUI download template byte counts must not be negative");
  }
  std::string progress = "indeterminate";
  if (total > 0 || status != "Downloading") {
    const double fraction = status == "Complete" ? 1.0 : total > 0
        ? std::clamp(static_cast<double>(downloaded) / static_cast<double>(total), 0.0, 1.0) : 0.0;
    // XML numeric attributes use a decimal point even when the user's locale uses a comma.
    char text[32];
    const auto converted = std::to_chars(text, text + sizeof(text), fraction, std::chars_format::fixed, 4);
    if (converted.ec != std::errc{}) {
      throw std::runtime_error("HuxerUI could not format notification progress");
    }
    progress.assign(text, converted.ptr);
  }

  using namespace winrt::Windows::Data::Xml::Dom;
  XmlDocument document;
  document.LoadXml(L"<toast><visual><binding template='ToastGeneric'/></visual><audio silent='true'/></toast>");
  const auto binding = document.SelectSingleNode(L"/toast/visual/binding");
  const auto append_text = [&](std::string_view value) {
    auto element = document.CreateElement(L"text");
    // DOM text nodes and attributes escape file names and other application values, including XML metacharacters.
    element.InnerText(winrt::to_hstring(value));
    binding.AppendChild(element);
  };
  append_text(file_name);
  append_text(body);
  if (expanded) {
    append_text(title);
  }
  auto bar = document.CreateElement(L"progress");
  bar.SetAttribute(L"value", winrt::to_hstring(progress));
  bar.SetAttribute(L"status", winrt::to_hstring(status));
  if (expanded) {
    std::string counts = std::to_string(downloaded / 1024) + " KiB";
    if (total > 0) {
      counts += " / " + std::to_string(total / 1024) + " KiB";
    }
    bar.SetAttribute(L"valueStringOverride", winrt::to_hstring(counts));
  }
  binding.AppendChild(bar);
  // HuxerUI injects primary activation data after this callback; the template must not assign launch or actions.
  return winrt::to_string(document.GetXml());
}

} // namespace local_notification_example
