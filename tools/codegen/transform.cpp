#include "transform.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace huxerui::codegen {

namespace {

constexpr std::string_view kComposableMarker = "[[huxerui::composable]]";
constexpr std::string_view kScope = "HUXERUI_SCOPE";
constexpr std::string_view kScopeBegin = "HUXERUI_SCOPE_BEGIN";
constexpr std::string_view kScopeEnd = "HUXERUI_SCOPE_END";

struct Edit {
  std::size_t offset;
  std::size_t length;
  std::string replacement;
};

struct ComposableBody {
  std::size_t marker_offset;
  std::size_t opening_brace;
  std::size_t closing_brace;
};

struct SourceRange {
  std::size_t opening_brace;
  std::size_t closing_brace;
};

struct FunctionBody {
  std::size_t name_offset;
  SourceRange range;
};

[[nodiscard]] bool StartsWith(std::string_view source, std::size_t offset, std::string_view value) noexcept {
  return offset <= source.size() && source.substr(offset, value.size()) == value;
}

[[nodiscard]] std::size_t SkipLineComment(std::string_view source, std::size_t offset) noexcept {
  const std::size_t newline = source.find('\n', offset + 2);
  return newline == std::string_view::npos ? source.size() : newline;
}

[[nodiscard]] std::size_t SkipBlockComment(std::string_view source, std::size_t offset) {
  const std::size_t end = source.find("*/", offset + 2);
  if (end == std::string_view::npos) {
    throw TransformError(offset, "unterminated block comment");
  }
  return end + 2;
}

[[nodiscard]] std::size_t SkipQuotedLiteral(std::string_view source, std::size_t offset, char quote) {
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
  throw TransformError(offset, quote == '"' ? "unterminated string literal" : "unterminated character literal");
}

[[nodiscard]] std::optional<std::size_t> SkipRawString(std::string_view source, std::size_t offset) {
  if (!StartsWith(source, offset, "R\"")) {
    return std::nullopt;
  }

  const std::size_t delimiter_begin = offset + 2;
  const std::size_t open_parenthesis = source.find('(', delimiter_begin);
  if (open_parenthesis == std::string_view::npos || open_parenthesis - delimiter_begin > 16) {
    return std::nullopt;
  }

  const std::string_view delimiter = source.substr(delimiter_begin, open_parenthesis - delimiter_begin);
  for (char character : delimiter) {
    const unsigned char value = static_cast<unsigned char>(character);
    if (std::isspace(value) || character == '\\' || character == ')' || character == '(') {
      return std::nullopt;
    }
  }

  std::string closing;
  closing.reserve(delimiter.size() + 2);
  closing.push_back(')');
  closing.append(delimiter);
  closing.push_back('"');

  const std::size_t end = source.find(closing, open_parenthesis + 1);
  if (end == std::string_view::npos) {
    throw TransformError(offset, "unterminated raw string literal");
  }
  return end + closing.size();
}

[[nodiscard]] std::optional<std::size_t> SkipNonCode(std::string_view source, std::size_t offset) {
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
    const auto is_digit_separator_neighbor = [](char character) {
      return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
    };
    if (offset > 0 && offset + 1 < source.size() && is_digit_separator_neighbor(source[offset - 1]) &&
        is_digit_separator_neighbor(source[offset + 1])) {
      return std::nullopt;
    }
    return SkipQuotedLiteral(source, offset, '\'');
  }
  return std::nullopt;
}

[[nodiscard]] bool IsIdentifierCharacter(char character) noexcept {
  const unsigned char value = static_cast<unsigned char>(character);
  return std::isalnum(value) || character == '_';
}

[[nodiscard]] bool IsIdentifierStart(char character) noexcept {
  const unsigned char value = static_cast<unsigned char>(character);
  return std::isalpha(value) || character == '_';
}

