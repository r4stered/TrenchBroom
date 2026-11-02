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

#include "cli/Process.h"

#include "cli/CliError.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <cstdlib>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <thread>
#endif

namespace tb::cli
{
namespace
{

CliError cannotStart(const std::filesystem::path& executable, const std::string& reason)
{
  return CliError{
    ErrorCode::InternalError, fmt::format("cannot run {}: {}", executable, reason)};
}

#ifdef _WIN32

/**
 * Quotes an argument so that the C runtime's command line parser (CommandLineToArgvW)
 * gives it back unchanged.
 */
std::wstring quoteArgument(const std::wstring& argument)
{
  if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
  {
    return argument;
  }

  auto result = std::wstring{L"\""};
  for (auto it = argument.begin();; ++it)
  {
    auto backslashes = size_t{0};
    while (it != argument.end() && *it == L'\\')
    {
      ++it;
      ++backslashes;
    }

    if (it == argument.end())
    {
      // double the backslashes so that the closing quote is not escaped
      result.append(backslashes * 2, L'\\');
      break;
    }
    if (*it == L'"')
    {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(L'"');
    }
    else
    {
      result.append(backslashes, L'\\');
      result.push_back(*it);
    }
  }
  result.push_back(L'"');
  return result;
}

std::string lastErrorMessage()
{
  return fmt::format("error {}", GetLastError());
}

/** Closes a handle when it goes out of scope. */
struct HandleGuard
{
  HANDLE handle;

