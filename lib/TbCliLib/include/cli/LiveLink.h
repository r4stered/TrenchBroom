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

#include "cli/Json.h"
#include "cli/Run.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tb::cli
{

/**
 * The live link: how tbcli reaches an editor that has a map open.
 *
 * The editor listens on a local socket (a named pipe on Windows) per open map and
 * publishes a rendezvous file that tbcli finds by the map's path. tbcli sends one
 * request per connection, and the editor replies once and closes the connection.
 */

/** The version of the live link protocol that this tbcli and editor speak. */
inline constexpr auto LinkProto = std::string_view{"tbcli.link/1"};

/** Whether tbcli can reach an editor on this platform: only on Windows so far. */
#ifdef _WIN32
inline constexpr auto HasLinkClient = true;
#else
inline constexpr auto HasLinkClient = false;
#endif

/**
 * The per-user directory that holds the rendezvous files of open editors:
 * `%LOCALAPPDATA%\TrenchBroom\live` on Windows.
 */
std::filesystem::path defaultRendezvousDir();

/**
 * The rendezvous file in the given directory for the given map. It is named by the
 * first 16 hex digits of the SHA-256 of the map's absolute path, lower-cased on
 * Windows, whose file systems ignore case.
 */
std::filesystem::path rendezvousPath(
  const std::filesystem::path& rendezvousDir, const std::filesystem::path& mapPath);

/** What an editor publishes so that tbcli can reach the map it has open. */
struct Rendezvous
{
  /** The protocol version that the editor speaks. */
  std::string proto = std::string{LinkProto};
  /** The open map's path. */
  std::string map;
  /** The name of the local socket that the editor listens on. */
  std::string endpoint;
  /** The secret that every request must carry. */
  std::string token;
  /** The editor's process id. */
  int64_t pid = 0;

  friend bool operator==(const Rendezvous&, const Rendezvous&) = default;
};

std::string encodeRendezvous(const Rendezvous& rendezvous);

/** Reads a rendezvous file, or nothing if it is missing or malformed. */
std::optional<Rendezvous> readRendezvous(const std::filesystem::path& path);

/**
 * One message on the live link: a JSON header and a binary payload.
 *
 * On the wire, a frame is a 4 byte little-endian header length, the header's UTF-8
 * JSON, a 4 byte little-endian payload length and the payload's bytes. A request's
 * header is `{proto, token, args}` and its payload is stdin; a reply's header is
 * `{err, exitCode}` and its payload is stdout. Neither payload needs escaping, so a reply
 * can carry bytes that aren't valid UTF-8.
 */
struct Frame
{
  Json header;
  std::string payload;
};

std::string encodeFrame(const Json& header, std::string_view payload);

/**
 * Decodes the frame at the start of the buffer. Returns the frame and the number of
 * bytes it took, or nothing if the buffer does not hold a complete frame yet.
 *
 * @throw CliError `internal_error` if the frame is malformed
 */
std::optional<std::pair<Frame, size_t>> decodeFrame(std::string_view buffer);

/** The request frame that runs the given invocation in the editor. */
std::string encodeRequest(const Invocation& invocation, std::string_view token);

/**
 * The invocation that a request frame carries, or the reply that refuses it:
 * `proto_mismatch` if it speaks another protocol version, `bad_token` if its token is
 * not the given one.
 */
std::variant<Invocation, RunResult> decodeRequest(
  const Frame& request, std::string_view token);

/** The reply frame that carries the given result. */
std::string encodeReply(const RunResult& result);

/** The result that a reply frame carries. */
RunResult decodeReply(const Frame& reply);

/** Whether tbcli tries the live link before file mode. */
enum class LinkMode
{
  /** use the live link when an editor has the map open, file mode otherwise */
  Auto,
  /** fail with `no_session` unless an editor has the map open */
  Live,
  /** never use the live link */
  File,
};

/**
 * Sends a request frame to the editor listening on the given endpoint and returns its
 * reply, or nothing if no editor listens there.
 *
 * @throw CliError `internal_error` if the editor closes the connection mid-reply
 */
std::optional<Frame> exchangeFrames(
  const std::string& endpoint, std::string_view request);

/**
 * Runs the command through the live link of the editor that has its map open, as
 * published in the given rendezvous directory. Returns nothing if the command should
 * run in file mode instead: with LinkMode::File, for commands without a map, and
 * with LinkMode::Auto when no editor has the map open.
 *
 * Fails with `no_session` if no editor has the map open and mode is LinkMode::Live,
 * and with `proto_mismatch` if the editor speaks another protocol version.
 */
std::optional<RunResult> runLive(
  const std::vector<std::string>& args,
  std::string_view in,
  LinkMode mode,
  const std::filesystem::path& rendezvousDir = defaultRendezvousDir());

} // namespace tb::cli