[[nodiscard]] bool IsPreprocessorDirective(std::string_view source, std::size_t offset) noexcept {
  if (source[offset] != '#') {
    return false;
  }
  while (offset > 0 && source[offset - 1] != '\n') {
    --offset;
  }
  while (offset < source.size() && (source[offset] == ' ' || source[offset] == '\t')) {
    ++offset;
  }
  return offset < source.size() && source[offset] == '#';
}

[[nodiscard]] std::size_t SkipPreprocessorDirective(std::string_view source, std::size_t offset) noexcept {
  while (offset < source.size()) {
    const std::size_t line_end = source.find('\n', offset);
    if (line_end == std::string_view::npos) {
      return source.size();
    }
    std::size_t last = line_end;
    while (last > offset && (source[last - 1] == ' ' || source[last - 1] == '\t' || source[last - 1] == '\r')) {
      --last;
    }
    if (last == offset || source[last - 1] != '\\') {
      return line_end;
    }
    offset = line_end + 1;
  }
  return source.size();
}

[[nodiscard]] std::size_t SkipTrivia(std::string_view source, std::size_t offset) {
  while (offset < source.size()) {
    if (std::isspace(static_cast<unsigned char>(source[offset]))) {
      ++offset;
      continue;
    }
    if (StartsWith(source, offset, "//")) {
      offset = SkipLineComment(source, offset);
      continue;
    }
    if (StartsWith(source, offset, "/*")) {
      offset = SkipBlockComment(source, offset);
      continue;
    }
    break;
  }
  return offset;
}

[[nodiscard]] std::size_t FindMatchingDelimiter(
    std::string_view source, std::size_t opening, char open, char close, std::string_view diagnostic
) {
  std::size_t cursor = opening + 1;
  std::size_t depth = 1;
  while (cursor < source.size()) {
    if (const auto end = SkipNonCode(source, cursor)) {
      cursor = *end;
      continue;
    }
    if (source[cursor] == open) {
      ++depth;
    } else if (source[cursor] == close) {
      --depth;
      if (depth == 0) {
        return cursor;
      }
    }
    ++cursor;
  }
  throw TransformError(opening, std::string(diagnostic));
}

[[nodiscard]] std::size_t FindClosingBrace(std::string_view source, std::size_t opening_brace) {
  return FindMatchingDelimiter(source, opening_brace, '{', '}', "unable to match function body");
}

[[nodiscard]] std::optional<SourceRange>
FindFunctionBody(std::string_view source, std::size_t parameter_end, std::string_view diagnostic) {
  std::size_t cursor = parameter_end + 1;
  std::size_t parentheses = 0;
  std::size_t brackets = 0;
  while (cursor < source.size()) {
    if (const auto end = SkipNonCode(source, cursor)) {
      cursor = *end;
      continue;
    }
    const char character = source[cursor];
    if (character == '(') {
      ++parentheses;
    } else if (character == ')') {
      if (parentheses == 0) {
        throw TransformError(parameter_end, std::string(diagnostic));
      }
      --parentheses;
    } else if (character == '[') {
      ++brackets;
    } else if (character == ']') {
      if (brackets > 0) {
        --brackets;
      }
    } else if (character == ';' && parentheses == 0 && brackets == 0) {
      return std::nullopt;
    } else if (character == '{' && parentheses == 0 && brackets == 0) {
      return SourceRange{cursor, FindClosingBrace(source, cursor)};
    }
    ++cursor;
  }
  throw TransformError(parameter_end, std::string(diagnostic));
}

[[nodiscard]] bool CanBeginFunctionSuffix(std::string_view source, std::size_t parameter_end) {
  const std::size_t suffix = SkipTrivia(source, parameter_end + 1);
  if (suffix >= source.size()) {
    return false;
  }
  if (source[suffix] == '{' || source[suffix] == '&' || source[suffix] == '[' || StartsWith(source, suffix, "->")) {
    return true;
  }
  constexpr std::string_view qualifiers[] = {"const", "final", "noexcept", "override", "requires", "volatile"};
  return std::ranges::any_of(qualifiers, [source, suffix](std::string_view qualifier) {
    const std::size_t end = suffix + qualifier.size();
    return StartsWith(source, suffix, qualifier) && (end >= source.size() || !IsIdentifierCharacter(source[end]));
  });
}

