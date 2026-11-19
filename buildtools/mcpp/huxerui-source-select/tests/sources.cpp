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
using huxerui::rules::sources::without_entry;
using huxerui::rules::sources::defined_macros;
using huxerui::rules::sources::names_identifier;
using huxerui::rules::sources::package_module_name;
using huxerui::rules::sources::reexported_modules;
using huxerui::rules::sources::render_library_module;
using huxerui::rules::sources::unexported_names;

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

// ------------------------------------------------------------- installer --

void test_upgrade_code() {
    using huxerui::rules::sources::is_upgrade_code;
    check(is_upgrade_code("6F2B4C1E-9A3D-4F58-8B27-1D0E5A7C93B4"), "a GUID is accepted");
    check(is_upgrade_code("6f2b4c1e-9a3d-4f58-8b27-1d0e5a7c93b4"), "lower case is a GUID too");
    check(!is_upgrade_code(""), "empty is not a GUID");
    check(!is_upgrade_code("not-a-guid"), "a word is not a GUID");
    // The failure this guards is silent: WiX takes a malformed code literally
    // and the MSI then never upgrades in place.
    check(!is_upgrade_code("6F2B4C1E9A3D4F588B271D0E5A7C93B4"), "dashes are required");
    check(!is_upgrade_code("6F2B4C1E-9A3D-4F58-8B27-1D0E5A7C93B"), "35 characters is not a GUID");
    check(!is_upgrade_code("6F2B4C1E-9A3D-4F58-8B27-1D0E5A7C93BG"), "G is not a hex digit");
}

void test_name_uuid() {
    using huxerui::rules::sources::name_uuid;
    // CMake: string(UUID ... NAMESPACE 6ba7b810-9dad-11d1-80b4-00c04fd430c8
    //        NAME "org.huxerui.cts.appminimal.msi" TYPE SHA1 UPPER)
    check(name_uuid("6ba7b810-9dad-11d1-80b4-00c04fd430c8", "org.huxerui.cts.appminimal.msi") ==
              "73C5E7BA-5A00-5AC0-9A06-590CFDEBBE4D",
          "the upgrade code CMake derives from the same name");
    check(name_uuid("not a uuid", "x").empty(), "a namespace that is not a UUID gives nothing");
}

void test_render_at_tokens() {
    using huxerui::rules::sources::render_at_tokens;
    std::string error;
    const std::string out = render_at_tokens("<a href=\"./@FILE@\">x@y.z @NAME@</a>",
                                             {{"FILE", "favicon.svg"}, {"NAME", "App"}}, error);
    check(error.empty(), "a complete token set renders");
    check(out == "<a href=\"./favicon.svg\">x@y.z App</a>", "tokens replaced, a lone @ kept");

    error.clear();
    render_at_tokens("@NOPE@", {{"FILE", "f"}}, error);
    check(error.find("NOPE") != std::string::npos, "an unknown token is named, not copied");
}

void header_scan_reads_a_library_namespace() {
    const std::string header =
        "namespace acme {\n"
        "namespace widgets {\n"
        "void Install(huxerui::ApplicationContext& context);\n"
        "static int Helper();\n"
        "class Gauge {};\n"
        "}\n"
        "void Outside();\n"
        "}\n"
        "namespace acme::widgets {\n"
        "enum class Size { Small };\n"
        "}\n";
    const auto names = public_names(header, "acme::widgets");
    check(has(names, "Install") && has(names, "Gauge") && has(names, "Size"),
          "nested blocks and a qualified namespace are both read");
    check(!has(names, "Outside"), "an enclosing namespace's name is not the library's");
    check(!has(names, "Helper"), "a static name has internal linkage and is left out");
    check(public_names(header).empty(), "the default namespace is still huxerui");
}

