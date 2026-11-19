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

// C++23, mcpp's default: a host module is compiled with the package's
// `standard`, and the official dist members it imports need C++23.
import std;
import mcpp;
import huxerui.rules.sources;
import mcpp.dist.appimage;
import mcpp.dist.deb;
import mcpp.dist.rpm;
import mcpp.dist.wix;
import mcpp.dist.apple;
import mcpp.dist.web;
import mcpp.dist.apk;

export namespace huxerui::rules {

// ---------------------------------------------------------------- options --
// Field-for-field the arguments of huxerui_add_app() in cmake/HuxerUIApp.cmake,
// so the two spellings of "an application" can be reviewed side by side.
// What packaging needs beyond the application's identity. Every field has a
// default, so `mcpp pack --pkg-format <format>` works on an application that
// states nothing here; the official `mcpp:plugins` member of each format
// writes the package (`plan_packages` below).
struct package_options {
    std::vector<std::string> categories = {"Utility"};  // Linux desktop entry
    std::string              maintainer;  // deb, rpm; default: the first of [package] authors
    std::vector<std::string> depends;     // deb, rpm: packages the machine provides
    std::string              manufacturer;  // MSI; default: the package namespace, else its name
    // MSI; default: derived from the identifier as the CMake integration derives it.
    std::string              upgrade_code;
};

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
    package_options          package;             // `mcpp pack` formats
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
        // The transformed copy is a new file to the module scanner, so the modules it imports are declared with the
        // action: an interface unit's own name and imports, and the imports of an ordinary unit that has some, such as
        // `import <library>;` beside a composable.
        const auto declared = huxerui::rules::sources::scan_module_interface(text);
        if (abs.extension() == ".cppm") e.provides = declared.name;
        e.imports = declared.imports;
        out.push_back(std::move(e));
    }
    return out;
}

// ------------------------------------------------------------------ graph --
// What an application's build needs from the packages it does not name: every
// HuxerUI library in its dependency closure, in the order the build-system
// contract fixes (depth-first, declaration order, each library once), with the
// identity and resource root each library states about itself in
// `[package.metadata.huxerui]`.
namespace detail {

struct json_value {
    enum class kind { null, boolean, number, string, array, object };
    kind                                             type = kind::null;
    bool                                             flag = false;
    std::string                                      text;
    std::vector<json_value>                          items;
    std::vector<std::pair<std::string, json_value>>  members;

    [[nodiscard]] const json_value* find(std::string_view key) const {
        for (const auto& [name, value] : members)
            if (name == key) return &value;
        return nullptr;
    }
    [[nodiscard]] std::string string_at(std::string_view key) const {
        const json_value* value = find(key);
        return (value != nullptr && value->type == kind::string) ? value->text : std::string{};
    }
};

class json_reader {
public:
    explicit json_reader(std::string_view text) : text_(text) {}

    bool parse(json_value& out) {
        if (!value(out)) return false;
        skip();
        return position_ == text_.size();
    }

private:
    void skip() {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\t' ||
                text_[position_] == '\n' || text_[position_] == '\r'))
            ++position_;
    }
    bool literal(std::string_view word) {
        if (text_.substr(position_, word.size()) != word) return false;
        position_ += word.size();
        return true;
    }
    static void append_utf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    bool string(std::string& out) {
        if (position_ >= text_.size() || text_[position_] != '"') return false;
        ++position_;
        while (position_ < text_.size()) {
            const char c = text_[position_++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (position_ >= text_.size()) return false;
            const char escaped = text_[position_++];
            switch (escaped) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    if (position_ + 4 > text_.size()) return false;
                    unsigned code = 0;
                    for (int digit = 0; digit < 4; ++digit) {
                        const char h = text_[position_++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                        else return false;
                    }
                    append_utf8(out, code);
                    break;
                }
                default: out.push_back(escaped); break;
            }
        }
        return false;
    }
    bool value(json_value& out) {
        skip();
        if (position_ >= text_.size()) return false;
        const char c = text_[position_];
        if (c == '{') {
            ++position_;
            out.type = json_value::kind::object;
            skip();
            if (position_ < text_.size() && text_[position_] == '}') { ++position_; return true; }
            while (true) {
                skip();
                std::string key;
                if (!string(key)) return false;
                skip();
                if (position_ >= text_.size() || text_[position_++] != ':') return false;
                json_value member;
                if (!value(member)) return false;
                out.members.emplace_back(std::move(key), std::move(member));
                skip();
                if (position_ >= text_.size()) return false;
                if (text_[position_] == ',') { ++position_; continue; }
                if (text_[position_] == '}') { ++position_; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++position_;
            out.type = json_value::kind::array;
            skip();
            if (position_ < text_.size() && text_[position_] == ']') { ++position_; return true; }
            while (true) {
                json_value item;
                if (!value(item)) return false;
                out.items.push_back(std::move(item));
                skip();
                if (position_ >= text_.size()) return false;
                if (text_[position_] == ',') { ++position_; continue; }
                if (text_[position_] == ']') { ++position_; return true; }
                return false;
            }
        }
        if (c == '"') {
            out.type = json_value::kind::string;
            return string(out.text);
        }
        if (literal("true"))  { out.type = json_value::kind::boolean; out.flag = true;  return true; }
        if (literal("false")) { out.type = json_value::kind::boolean; out.flag = false; return true; }
        if (literal("null"))  { out.type = json_value::kind::null; return true; }
        const std::size_t start = position_;
        while (position_ < text_.size() &&
               (std::string_view("+-.eE0123456789").find(text_[position_]) != std::string_view::npos))
            ++position_;
        if (position_ == start) return false;
        out.type = json_value::kind::number;
        out.text = std::string(text_.substr(start, position_ - start));
        return true;
    }

    std::string_view text_;
    std::size_t      position_ = 0;
};

inline std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

inline void write_if_changed(const std::filesystem::path& path, const std::string& content) {
    if (std::filesystem::exists(path) && read_text(path) == content) return;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
}

inline std::string json_escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// A boolean of this package's own `[package.metadata.huxerui]` table, written
// bare (`cpp_modules_gen = false`); empty when the key is absent.
inline std::optional<bool> own_flag(std::string_view key) {
    const std::string manifest =
        read_text(std::filesystem::path(mcpp::manifest_dir()) / "mcpp.toml");
    std::istringstream lines(manifest);
    std::string line;
    bool inside = false;
    while (std::getline(lines, line)) {
        const std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        if (line[first] == '[') {
            inside = line.find("[package.metadata.huxerui]") != std::string::npos;
            continue;
        }
        const std::size_t equals = line.find('=');
        if (!inside || equals == std::string::npos) continue;
        std::string name = line.substr(first, equals - first);
        name.erase(name.find_last_not_of(" \t") + 1);
        if (name != key) continue;
        const std::string value(huxerui::rules::sources::detail::trim(
            std::string_view(line).substr(equals + 1, line.find('#', equals) - equals - 1)));
        if (value == "true") return true;
        if (value == "false") return false;
        mcpp::warning(("huxerui.rules: [package.metadata.huxerui] " + std::string(key) +
                       " must be true or false; got " + value).c_str());
        return std::nullopt;
    }
    return std::nullopt;
}

// One value of a table of this package's own manifest. A dependency's build
// program is not handed the resolved graph, so a package reads what it states
// about itself from its own manifest.
inline std::string own_value(std::string_view table, std::string_view key) {
    const std::string manifest =
        read_text(std::filesystem::path(mcpp::manifest_dir()) / "mcpp.toml");
    const std::string header = "[" + std::string(table) + "]";
    std::istringstream lines(manifest);
    std::string line;
    bool inside = false;
    while (std::getline(lines, line)) {
        const std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        if (line[first] == '[') {
            inside = line.compare(first, header.size(), header) == 0;
            continue;
        }
        if (!inside) continue;
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string name = line.substr(first, equals - first);
        name.erase(name.find_last_not_of(" \t") + 1);
        if (name != key) continue;
        const std::size_t open = line.find('"', equals);
        const std::size_t close = open == std::string::npos ? open : line.find('"', open + 1);
        if (close == std::string::npos) return {};
        return line.substr(open + 1, close - open - 1);
    }
    return {};
}

// One value of this package's own `[package.metadata.huxerui]` table.
inline std::string own_metadata(std::string_view key) {
    return own_value("package.metadata.huxerui", key);
}

} // namespace detail