[[nodiscard]] bool Contains(const SourceRange& range, std::size_t offset) noexcept {
  return offset > range.opening_brace && offset < range.closing_brace;
}

[[nodiscard]] bool Contains(const ComposableBody& body, std::size_t offset) noexcept {
  return offset > body.opening_brace && offset < body.closing_brace;
}

[[nodiscard]] bool IsConditionalDirective(std::string_view source, std::size_t offset) noexcept {
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
  while (cursor < source.size() && (source[cursor] == ' ' || source[cursor] == '\t')) {
    ++cursor;
  }
  const std::size_t name_begin = cursor;
  while (cursor < source.size() && IsIdentifierCharacter(source[cursor])) {
    ++cursor;
  }
  const std::string_view name = source.substr(name_begin, cursor - name_begin);
  return name == "if" || name == "ifdef" || name == "ifndef" || name == "elif" || name == "else" || name == "endif";
}

[[nodiscard]] std::size_t FindOpeningBrace(std::string_view source, std::size_t marker_offset) {
  std::size_t cursor = marker_offset + kComposableMarker.size();
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
        throw TransformError(marker_offset, "unmatched ')' after composable marker");
      }
      --parentheses;
    } else if (character == '[') {
      ++brackets;
    } else if (character == ']') {
      if (brackets > 0) {
        --brackets;
      }
    } else if (character == ';' && parentheses == 0 && brackets == 0) {
      throw TransformError(marker_offset, "composable marker must precede a function definition");
    } else if (character == '{' && parentheses == 0 && brackets == 0) {
      if (!saw_parameter_list) {
        throw TransformError(marker_offset, "composable marker must precede a function definition");
      }
      return cursor;
    }
    ++cursor;
  }

  throw TransformError(marker_offset, "composable marker has no function body");
}

