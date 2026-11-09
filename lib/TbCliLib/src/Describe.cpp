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

#include "cli/Describe.h"

#include "cli/BuiltRooms.h"
#include "cli/CappedList.h"
#include "cli/EntityJson.h"
#include "cli/GeometryJson.h"
#include "cli/Json.h"
#include "cli/MapContents.h"
#include "cli/MapRef.h"
#include "cli/NodeHandles.h"
#include "cli/NodeJson.h"
#include "cli/NodePath.h"
#include "cli/Verify.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityDefinitionUtils.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/PatchNode.h"
#include "mdl/PropertyDefinition.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"
#include "kd/ranges/contains.h"
#include "kd/ranges/to.h"

#include <algorithm>
#include <map>
#include <optional>
#include <ranges>
#include <unordered_map>

namespace tb::cli
{
namespace
{

// Caps that keep the output bounded however large the map is.
constexpr auto MaxTreeGroups = size_t{100};
constexpr auto MaxEntityClasses = size_t{100};
constexpr auto MaxTextures = size_t{100};
constexpr auto MaxKeyEntities = size_t{50};
constexpr auto MaxLinks = size_t{100};
constexpr auto MaxDanglingLinks = size_t{50};
constexpr auto MaxBuiltRooms = size_t{100};

/** The classes listed in full unless tbcli.json overrides them. */
const auto DefaultKeyEntityClasses = std::vector<std::string>{
  "info_player_start",
  "info_player_deathmatch",
  "info_player_coop",
  "trigger_changelevel",
};

/** The groups of each link set with more than one group, in file order. */
using LinkSets = std::unordered_map<std::string, std::vector<const mdl::GroupNode*>>;

LinkSets collectLinkSets(const std::vector<const mdl::GroupNode*>& groups)
{
  auto linkSets = LinkSets{};
  for (const auto* group : groups)
  {
    linkSets[group->linkId()].push_back(group);
  }
  std::erase_if(linkSets, [](const auto& entry) { return entry.second.size() < 2; });
  return linkSets;
}

/**
 * Writes the layer → group tree in file order. Every layer is written, but groups
 * only up to MaxTreeGroups. Linked groups are written once per instance.
 */
class TreeWriter
{
private:
  const LinkSets& m_linkSets;
  NodeHandleTable* m_handles;
  size_t m_total = 0;
  size_t m_writtenGroups = 0;
  bool m_truncated = false;

public:
  TreeWriter(const LinkSets& linkSets, NodeHandleTable* handles)
    : m_linkSets{linkSets}
    , m_handles{handles}
  {
  }

  Json write(const mdl::WorldNode& worldNode)
  {
    auto items = Json::array();
    for (const auto* layerNode : worldNode.allLayers())
    {
      items.push(writeLayer(*layerNode));
    }

    auto result = Json::object();
    setCapped(result, std::move(items), m_total, m_truncated);
    return result;
  }

private:
  Json writeLayer(const mdl::LayerNode& layerNode)
  {
    ++m_total;
    auto result = layerToJson(layerNode, m_handles);
    result.set("groups", writeChildGroups(layerNode));
    return result;
  }

  std::optional<Json> writeGroup(const mdl::GroupNode& groupNode)
  {
    ++m_total;
    if (m_writtenGroups == MaxTreeGroups)
    {
      // keep counting the groups that are left out
      m_truncated = true;
      writeChildGroups(groupNode);
      return std::nullopt;
    }
    ++m_writtenGroups;

    auto result = groupToJson(groupNode, m_handles);
    if (
      const auto linkSet = m_linkSets.find(groupNode.linkId());
      linkSet != m_linkSets.end())
    {
      auto siblings = NodePathList{m_handles};
      for (const auto* sibling : linkSet->second)
      {
        if (sibling != &groupNode)
        {
          siblings.push(*sibling);
        }
      }
      result.set("link_id", groupNode.linkId());
      std::move(siblings).setOn(result, "siblings");
    }
    result.set("groups", writeChildGroups(groupNode));
    return result;
  }