// The resource namespace a library's public identity projects to, by the rule
// the CLI and the CMake helpers use: each `::` segment lower-cased, the
// segments joined by `_`, and one segment when both are the same token
// (`CameraKit::CameraKit` is `camerakit`, `Scave::Camera` is `scave_camera`).
inline std::string library_namespace(std::string_view identity) {
    std::vector<std::string> segments;
    std::string current;
    for (std::size_t index = 0; index < identity.size(); ++index) {
        if (identity[index] == ':') {
            if (!current.empty()) segments.push_back(std::move(current));
            current.clear();
            continue;
        }
        current.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(identity[index]))));
    }
    if (!current.empty()) segments.push_back(std::move(current));
    if (segments.empty()) return {};
    if (segments.size() == 2 && segments[0] == segments[1]) return segments[0];
    std::string out = segments[0];
    for (std::size_t index = 1; index < segments.size(); ++index) out += "_" + segments[index];
    return out;
}

struct library {
    std::string identity;    // the public identity, e.g. CameraKit::CameraKit
    std::string root;        // the library's source root
    std::string resources;   // its absolute resource root; empty when it has none
    std::string id;          // its reverse-domain identifier, the Android package its Gradle module declares
    // The package states `[package.metadata.dist-apk]`, which dist-apk reads itself in place of the inference.
    bool        states_dist_apk = false;
};

struct project_graph {
    bool                      is_root = false;
    std::vector<std::string>  programs;    // the root package's program targets
    std::vector<library>      libraries;   // contract order
};

inline project_graph load_graph() {
    project_graph graph;
    const std::string file = mcpp::graph_file();
    if (file.empty()) return graph;

    detail::json_value document;
    const std::string text = detail::read_text(file);
    if (!detail::json_reader(text).parse(document)) {
        std::cerr << "huxerui.rules: cannot read the resolved graph " << file << "\n";
        return graph;
    }
    const detail::json_value* packages = document.find("packages");
    if (packages == nullptr) return graph;
    graph.is_root = true;

    std::map<std::string, std::size_t> by_canonical;
    std::size_t root_index = packages->items.size();
    for (std::size_t index = 0; index < packages->items.size(); ++index) {
        const detail::json_value& entry = packages->items[index];
        if (const detail::json_value* package = entry.find("package"))
            by_canonical[package->string_at("canonical")] = index;
        const detail::json_value* root = entry.find("root");
        if (root != nullptr && root->flag) root_index = index;
    }
    if (root_index == packages->items.size()) return graph;

    if (const detail::json_value* targets = packages->items[root_index].find("targets")) {
        for (const detail::json_value& target : targets->items) {
            const std::string kind = target.string_at("kind");
            if (kind == "bin" || kind == "app") graph.programs.push_back(target.string_at("name"));
        }
    }

    // Each requester's dependencies in the order its manifest declares them:
    // the graph lists dependencies before their requesters, not in declaration
    // order, and the contract's order is the declaration's.
    std::map<std::size_t, std::vector<std::pair<std::size_t, std::size_t>>> children;
    std::map<std::size_t, std::string> manifests;
    for (std::size_t index = 0; index < packages->items.size(); ++index) {
        const detail::json_value* requests = packages->items[index].find("requested_by");
        if (requests == nullptr) continue;
        for (const detail::json_value& request : requests->items) {
            // Only what the produced program links: a build-dependency is a
            // tool or a rule module, not a library of the application.
            if (request.string_at("table") != "[dependencies]") continue;
            const auto requester = by_canonical.find(request.string_at("requester"));
            if (requester == by_canonical.end()) continue;
            auto manifest = manifests.find(requester->second);
            if (manifest == manifests.end()) {
                const std::string dir = packages->items[requester->second].string_at("manifest_dir");
                manifest = manifests.emplace(requester->second,
                    detail::read_text(std::filesystem::path(dir) / "mcpp.toml")).first;
            }
            std::size_t position = manifest->second.find("\n" + request.string_at("key"));
            if (position == std::string::npos) position = manifest->second.size() + index;
            children[requester->second].emplace_back(position, index);
        }
    }
    for (auto& [requester, list] : children) std::ranges::sort(list);

    std::vector<bool> visited(packages->items.size(), false);
    const auto visit = [&](auto&& self, std::size_t index) -> void {
        const auto found = children.find(index);
        if (found == children.end()) return;
        for (const auto& [position, child] : found->second) {
            if (visited[child]) continue;
            visited[child] = true;
            const detail::json_value& entry = packages->items[child];
            const detail::json_value* metadata = entry.find("metadata");
            const detail::json_value* huxerui = metadata ? metadata->find("huxerui") : nullptr;
            const std::string identity = huxerui ? huxerui->string_at("library") : std::string{};
            if (!identity.empty()) {
                library lib;
                lib.identity = identity;
                lib.root = entry.string_at("manifest_dir");
                lib.id = huxerui->string_at("id");
                lib.states_dist_apk = metadata->find("dist-apk") != nullptr;
                const std::string resources = huxerui->string_at("resources");
                if (!resources.empty())
                    lib.resources = (std::filesystem::path(lib.root) / resources).string();
                graph.libraries.push_back(std::move(lib));
            }
            self(self, child);
        }
    };
    visited[root_index] = true;
    visit(visit, root_index);
    return graph;
}

