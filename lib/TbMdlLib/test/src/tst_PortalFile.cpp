/*
 Copyright (C) 2010 Kristian Duske

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "TestEnvironment.h"
#include "mdl/CatchConfig.h"
#include "mdl/PortalFile.h"

#include "kd/filesystem_utils.h"

#include "vm/polygon.h"
#include "vm/polygon_io.h" // IWYU pragma: keep

#include <filesystem>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::mdl
{

namespace
{
const std::vector<vm::polygon3f> ExpectedPortals{
  {{-96, -32, 80}, {-96, 160, 80}, {0, 160, 80}, {0, -32, 80}},
  {{208, -64, 80}, {64, -64, 80}, {64, 160, 80}, {208, 160, 80}},
  {{64, 80, 48},
   {64, 80, 16},
   {64, 64, 0},
   {64, 32, 0},
   {64, 16, 16},
   {64, 16, 48},
   {64, 32, 64},
   {64, 64, 64}},
  {{0, 80, 48},
   {0, 80, 16},
   {0, 64, 0},
   {0, 32, 0},
   {0, 16, 16},
   {0, 16, 48},
   {0, 32, 64},
   {0, 64, 64}},
  {{-64, -32, 0}, {-32, -32, 0}, {-48, -32, 64}}};

std::vector<PortalGraphPortal> expectedGraphPortals()
{
  const auto cells = std::vector<std::pair<size_t, size_t>>{
    {1, 4},
    {1, 2},
    {2, 3},
    {3, 4},
    {4, 5},
  };

  auto result = std::vector<PortalGraphPortal>{};
  for (size_t i = 0; i < cells.size(); ++i)
  {
    result.push_back({ExpectedPortals[i], cells[i].first, cells[i].second});
  }
  return result;
}

Result<PortalGraph> loadPortalGraphFromFixture(const std::filesystem::path& path)
{
  return kdl::with_istream(
    getFixtureRoot() / path, [](auto& stream) { return loadPortalGraph(stream); });
}

Result<PortalGraph> loadPortalGraphFromString(const std::string& str)
{
  auto stream = std::istringstream{str};
  return loadPortalGraph(stream);
}
} // namespace

TEST_CASE("loadPortalGraph")
{
  SECTION("handwritten files")
  {
    using T = std::tuple<std::string, size_t, size_t, std::vector<size_t>>;

    // clang-format off
    const auto
    [fileName,                 expectedLeafCount, expectedClusterCount, expectedClusterOfLeaf] = GENERATE(values<T>({
    {"portaltest_prt1.prt",    6,                 6,                    {0, 1, 2, 3, 4, 5}},
    {"portaltest_prt1q3.prt",  6,                 6,                    {0, 1, 2, 3, 4, 5}},
    {"portaltest_prt1am.prt",  10,                6,                    {0, 1, 2, 2, 2, 2, 2, 3, 4, 5}},
    {"portaltest_prt2.prt",    10,                6,                    {0, 1, 2, 2, 2, 2, 2, 3, 4, 5}},
    }));
    // clang-format on

    CAPTURE(fileName);

    CHECK(
      loadPortalGraphFromFixture(std::filesystem::path{"test/mdl/PortalFile"} / fileName)
      == Result<PortalGraph>{PortalGraph{
        expectedLeafCount,
        expectedClusterCount,
        expectedGraphPortals(),
        expectedClusterOfLeaf,
      }});
  }

  SECTION("PRT1 compiled by qbsp")
  {
    const auto graph =
      loadPortalGraphFromFixture("test/mdl/CompiledMaps/rooms_prt1/qbsp/rooms_prt1.prt")
      | kdl::value();

    CHECK(graph.leafCount == 7);
    CHECK(graph.clusterCount == 7);
    CHECK(graph.clusterOfLeaf == std::vector<size_t>{0, 1, 2, 3, 4, 5, 6});
    REQUIRE(graph.portals.size() == 6);
    CHECK(
      graph.portals[0]
      == PortalGraphPortal{
        {{16, 32, 0}, {272, 32, 0}, {272, 32, 128}, {16, 32, 128}}, 0, 2});
    // the doorway in the wall between the rooms
    CHECK(
      graph.portals[3]
      == PortalGraphPortal{
        {{16, 32, 96}, {16, 32, 0}, {16, -32, 0}, {16, -32, 96}}, 2, 3});
  }

  SECTION("PRT2 compiled by qbsp")
  {
    CHECK(
      loadPortalGraphFromFixture(
        "test/mdl/CompiledMaps/rooms_prt2_water/qbsp/rooms_prt2_water.prt")
      == Result<PortalGraph>{PortalGraph{
        11,
        3,
        {
          {{{16, 32, 0}, {16, -32, 0}, {16, -32, 96}, {16, 32, 96}}, 0, 1},
          {{{0, 32, 0}, {0, -32, 0}, {0, -32, 96}, {0, 32, 96}}, 1, 2},
        },
        {0, 0, 0, 0, 0, 0, 1, 2, 2, 2, 2},
      }});
  }

  SECTION("invalid files")
  {
    using T = std::tuple<std::string, std::string>;

    // clang-format off
    const auto
    [str,                                                    expectedError] = GENERATE(values<T>({
    {"",                                                     "Unknown portal format: "},
    {"PRT3\n",                                               "Unknown portal format: PRT3"},
    {"PRT1\n2\n",                                            "Line 3: Unexpected end of file"},
    {"PRT1\nx\n1\n",                                         "Line 2: Expected a number"},
    {"PRT1\n2\n1\n",                                         "Line 4: Unexpected end of file"},
    {"PRT1\n2\n1\n3 0 1 (0 0 0) (1 0 0)\n",                  "Line 4: Expected 3 points"},
    {"PRT1\n2\n1\n3 0 (0 0 0) (1 0 0) (1 1 0)\n",            "Line 4: Expected portal header"},
    {"PRT1\n2\n1\n3 0 2 (0 0 0) (1 0 0) (1 1 0)\n",          "Line 4: Cell index 2 out of range"},
    {"PRT1\n2\n1\n3 0 1 (0 0 0) (1 x 0) (1 1 0)\n",          "Line 4: Expected a number"},
    {"PRT2\n2\n1\n0\n",                                      "Line 5: Unexpected end of file"},
    {"PRT2\n2\n1\n0\n0 x -1\n",                              "Line 5: Expected a number"},
    {"PRT2\n2\n1\n0\n0 2 -1\n",                              "Line 5: Leaf index 2 out of range"},
    {"PRT2\n2\n1\n0\n0 1\n",                                 "Line 5: Expected -1"},
    {"PRT2\n2\n2\n0\n0 -1\n0 -1\n",                          "Line 6: Leaf 0 is in more than one cluster"},
    {"PRT2\n2\n2\n0\n0 -1\n-1\n",                            "Line 6: Leaf 1 is not in any cluster"},
    {"PRT1-AM\n1\n0\n2\n0\n",                                "Line 6: Unexpected end of file"},
    {"PRT1-AM\n1\n0\n2\n0\n1\n",                             "Line 6: Cluster index 1 out of range"},
    }));
    // clang-format on

    CAPTURE(str);
    CHECK(loadPortalGraphFromString(str) == Result<PortalGraph>{Error{expectedError}});
  }
}

TEST_CASE("loadPortalFile")
{
  SECTION("invalid PRT1")
  {
    const auto path =
      getFixtureRoot() / "test/mdl/PortalFile/portaltest_prt1_invalid.prt";
    CHECK(kdl::with_istream(path, [](auto& stream) {
            return loadPortalFile(stream);
          }).is_error());
  }

  SECTION("PRT1")
  {
    const auto path = getFixtureRoot() / "test/mdl/PortalFile/portaltest_prt1.prt";
    CHECK(
      (kdl::with_istream(path, [](auto& stream) { return loadPortalFile(stream); })
       | kdl::value())
      == ExpectedPortals);
  }

  SECTION("PRT1Q3")
  {
    const auto path = getFixtureRoot() / "test/mdl/PortalFile/portaltest_prt1q3.prt";
    CHECK(
      (kdl::with_istream(path, [](auto& stream) { return loadPortalFile(stream); })
       | kdl::value())
      == ExpectedPortals);
  }

  SECTION("PRT1AM")
  {
    const auto path = getFixtureRoot() / "test/mdl/PortalFile/portaltest_prt1am.prt";
    CHECK(
      (kdl::with_istream(path, [](auto& stream) { return loadPortalFile(stream); })
       | kdl::value())
      == ExpectedPortals);
  }

  SECTION("PRT2")
  {
    const auto path = getFixtureRoot() / "test/mdl/PortalFile/portaltest_prt2.prt";
    CHECK(
      (kdl::with_istream(path, [](auto& stream) { return loadPortalFile(stream); })
       | kdl::value())
      == ExpectedPortals);
  }

  SECTION("ignores the cells and the cluster trailer")
  {
    // loadPortalGraph rejects these files
    // (out of range cell index, an extra point and no cluster trailer)
    auto stream =
      std::istringstream{"PRT2\n2\n1\n1\n3 0 7 (0 0 0) (1 0 0) (1 1 0) (2 2 2)\n"};
    CHECK(
      loadPortalFile(stream)
      == Result<std::vector<vm::polygon3f>>{
        std::vector<vm::polygon3f>{{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}}}});
  }
}

} // namespace tb::mdl
