// Asserts that mcpp.toml and the CMake build describe the same project.
//
// Two build systems maintaining one set of facts drift, and the drift is not
// loud: adding platform/windows/win32_foo.cpp and forgetting mcpp.toml makes
// the Windows mcpp build fail at link with `undefined reference`, far from the
// cause. Everything checked here is a fact both build systems state; anything
// only one of them owns is deliberately absent.
//
// Linux libraries are NOT compared. The two answer "where does GTK come from"
// differently on purpose -- CMake takes the distribution's, mcpp takes xlings
// payloads -- because they compile against different C libraries.

import std;
import huxerui.rules.sources;
import tomlplusplus;

namespace {

namespace sources = huxerui::rules::sources;

std::vector<std::string> failures;

void fail(std::string message) { failures.push_back(std::move(message)); }

std::string read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<std::string> string_array(const toml::table& table, std::string_view key) {
    std::vector<std::string> out;
    if (const toml::array* array = table[key].as_array()) {
        for (const toml::node& node : *array) {
            if (const auto value = node.value<std::string>()) out.push_back(*value);
        }
    }
    return out;
}

// Files under `root` matching the globs, with `!`-prefixed globs removed.
std::set<std::string> expand(const std::filesystem::path& root,
                             const std::vector<std::string>& globs) {
    std::set<std::string> kept, dropped;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = std::filesystem::relative(it->path(), root, ec).generic_string();
        if (ec) continue;
        for (const std::string& glob : globs) {
            const bool negated = glob.starts_with("!");
            const std::string_view pattern =
                negated ? std::string_view(glob).substr(1) : std::string_view(glob);
            if (sources::matches(pattern, rel)) (negated ? dropped : kept).insert(rel);
        }
    }
    for (const std::string& d : dropped) kept.erase(d);
    return kept;
}

std::string join(const std::set<std::string>& values) {
    std::string out;
    for (const std::string& v : values) {
        if (!out.empty()) out += ", ";
        out += v;
    }
    return out;
}

void check_standard(const std::filesystem::path& root, const toml::table& manifest) {
    const std::string cmake = read(root / "CMakeLists.txt");
    const std::size_t at = cmake.find("set(CMAKE_CXX_STANDARD ");
    if (at == std::string::npos) { fail("cannot find CMAKE_CXX_STANDARD in CMakeLists.txt"); return; }
    const std::size_t begin = at + std::string("set(CMAKE_CXX_STANDARD ").size();
    const std::size_t end = cmake.find(')', begin);
    const std::string expected = "c++" + cmake.substr(begin, end - begin);
    const auto declared = manifest["package"]["standard"].value<std::string>();
    if (!declared || *declared != expected) {
        fail("C++ standard differs: CMakeLists.txt says " + expected + ", mcpp.toml says " +
             declared.value_or("(none)"));
    }
}

void check_core_sources(const std::filesystem::path& root, const toml::table& manifest) {
    const std::string build_cmake = read(root / "cmake" / "HuxerUIBuild.cmake");
    const std::size_t at = build_cmake.find("file(GLOB HUXERUI_CORE_SOURCE_FILES");
    if (at == std::string::npos) { fail("cannot find the core source glob in cmake/HuxerUIBuild.cmake"); return; }
    const std::size_t open = build_cmake.find("${HUXERUI_PROJECT_DIR}/", at);
    const std::size_t close = build_cmake.find('"', open);
    const std::string glob =
        build_cmake.substr(open + std::string("${HUXERUI_PROJECT_DIR}/").size(),
                           close - open - std::string("${HUXERUI_PROJECT_DIR}/").size());

    const std::vector<std::string> declared =
        string_array(*manifest["build"].as_table(), "sources");
    if (std::ranges::find(declared, glob) == declared.end()) {
        fail("core source glob differs: CMake uses '" + glob + "', mcpp.toml does not declare it");
        return;
    }
    if (expand(root, { glob }).empty()) fail("core source glob '" + glob + "' matches no file");
}