// The ordered library-graph record of the build-system contract, in the
// location and with the schema the CMake integration uses, so a platform shell
// reads the same file whichever integration built the application.
inline void write_library_graph(const project_graph& graph) {
    std::string content = "{\n  \"schema\": 1,\n  \"libraries\": [\n";
    for (std::size_t index = 0; index < graph.libraries.size(); ++index) {
        const library& lib = graph.libraries[index];
        const std::string root =
            std::filesystem::weakly_canonical(lib.root).generic_string();
        content += "    {\"target\": \"" + detail::json_escape(lib.identity) +
                   "\", \"sourceRoot\": \"" + detail::json_escape(root) + "\"}";
        content += index + 1 < graph.libraries.size() ? ",\n" : "\n";
    }
    content += "  ]\n}\n";
    const std::filesystem::path record =
        std::filesystem::path(mcpp::manifest_dir()) / ".huxerui" / "generated" / "libraries.json";
    detail::write_if_changed(record, content);
    // The record is generated metadata a developer may delete with `.huxerui/`;
    // naming it as an input makes the next build run this program and restore it.
    mcpp::rerun_if_changed(record.string().c_str());
}

// Where the framework the application resolved lives, for the platform shells: the Gradle shell includes the
// framework's Android library module from it, as a CMake build takes it from HUXERUI_HOME. mcpp resolves the
// framework, so only this build knows the directory.
inline void write_framework_record() {
    const std::string root = std::filesystem::weakly_canonical(sdk_root()).generic_string();
    const std::filesystem::path record =
        std::filesystem::path(mcpp::manifest_dir()) / ".huxerui" / "generated" / "framework.json";
    detail::write_if_changed(record, "{\n  \"schema\": 1,\n  \"sourceRoot\": \"" + detail::json_escape(root) +
                                     "\"\n}\n");
    mcpp::rerun_if_changed(record.string().c_str());
}

// --------------------------------------------------------------- resources --
// The shape of cmake/HuxerUIResourceBuild.cmake: compile each root, merge the
// packages in contract order, and place the one final package where the
// Runtime looks for it.
//
// The framework's builtin package and each library's package are compiled
// HERE, in the application's build. A consumer is handed a dependency's SOURCE
// root and never its MCPP_OUT_DIR; `resources/` is source, so it is reachable,
// and the compiled package is not.
namespace detail {

inline std::vector<std::string> files_below(const std::string& root) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file(ec)) out.push_back(it->path().string());
    }
    std::ranges::sort(out);
    return out;
}

