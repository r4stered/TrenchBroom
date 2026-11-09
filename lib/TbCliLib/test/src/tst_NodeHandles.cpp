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

#include "cli/CliError.h"
#include "cli/Json.h"
#include "cli/NodeHandles.h"
#include "mdl/BrushNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Nodes.h"
#include "mdl/TestFactory.h"
#include "mdl/WorldNode.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{
namespace
{

mdl::Node* addBrush(mdl::Map& map, mdl::Node& parent)
{
  auto nodes = std::vector<std::unique_ptr<mdl::Node>>{};
  nodes.push_back(mdl::createBrushNode(map));
  return mdl::addNodes(map, parent, std::move(nodes)).front();
}

std::optional<ErrorCode> errorCode(const std::function<void()>& f)
{
  try
  {
    f();
  }
  catch (const CliError& e)
  {
    return e.code();
  }
  return std::nullopt;
}

} // namespace

TEST_CASE("NodeHandleTable")
{
  auto fixture = mdl::MapFixture{};
  auto& map = fixture.create();
  auto& layer = *map.worldNode().defaultLayer();
  auto* brush0 = addBrush(map, layer);
  auto* brush1 = addBrush(map, layer);

  auto handles = NodeHandleTable{map, "ab12"};

  SECTION("handle")
  {
    CHECK(handles.handle(*brush0) == "n:ab12.1");
    CHECK(handles.handle(*brush1) == "n:ab12.2");
    // a node keeps its handle
    CHECK(handles.handle(*brush0) == "n:ab12.1");
  }

  SECTION("faceHandle")
  {
    CHECK(
      handles.faceHandle(static_cast<const mdl::BrushNode&>(*brush1), 3)
      == "n:ab12.1/face:3");
  }

  SECTION("resolve")
  {
    const auto handle0 = handles.handle(*brush0);
    const auto handle1 = handles.handle(*brush1);

    SECTION("names the node, or a face of it")
    {
      CHECK(handles.resolve(handle1).node == brush1);
      CHECK(handles.resolve(handle1).faceIndex == std::nullopt);
      CHECK(handles.resolve(handle1 + "/face:2").node == brush1);
      CHECK(handles.resolve(handle1 + "/face:2").faceIndex == 2);
    }

    SECTION("follows the node when its path shifts")
    {
      mdl::removeNodes(map, {brush0});
      CHECK(handles.resolve(handle1).node == brush1);
    }

    SECTION("gives gone for a removed node, and names it again after undo")
    {
      mdl::removeNodes(map, {brush0});
      CHECK(errorCode([&]() { handles.resolve(handle0); }) == ErrorCode::Gone);

      map.undoCommand();
      CHECK(handles.resolve(handle0).node == brush0);
    }

    SECTION("gives stale_session for a handle of another epoch")
    {
      CHECK(errorCode([&]() { handles.resolve("n:cd34.1"); }) == ErrorCode::StaleSession);
    }

    SECTION("gives node_not_found for a handle never given out or a missing face")
    {
      CHECK(errorCode([&]() { handles.resolve("n:ab12.9"); }) == ErrorCode::NodeNotFound);
      CHECK(
        errorCode([&]() { handles.resolve(handle1 + "/face:99"); })
        == ErrorCode::NodeNotFound);
    }

    SECTION("gives invalid_path for a malformed handle or a face of a non-brush")
    {
      for (const auto* malformed :
           {"n:", "n:ab12", "n:ab12.", "n:ab12.0", "n:.1", "n:ab12.1/brush:0", "x"})
      {
        CAPTURE(malformed);
        CHECK(errorCode([&]() { handles.resolve(malformed); }) == ErrorCode::InvalidPath);
      }
      const auto layerHandle = handles.handle(layer);
      CHECK(
        errorCode([&]() { handles.resolve(layerHandle + "/face:0"); })
        == ErrorCode::InvalidPath);
    }
  }
}

TEST_CASE("NodeHandles")
{
  auto fixture = mdl::MapFixture{};
  auto& map = fixture.create();
  auto& layer = *map.worldNode().defaultLayer();
  auto* brush = addBrush(map, layer);
  const auto& brushNode = static_cast<const mdl::BrushNode&>(*brush);

  auto handles = NodeHandleTable{map, "ab12"};

  SECTION("isNodeHandle")
  {
    CHECK(isNodeHandle("n:ab12.1"));
    CHECK(!isNodeHandle("layer:0"));
    CHECK(!isNodeHandle("world"));
  }

  SECTION("resolveNodeReference")
  {
    auto& worldNode = map.worldNode();
    CHECK(resolveNodeReference(worldNode, "layer:0/brush:0", nullptr).node == brush);
    CHECK(resolveNodeReference(worldNode, "layer:0/brush:0", &handles).node == brush);

    const auto handle = handles.handle(*brush);
    CHECK(resolveNodeReference(worldNode, handle, &handles).node == brush);
    // without a live session, a handle is just a malformed path
    CHECK(
      errorCode([&]() { resolveNodeReference(worldNode, handle, nullptr); })
      == ErrorCode::InvalidPath);
  }

  SECTION("handleKey")
  {
    CHECK(handleKey("path") == "handle");
    CHECK(handleKey("entity") == "entity_handle");
    CHECK(handleKey("created", true) == "created_handles");
  }

  SECTION("setNodePath")
  {
    auto json = Json::object();
    setNodePath(json, "entity", *brush, nullptr);
    CHECK(json == Json{Json::Object{{"entity", "layer:0/brush:0"}}});

    setNodePath(json, "path", *brush, &handles);
    CHECK(json["path"] == Json{"layer:0/brush:0"});
    CHECK(json["handle"] == Json{"n:ab12.1"});
  }

  SECTION("setFacePath")
  {
    auto json = Json::object();
    setFacePath(json, "path", brushNode, 2, nullptr);
    CHECK(json == Json{Json::Object{{"path", "layer:0/brush:0/face:2"}}});

    setFacePath(json, "node", brushNode, 2, &handles);
    CHECK(json["node"] == Json{"layer:0/brush:0/face:2"});
    CHECK(json["node_handle"] == Json{"n:ab12.1/face:2"});
  }

  SECTION("NodePathList")
  {
    auto withoutHandles = NodePathList{nullptr};
    withoutHandles.push(*brush);
    CHECK(withoutHandles.size() == 1);
    auto json = Json::object();
    std::move(withoutHandles).setOn(json, "created");
    CHECK(json == Json{Json::Object{{"created", Json::Array{"layer:0/brush:0"}}}});

    auto withHandles = NodePathList{&handles};
    withHandles.push("layer:0/brush:0", *brush);
    CHECK(withHandles.paths() == Json{Json::Array{"layer:0/brush:0"}});
    CHECK(withHandles.handles() == Json{Json::Array{"n:ab12.1"}});
    std::move(withHandles).setOn(json, "removed");
    CHECK(json["removed_handles"] == Json{Json::Array{"n:ab12.1"}});
  }
}

} // namespace tb::cli