void library_module_exports_the_scanned_names() {
    const std::vector<std::string> includes{"acme/widgets.h"};
    const std::vector<std::string> names{"Gauge", "Install"};
    const std::string module = render_library_module("acme.widgets", "acme::widgets", includes, names);
    check(module.find("module;\n\n#include <typeinfo>\n#include <acme/widgets.h>\n") != std::string::npos,
          "the headers are included in the global module fragment");
    check(module.find("export module acme.widgets;\n\nexport import huxerui;\n") != std::string::npos,
          "the module is named and re-exports huxerui");
    check(module.find("    using acme::widgets::Gauge;\n    using acme::widgets::Install;\n") != std::string::npos,
          "every name is exported");
    check(unexported_names(module, "acme::widgets", names).empty(), "a generated interface misses nothing");
    const std::vector<std::string> more{"Gauge", "Install", "Meter"};
    check(unexported_names(module, "acme::widgets", more) == std::vector<std::string>{"Meter"},
          "a name a hand-written interface lacks is reported");
    check(module.find("export import acme.widgets.macros;") == std::string::npos,
          "without a macros unit the interface re-exports none");
    const std::vector<std::string> units{"acme.widgets.macros"};
    const std::string with_macros = render_library_module("acme.widgets", "acme::widgets", includes, names, units);
    check(with_macros.find("export import huxerui;\nexport import acme.widgets.macros;\n") != std::string::npos,
          "the author's macros unit is re-exported after huxerui");
    check(reexported_modules(with_macros) == std::vector<std::string>{"huxerui", "acme.widgets.macros"},
          "the re-exported modules are read back");
    check(reexported_modules("// export import gone;\nexport import kept;\n") == std::vector<std::string>{"kept"},
          "a commented re-export is not one");
}

void a_module_is_named_after_its_package() {
    check(package_module_name("acme", "gauge") == "acme.gauge", "the package namespace prefixes the name");
    check(package_module_name("", "gauge") == "gauge", "a package without a namespace is its name");
    check(package_module_name("mcpplibs.mwrap", "zlib") == "mcpplibs.mwrap.zlib", "a dotted namespace is kept");
    check(package_module_name("", "example-widgets") == "example_widgets", "a dash is no module-name character");
}

void macros_a_header_undefines_are_its_own() {
    const auto macros = defined_macros(
        "#pragma once\n"
        "#define ACME_VERSION 3\n"
        "#define ACME_JOIN(a, b) a##b\n"
        "#undef ACME_JOIN\n");
    check(macros.size() == 1 && macros[0].name == "ACME_VERSION", "an undefined helper is not reported");
}

void a_macros_unit_names_whole_identifiers() {
    const std::string unit = "inline constexpr auto ACME_VERSION = capture::value_ACME_VERSION;\n";
    check(names_identifier(unit, "ACME_VERSION"), "a mapped macro is found");
    check(!names_identifier(unit, "ACME_VERS"), "a prefix is not a name");
    check(!names_identifier(unit, "VERSION"), "a suffix is not a name");
    check(!names_identifier("", "ACME_VERSION"), "an absent unit maps nothing");
}

void macros_are_found_apart_from_the_include_guard() {
    const auto macros = defined_macros(
        "#ifndef ACME_WIDGETS_H\n"
        "#define ACME_WIDGETS_H\n"
        "#define ACME_VERSION 3\n"
        "  #  define ACME_CALL(x) (x)\n"
        "#endif\n");
    check(macros.size() == 2, "the guard is not a macro a consumer uses");
    check(!macros.empty() && macros[0].name == "ACME_VERSION" && macros[0].line == 3, "a macro is named with its line");
    check(macros.size() == 2 && macros[1].name == "ACME_CALL", "a function-like macro is found");
}

int main() {
    header_scan_reads_a_library_namespace();
    library_module_exports_the_scanned_names();
    macros_are_found_apart_from_the_include_guard();
    macros_a_header_undefines_are_its_own();
    a_macros_unit_names_whole_identifiers();
    a_module_is_named_after_its_package();
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
    package_names_become_identifiers();
    glob_single_star_stays_within_one_segment();
    glob_double_star_crosses_separators();
    glob_anchors_at_both_ends();
    codegen_prefilter_matches_the_cmake_rule();
    entry_is_excluded_from_the_transform_set();
    test_upgrade_code();
    test_name_uuid();
    test_render_at_tokens();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all source-selection checks passed\n";
    return 0;
}
