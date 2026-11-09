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

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
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

/** The path that names a map's session: absolute, and lower-cased on Windows. */
std::string sessionKey(const std::filesystem::path& mapPath)
{
  const auto path = std::filesystem::absolute(mapPath).lexically_normal();
#ifdef _WIN32
  auto wide = path.wstring();
  CharLowerBuffW(wide.data(), DWORD(wide.size()));
  const auto key = std::filesystem::path{wide}.generic_u8string();
#else
  const auto key = path.generic_u8string();
#endif
  return std::string{key.begin(), key.end()};
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

#endif

} // namespace

std::filesystem::path defaultRendezvousDir()
{
#ifdef _WIN32
  if (const auto* localAppData = _wgetenv(L"LOCALAPPDATA"))
  {
    return std::filesystem::path{localAppData} / "TrenchBroom" / "live";
  }
#else
  if (const auto* runtimeDir = std::getenv("XDG_RUNTIME_DIR"))
  {
    return std::filesystem::path{runtimeDir} / "trenchbroom-live";
  }
  if (const auto* home = std::getenv("HOME"))
  {
    return std::filesystem::path{home} / ".cache" / "trenchbroom" / "live";
  }
#endif
  return std::filesystem::temp_directory_path() / "trenchbroom-live";
}

std::filesystem::path rendezvousPath(
  const std::filesystem::path& rendezvousDir, const std::filesystem::path& mapPath)
{
  return rendezvousDir / (sha256Hex(sessionKey(mapPath)).substr(0, 16) + ".json");
}

std::string encodeRendezvous(const Rendezvous& rendezvous)
{
  auto json = Json::object();
  json.set("proto", rendezvous.proto);
  json.set("map", rendezvous.map);
  json.set("endpoint", rendezvous.endpoint);
  json.set("token", rendezvous.token);
  json.set("pid", rendezvous.pid);
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
  if (!proto || !map || !endpoint || !token || !pid || !pid->isNumber())
  {
    return std::nullopt;
  }
  return Rendezvous{*proto, *map, *endpoint, *token, int64_t(pid->asNumber())};
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
  // the Unix domain socket client is not written yet
  static_cast<void>(endpoint);
  static_cast<void>(request);
  return std::nullopt;
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

  const auto rendezvous = readRendezvous(rendezvousPath(rendezvousDir, *mapPath));
  if (!rendezvous)
  {
    return noSession();
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

  try
  {
    const auto invocation = forwardableInvocation(args, std::string{in});
    const auto reply =
      exchangeFrames(rendezvous->endpoint, encodeRequest(invocation, rendezvous->token));
    if (!reply)
    {
      // the editor that wrote the rendezvous file is gone
      return noSession();
    }
    return decodeReply(*reply);
  }
  catch (const CliError& e)
  {
    return errorResult(e);
  }
}

} // namespace tb::cli