[[nodiscard]] std::size_t
FindClosingBrace(std::string_view source, std::size_t marker_offset, std::size_t opening_brace) {
  std::size_t cursor = opening_brace + 1;
  std::size_t depth = 1;

  while (cursor < source.size()) {
    if (const auto end = SkipNonCode(source, cursor)) {
      cursor = *end;
      continue;
    }

    if (StartsWith(source, cursor, kComposableMarker)) {
      throw TransformError(cursor, "nested composable markers are not supported");
    }
    if (StartsWith(source, cursor, kScopeBegin)) {
      throw TransformError(cursor, "composable function already contains an explicit HuxerUI scope");
    }
    if (IsConditionalDirective(source, cursor)) {
      throw TransformError(cursor, "conditional compilation inside a composable function is not supported");
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

  throw TransformError(marker_offset, "unable to match the composable function body");
}

struct Identifier {
  std::string_view text;
  std::size_t begin;
  std::size_t end;
};

[[nodiscard]] std::optional<Identifier> ReadIdentifier(std::string_view source, std::size_t offset) {
  offset = SkipTrivia(source, offset);
  if (offset >= source.size() || !IsIdentifierStart(source[offset])) {
    return std::nullopt;
  }
  std::size_t end = offset + 1;
  while (end < source.size() && IsIdentifierCharacter(source[end])) {
    ++end;
  }
  return Identifier{source.substr(offset, end - offset), offset, end};
}

[[nodiscard]] std::unordered_set<std::string> FindApplicationRootNames(std::string_view source) {
  std::unordered_set<std::string> roots;
  std::size_t cursor = 0;
  while (cursor < source.size()) {
    if (const auto end = SkipNonCode(source, cursor)) {
      cursor = *end;
      continue;
    }
    if (IsPreprocessorDirective(source, cursor)) {
      cursor = SkipPreprocessorDirective(source, cursor);
      continue;
    }
    if (!IsIdentifierStart(source[cursor])) {
      ++cursor;
      continue;
    }

    const auto type = ReadIdentifier(source, cursor);
    cursor = type->end;
    if (type->text != "Application") {
      continue;
    }
    const auto variable = ReadIdentifier(source, cursor);
    if (!variable.has_value()) {
      continue;
    }
    std::size_t initializer = SkipTrivia(source, variable->end);
    if (initializer < source.size() && source[initializer] == '=') {
      initializer = SkipTrivia(source, initializer + 1);
    }
    if (initializer >= source.size() || (source[initializer] != '{' && source[initializer] != '(')) {
      continue;
    }

    std::size_t root_cursor = SkipTrivia(source, initializer + 1);
    if (root_cursor < source.size() && source[root_cursor] == '&') {
      root_cursor = SkipTrivia(source, root_cursor + 1);
    }
    auto root = ReadIdentifier(source, root_cursor);
    if (!root.has_value()) {
      continue;
    }
    while (true) {
      const std::size_t qualifier = SkipTrivia(source, root->end);
      if (!StartsWith(source, qualifier, "::")) {
        break;
      }
      auto qualified = ReadIdentifier(source, qualifier + 2);
      if (!qualified.has_value()) {
        break;
      }
      root = qualified;
    }
    const std::size_t argument_end = SkipTrivia(source, root->end);
    if (argument_end < source.size() &&
        (source[argument_end] == ',' || source[argument_end] == '}' || source[argument_end] == ')')) {
      roots.emplace(root->text);
    }
  }
  return roots;
}

[[nodiscard]] std::vector<SourceRange> FindApplicationRootBodies(std::string_view source) {
  const auto root_names = FindApplicationRootNames(source);
  std::vector<SourceRange> bodies;
  if (root_names.empty()) {
    return bodies;
  }

  std::size_t cursor = 0;
  while (cursor < source.size()) {
    if (const auto end = SkipNonCode(source, cursor)) {
      cursor = *end;
      continue;
    }
    if (!IsIdentifierStart(source[cursor])) {
      ++cursor;
      continue;
    }
    const auto identifier = ReadIdentifier(source, cursor);
    cursor = identifier->end;
    if (!root_names.contains(std::string(identifier->text))) {
      continue;
    }
    const std::size_t parameters = SkipTrivia(source, identifier->end);
    if (parameters >= source.size() || source[parameters] != '(') {
      continue;
    }
    const std::size_t parameter_end =
        FindMatchingDelimiter(source, parameters, '(', ')', "unable to match application root parameter list");
    if (!CanBeginFunctionSuffix(source, parameter_end)) {
      continue;
    }
    const auto body = FindFunctionBody(source, parameter_end, "unable to locate application root body");
    if (!body.has_value()) {
      continue;
    }
    bodies.push_back(*body);
    cursor = bodies.back().closing_brace + 1;
  }
  return bodies;
}

[[nodiscard]] bool IsMemberCall(std::string_view source, std::size_t identifier_offset) noexcept {
  std::size_t cursor = identifier_offset;
  while (cursor > 0 && std::isspace(static_cast<unsigned char>(source[cursor - 1]))) {
    --cursor;
  }
  if (cursor > 0 && source[cursor - 1] == '.') {
    return true;
  }
  return cursor > 1 && source[cursor - 1] == '>' && source[cursor - 2] == '-';
}

[[nodiscard]] std::vector<SourceRange> FindExplicitScopeBodies(std::string_view source) {
  std::vector<SourceRange> bodies;
  std::size_t cursor = 0;
  while (cursor < source.size()) {
    if (const auto end = SkipNonCode(source, cursor)) {
      cursor = *end;
      continue;
    }
    if (!IsIdentifierStart(source[cursor])) {
      ++cursor;
      continue;
    }
    const auto identifier = ReadIdentifier(source, cursor);
    cursor = identifier->end;
    if (identifier->text != "Scope" || IsMemberCall(source, identifier->begin)) {
      continue;
    }
    const std::size_t arguments = SkipTrivia(source, identifier->end);
    if (arguments >= source.size() || source[arguments] != '(') {
      continue;
    }
    const std::size_t capture = SkipTrivia(source, arguments + 1);
    if (capture >= source.size() || source[capture] != '[') {
      continue;
    }
    const std::size_t capture_end =
        FindMatchingDelimiter(source, capture, '[', ']', "unable to match explicit Scope capture list");
    std::size_t lambda_declarator_end = capture_end;
    const std::size_t parameters = SkipTrivia(source, capture_end + 1);
    if (parameters < source.size() && source[parameters] == '(') {
      lambda_declarator_end =
          FindMatchingDelimiter(source, parameters, '(', ')', "unable to match explicit Scope parameter list");
    }
    const auto body = FindFunctionBody(source, lambda_declarator_end, "unable to locate explicit Scope body");
    if (!body.has_value()) {
      continue;
    }
    bodies.push_back(*body);
    cursor = body->closing_brace + 1;
  }
  return bodies;
}

[[nodiscard]] std::vector<SourceRange> FindExplicitScopeMacroBodies(std::string_view source) {
  std::vector<SourceRange> bodies;
  std::size_t cursor = 0;
  while (cursor < source.size()) {
    if (const auto end = SkipNonCode(source, cursor)) {
      cursor = *end;
      continue;
    }
    if (StartsWith(source, cursor, kScope) && !StartsWith(source, cursor, kScopeBegin)) {
      const std::size_t arguments = SkipTrivia(source, cursor + kScope.size());
      if (arguments < source.size() && source[arguments] == '(') {
        const std::size_t closing =
            FindMatchingDelimiter(source, arguments, '(', ')', "unable to match explicit HuxerUI scope");
        bodies.push_back(SourceRange{cursor, closing});
        cursor = closing + 1;
        continue;
      }
    }
    if (!StartsWith(source, cursor, kScopeBegin)) {
      ++cursor;
      continue;
    }
    const std::size_t opening = cursor;
    cursor += kScopeBegin.size();
    while (cursor < source.size()) {
      if (const auto end = SkipNonCode(source, cursor)) {
        cursor = *end;
        continue;
      }
      if (StartsWith(source, cursor, kScopeEnd)) {
        bodies.push_back(SourceRange{opening, cursor});
        cursor += kScopeEnd.size();
        break;
      }
      ++cursor;
    }
    if (bodies.empty() || bodies.back().opening_brace != opening) {
      throw TransformError(opening, "explicit HuxerUI scope has no matching end");
    }
  }
  return bodies;
}

[[nodiscard]] bool IsUseCallName(std::string_view identifier) noexcept {
  return identifier.size() > 3 && identifier.starts_with("Use") &&
         std::isupper(static_cast<unsigned char>(identifier[3]));
}

[[nodiscard]] bool CanBeFunctionName(std::string_view source, std::size_t identifier_offset) noexcept {
  std::size_t cursor = identifier_offset;
  while (cursor > 0 && std::isspace(static_cast<unsigned char>(source[cursor - 1]))) {
    --cursor;
  }
  while (cursor > 1 && source[cursor - 1] == ':' && source[cursor - 2] == ':') {
    cursor -= 2;
    while (cursor > 0 && std::isspace(static_cast<unsigned char>(source[cursor - 1]))) {
      --cursor;
    }
    if (cursor == 0 || !IsIdentifierCharacter(source[cursor - 1])) {
      return false;
    }
    while (cursor > 0 && IsIdentifierCharacter(source[cursor - 1])) {
      --cursor;
    }
    while (cursor > 0 && std::isspace(static_cast<unsigned char>(source[cursor - 1]))) {
      --cursor;
    }
  }
  if (cursor == 0) {
    return false;
  }

  const char previous = source[cursor - 1];
  return IsIdentifierCharacter(previous) || previous == '*' || previous == '&' || previous == '>' || previous == ')' ||
         previous == ']' || previous == ':';
}

[[nodiscard]] bool
IsFunctionDeclaration(std::string_view source, std::size_t identifier_offset, std::size_t argument_end) {
  const std::size_t suffix = SkipTrivia(source, argument_end + 1);
  if (suffix >= source.size() || source[suffix] != ';') {
    return false;
  }
  const std::size_t line_begin = source.rfind('\n', identifier_offset);
  const std::size_t prefix_begin = line_begin == std::string_view::npos ? 0 : line_begin + 1;
  std::string_view prefix = source.substr(prefix_begin, identifier_offset - prefix_begin);
  while (!prefix.empty() && std::isspace(static_cast<unsigned char>(prefix.back()))) {
    prefix.remove_suffix(1);
  }
  if (prefix.empty() || prefix.ends_with("::") || prefix.ends_with('.') || prefix.ends_with("->")) {
    return false;
  }
  if (prefix.ends_with("return") || prefix.ends_with("co_return")) {
    return false;
  }
  return prefix.find_first_of("=(),?:") == std::string_view::npos;
}

[[nodiscard]] std::vector<FunctionBody> FindUseFunctionBodies(std::string_view source) {
  std::vector<FunctionBody> bodies;
  std::size_t cursor = 0;
  while (cursor < source.size()) {
    if (const auto end = SkipNonCode(source, cursor)) {
      cursor = *end;
      continue;
    }
    if (IsPreprocessorDirective(source, cursor)) {
      cursor = SkipPreprocessorDirective(source, cursor);
      continue;
    }
    if (!IsIdentifierStart(source[cursor])) {
      ++cursor;
      continue;
    }
    const auto identifier = ReadIdentifier(source, cursor);
    cursor = identifier->end;
    if (!IsUseCallName(identifier->text) || IsMemberCall(source, identifier->begin) ||
        !CanBeFunctionName(source, identifier->begin)) {
      continue;
    }
    const std::size_t parameters = SkipTrivia(source, identifier->end);
    if (parameters >= source.size() || source[parameters] != '(') {
      continue;
    }
    const std::size_t parameter_end =
        FindMatchingDelimiter(source, parameters, '(', ')', "unable to match composition function parameter list");
    if (!CanBeginFunctionSuffix(source, parameter_end)) {
      continue;
    }
    const auto body = FindFunctionBody(source, parameter_end, "unable to locate composition hook body");
    if (!body.has_value()) {
      continue;
    }
    bodies.push_back(FunctionBody{identifier->begin, *body});
    cursor = bodies.back().range.closing_brace + 1;
  }
  return bodies;
}

void ValidateCompositionCalls(
    std::string_view source,
    const std::vector<ComposableBody>& composables,
    const std::vector<SourceRange>& application_roots,
    const std::vector<FunctionBody>& hooks
) {
  std::size_t cursor = 0;
  while (cursor < source.size()) {
    if (const auto end = SkipNonCode(source, cursor)) {
      cursor = *end;
      continue;
    }
    if (IsPreprocessorDirective(source, cursor)) {
      cursor = SkipPreprocessorDirective(source, cursor);
      continue;
    }
    if (!IsIdentifierStart(source[cursor])) {
      ++cursor;
      continue;
    }
    const auto identifier = ReadIdentifier(source, cursor);
    cursor = identifier->end;
    if (!IsUseCallName(identifier->text) || IsMemberCall(source, identifier->begin)) {
      continue;
    }
    std::size_t arguments = SkipTrivia(source, identifier->end);
    if (arguments < source.size() && source[arguments] == '<') {
      arguments = SkipTrivia(
          source,
          FindMatchingDelimiter(
              source,
              arguments,
              '<',
              '>',
              "unable to match composition function template argument list"
          ) + 1
      );
    }
    if (arguments >= source.size() || source[arguments] != '(') {
      continue;
    }
    const std::size_t argument_end =
        FindMatchingDelimiter(source, arguments, '(', ')', "unable to match composition function argument list");
    if (IsFunctionDeclaration(source, identifier->begin, argument_end)) {
      continue;
    }
    if (std::ranges::any_of(hooks, [offset = identifier->begin](const FunctionBody& hook) {
          return hook.name_offset == offset;
        })) {
      continue;
    }
    const bool in_composable =
        std::ranges::any_of(composables, [offset = identifier->begin](const ComposableBody& body) {
          return Contains(body, offset);
        });
    const bool in_application_root =
        std::ranges::any_of(application_roots, [offset = identifier->begin](const SourceRange& body) {
          return Contains(body, offset);
        });
    const bool in_hook = std::ranges::any_of(hooks, [offset = identifier->begin](const FunctionBody& hook) {
      return Contains(hook.range, offset);
    });
    if (!in_composable && !in_application_root && !in_hook) {
      throw TransformError(
          identifier->begin,
          "composition function " + std::string(identifier->text) +
              "() must be called from a [[huxerui::composable]] function"
      );
    }
  }
}

[[nodiscard]] std::string EscapeLinePath(std::string_view source_path) {
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

[[nodiscard]] std::string LineDirective(std::size_t line, std::string_view escaped_path) {
  return "#line " + std::to_string(line) + " \"" + std::string(escaped_path) + "\"\n";
}

[[nodiscard]] std::string BlankMarker() {
  return std::string(kComposableMarker.size(), ' ');
}

} // namespace

TransformError::TransformError(std::size_t offset, std::string message)
    : std::runtime_error(std::move(message)), offset_(offset) {}

SourcePosition PositionAt(std::string_view source, std::size_t offset) noexcept {
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

TransformResult TransformSource(std::string_view source, std::string_view source_path) {
  std::vector<ComposableBody> composables;
  std::size_t cursor = 0;

  while (cursor < source.size()) {
    if (const auto end = SkipNonCode(source, cursor)) {
      cursor = *end;
      continue;
    }
    if (!StartsWith(source, cursor, kComposableMarker)) {
      ++cursor;
      continue;
    }

    const std::size_t opening_brace = FindOpeningBrace(source, cursor);
    const std::size_t closing_brace = FindClosingBrace(source, cursor, opening_brace);
    composables.push_back(
        ComposableBody{
            cursor,
            opening_brace,
            closing_brace,
        }
    );
    cursor = closing_brace + 1;
  }

  std::vector<SourceRange> composition_roots = FindApplicationRootBodies(source);
  std::vector<SourceRange> explicit_scopes = FindExplicitScopeBodies(source);
  composition_roots.insert(composition_roots.end(), explicit_scopes.begin(), explicit_scopes.end());
  explicit_scopes = FindExplicitScopeMacroBodies(source);
  composition_roots.insert(composition_roots.end(), explicit_scopes.begin(), explicit_scopes.end());
  ValidateCompositionCalls(source, composables, composition_roots, FindUseFunctionBodies(source));

  if (composables.empty()) {
    return TransformResult{
        std::string(source),
        0,
    };
  }

  const std::string escaped_path = EscapeLinePath(source_path);
  std::vector<Edit> edits;
  edits.reserve(composables.size() * 3);

  for (const ComposableBody& composable : composables) {
    const SourcePosition opening = PositionAt(source, composable.opening_brace);
    const SourcePosition closing = PositionAt(source, composable.closing_brace);

    edits.push_back(
        Edit{
            composable.marker_offset,
            kComposableMarker.size(),
            BlankMarker(),
        }
    );
    edits.push_back(
        Edit{
            composable.opening_brace + 1,
            0,
            "\n  HUXERUI_SCOPE_BEGIN\n" + LineDirective(opening.line, escaped_path),
        }
    );
    edits.push_back(
        Edit{
            composable.closing_brace,
            0,
            "\n  HUXERUI_SCOPE_END\n" + LineDirective(closing.line, escaped_path),
        }
    );
  }

  std::sort(edits.begin(), edits.end(), [](const Edit& left, const Edit& right) { return left.offset > right.offset; });

  std::string transformed(source);
  for (const Edit& edit : edits) {
    transformed.replace(edit.offset, edit.length, edit.replacement);
  }
  transformed.insert(0, LineDirective(1, escaped_path));

  return TransformResult{
      std::move(transformed),
      composables.size(),
  };
}

} // namespace huxerui::codegen
