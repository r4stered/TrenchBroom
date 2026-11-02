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

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::cli
{

TEST_CASE("CliError")
{
  SECTION("exitCode")
  {
    using T = std::tuple<ErrorCode, int>;
    const auto [code, exitCode] = GENERATE(
      T{ErrorCode::Usage, 2},
      T{ErrorCode::InvalidBatch, 2},
      T{ErrorCode::InvalidArgument, 2},
      T{ErrorCode::StaleRev, 3},
      T{ErrorCode::WriteFailed, 1},
      T{ErrorCode::TargetDeleted, 1},
      T{ErrorCode::Locked, 1},
      T{ErrorCode::ExpectMismatch, 1},
      T{ErrorCode::LossyLoad, 1},
      T{ErrorCode::OpFailed, 1},
      T{ErrorCode::CompilerMissing, 1},
      T{ErrorCode::NodeNotFound, 1});
    CAPTURE(toString(code));
    CHECK(CliError{code, "message"}.exitCode() == exitCode);
  }

  SECTION("toJson")
  {
    CHECK(
      CliError{ErrorCode::StaleRev, "stale"}.toJson()
      == parseJsonLiteral(R"({"error": {"code": "stale_rev", "message": "stale"}})"));
    CHECK(
      CliError{ErrorCode::NodeNotFound, "gone", "layer:0/brush:3", 7}
        .withOpIndex(2)
        .toJson()
      == parseJsonLiteral(R"({"error": {
        "code": "node_not_found", "message": "gone", "node": "layer:0/brush:3",
        "line": 7, "op_index": 2
      }})"));
  }

  SECTION("withOpIndex")
  {
    const auto error = CliError{ErrorCode::OpFailed, "failed", "world"};
    const auto attributed = error.withOpIndex(4);
    CHECK(error.opIndex() == std::nullopt);
    CHECK(attributed.opIndex() == 4);
    CHECK(attributed.code() == ErrorCode::OpFailed);
    CHECK(attributed.node() == "world");
    CHECK(std::string{attributed.what()} == "failed");
  }

  SECTION("attributeToOp")
  {
    CHECK(attributeToOp(1, [] { return 42; }) == 42);

    try
    {
      attributeToOp(3, []() -> void { throw CliError{ErrorCode::OpFailed, "failed"}; });
      FAIL("attributeToOp swallowed the error");
    }
    catch (const CliError& e)
    {
      CHECK(e.code() == ErrorCode::OpFailed);
      CHECK(e.opIndex() == 3);
    }
  }

  SECTION("invalidArgument")
  {
    const auto error = invalidArgument("bad");
    CHECK(error.code() == ErrorCode::InvalidArgument);
    CHECK(error.exitCode() == ExitCode::Usage);
    CHECK(std::string{error.what()} == "bad");
  }
}

} // namespace tb::cli
