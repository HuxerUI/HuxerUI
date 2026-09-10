// mcpp's contract is the exit code: this program passes by returning 0.

import std;
import huxerui.rules.sources;

namespace {

int failures = 0;

void check(bool condition, std::string_view what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

using huxerui::rules::sources::matches;
using huxerui::rules::sources::needs_codegen;
using huxerui::rules::sources::identifier;
using huxerui::rules::sources::cmake_list;
using huxerui::rules::sources::macro_definition;
using huxerui::rules::sources::public_names;
using huxerui::rules::sources::scan_module_interface;
using huxerui::rules::sources::strip_noncode;
using huxerui::rules::sources::umbrella_includes;
using huxerui::rules::sources::wix_paths;
using huxerui::rules::sources::without_entry;

void glob_single_star_stays_within_one_segment() {
    check(matches("src/*/*.cpp", "src/runtime/view.cpp"), "one directory matches src/*/*.cpp");
    // The defect this exists for: `*` must NOT cross a separator, or
    // `src/*/*.cpp` selects the same set as `src/**/*.cpp` and the manifest's
    // two spellings stop meaning different things.
    check(!matches("src/*/*.cpp", "src/runtime/detail/view.cpp"),
          "two directories do not match src/*/*.cpp");
    check(!matches("src/*.cpp", "src/runtime/view.cpp"), "src/*.cpp is one segment");
}

void glob_double_star_crosses_separators() {
    check(matches("src/**/*.cpp", "src/runtime/view.cpp"), "one directory matches src/**/*.cpp");
    check(matches("src/**/*.cpp", "src/a/b/c/view.cpp"), "three directories match src/**/*.cpp");
    // `**/` matches zero directories too, which is what makes a single glob
    // cover a flat project and a nested one.
    check(matches("src/**/*.cpp", "src/main.cpp"), "zero directories match src/**/*.cpp");
}

void glob_anchors_at_both_ends() {
    check(!matches("src/*.cpp", "other/src/main.cpp"), "pattern is anchored at the start");
    check(!matches("src/*.cpp", "src/main.cpp.bak"), "pattern is anchored at the end");
    check(matches("platform/windows/*.cpp", "platform/windows/win32_adapter.cpp"),
          "platform glob matches");
    check(!matches("platform/windows/*.cpp", "platform/linux/linux_adapter.cpp"),
          "platform glob rejects another platform");
}

void codegen_prefilter_matches_the_cmake_rule() {
    check(needs_codegen("[[huxerui::composable]]\nView Counter() {}"), "marker is detected");
    check(needs_codegen("auto value = UseState(0);"), "Use call is detected");
    // Anything else must produce NO transform edge; the cost of a false
    // positive is a needless build-graph node per source.
    check(!needs_codegen("#include <huxerui/huxerui.h>\nint main() { return 0; }"),
          "plain source needs no transform");
}

void entry_is_excluded_from_the_transform_set() {
    const std::vector<std::string> sources{"src/main.cpp", "src/counter.cpp"};
    const std::vector<std::string> kept = without_entry(sources, "src/main.cpp");
    check(kept.size() == 1 && kept.front() == "src/counter.cpp", "entry is removed");
    check(without_entry(sources, "src/absent.cpp").size() == 2, "an absent entry removes nothing");
}

void package_names_become_identifiers() {
    check(identifier("demo") == "demo", "a plain name is unchanged");
    // The defect this exists for: hrc refuses `mod-proj` with
    // "resource namespace must be a non-reserved C++ identifier".
    check(identifier("mod-proj") == "mod_proj", "a hyphen becomes an underscore");
    check(identifier("my.app") == "my_app", "a dot becomes an underscore");
    check(identifier("2fast") == "_2fast", "a leading digit is prefixed");
    check(identifier("") == "resources", "an empty name still yields an identifier");
}

void wix_layout_matches_what_the_package_installs() {
    const auto paths = wix_paths("/x/wix");
    // The three payloads keep their own upstream shapes; these are the paths
    // xim:wix's own post-install anchors check for.
    check(paths.tool == "/x/wix/tool/tools/net6.0/any/wix.exe", "wix.exe path");
    check(paths.bootstrapper_lib == "/x/wix/bootstrapper/build/native/v14/x64/balutil.lib",
          "balutil.lib path");
    check(paths.dutil_lib == "/x/wix/dutil/build/native/v14/x64/dutil.lib", "dutil.lib path");
    check(paths.bootstrapper_runtime ==
              "/x/wix/bootstrapper/runtimes/win-x64/native/mbanative.dll",
          "mbanative.dll path");
    check(paths.bootstrapper_include.ends_with("/build/native/include"), "bootstrapper include");
    check(paths.dutil_include.ends_with("/build/native/include"), "dutil include");
}

void module_interface_is_read_from_the_source() {
    const auto m = scan_module_interface(
        "module;\n"
        "#include <typeinfo>\n"
        "export module app;\n"
        "import huxerui;\n"
        "import std;\n");
    // Without these two the importer compiles before the generated interface
    // exists and fails with `failed to read compiled module: app`.
    check(m.name == "app", "the declared module name is read");
    check(m.imports.size() == 2 && m.imports[0] == "huxerui" && m.imports[1] == "std",
          "every import is read");

    const auto none = scan_module_interface("#include <x>\nint main() { return 0; }\n");
    check(none.name.empty(), "an ordinary translation unit declares no module");

    // A commented-out declaration is not one.
    const auto commented = scan_module_interface("// export module ghost;\nexport module real;\n");
    check(commented.name == "real", "a comment is not a declaration");
}

bool has(const std::vector<std::string>& names, std::string_view what) {
    return std::ranges::find(names, what) != names.end();
}

void header_scan_finds_namespace_scope_declarations() {
    const auto names = public_names(
        "namespace huxerui {\n"
        "class Column final : public Layout<Column> {\n"
        "  void Member();\n"
        "};\n"
        "enum class TextRole { Title };\n"
        "using Views = std::vector<View>;\n"
        "View Divider();\n"
        "}\n");
    check(has(names, "Column"), "a class is found");
    check(has(names, "TextRole"), "an enum is found");
    check(has(names, "Views"), "a using alias is found");
    check(has(names, "Divider"), "a function is found");
    check(!has(names, "Member"), "a member function is not a namespace-scope name");
}

void header_scan_sees_past_an_attribute() {
    // `class [[nodiscard]] Result final {` -- the attribute sits between the
    // keyword and the name, and skipping it is what makes Result reachable
    // through `import huxerui;`.
    const auto names = public_names(
        "namespace huxerui {\n"
        "template <class T, class E> class [[nodiscard]] Result final {\n"
        "};\n"
        "}\n");
    check(has(names, "Result"), "an attributed class is found");
}

void header_scan_ignores_the_detail_namespace() {
    const auto names = public_names(
        "namespace huxerui {\n"
        "class Widget {};\n"
        "namespace detail {\n"
        "class Internal {};\n"
        "}\n"
        "}\n");
    check(has(names, "Widget"), "the public class is found");
    check(!has(names, "Internal"), "detail is excluded");
}

void header_scan_survives_braces_in_literals_and_macros() {
    // A '}' in a char literal closed the namespace early: view.h yielded 15
    // names instead of hundreds.
    const auto literal = public_names(
        "namespace huxerui {\n"
        "inline char Closing() { return '}'; }\n"
        "class Later {};\n"
        "}\n");
    check(has(literal, "Later"), "a brace in a char literal is not scope");

    // The same from the other direction: a #define is skipped by its leading
    // '#', but its CONTINUATION lines are not.
    const auto macro = public_names(
        "namespace huxerui {\n"
        "#define SCOPE_BEGIN \\\n"
        "  return Scope([=]() -> View {\n"
        "class Later {};\n"
        "}\n");
    check(has(macro, "Later"), "an unbalanced brace in a macro body is not scope");
}

void header_scan_handles_templates_and_qualified_names() {
    const auto single_line = public_names(
        "namespace huxerui {\n"
        "template <class T> class State final : public Cell {};\n"
        "}\n");
    check(has(single_line, "State"), "a single-line template declaration is found");

    // `StringVariant StringVariant::Format(...)` is at namespace scope but
    // `Format` does not exist in namespace huxerui; exporting it stopped the
    // module compiling.
    const auto qualified = public_names(
        "namespace huxerui {\n"
        "class StringVariant {};\n"
        "template <class... A> StringVariant StringVariant::Format(A&&... a) { return {}; }\n"
        "}\n");
    check(!has(qualified, "Format"), "an out-of-class definition is not a namespace name");
}

void strip_noncode_removes_what_it_should() {
    const std::string out = strip_noncode("int a; // }\n/* } */ int b;\n\"}\" '}'\n");
    check(out.find('}') == std::string::npos, "no brace survives a comment or literal");
    check(out.find("int a;") != std::string::npos, "code survives");
    check(std::ranges::count(out, '\n') == 3, "line structure survives");
}

void cmake_lists_keep_quoted_tokens_whole() {
    // `"-framework AppKit"` is ONE token; splitting on whitespace made every
    // macOS framework look unmatched.
    const auto frameworks = cmake_list(
        "set(HUXERUI_PLATFORM_LINK_LIBRARIES\n"
        "        \"-framework AppKit\"\n"
        "        \"-weak_framework UniformTypeIdentifiers\"\n"
        ")\n",
        "HUXERUI_PLATFORM_LINK_LIBRARIES");
    check(frameworks.size() == 2 && frameworks[0] == "-framework AppKit",
          "a quoted token stays whole");

    const auto libraries = cmake_list(
        "set(HUXERUI_PLATFORM_LINK_LIBRARIES\n"
        "        advapi32  # the registry\n"
        "        d2d1\n"
        ")\n",
        "HUXERUI_PLATFORM_LINK_LIBRARIES");
    check(libraries.size() == 2 && libraries[0] == "advapi32" && libraries[1] == "d2d1",
          "bare tokens and comments");

    check(cmake_list("set(OTHER a)\n", "HUXERUI_PLATFORM_LINK_LIBRARIES").empty(),
          "a missing variable is empty rather than an error");
}

void macro_definitions_come_out_verbatim() {
    const std::string header =
        "#define HUXERUI_SCOPE(...) return ::huxerui::Scope(__VA_ARGS__)\n"
        "\n"
        "#define HUXERUI_SCOPE_BEGIN \\\n"
        "  return ::huxerui::Scope([=]() -> ::huxerui::View {\n"
        "int after;\n";
    // A prefix must not match a longer name, or HUXERUI_SCOPE picks up
    // HUXERUI_SCOPE_BEGIN and the prelude defines the wrong thing.
    const std::string scope = macro_definition(header, "HUXERUI_SCOPE");
    check(scope.find("__VA_ARGS__") != std::string::npos, "HUXERUI_SCOPE is the short one");
    check(scope.find("SCOPE_BEGIN") == std::string::npos, "it did not swallow the longer name");

    // Continuations have to come along, or the prelude defines an empty macro
    // and hand-written composables silently produce nothing.
    const std::string begin = macro_definition(header, "HUXERUI_SCOPE_BEGIN");
    check(begin.find("::huxerui::Scope([=]()") != std::string::npos, "the continuation is included");
    check(begin.find("int after") == std::string::npos, "it stops at the end of the definition");

    check(macro_definition(header, "HUXERUI_ABSENT").empty(), "a missing macro is empty");
}

void umbrella_includes_are_read_in_order() {
    const auto list = umbrella_includes(
        "#pragma once\n#include <huxerui/view.h>\n#include <string>\n#include <huxerui/app.h>\n");
    check(list.size() == 2 && list[0] == "view.h" && list[1] == "app.h",
          "only huxerui headers, in order");
}

} // namespace

int main() {
    header_scan_finds_namespace_scope_declarations();
    header_scan_sees_past_an_attribute();
    header_scan_ignores_the_detail_namespace();
    header_scan_survives_braces_in_literals_and_macros();
    header_scan_handles_templates_and_qualified_names();
    strip_noncode_removes_what_it_should();
    cmake_lists_keep_quoted_tokens_whole();
    macro_definitions_come_out_verbatim();
    umbrella_includes_are_read_in_order();
    module_interface_is_read_from_the_source();
    wix_layout_matches_what_the_package_installs();
    package_names_become_identifiers();
    glob_single_star_stays_within_one_segment();
    glob_double_star_crosses_separators();
    glob_anchors_at_both_ends();
    codegen_prefilter_matches_the_cmake_rule();
    entry_is_excluded_from_the_transform_set();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all source-selection checks passed\n";
    return 0;
}
