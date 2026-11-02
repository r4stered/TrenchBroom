/*
 Copyright (C) 2026 Drew Williams

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
#include "fs/DiskIO.h"
#include "fs/File.h"
#include "fs/Reader.h"
#include "mdl/CatchConfig.h"
#include "mdl/LoadBspLeafTree.h"
#include "mdl/PortalFile.h"

#include "kd/filesystem_utils.h"
#include "kd/result.h"

#include "vm/bbox_io.h" // IWYU pragma: keep
#include "vm/polygon.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::mdl
{
namespace
{

const auto CompiledMaps = std::filesystem::path{"test/mdl/CompiledMaps"};
const auto Prt1Bsp = CompiledMaps / "rooms_prt1/qbsp/rooms_prt1.bsp";
const auto Prt1Bsp2 = CompiledMaps / "rooms_prt1_bsp2/qbsp/rooms_prt1.bsp";
const auto Prt1Prt = CompiledMaps / "rooms_prt1/qbsp/rooms_prt1.prt";
const auto Prt2Bsp = CompiledMaps / "rooms_prt2_water/qbsp/rooms_prt2_water.bsp";
const auto Prt2Prt = CompiledMaps / "rooms_prt2_water/qbsp/rooms_prt2_water.prt";

Result<BspLeafTree> loadLeafTree(const std::filesystem::path& path)
{
  return fs::Disk::openFile(getFixtureRoot() / path) | kdl::and_then([](auto file) {
           return loadBspLeafTree(file->reader().buffer());
         });
}

BspLeafTree requireLeafTree(const std::filesystem::path& path)
{
  return loadLeafTree(path) | kdl::if_error([](const auto& e) { FAIL(e.msg); })
         | kdl::value();
}

PortalGraph loadGraph(const std::filesystem::path& path)
{
  return kdl::with_istream(
           getFixtureRoot() / path, [](auto& stream) { return loadPortalGraph(stream); })
         | kdl::value();
}

vm::bbox3d mergedBounds(const std::vector<BspLeaf>& leafs, const BspLeafContents contents)
{
  auto result = std::optional<vm::bbox3d>{};
  for (const auto& leaf : leafs)
  {
    if (leaf.contents == contents)
    {
      result = result ? vm::merge(*result, leaf.bounds) : leaf.bounds;
    }
  }
  return result.value_or(vm::bbox3d{});
}

/**
 * Returns the BSP leafs on either side of the center of the given portal.
 */
std::set<size_t> leafsAtPortal(const BspLeafTree& tree, const vm::polygon3f& polygon)
{
  const auto& vertices = polygon.vertices();
  const auto center = vm::vec3d{polygon.center()};
  const auto normal = vm::vec3d{
    vm::normalize(vm::cross(vertices[1] - vertices[0], vertices[2] - vertices[0]))};

  return {tree.findLeaf(center + normal), tree.findLeaf(center - normal)};
}

} // namespace

