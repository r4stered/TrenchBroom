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

#include <string>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{

TEST_CASE("LiveLink")
{
  auto env = CliTestEnvironment{};

  SECTION("rendezvousPath")
  {
    const auto dir = env.path("live");
    const auto path = rendezvousPath(dir, env.path("maps/a.map"));
    CHECK(path.parent_path() == dir);
    CHECK(path.extension() == ".json");
    CHECK(path.stem().string().size() == 16);

    CHECK(rendezvousPath(dir, env.path("maps/../maps/a.map")) == path);
    CHECK(rendezvousPath(dir, env.path("maps/b.map")) != path);
#ifdef _WIN32
    // Windows file systems ignore case
    CHECK(rendezvousPath(dir, env.path("MAPS/A.map")) == path);
#endif
  }

  SECTION("encodeRendezvous and readRendezvous")
  {
    const auto rendezvous = Rendezvous{
      std::string{LinkProto}, "C:/maps/a.map", R"(\\.\pipe\tb-1234)", "secret", 4711};
    const auto path = env.writeFile("a.json", encodeRendezvous(rendezvous));
    CHECK(readRendezvous(path) == rendezvous);

    CHECK(readRendezvous(env.path("missing.json")) == std::nullopt);
    CHECK(readRendezvous(env.writeFile("bad.json", "{")) == std::nullopt);
    CHECK(
      readRendezvous(env.writeFile("partial.json", R"({"proto": "tbcli.link/1"})"))
      == std::nullopt);
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
  }

  SECTION("runLive")
  {
    const auto dir = env.path("live");
    const auto map = env.copyFixture("maps/simple.map", "simple.map");
    const auto args = std::vector<std::string>{"describe", arg(map)};

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
          {std::string{LinkProto}, arg(map), R"(\\.\pipe\tb-no-such-pipe)", "t", 1}));
      CHECK(runLive(args, {}, LinkMode::Auto, dir) == std::nullopt);
      checkError(*runLive(args, {}, LinkMode::Live, dir), "no_session", 1);
    }

    SECTION("an editor that speaks another protocol gives proto_mismatch")
    {
      env.writeFile(
        rendezvousPath(dir, map),
        encodeRendezvous({"tbcli.link/0", arg(map), R"(\\.\pipe\tb-x)", "t", 1}));
      checkError(*runLive(args, {}, LinkMode::Auto, dir), "proto_mismatch", 1);
    }
  }
}

} // namespace tb::cli
