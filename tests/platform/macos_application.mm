#import <AppKit/AppKit.h>

#include <catch2/catch_amalgamated.hpp>

#include <optional>
#include <variant>
#include <vector>

#include "macos_application_internal.h"

namespace huxerui::test {

namespace {

NSURL* MakeActivationTemporaryDirectory() {
  NSString* name = [@"huxerui-mac-activation-tests-" stringByAppendingString:NSUUID.UUID.UUIDString];
  return [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:name] isDirectory:YES];
}

} // namespace

TEST_CASE("MacGestureDefaultsUseTheSystemDoubleClickInterval") {
  const GestureSettings settings = detail::MacGestureDefaults();

  REQUIRE(settings.multi_tap_interval.count() == Catch::Approx([NSEvent doubleClickInterval]));
}

TEST_CASE("MacApplicationActivationPreservesOrderedUrlsAndFileCapabilities") {
  @autoreleasepool {
    NSURL* directory = MakeActivationTemporaryDirectory();
    REQUIRE([NSFileManager.defaultManager createDirectoryAtURL:directory
                                   withIntermediateDirectories:YES
                                                    attributes:nil
                                                         error:nil]);
    NSURL* first_file = [directory URLByAppendingPathComponent:@"first.txt"];
    NSURL* second_file = [directory URLByAppendingPathComponent:@"第二.txt"];
    NSURL* third_file = [directory URLByAppendingPathComponent:@"third.txt"];
    REQUIRE([@"first" writeToURL:first_file atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    REQUIRE([@"second" writeToURL:second_file atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    REQUIRE([@"third" writeToURL:third_file atomically:YES encoding:NSUTF8StringEncoding error:nil]);

    NSURL* first_url = [NSURL URLWithString:@"huxerui-example://documents/%E6%B5%8B%E8%AF%95"];
    NSURL* second_url = [NSURL URLWithString:@"huxerui-example://documents/42"];
    std::optional<std::vector<ApplicationActivation>> activations =
        detail::DecodeMacApplicationActivations(@[ first_file, second_file, first_url, second_url, third_file ]);
    REQUIRE(activations.has_value());
    REQUIRE(activations->size() == 4);

    const FileActivation& first_files = std::get<FileActivation>((*activations)[0]);
    REQUIRE(first_files.files.size() == 2);
    REQUIRE(first_files.files[0].Name() == "first.txt");
    REQUIRE(first_files.files[1].Name() == "第二.txt");
    REQUIRE(
        std::get<UrlActivation>((*activations)[1]).url ==
        "huxerui-example://documents/%E6%B5%8B%E8%AF%95"
    );
    REQUIRE(std::get<UrlActivation>((*activations)[2]).url == "huxerui-example://documents/42");
    const FileActivation& last_files = std::get<FileActivation>((*activations)[3]);
    REQUIRE(last_files.files.size() == 1);
    REQUIRE(last_files.files[0].Name() == "third.txt");

    REQUIRE([NSFileManager.defaultManager removeItemAtURL:directory error:nil]);
  }
}

TEST_CASE("MacApplicationActivationRejectsIncompleteNativeInput") {
  @autoreleasepool {
    NSURL* directory = MakeActivationTemporaryDirectory();
    REQUIRE([NSFileManager.defaultManager createDirectoryAtURL:directory
                                   withIntermediateDirectories:YES
                                                    attributes:nil
                                                         error:nil]);
    NSURL* url = [NSURL URLWithString:@"huxerui-example://documents/42"];

    REQUIRE_FALSE(detail::DecodeMacApplicationActivations(@[ url, directory ]).has_value());
    REQUIRE_FALSE(detail::DecodeMacApplicationActivations(@[ url, @"not a URL" ]).has_value());
    REQUIRE(detail::DecodeMacApplicationActivations(@[])->empty());
    REQUIRE_FALSE(detail::DecodeMacApplicationActivations(nil).has_value());

    REQUIRE([NSFileManager.defaultManager removeItemAtURL:directory error:nil]);
  }
}

} // namespace huxerui::test
