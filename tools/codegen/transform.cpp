#include "transform.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <utility>
#include <vector>

namespace huxerui::codegen {

namespace {

constexpr std::string_view kScopeMarker =
    "[[huxerui::scope]]";
constexpr std::string_view kScopeBegin =
    "HUXERUI_SCOPE_BEGIN";

struct Edit {
  std::size_t offset;
  std::size_t length;
  std::string replacement;
};

struct ScopeBody {
  std::size_t marker_offset;
  std::size_t opening_brace;
  std::size_t closing_brace;
};

[[nodiscard]] bool StartsWith(
    std::string_view source,
    std::size_t offset,
    std::string_view value) noexcept {
  return offset <= source.size() &&
         source.substr(offset, value.size()) == value;
}

[[nodiscard]] std::size_t SkipLineComment(
    std::string_view source,
    std::size_t offset) noexcept {
  const std::size_t newline = source.find('\n', offset + 2);
  return newline == std::string_view::npos
             ? source.size()
             : newline;
}

[[nodiscard]] std::size_t SkipBlockComment(
    std::string_view source,
    std::size_t offset) {
  const std::size_t end = source.find("*/", offset + 2);
  if (end == std::string_view::npos) {
    throw TransformError(offset, "unterminated block comment");
  }
  return end + 2;
}

[[nodiscard]] std::size_t SkipQuotedLiteral(
    std::string_view source,
    std::size_t offset,
    char quote) {
  std::size_t cursor = offset + 1;
  while (cursor < source.size()) {
    if (source[cursor] == '\\') {
      cursor = std::min(source.size(), cursor + 2);
      continue;
    }
    if (source[cursor] == quote) {
      return cursor + 1;
    }
    ++cursor;
  }
  throw TransformError(
      offset,
      quote == '"'
          ? "unterminated string literal"
          : "unterminated character literal");
}

[[nodiscard]] std::optional<std::size_t> SkipRawString(
    std::string_view source,
    std::size_t offset) {
  if (!StartsWith(source, offset, "R\"")) {
    return std::nullopt;
  }

  const std::size_t delimiter_begin = offset + 2;
  const std::size_t open_parenthesis =
      source.find('(', delimiter_begin);
  if (open_parenthesis == std::string_view::npos ||
      open_parenthesis - delimiter_begin > 16) {
    return std::nullopt;
  }

  const std::string_view delimiter = source.substr(
      delimiter_begin,
      open_parenthesis - delimiter_begin);
  for (char character : delimiter) {
    const unsigned char value =
        static_cast<unsigned char>(character);
    if (std::isspace(value) ||
        character == '\\' ||
        character == ')' ||
        character == '(') {
      return std::nullopt;
    }
  }

  std::string closing;
  closing.reserve(delimiter.size() + 2);
  closing.push_back(')');
  closing.append(delimiter);
  closing.push_back('"');

  const std::size_t end =
      source.find(closing, open_parenthesis + 1);
  if (end == std::string_view::npos) {
    throw TransformError(offset, "unterminated raw string literal");
  }
  return end + closing.size();
}

[[nodiscard]] std::optional<std::size_t> SkipNonCode(
    std::string_view source,
    std::size_t offset) {
  if (StartsWith(source, offset, "//")) {
    return SkipLineComment(source, offset);
  }
  if (StartsWith(source, offset, "/*")) {
    return SkipBlockComment(source, offset);
  }
  if (const auto raw_end = SkipRawString(source, offset)) {
    return raw_end;
  }
  if (source[offset] == '"') {
    return SkipQuotedLiteral(source, offset, '"');
  }
  if (source[offset] == '\'') {
    return SkipQuotedLiteral(source, offset, '\'');
  }
  return std::nullopt;
}

[[nodiscard]] bool IsIdentifierCharacter(char character) noexcept {
  const unsigned char value =
      static_cast<unsigned char>(character);
  return std::isalnum(value) || character == '_';
}

[[nodiscard]] bool IsConditionalDirective(
    std::string_view source,
    std::size_t offset) noexcept {
  if (source[offset] != '#') {
    return false;
  }

  std::size_t line_begin = offset;
  while (line_begin > 0 && source[line_begin - 1] != '\n') {
    --line_begin;
  }
  for (std::size_t cursor = line_begin; cursor < offset; ++cursor) {
    if (source[cursor] != ' ' && source[cursor] != '\t') {
      return false;
    }
  }

  std::size_t cursor = offset + 1;
  while (cursor < source.size() &&
         (source[cursor] == ' ' || source[cursor] == '\t')) {
    ++cursor;
  }
  const std::size_t name_begin = cursor;
  while (cursor < source.size() &&
         IsIdentifierCharacter(source[cursor])) {
    ++cursor;
  }
  const std::string_view name =
      source.substr(name_begin, cursor - name_begin);
  return name == "if" ||
         name == "ifdef" ||
         name == "ifndef" ||
         name == "elif" ||
         name == "else" ||
         name == "endif";
}

[[nodiscard]] std::size_t FindOpeningBrace(
    std::string_view source,
    std::size_t marker_offset) {
  std::size_t cursor = marker_offset + kScopeMarker.size();
  std::size_t parentheses = 0;
  std::size_t brackets = 0;
  bool saw_parameter_list = false;

  while (cursor < source.size()) {
    if (const auto end = SkipNonCode(source, cursor)) {
      cursor = *end;
      continue;
    }

    const char character = source[cursor];
    if (character == '(') {
      ++parentheses;
      saw_parameter_list = true;
    } else if (character == ')') {
      if (parentheses == 0) {
        throw TransformError(
            marker_offset,
            "unmatched ')' after scope marker");
      }
      --parentheses;
    } else if (character == '[') {
      ++brackets;
    } else if (character == ']') {
      if (brackets > 0) {
        --brackets;
      }
    } else if (character == ';' &&
               parentheses == 0 &&
               brackets == 0) {
      throw TransformError(
          marker_offset,
          "scope marker must precede a function definition");
    } else if (character == '{' &&
               parentheses == 0 &&
               brackets == 0) {
      if (!saw_parameter_list) {
        throw TransformError(
            marker_offset,
            "scope marker must precede a function definition");
      }
      return cursor;
    }
    ++cursor;
  }

  throw TransformError(
      marker_offset,
      "scope marker has no function body");
}

[[nodiscard]] std::size_t FindClosingBrace(
    std::string_view source,
    std::size_t marker_offset,
    std::size_t opening_brace) {
  std::size_t cursor = opening_brace + 1;
  std::size_t depth = 1;

  while (cursor < source.size()) {
    if (const auto end = SkipNonCode(source, cursor)) {
      cursor = *end;
      continue;
    }

    if (StartsWith(source, cursor, kScopeMarker)) {
      throw TransformError(
          cursor,
          "nested scope markers are not supported");
    }
    if (StartsWith(source, cursor, kScopeBegin)) {
      throw TransformError(
          cursor,
          "scope-marked function already contains an explicit HuxerUI scope");
    }
    if (IsConditionalDirective(source, cursor)) {
      throw TransformError(
          cursor,
          "conditional compilation inside a scope-marked function is not supported");
    }

    if (source[cursor] == '{') {
      ++depth;
    } else if (source[cursor] == '}') {
      --depth;
      if (depth == 0) {
        return cursor;
      }
    }
    ++cursor;
  }

  throw TransformError(
      marker_offset,
      "unable to match the scope-marked function body");
}

[[nodiscard]] std::string EscapeLinePath(
    std::string_view source_path) {
  std::string escaped;
  escaped.reserve(source_path.size());
  for (char character : source_path) {
    if (character == '\\' || character == '"') {
      escaped.push_back('\\');
    }
    escaped.push_back(character);
  }
  return escaped;
}

[[nodiscard]] std::string LineDirective(
    std::size_t line,
    std::string_view escaped_path) {
  return "#line " + std::to_string(line) +
         " \"" + std::string(escaped_path) + "\"\n";
}

[[nodiscard]] std::string BlankMarker() {
  return std::string(kScopeMarker.size(), ' ');
}

}  // namespace

TransformError::TransformError(
    std::size_t offset,
    std::string message)
    : std::runtime_error(std::move(message)),
      offset_(offset) {}

SourcePosition PositionAt(
    std::string_view source,
    std::size_t offset) noexcept {
  SourcePosition position;
  const std::size_t limit = std::min(offset, source.size());
  for (std::size_t cursor = 0; cursor < limit; ++cursor) {
    if (source[cursor] == '\n') {
      ++position.line;
      position.column = 1;
    } else {
      ++position.column;
    }
  }
  return position;
}

TransformResult TransformSource(
    std::string_view source,
    std::string_view source_path) {
  std::vector<ScopeBody> scopes;
  std::size_t cursor = 0;

  while (cursor < source.size()) {
    if (const auto end = SkipNonCode(source, cursor)) {
      cursor = *end;
      continue;
    }
    if (!StartsWith(source, cursor, kScopeMarker)) {
      ++cursor;
      continue;
    }

    const std::size_t opening_brace =
        FindOpeningBrace(source, cursor);
    const std::size_t closing_brace =
        FindClosingBrace(source, cursor, opening_brace);
    scopes.push_back(ScopeBody{
        cursor,
        opening_brace,
        closing_brace,
    });
    cursor = closing_brace + 1;
  }

  if (scopes.empty()) {
    return TransformResult{
        std::string(source),
        0,
    };
  }

  const std::string escaped_path =
      EscapeLinePath(source_path);
  std::vector<Edit> edits;
  edits.reserve(scopes.size() * 3);

  for (const ScopeBody& scope : scopes) {
    const SourcePosition opening =
        PositionAt(source, scope.opening_brace);
    const SourcePosition closing =
        PositionAt(source, scope.closing_brace);

    edits.push_back(Edit{
        scope.marker_offset,
        kScopeMarker.size(),
        BlankMarker(),
    });
    edits.push_back(Edit{
        scope.opening_brace + 1,
        0,
        "\n  HUXERUI_SCOPE_BEGIN\n" +
            LineDirective(opening.line, escaped_path),
    });
    edits.push_back(Edit{
        scope.closing_brace,
        0,
        "\n  HUXERUI_SCOPE_END\n" +
            LineDirective(closing.line, escaped_path),
    });
  }

  std::sort(
      edits.begin(),
      edits.end(),
      [](const Edit& left, const Edit& right) {
        return left.offset > right.offset;
      });

  std::string transformed(source);
  for (const Edit& edit : edits) {
    transformed.replace(
        edit.offset,
        edit.length,
        edit.replacement);
  }
  transformed.insert(
      0,
      LineDirective(1, escaped_path));

  return TransformResult{
      std::move(transformed),
      scopes.size(),
  };
}

}  // namespace huxerui::codegen
