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
#include "cli/CliError.h"
#include "cli/LiveLink.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::cli
{
namespace
{

/** The pid of this process, which is alive. */
int64_t livePid()
{
#ifdef _WIN32
  return int64_t(_getpid());
#else
  return int64_t(::getpid());
#endif
}

/** A pid that no process has: above Linux's pid_max, and Windows pids are smaller. */
constexpr auto DeadPid = int64_t{0x7ffffffc};

} // namespace

TEST_CASE("LiveLink")
{
  auto env = CliTestEnvironment{};

  SECTION("rendezvousPath")
  {
    const auto dir = env.path("live");
    const auto map = env.writeFile("maps/a.map", "");
    const auto path = rendezvousPath(dir, map);
    CHECK(path.parent_path() == dir);
    CHECK(path.extension() == ".json");
    CHECK(path.stem().string().size() == 16);

    CHECK(rendezvousPath(dir, env.path("maps/../maps/a.map")) == path);
    CHECK(rendezvousPath(dir, env.path("maps/b.map")) != path);

    // a differently cased path names the same map only where the file system ignores
    // case, as on Windows and by default on macOS
    const auto fileSystemIgnoresCase = std::filesystem::exists(env.path("MAPS/A.MAP"));
#ifdef _WIN32
    REQUIRE(fileSystemIgnoresCase);
#endif
    CHECK((rendezvousPath(dir, env.path("MAPS/A.MAP")) == path) == fileSystemIgnoresCase);
  }

  SECTION("prepareRendezvousDir")
  {
    const auto dir = env.path("app-data/live");
    CHECK(prepareRendezvousDir(dir) == std::nullopt);
    CHECK(std::filesystem::is_directory(dir));

    // an existing directory is fine
    CHECK(prepareRendezvousDir(dir) == std::nullopt);

    CHECK(prepareRendezvousDir(env.writeFile("file", "")) != std::nullopt);

#ifndef _WIN32
    // the rendezvous files hold session tokens, so only the user may look inside
    using std::filesystem::perms;
    std::filesystem::permissions(dir, perms::all);
    CHECK(prepareRendezvousDir(dir) == std::nullopt);
    CHECK((std::filesystem::status(dir).permissions() & perms::all) == perms::owner_all);
#endif
  }

  SECTION("encodeRendezvous and readRendezvous")
  {
    const auto rendezvous = Rendezvous{
      std::string{LinkProto}, "C:/maps/a.map", R"(\\.\pipe\tb-1234)", "secret", 4711};
    const auto path = env.writeFile("a.json", encodeRendezvous(rendezvous));
    CHECK(readRendezvous(path) == rendezvous);
    CHECK(rendezvous.enabled);

    auto disabled = rendezvous;
    disabled.enabled = false;
    CHECK(
      readRendezvous(env.writeFile("disabled.json", encodeRendezvous(disabled)))
      == disabled);

    // from an editor that predates the preference
    CHECK(readRendezvous(
            env.writeFile(
              "old.json",
              R"({"proto": "tbcli.link/1", "map": "C:/maps/a.map", "endpoint": "e", )"
              R"("token": "t", "pid": 1})"))
            ->enabled);

    CHECK(readRendezvous(env.path("missing.json")) == std::nullopt);
    CHECK(readRendezvous(env.writeFile("bad.json", "{")) == std::nullopt);
    CHECK(
      readRendezvous(env.writeFile("partial.json", R"({"proto": "tbcli.link/1"})"))
      == std::nullopt);
  }

  SECTION("encodeEndedSession and readEndedSession")
  {
    const auto closed = EndedSession{"C:/maps/a.map", std::nullopt};
    CHECK(
      readEndedSession(env.writeFile("closed.json", encodeEndedSession(closed)))
      == closed);

    const auto moved = EndedSession{"C:/maps/a.map", "C:/maps/b.map"};
    CHECK(
      readEndedSession(env.writeFile("moved.json", encodeEndedSession(moved))) == moved);

    CHECK(readEndedSession(env.path("missing.json")) == std::nullopt);
    CHECK(readEndedSession(env.writeFile("bad.json", "[]")) == std::nullopt);
  }

  SECTION("processIsAlive")
  {
    CHECK(processIsAlive(livePid()));
    CHECK(!processIsAlive(DeadPid));
    CHECK(!processIsAlive(0));
    CHECK(!processIsAlive(-1));
  }

  SECTION("encodeFrame and decodeFrame")
  {
    // a payload that isn't valid UTF-8
    const auto payload = std::string{"\xff\xfe\x00\x01map", 7};
    const auto header = parseJsonLiteral(R"({"err": "", "exitCode": 0})");
    const auto frame = encodeFrame(header, payload);

    for (size_t size = 0; size < frame.size(); ++size)
    {
      CHECK(decodeFrame(std::string_view{frame}.substr(0, size)) == std::nullopt);
    }

    const auto decoded = decodeFrame(frame + "next");
    REQUIRE(decoded.has_value());
    CHECK(decoded->first.header == header);
    CHECK(decoded->first.payload == payload);
    CHECK(decoded->second == frame.size());

    const auto malformed = std::string{"\x02\x00\x00\x00{]\x00\x00\x00\x00", 10};
    CHECK_THROWS_AS(decodeFrame(malformed), CliError);
  }

  SECTION("encodeRequest and decodeRequest")
  {
    const auto invocation = Invocation{{"apply", "C:/maps/a.map", "-"}, "[]"};
    const auto request = decodeFrame(encodeRequest(invocation, "secret"));
    REQUIRE(request.has_value());

    const auto accepted = decodeRequest(request->first, "secret");
    REQUIRE(std::holds_alternative<Invocation>(accepted));
    CHECK(std::get<Invocation>(accepted).args == invocation.args);
    CHECK(std::get<Invocation>(accepted).in == invocation.in);

    const auto refused = decodeRequest(request->first, "other");
    REQUIRE(std::holds_alternative<RunResult>(refused));
    checkError(std::get<RunResult>(refused), "bad_token", 1);

    auto otherProto = request->first;
    otherProto.header.set("proto", "tbcli.link/0");
    const auto mismatched = decodeRequest(otherProto, "secret");
    REQUIRE(std::holds_alternative<RunResult>(mismatched));
    checkError(std::get<RunResult>(mismatched), "proto_mismatch", 1);
  }

  SECTION("encodeReply and decodeReply")
  {
    const auto result = RunResult{std::string{"\xff\x00out", 5}, "warning: x\n", 3};
    const auto reply = decodeFrame(encodeReply(result));
    REQUIRE(reply.has_value());
    const auto decoded = decodeReply(reply->first);
    CHECK(decoded.out == result.out);
    CHECK(decoded.err == result.err);
    CHECK(decoded.exitCode == result.exitCode);
    CHECK(!decoded.eject.has_value());

    SECTION("an eject carries map bytes that aren't valid UTF-8")
    {
      const auto ejected = RunResult{
        "", "", 0, Eject{"/maps/x.map", std::string{"caf\xe9\x00\xff", 6}, "live-ab:3"}};
      const auto ejectReply = decodeFrame(encodeReply(ejected));
      REQUIRE(ejectReply.has_value());
      const auto decodedEject = decodeReply(ejectReply->first);
      REQUIRE(decodedEject.eject.has_value());
      CHECK(decodedEject.eject->path == "/maps/x.map");
      CHECK(decodedEject.eject->bytes == ejected.eject->bytes);
      CHECK(decodedEject.eject->rev == "live-ab:3");
      CHECK(decodedEject.out.empty());
      CHECK(decodedEject.exitCode == 0);
    }
  }

  SECTION("runLive")
  {
    const auto dir = env.path("live");
    const auto map = env.copyFixture("maps/simple.map", "simple.map");
    const auto args = std::vector<std::string>{"describe", arg(map)};
    // as the editor would
    REQUIRE(prepareRendezvousDir(dir) == std::nullopt);

    SECTION("without an editor, Auto falls back to file mode and Live fails")
    {
      CHECK(runLive(args, {}, LinkMode::Auto, dir) == std::nullopt);
      checkError(*runLive(args, {}, LinkMode::Live, dir), "no_session", 1);
    }

    SECTION("File and commands without a map never use the live link")
    {
      CHECK(runLive(args, {}, LinkMode::File, dir) == std::nullopt);
      CHECK(runLive({"ops"}, {}, LinkMode::Live, dir) == std::nullopt);
    }

    SECTION("an editor that no longer listens counts as no session")
    {
      env.writeFile(
        rendezvousPath(dir, map),
        encodeRendezvous(
          {std::string{LinkProto},
           arg(map),
           R"(\\.\pipe\tb-no-such-pipe)",
           "t",
           livePid()}));
      CHECK(runLive(args, {}, LinkMode::Auto, dir) == std::nullopt);
      checkError(*runLive(args, {}, LinkMode::Live, dir), "no_session", 1);
    }

    SECTION("an editor that speaks another protocol gives proto_mismatch")
    {
      env.writeFile(
        rendezvousPath(dir, map),
        encodeRendezvous({"tbcli.link/0", arg(map), R"(\\.\pipe\tb-x)", "t", livePid()}));
      checkError(*runLive(args, {}, LinkMode::Auto, dir), "proto_mismatch", 1);
    }

    SECTION("a rendezvous file whose editor is dead counts as no session")
    {
      // left behind by a crashed editor, of another protocol version even
      env.writeFile(
        rendezvousPath(dir, map),
        encodeRendezvous({"tbcli.link/0", arg(map), R"(\\.\pipe\tb-x)", "t", DeadPid}));
      CHECK(runLive(args, {}, LinkMode::Auto, dir) == std::nullopt);
      checkError(*runLive(args, {}, LinkMode::Live, dir), "no_session", 1);
    }

    SECTION("a session that ended gives session_ended once, then file mode")
    {
      env.writeFile(
        endedSessionPath(dir, map), encodeEndedSession({arg(map), std::nullopt}));
      const auto original = env.readFile("simple.map");
      const auto apply = std::vector<std::string>{"apply", arg(map), "-"};

      const auto ended = runLive(apply, "[]", LinkMode::Auto, dir);
      REQUIRE(ended.has_value());
      checkError(*ended, "session_ended", 1);
      CHECK(env.readFile("simple.map") == original);

      CHECK(runLive(apply, "[]", LinkMode::Auto, dir) == std::nullopt);
    }

    SECTION("a session that moved to another path names it")
    {
      env.writeFile(
        endedSessionPath(dir, map),
        encodeEndedSession({arg(map), arg(env.path("moved.map"))}));

      const auto ended = runLive(args, {}, LinkMode::Live, dir);
      REQUIRE(ended.has_value());
      checkError(*ended, "session_ended", 1);
      CHECK_THAT(
        parseOutput(*ended)["error"]["message"].asString(),
        Catch::Matchers::ContainsSubstring("moved.map"));
    }

    SECTION("an editor that does not accept connections gives link_disabled")
    {
      auto rendezvous =
        Rendezvous{std::string{LinkProto}, arg(map), R"(\\.\pipe\tb-x)", "t", livePid()};
      rendezvous.enabled = false;
      env.writeFile(rendezvousPath(dir, map), encodeRendezvous(rendezvous));
      // file mode would fail to parse this
      env.writeFile("simple.map", "garbage");
      const auto apply = std::vector<std::string>{"apply", arg(map), "-"};

      const auto disabled = runLive(apply, "[]", LinkMode::Auto, dir);
      REQUIRE(disabled.has_value());
      checkError(*disabled, "link_disabled", 1);
      CHECK_THAT(
        parseOutput(*disabled)["error"]["message"].asString(),
        Catch::Matchers::ContainsSubstring("Accept AI connections"));
      CHECK(env.readFile("simple.map") == "garbage");
    }

#ifndef _WIN32
    SECTION("a rendezvous dir that other users can write to holds no sessions")
    {
      env.writeFile(
        rendezvousPath(dir, map),
        encodeRendezvous({"tbcli.link/0", arg(map), "/tmp/tb-x", "t", 1}));
      std::filesystem::permissions(dir, std::filesystem::perms::all);
      checkError(*runLive(args, {}, LinkMode::Live, dir), "no_session", 1);
    }
#endif
  }
}

} // namespace tb::cli
