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
  /** an unexpected failure inside tbcli */
  InternalError,
};

std::string_view toString(ErrorCode code);

/**
 * A failed command, rendered as the error envelope
 * `{"error": {"code", "message", "node"?, "line"?}}` on stdout.
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

  Json toJson() const;
};

/** A malformed command line (exit 2). */
CliError usageError(const std::string& message);

/** A missing or malformed configuration (exit 1). */
CliError configError(
  const std::string& message, std::optional<size_t> line = std::nullopt);

} // namespace tb::cli
