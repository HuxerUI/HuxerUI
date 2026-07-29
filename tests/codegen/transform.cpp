#include "transform.h"

#include <catch2/catch_amalgamated.hpp>

#include <string>

namespace {

using huxerui::codegen::TransformError;
using huxerui::codegen::TransformSource;

TEST_CASE("Unmarked source is unchanged") {
  const std::string source = "View Plain() {\n"
                             "  return Text(\"plain\");\n"
                             "}\n";
  const auto result = TransformSource(source, "plain.cpp");

  REQUIRE(result.scope_count == 0);
  REQUIRE(result.source == source);
}

TEST_CASE("Scope marker generates scope boundaries") {
  const std::string source = "[[huxerui::scope]]\n"
                             "View Counter(int initial) {\n"
                             "  auto count = UseState(initial);\n"
                             "  return Text(count);\n"
                             "}\n";
  const auto result = TransformSource(source, "counter.cpp");

  REQUIRE(result.scope_count == 1);
  REQUIRE(result.source.find("[[huxerui::scope]]") == std::string::npos);
  REQUIRE(result.source.find("HUXERUI_SCOPE_BEGIN") != std::string::npos);
  REQUIRE(result.source.find("HUXERUI_SCOPE_END") != std::string::npos);
  REQUIRE(result.source.find("#line 1 \"counter.cpp\"") != std::string::npos);
}

TEST_CASE("Nested syntax and literals are preserved") {
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
  const auto result = TransformSource(source, "complex.cpp");

  REQUIRE(result.scope_count == 1);
  REQUIRE(result.source.find("return factory();") != std::string::npos);
  REQUIRE(result.source.find("const std::string raw") != std::string::npos);
}

TEST_CASE("Multiple scopes are transformed") {
  const std::string source = "[[huxerui::scope]]\n"
                             "View First() { return Text(\"first\"); }\n"
                             "\n"
                             "View Plain() { return Text(\"plain\"); }\n"
                             "\n"
                             "[[huxerui::scope]]\n"
                             "View Second() { return Text(\"second\"); }\n";
  const auto result = TransformSource(source, "multiple.cpp");

  REQUIRE(result.scope_count == 2);
  const std::size_t first = result.source.find("HUXERUI_SCOPE_BEGIN");
  REQUIRE(first != std::string::npos);
  REQUIRE(result.source.find("HUXERUI_SCOPE_BEGIN", first + 1) != std::string::npos);
}

TEST_CASE("Markers inside non-code text are ignored") {
  const std::string source = "// [[huxerui::scope]]\n"
                             "const char* marker = \"[[huxerui::scope]]\";\n";
  const auto result = TransformSource(source, "ignored.cpp");

  REQUIRE(result.scope_count == 0);
  REQUIRE(result.source == source);
}

template <class Function> void ExpectTransformError(Function&& function) {
  bool rejected = false;
  try {
    function();
  } catch (const TransformError&) {
    rejected = true;
  }
  REQUIRE(rejected);
}

TEST_CASE("Scope declarations are rejected") {
  ExpectTransformError([] {
    static_cast<void>(TransformSource("[[huxerui::scope]] View Counter();\n", "declaration.cpp"));
  });
}

TEST_CASE("Explicit scope boundaries are rejected") {
  ExpectTransformError([] {
    static_cast<void>(TransformSource(
        "[[huxerui::scope]]\n"
        "View Counter() {\n"
        "  HUXERUI_SCOPE_BEGIN\n"
        "  return Text(\"counter\");\n"
        "  HUXERUI_SCOPE_END\n"
        "}\n",
        "explicit.cpp"
    ));
  });
}

TEST_CASE("Conditional compilation inside scopes is rejected") {
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
        "conditional.cpp"
    ));
  });
}

} // namespace
