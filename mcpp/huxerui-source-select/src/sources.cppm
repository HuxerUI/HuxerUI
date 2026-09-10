// Which project sources exist, and which of them need the composable transform.
//
// Pure functions of their arguments: no mcpp, no filesystem writes, nothing
// that needs a build program's environment. That is the point -- see the
// package manifest.

export module huxerui.rules.sources;

import std;

export namespace huxerui::rules::sources {

// Glob matching over `**`, `*` and `?`, anchored at both ends.
//
// SEGMENT-WISE, and it has to be. `*` must not cross a directory separator
// while `**` must, and a single-star backtracking matcher cannot express both:
// with `src/**/*.cpp` against `src/a/b/view.cpp` the inner `*` backtracks onto
// a `/`, fails, and has no way to hand the failure back to the outer `**`. The
// unit tests in this package caught exactly that.
//
// Splitting on `/` and letting `**` consume zero or more whole segments makes
// the two spellings mean different things, which is what a manifest that writes
// `src/*/*.cpp` beside `src/**/*.cpp` is relying on.
[[nodiscard]] inline std::vector<std::string_view> split(std::string_view value) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = value.find('/', start);
        if (slash == std::string_view::npos) {
            parts.push_back(value.substr(start));
            return parts;
        }
        parts.push_back(value.substr(start, slash - start));
        start = slash + 1;
    }
}

// One segment: `*` and `?` never see a separator here, so ordinary
// backtracking is enough.
[[nodiscard]] inline bool match_segment(std::string_view pattern, std::string_view text) {
    std::size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

[[nodiscard]] inline bool match_parts(std::span<const std::string_view> pattern,
                                      std::span<const std::string_view> text) {
    if (pattern.empty()) return text.empty();
    if (pattern.front() == "**") {
        // Zero or more whole segments, so one glob covers a flat project and a
        // nested one.
        for (std::size_t taken = 0; taken <= text.size(); ++taken) {
            if (match_parts(pattern.subspan(1), text.subspan(taken))) return true;
        }
        return false;
    }
    if (text.empty()) return false;
    if (!match_segment(pattern.front(), text.front())) return false;
    return match_parts(pattern.subspan(1), text.subspan(1));
}

[[nodiscard]] inline bool matches(std::string_view pattern, std::string_view text) {
    const std::vector<std::string_view> patternParts = split(pattern);
    const std::vector<std::string_view> textParts = split(text);
    return match_parts(patternParts, textParts);
}

// The same pre-filter cmake/HuxerUICodegen.cmake applies with file(READ) plus
// string(FIND): a source with neither marker gets no transform edge at all.
// Without it every source in a project would grow a needless graph node.
[[nodiscard]] inline bool needs_codegen(std::string_view source) {
    return source.find("[[huxerui::composable]]") != std::string_view::npos
        || source.find("Use") != std::string_view::npos;
}

// `sources` minus the target's entry. A build program can ADD a source but
// cannot replace one, so a transformed copy of the entry would link beside the
// original as `multiple definition of main`.
[[nodiscard]] inline std::vector<std::string>
without_entry(std::span<const std::string> sources, std::string_view entry) {
    std::vector<std::string> out;
    out.reserve(sources.size());
    for (const std::string& source : sources) {
        if (source != entry) out.push_back(source);
    }
    return out;
}

// A package name is not a C++ identifier: `mod-proj` is a perfectly good
// package name and hrc rejects it as a resource namespace
// ("resource namespace must be a non-reserved C++ identifier"). Hyphens and
// dots become underscores, and a leading digit gets a prefix.
[[nodiscard]] inline std::string identifier(std::string_view name) {
    std::string out;
    out.reserve(name.size() + 1);
    for (const char character : name) {
        const bool ok = (character >= 'a' && character <= 'z')
                     || (character >= 'A' && character <= 'Z')
                     || (character >= '0' && character <= '9')
                     || character == '_';
        out.push_back(ok ? character : '_');
    }
    if (out.empty()) return "resources";
    if (out.front() >= '0' && out.front() <= '9') out.insert(out.begin(), '_');
    return out;
}

// What a module interface unit declares, so a GENERATED copy of it can say the
// same thing.
//
// `mcpp::action` fixes the module graph during prepare, before the generator
// has run, so an action whose output is a module interface has to declare what
// that output will provide and import -- mcpp seeds a placeholder carrying
// exactly that declaration. Without it the importer compiles first and fails
// with `failed to read compiled module`, which names the module and nothing
// about the cause.
struct module_interface {
    std::string              name;      // empty when the unit declares none
    std::vector<std::string> imports;
};

[[nodiscard]] inline module_interface scan_module_interface(std::string_view source) {
    module_interface out;
    std::size_t line_start = 0;
    while (line_start <= source.size()) {
        const std::size_t line_end = source.find('\n', line_start);
        std::string_view line = source.substr(
            line_start, (line_end == std::string_view::npos ? source.size() : line_end) - line_start);
        line_start = (line_end == std::string_view::npos) ? source.size() + 1 : line_end + 1;

        // Trim leading space; a declaration is at column zero in practice but
        // nothing forbids indenting it.
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.starts_with("//")) continue;

        auto take_name = [](std::string_view rest) {
            while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
            std::size_t n = 0;
            while (n < rest.size() &&
                   (std::isalnum(static_cast<unsigned char>(rest[n])) || rest[n] == '_' || rest[n] == '.')) {
                ++n;
            }
            return std::string(rest.substr(0, n));
        };

        if (line.starts_with("export module ")) {
            out.name = take_name(line.substr(std::string_view("export module ").size()));
        } else if (line.starts_with("import ")) {
            const std::string name = take_name(line.substr(std::string_view("import ").size()));
            if (!name.empty()) out.imports.push_back(name);
        }
    }
    return out;
}

