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

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tb::cli
{

class Json;

namespace ExitCode
{
constexpr int Ok = 0;
constexpr int Failed = 1;
constexpr int Usage = 2;
constexpr int StaleRev = 3;
/** verify found an unsuppressed issue at or above `--fail-on` */
constexpr int FailOn = 4;
} // namespace ExitCode

/** The `code` of an error envelope. */
enum class ErrorCode
{
  /** malformed command line (exit 2) */
  Usage,
  /** tbcli.json, the game config or a configured file is missing or malformed */
  ConfigError,
  /** the map file cannot be read */
  FileNotFound,
  /** the map is in a format other than Valve 220 */
  NotValve220,
  /** the map cannot be parsed */
  ParseError,
  /** a node path is malformed */
  InvalidPath,
  /** a well-formed node path addresses no node */
  NodeNotFound,
  /** a command was given a node of a kind it does not accept */
  WrongNodeKind,
  /** a classname is not defined by the entity definitions */
  UndefinedClass,
  /** a batch is not a JSON array of op objects (exit 2) */
  InvalidBatch,
  /** an op is unknown, or its arguments are missing, unknown or malformed (exit 2) */
  InvalidArgument,
  /** `--expect-rev` does not match the map's rev (exit 3) */
  StaleRev,
  /** an atomic write could not be confirmed; the original file is unchanged */
  WriteFailed,
  /** an op targets a node that an earlier op in the batch removed */
  TargetDeleted,
  /** an op targets a locked node without --allow-locked */
  Locked,
  /** a query target matched a number of nodes other than its `expect` */
  ExpectMismatch,
  /** a mutating command after a load that skipped content, without --allow-lossy */
  LossyLoad,
  /** an op cannot be carried out with the given arguments */
  OpFailed,
  /** a needed compiler is not configured or not found */
  CompilerMissing,
  /** a compiler that a command needs failed or was stopped */
  CompileFailed,
  /** `rooms --from-artifacts` dir does not match the rev, leaked or lacks files */
  StaleArtifact,
  /** `rooms` on a map that leaks; the error carries the `leak` issue */
  MapLeaks,
  /** `add_room` would build a room whose walls overlap another built room's */
  LayoutOverlap,
  /** an unexpected failure inside tbcli */
  InternalError,
};

std::string_view toString(ErrorCode code);

/**
 * A failed command, rendered as the error envelope
 * `{"error": {"code", "message", "node"?, "line"?, "op_index"?, "issue"?}}` on stdout.
 *
 * Commands throw this and run() turns it into output and an exit code, so that every
 * failure, however deep, reaches the caller in the same shape.
 */
class CliError : public std::runtime_error
{
private:
  ErrorCode m_code;
  std::optional<std::string> m_node;
  std::optional<size_t> m_line;
  std::optional<size_t> m_opIndex;
  std::shared_ptr<const Json> m_issue;

public:
  CliError(
    ErrorCode code,
    const std::string& message,
    std::optional<std::string> node = std::nullopt,
    std::optional<size_t> line = std::nullopt);

  ErrorCode code() const;
  int exitCode() const;
  const std::optional<std::string>& node() const;
  const std::optional<size_t>& line() const;
  const std::optional<size_t>& opIndex() const;

  /** This error, attributed to the op at the given index of a batch. */
  CliError withOpIndex(size_t opIndex) const;

  /** This error, carrying the given issue record, such as the `leak` issue. */
  CliError withIssue(Json issue) const;

  Json toJson() const;
};

/** Calls `f`, attributing a CliError it throws to the op at the given index of a batch.
 */
template <typename F>
decltype(auto) attributeToOp(const size_t opIndex, F&& f)
{
  try
  {
    return f();
  }
  catch (const CliError& e)
  {
    throw e.withOpIndex(opIndex);
  }
}

/** A malformed command line (exit 2). */
CliError usageError(const std::string& message);

/** An op that is unknown, or whose arguments are missing, unknown or malformed (exit 2).
 */
CliError invalidArgument(const std::string& message);

/**
 * An op that cannot be carried out with the given arguments (exit 1), with the op's name
 * in front of the message, and the node or line it is about, if any.
 */
CliError opFailed(
  std::string_view opName,
  std::string_view message,
  std::optional<std::string> node = std::nullopt,
  std::optional<size_t> line = std::nullopt);

/** A missing or malformed configuration (exit 1). */
CliError configError(
  const std::string& message, std::optional<size_t> line = std::nullopt);

} // namespace tb::cli
