#include <catch2/catch_amalgamated.hpp>

#include <huxerui/app.h>

namespace huxerui::test {
namespace {

View EmptyApplication() {
  return {};
}

} // namespace

TEST_CASE("Application registration follows object lifetime") {
  REQUIRE_THROWS_WITH(detail::CurrentApplication(), "HuxerUI application has not been declared");

  Application first{EmptyApplication};
  REQUIRE(&detail::CurrentApplication() == &first);

  {
    Application second{EmptyApplication};
    REQUIRE_THROWS_WITH(detail::CurrentApplication(), "HuxerUI application declaration is not unique");
  }

  REQUIRE(&detail::CurrentApplication() == &first);
}

TEST_CASE("Application requires a root factory") {
  REQUIRE_THROWS_WITH(Application(nullptr), "HuxerUI application requires a root factory");
  REQUIRE_THROWS_WITH(detail::CurrentApplication(), "HuxerUI application has not been declared");
}

} // namespace huxerui::test