// The layout `xim:wix` installs, expressed once.
//
// Each of WiX's three NuGet payloads keeps its own directory shape under a
// subdirectory of the package root, so these are the upstream-documented paths
// rather than a repackaging. They are here rather than in the rule so they can
// be tested: a wrong path surfaces on Windows only, at link time, as a missing
// .lib -- and there is no Windows machine on the way to that discovery.
struct wix_layout {
    std::string tool;                   // wix.exe
    std::string bootstrapper_include;   // BootstrapperApplication.h and friends
    std::string bootstrapper_lib;       // balutil.lib
    std::string bootstrapper_runtime;   // mbanative.dll, deployed beside the BA
    std::string dutil_include;          // dutil.h and friends
    std::string dutil_lib;              // dutil.lib
};

[[nodiscard]] inline wix_layout wix_paths(std::string_view root) {
    const std::string r(root);
    return wix_layout{
        .tool                 = r + "/tool/tools/net6.0/any/wix.exe",
        .bootstrapper_include = r + "/bootstrapper/build/native/include",
        .bootstrapper_lib     = r + "/bootstrapper/build/native/v14/x64/balutil.lib",
        .bootstrapper_runtime = r + "/bootstrapper/runtimes/win-x64/native/mbanative.dll",
        .dutil_include        = r + "/dutil/build/native/include",
        .dutil_lib            = r + "/dutil/build/native/v14/x64/dutil.lib",
    };
}

