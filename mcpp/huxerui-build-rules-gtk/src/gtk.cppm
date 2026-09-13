// The Linux GTK stack for HuxerUI packages built by mcpp: the payload table
// (this package's manifest) and the pkg-config probe that turns it into
// compile and link flags. A host module the framework re-exports, so a
// consumer's build program reaches `huxerui::rules::linux_gtk` through
// `import huxerui.rules;`.

export module huxerui.rules.gtk;

import std;
import mcpp;

export namespace huxerui::rules {

// -------------------------------------------------------------- linux gtk --
// THE GTK STACK REACHES A PACKAGE'S TRANSLATION UNITS FROM HERE.
//
// mcpp compiles with its own toolchain and its own glibc, so GTK comes from
// xlings payloads, never from the machine (mcpp.toml's Linux section says
// why). The framework probes pkg-config for the stack in its build program;
// the `-l`/`-L` half of that reaches every consumer's final link, but the
// `-I` half colours the framework's own translation units only -- mcpp offers
// no channel for a dependency's computed include directories to reach a
// consumer's compile. A library whose own sources include GTK headers (one
// integrating with platform/linux, as Lib-Live2D's GL surface does) therefore
// needs the same probe in its own build program, and it cannot copy the probe:
// `xpkg_dir` answers only for payloads the BUILDING package declared.
//
// So the payload table lives once, in this package's manifest. A host module's
// `[xlings.workspace]` reaches every build program it is compiled into, which
// is every HuxerUI application's and library's; this function reads the table
// wherever it is called. huxerui-build-check asserts the table and the
// framework's own (mcpp.toml, the target-axis declaration for the artifact)
// stay identical.
namespace detail {

// The Linux payloads, names only: their VERSIONS live in the manifest, and
// xpkg_dir answers with the directory of whatever this build installed.
constexpr std::string_view kLinuxPayloads[] = {
    "gtk4", "libepoxy", "libsoup", "glib", "cairo", "pango", "harfbuzz",
    "fribidi", "fontconfig", "gdk-pixbuf", "graphene", "libpng", "libtiff",
    "libjpeg-turbo", "pcre2", "libffi", "zlib", "expat", "freetype",
    "graphite2", "sqlite", "libpsl", "nghttp2", "libthai", "libdatrie",
    "libselinux", "util-linux", "pixman", "libglvnd", "libX11", "libxcb",
    "libXau", "libXdmcp", "libXext", "libXft", "libXrender", "xorgproto",
};

// pkg-config searches THESE directories and nothing else.
//
// PKG_CONFIG_LIBDIR replaces the default search path outright (unlike
// PKG_CONFIG_PATH, which prepends to it), so /usr/lib/pkgconfig never
// participates and no host header or library can enter the build. That is the
// whole point: mcpp compiles against xim:glibc, and a GTK built against the
// machine's glibc does not belong in the same binary. --define-prefix
// recomputes each package's `prefix` from the .pc file's own location, which
// is what makes a relocatable payload store work at all.
inline std::string pkg_config_libdir() {
    std::string dirs;
    // A payload enumerated here but not declared in the manifest resolves to
    // an empty directory, and skipping it silently is what hid graphite2 for
    // a release: the omission is felt only once some .pc Requires the
    // missing package, as `Package 'graphite2' was not found`, naming neither
    // this list nor the manifest. Collect the names instead, and say which.
    std::string undeclared;
    std::size_t missing = 0;
    for (std::string_view name : kLinuxPayloads) {
        const std::string root = mcpp::xpkg_dir("xim", std::string(name).c_str());
        if (root.empty()) {
            undeclared += ' ';
            undeclared += name;
            ++missing;
            continue;
        }
        // xorgproto and friends install their .pc under share/, not lib/.
        for (const char* sub : { "/lib/pkgconfig", "/share/pkgconfig", "/lib64/pkgconfig" }) {
            const std::string d = root + sub;
            if (!std::filesystem::is_directory(d)) continue;
            if (!dirs.empty()) dirs += ':';
            dirs += d;
        }
    }
    if (missing != 0) {
        std::cerr << "huxerui.rules: " << missing << " of "
                  << std::size(kLinuxPayloads)
                  << " xlings payloads did not resolve:" << undeclared;
        std::cerr << "\n         they are declared in mcpp/huxerui-build-rules-gtk/mcpp.toml "
                     "under [target.'cfg(all(linux, not(env = \"android\")))'.xlings.workspace]; "
                     "a build program sees them only through that host module.\n";
    }
    return dirs;
}

// pkg-config's output, split into mcpp directives. `-I` colours this package's
// own TUs; `-l` and `-L` reach the final link, which is what puts GTK on an
// application's link line without the application naming it.
inline bool pkg_config(std::string_view module_spec, const std::string& libdir, bool link) {
    const std::string out = std::string(mcpp::out_dir()) + "/pkg-config.txt";
    const std::string cmd = "PKG_CONFIG_LIBDIR='" + libdir + "' "
                            "pkg-config --define-prefix --cflags "
                          + std::string(link ? "--libs " : "") + "'"
                          + std::string(module_spec) + "' > '" + out + "' 2>&1";
    const bool ok = std::system(cmd.c_str()) == 0;

    std::ifstream in(out);
    if (!in) return false;
    if (!ok) {
        std::cerr << "huxerui.rules: pkg-config failed for '" << module_spec << "':\n";
        std::cerr << in.rdbuf() << "\n";
        return false;
    }
    std::string token;
    while (in >> token) {
        if (token.empty())                     continue;
        else if (token.starts_with("-I"))      mcpp::include_dir(token.substr(2).c_str());
        else if (token.starts_with("-l"))      mcpp::link_lib(token.substr(2).c_str());
        else if (token.starts_with("-L"))      mcpp::link_search(token.substr(2).c_str());
        // A linker flag is not a compile flag. Routing -Wl,... to cxxflag would
        // put it on every compile line, where it is at best ignored.
        else if (token.starts_with("-Wl,"))    mcpp::link_flag(token.c_str());
        // DIALECT FLAGS ARE NOT OURS TO EMIT. clang records the target-feature
        // and thread-model set in the BMI it writes and refuses an importer
        // compiled without them (`POSIX thread support was enabled in
        // precompiled file ... but is currently disabled`), so every name in
        // `import huxerui;` becomes undefined. `-pthread` is dropped: no
        // translation unit in the graph carries it and the payload glibc has
        // pthread in libc. The -m flags are dropped: SSE2 is baseline on
        // x86-64, so the code generation is identical, and naming them is
        // what put the feature list in the BMI in the first place.
        else if (token == "-pthread")          {}
        else if (token.starts_with("-msse") ||
                 token.starts_with("-mfpmath")) {}
        else                                   mcpp::cxxflag(token.c_str());
    }
    return true;
}

} // namespace detail

// The GTK stack's compile flags on this package's translation units, and with
// `link` its libraries on the final link. A no-op on every row but the Linux
// desktop, so a build program calls it unconditionally. The framework calls
// it with `link`; a library that includes GTK headers calls it without, since
// the framework's link line already carries the libraries.
//
// Version floors match cmake/platform/Linux.cmake.
inline bool linux_gtk(bool link) {
    if (std::string_view(mcpp::target_os()) != "linux") return true;
    if (std::string_view(mcpp::target_env()) == "android") return true;
    const std::string libdir = detail::pkg_config_libdir();
    if (libdir.empty()) {
        std::cerr << "huxerui.rules: no xlings payload resolved for the Linux GTK stack\n";
        return false;
    }
    constexpr std::string_view modules[] = { "gtk4 >= 4.14", "epoxy >= 1.5", "gio-2.0", "libsoup-3.0 >= 3.0" };
    for (std::string_view m : modules) {
        if (!detail::pkg_config(m, libdir, link)) return false;
    }
    return true;
}

} // namespace huxerui::rules
