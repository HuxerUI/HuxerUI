// Build rules for HuxerUI projects driven by mcpp.
//
// A consumer's build.mcpp is expected to be one line:
//
//     import mcpp; import huxerui.rules;
//     int main() { return huxerui::rules::configure({ .resources = "resources" }) ? 0 : 1; }
//
// Everything this module schedules mirrors a step CMake performs in
// cmake/HuxerUICodegen.cmake and cmake/HuxerUIResources.cmake. Where the two
// must agree, the CMake function is named in a comment so a reader can diff
// them; `huxerui-build-check` mechanically checks the parts that can be
// checked.

export module huxerui.rules;

// C++20, not C++23. A host module is compiled with the ROOT package's
// `standard` -- module-graph-global, dependencies included -- and HuxerUI's is
// c++20 because that is the SDK ABI baseline. So no std::println here.
import std;
import mcpp;
import huxerui.rules.sources;

export namespace huxerui::rules {

// ---------------------------------------------------------------- options --
// Field-for-field the arguments of huxerui_add_app() in cmake/HuxerUIApp.cmake,
// so the two spellings of "an application" can be reviewed side by side.
struct options {
    std::vector<std::string> sources;             // default: src/**/*.cpp
    // The bin target's entry, which mcpp compiles separately from `sources`.
    // It is EXCLUDED from the transform set: a build program can add sources
    // but cannot replace one, so a transformed copy of the entry would link
    // beside the original as `multiple definition of 'main'`. AGENTS.md
    // already says not to annotate the app root, so this costs nothing.
    std::string              entry = "src/main.cpp";
    bool                     codegen = true;      // = huxerui_enable_codegen()
    std::string              resources;           // = RESOURCES (root; empty = none)
    std::string              resource_namespace;  // = RESOURCE_NAMESPACE
    std::string              bundle_name;         // = BUNDLE_NAME
    std::string              bundle_identifier;   // = BUNDLE_IDENTIFIER
};

// A planned build-graph edge, handed back so a caller that needs to adjust one
// can. A rule without this pair has a cliff: past its last knob the only way
// out is to hand-write the action, and that copy then drifts.
struct edge {
    std::string              id;
    std::string              role;
    std::string              description;
    std::vector<std::string> command;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    // A generated MODULE INTERFACE has to declare what it will provide and
    // import: mcpp fixes the module graph during prepare, before the generator
    // runs, and seeds a placeholder carrying exactly this declaration. Empty
    // for an ordinary generated source.
    std::string              provides;
    std::vector<std::string> imports;
};

// ------------------------------------------------------------ SDK locating --
namespace detail {

inline std::string env_or(const char* name, std::string_view fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string(fallback);
}

} // namespace detail

// The SDK root: the directory holding include/ and resources/.
//
// Two callers, two answers, and neither may guess. When HuxerUI builds itself
// the root is its own manifest directory. When an application builds, the root
// is the resolved install directory of the `huxerui` dependency -- which is a
// SOURCE root, not a build output directory: mcpp deliberately offers no
// channel from a dependency's MCPP_OUT_DIR to a consumer's build.
inline std::string sdk_root() {
    const std::string self = mcpp::manifest_dir();
    if (!self.empty() && std::filesystem::exists(
            std::filesystem::path(self) / "include" / "huxerui" / "huxerui.h")) {
        return self;
    }
    const std::string dep = mcpp::dep_dir("huxerui");
    return dep;
}

// The host tools, built from source by mcpp and handed here as absolute paths.
//
// NOT tools/prebuilt/. Those binaries are produced for the host's C library by
// .github/workflows/update-host-tools.yml, while mcpp compiles with its own
// payload toolchain and its own glibc; and after a change to
// tools/codegen/transform.cpp they are stale until that workflow runs on main.
// Building from source keeps the mcpp leg self-consistent and takes nothing
// from the machine. CMake keeps using tools/prebuilt/ exactly as before.
//
// `tools = ["hcg"]` on the dependency edge is what asks for this; `reexport`
// on the same edge is what makes it reach an APPLICATION's build program
// without the application declaring the tool packages itself.
inline std::string host_tool(std::string_view tool) {
    if (tool == "hcg")
        return mcpp::dep_bin("huxerui-codegen", "hcg");
    if (tool == "hrc")
        return mcpp::dep_bin("huxerui-resource-compiler", "hrc");
    return {};
}

// ------------------------------------------------------------- WiX toolset --
// Where the Windows installer's tool and libraries are, or empty when the
// payload is not installed -- which is every non-Windows build, and a Windows
// build whose project never asked for an installer.
//
// The paths themselves are composed in huxerui.rules.sources so they are unit
// tested: a wrong one shows up on Windows only, at link time, as a missing
// .lib.
inline huxerui::rules::sources::wix_layout wix() {
    const std::string root = mcpp::xpkg_dir("xim", "wix");
    if (root.empty()) return {};
    return huxerui::rules::sources::wix_paths(root);
}

// ------------------------------------------------------------------ globs --
namespace detail {

inline void collect(const std::filesystem::path& root, std::string_view pattern,
                    std::vector<std::string>& out) {
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = std::filesystem::relative(it->path(), root, ec).generic_string();
        if (ec) continue;
        if (huxerui::rules::sources::matches(pattern, rel)) out.push_back(rel);
    }
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
}

inline std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

} // namespace detail

