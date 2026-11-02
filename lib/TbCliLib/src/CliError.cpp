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

#include "cli/CliError.h"

#include "cli/Json.h"

namespace tb::cli
{

std::string_view toString(const ErrorCode code)
{
  switch (code)
  {
  case ErrorCode::Usage:
    return "usage";
  case ErrorCode::ConfigError:
    return "config_error";
  case ErrorCode::FileNotFound:
    return "file_not_found";
  case ErrorCode::NotValve220:
    return "not_valve_220";
  case ErrorCode::ParseError:
    return "parse_error";
  case ErrorCode::InternalError:
    return "internal_error";
  }
  return "internal_error";
}

CliError::CliError(
  const ErrorCode code,
  const std::string& message,
  std::optional<std::string> node,
  const std::optional<size_t> line)
  : std::runtime_error{message}
  , m_code{code}
  , m_node{std::move(node)}
  , m_line{line}
{
}

ErrorCode CliError::code() const
{
  return m_code;
}

int CliError::exitCode() const
{
  return m_code == ErrorCode::Usage ? ExitCode::Usage : ExitCode::Failed;
}

const std::optional<std::string>& CliError::node() const
{
  return m_node;
}

const std::optional<size_t>& CliError::line() const
{
  return m_line;
}

Json CliError::toJson() const
{
  auto error = Json::object();
  error.set("code", toString(m_code));
  error.set("message", std::string{what()});
  if (m_node)
  {
    error.set("node", *m_node);
  }
  if (m_line)
  {
    error.set("line", *m_line);
  }

  auto result = Json::object();
  result.set("error", std::move(error));
  return result;
}

CliError usageError(const std::string& message)
{
  return CliError{ErrorCode::Usage, message};
}

CliError configError(const std::string& message, const std::optional<size_t> line)
{
  return CliError{ErrorCode::ConfigError, message, std::nullopt, line};
}

} // namespace tb::cli
