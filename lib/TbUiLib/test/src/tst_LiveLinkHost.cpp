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

#include <QCoreApplication>

#include "base/Logger.h"
#include "cli/Json.h"
#include "cli/LiveLink.h"
#include "fs/TestEnvironment.h"
#include "mdl/EntityNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Selection.h"
#include "mdl/TransactionScope.h"
#include "mdl/WorldNode.h"
#include "ui/LiveLinkHost.h"
#include "ui/MapDocument.h"
#include "ui/MapDocumentFixture.h"

#include <chrono>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::ui
{
using namespace std::chrono_literals;
using namespace Catch::Matchers;

namespace
{

constexpr auto MapText = R"(// Game: Quake
// Format: Valve
// entity 0
{
"mapversion" "220"
"classname" "worldspawn"
}
// entity 1
{
"classname" "light"
"origin" "0 0 64"
}
)";

/** Collects what the Console would show, without debug messages. */
class ConsoleLogger : public Logger
{
public:
  std::vector<std::string> messages;

private:
  void doLog(const LogLevel level, const std::string_view message) override
  {
    if (level != LogLevel::Debug)
    {
      messages.emplace_back(message);
    }
  }
};

/** Runs the Qt event loop for the given time. */
void processEventsFor(const std::chrono::milliseconds duration)
{
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline)
  {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  }
}

/** Runs the Qt event loop until the predicate holds. */
template <typename P>
void processEventsUntil(const P& predicate)
{
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  while (!predicate() && std::chrono::steady_clock::now() < deadline)
  {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  }
  REQUIRE(predicate());
}

/** Runs the Qt event loop until the future is ready, as the editor would. */
template <typename T>
T await(std::future<T>& future)
{
  processEventsUntil([&]() { return future.wait_for(0ms) == std::future_status::ready; });
  return future.get();
}

cli::Json parse(const cli::RunResult& result)
{
  INFO(result.out);
  INFO(result.err);
  auto json = cli::parseJson(result.out);
  REQUIRE(json.is_success());
  return std::move(json).value();
}

void checkError(const cli::RunResult& result, const std::string& code)
{
  CHECK(result.exitCode != 0);
  CHECK(parse(result)["error"]["code"] == cli::Json{code});
}

size_t revCounter(const cli::Json& output)
{
  const auto& rev = output["rev"].asString();
  return std::stoul(rev.substr(rev.find(':') + 1));
}

} // namespace

