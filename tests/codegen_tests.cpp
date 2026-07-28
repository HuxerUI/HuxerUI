#include "transform.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using huxerui::codegen::TransformError;
using huxerui::codegen::TransformSource;

void Check(
    bool condition,
    std::string_view expression,
    int line) {
  if (condition) {
    return;
  }
  std::cerr
      << "Check failed at line "
      << line
      << ": "
      << expression
      << '\n';
  std::exit(1);
}

#define HUXERUI_CODEGEN_CHECK(expression) \
  Check((expression), #expression, __LINE__)

void TestUnmarkedSourceIsUnchanged() {
  const std::string source =
      "View Plain() {\n"
      "  return Text(\"plain\");\n"
      "}\n";
  const auto result =
      TransformSource(source, "plain.cpp");

  HUXERUI_CODEGEN_CHECK(result.scope_count == 0);
  HUXERUI_CODEGEN_CHECK(result.source == source);
}

void TestBasicScopeTransformation() {
  const std::string source =
      "[[huxerui::scope]]\n"
      "View Counter(int initial) {\n"
      "  auto count = UseState(initial);\n"
      "  return Text(count);\n"
      "}\n";
  const auto result =
      TransformSource(source, "counter.cpp");

  HUXERUI_CODEGEN_CHECK(result.scope_count == 1);
  HUXERUI_CODEGEN_CHECK(
      result.source.find("[[huxerui::scope]]") ==
      std::string::npos);
  HUXERUI_CODEGEN_CHECK(
      result.source.find("HUXERUI_SCOPE_BEGIN") !=
      std::string::npos);
  HUXERUI_CODEGEN_CHECK(
      result.source.find("HUXERUI_SCOPE_END") !=
      std::string::npos);
  HUXERUI_CODEGEN_CHECK(
      result.source.find("#line 1 \"counter.cpp\"") !=
      std::string::npos);
}

void TestNestedSyntaxAndLiterals() {
  const std::string source = R"source(
[[huxerui::scope]]
View Complex(bool enabled) {
  const std::string normal = "{ value }";
  const std::string raw = R"raw({ raw })raw";
  // }
  /* { */
  if (enabled) {
    auto factory = [] {
      return View{};
    };
    return factory();
  }
  return View{};
}
)source";
  const auto result =
      TransformSource(source, "complex.cpp");

  HUXERUI_CODEGEN_CHECK(result.scope_count == 1);
  HUXERUI_CODEGEN_CHECK(
      result.source.find("return factory();") !=
      std::string::npos);
  HUXERUI_CODEGEN_CHECK(
      result.source.find("const std::string raw") !=
      std::string::npos);
}

void TestMultipleScopes() {
  const std::string source =
      "[[huxerui::scope]]\n"
      "View First() { return Text(\"first\"); }\n"
      "\n"
      "View Plain() { return Text(\"plain\"); }\n"
      "\n"
      "[[huxerui::scope]]\n"
      "View Second() { return Text(\"second\"); }\n";
  const auto result =
      TransformSource(source, "multiple.cpp");

  HUXERUI_CODEGEN_CHECK(result.scope_count == 2);
  const std::size_t first =
      result.source.find("HUXERUI_SCOPE_BEGIN");
  HUXERUI_CODEGEN_CHECK(first != std::string::npos);
  HUXERUI_CODEGEN_CHECK(
      result.source.find("HUXERUI_SCOPE_BEGIN", first + 1) !=
      std::string::npos);
}

void TestMarkerInsideNonCodeIsIgnored() {
  const std::string source =
      "// [[huxerui::scope]]\n"
      "const char* marker = \"[[huxerui::scope]]\";\n";
  const auto result =
      TransformSource(source, "ignored.cpp");

  HUXERUI_CODEGEN_CHECK(result.scope_count == 0);
  HUXERUI_CODEGEN_CHECK(result.source == source);
}

template<class Function>
void ExpectTransformError(Function&& function) {
  bool rejected = false;
  try {
    function();
  } catch (const TransformError&) {
    rejected = true;
  }
  HUXERUI_CODEGEN_CHECK(rejected);
}

void TestDeclarationIsRejected() {
  ExpectTransformError([] {
    static_cast<void>(TransformSource(
        "[[huxerui::scope]] View Counter();\n",
        "declaration.cpp"));
  });
}

void TestExplicitScopeIsRejected() {
  ExpectTransformError([] {
    static_cast<void>(TransformSource(
        "[[huxerui::scope]]\n"
        "View Counter() {\n"
        "  HUXERUI_SCOPE_BEGIN\n"
        "  return Text(\"counter\");\n"
        "  HUXERUI_SCOPE_END\n"
        "}\n",
        "explicit.cpp"));
  });
}

void TestConditionalCompilationIsRejected() {
  ExpectTransformError([] {
    static_cast<void>(TransformSource(
        "[[huxerui::scope]]\n"
        "View Counter() {\n"
        "#if ENABLE_COUNTER\n"
        "  return Text(\"counter\");\n"
        "#else\n"
        "  return View{};\n"
        "#endif\n"
        "}\n",
        "conditional.cpp"));
  });
}

}  // namespace

int main() {
  TestUnmarkedSourceIsUnchanged();
  TestBasicScopeTransformation();
  TestNestedSyntaxAndLiterals();
  TestMultipleScopes();
  TestMarkerInsideNonCodeIsIgnored();
  TestDeclarationIsRejected();
  TestExplicitScopeIsRejected();
  TestConditionalCompilationIsRejected();
  return 0;
}
