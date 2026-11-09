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

#include <fmt/format.h>

#include <utility>

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
  case ErrorCode::InvalidPath:
    return "invalid_path";
  case ErrorCode::NodeNotFound:
    return "node_not_found";
  case ErrorCode::WrongNodeKind:
    return "wrong_node_kind";
  case ErrorCode::UndefinedClass:
    return "undefined_class";
  case ErrorCode::InvalidBatch:
    return "invalid_batch";
  case ErrorCode::InvalidArgument:
    return "invalid_argument";
  case ErrorCode::StaleRev:
    return "stale_rev";
  case ErrorCode::WriteFailed:
    return "write_failed";
  case ErrorCode::TargetDeleted:
    return "target_deleted";
  case ErrorCode::Locked:
    return "locked";
  case ErrorCode::ExpectMismatch:
    return "expect_mismatch";
  case ErrorCode::LossyLoad:
    return "lossy_load";
  case ErrorCode::OpFailed:
    return "op_failed";
  case ErrorCode::CompilerMissing:
    return "compiler_missing";
  case ErrorCode::CompileFailed:
    return "compile_failed";
  case ErrorCode::StaleArtifact:
    return "stale_artifact";
  case ErrorCode::MapLeaks:
    return "map_leaks";
  case ErrorCode::LayoutOverlap:
    return "layout_overlap";
  case ErrorCode::LayoutCorridorDoesNotFit:
    return "layout_corridor_does_not_fit";
  case ErrorCode::LayoutFloorMismatch:
    return "layout_floor_mismatch";
  case ErrorCode::LayoutCorridorTooTall:
    return "layout_corridor_too_tall";
  case ErrorCode::HandEdited:
    return "hand_edited";
  case ErrorCode::ConnectCutsLinkedRoom:
    return "connect_cuts_linked_room";
  case ErrorCode::AmbiguousRoomName:
    return "ambiguous_room_name";
  case ErrorCode::NoSession:
    return "no_session";
  case ErrorCode::BadToken:
    return "bad_token";
  case ErrorCode::ProtoMismatch:
    return "proto_mismatch";
  case ErrorCode::Busy:
    return "busy";
  case ErrorCode::SessionEnded:
    return "session_ended";
  case ErrorCode::LinkDisabled:
    return "link_disabled";
  case ErrorCode::Gone:
    return "gone";
  case ErrorCode::StaleSession:
    return "stale_session";
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
  switch (m_code)
  {
  case ErrorCode::Usage:
  case ErrorCode::InvalidBatch:
  case ErrorCode::InvalidArgument:
    return ExitCode::Usage;
  case ErrorCode::StaleRev:
    return ExitCode::StaleRev;
  default:
    return ExitCode::Failed;
  }
}

const std::optional<std::string>& CliError::node() const
{
  return m_node;
}

const std::optional<size_t>& CliError::line() const
{
  return m_line;
}

const std::optional<size_t>& CliError::opIndex() const
{
  return m_opIndex;
}

CliError CliError::withOpIndex(const size_t opIndex) const
{
  auto result = *this;
  result.m_opIndex = opIndex;
  return result;
}

CliError CliError::withIssue(Json issue) const
{
  auto result = *this;
  result.m_issue = std::make_shared<const Json>(std::move(issue));
  return result;
}

CliError CliError::withCandidates(std::vector<std::string> candidates) const
{
  auto result = *this;
  result.m_candidates = std::move(candidates);
  return result;
}

CliError CliError::withNodeHandle(std::string nodeHandle) const
{
  auto result = *this;
  result.m_nodeHandle = std::move(nodeHandle);
  return result;
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
  if (m_nodeHandle)
  {
    error.set("node_handle", *m_nodeHandle);
  }
  if (m_line)
  {
    error.set("line", *m_line);
  }
  if (m_opIndex)
  {
    error.set("op_index", *m_opIndex);
  }
  if (m_issue)
  {
    error.set("issue", *m_issue);
  }
  if (!m_candidates.empty())
  {
    auto candidates = Json::array();
    for (const auto& candidate : m_candidates)
    {
      candidates.push(candidate);
    }
    error.set("candidates", std::move(candidates));
  }

  auto result = Json::object();
  result.set("error", std::move(error));
  return result;
}

CliError usageError(const std::string& message)
{
  return CliError{ErrorCode::Usage, message};
}

CliError invalidArgument(const std::string& message)
{
  return CliError{ErrorCode::InvalidArgument, message};
}

CliError opFailed(
  const std::string_view opName,
  const std::string_view message,
  std::optional<std::string> node,
  const std::optional<size_t> line)
{
  return CliError{
    ErrorCode::OpFailed, fmt::format("{}: {}", opName, message), std::move(node), line};
}

CliError configError(const std::string& message, const std::optional<size_t> line)
{
  return CliError{ErrorCode::ConfigError, message, std::nullopt, line};
}

} // namespace tb::cli
