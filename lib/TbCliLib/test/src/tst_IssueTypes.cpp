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

#include "CliTestUtils.h"
#include "base/Logger.h"
#include "cli/CliValidators.h"
#include "cli/IssueTypes.h"
#include "mdl/Map.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{

TEST_CASE("IssueTypes")
{
  SECTION("Severity")
  {
    CHECK(Severity::Info < Severity::Warning);
    CHECK(Severity::Warning < Severity::Error);
  }

  SECTION("toString")
  {
    CHECK(toString(Severity::Error) == "error");
    CHECK(toString(Severity::Warning) == "warning");
    CHECK(toString(Severity::Info) == "info");
  }

  SECTION("parseSeverity")
  {
    CHECK(parseSeverity("error") == Severity::Error);
    CHECK(parseSeverity("warning") == Severity::Warning);
    CHECK(parseSeverity("info") == Severity::Info);
    CHECK(parseSeverity("Error") == std::nullopt);
  }

  SECTION("issueTypes")
  {
    using enum Severity;
    auto table = std::vector<std::pair<std::string, Severity>>{};
    for (const auto& type : issueTypes())
    {
      table.emplace_back(type.id, type.severity);
    }

    // the spec's ids and default severities
    CHECK(
      table
      == std::vector<std::pair<std::string, Severity>>{
        {"load_error", Error},
        {"missing_classname", Error},
        {"missing_definition", Warning},
        {"missing_mod", Warning},
        {"empty_group", Warning},
        {"empty_brush_entity", Warning},
        {"point_entity_with_brushes", Warning},
        {"link_source", Warning},
        {"link_target", Warning},
        {"non_integer_vertices", Info},
        {"mixed_brush_contents", Warning},
        {"world_bounds", Error},
        {"soft_map_bounds", Info},
        {"empty_property_key", Warning},
        {"empty_property_value", Warning},
        {"long_property_key", Warning},
        {"long_property_value", Warning},
        {"property_key_with_double_quotation_marks", Warning},
        {"property_value_with_double_quotation_marks", Warning},
        {"world_node_path_separator", Warning},
        {"invalid_uv_scale", Warning},
        {"missing_material", Warning},
        {"fgd_property_type", Warning},
        {"entity_inside_solid", Error},
        {"no_player_start", Error},
        {"degenerate_brush", Warning},
      });

    // only a missing player start is about the whole map
    for (const auto& type : issueTypes())
    {
      CAPTURE(type.id);
      CHECK(
        type.scope
        == (type.id == "no_player_start" ? IssueScope::Map : IssueScope::Node));
      // and only CLI check types create their validators
      CHECK((type.createValidator != nullptr) == (type.source == IssueSource::CliCheck));
    }
  }

  SECTION("findIssueType by id")
  {
    REQUIRE(findIssueType("empty_group") != nullptr);
    CHECK(findIssueType("empty_group")->id == "empty_group");
    CHECK(findIssueType("no_such_type") == nullptr);
  }

  SECTION("findIssueType by validator")
  {
    // every registered validator has exactly one entry
    auto logger = NullLogger{};
    const auto loadedMap = loadFixtureMap("maps/simple.map", logger);
    const auto validators = loadedMap.map->worldNode().registeredValidators();
    REQUIRE(!validators.empty());

    auto ids = std::set<std::string_view>{};
    for (const auto* validator : validators)
    {
      CAPTURE(validator->description());
      const auto* type = findIssueType(*validator);
      REQUIRE(type != nullptr);
      CHECK(type->source == IssueSource::Validator);
      ids.insert(type->id);
    }
    CHECK(ids.size() == validators.size());

    // and the table has no validator entry that the map does not register
    CHECK(
      size_t(
        std::ranges::count(issueTypes(), IssueSource::Validator, &IssueTypeDef::source))
      == validators.size());
  }

  SECTION("findIssueType by CLI validator")
  {
    // every CLI validator has exactly one entry, and the table has no other
    const auto validators = makeCliValidators(MaterialCheck::On);
    auto ids = std::set<std::string_view>{};
    for (const auto& validator : validators)
    {
      CAPTURE(validator->description());
      const auto* type = findIssueType(*validator);
      REQUIRE(type != nullptr);
      CHECK(type->source == IssueSource::CliCheck);
      ids.insert(type->id);
    }
    CHECK(ids.size() == validators.size());
    CHECK(
      size_t(
        std::ranges::count(issueTypes(), IssueSource::CliCheck, &IssueTypeDef::source))
      == validators.size());
  }

  SECTION("fixIdsJson")
  {
    // no quick-fix has an id yet
    CHECK(fixIdsJson(*findIssueType("empty_group")) == Json::array());
  }

  SECTION("issueTypesJson")
  {
    const auto json = issueTypesJson();
    REQUIRE(json.size() == issueTypes().size());
    CHECK(
      json[1]
      == parseJsonLiteral(
        R"({"type": "missing_classname", "severity": "error", "fixes": []})"));
  }
}

} // namespace tb::cli