// The payload files hrc writes for one root, relative to a package directory.
// A deployed file has to be a declared output, and a declared output has to be
// named before hrc runs, so the set is predicted from the root by hrc's own
// rule: images keep their path, an SVG becomes `.huxv`, raw files keep theirs.
inline std::vector<std::string> payloads(const std::string& root, const std::string& ns) {
    std::vector<std::string> out;
    const std::filesystem::path images = std::filesystem::path(root) / "images";
    for (const std::string& file : files_below(images.string())) {
        std::filesystem::path relative = std::filesystem::relative(file, images);
        std::string extension = relative.extension().string();
        std::ranges::transform(extension, extension.begin(), [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        if (extension == ".svg") relative.replace_extension(".huxv");
        else if (extension != ".png" && extension != ".jpg" && extension != ".jpeg") continue;
        out.push_back("huxerui/" + ns + "/images/" + relative.generic_string());
    }
    const std::filesystem::path raw = std::filesystem::path(root) / "raw";
    for (const std::string& file : files_below(raw.string())) {
        out.push_back("huxerui/" + ns + "/raw/" +
                      std::filesystem::relative(file, raw).generic_string());
    }
    return out;
}

} // namespace detail

// The final package's files below `prefix` of the staged tree, which a package
// format places where its platform's Runtime reads them.
inline void deploy_package(const std::string& final_package, const std::vector<std::string>& files,
                           const std::string& prefix) {
    for (const std::string& file : files) {
        std::string directory = std::filesystem::path(file).parent_path().generic_string();
        if (!prefix.empty()) directory = prefix + "/" + directory;
        mcpp::deploy((final_package + "/" + file).c_str(), directory.c_str());
    }
}

inline std::vector<edge> plan_resources(const options& opt, const project_graph& graph,
                                        bool resource_module) {
    std::vector<edge> out;
    const std::string hrc = host_tool("hrc");
    if (hrc.empty()) return out;

    const std::string root = sdk_root();
    const std::string odir = std::string(mcpp::out_dir()) + "/hrc";
    const std::string builtin_src = root + "/resources";

    std::vector<std::string> packages;
    std::vector<std::string> package_files{ "huxerui/resources.bin" };
    const auto add_root = [&](std::string id, std::string description, const std::string& source,
                              const std::string& ns, const std::string& directory,
                              std::string header) {
        std::vector<std::string> inputs = detail::files_below(source);
        inputs.insert(inputs.begin(), hrc);
        edge compile{
            .id          = std::move(id),
            .role        = "source",
            .description = std::move(description),
            .command     = { hrc, "--root", source, "--output", directory, "--namespace", ns },
            .inputs      = std::move(inputs),
            .outputs     = { directory + "/include/" + (header.empty() ? ns + "_resources.h" : header),
                             directory + "/package/huxerui/resources.bin" },
        };
        if (!header.empty()) {
            compile.command.push_back("--header-name");
            compile.command.push_back(header);
        }
        for (const std::string& payload : detail::payloads(source, ns)) {
            compile.outputs.push_back(directory + "/package/" + payload);
            if (std::ranges::find(package_files, payload) == package_files.end())
                package_files.push_back(payload);
        }
        out.push_back(std::move(compile));
        packages.push_back(directory + "/package");
    };

    if (std::filesystem::exists(builtin_src)) {
        add_root("hrc:builtin", "huxerui builtin resources", builtin_src, "huxerui",
                 odir + "/builtin", "huxerui_builtin_resources.h");
    }

    for (const library& lib : graph.libraries) {
        if (lib.resources.empty() || !std::filesystem::is_directory(lib.resources)) continue;
        const std::string ns = library_namespace(lib.identity);
        add_root("hrc:library:" + ns, "library resources " + lib.identity, lib.resources, ns,
                 odir + "/library/" + ns, {});
    }

    // This package's own root: an application's is the namespace `app` unless
    // it states another; a library's is the one its public identity projects.
    const std::string own_identity = detail::own_metadata("library");
    std::string own_root = opt.resources.empty() ? detail::own_metadata("resources") : opt.resources;
    if (!own_root.empty()) {
        const std::string ns =
            !opt.resource_namespace.empty() ? opt.resource_namespace
            : !own_identity.empty()         ? library_namespace(own_identity)
                                            : std::string("app");
        // ABSOLUTE. An action's command runs with the BUILD directory as its
        // working directory, not the package root.
        const std::string source =
            (std::filesystem::path(mcpp::manifest_dir()) / own_root).string();
        add_root("hrc:own", "resources " + own_root, source, ns, odir + "/own", {});
        mcpp::include_dir((odir + "/own/include").c_str());
        if (resource_module) {
            // A package written as module units reaches its resources with
            // `import <namespace>.resources;` and needs no header. The unit
            // re-exports the generated accessors: the framework header is
            // included in the global module fragment, so the second inclusion
            // inside `export { }` declares only the package's own names.
            const std::string unit = odir + "/own/module/" + ns + "_resources.cppm";
            detail::write_if_changed(unit,
                "module;\n\n#include <huxerui/resource.h>\n\nexport module " + ns +
                ".resources;\n\nexport {\n#include <" + ns + "_resources.h>\n}\n");
            mcpp::source(unit.c_str());
        }
    }

    if (!graph.is_root || graph.programs.empty()) return out;

    std::string final_package = packages.empty() ? std::string{} : packages.front();
    if (packages.size() > 1) {
        edge merge{
            .id          = "hrc:merge",
            .role        = "source",
            .description = "merge huxerui resource packages",
            .command     = { hrc, "merge" },
            .inputs      = { hrc },
            .outputs     = {},
        };
        for (const std::string& package : packages) {
            merge.command.push_back("--input");
            merge.command.push_back(package);
            merge.inputs.push_back(package + "/huxerui/resources.bin");
        }
        merge.command.push_back("--output");
        merge.command.push_back(odir + "/final");
        final_package = odir + "/final/package";
        for (const std::string& file : package_files)
            merge.outputs.push_back(final_package + "/" + file);
        out.push_back(std::move(merge));
    }

    if (final_package.empty()) return out;

    if (std::string_view(mcpp::target_os()) == "macos") {
        // A macOS application is a bundle, and the Runtime reads the package
        // from the bundle's resources. The shell owns Info.plist and the icon;
        // the rule assembles `<program>.app` beside the linked program, one
        // file per action: the engine copies each (`${mcpp.self} stage`), so no
        // action is a shell script.
        const std::filesystem::path shell =
            std::filesystem::path(mcpp::manifest_dir()) / "platform" / "macos";
        for (const std::string& program : graph.programs) {
            const std::string bundle = "${mcpp.bin_dir}/" + program + ".app/Contents";
            const auto copy = [&](const std::string& id, const std::string& from, const std::string& to) {
                out.push_back(edge{
                    .id          = "bundle:" + program + ":" + id,
                    .role        = "artifact",
                    .description = "macOS application bundle " + program + ".app: " + id,
                    .command     = { "${mcpp.self}", "stage", "--verify", "content", "--output", to, from },
                    .inputs      = { from },
                    .outputs     = { to },
                });
            };
            copy("program", "${mcpp.target_file:" + program + "}", bundle + "/MacOS/" + program);
            for (std::size_t index = 0; index < package_files.size(); ++index)
                copy("package-" + std::to_string(index), final_package + "/" + package_files[index],
                     bundle + "/Resources/HuxerUI/" + package_files[index]);

            const std::filesystem::path plist_template = shell / "Info.plist.in";
            if (std::filesystem::exists(plist_template)) {
                std::string plist = detail::read_text(plist_template);
                const auto replace = [&](std::string_view token, std::string_view value) {
                    for (std::size_t at = plist.find(token); at != std::string::npos;
                         at = plist.find(token, at + value.size()))
                        plist.replace(at, token.size(), value);
                };
                replace("${MACOSX_BUNDLE_EXECUTABLE_NAME}", program);
                replace("${MACOSX_BUNDLE_SHORT_VERSION_STRING}", mcpp::package_version());
                replace("${MACOSX_BUNDLE_BUNDLE_VERSION}", mcpp::package_version());
                const std::string rendered = odir + "/bundle/" + program + "/Info.plist";
                detail::write_if_changed(rendered, plist);
                mcpp::rerun_if_changed(plist_template.string().c_str());
                copy("info-plist", rendered, bundle + "/Info.plist");
            } else {
                // A bundle without Info.plist has no identifier, and AppKit services refuse to start in it.
                mcpp::warning(("huxerui.rules: " + plist_template.string() +
                               " is missing; the macOS bundle will have no Info.plist").c_str());
            }
            const std::filesystem::path icon = shell / "AppIcon.icns";
            if (std::filesystem::exists(icon))
                copy("icon", icon.generic_string(), bundle + "/Resources/AppIcon.icns");
        }
        deploy_package(final_package, package_files, "HuxerUI");
        return out;
    }

    if (std::string_view(mcpp::target_os()) == "ios") {
        // The iOS application core the Xcode shell links, in the layout the CMake
        // integration uses: one static archive with the application, its libraries
        // and the framework, the link inputs that go with it, and the package.
        // The shell supplies the process entry, so the entry object stays out.
        std::vector<std::string> frameworks;
        {
            const std::string manifest = detail::read_text(std::filesystem::path(root) / "mcpp.toml");
            const std::size_t table = manifest.find("[target.'cfg(os = \"ios\")'.runtime]");
            const std::size_t open = table == std::string::npos ? table : manifest.find('[', table + 1);
            const std::size_t close = open == std::string::npos ? open : manifest.find(']', open);
            if (close != std::string::npos) {
                std::size_t at = open;
                while ((at = manifest.find('"', at + 1)) != std::string::npos && at < close) {
                    const std::size_t end = manifest.find('"', at + 1);
                    frameworks.push_back(manifest.substr(at + 1, end - at - 1));
                    at = end;
                }
            }
        }
        // The application dist-apple writes reads the package from the bundle's
        // root, where its deployed files land.
        deploy_package(final_package, package_files, "HuxerUI");
        for (const std::string& program : graph.programs) {
            const std::string core = "huxerui-ios/" + program;
            for (const std::string& file : package_files) {
                const std::string directory =
                    std::filesystem::path(core + "/resources/package/" + file).parent_path().generic_string();
                mcpp::deploy((final_package + "/" + file).c_str(), directory.c_str());
            }
            const std::string directory = "${mcpp.bin_dir}/" + core;
            const std::string archive = directory + "/lib" + program + "_huxerui.a";
            const std::string linked = "${mcpp.target_file:" + program + "}";
            // The frameworks are known now; the archive's absolute path is known only where the action runs.
            std::string tail;
            for (const std::string& framework : frameworks) tail += "-framework " + framework + "\n";
            tail += "-weak_framework UniformTypeIdentifiers\n";
            const std::string link_tail = odir + "/ios/" + program + "/link-tail.rsp";
            detail::write_if_changed(link_tail, tail);
            // The shell compiles its own entry, so the program's `platform/ios/App/main.mm` stays out of the core.
            // mcpp files the root package's objects flat, as `obj/main.o`, unless two of its sources share a name,
            // and below their directories otherwise; both places are left out.
            const std::string script =
                "set -e; mkdir -p '" + directory + "'; rm -f '" + archive + "'; "
                "find obj -name '*.o' ! -path '*platform/ios/App/*' ! -path 'obj/main.o' -print0 | "
                "xargs -0 libtool -static -o '" + archive +
                "'; core=$(cd '" + directory + "' && pwd); "
                "{ printf '%s\\n\"%s\"\\n' -force_load \"$core/lib" + program + "_huxerui.a\"; cat '" + link_tail +
                "'; } > '" + directory + "/link.rsp'";
            out.push_back(edge{
                .id          = "ios-core:" + program,
                .role        = "artifact",
                .description = "iOS application core " + program,
                .command     = { "/bin/sh", "-c", script },
                .inputs      = { linked, link_tail },
                .outputs     = { archive, directory + "/link.rsp" },
            });
        }
        return out;
    }

    if (std::string_view(mcpp::target_os()) == "emscripten") {
        // The Web has no directory beside the program: the package is preloaded
        // into the module's virtual file system at its root.
        mcpp::link_flag("--preload-file");
        mcpp::link_flag((final_package + "@/").c_str());
        return out;
    }

    // Android reads the package from the APK's assets, where dist-apk packs the
    // deployed files.
    if (std::string_view(mcpp::target_env()) == "android") {
        deploy_package(final_package, package_files, {});
        return out;
    }

    // Where the Runtime looks on the other desktop platforms: `<program>.resources/`
    // beside the program, the directory every integration stages the package into.
    for (const std::string& program : graph.programs) {
        for (const std::string& file : package_files) {
            const std::string directory =
                std::filesystem::path(program + ".resources/" + file).parent_path().generic_string();
            mcpp::deploy((final_package + "/" + file).c_str(), directory.c_str());
        }
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

// --------------------------------------------------------------- packages --
// `mcpp pack --pkg-format <format>`: the official `mcpp:plugins` member of each
// format writes the package from the tree the engine stages, which holds the
// program, its closure and the resource package deployed beside it. This rule
// supplies the application's identity and its platform shell's inputs; the
// members declare their format in every pass and act only in the pass that
// asked for it, so configuring all of them costs an ordinary build nothing.
namespace detail {

struct identity {
    std::string name;   // the display name
    std::string id;     // the reverse-DNS identifier
};

inline identity application_identity(const options& opt) {
    identity out;
    out.name = !opt.bundle_name.empty() ? opt.bundle_name : own_metadata("name");
    if (out.name.empty()) out.name = std::string(mcpp::package_name());
    out.id = !opt.bundle_identifier.empty() ? opt.bundle_identifier : own_metadata("id");
    return out;
}

// The first `authors` entry of [package], which is what dist-deb and dist-rpm
// read themselves; returned so a stated maintainer and the default share a path.
inline std::string first_author() {
    const std::string manifest = read_text(std::filesystem::path(mcpp::manifest_dir()) / "mcpp.toml");
    const std::size_t key = manifest.find("\nauthors");
    if (key == std::string::npos) return {};
    const std::size_t open = manifest.find('"', key);
    const std::size_t close = open == std::string::npos ? open : manifest.find('"', open + 1);
    return close == std::string::npos ? std::string{} : manifest.substr(open + 1, close - open - 1);
}

inline std::string shell_file(std::string_view platform, std::string_view file) {
    const std::filesystem::path path = std::filesystem::path(mcpp::manifest_dir()) / "platform" / platform / file;
    return std::filesystem::exists(path) ? path.string() : std::string{};
}

// The DNS namespace CMake's string(UUID ...) uses for the same name.
inline constexpr std::string_view uuid_namespace = "6ba7b810-9dad-11d1-80b4-00c04fd430c8";

} // namespace detail

// The Android application shell the CMake integration's Gradle template holds:
// an Application that loads the program's library and an Activity on the
// framework's. The framework's own Java host and manifest entries arrive through
// `[package.metadata.dist-apk]` in its manifest, which dist-apk reads from the
// resolved graph.
inline bool plan_android_shell(const std::string& program, const detail::identity& app,
                               mcpp::dist::apk::options& apk) {
    if (app.id.empty()) {
        mcpp::warning("huxerui.rules: an Android package needs the application identifier; "
                      "state `id` under [package.metadata.huxerui] to pack one");
        return false;
    }
    std::string path = app.id;
    std::ranges::replace(path, '.', '/');
    const std::string root = std::string(mcpp::out_dir()) + "/android";
    detail::write_if_changed(root + "/java/" + path + "/MainApplication.java",
        "package " + app.id + ";\n\nimport android.app.Application;\nimport android.content.Context;\n\n"
        "public final class MainApplication extends Application {\n"
        "    @Override\n"
        "    protected void attachBaseContext(Context base) {\n"
        "        super.attachBaseContext(base);\n"
        "        System.loadLibrary(\"" + program + "\");\n"
        "    }\n"
        "}\n");
    detail::write_if_changed(root + "/java/" + path + "/MainActivity.java",
        "package " + app.id + ";\n\nimport org.huxerui.HuxerUIActivity;\n\n"
        "public final class MainActivity extends HuxerUIActivity {\n}\n");
    detail::write_if_changed(root + "/AndroidManifest.xml.in",
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
        "    package=\"{{application_id}}\"\n"
        "    android:versionCode=\"{{version_code}}\"\n"
        "    android:versionName=\"{{version_name}}\">\n"
        "    <uses-sdk android:minSdkVersion=\"{{min_sdk}}\" android:targetSdkVersion=\"{{target_sdk}}\" />\n"
        "    <application\n"
        "        android:name=\".MainApplication\"\n"
        "        android:label=\"{{label}}\"\n"
        "        android:supportsRtl=\"true\"\n"
        "        android:theme=\"@android:style/Theme.Material.Light.NoActionBar\">\n"
        "        <activity\n"
        "            android:name=\"{{activity}}\"\n"
        "            android:configChanges=\"orientation|screenSize|smallestScreenSize|screenLayout|uiMode\"\n"
        "            android:exported=\"true\"\n"
        "            android:windowSoftInputMode=\"adjustResize\">\n"
        "            <intent-filter>\n"
        "                <action android:name=\"android.intent.action.MAIN\" />\n"
        "                <category android:name=\"android.intent.category.LAUNCHER\" />\n"
        "            </intent-filter>\n"
        "        </activity>\n"
        "    </application>\n"
        "</manifest>\n");
    apk.java_sources      = {root + "/java"};
    apk.activity          = app.id + ".MainActivity";
    apk.manifest_template = root + "/AndroidManifest.xml.in";
    return true;
}

// What a library's Android platform package contributes to a package without the Gradle shell, inferred from the
// standard layout of a Gradle library module at `platform/android`: Java sources, the manifest, resources and
// assets under `src/main`, under the package its identity names, which is the namespace the CLI gives that module.
// A library that states `[package.metadata.dist-apk]` is not inferred: dist-apk reads that table from the graph,
// and it replaces the inference as a whole.
inline std::vector<mcpp::dist::apk::library> android_library_contributions(const project_graph& graph) {
    std::vector<mcpp::dist::apk::library> out;
    for (const library& lib : graph.libraries) {
        if (lib.states_dist_apk) continue;
        const std::filesystem::path main = std::filesystem::path(lib.root) / "platform" / "android" / "src" / "main";
        if (!std::filesystem::is_directory(main)) continue;
        mcpp::dist::apk::library contribution;
        const auto existing = [&](const char* name) {
            const std::filesystem::path path = main / name;
            return std::filesystem::exists(path) ? path.generic_string() : std::string{};
        };
        contribution.resources = existing("res");
        contribution.manifest  = existing("AndroidManifest.xml");
        contribution.assets    = existing("assets");
        if (const std::string java = existing("java"); !java.empty()) contribution.java_sources = {java};
        if (const std::string kotlin = existing("kotlin"); !kotlin.empty()) contribution.kotlin_sources = {kotlin};
        if (contribution.resources.empty() && contribution.manifest.empty() && contribution.assets.empty() &&
            contribution.java_sources.empty() && contribution.kotlin_sources.empty())
            continue;
        if (!contribution.resources.empty()) {
            if (lib.id.empty()) {
                mcpp::warning(("huxerui.rules: the library " + lib.identity + " has Android resources but no `id` "
                               "under [package.metadata.huxerui], which names the package they link under; "
                               "its resources are not packaged").c_str());
                contribution.resources.clear();
            }
        }
        contribution.package = lib.id;
        out.push_back(std::move(contribution));
    }
    return out;
}

inline bool plan_packages(const options& opt, const std::string& program, const project_graph& graph) {
    const std::string_view os = mcpp::target_os();
    const bool android = std::string_view(mcpp::target_env()) == "android";
    const detail::identity app = detail::application_identity(opt);
    bool ok = true;

    if (os == "linux" && !android) {
        const std::string icon = detail::shell_file("linux", "package/" + program + ".svg");
        mcpp::dist::appimage::options image;
        image.target     = program;
        image.app_name   = app.name;
        image.categories = opt.package.categories;
        image.terminal   = false;
        image.icon       = icon;
        ok = mcpp::dist::appimage::generate(image) && ok;

        mcpp::dist::linux_package::options package;
        package.target     = program;
        package.app_name   = app.name;
        package.categories = opt.package.categories;
        package.terminal   = false;
        package.icon       = icon;
        package.maintainer = !opt.package.maintainer.empty() ? opt.package.maintainer : detail::first_author();
        package.depends    = opt.package.depends;
        ok = mcpp::dist::deb::generate(package) && ok;
        ok = mcpp::dist::rpm::generate(package) && ok;
    } else if (os == "windows") {
        std::string upgrade_code = opt.package.upgrade_code;
        if (upgrade_code.empty() && !app.id.empty())
            upgrade_code = huxerui::rules::sources::name_uuid(detail::uuid_namespace, app.id + ".msi");
        if (!upgrade_code.empty() && !huxerui::rules::sources::is_upgrade_code(upgrade_code)) {
            std::cerr << "huxerui.rules: package.upgrade_code must be a GUID (8-4-4-4-12 hex digits); got '"
                      << upgrade_code << "'\n";
            return false;
        }
        std::string maker = opt.package.manufacturer;
        if (maker.empty()) maker = std::string(mcpp::package_namespace());
        if (maker.empty()) maker = std::string(mcpp::package_name());
        mcpp::dist::wix::options installer;
        installer.target       = program;
        installer.product_name = app.name;
        installer.manufacturer = maker;
        installer.upgrade_code = upgrade_code;
        ok = mcpp::dist::wix::generate(installer) && ok;
    } else if (os == "macos" || os == "ios") {
        mcpp::dist::apple::options bundle;
        bundle.target    = program;
        bundle.app_name  = app.name;
        bundle.bundle_id = app.id;
        if (os == "macos") bundle.icon = detail::shell_file("macos", "AppIcon.icns");
        if (os == "ios") {
            // A library's Swift package reaches the application through the Xcode shell, which adds it as a local
            // package. Without the shell nothing compiles it: rules-swift compiles only the Swift sources a package
            // names in its own manifest, and a build program cannot add one.
            for (const library& lib : graph.libraries) {
                std::error_code error;
                const std::filesystem::path platform = std::filesystem::path(lib.root) / "platform" / "ios";
                for (std::filesystem::recursive_directory_iterator entry(platform, error), end;
                     !error && entry != end; entry.increment(error)) {
                    if (entry->path().filename() != "Package.swift") continue;
                    mcpp::warning(("huxerui.rules: the library " + lib.identity + " has the Swift package " +
                                   entry->path().parent_path().generic_string() + ", which only the Xcode shell "
                                   "builds; this package of the application does not include it").c_str());
                    break;
                }
            }
        }
        ok = mcpp::dist::apple::generate(bundle) && ok;
    } else if (os == "emscripten") {
        // The shell's page, rendered with the values the CMake integration gives
        // it; dist-web copies it beside the module as the package's entry.
        const std::string page = detail::shell_file("web", "index.html.in");
        mcpp::dist::web::options web;
        web.target = program;
        web.title  = app.name;
        if (!page.empty()) {
            const std::string directory = std::string(mcpp::out_dir()) + "/huxerui-web";
            std::vector<std::pair<std::string, std::string>> values{
                {"HUXERUI_WEB_MODULE_FILE", program + ".js"},
                {"HUXERUI_WEB_STORAGE_KEY", app.id.empty() ? program : app.id},
            };
            for (const auto& [token, file] : {std::pair{"HUXERUI_WEB_FAVICON_FILE", "favicon.svg"},
                                              std::pair{"HUXERUI_WEB_APPLE_TOUCH_ICON_FILE", "apple-touch-icon.png"}}) {
                const std::string source = detail::shell_file("web", file);
                const std::string staged = program + "." + file;
                values.emplace_back(token, staged);
                if (source.empty()) continue;
                const std::string copy = directory + "/" + staged;
                detail::write_if_changed(copy, detail::read_text(source));
                mcpp::rerun_if_changed(source.c_str());
                mcpp::deploy(copy.c_str(), ".");
            }
            std::string error;
            const std::string rendered =
                huxerui::rules::sources::render_at_tokens(detail::read_text(page), values, error);
            if (!error.empty()) {
                std::cerr << "huxerui.rules: " << page << ": " << error << "\n";
                return false;
            }
            web.template_file = directory + "/" + program + ".index.html.in";
            detail::write_if_changed(web.template_file, rendered);
            mcpp::rerun_if_changed(page.c_str());
            // The CMake build lays the page beside the module under the program's name, where `huxerui run web
            // --watch` serves it from; so does this one. mcpp pack stages it with the module, so a web package
            // carries it beside dist-web's `index.html`.
            const std::string beside = directory + "/" + program + ".html";
            detail::write_if_changed(beside, rendered);
            mcpp::deploy(beside.c_str(), ".");
        }
        ok = mcpp::dist::web::generate(web) && ok;
    } else if (android) {
        mcpp::dist::apk::options apk;
        apk.target         = program;
        apk.application_id = app.id;
        apk.label          = app.name;
        // The graph lists requesters before the libraries they depend on, the order of precedence dist-apk takes.
        apk.libraries      = android_library_contributions(graph);
        // Without an identifier there is no package to configure; the build itself goes on.
        if (plan_android_shell(program, app, apk)) ok = mcpp::dist::apk::generate(apk) && ok;
    }
    return ok;
}

// -------------------------------------------------------------- configure --
// One call for the common case. Returns false only on a condition that should
// stop the build; a missing optional input is reported and tolerated.
// A library that keeps its public headers and declares mcpp exports them as a module too. Its `[lib]` path, wherever
// the author puts it, holds the interface: the headers in its global module fragment and a `using` declaration for
// each name the headers declare in the library's C++ `namespace`, so `import <module>;` and `#include` reach the same
// entities. The module is named after the package identity. With `cpp_modules_gen` absent or true the file is
// generated from the headers on every build that changes them; with `false` it is the author's, and the names it
// leaves out are reported. A module carries no macro: the author maps the macros importers need to constants in a
// unit named `<module>.macros`, conventionally modules/macros.cppm. The units below modules/ are compiled, and the
// interface re-exports those named under the module. Returns whether the package has such an interface.
inline bool plan_module_interface() {
    namespace sources = huxerui::rules::sources;
    const std::filesystem::path root = mcpp::manifest_dir();
    // Whether the package is a module at all follows the manifest, its headers, and its module units, and the rules
    // may run first while one of them is still missing, as when a headers library is being made a module: watched
    // only once the package is one, adding the last of them would never run the rules again. The set of headers
    // decides the interface too, and `cpp_modules_gen` is read from the manifest.
    mcpp::rerun_if_changed((root / "mcpp.toml").string().c_str());
    mcpp::rerun_if_changed_glob("include/**");
    mcpp::rerun_if_changed_glob("modules/**/*.cppm");
    const std::string name_space = detail::own_metadata("namespace");
    const std::string lib_path = detail::own_value("lib", "path");
    if (name_space.empty() || lib_path.empty() || !std::filesystem::is_directory(root / "include")) return false;
    const std::filesystem::path file = root / lib_path;
    if (!std::filesystem::is_regular_file(file)) return false;
    mcpp::source(file.string().c_str());
    mcpp::rerun_if_changed(file.string().c_str());
    const std::string module =
        sources::package_module_name(detail::own_value("package", "namespace"), detail::own_value("package", "name"));

    // The package's other units below modules/, such as `<module>.macros`. Each is compiled; those named under the
    // module are re-exported, and the macros they name are mapped rather than missing.
    std::vector<std::string> units;
    detail::collect(root, "modules/**/*.cppm", units);
    std::vector<std::string> reexports;
    std::string mapped;
    for (const std::string& unit : units) {
        std::error_code same;
        if (std::filesystem::equivalent(root / unit, file, same)) continue;
        mcpp::source((root / unit).string().c_str());
        mcpp::rerun_if_changed((root / unit).string().c_str());
        const std::string text = detail::read_text(root / unit);
        const std::string name = sources::scan_module_interface(text).name;
        if (name.starts_with(module + ".")) {
            reexports.push_back(name);
            mapped += text;
        }
    }
    std::ranges::sort(reexports);

    std::vector<std::string> headers;
    detail::collect(root, "include/**/*.h", headers);
    detail::collect(root, "include/**/*.hpp", headers);
    std::vector<std::string> includes;
    std::vector<std::string> names;
    for (const std::string& header : headers) {
        mcpp::rerun_if_changed((root / header).string().c_str());
        const std::string text = detail::read_text(root / header);
        includes.push_back(header.substr(std::string_view("include/").size()));
        const std::vector<std::string> found = sources::public_names(text, name_space);
        names.insert(names.end(), found.begin(), found.end());
        for (const sources::defined_macro& macro : sources::defined_macros(text)) {
            if (sources::names_identifier(mapped, macro.name)) continue;
            const std::string where = header + ":" + std::to_string(macro.line);
            const std::string message = "the macro " + macro.name + " (" + where + ") does not reach `import " + module + ";`";
            // No line of this unit may start with the word `module`: dependency scanners read it as a declaration.
            const std::string macros_unit = module + ".macros";
            const std::string hint = "declare it as a constant or function in " + name_space + ", map it in a " +
                                     macros_unit + " unit such as modules/macros.cppm, or #undef it if it is the "
                                     "header's own; a translation unit that uses the macro includes " +
                                     includes.back() + " as well";
            mcpp::report({.severity = "warning", .message = message.c_str(), .hint = hint.c_str()});
        }
    }
    std::ranges::sort(names);
    names.erase(std::ranges::unique(names).begin(), names.end());

    if (detail::own_flag("cpp_modules_gen").value_or(true)) {
        const std::string content = sources::render_library_module(module, name_space, includes, names, reexports);
        if (detail::read_text(file) != content) {
            detail::write_if_changed(file, content);
            const std::string message = "generated " + lib_path + " from the public headers (" +
                                        std::to_string(names.size()) + " names)";
            mcpp::report({.severity = "note", .message = message.c_str()});
        }
    } else {
        const std::string interface_text = detail::read_text(file);
        const std::vector<std::string> declared = sources::reexported_modules(interface_text);
        for (const std::string& unit : reexports) {
            if (std::ranges::find(declared, unit) != declared.end()) continue;
            const std::string message = lib_path + " does not re-export " + unit;
            const std::string hint = "add `export import " + unit + ";` after `export import huxerui;`";
            mcpp::report({.severity = "warning", .message = message.c_str(), .hint = hint.c_str()});
        }
        const std::vector<std::string> missing = sources::unexported_names(interface_text, name_space, names);
        if (!missing.empty()) {
            std::string list;
            for (const std::string& name : missing) list += (list.empty() ? "" : ", ") + name;
            const std::string message = lib_path + " does not export " + list;
            const std::string impact = "`import " + module + ";` cannot name " + (missing.size() == 1 ? "it" : "them");
            const std::string hint = "add `using " + name_space + "::<name>;` for each, or remove "
                                     "`cpp_modules_gen = false` to have the file generated";
            mcpp::report({.severity = "warning", .message = message.c_str(), .impact = impact.c_str(),
                          .hint = hint.c_str()});
        }
    }
    return true;
}

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
    // A library's generated interface is a module unit of this package as well, so nothing is forced on it either.
    const bool generated_interface = plan_module_interface();
    const bool has_module_interface = generated_interface || std::ranges::any_of(
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
        root + "/buildtools/mcpp/huxerui-build-rules/include/huxerui_scope_prelude.h";
    const bool msvc =
        std::string_view(mcpp::compiler()).find("msvc") != std::string_view::npos;
    // The prelude's DIRECTORY is on the include path either way, so a module
    // unit can put `#include <huxerui_scope_prelude.h>` in its global module
    // fragment and hand-written HUXERUI_SCOPE works there too. Only the
    // FORCING is conditional.
    mcpp::include_dir(
        (root + "/buildtools/mcpp/huxerui-build-rules/include").c_str());

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

    const project_graph graph = load_graph();
    if (graph.is_root && !graph.programs.empty()) {
        write_library_graph(graph);
        write_framework_record();
    }

    auto rs = plan_resources(opt, graph, has_module_interface);
    if (!rs.empty()) {
        mcpp::include_dir(
            (std::string(mcpp::out_dir()) + "/hrc/builtin/include").c_str());
    }
    edges.insert(edges.end(), rs.begin(), rs.end());

    if (!submit(edges)) return false;
    bool packaged = true;
    if (graph.is_root)
        for (const std::string& program : graph.programs) packaged = plan_packages(opt, program, graph) && packaged;
    return packaged;
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
