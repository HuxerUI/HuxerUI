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

  REQUIRE(result.composable_count == 0);
  REQUIRE(result.source == source);
}

TEST_CASE("Composable marker generates scope boundaries") {
  const std::string source = "[[huxerui::composable]]\n"
                             "View Counter(int initial) {\n"
                             "  auto count = UseState(initial);\n"
                             "  return Text(count);\n"
                             "}\n";
  const auto result = TransformSource(source, "counter.cpp");

  REQUIRE(result.composable_count == 1);
  REQUIRE(result.source.find("[[huxerui::composable]]") == std::string::npos);
  REQUIRE(result.source.find("HUXERUI_SCOPE_BEGIN") != std::string::npos);
  REQUIRE(result.source.find("HUXERUI_SCOPE_END") != std::string::npos);
  REQUIRE(result.source.find("#line 1 \"counter.cpp\"") != std::string::npos);
}

TEST_CASE("Nested syntax and literals are preserved") {
  const std::string source = R"source(
[[huxerui::composable]]
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

  REQUIRE(result.composable_count == 1);
  REQUIRE(result.source.find("return factory();") != std::string::npos);
  REQUIRE(result.source.find("const std::string raw") != std::string::npos);
}

TEST_CASE("Multiple composables are transformed") {
  const std::string source = "[[huxerui::composable]]\n"
                             "View First() { return Text(\"first\"); }\n"
                             "\n"
                             "View Plain() { return Text(\"plain\"); }\n"
                             "\n"
                             "[[huxerui::composable]]\n"
                             "View Second() { return Text(\"second\"); }\n";
  const auto result = TransformSource(source, "multiple.cpp");

  REQUIRE(result.composable_count == 2);
  const std::size_t first = result.source.find("HUXERUI_SCOPE_BEGIN");
  REQUIRE(first != std::string::npos);
  REQUIRE(result.source.find("HUXERUI_SCOPE_BEGIN", first + 1) != std::string::npos);
}

TEST_CASE("Markers inside non-code text are ignored") {
  const std::string source = "// [[huxerui::composable]]\n"
                             "const char* marker = \"[[huxerui::composable]]\";\n";
  const auto result = TransformSource(source, "ignored.cpp");

  REQUIRE(result.composable_count == 0);
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

TEST_CASE("Composable declarations are rejected") {
  ExpectTransformError([] {
    static_cast<void>(TransformSource("[[huxerui::composable]] View Counter();\n", "declaration.cpp"));
  });
}

TEST_CASE("Explicit scope boundaries are rejected") {
  ExpectTransformError([] {
    static_cast<void>(TransformSource(
        "[[huxerui::composable]]\n"
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
        "[[huxerui::composable]]\n"
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

TEST_CASE("Unmarked composition calls are rejected") {
  ExpectTransformError([] {
    static_cast<void>(TransformSource(
        "View Counter() {\n"
        "  auto count = UseState(0);\n"
        "  return Text(count);\n"
        "}\n",
        "unmarked.cpp"
    ));
  });
}

TEST_CASE("Qualified unmarked composition calls are rejected") {
  ExpectTransformError([] {
    static_cast<void>(TransformSource(
        "View Timer() {\n"
        "  auto timer = example::UseTimer();\n"
        "  return View{};\n"
        "}\n",
        "qualified.cpp"
    ));
  });
}

TEST_CASE("Templated unmarked composition calls are rejected") {
  ExpectTransformError([] {
    static_cast<void>(TransformSource(
        "View Content() {\n"
        "  const auto& value = UseEnvironment<Locale>();\n"
        "  return Text(value.name);\n"
        "}\n",
        "templated.cpp"
    ));
  });
}

TEST_CASE("Composition calls in control flow are not mistaken for hook definitions") {
  ExpectTransformError([] {
    static_cast<void>(TransformSource(
        "View Content() {\n"
        "  if (UseState(false)) {\n"
        "    return Text(\"active\");\n"
        "  }\n"
        "  return View{};\n"
        "}\n",
        "control_flow.cpp"
    ));
  });
}

TEST_CASE("Qualified composition calls in control flow are not mistaken for hook definitions") {
  ExpectTransformError([] {
    static_cast<void>(TransformSource(
        "View Content() {\n"
        "  if (huxerui::UseState(false)) {\n"
        "    return Text(\"active\");\n"
        "  }\n"
        "  return View{};\n"
        "}\n",
        "qualified_control_flow.cpp"
    ));
  });
}

TEST_CASE("Application roots use their implicit composition scope") {
  const std::string source = "View App() {\n"
                             "  auto count = UseState(0);\n"
                             "  return Text(count);\n"
                             "}\n"
                             "const Application application{App};\n";
  const auto result = TransformSource(source, "application.cpp");

  REQUIRE(result.composable_count == 0);
  REQUIRE(result.source == source);
}

TEST_CASE("Use-prefixed hooks share their caller composition context") {
  const std::string source = "auto UseService() noexcept -> Service { return UseState(Service{}); }\n"
                             "View Plain(Helper& helper) {\n"
                             "  helper.UseValue();\n"
                             "  return View{};\n"
                             "}\n";
  const auto result = TransformSource(source, "plain.cpp");

  REQUIRE(result.composable_count == 0);
  REQUIRE(result.source == source);
}

TEST_CASE("Use-prefixed function declarations are not composition calls") {
  const std::string source = "Service UseService();\n"
                             "View Plain() { return View{}; }\n";
  const auto result = TransformSource(source, "hook_declaration.cpp");

  REQUIRE(result.composable_count == 0);
  REQUIRE(result.source == source);
}

TEST_CASE("Explicit Scope lambdas provide a composition context") {
  const std::string source = "View Counter() {\n"
                             "  return Scope([] {\n"
                             "    auto count = UseState(0);\n"
                             "    return Text(count);\n"
                             "  });\n"
                             "}\n";
  const auto result = TransformSource(source, "explicit_scope.cpp");

  REQUIRE(result.composable_count == 0);
  REQUIRE(result.source == source);
}

TEST_CASE("Explicit scope macros provide a composition context") {
  const std::string source = "View Counter() {\n"
                             "  HUXERUI_SCOPE_BEGIN\n"
                             "  auto count = UseState(0);\n"
                             "  return Text(count);\n"
                             "  HUXERUI_SCOPE_END\n"
                             "}\n";
  const auto result = TransformSource(source, "explicit_scope_macro.cpp");

  REQUIRE(result.composable_count == 0);
  REQUIRE(result.source == source);
}

TEST_CASE("Explicit scope macro calls provide a composition context") {
  const std::string source = "View Counter() {\n"
                             "  HUXERUI_SCOPE({\n"
                             "    auto count = UseState(0);\n"
                             "    return Text(count);\n"
                             "  });\n"
                             "}\n";
  const auto result = TransformSource(source, "explicit_scope_call.cpp");

  REQUIRE(result.composable_count == 0);
  REQUIRE(result.source == source);
}

TEST_CASE("Composition calls in preprocessor directives are outside lexical validation") {
  const std::string source = R"source(#define HUXERUI_TEST_STATE() \
  UseState(0)
View Plain() { return View{}; }
)source";
  const auto result = TransformSource(source, "macro.cpp");

  REQUIRE(result.composable_count == 0);
  REQUIRE(result.source == source);
}

} // namespace