// ---------------------------------------------------------------- codegen --
namespace detail {

using huxerui::rules::sources::needs_codegen;

// cmake/HuxerUICodegen.cmake ends by suppressing the diagnostic that
// [[huxerui::composable]] provokes, because no compiler knows the attribute.
inline void suppress_attribute_warning() {
    const std::string os = mcpp::target_os();
    if (os == "windows" && std::string_view(mcpp::compiler()).find("msvc")
                           != std::string_view::npos) {
        mcpp::cxxflag("/wd5030");
    } else if (std::string_view(mcpp::compiler()).find("gcc")
               != std::string_view::npos) {
        mcpp::cxxflag("-Wno-attributes");
    } else {
        mcpp::cxxflag("-Wno-unknown-attributes");
    }
}

} // namespace detail

// Plan one hcg edge per source that actually contains a composable.
inline std::vector<edge> plan_codegen(std::span<const std::string> sources) {
    std::vector<edge> out;
    const std::string hcg = host_tool("hcg");
    if (hcg.empty()) return out;

    const std::filesystem::path root = mcpp::manifest_dir();
    const std::string           odir = std::string(mcpp::out_dir()) + "/hcg";

    for (const std::string& rel : sources) {
        const std::filesystem::path abs = root / rel;
        const std::string text = detail::read_file(abs);
        if (text.empty() || !detail::needs_codegen(text)) continue;

        std::string stem = abs.stem().string();
        std::string flat = rel;
        for (char& c : flat) if (c == '/' || c == '\\') c = '_';
        const std::string gen = odir + "/" + flat;

        edge e{
            .id          = "hcg:" + stem,
            .role        = "source",
            .description = "huxerui composable " + rel,
            .command     = { hcg, "--input", abs.string(), "--output", gen },
            .inputs      = { abs.string(), hcg },
            .outputs     = { gen },
        };
        if (abs.extension() == ".cppm") {
            const auto declared = huxerui::rules::sources::scan_module_interface(text);
            e.provides = declared.name;
            e.imports  = declared.imports;
        }
        out.push_back(std::move(e));
    }
    return out;
}