  ~HandleGuard()
  {
    if (handle && handle != INVALID_HANDLE_VALUE)
    {
      CloseHandle(handle);
    }
  }
};

#endif

} // namespace

ProcessResult runProcess(
  const std::filesystem::path& executable,
  const std::vector<std::string>& arguments,
  const std::filesystem::path& workDir,
  const std::chrono::milliseconds timeout)
{
#ifdef _WIN32
  auto commandLine = quoteArgument(executable.wstring());
  for (const auto& argument : arguments)
  {
    commandLine += L" ";
    commandLine += quoteArgument(std::filesystem::path{argument}.wstring());
  }

  auto security = SECURITY_ATTRIBUTES{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  const auto nul = HandleGuard{CreateFileW(
    L"NUL",
    GENERIC_READ | GENERIC_WRITE,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
    &security,
    OPEN_EXISTING,
    0,
    nullptr)};
  if (nul.handle == INVALID_HANDLE_VALUE)
  {
    throw cannotStart(executable, lastErrorMessage());
  }

  auto startup = STARTUPINFOW{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = nul.handle;
  startup.hStdOutput = nul.handle;
  startup.hStdError = nul.handle;

  auto info = PROCESS_INFORMATION{};
  const auto workDirString = workDir.wstring();
  if (!CreateProcessW(
        executable.wstring().c_str(),
        commandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        workDirString.c_str(),
        &startup,
        &info))
  {
    throw cannotStart(executable, lastErrorMessage());
  }
  const auto process = HandleGuard{info.hProcess};
  const auto thread = HandleGuard{info.hThread};

  // INFINITE is the largest DWORD, so a longer timeout waits as long as possible
  constexpr auto MaxWait = std::chrono::milliseconds{INFINITE - 1};
  const auto wait = DWORD(std::min(timeout, MaxWait).count());
  auto result = ProcessResult{};
  if (WaitForSingleObject(process.handle, wait) == WAIT_TIMEOUT)
  {
    TerminateProcess(process.handle, 1);
    WaitForSingleObject(process.handle, INFINITE);
    result.timedOut = true;
    return result;
  }

  auto exitCode = DWORD{0};
  GetExitCodeProcess(process.handle, &exitCode);
  result.exitCode = int(exitCode);
  return result;
#else
  auto argv = std::vector<char*>{};
  auto executableString = executable.string();
  argv.push_back(executableString.data());
  auto argumentStrings = arguments;
  for (auto& argument : argumentStrings)
  {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);

  // the child reports a failed chdir or exec through this pipe; a successful exec
  // closes it
  int errorPipe[2];
  if (
    pipe(errorPipe) != 0 || fcntl(errorPipe[0], F_SETFD, FD_CLOEXEC) != 0
    || fcntl(errorPipe[1], F_SETFD, FD_CLOEXEC) != 0)
  {
    throw cannotStart(executable, "pipe failed");
  }

  const auto pid = fork();
  if (pid < 0)
  {
    close(errorPipe[0]);
    close(errorPipe[1]);
    throw cannotStart(executable, "fork failed");
  }
  if (pid == 0)
  {
    // only async-signal-safe calls between fork and exec
    close(errorPipe[0]);
    if (chdir(workDir.c_str()) == 0)
    {
      if (const auto nul = open("/dev/null", O_RDWR); nul >= 0)
      {
        dup2(nul, STDIN_FILENO);
        dup2(nul, STDOUT_FILENO);
        dup2(nul, STDERR_FILENO);
      }
      execv(argv[0], argv.data());
    }
    const auto error = errno;
    [[maybe_unused]] const auto written = write(errorPipe[1], &error, sizeof(error));
    _exit(127);
  }

  close(errorPipe[1]);
  auto childError = 0;
  const auto readCount = read(errorPipe[0], &childError, sizeof(childError));
  close(errorPipe[0]);
  if (readCount == static_cast<ssize_t>(sizeof(childError)))
  {
    waitpid(pid, nullptr, 0);
    throw cannotStart(executable, std::strerror(childError));
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto status = 0;
  while (true)
  {
    const auto waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid)
    {
      break;
    }
    if (waited < 0)
    {
      throw cannotStart(executable, "waiting for the process failed");
    }
    if (std::chrono::steady_clock::now() >= deadline)
    {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      return ProcessResult{std::nullopt, true};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }

  // a process killed by a signal counts as failed, with the shell's exit code for it
  return ProcessResult{
    WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status), false};
#endif
}

bool isBareName(const std::filesystem::path& path)
{
  return !path.has_parent_path() && !path.is_absolute();
}

std::optional<std::filesystem::path> findExecutable(const std::filesystem::path& path)
{
  const auto candidates = [](const std::filesystem::path& p) {
    auto result = std::vector<std::filesystem::path>{p};
#ifdef _WIN32
    if (!p.has_extension())
    {
      auto exe = p;
      exe += ".exe";
      result.push_back(std::move(exe));
    }
#endif
    return result;
  };

  const auto firstFile =
    [&](const std::filesystem::path& p) -> std::optional<std::filesystem::path> {
    for (const auto& candidate : candidates(p))
    {
      auto ec = std::error_code{};
#ifdef _WIN32
      if (std::filesystem::is_regular_file(candidate, ec))
#else
      if (
        std::filesystem::is_regular_file(candidate, ec)
        && access(candidate.c_str(), X_OK) == 0)
#endif
      {
        return candidate;
      }
    }
    return std::nullopt;
  };

  if (!isBareName(path))
  {
    return firstFile(path);
  }

  const auto* pathVariable = std::getenv("PATH");
  if (!pathVariable)
  {
    return std::nullopt;
  }

#ifdef _WIN32
  constexpr auto Separator = ';';
#else
  constexpr auto Separator = ':';
#endif
  auto dirs = std::string_view{pathVariable};
  while (!dirs.empty())
  {
    const auto end = dirs.find(Separator);
    const auto dir = dirs.substr(0, end);
    if (!dir.empty())
    {
      if (auto found = firstFile(std::filesystem::path{dir} / path))
      {
        return found;
      }
    }
    dirs = end == std::string_view::npos ? std::string_view{} : dirs.substr(end + 1);
  }
  return std::nullopt;
}

} // namespace tb::cli