// ------------------------------------------------------------ CMake reading --
//
// The body of a `set(VAR ...)` list, tokenised.
//
// `"-framework AppKit"` is ONE token. Splitting on whitespace made every macOS
// framework look unmatched and the parity check fail on a platform it was
// meant to protect.
[[nodiscard]] inline std::vector<std::string> cmake_list(std::string_view text,
                                                         std::string_view variable) {
    const std::string open = "set(" + std::string(variable);
    std::size_t at = text.find(open);
    if (at == std::string_view::npos) {
        // `set(\n        VAR` is also legal; fall back to the bare name.
        at = text.find(variable);
        if (at == std::string_view::npos) return {};
    }
    const std::size_t body = text.find_first_of("\n", at);
    if (body == std::string_view::npos) return {};
    // The list ends at the first `)` alone on a line, which is how this
    // repository writes them.
    std::size_t end = body;
    while (true) {
        const std::size_t nl = text.find('\n', end + 1);
        const std::string_view line =
            text.substr(end + 1, (nl == std::string_view::npos ? text.size() : nl) - end - 1);
        std::string_view trimmed = line;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
            trimmed.remove_prefix(1);
        if (trimmed.starts_with(")")) break;
        if (nl == std::string_view::npos) break;
        end = nl;
    }
    const std::string_view region = text.substr(body, end - body);

    std::vector<std::string> tokens;
    for (std::size_t i = 0; i < region.size(); ++i) {
        const char c = region[i];
        if (c == '#') {                       // a comment runs to end of line
            while (i < region.size() && region[i] != '\n') ++i;
            continue;
        }
        if (c == '"') {
            const std::size_t close = region.find('"', i + 1);
            if (close == std::string_view::npos) break;
            tokens.emplace_back(region.substr(i + 1, close - i - 1));
            i = close;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        std::size_t j = i;
        while (j < region.size() && !std::isspace(static_cast<unsigned char>(region[j])) &&
               region[j] != '#')
            ++j;
        tokens.emplace_back(region.substr(i, j - i));
        i = j - 1;
    }
    return tokens;
}

// ------------------------------------------------------- public header scan --
//
// What the module shell re-exports, read out of the public headers.
//
// Every rule below exists because of a defect it caused. A `\'}\'` in a char
// literal closed `namespace huxerui` early and view.h yielded 15 names instead
// of hundreds; a multi-line #define whose body carries an unbalanced `{` did
// the same from the other direction; an out-of-class member definition
// (`StringVariant StringVariant::Format`) sits at namespace scope but is not a
// namespace-scope NAME, and exporting it stopped the module compiling.
//
// Over-inclusion is caught by the compiler -- every entry becomes a `using`
// declaration -- so this errs toward finding too much rather than too little.

// Comments, string and character literals, and whole preprocessor directives
// (continuations included) replaced by whitespace, so nothing inside them is
// mistaken for a brace or a declaration. Newlines are preserved so line
// structure survives.
[[nodiscard]] inline std::string strip_noncode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    enum class In { Code, Block, Line, String, Char, Directive };
    In state = In::Code;
    bool escaped = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const char next = (i + 1 < text.size()) ? text[i + 1] : '\0';
        switch (state) {
        case In::Code:
            if (c == '/' && next == '*') { state = In::Block; out += "  "; ++i; continue; }
            if (c == '/' && next == '/') { state = In::Line;  out += "  "; ++i; continue; }
            if (c == '"')  { state = In::String; out += ' '; continue; }
            if (c == '\'') { state = In::Char;   out += ' '; continue; }
            // A directive owns the rest of its logical line, and its
            // continuations: `#define X \` + a body with a stray brace is a
            // real shape in view.h.
            if (c == '#') {
                bool only_space = true;
                for (std::size_t j = out.size(); j-- > 0;) {
                    if (out[j] == '\n') break;
                    if (out[j] != ' ' && out[j] != '\t') { only_space = false; break; }
                }
                if (only_space) { state = In::Directive; out += ' '; continue; }
            }
            out += c;
            continue;
        case In::Block:
            if (c == '*' && next == '/') { state = In::Code; out += "  "; ++i; continue; }
            out += (c == '\n') ? '\n' : ' ';
            continue;
        case In::Line:
            if (c == '\n') { state = In::Code; out += '\n'; continue; }
            out += ' ';
            continue;
        case In::String:
        case In::Char: {
            const char closer = (state == In::String) ? '"' : '\'';
            if (escaped)            { escaped = false; }
            else if (c == '\\')     { escaped = true; }
            else if (c == closer)   { state = In::Code; }
            out += (c == '\n') ? '\n' : ' ';
            continue;
        }
        case In::Directive:
            if (c == '\n') {
                // A trailing backslash continues the directive onto the next
                // line; anything else ends it.
                bool continued = false;
                for (std::size_t j = i; j-- > 0;) {
                    if (text[j] == '\\') { continued = true; break; }
                    if (text[j] != ' ' && text[j] != '\t' && text[j] != '\r') break;
                }
                if (!continued) state = In::Code;
                out += '\n';
                continue;
            }
            out += ' ';
            continue;
        }
    }
    return out;
}