// --------------------------------------------------------------- resources --
// Two hrc invocations plus a merge, which is the shape of
// cmake/HuxerUIResourceBuild.cmake: compile each root, then merge the packages.
//
// The framework's own builtin package is compiled HERE, in the consumer's
// build, rather than reused from the framework's build. It has to be: the
// framework generates it into its own MCPP_OUT_DIR, and a consumer is given
// dep_dir() -- the dependency's SOURCE root. `resources/` is source, so it is
// reachable; the compiled package is not. Recompiling 44 files / 196 KB is the
// cost of not inventing a channel mcpp deliberately does not have.
inline std::vector<edge> plan_resources(const options& opt) {
    std::vector<edge> out;
    const std::string hrc = host_tool("hrc");
    if (hrc.empty()) return out;

    const std::string root = sdk_root();
    const std::string odir = std::string(mcpp::out_dir()) + "/hrc";
    const std::string builtin_src = root + "/resources";

    std::vector<std::string> packages;

    if (std::filesystem::exists(builtin_src)) {
        std::vector<std::string> inputs{ hrc };
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(builtin_src, ec);
             it != std::filesystem::recursive_directory_iterator(); ++it) {
            if (it->is_regular_file(ec)) inputs.push_back(it->path().string());
        }
        out.push_back(edge{
            .id          = "hrc:builtin",
            .role        = "source",
            .description = "huxerui builtin resources",
            .command     = { hrc, "--root", builtin_src,
                             "--output", odir + "/builtin",
                             "--namespace", "huxerui",
                             "--header-name", "huxerui_builtin_resources.h" },
            .inputs      = inputs,
            .outputs     = { odir + "/builtin/include/huxerui_builtin_resources.h",
                             odir + "/builtin/package/huxerui/resources.bin" },
        });
        packages.push_back(odir + "/builtin/package");
    }

    if (!opt.resources.empty()) {
        // The package name is not necessarily a C++ identifier; hrc requires
        // one for the generated accessors.
        const std::string ns =
            opt.resource_namespace.empty()
                ? huxerui::rules::sources::identifier(mcpp::package_name())
                : opt.resource_namespace;
        // ABSOLUTE. An action's command runs with the BUILD directory as its
        // working directory, not the package root, so a relative root reaches
        // hrc as "resource root is not a directory: resources".
        const std::string app_src =
            (std::filesystem::path(mcpp::manifest_dir()) / opt.resources).string();
        std::vector<std::string> inputs{ hrc };
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(app_src, ec);
             it != std::filesystem::recursive_directory_iterator(); ++it) {
            if (it->is_regular_file(ec)) inputs.push_back(it->path().string());
        }
        out.push_back(edge{
            .id          = "hrc:app",
            .role        = "source",
            .description = "application resources " + opt.resources,
            .command     = { hrc, "--root", app_src,
                             "--output", odir + "/app",
                             "--namespace", ns },
            .inputs      = inputs,
            .outputs     = { odir + "/app/package/huxerui/resources.bin" },
        });
        packages.push_back(odir + "/app/package");
    }

    if (packages.size() > 1) {
        edge merge{
            .id          = "hrc:merge",
            .role        = "source",
            .description = "merge huxerui resource packages",
            .command     = { hrc, "merge" },
            .inputs      = {},
            .outputs     = { odir + "/final/huxerui/resources.bin" },
        };
        for (const std::string& p : packages) {
            merge.command.push_back("--input");
            merge.command.push_back(p);
            merge.inputs.push_back(p + "/huxerui/resources.bin");
        }
        merge.command.push_back("--output");
        merge.command.push_back(odir + "/final");
        out.push_back(std::move(merge));
    }
    return out;
}

// ----------------------------------------------------------------- submit --
inline bool submit(std::span<const edge> edges) {
    for (const edge& e : edges) {
        mcpp::action a;
        a.id          = e.id.c_str();
        a.role        = e.role.c_str();
        a.description = e.description.c_str();
        for (const std::string& c : e.command) a.arg(c.c_str());
        for (const std::string& i : e.inputs)  a.input(i.c_str());
        for (const std::string& o : e.outputs) a.output(o.c_str());
        if (!e.provides.empty()) a.provides(e.provides.c_str());
        for (const std::string& i : e.imports) a.imports(i.c_str());
        a.submit();
    }
    return true;
}