TEST_CASE("LiveLinkHost")
{
  if (!cli::HasLinkClient)
  {
    SKIP("tbcli has no live link client on this platform yet");
  }

  auto env = fs::TestEnvironment{};
  env.createFile("live.map", MapText);
  const auto mapPath = env.dir() / "live.map";
  const auto rendezvousDir = env.dir() / "live";

  auto fixture = MapDocumentFixture{};
  auto& document = fixture.load(mapPath, mdl::QuakeFixtureConfig);
  auto& map = document.map();
  // as in the editor
  map.setIsCommandCollationEnabled(true);

  auto logger = ConsoleLogger{};
  document.setTargetLogger(&logger);

  auto host = std::make_unique<LiveLinkHost>(document, rendezvousDir);
  // what loading the map logged
  logger.messages.clear();

  // tbcli on another thread, as another process would be
  const auto tbcli = [&](
                       std::vector<std::string> args,
                       std::string in = {},
                       const std::filesystem::path& dir = {}) {
    return std::async(std::launch::async, [=]() {
      return cli::runLive(
        args, in, cli::LinkMode::Live, dir.empty() ? rendezvousDir : dir);
    });
  };
  const auto run = [&](
                     std::vector<std::string> args,
                     std::string in = {},
                     const std::filesystem::path& dir = {}) {
    auto future = tbcli(std::move(args), std::move(in), dir);
    const auto result = await(future);
    REQUIRE(result.has_value());
    return *result;
  };

  const auto mapArg = mapPath.string();
  const auto createBox = std::string{
    R"([{"op": "create_box", "min": [0, 0, 0], "max": [64, 32, 16], "material": "x"}])"};

  SECTION("publishes a rendezvous file while the map is open")
  {
    REQUIRE(host->rendezvousFile().has_value());
    const auto rendezvous = cli::readRendezvous(*host->rendezvousFile());
    REQUIRE(rendezvous.has_value());
    CHECK(rendezvous->proto == cli::LinkProto);
    CHECK(rendezvous->map == mapArg);
    CHECK(rendezvous->pid == QCoreApplication::applicationPid());
    CHECK(rendezvous->token.size() >= 32);
    CHECK_THAT(rendezvous->endpoint, ContainsSubstring("TrenchBroom-live-"));
#ifndef _WIN32
    // the token is a secret, so other users can't look inside the rendezvous dir
    CHECK(
      (std::filesystem::status(rendezvousDir).permissions() & std::filesystem::perms::all)
      == std::filesystem::perms::owner_all);
#endif

    const auto file = *host->rendezvousFile();
    host.reset();
    CHECK(!std::filesystem::exists(file));
  }

  SECTION("tbcli runs commands against the open map")
  {
    const auto described = parse(run({"describe", mapArg}));
    CHECK(described["rev"] == cli::Json{host->rev()});
    CHECK(described["session"]["live"] == cli::Json{true});

    const auto applied = parse(run({"apply", mapArg, "-"}, createBox));
    CHECK(applied["session"] == cli::Json{"live"});
    CHECK(revCounter(applied) > revCounter(described));
    CHECK(map.worldNode().defaultLayer()->childCount() == 2);
    CHECK(env.loadFile("live.map") == MapText);

    REQUIRE(map.undoCommandName() != nullptr);
    CHECK(*map.undoCommandName() == "AI: create_box");

    CHECK(logger.messages.size() == 1);
    CHECK_THAT(logger.messages.back(), StartsWith("AI: apply done (live-"));
  }

  SECTION("reads write no Console line, failures do")
  {
    run({"describe", mapArg});
    CHECK(logger.messages.empty());

    run({"apply", mapArg, "-"}, R"([{"op": "frobnicate"}])");
    CHECK(logger.messages.size() == 1);
    CHECK_THAT(logger.messages.back(), StartsWith("AI: apply failed: invalid_argument"));
  }

  SECTION("a mapper command right after a batch does not merge into it")
  {
    auto* light = map.worldNode().defaultLayer()->children().front();
    mdl::selectNodes(map, {light});
    run({"translate", mapArg, "--target", "layer:0/entity:0", "--delta", "0 0 8"});

    // the mapper drags the light, which would merge with an earlier translate
    mdl::translateSelection(map, {0, 0, 8});
    CHECK(light->logicalBounds().center().z() == 80);

    map.undoCommand();
    CHECK(light->logicalBounds().center().z() == 72);
    REQUIRE(map.undoCommandName() != nullptr);
    CHECK(*map.undoCommandName() == "AI: translate");
    map.undoCommand();
    CHECK(light->logicalBounds().center().z() == 64);
  }

  SECTION("request and reply payloads are binary-safe")
  {
    // a property value in Latin-1, which isn't valid UTF-8
    const auto value = std::string{"caf\xe9"};
    run(
      {"apply", mapArg, "-"},
      R"([{"op": "set_property", "target": "layer:0/entity:0", "key": "note", "value": ")"
        + value + R"("}])");

    auto* light =
      dynamic_cast<mdl::EntityNode*>(map.worldNode().defaultLayer()->children().front());
    REQUIRE(light != nullptr);
    CHECK(*light->entity().property("note") == value);

    const auto shown = run({"show", mapArg, "layer:0/entity:0"});
    CHECK_THAT(shown.out, ContainsSubstring(value));
  }

  SECTION("a wrong token gives bad_token")
  {
    auto rendezvous = *cli::readRendezvous(*host->rendezvousFile());
    rendezvous.token = "stale";
    const auto otherDir = env.dir() / "stale";
    REQUIRE(cli::prepareRendezvousDir(otherDir) == std::nullopt);
    env.createFile(
      cli::rendezvousPath(otherDir, mapPath).lexically_relative(env.dir()),
      cli::encodeRendezvous(rendezvous));

    checkError(run({"describe", mapArg}, {}, otherDir), "bad_token");
  }

  SECTION("a request in another protocol version gives proto_mismatch")
  {
    const auto rendezvous = *cli::readRendezvous(*host->rendezvousFile());
    auto header = cli::Json::object();
    header.set("proto", "tbcli.link/0");
    header.set("token", rendezvous.token);
    header.set("args", cli::Json::array());
    const auto request = cli::encodeFrame(header, {});

    auto future = std::async(std::launch::async, [&]() {
      return cli::exchangeFrames(rendezvous.endpoint, request);
    });
    const auto reply = await(future);
    REQUIRE(reply.has_value());
    checkError(cli::decodeReply(*reply), "proto_mismatch");
  }

  SECTION("a request waits while a transaction is open, then runs")
  {
    map.startTransaction("mapper drag", mdl::TransactionScope::LongRunning);
    auto future = tbcli({"apply", mapArg, "-"}, createBox);
    processEventsUntil([&]() { return host->queuedRequests() == 1; });
    processEventsFor(300ms);
    CHECK(future.wait_for(0ms) != std::future_status::ready);

    map.commitTransaction();
    const auto result = await(future);
    REQUIRE(result.has_value());
    CHECK(result->exitCode == 0);
    CHECK(map.worldNode().defaultLayer()->childCount() == 2);
  }

  SECTION("a request fails with busy if the mapper stays busy")
  {
    host->setBusyTimeout(200ms);
    map.startTransaction("mapper drag", mdl::TransactionScope::LongRunning);
    const auto result = run({"apply", mapArg, "-"}, createBox);
    map.cancelTransaction();

    checkError(result, "busy");
    CHECK(map.worldNode().defaultLayer()->childCount() == 1);
  }

  SECTION("requests are served in the order they arrive")
  {
    map.startTransaction("mapper drag", mdl::TransactionScope::LongRunning);
    auto futures = std::vector<std::future<std::optional<cli::RunResult>>>{};
    for (size_t i = 0; i < 3; ++i)
    {
      futures.push_back(tbcli({"apply", mapArg, "-"}, createBox));
      processEventsUntil([&]() { return host->queuedRequests() == i + 1; });
    }
    map.commitTransaction();

    auto revs = std::vector<size_t>{};
    for (auto& future : futures)
    {
      const auto result = await(future);
      REQUIRE(result.has_value());
      revs.push_back(revCounter(parse(*result)));
    }
    CHECK(revs[0] < revs[1]);
    CHECK(revs[1] < revs[2]);
  }

  document.setTargetLogger(nullptr);
}

} // namespace tb::ui
