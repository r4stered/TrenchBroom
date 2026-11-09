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

#include "cli/LiveLink.h"

#include "cli/CliError.h"
#include "cli/FileUtils.h"
#include "cli/Sha256.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstdlib>
#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pwd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace tb::cli
{
namespace
{

/** A header longer than this is not a frame that tbcli or the editor would send. */
constexpr auto MaxHeaderSize = size_t{1} << 20;
/** A payload longer than this is not a map or a batch that tbcli would send. */
constexpr auto MaxPayloadSize = size_t{1} << 30;

void appendU32(std::string& out, const uint32_t value)
{
  for (size_t i = 0; i < 4; ++i)
  {
    out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
  }
}

uint32_t readU32(const std::string_view in, const size_t offset)
{
  auto value = uint32_t{0};
  for (size_t i = 0; i < 4; ++i)
  {
    value |= uint32_t{static_cast<unsigned char>(in[offset + i])} << (8 * i);
  }
  return value;
}

#ifndef _WIN32

/**
 * The string with the case of its ASCII letters swapped, or only lowered. Other
 * letters keep their case, so paths that differ in the case of them name different
 * sessions.
 */
std::string changeAsciiCase(std::string str, const bool swap)
{
  for (auto& c : str)
  {
    if (c >= 'A' && c <= 'Z')
    {
      c = char(c - 'A' + 'a');
    }
    else if (swap && c >= 'a' && c <= 'z')
    {
      c = char(c - 'a' + 'A');
    }
  }
  return str;
}

/**
 * Whether the file system that holds the given file ignores case, as macOS file
 * systems do by default: whether the file's directory finds it under its name with the
 * case of its letters swapped. A file that doesn't exist names no session, so this
 * may be false for it.
 *
 * Only the file's own directory is probed, so a case-sensitive volume mounted below a
 * case-insensitive one is keyed by the case of the path as given.
 */
bool fileSystemIgnoresCase(const std::filesystem::path& path)
{
  const auto name = path.filename().string();
  const auto swapped = changeAsciiCase(name, true);
  if (swapped == name)
  {
    return false;
  }
  auto error = std::error_code{};
  return std::filesystem::equivalent(path, path.parent_path() / swapped, error);
}

#endif

/**
 * The path that names a map's session: absolute, and lower-cased if the file system
 * ignores case.
 */
std::string sessionKey(const std::filesystem::path& mapPath)
{
  const auto path = std::filesystem::absolute(mapPath).lexically_normal();
#ifdef _WIN32
  auto wide = path.wstring();
  CharLowerBuffW(wide.data(), DWORD(wide.size()));
  const auto u8Key = std::filesystem::path{wide}.generic_u8string();
  return std::string{u8Key.begin(), u8Key.end()};
#else
  const auto u8Key = path.generic_u8string();
  auto key = std::string{u8Key.begin(), u8Key.end()};
  return fileSystemIgnoresCase(path) ? changeAsciiCase(std::move(key), false) : key;
#endif
}

/** The name of a map's session files: a hash of its session key. */
std::string sessionId(const std::filesystem::path& mapPath)
{
  return sha256Hex(sessionKey(mapPath)).substr(0, 16);
}

#ifdef _WIN32

/** Closes a Windows handle when it goes out of scope. */
class HandleCloser
{
private:
  HANDLE m_handle;

public:
  explicit HandleCloser(HANDLE handle)
    : m_handle{handle}
  {
  }

  ~HandleCloser() { CloseHandle(m_handle); }

  HandleCloser(const HandleCloser&) = delete;
  HandleCloser& operator=(const HandleCloser&) = delete;
};

/** Connects to the named pipe, waiting while all its instances are busy. */
HANDLE connectPipe(const std::wstring& name)
{
  for (int attempt = 0; attempt < 100; ++attempt)
  {
    // SECURITY_IDENTIFICATION: a server squatting on the name can't impersonate us
    const auto pipe = CreateFileW(
      name.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
      nullptr);
    if (pipe != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY)
    {
      return pipe;
    }
    WaitNamedPipeW(name.c_str(), 100);
  }
  return INVALID_HANDLE_VALUE;
}

#else

/** Closes a file descriptor when it goes out of scope. */
class FdCloser
{
private:
  int m_fd;

public:
  explicit FdCloser(const int fd)
    : m_fd{fd}
  {
  }

  ~FdCloser() { ::close(m_fd); }

  FdCloser(const FdCloser&) = delete;
  FdCloser& operator=(const FdCloser&) = delete;
};

/** Connects to the Unix domain socket at the given path, or returns -1. */
int connectSocket(const std::string& path)
{
  auto address = sockaddr_un{};
  address.sun_family = AF_UNIX;
  if (path.empty() || path.size() >= sizeof(address.sun_path))
  {
    return -1;
  }
  std::memcpy(address.sun_path, path.data(), path.size());

  const auto fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
  {
    return -1;
  }
#ifdef SO_NOSIGPIPE
  // macOS: writing to a closed connection must fail, not kill tbcli with SIGPIPE
  const auto on = int{1};
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
  {
    ::close(fd);
    return -1;
  }
  return fd;
}

/**
 * The mode of the given directory if it belongs to the current user. A symbolic link
 * doesn't count, since someone else may have planted it.
 */
std::optional<mode_t> modeOfOwnDirectory(const std::filesystem::path& dir)
{
  struct stat info = {};
  if (
    ::lstat(dir.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)
    || info.st_uid != ::geteuid())
  {
    return std::nullopt;
  }
  return info.st_mode & 0777;
}

/** The current user's home directory, if it is known. */
std::optional<std::filesystem::path> homeDir()
{
  if (const auto* home = std::getenv("HOME"); home && *home)
  {
    return std::filesystem::path{home};
  }
  if (const auto* user = ::getpwuid(::geteuid()); user && user->pw_dir)
  {
    return std::filesystem::path{user->pw_dir};
  }
  return std::nullopt;
}

#endif

} // namespace

std::filesystem::path defaultRendezvousDir()
{
#ifdef _WIN32
  if (const auto* localAppData = _wgetenv(L"LOCALAPPDATA"))
  {
    return std::filesystem::path{localAppData} / "TrenchBroom" / "live";
  }
#elif defined(__APPLE__)
  if (const auto home = homeDir())
  {
    return *home / "Library" / "Application Support" / "TrenchBroom" / "live";
  }
#else
  if (
    const auto* dataHome = std::getenv("XDG_DATA_HOME");
    dataHome && std::filesystem::path{dataHome}.is_absolute())
  {
    return std::filesystem::path{dataHome} / "TrenchBroom" / "live";
  }
  if (const auto home = homeDir())
  {
    return *home / ".local" / "share" / "TrenchBroom" / "live";
  }
#endif
  // prepareRendezvousDir refuses this if another user created it first
  return std::filesystem::temp_directory_path() / "trenchbroom-live";
}

std::optional<std::string> prepareRendezvousDir(const std::filesystem::path& dir)
{
  auto error = std::error_code{};
  std::filesystem::create_directories(dir, error);
  if (error)
  {
    return fmt::format(
      "cannot create the directory {}: {}", dir.string(), error.message());
  }
  if (!std::filesystem::is_directory(dir, error))
  {
    return fmt::format("{} is not a directory", dir.string());
  }

#ifndef _WIN32
  const auto mode = modeOfOwnDirectory(dir);
  if (!mode)
  {
    return fmt::format("{} is not a directory of the current user", dir.string());
  }
  if (*mode != 0700 && ::chmod(dir.c_str(), 0700) != 0)
  {
    const auto chmodError = errno;
    return fmt::format(
      "cannot make {} private to the current user: {}",
      dir.string(),
      std::strerror(chmodError));
  }
#endif
  return std::nullopt;
}

std::filesystem::path rendezvousPath(
  const std::filesystem::path& rendezvousDir, const std::filesystem::path& mapPath)
{
  return rendezvousDir / (sessionId(mapPath) + ".json");
}

std::string encodeRendezvous(const Rendezvous& rendezvous)
{
  auto json = Json::object();
  json.set("proto", rendezvous.proto);
  json.set("map", rendezvous.map);
  json.set("endpoint", rendezvous.endpoint);
  json.set("token", rendezvous.token);
  json.set("pid", rendezvous.pid);
  json.set("enabled", rendezvous.enabled);
  return json.dump() + "\n";
}

std::optional<Rendezvous> readRendezvous(const std::filesystem::path& path)
{
  const auto text = readFile(path);
  if (!text)
  {
    return std::nullopt;
  }
  auto parsed = parseJson(*text);
  if (!parsed || !parsed.value().isObject())
  {
    return std::nullopt;
  }

  const auto& json = parsed.value();
  const auto string = [&](const std::string_view key) -> std::optional<std::string> {
    const auto* value = json.find(key);
    return value && value->isString() ? std::optional{value->asString()} : std::nullopt;
  };
  const auto proto = string("proto");
  const auto map = string("map");
  const auto endpoint = string("endpoint");
  const auto token = string("token");
  const auto* pid = json.find("pid");
  const auto* enabled = json.find("enabled");
  // editors that predate the preference always accept connections
  if (
    !proto || !map || !endpoint || !token || !pid || !pid->isNumber()
    || (enabled && !enabled->isBool()))
  {
    return std::nullopt;
  }
  return Rendezvous{
    *proto,
    *map,
    *endpoint,
    *token,
    int64_t(pid->asNumber()),
    !enabled || enabled->asBool()};
}

bool processIsAlive(const int64_t pid)
{
#ifdef _WIN32
  if (pid <= 0 || pid > int64_t{std::numeric_limits<DWORD>::max()})
  {
    return false;
  }
  const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
  if (!process)
  {
    // a process that we may not look at is still running
    return GetLastError() == ERROR_ACCESS_DENIED;
  }
  const auto closer = HandleCloser{process};
  auto exitCode = DWORD{0};
  return GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
#else
  if (pid <= 0 || pid > int64_t{std::numeric_limits<pid_t>::max()})
  {
    // kill() would signal a process group instead
    return false;
  }
  // signal 0 only checks whether the process exists
  return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}

std::filesystem::path endedSessionPath(
  const std::filesystem::path& rendezvousDir, const std::filesystem::path& mapPath)
{
  return rendezvousDir / (sessionId(mapPath) + ".ended.json");
}

std::string encodeEndedSession(const EndedSession& endedSession)
{
  auto json = Json::object();
  json.set("map", endedSession.map);
  if (endedSession.movedTo)
  {
    json.set("movedTo", *endedSession.movedTo);
  }
  return json.dump() + "\n";
}

std::optional<EndedSession> readEndedSession(const std::filesystem::path& path)
{
  const auto text = readFile(path);
  if (!text)
  {
    return std::nullopt;
  }
  auto parsed = parseJson(*text);
  if (!parsed || !parsed.value().isObject())
  {
    return std::nullopt;
  }

  const auto& json = parsed.value();
  const auto* map = json.find("map");
  const auto* movedTo = json.find("movedTo");
  if (!map || !map->isString() || (movedTo && !movedTo->isString()))
  {
    return std::nullopt;
  }
  return EndedSession{
    map->asString(), movedTo ? std::optional{movedTo->asString()} : std::nullopt};
}

CliError sessionEndedError(const EndedSession& endedSession)
{
  return CliError{
    ErrorCode::SessionEnded,
    endedSession.movedTo
      ? fmt::format(
          "the editor saved {} as {}, so this command did not run; run it on {}",
          endedSession.map,
          *endedSession.movedTo,
          *endedSession.movedTo)
      : fmt::format(
          "the editor closed {}, so this command did not run; it is no longer open in "
          "the editor",
          endedSession.map)};
}

std::string encodeFrame(const Json& header, const std::string_view payload)
{
  const auto headerText = header.dump();
  auto result = std::string{};
  result.reserve(8 + headerText.size() + payload.size());
  appendU32(result, uint32_t(headerText.size()));
  result += headerText;
  appendU32(result, uint32_t(payload.size()));
  result += payload;
  return result;
}

std::optional<std::pair<Frame, size_t>> decodeFrame(const std::string_view buffer)
{
  if (buffer.size() < 4)
  {
    return std::nullopt;
  }
  const auto headerSize = size_t(readU32(buffer, 0));
  if (headerSize > MaxHeaderSize)
  {
    throw CliError{ErrorCode::InternalError, "the live link sent a malformed frame"};
  }
  if (buffer.size() < 8 + headerSize)
  {
    return std::nullopt;
  }
  const auto payloadSize = size_t(readU32(buffer, 4 + headerSize));
  if (payloadSize > MaxPayloadSize)
  {
    throw CliError{ErrorCode::InternalError, "the live link sent a malformed frame"};
  }
  const auto size = 8 + headerSize + payloadSize;
  if (buffer.size() < size)
  {
    return std::nullopt;
  }

  auto header = parseJson(buffer.substr(4, headerSize));
  if (!header || !header.value().isObject())
  {
    throw CliError{ErrorCode::InternalError, "the live link sent a malformed frame"};
  }
  return std::pair{
    Frame{
      std::move(header).value(), std::string{buffer.substr(8 + headerSize, payloadSize)}},
    size};
}

std::string encodeRequest(const Invocation& invocation, const std::string_view token)
{
  auto args = Json::array();
  for (const auto& arg : invocation.args)
  {
    args.push(arg);
  }

  auto header = Json::object();
  header.set("proto", LinkProto);
  header.set("token", token);
  header.set("args", std::move(args));
  return encodeFrame(header, invocation.in);
}

std::variant<Invocation, RunResult> decodeRequest(
  const Frame& request, const std::string_view token)
{
  const auto* proto = request.header.find("proto");
  if (!proto || !proto->isString() || proto->asString() != LinkProto)
  {
    return errorResult(
      CliError{
        ErrorCode::ProtoMismatch,
        fmt::format(
          "the editor speaks {}, but tbcli sent {}; use a tbcli of the editor's version",
          LinkProto,
          proto && proto->isString() ? proto->asString() : "no protocol version")});
  }

  const auto* requestToken = request.header.find("token");
  if (!requestToken || !requestToken->isString() || requestToken->asString() != token)
  {
    return errorResult(
      CliError{
        ErrorCode::BadToken,
        "the editor refused the session token; its rendezvous file may be stale"});
  }

  const auto* args = request.header.find("args");
  if (!args || !args->isArray())
  {
    return errorResult(
      CliError{ErrorCode::InternalError, "the request carries no arguments"});
  }

  auto result = Invocation{{}, request.payload};
  for (const auto& arg : args->asArray())
  {
    if (!arg.isString())
    {
      return errorResult(
        CliError{ErrorCode::InternalError, "the request's arguments are malformed"});
    }
    result.args.push_back(arg.asString());
  }
  return result;
}

std::string encodeReply(const RunResult& result)
{
  auto header = Json::object();
  header.set("err", result.err);
  header.set("exitCode", result.exitCode);
  return encodeFrame(header, result.out);
}

RunResult decodeReply(const Frame& reply)
{
  const auto* err = reply.header.find("err");
  const auto* exitCode = reply.header.find("exitCode");
  if (!err || !err->isString() || !exitCode || !exitCode->isNumber())
  {
    return errorResult(
      CliError{ErrorCode::InternalError, "the editor sent a malformed reply"});
  }
  return RunResult{reply.payload, err->asString(), int(exitCode->asNumber())};
}

std::optional<Frame> exchangeFrames(
  const std::string& endpoint, const std::string_view request)
{
#ifdef _WIN32
  const auto pipe = connectPipe(std::filesystem::path{endpoint}.wstring());
  if (pipe == INVALID_HANDLE_VALUE)
  {
    return std::nullopt;
  }
  const auto closer = HandleCloser{pipe};

  for (auto remaining = request; !remaining.empty();)
  {
    auto written = DWORD{0};
    const auto chunk = DWORD(std::min(remaining.size(), size_t{1} << 20));
    if (!WriteFile(pipe, remaining.data(), chunk, &written, nullptr))
    {
      // the editor closed the connection before reading the request
      return std::nullopt;
    }
    remaining.remove_prefix(written);
  }

  auto buffer = std::string{};
  auto chunk = std::string(65536, '\0');
  while (true)
  {
    if (auto frame = decodeFrame(buffer))
    {
      return std::move(frame->first);
    }
    auto read = DWORD{0};
    if (!ReadFile(pipe, chunk.data(), DWORD(chunk.size()), &read, nullptr) || read == 0)
    {
      throw CliError{
        ErrorCode::InternalError, "the editor closed the live link mid-reply"};
    }
    buffer.append(chunk.data(), read);
  }
#else
  const auto fd = connectSocket(endpoint);
  if (fd < 0)
  {
    return std::nullopt;
  }
  const auto closer = FdCloser{fd};

#ifdef MSG_NOSIGNAL
  // Linux: writing to a closed connection must fail, not kill tbcli with SIGPIPE
  constexpr auto sendFlags = int{MSG_NOSIGNAL};
#else
  constexpr auto sendFlags = int{0};
#endif
  for (auto remaining = request; !remaining.empty();)
  {
    const auto sent = ::send(fd, remaining.data(), remaining.size(), sendFlags);
    if (sent < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }
      // the editor closed the connection before reading the request
      return std::nullopt;
    }
    remaining.remove_prefix(size_t(sent));
  }

  auto buffer = std::string{};
  auto chunk = std::string(65536, '\0');
  while (true)
  {
    if (auto frame = decodeFrame(buffer))
    {
      return std::move(frame->first);
    }
    const auto received = ::recv(fd, chunk.data(), chunk.size(), 0);
    if (received < 0 && errno == EINTR)
    {
      continue;
    }
    if (received <= 0)
    {
      throw CliError{
        ErrorCode::InternalError, "the editor closed the live link mid-reply"};
    }
    buffer.append(chunk.data(), size_t(received));
  }
#endif
}