// -------------------------------------------------------------- configure --
// One call for the common case. Returns false only on a condition that should
// stop the build; a missing optional input is reported and tolerated.
inline bool configure(options opt = {}) {
    const std::string root = sdk_root();
    if (root.empty()) {
        std::cerr << "huxerui.rules: cannot locate the HuxerUI SDK root "
                     "(neither this package nor a `huxerui` dependency)\n";
        return false;
    }

    // A MODULE INTERFACE UNIT CANNOT RECEIVE A FORCED INCLUDE, so this is
    // conditional rather than unconditional.
    //
    // `-include` prepends the header before the first line of the translation
    // unit, and a module interface must open with `module;` or `export module`.
    // Measured on gcc 16.1.0, even a unit that opens with a global module
    // fragment:
    //
    //     error: module-declaration only permitted as first declaration,
    //            or ending a global module fragment
    //
    // A package with module interface units therefore writes its own includes
    // in the global module fragment, which is where a module unit's includes
    // belong anyway. A package without them keeps the clean form, which is the
    // whole point of forcing the include: `import huxerui;` has to be the only
    // difference from `#include <huxerui/huxerui.h>`.
    std::vector<std::string> selected = opt.sources;
    if (selected.empty()) {
        detail::collect(mcpp::manifest_dir(), "src/**/*.cpp", selected);
        detail::collect(mcpp::manifest_dir(), "src/**/*.cppm", selected);
    }
    selected = huxerui::rules::sources::without_entry(selected, opt.entry);
    const bool has_module_interface = std::ranges::any_of(
        selected, [](const std::string& source) { return source.ends_with(".cppm"); });

    // `import huxerui;` needs <typeinfo> in the IMPORTING translation unit.
    //
    // GCC's `typeid` check is per-TU: it asks whether <typeinfo> was included
    // here, not whether std::type_info is reachable. Entities in the module's
    // global module fragment do not satisfy it, and HuxerUI's UseState(),
    // View and Layout machinery all instantiate typeid in the caller -- so
    // every importer would otherwise open with a line of compiler trivia:
    //
    //     include/huxerui/state.h:463: error: must '#include <typeinfo>'
    //                                         before using 'typeid'
    //
    // Emitting it as a forced include keeps that out of application code,
    // which is the point: `import huxerui;` has to be the ONLY difference from
    // `#include <huxerui/huxerui.h>`. Measured on gcc 16.1.0.
    // THE SCOPE MACROS COME THE SAME WAY, AND FOR A SHARPER REASON.
    //
    // HUXERUI_SCOPE / _BEGIN / _END are public API -- hand-written composables
    // use them directly -- and a macro does not cross a module boundary. So an
    // importing translation unit that writes HUXERUI_SCOPE(...) by hand fails
    // with `'HUXERUI_SCOPE' was not declared in this scope`, no matter that hcg
    // no longer emits the names itself.
    //
    // hcg emitting the expansion fixes GENERATED code; this fixes HAND-WRITTEN
    // code. They are different holes and both have to be closed for
    // `import huxerui;` to behave exactly like `#include <huxerui/huxerui.h>`.
    //
    // The header is generated from view.h by `huxerui-module-gen` and checked
    // by that same program's --check mode, so the two definitions cannot
    // drift into an illegal redefinition for a unit that does both.
    const std::string prelude =
        root + "/mcpp/huxerui-build-rules/include/huxerui_scope_prelude.h";
    const bool msvc =
        std::string_view(mcpp::compiler()).find("msvc") != std::string_view::npos;
    // The prelude's DIRECTORY is on the include path either way, so a module
    // unit can put `#include <huxerui_scope_prelude.h>` in its global module
    // fragment and hand-written HUXERUI_SCOPE works there too. Only the
    // FORCING is conditional.
    mcpp::include_dir(
        (root + "/mcpp/huxerui-build-rules/include").c_str());

    if (has_module_interface) {
        // Nothing forced; the module units carry it themselves.
    } else if (msvc) {
        mcpp::cxxflag("/FItypeinfo");
        mcpp::cxxflag(("/FI" + prelude).c_str());
    } else {
        mcpp::cxxflag("-include");
        mcpp::cxxflag("typeinfo");
        mcpp::cxxflag("-include");
        mcpp::cxxflag(prelude.c_str());
    }
    mcpp::rerun_if_changed(prelude.c_str());

    // Re-run when the SET of sources or resources changes; the edges below
    // track content.
    mcpp::rerun_if_changed_glob("src/**/*.cpp");
    mcpp::rerun_if_changed_glob("src/**/*.cppm");
    if (!opt.resources.empty()) {
        mcpp::rerun_if_changed_glob((opt.resources + "/**").c_str());
    }

    std::vector<edge> edges;

    if (opt.codegen) {
        const std::vector<std::string>& sources = selected;

        // THE MANIFEST MUST SAY `sources = []`, AND THIS IS WHY.
        //
        // `mcpp::action{role = "source"}` ADDS its outputs to the compile set;
        // there is no mechanism to replace an input. So a package that both
        // globs `src/**/*.cpp` and receives a transformed copy links two
        // definitions of every composable. Letting the rule own the selection
        // -- ordinary sources through mcpp::source(), composable ones through
        // the transform -- is the only shape that yields one object per file.
        auto cg = plan_codegen(sources);
        std::vector<std::string> transformed;
        for (const edge& e : cg) {
            const std::string stem =
                std::filesystem::path(e.description).filename().string();
            transformed.push_back(e.inputs.empty() ? std::string{} : e.inputs[0]);
        }
        for (const std::string& rel : sources) {
            const std::string abs =
                (std::filesystem::path(mcpp::manifest_dir()) / rel).string();
            if (std::ranges::find(transformed, abs) == transformed.end())
                mcpp::source(abs.c_str());
        }
        if (!cg.empty()) {
            detail::suppress_attribute_warning();
            // A transformed source is compiled from MCPP_OUT_DIR, so its own
            // relative #includes no longer resolve. CMake solves this with
            // target_include_directories(PRIVATE <source dir>); the same
            // directories have to be added here or the moved file stops
            // finding its neighbours.
            std::vector<std::string> seen;
            for (const std::string& rel : sources) {
                std::filesystem::path d =
                    (std::filesystem::path(mcpp::manifest_dir()) / rel).parent_path();
                std::string s = d.string();
                if (std::ranges::find(seen, s) == seen.end()) {
                    seen.push_back(s);
                    mcpp::include_dir(s.c_str());
                }
            }
        }
        edges.insert(edges.end(), cg.begin(), cg.end());
    }

    auto rs = plan_resources(opt);
    if (!rs.empty()) {
        mcpp::include_dir(
            (std::string(mcpp::out_dir()) + "/hrc/builtin/include").c_str());
    }
    edges.insert(edges.end(), rs.begin(), rs.end());

    return submit(edges);
}

