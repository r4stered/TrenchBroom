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

#pragma once

#include "cli/Config.h"
#include "cli/Json.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace tb::cli
{

/** The topics `tbcli guide <topic>` loads, in the order the guide's index lists them. */
std::span<const std::string_view> guideTopics();

/** Whether the given guide topic reads tbcli.json. */
bool guideTopicReadsConfig(std::string_view topic);

/**
 * The agent-facing guide as Markdown: without a topic, the edit and verify loop, the
 * rules and the topic index; otherwise the given topic. The `conventions` topic shows
 * `conventions` from tbcli.json if it is set, and the Quake 1 defaults otherwise.
 *
 * The guide never copies op schemas, the issue type table or fix ids; it points at
 * `tbcli ops --json` for them.
 *
 * @throw CliError `usage` if the topic is unknown
 */
std::string guideText(
  std::optional<std::string_view> topic, const GuideConfig& config = {});

/** The file names `tbcli guide --install` writes into its directory. */
inline constexpr auto GuideSkillFileName = "SKILL.md";
inline constexpr auto GuideAgentsSnippetFileName = "AGENTS.snippet.md";

/**
 * Writes the SKILL.md and the agents-file snippet into the given directory, creating
 * it if needed and replacing earlier copies. Both only point at `tbcli guide`, so that
 * they do not go stale when tbcli changes.
 *
 * @return the `tbcli.guide_install/1` result naming the written files
 * @throw CliError `write_failed` if the directory or a file cannot be written
 */
Json installGuide(const std::filesystem::path& dir);

} // namespace tb::cli