TEST_CASE("BspLeafTree")
{
  SECTION("leafs")
  {
    SECTION("BSP29 without liquids")
    {
      const auto tree = requireLeafTree(Prt1Bsp);
      const auto& leafs = tree.leafs();

      // leaf 0 and the 7 vis leafs of the portal file
      REQUIRE(leafs.size() == 8);
      CHECK(leafs[0].contents == BspLeafContents::Solid);
      for (size_t i = 1; i < leafs.size(); ++i)
      {
        CAPTURE(i);
        CHECK(leafs[i].contents == BspLeafContents::Empty);
      }

      // the empty leafs fill the interior of both rooms and the doorway
      CHECK(
        mergedBounds(leafs, BspLeafContents::Empty)
        == vm::bbox3d{{-256, -128, 0}, {272, 128, 128}});
    }

    SECTION("BSP29 with water")
    {
      const auto tree = requireLeafTree(Prt2Bsp);
      const auto& leafs = tree.leafs();

      REQUIRE(leafs.size() == 12);
      CHECK(leafs[0].contents == BspLeafContents::Solid);
      CHECK(leafs[4].contents == BspLeafContents::Water);
      CHECK(
        mergedBounds(leafs, BspLeafContents::Water)
        == vm::bbox3d{{80, -96, 0}, {208, 96, 48}});
    }

    SECTION("BSP2 has the same leafs as BSP29")
    {
      const auto bsp29 = requireLeafTree(Prt1Bsp);
      const auto bsp2 = requireLeafTree(Prt1Bsp2);
      CHECK(bsp2.leafs() == bsp29.leafs());
    }
  }

  SECTION("findLeaf")
  {
    using T = std::tuple<std::filesystem::path, vm::vec3d, size_t, BspLeafContents>;

    // expected leafs as reported by bsputil -findleaf from the pinned ericw-tools
    // clang-format off
    const auto
    [path,     point,          expectedLeaf, expectedContents] = GENERATE_COPY(values<T>({
    {Prt1Bsp,  {-128, 0, 24},  5,            BspLeafContents::Empty},
    {Prt1Bsp,  {-128, 100, 24}, 2,           BspLeafContents::Empty},
    {Prt1Bsp,  {144, 0, 96},   3,            BspLeafContents::Empty},
    {Prt1Bsp,  {8, 0, 48},     4,            BspLeafContents::Empty},
    {Prt1Bsp,  {8, 0, 112},    0,            BspLeafContents::Solid},
    {Prt1Bsp,  {500, 0, 0},    0,            BspLeafContents::Solid},
    {Prt1Bsp2, {-128, 0, 24},  5,            BspLeafContents::Empty},
    {Prt1Bsp2, {8, 0, 112},    0,            BspLeafContents::Solid},
    {Prt2Bsp,  {144, 0, 24},   4,            BspLeafContents::Water},
    {Prt2Bsp,  {144, 0, 96},   3,            BspLeafContents::Empty},
    {Prt2Bsp,  {-144, 0, 64},  0,            BspLeafContents::Solid},
    {Prt2Bsp,  {-208, 0, 24},  11,           BspLeafContents::Empty},
    {Prt2Bsp,  {8, 0, 48},     7,            BspLeafContents::Empty},
    }));
    // clang-format on

    CAPTURE(path, point);

    const auto tree = requireLeafTree(path);
    const auto leaf = tree.findLeaf(point);
    CHECK(leaf == expectedLeaf);
    REQUIRE(leaf < tree.leafs().size());
    CHECK(tree.leafs()[leaf].contents == expectedContents);
    if (expectedContents != BspLeafContents::Solid)
    {
      CHECK(tree.leafs()[leaf].bounds.contains(point));
    }
  }
}

TEST_CASE("loadBspLeafTree")
{
  SECTION("Half-Life BSP")
  {
    const auto tree = requireLeafTree("test/mdl/LoadBspModel/hl.bsp");
    REQUIRE(!tree.leafs().empty());
    CHECK(tree.leafs()[0].contents == BspLeafContents::Solid);
  }

  SECTION("unsupported version")
  {
    CHECK(
      (loadLeafTree("test/mdl/LoadBspModel/invalid_version.bsp")
       | kdl::transform([](const auto&) { return std::string{}; })
       | kdl::transform_error([](const auto& e) { return e.msg; }) | kdl::value())
      == "Unsupported BSP version: 63");
  }

  SECTION("truncated file")
  {
    const auto file = fs::Disk::openFile(getFixtureRoot() / Prt1Bsp) | kdl::value();
    const auto buffer = file->reader().buffer();
    const auto bytes = buffer.stringView();
    const auto truncated = std::string{bytes.substr(0, bytes.size() / 2)};

    CHECK(loadBspLeafTree(
            fs::Reader::from(truncated.data(), truncated.data() + truncated.size()))
            .is_error());
  }

  SECTION("vis leaf i of the portal file is BSP leaf i+1")
  {
    SECTION("PRT1")
    {
      const auto tree = requireLeafTree(Prt1Bsp);
      const auto graph = loadGraph(Prt1Prt);

      REQUIRE(tree.leafs().size() == graph.leafCount + 1);
      for (const auto& portal : graph.portals)
      {
        CAPTURE(portal);
        CHECK(
          leafsAtPortal(tree, portal.polygon) == std::set{portal.a + 1, portal.b + 1});
      }
    }

    SECTION("PRT2")
    {
      const auto tree = requireLeafTree(Prt2Bsp);
      const auto graph = loadGraph(Prt2Prt);

      REQUIRE(tree.leafs().size() == graph.leafCount + 1);
      for (const auto& portal : graph.portals)
      {
        CAPTURE(portal);

        auto clusters = std::set<size_t>{};
        for (const auto leaf : leafsAtPortal(tree, portal.polygon))
        {
          REQUIRE(leaf > 0);
          clusters.insert(graph.clusterOfLeaf[leaf - 1]);
        }
        CHECK(clusters == std::set{portal.a, portal.b});
      }

      // the water leaf belongs to the cluster of the room with the pool
      const auto waterLeaf = tree.findLeaf({144, 0, 24});
      const auto roomLeaf = tree.findLeaf({144, 0, 96});
      CHECK(graph.clusterOfLeaf[waterLeaf - 1] == graph.clusterOfLeaf[roomLeaf - 1]);
    }
  }
}

} // namespace tb::mdl