namespace detail {

inline std::string_view trim(std::string_view v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r')) v.remove_prefix(1);
    while (!v.empty() && (v.back()  == ' ' || v.back()  == '\t' || v.back()  == '\r')) v.remove_suffix(1);
    return v;
}

inline bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// What follows `template <...>`, or "" when the line is only the header.
inline std::string_view after_template_header(std::string_view line) {
    if (!line.starts_with("template")) return line;
    const std::size_t open = line.find('<');
    if (open == std::string_view::npos) return {};
    int depth = 0;
    for (std::size_t i = open; i < line.size(); ++i) {
        if (line[i] == '<') ++depth;
        else if (line[i] == '>') {
            if (--depth == 0) return trim(line.substr(i + 1));
        }
    }
    return {};
}

} // namespace detail

// Namespace-scope names declared directly in `namespace huxerui`.
[[nodiscard]] inline std::vector<std::string> public_names(std::string_view header) {
    const std::string text = strip_noncode(header);
    std::vector<std::string> names;

    int depth = 0;
    // Parenthesis depth, tracked across lines for one reason: a parameter list
    // that spans lines leaves continuation fragments at namespace scope, and
    // taking the identifier before `=` out of one of them exported `location`
    // from `const std::source_location& location = ...`. A declaration starts
    // at paren depth zero; a continuation does not.
    int paren = 0;
    std::vector<std::pair<std::string, int>> namespaces;   // (name, depth inside)
    bool pending_template = false;

    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        std::string_view raw = std::string_view(text).substr(
            start, (end == std::string::npos ? text.size() : end) - start);
        start = (end == std::string::npos) ? text.size() + 1 : end + 1;

        std::string_view line = detail::trim(raw);
        if (line.empty()) continue;

        const auto braces = [&](std::string_view v) {
            int d = 0;
            for (char c : v) { if (c == '{') ++d; else if (c == '}') --d; }
            return d;
        };

        if (line.starts_with("namespace ")) {
            std::string_view rest = detail::trim(line.substr(std::string_view("namespace ").size()));
            std::size_t n = 0;
            while (n < rest.size() && (detail::is_ident_char(rest[n]) || rest[n] == ':')) ++n;
            namespaces.emplace_back(std::string(rest.substr(0, n)), depth + 1);
            depth += braces(line);
            continue;
        }

        // ns_stack records the depth INSIDE the namespace, so a declaration at
        // namespace scope sits at exactly that depth -- not one deeper.
        const bool in_huxerui = namespaces.size() == 1 && namespaces.front().first == "huxerui";
        const bool at_scope = in_huxerui && depth == namespaces.front().second && paren == 0;

        if (at_scope) {
            std::string_view decl = line;
            if (decl.starts_with("template")) {
                const std::string_view rest = detail::after_template_header(decl);
                if (rest.empty()) { pending_template = true; depth += braces(line); continue; }
                decl = rest;
            }
            const bool eligible = at_scope || pending_template;
            pending_template = false;
            if (eligible) {
                std::string name;
                std::size_t name_at = std::string_view::npos;
                auto after_keyword = [&](std::string_view kw) -> std::string_view {
                    if (!decl.starts_with(kw)) return {};
                    std::string_view rest = detail::trim(decl.substr(kw.size()));
                    // An attribute may sit between the keyword and the name:
                    // `class [[nodiscard]] Result final {`. Skipping it is what
                    // makes Result reachable through `import huxerui;`.
                    while (rest.starts_with("[[")) {
                        const std::size_t close = rest.find("]]");
                        if (close == std::string_view::npos) return {};
                        rest = detail::trim(rest.substr(close + 2));
                    }
                    return rest;
                };
                for (std::string_view kw : { "class ", "struct ", "enum class ", "enum struct ", "enum ", "using " }) {
                    const std::string_view rest = after_keyword(kw);
                    if (rest.empty()) continue;
                    std::size_t n = 0;
                    while (n < rest.size() && detail::is_ident_char(rest[n])) ++n;
                    if (n == 0) continue;
                    const std::string_view candidate = rest.substr(0, n);
                    const std::string_view tail = detail::trim(rest.substr(n));
                    const bool declares =
                        kw == "using " ? tail.starts_with("=")
                                       : (tail.starts_with("final") || tail.starts_with(":") ||
                                          tail.starts_with("{") || tail.starts_with(";"));
                    if (declares) {
                        name = std::string(candidate);
                        name_at = static_cast<std::size_t>(candidate.data() - decl.data());
                    }
                    break;
                }
                if (name.empty()) {
                    // A function or a constant: the identifier before `(` or
                    // `=`/`{`. Continuation fragments never reach here -- the
                    // paren-depth guard above kept them out -- so a signature
                    // may span as many lines as it likes.
                    const std::size_t stop = decl.find_first_of("(={");
                    if (stop != std::string_view::npos && stop > 0) {
                        std::size_t e = stop;
                        while (e > 0 && (decl[e - 1] == ' ' || decl[e - 1] == '\t')) --e;
                        std::size_t b = e;
                        while (b > 0 && detail::is_ident_char(decl[b - 1])) --b;
                        if (b < e && !std::isdigit(static_cast<unsigned char>(decl[b]))) {
                            name = std::string(decl.substr(b, e - b));
                            name_at = b;
                        }
                    }
                }
                // An out-of-class member definition sits at namespace scope but
                // is not a namespace-scope name.
                const bool qualified =
                    name_at != std::string_view::npos && name_at >= 2 &&
                    decl[name_at - 1] == ':' && decl[name_at - 2] == ':';
                static constexpr std::string_view kKeywords[] = {
                    "if", "for", "while", "switch", "return", "sizeof", "else", "catch",
                    "operator", "explicit", "friend", "typedef", "namespace", "template",
                    "public", "private", "protected", "virtual", "static", "inline",
                    "constexpr", "consteval", "constinit", "decltype", "requires",
                    "concept", "alignas", "alignof", "throw", "try", "new", "delete",
                    "this", "true", "false", "nullptr", "auto", "void", "struct",
                    "class", "enum", "union", "using", "extern", "export", "import",
                    "module", "do", "case", "default", "break", "continue", "noexcept",
                    // `class Foo\n    final : public Bar {` puts `final` at the
                    // start of a line at namespace scope.
                    "final", "override",
                };
                const bool keyword =
                    std::ranges::find(kKeywords, std::string_view(name)) != std::end(kKeywords);
                if (!name.empty() && !qualified && !keyword && name.front() != '_') {
                    names.push_back(name);
                }
            }
        }

        for (char c : line) { if (c == '(') ++paren; else if (c == ')') --paren; }
        if (paren < 0) paren = 0;

        const int before = depth;
        depth += braces(line);
        if (depth < before) {
            while (!namespaces.empty() && depth < namespaces.back().second) namespaces.pop_back();
        }
    }

    std::ranges::sort(names);
    names.erase(std::ranges::unique(names).begin(), names.end());
    return names;
}