// The framework's own build needs only the builtin resource header: 11 of its
// translation units #include "huxerui_builtin_resources.h", so this cannot be
// deferred to the consumer even though the compiled package is also produced
// here and thrown away.
inline bool builtin_resources() {
    const std::string hrc = host_tool("hrc");
    if (hrc.empty()) {
        std::cerr << "huxerui.rules: hrc was not provided -- the `huxerui` "
                     "dependency must carry tools = [\"hrc\"]\n";
        return false;
    }
    const std::string odir = std::string(mcpp::out_dir()) + "/hrc";
    const std::string src  = std::string(mcpp::manifest_dir()) + "/resources";

    mcpp::rerun_if_changed_glob("resources/**");

    std::vector<std::string> inputs{ hrc };
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(src, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file(ec)) inputs.push_back(it->path().string());
    }

    edge e{
        .id          = "hrc:builtin",
        .role        = "source",
        .description = "huxerui builtin resources",
        .command     = { hrc, "--root", src,
                         "--output", odir + "/builtin",
                         "--namespace", "huxerui",
                         "--header-name", "huxerui_builtin_resources.h" },
        .inputs      = inputs,
        .outputs     = { odir + "/builtin/include/huxerui_builtin_resources.h",
                         odir + "/builtin/package/huxerui/resources.bin" },
    };
    mcpp::include_dir((odir + "/builtin/include").c_str());
    return submit(std::span<const edge>(&e, 1));
}

} // namespace huxerui::rules