void check_platform(const std::filesystem::path& root, const toml::table& manifest,
                    std::string_view cmake_file, std::string_view selector) {
    const std::string text = read(root / "cmake" / "platform" / std::string(cmake_file));
    if (text.empty()) { fail(std::string(cmake_file) + " is missing"); return; }

    const toml::table* target = manifest["target"][selector].as_table();
    if (target == nullptr) { fail("mcpp.toml declares no [target.'" + std::string(selector) + "']"); return; }

    // --- sources -----------------------------------------------------------
    std::set<std::string> from_cmake;
    for (const std::string& entry : sources::cmake_list(text, "HUXERUI_PLATFORM_SOURCE_FILES")) {
        static constexpr std::string_view kPrefix = "${HUXERUI_PROJECT_DIR}/";
        from_cmake.insert(entry.starts_with(kPrefix) ? entry.substr(kPrefix.size()) : entry);
    }
    std::set<std::string> from_mcpp;
    if (const toml::table* build = (*target)["build"].as_table()) {
        from_mcpp = expand(root, string_array(*build, "sources"));
    }
    if (from_cmake != from_mcpp) {
        std::set<std::string> only_cmake, only_mcpp;
        std::ranges::set_difference(from_cmake, from_mcpp,
                                    std::inserter(only_cmake, only_cmake.end()));
        std::ranges::set_difference(from_mcpp, from_cmake,
                                    std::inserter(only_mcpp, only_mcpp.end()));
        std::string detail;
        if (!only_cmake.empty())
            detail += "missing from mcpp.toml: " + join(only_cmake);
        if (!only_mcpp.empty())
            detail += (detail.empty() ? "" : "; ") + std::string("missing from CMake: ") + join(only_mcpp);
        fail(std::string(cmake_file) + ": platform sources differ -- " + detail);
    }

    // --- libraries ---------------------------------------------------------
    const std::vector<std::string> cmake_libraries =
        sources::cmake_list(text, "HUXERUI_PLATFORM_LINK_LIBRARIES");
    if (selector == "windows") {
        std::set<std::string> declared;
        if (const toml::table* runtime = (*target)["runtime"].as_table()) {
            for (const std::string& v : string_array(*runtime, "libraries")) declared.insert(v);
        }
        // Windows appends more in an if() branch; sweep the whole file.
        std::vector<std::string> all = cmake_libraries;
        std::size_t at = 0;
        const std::string append = "list(APPEND HUXERUI_PLATFORM_LINK_LIBRARIES ";
        while ((at = text.find(append, at)) != std::string::npos) {
            const std::size_t close = text.find(')', at);
            std::istringstream in(text.substr(at + append.size(), close - at - append.size()));
            for (std::string token; in >> token;) all.push_back(token);
            at = close;
        }
        std::set<std::string> missing;
        for (const std::string& library : all) {
            if (!library.starts_with("PkgConfig::") && !declared.contains(library))
                missing.insert(library);
        }
        if (!missing.empty())
            fail(std::string(cmake_file) + ": libraries missing from [target.windows.runtime]: " +
                 join(missing));
    } else if (selector == "macos") {
        std::set<std::string> declared;
        if (const toml::table* runtime = manifest["runtime"].as_table()) {
            for (const std::string& v : string_array(*runtime, "frameworks")) declared.insert(v);
        }
        std::string ldflags;
        if (const toml::table* build = (*target)["build"].as_table()) {
            for (const std::string& v : string_array(*build, "ldflags")) ldflags += v + " ";
        }
        std::set<std::string> missing;
        for (const std::string& entry : cmake_libraries) {
            static constexpr std::string_view kWeak = "-weak_framework ";
            static constexpr std::string_view kFramework = "-framework ";
            if (entry.starts_with(kWeak)) {
                const std::string name = entry.substr(kWeak.size());
                if (ldflags.find(name) == std::string::npos)
                    fail(std::string(cmake_file) + ": weak framework '" + name +
                         "' missing from [target.macos.build] ldflags");
            } else if (entry.starts_with(kFramework)) {
                const std::string name = entry.substr(kFramework.size());
                if (!declared.contains(name)) missing.insert(name);
            }
        }
        if (!missing.empty())
            fail(std::string(cmake_file) + ": frameworks missing from [runtime] frameworks: " +
                 join(missing));
    }
    // Linux: both sides go through pkg-config or a payload; nothing to compare.

    // --- defines -----------------------------------------------------------
    std::set<std::string> cmake_defines;
    for (const std::string& d : sources::cmake_list(text, "HUXERUI_PLATFORM_COMPILE_DEFINITIONS")) {
        if (!d.starts_with("$")) cmake_defines.insert(d);
    }
    if (!cmake_defines.empty()) {
        std::set<std::string> declared;
        if (const toml::table* build = (*target)["build"].as_table()) {
            for (const std::string& v : string_array(*build, "defines")) declared.insert(v);
        }
        std::set<std::string> missing;
        std::ranges::set_difference(cmake_defines, declared,
                                    std::inserter(missing, missing.end()));
        if (!missing.empty())
            fail(std::string(cmake_file) + ": defines missing from [target.'" +
                 std::string(selector) + "'.build] defines: " + join(missing));
    }
}


} // namespace

int main(int argc, char** argv) {
    std::filesystem::path root = std::filesystem::current_path();
    if (argc > 2 && std::string_view(argv[1]) == "--root") root = argv[2];
    // BOTH files, not just the manifest. `mcpp run` starts in the tools
    // package, whose own mcpp.toml would end the walk one directory in --
    // observed in CI as "no HuxerUI tree above the working directory" from a
    // checkout that plainly had one.
    const auto is_root = [](const std::filesystem::path& p) {
        return std::filesystem::exists(p / "mcpp.toml") &&
               std::filesystem::exists(p / "CMakeLists.txt");
    };
    while (!is_root(root) && root.has_parent_path() && root.parent_path() != root) {
        root = root.parent_path();
    }
    if (!is_root(root)) {
        std::cerr << "huxerui-build-check: no HuxerUI tree (mcpp.toml + CMakeLists.txt) above "
                     "the working directory\n";
        return 1;
    }

    toml::table manifest;
    try {
        manifest = toml::parse_file((root / "mcpp.toml").string());
    } catch (const toml::parse_error& error) {
        std::cerr << "huxerui-build-check: mcpp.toml does not parse: " << error.description() << "\n";
        return 1;
    }

    check_standard(root, manifest);
    check_core_sources(root, manifest);
    check_platform(root, manifest, "Linux.cmake", "cfg(linux)");
    check_platform(root, manifest, "Windows.cmake", "windows");
    check_platform(root, manifest, "MacOS.cmake", "macos");

    if (!failures.empty()) {
        std::cerr << "mcpp/CMake parity check FAILED:\n\n";
        for (const std::string& message : failures) std::cerr << "  - " << message << "\n";
        std::cerr << "\nBoth build systems describe the same sources and platform interface; "
                     "update whichever one is behind.\n";
        return 1;
    }
    std::cout << "mcpp/CMake parity check passed\n";
    return 0;
}