// A macro definition, verbatim, continuations included.
//
// The scope macros are lifted out of view.h rather than written twice: a
// translation unit that both imports the module and includes view.h would hit
// an illegal redefinition if the two copies differed by a token.
[[nodiscard]] inline std::string macro_definition(std::string_view header, std::string_view name) {
    const std::string needle = "#define " + std::string(name);
    std::size_t at = 0;
    while ((at = header.find(needle, at)) != std::string_view::npos) {
        // The name must end here, or `HUXERUI_SCOPE` matches `HUXERUI_SCOPE_END`.
        const std::size_t after = at + needle.size();
        if (after < header.size() && (std::isalnum(static_cast<unsigned char>(header[after])) ||
                                      header[after] == '_')) {
            at = after;
            continue;
        }
        std::size_t end = at;
        while (true) {
            const std::size_t nl = header.find('\n', end);
            if (nl == std::string_view::npos) { end = header.size(); break; }
            std::size_t back = nl;
            while (back > at && (header[back - 1] == ' ' || header[back - 1] == '\t' ||
                                 header[back - 1] == '\r')) --back;
            if (back > at && header[back - 1] == '\\') { end = nl + 1; continue; }
            end = nl;
            break;
        }
        return std::string(header.substr(at, end - at));
    }
    return {};
}

// The `#include <huxerui/...>` list of the umbrella header, in order.
[[nodiscard]] inline std::vector<std::string> umbrella_includes(std::string_view umbrella) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while ((at = umbrella.find("<huxerui/", at)) != std::string_view::npos) {
        const std::size_t close = umbrella.find('>', at);
        if (close == std::string_view::npos) break;
        out.emplace_back(umbrella.substr(at + 9, close - at - 9));
        at = close;
    }
    return out;
}

} // namespace huxerui::rules::sources
