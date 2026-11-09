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

#include "cli/CliError.h"
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

/**
 * Whether tbcli can reach an editor on this platform: over a named pipe on Windows and
 * a Unix domain socket on macOS and Linux, which is what QLocalServer listens on.
 */
#if defined(_WIN32) || defined(__unix__) || defined(__APPLE__)
inline constexpr auto HasLinkClient = true;
#else
inline constexpr auto HasLinkClient = false;
#endif

/**
 * The per-user directory that holds the rendezvous files of open editors, in the
 * platform's app data location: `%LOCALAPPDATA%\TrenchBroom\live` on Windows,
 * `~/Library/Application Support/TrenchBroom/live` on macOS and
 * `$XDG_DATA_HOME/TrenchBroom/live` (by default `~/.local/share/TrenchBroom/live`)
 * elsewhere.
 */
std::filesystem::path defaultRendezvousDir();

/**
 * Creates the rendezvous directory if needed and makes sure that only the current user
 * can look inside, since the rendezvous files hold session tokens. On macOS and Linux,
 * the directory must belong to the current user and gets mode 0700; on Windows, the
 * per-user app data location already keeps other users out.
 *
 * Returns why the directory can't be used, or nothing if it can.
 */
std::optional<std::string> prepareRendezvousDir(const std::filesystem::path& dir);

/**
 * The rendezvous file in the given directory for the given map. It is named by the
 * first 16 hex digits of the SHA-256 of the map's absolute path, lower-cased if the
 * file system ignores case: always on Windows, and elsewhere if the map's directory
 * finds the map under its name with the case of its letters swapped.
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
  /**
   * Whether the editor accepts requests for the map. If the mapper turned off the
   * "Accept AI connections" preference, the editor still publishes the rendezvous file
   * so that tbcli refuses to edit the file under it, but doesn't listen.
   */
  bool enabled = true;

  friend bool operator==(const Rendezvous&, const Rendezvous&) = default;
};

std::string encodeRendezvous(const Rendezvous& rendezvous);

/** Reads a rendezvous file, or nothing if it is missing or malformed. */
std::optional<Rendezvous> readRendezvous(const std::filesystem::path& path);

/**
 * Whether a process with the given id is running. tbcli treats a rendezvous file whose
 * editor isn't as left behind by a crash.
 */
bool processIsAlive(int64_t pid);

/**
 * The file in the given directory that tells tbcli that an agent's session on the given
 * map has ended: the editor writes it when it closes the map, or saves it under another
 * path, after an agent sent it a request, and tbcli removes it when it reports
 * `session_ended`. Without it, tbcli could not tell a closed editor from no editor, and
 * would silently edit the file that the agent thinks is still open.
 */
std::filesystem::path endedSessionPath(
  const std::filesystem::path& rendezvousDir, const std::filesystem::path& mapPath);

/** What the editor leaves behind when it ends a session that an agent used. */
struct EndedSession
{
  /** The map that the session had open. */
  std::string map;
  /** The path that the map was saved under, if it was saved under another one. */
  std::optional<std::string> movedTo;

  friend bool operator==(const EndedSession&, const EndedSession&) = default;
};

std::string encodeEndedSession(const EndedSession& endedSession);

/** Reads an ended session file, or nothing if it is missing or malformed. */
std::optional<EndedSession> readEndedSession(const std::filesystem::path& path);

/** The `session_ended` error for a command that the given session did not run. */
CliError sessionEndedError(const EndedSession& endedSession);

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
 * with `proto_mismatch` if the editor speaks another protocol version, with
 * `link_disabled` if the editor doesn't accept connections for the map, and, once,
 * with `session_ended` if an editor ended a session on the map that an agent used (see
 * endedSessionPath). A rendezvous file whose editor process is dead counts as no
 * session. On macOS and Linux, a rendezvous directory that isn't private to the current
 * user (see prepareRendezvousDir) holds no sessions.
 */
std::optional<RunResult> runLive(
  const std::vector<std::string>& args,
  std::string_view in,
  LinkMode mode,
  const std::filesystem::path& rendezvousDir = defaultRendezvousDir());

} // namespace tb::cli