std::optional<RunResult> runLive(
  const std::vector<std::string>& args,
  const std::string_view in,
  const LinkMode mode,
  const std::filesystem::path& rendezvousDir)
{
  if (mode == LinkMode::File)
  {
    return std::nullopt;
  }

  const auto mapPath = liveMapArgument(args);
  if (!mapPath)
  {
    return std::nullopt;
  }

  const auto noSession = [&]() -> std::optional<RunResult> {
    if (mode == LinkMode::Live)
    {
      return errorResult(
        CliError{
          ErrorCode::NoSession,
          fmt::format("no running editor has {} open", mapPath->string())});
    }
    return std::nullopt;
  };

  // never falls back to file mode silently while the agent thinks the map is open
  const auto sessionEndedOrNoSession = [&]() -> std::optional<RunResult> {
    const auto endedFile = endedSessionPath(rendezvousDir, *mapPath);
    const auto endedSession = readEndedSession(endedFile);
    if (!endedSession)
    {
      return noSession();
    }

    // reported once, so the agent's next command runs in file mode
    auto error = std::error_code{};
    std::filesystem::remove(endedFile, error);
    return errorResult(sessionEndedError(*endedSession));
  };

#ifndef _WIN32
  if (
    const auto dirMode = modeOfOwnDirectory(rendezvousDir); !dirMode || *dirMode != 0700)
  {
    // the editor only publishes in a private directory, so someone else wrote this
    return noSession();
  }
#endif

  const auto rendezvous = readRendezvous(rendezvousPath(rendezvousDir, *mapPath));
  if (!rendezvous || !processIsAlive(rendezvous->pid))
  {
    // a dead editor's rendezvous file is left behind by a crash
    return sessionEndedOrNoSession();
  }
  if (rendezvous->proto != LinkProto)
  {
    return errorResult(
      CliError{
        ErrorCode::ProtoMismatch,
        fmt::format(
          "the editor speaks {}, but this tbcli speaks {}; use a tbcli of the editor's "
          "version",
          rendezvous->proto,
          LinkProto)});
  }
  if (!rendezvous->enabled)
  {
    return errorResult(
      CliError{
        ErrorCode::LinkDisabled,
        fmt::format(
          "TrenchBroom has {} open, but its \"Accept AI connections\" preference "
          "(Preferences > View) was off when the map was opened; ask the mapper to turn "
          "it on and reopen the map",
          rendezvous->map)});
  }

  try
  {
    const auto invocation = forwardableInvocation(args, std::string{in});
    const auto reply =
      exchangeFrames(rendezvous->endpoint, encodeRequest(invocation, rendezvous->token));
    if (!reply)
    {
      // the editor that wrote the rendezvous file is gone
      return sessionEndedOrNoSession();
    }
    return decodeReply(*reply);
  }
  catch (const CliError& e)
  {
    return errorResult(e);
  }
}

} // namespace tb::cli
