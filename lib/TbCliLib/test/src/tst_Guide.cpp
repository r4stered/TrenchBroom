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
#include "FakeEditorHost.h"
#include "cli/CliError.h"
#include "cli/Compile.h"
#include "cli/FileUtils.h"
#include "cli/Guide.h"
#include "cli/IssueTypes.h"
#include "cli/Ops.h"
#include "cli/View.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Selection.h"
#include "mdl/WorldNode.h"

#include "kd/string_utils.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::cli
{
using namespace Catch::Matchers;

namespace
{

/** The guide without a topic and every topic. */
std::vector<std::string> wholeGuide()
{
  auto result = std::vector<std::string>{guideText(std::nullopt)};
  for (const auto topic : guideTopics())
  {
    result.push_back(guideText(topic));
  }
  return result;
}

size_t lineCount(const std::string& text)
{
  return size_t(std::ranges::count(text, '\n'));
}

/** What a guide example needs to run. */
struct ExampleSetup
{
  std::string_view name;
  /** The fixture map that the example starts from, or empty for an empty map. */
  std::string_view map;
  /** The qbsp recording that the example's inspect commands replay, if any. */
  std::string_view inspectRecording;
  /**
   * The qbsp recording that the example's check commands replay, if any. The recording
   * was made from compiled/<recording>.map, which must be the map the batch writes.
   */
  std::string_view checkRecording;
};

constexpr auto ExampleSetups = std::array{
  ExampleSetup{"retexture", "compiled/rooms_prt1.map", "", ""},
  ExampleSetup{"doorway", "compiled/rooms_prt1.map", "", "guide_doorway"},
  ExampleSetup{"leak", "compiled/compile_leak.map", "compile_leak", "guide_leak_fixed"},
  ExampleSetup{"rooms", "", "", "connected_rooms"},
};

/** What the mapper does in the editor while a live example runs. */
using MapperAction = void (*)(FakeEditorHost&);

/** What a guide example of a live session needs to run. */
struct LiveExampleSetup
{
  std::string_view name;
  /** The fixture map that the editor has open. */
  std::string_view map;
  /** What the mapper does before the example's inspect commands, if anything. */
  MapperAction beforeInspect = nullptr;
  /** What the mapper does between the inspect commands and the batch, if anything. */
  MapperAction beforeApply = nullptr;
};

/** The first entity of the given class in the default layer of the editor's map. */
mdl::Node* entityOfClass(FakeEditorHost& editor, const std::string_view classname)
{
  for (auto* node : editor.map().worldNode().defaultLayer()->children())
  {
    if (
      const auto* entityNode = dynamic_cast<mdl::EntityNode*>(node);
      entityNode && entityNode->entity().classname() == classname)
    {
      return node;
    }
  }
  FAIL("the map has no " << classname);
  return nullptr;
}

constexpr auto LiveExampleSetups = std::array{
  LiveExampleSetup{
    "live_handles",
    "compiled/rooms_prt1.map",
    nullptr,
    // the mapper moves the player start
    [](FakeEditorHost& editor) {
      auto& map = editor.map();
      mdl::selectNodes(map, {entityOfClass(editor, "info_player_start")});
      mdl::translateSelection(map, {0, 32, 0});
      mdl::deselectAll(map);
    },
  },
  LiveExampleSetup{
    "live_shown",
    "compiled/rooms_prt1.map",
    // the mapper points at the light and shows the agent the view
    [](FakeEditorHost& editor) {
      editor.show(
        ViewCapture{
          .png = "PNG of the shown view",
          .image = {1024, 768},
          .camera =
            CameraPose{
              .position = {-128, 0, 48},
              .direction = {1, 0, 0},
              .viewport = {1024, 768},
            },
          .hovered =
            HoveredPick{entityOfClass(editor, "light"), std::nullopt, {144, 0, 96}},
          .cursor = ImagePoint{512, 293},
        });
    },
    nullptr,
  },
};

/** The words of a command line, where single quotes group words, as in a shell. */
std::vector<std::string> splitCommand(const std::string_view command)
{
  auto result = std::vector<std::string>{};
  auto word = std::optional<std::string>{};
  auto quoted = false;
  for (const auto c : command)
  {
    if (c == '\'')
    {
      quoted = !quoted;
      word = word.value_or("");
    }
    else if (c == ' ' && !quoted)
    {
      if (word)
      {
        result.push_back(*word);
        word.reset();
      }
    }
    else
    {
      word = word.value_or("") + c;
    }
  }
  REQUIRE(!quoted);
  if (word)
  {
    result.push_back(*word);
  }
  return result;
}

/**
 * Whether the actual JSON has everything the expected JSON has: an object may have more
 * members than expected, but an array must have as many elements.
 */
bool containsJson(const Json& actual, const Json& expected)
{
  if (expected.isObject())
  {
    return actual.isObject()
           && std::ranges::all_of(expected.asObject(), [&](const auto& member) {
                const auto* value = actual.find(member.key);
                return value && containsJson(*value, member.value);
              });
  }
  if (expected.isArray())
  {
    return actual.isArray() && actual.size() == expected.size()
           && std::ranges::all_of(
             std::views::iota(size_t(0), expected.size()),
             [&](const auto i) { return containsJson(actual[i], expected[i]); });
  }
  return actual == expected;
}

/** The JSON text of an example with its rev placeholders replaced. */
Json withRevs(
  const std::string_view text, const std::string& rev, const std::string& newRev)
{
  return parseJsonLiteral(
    kdl::str_replace_every(
      kdl::str_replace_every(std::string{text}, "\"<rev>\"", fmt::format("\"{}\"", rev)),
      "\"<new rev>\"",
      fmt::format("\"{}\"", newRev)));
}

/** The text of a live example with the placeholder `<epoch>` of its handles replaced. */
std::string withEpoch(const std::string_view text, const FakeEditorHost* editor)
{
  return editor ? kdl::str_replace_every(std::string{text}, "<epoch>", editor->epoch())
                : std::string{text};
}

/** Lets verify --compile replay the given qbsp recording. */
void useQbspRecording(CliTestEnvironment& env, const std::string_view recording)
{
  auto compilers = Json::object();
  compilers.set(
    std::string{QbspTool}, arg(setUpFakeCompiler(env, QbspTool, std::string{recording})));
  auto config = quakeConfig();
  config.set("compilers", std::move(compilers));
  env.writeConfig({}, config);
}

/**
 * Runs a command of an example on the map and checks that it prints what it shows. With
 * an editor, the command runs in the editor's live session.
 */
void checkCommand(
  const GuideCommand& command,
  const std::string& map,
  const std::string& rev,
  const std::string& newRev,
  FakeEditorHost* editor = nullptr)
{
  CAPTURE(command.command);

  auto args = splitCommand(withEpoch(command.command, editor));
  REQUIRE(args.front() == "tbcli");
  args.erase(args.begin());
  for (auto& word : args)
  {
    word = word == "<map>" ? map : word == "<rev>" ? rev : word;
  }

  const auto actual = editor ? parseOutput(editor->run(args)) : runJson(args);
  const auto expected = withRevs(withEpoch(command.output, editor), rev, newRev);
  CAPTURE(actual, expected);
  CHECK((command.excerpt ? containsJson(actual, expected) : actual == expected));
}

/**
 * Runs a live example in an editor that has its map open, with the mapper's actions in
 * between, and checks that tbcli prints what the example shows.
 */
void checkLiveExample(const GuideExample& example)
{
  const auto setup =
    std::ranges::find(LiveExampleSetups, example.name, &LiveExampleSetup::name);
  REQUIRE(setup != LiveExampleSetups.end());

  // no tbcli.json: a live session doesn't need one
  auto env = CliTestEnvironment{};
  const auto mapPath = env.copyFixture(setup->map, "example.map");
  const auto map = arg(mapPath);
  auto editor = FakeEditorHost{mapPath};

  if (setup->beforeInspect)
  {
    setup->beforeInspect(editor);
  }
  const auto rev = editor.rev();
  for (const auto& command : example.inspect)
  {
    checkCommand(command, map, rev, "", &editor);
  }

  if (setup->beforeApply)
  {
    setup->beforeApply(editor);
  }
  const auto applied = editor.run(
    {"apply", map, "-", "--expect-rev", rev}, withEpoch(example.batch, &editor));
  INFO(applied.out);
  REQUIRE(applied.exitCode == 0);
  const auto result = parseOutput(applied);
  const auto newRev = result["rev"].asString();
  CHECK(newRev != rev);
  CHECK(result == withRevs(withEpoch(example.result, &editor), rev, newRev));

  for (const auto& command : example.check)
  {
    checkCommand(command, map, rev, newRev, &editor);
  }

  // the map's file is never written
  CHECK(env.readFile("example.map") == *readFile(fixturePath(setup->map)));
}

} // namespace

TEST_CASE("Guide")
{
  SECTION("guideTopics")
  {
    CHECK(
      std::vector<std::string_view>{guideTopics().begin(), guideTopics().end()}
      == std::vector<std::string_view>{
        "conventions", "targeting", "verify", "generate", "examples", "live"});
  }

  SECTION("guideExamples")
  {
    // every setup belongs to an example
    auto setupNames = std::vector<std::string_view>{};
    std::ranges::transform(
      ExampleSetups, std::back_inserter(setupNames), &ExampleSetup::name);
    std::ranges::transform(
      LiveExampleSetups, std::back_inserter(setupNames), &LiveExampleSetup::name);
    for (const auto name : setupNames)
    {
      CAPTURE(name);
      CHECK(
        std::ranges::find(guideExamples(), name, &GuideExample::name)
        != guideExamples().end());
    }

    const auto example = GENERATE(from_range(guideExamples()));
    CAPTURE(example.name);
    CHECK(std::ranges::find(guideTopics(), example.topic) != guideTopics().end());

    // the topic shows the example
    const auto text = guideText(example.topic);
    CHECK_THAT(text, ContainsSubstring(fmt::format("## {}\n", example.title)));
    CHECK_THAT(text, ContainsSubstring(std::string{example.batch}));
    CHECK_THAT(text, ContainsSubstring(std::string{example.result}));

    if (example.topic == "live")
    {
      checkLiveExample(example);
      return;
    }

    // the example runs and tbcli prints what it shows
    const auto setup =
      std::ranges::find(ExampleSetups, example.name, &ExampleSetup::name);
    REQUIRE(setup != ExampleSetups.end());

    auto env = CliTestEnvironment{};
    env.writeConfig({}, quakeConfig());
    const auto map = arg(
      setup->map.empty() ? env.writeFile("example.map", EmptyMap)
                         : env.copyFixture(setup->map, "example.map"));
    const auto rev = revOf(map);

    if (!setup->inspectRecording.empty())
    {
      useQbspRecording(env, setup->inspectRecording);
    }
    for (const auto& command : example.inspect)
    {
      checkCommand(command, map, rev, "");
    }

    const auto result = runJson({"apply", map, "-", "--expect-rev", rev}, example.batch);
    const auto newRev = result["rev"].asString();
    CHECK(newRev != rev);
    CHECK(result == withRevs(example.result, rev, newRev));

    if (!setup->checkRecording.empty())
    {
      const auto recorded =
        *readFile(fixturePath(fmt::format("compiled/{}.map", setup->checkRecording)));
      CHECK(
        withoutVolatileLines(env.readFile("example.map"))
        == withoutVolatileLines(recorded));
      useQbspRecording(env, setup->checkRecording);
    }
    for (const auto& command : example.check)
    {
      checkCommand(command, map, rev, newRev);
    }
  }

  SECTION("guideTopicReadsConfig")
  {
    CHECK(guideTopicReadsConfig("conventions"));
    CHECK(!guideTopicReadsConfig("targeting"));
    CHECK(!guideTopicReadsConfig("verify"));
  }

  SECTION("guideText")
  {
    SECTION("without a topic, the loop, the rules and the topic index")
    {
      const auto text = guideText(std::nullopt);

      // the loop, in order, each step with a done condition
      const auto steps = std::vector<std::string_view>{
        "tbcli describe <map>",
        "tbcli query <map>",
        "--dry-run",
        "the same command without `--dry-run`",
        "`issues_introduced`",
        "tbcli verify <map>`",
        "tbcli verify <map> --compile",
        "until `verify --compile` is `ok`",
      };
      auto position = size_t(0);
      for (const auto step : steps)
      {
        CAPTURE(step);
        const auto found = text.find(step, position);
        REQUIRE(found != std::string::npos);
        position = found;
      }
      // every step but the last, which repeats them, says when it is done
      for (size_t step = 1; step < 7; ++step)
      {
        CAPTURE(step);
        const auto start = text.find(fmt::format("{}. **", step));
        const auto end = text.find(fmt::format("{}. **", step + 1));
        REQUIRE(start != std::string::npos);
        REQUIRE(end != std::string::npos);
        CHECK_THAT(text.substr(start, end - start), ContainsSubstring("Done when"));
      }

      // the rules
      CHECK_THAT(text, ContainsSubstring("Paths die with the rev"));
      CHECK_THAT(text, ContainsSubstring("Always pass `--expect-rev <rev>`"));
      CHECK_THAT(text, ContainsSubstring("Set `expect` on query targets"));
      CHECK_THAT(text, ContainsSubstring("A lossy load"));
      CHECK_THAT(text, ContainsSubstring("Sealed means done"));
      CHECK_THAT(text, ContainsSubstring(R"(classname is "light")"));

      // the topic index
      for (const auto topic : guideTopics())
      {
        CHECK_THAT(text, ContainsSubstring(fmt::format("- `{}`: ", topic)));
      }

      // the op index is generated from the op definitions
      for (const auto& def : opDefs())
      {
        CHECK_THAT(text, ContainsSubstring(fmt::format("`{}`", def.name)));
      }
    }

    SECTION("every topic loads")
    {
      const auto topic = GENERATE(from_range(guideTopics()));
      CAPTURE(topic);

      const auto text = guideText(topic);
      CHECK(text.starts_with("# "));
      CHECK(lineCount(text) > 2);
    }

    SECTION("the guide points at tbcli ops --json instead of copying it")
    {
      CHECK_THAT(guideText(std::nullopt), ContainsSubstring("`tbcli ops --json`"));

      for (const auto& text : wholeGuide())
      {
        // no fix ids, except those that are also op names
        for (const auto& type : issueTypes())
        {
          for (const auto& fix : type.fixes)
          {
            if (!findOpDef(fix.id))
            {
              CHECK_THAT(text, !ContainsSubstring(std::string{fix.id}));
            }
          }
        }

        // no op descriptions, which are part of the schemas
        for (const auto& def : opDefs())
        {
          CHECK_THAT(text, !ContainsSubstring(std::string{def.description}));
        }

        // no JSON Schema
        CHECK_THAT(text, !ContainsSubstring("\"properties\""));
      }

      // the issue table is not copied: most issue types go unmentioned
      const auto mentioned = std::ranges::count_if(issueTypes(), [](const auto& type) {
        return std::ranges::any_of(wholeGuide(), [&](const auto& text) {
          return text.find(fmt::format("`{}`", type.id)) != std::string::npos;
        });
      });
      CHECK(size_t(mentioned) < issueTypes().size() / 2);
    }

    SECTION("conventions shows the Quake 1 defaults")
    {
      const auto text = guideText("conventions");
      CHECK_THAT(text, ContainsSubstring("| Player hull | 32 x 32 x 56 |"));
      CHECK_THAT(text, ContainsSubstring("| Step height | 18 |"));
      CHECK_THAT(text, ContainsSubstring("| Door | 64 x 96 |"));
      CHECK_THAT(text, ContainsSubstring("`guide.conventions`"));
    }

    SECTION("conventions from tbcli.json replace the defaults")
    {
      const auto text = guideText(
        "conventions",
        GuideConfig{std::vector<Convention>{
          {"Player hull", "24 x 24 x 48"},
          {"Choice", "a | b"},
        }});

      CHECK_THAT(text, ContainsSubstring("| Player hull | 24 x 24 x 48 |"));
      CHECK_THAT(text, ContainsSubstring(R"(| Choice | a \| b |)"));
      CHECK_THAT(text, !ContainsSubstring("Step height"));
      CHECK_THAT(text, ContainsSubstring("project's tbcli.json"));
      // the notes on the default rows are left out
      CHECK_THAT(text, !ContainsSubstring("jump height"));
    }

    SECTION("targeting lists the node kinds and face directions tbcli accepts")
    {
      const auto text = guideText("targeting");
      CHECK_THAT(text, ContainsSubstring("`point_entity`"));
      CHECK_THAT(text, ContainsSubstring("`brush_entity`"));
      CHECK_THAT(text, ContainsSubstring("`+x`"));
      CHECK_THAT(text, ContainsSubstring("`-z`"));
    }

    SECTION("live covers what a live session changes for the agent")
    {
      const auto text = guideText("live");
      for (const auto* term : {
             "`session`",
             "live-<epoch>:<counter>",
             "n:<epoch>.<k>",
             "`<key>_handle`",
             "`<key>_handles`",
             "Target handles, not paths",
             "`gone`",
             "`stale_session`",
             "`stale_rev`",
             "`conflicts`",
             "`ancestor_changed`",
             "tbcli changes <map> --since <rev>",
             "`too_old`",
             "`busy`",
             "tbcli undo <map>",
             "`undo_refused`",
             "tbcli save <map>",
             "`write_failed`",
             "`\"stale\": true`",
             "tbcli.json",
             "tbcli view <map>",
             "--out <file.png>",
             "[left, top, right, bottom]",
             "`view_failed`",
             "`shown`",
             "Ctrl+Alt+S",
             "--look-at <path | handle>",
             "--orbit yaw,pitch[,distance]",
             "--size WxH",
             "`session_ended`",
             "`link_disabled`",
           })
      {
        CAPTURE(term);
        CHECK_THAT(text, ContainsSubstring(term));
      }
    }

    SECTION("an unknown topic is a usage error")
    {
      CHECK_THROWS_MATCHES(
        guideText("frobnicate"),
        CliError,
        MessageMatches(ContainsSubstring("conventions, targeting")));
    }
  }

  SECTION("installGuide")
  {
    auto env = CliTestEnvironment{};
    const auto dir = env.path("repo/.claude/skills/tbcli");

    const auto result = installGuide(dir);
    CHECK(result["schema"] == Json{"tbcli.guide_install/1"});
    CHECK(result["skill"] == Json{arg((dir / "SKILL.md").lexically_normal())});
    CHECK(
      result["agents_snippet"]
      == Json{arg((dir / "AGENTS.snippet.md").lexically_normal())});

    const auto skill = env.readFile("repo/.claude/skills/tbcli/SKILL.md");
    CHECK(skill.starts_with("---\nname: tbcli\n"));
    CHECK_THAT(
      skill,
      ContainsSubstring(
        "description: \"Reading, editing, generating or verifying Quake `.map` files "
        "with tbcli."));
    CHECK_THAT(skill, ContainsSubstring("`tbcli guide`"));
    CHECK_THAT(skill, ContainsSubstring("If an output has `shown`, run `tbcli view"));
    CHECK(lineCount(skill) < 15);

    const auto snippet = env.readFile("repo/.claude/skills/tbcli/AGENTS.snippet.md");
    CHECK_THAT(snippet, ContainsSubstring("`tbcli guide`"));
    CHECK(lineCount(snippet) < 10);

    // both only point at the guide
    for (const auto& text : {skill, snippet})
    {
      CHECK_THAT(text, !ContainsSubstring("ops --json"));
      CHECK_THAT(text, !ContainsSubstring("--expect-rev"));
    }

    SECTION("replaces earlier copies")
    {
      env.writeFile("repo/.claude/skills/tbcli/SKILL.md", "old");
      installGuide(dir);
      CHECK(env.readFile("repo/.claude/skills/tbcli/SKILL.md").starts_with("---"));
    }

    SECTION("fails if the directory cannot be created")
    {
      const auto file = env.writeFile("file", "");
      CHECK_THROWS_AS(installGuide(file / "dir"), CliError);
    }
  }
}

} // namespace tb::cli