  Json writeChildGroups(const mdl::Node& node)
  {
    auto groups = Json::array();
    for (const auto* child : node.children())
    {
      child->accept(
        kdl::overload(
          [](const mdl::WorldNode&) {},
          [](const mdl::LayerNode&) {},
          [&](const mdl::GroupNode& groupNode) {
            if (auto group = writeGroup(groupNode))
            {
              groups.push(std::move(*group));
            }
          },
          [](const mdl::EntityNode&) {},
          [](const mdl::BrushNode&) {},
          [](const mdl::PatchNode&) {}));
    }
    return groups;
  }
};

Json entityClasses(const std::vector<const mdl::EntityNode*>& entities)
{
  struct EntityClass
  {
    std::string classname;
    size_t count = 0;
    bool definitionFound = false;
  };

  auto byName = std::map<std::string, EntityClass>{};
  for (const auto* entityNode : entities)
  {
    const auto& entity = entityNode->entity();
    auto& entityClass = byName[entity.classname()];
    if (entityClass.count++ == 0)
    {
      entityClass.classname = entity.classname();
      entityClass.definitionFound = entity.definition() != nullptr;
    }
  }

  // by count, then by name
  auto classes = byName | std::views::values | kdl::ranges::to<std::vector>();
  std::ranges::stable_sort(classes, std::ranges::greater{}, &EntityClass::count);

  return capped(classes, MaxEntityClasses, [](const auto& entityClass) {
    auto result = Json::object();
    result.set("classname", entityClass.classname);
    result.set("count", entityClass.count);
    result.set("definition", entityClass.definitionFound ? "found" : "missing");
    return result;
  });
}

/**
 * The texture histogram. Missing textures can only be told apart from found ones if
 * textures could be loaded, which requires the palette from the Quake directory, so
 * they are flagged only if a Quake directory is configured (`checked`).
 */
Json textures(const std::vector<const mdl::BrushNode*>& brushes, const bool checked)
{
  struct Texture
  {
    std::string name;
    size_t count = 0;
    bool missing = false;
  };

  auto byName = std::map<std::string, Texture>{};
  for (const auto* brushNode : brushes)
  {
    for (const auto& face : brushNode->brush().faces())
    {
      auto& texture = byName[face.materialName()];
      texture.name = face.materialName();
      texture.missing = face.material() == nullptr;
      ++texture.count;
    }
  }

  // missing textures first so that they survive truncation, then by count and name
  auto textures = byName | std::views::values | kdl::ranges::to<std::vector>();
  std::ranges::stable_sort(textures, [&](const auto& lhs, const auto& rhs) {
    if (checked && lhs.missing != rhs.missing)
    {
      return lhs.missing;
    }
    return lhs.count > rhs.count;
  });

  auto result = Json::object();
  result.set("checked", checked);
  if (checked)
  {
    result.set("missing", std::ranges::count_if(textures, &Texture::missing));
  }
  setCapped(result, textures, MaxTextures, [&](const auto& texture) {
    auto entry = Json::object();
    entry.set("name", texture.name);
    entry.set("count", texture.count);
    if (checked)
    {
      entry.set("missing", texture.missing);
    }
    return entry;
  });
  return result;
}

/**
 * Worldspawn, then the entities of the key entity classes, then the other entities
 * with a targetname or target, each in file order. Listing the key classes before the
 * targeted entities keeps spawns and exits from being cut off by the cap.
 */
Json keyEntities(
  const mdl::WorldNode& worldNode,
  const std::vector<const mdl::EntityNode*>& entities,
  const std::vector<std::string>& keyEntityClasses,
  NodeHandleTable* handles)
{
  const auto isOfKeyClass = [&](const mdl::EntityNode* entityNode) {
    return kdl::ranges::contains(keyEntityClasses, entityNode->entity().classname());
  };
  const auto isLinked = [&](const mdl::EntityNode* entityNode) {
    return !isOfKeyClass(entityNode) && hasLink(entityNode->entity());
  };

  auto keyEntities = std::vector<const mdl::EntityNodeBase*>{&worldNode};
  std::ranges::copy(
    entities | std::views::filter(isOfKeyClass), std::back_inserter(keyEntities));
  std::ranges::copy(
    entities | std::views::filter(isLinked), std::back_inserter(keyEntities));

  return capped(keyEntities, MaxKeyEntities, [&](const auto* entityNode) {
    return boundedEntityToJson(*entityNode, handles);
  });
}

/** The given keys plus the keys of the given property definitions. */
std::vector<std::string> withDefinitionKeys(
  std::vector<std::string> keys,
  const std::vector<const mdl::PropertyDefinition*>& definitions)
{
  for (const auto* definition : definitions)
  {
    if (!kdl::ranges::contains(keys, definition->key))
    {
      keys.push_back(definition->key);
    }
  }
  return keys;
}

std::vector<std::string> linkSourceKeys(const mdl::EntityNodeBase& entityNode)
{
  return withDefinitionKeys(
    {mdl::EntityPropertyKeys::Target, mdl::EntityPropertyKeys::Killtarget},
    mdl::getLinkSourcePropertyDefinitions(entityNode.entity().definition()));
}

std::vector<std::string> linkTargetKeys(const mdl::EntityNodeBase& entityNode)
{
  return withDefinitionKeys(
    {mdl::EntityPropertyKeys::Targetname},
    mdl::getLinkTargetPropertyDefinitions(entityNode.entity().definition()));
}

/**
 * The properties of the given entity with non-empty values whose keys are one of the
 * given keys or a numbered variant of one (`target2`), in file order.
 */
std::vector<const mdl::EntityProperty*> linkProperties(
  const mdl::EntityNodeBase& entityNode, const std::vector<std::string>& keys)
{
  auto result = std::vector<const mdl::EntityProperty*>{};
  for (const auto& property : entityNode.entity().properties())
  {
    if (!property.value().empty() && std::ranges::any_of(keys, [&](const auto& key) {
          return mdl::isNumberedProperty(key, property.key());
        }))
    {
      result.push_back(&property);
    }
  }
  return result;
}

/**
 * The edges from link source properties (`target`, `killtarget` and the keys the
 * entity definitions declare as link sources) to the entities whose link target
 * properties (`targetname` and the declared link targets) have that value, in file
 * order. Sources whose value names no entity are dangling.
 */
Json linkGraph(
  const std::vector<const mdl::EntityNode*>& entities, NodeHandleTable* handles)
{
  auto entitiesByName =
    std::unordered_map<std::string, std::vector<const mdl::EntityNode*>>{};
  for (const auto* entityNode : entities)
  {
    for (const auto* property : linkProperties(*entityNode, linkTargetKeys(*entityNode)))
    {
      auto& named = entitiesByName[property->value()];
      if (!kdl::ranges::contains(named, entityNode))
      {
        named.push_back(entityNode);
      }
    }
  }

  struct Link
  {
    const mdl::EntityNode* from;
    const mdl::EntityProperty* property;
    const mdl::EntityNode* to;
  };

  auto links = std::vector<Link>{};
  auto dangling = std::vector<Link>{};
  for (const auto* entityNode : entities)
  {
    for (const auto* property : linkProperties(*entityNode, linkSourceKeys(*entityNode)))
    {
      if (
        const auto targets = entitiesByName.find(property->value());
        targets != entitiesByName.end())
      {
        for (const auto* target : targets->second)
        {
          links.push_back({entityNode, property, target});
        }
      }
      else
      {
        dangling.push_back({entityNode, property, nullptr});
      }
    }
  }

  auto result = Json::object();
  result.set("edges", capped(links, MaxLinks, [&](const auto& link) {
               auto edge = Json::object();
               setNodePath(edge, "from", *link.from, handles);
               setNodePath(edge, "to", *link.to, handles);
               edge.set("key", link.property->key());
               return edge;
             }));
  result.set("dangling", capped(dangling, MaxDanglingLinks, [&](const auto& link) {
               auto edge = Json::object();
               setNodePath(edge, "from", *link.from, handles);
               edge.set("key", link.property->key());
               edge.set("target", link.property->value());
               return edge;
             }));
  return result;
}

} // namespace

const std::vector<std::string>& keyEntityClasses(
  const std::optional<std::vector<std::string>>& configured)
{
  return configured ? *configured : DefaultKeyEntityClasses;
}

bool hasLink(const mdl::Entity& entity)
{
  return entity.hasProperty(mdl::EntityPropertyKeys::Targetname)
         || entity.hasProperty(mdl::EntityPropertyKeys::Target);
}

bool isKeyEntity(const mdl::Entity& entity, const std::vector<std::string>& classes)
{
  return kdl::ranges::contains(classes, entity.classname()) || hasLink(entity);
}

Json describe(
  const MapRef& mapRef,
  const std::optional<std::vector<std::string>>& keyEntityClasses,
  const IssueReport& issueReport)
{
  const auto& map = mapRef.map;
  const auto& worldNode = map.worldNode();
  const auto contents = collectContents(worldNode);
  const auto summary = summarize(contents);
  const auto linkSets = collectLinkSets(contents.groups);

  auto result = Json::object();
  result.set("schema", "tbcli.describe/1");
  result.set("rev", mapRef.rev);
  result.set("counts", countsToJson(summary));
  result.set("bounds", boundsToJson(summary));
  result.set("layers", TreeWriter{linkSets, mapRef.handles}.write(worldNode));
  result.set("entity_classes", entityClasses(contents.entities));
  result.set("textures", textures(contents.brushes, !map.gamePath().empty()));
  result.set(
    "key_entities",
    keyEntities(
      worldNode,
      contents.entities,
      cli::keyEntityClasses(keyEntityClasses),
      mapRef.handles));
  result.set("link_graph", linkGraph(contents.entities, mapRef.handles));
  result.set(
    "built_rooms",
    capped(collectBuiltRooms(worldNode), MaxBuiltRooms, [&](const auto& room) {
      return builtRoomToJson(room, mapRef.handles);
    }));
  auto issues = Json::object();
  setIssueCounts(issues, issueReport.issues);
  result.set("issues", std::move(issues));
  return result;
}

} // namespace tb::cli
