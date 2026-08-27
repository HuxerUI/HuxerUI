#include <catch2/catch_amalgamated.hpp>

#include <huxerui/geometry.h>

using namespace huxerui;

TEST_CASE("PointSupportsVectorArithmetic") {
  Point point{4.0F, 6.0F};

  REQUIRE((point + Point{2.0F, 3.0F} == Point{6.0F, 9.0F}));
  REQUIRE((point - Point{2.0F, 3.0F} == Point{2.0F, 3.0F}));
  REQUIRE((point * 2.0F == Point{8.0F, 12.0F}));
  REQUIRE((2.0F * point == Point{8.0F, 12.0F}));
  REQUIRE((point / 2.0F == Point{2.0F, 3.0F}));

  point += {1.0F, 2.0F};
  REQUIRE((point == Point{5.0F, 8.0F}));
  point -= {2.0F, 3.0F};
  REQUIRE((point == Point{3.0F, 5.0F}));
  point *= 3.0F;
  REQUIRE((point == Point{9.0F, 15.0F}));
  point /= 3.0F;
  REQUIRE((point == Point{3.0F, 5.0F}));
}
