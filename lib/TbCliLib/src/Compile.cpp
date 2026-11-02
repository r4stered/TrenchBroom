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

#include "cli/Compile.h"

#include "cli/CliError.h"
#include "cli/FileUtils.h"
#include "cli/Json.h"
#include "cli/Leak.h"
#include "cli/ParseUtils.h"
#include "cli/Timing.h"

#include "kd/ranges/to.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <array>
#include <random>
#include <ranges>
#include <regex>

namespace tb::cli
{
namespace
{

constexpr auto ErrorBanner = std::string_view{"************ ERROR ************"};
constexpr auto WarningPrefix = std::string_view{"WARNING: "};
constexpr auto ErrorPrefix = std::string_view{"ERROR: "};

/** How a message names a line of the map: `<map>[line N]`. */
const std::regex& linePattern()
{
  static const auto pattern = std::regex{R"(\[line (\d+)\])"};
  return pattern;
}

/** The artifacts that --artifacts keeps, by what follows the map name. */
constexpr auto ArtifactExtensions = std::array<std::string_view, 6>{
  ".bsp",
  ".pts",
  ".prt",
  ".log",
  "-vis.log",
  "-light.log",
};

/** The artifacts that only a successful run leaves usable, by extension. */
constexpr auto CompiledExtensions = std::array<std::string_view, 2>{
  ".bsp",
  ".prt",
};

std::string_view trimEnd(std::string_view str)
{
  while (!str.empty() && (str.back() == '\r' || str.back() == ' '))
  {
    str.remove_suffix(1);
  }
  return str;
}

/** Removes every occurrence of the given strings from the text. */
std::string removeAll(std::string text, const std::vector<std::string>& strings)
{
  for (const auto& str : strings)
  {
    if (str.empty())
    {
      continue;
    }
    for (auto pos = text.find(str); pos != std::string::npos; pos = text.find(str, pos))
    {
      text.erase(pos, str.size());
    }
  }
  return text;
}

/** The spellings of the working directory with a trailing separator, longest first. */
std::vector<std::string> workDirPrefixes(const std::filesystem::path& workDir)
{
  if (workDir.empty())
  {
    return {};
  }
  const auto native = workDir.string();
  const auto generic = workDir.generic_string();
  return {native + "\\", native + "/", generic + "/"};
}

CompilerMessage parseMessage(const Severity severity, std::string message)
{
  static const auto originPattern =
    std::regex{R"re("?([A-Za-z_][A-Za-z0-9_]*)"? at [(\[]?(\S+) (\S+) ([^\s)\]]+))re"};

  auto result = CompilerMessage{severity, std::move(message)};
  if (auto match = std::smatch{}; std::regex_search(result.message, match, linePattern()))
  {
    result.line = parseUnsigned(match[1].str());
  }
  for (auto it =
         std::sregex_iterator{
           result.message.begin(), result.message.end(), originPattern};
       it != std::sregex_iterator{};
       ++it)
  {
    const auto& match = *it;
    const auto x = parseDecimal(match[2].str());
    const auto y = parseDecimal(match[3].str());
    const auto z = parseDecimal(match[4].str());
    if (x && y && z)
    {
      result.classname = match[1].str();
      result.origin = vm::vec3d{*x, *y, *z};
      break;
    }
  }
  return result;
}

/** A new, empty temporary directory that is deleted when this goes out of scope. */
class TemporaryDirectory
{
private:
  std::filesystem::path m_path;

public:
  TemporaryDirectory()
  {
    auto random = std::random_device{};
    auto ec = std::error_code{};
    const auto base = std::filesystem::temp_directory_path(ec);
    if (ec)
    {
      throw CliError{
        ErrorCode::WriteFailed,
        fmt::format("cannot find the temporary directory: {}", ec.message())};
    }
    for (auto attempt = 0; attempt < 16; ++attempt)
    {
      auto path = base / fmt::format("tbcli-compile-{:08x}", random());
      if (std::filesystem::create_directory(path, ec))
      {
        m_path = std::move(path);
        return;
      }
    }
    throw CliError{
      ErrorCode::WriteFailed,
      fmt::format("cannot create a temporary directory in {}", base)};
  }

  ~TemporaryDirectory()
  {
    auto ec = std::error_code{};
    std::filesystem::remove_all(m_path, ec);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  const std::filesystem::path& path() const { return m_path; }
};

void writeOrThrow(const std::filesystem::path& path, const std::string_view bytes)
{
  if (auto error = writeFile(path, bytes))
  {
    throw CliError{ErrorCode::WriteFailed, std::move(*error)};
  }
}

/** A compiler that a compile runs, with its configuration. */
struct Tool
{
  const CompilerTool& compiler;
  const ToolConfig& config;
};

/** The tools that the given level runs, in the order they run. */
std::vector<Tool> toolsOf(const CompileOptions& options)
{
  const auto count = size_t(options.level) + 1;
  return CompilerTools | std::views::take(count)
         | std::views::transform([&](const CompilerTool& compiler) {
             return Tool{compiler, options.compilers.*compiler.config};
           })
         | kdl::ranges::to<std::vector<Tool>>();
}

std::filesystem::path executableOf(const Tool& tool)
{
  const auto& name = tool.compiler.name;
  const auto& configured = tool.config.path;
  if (!configured)
  {
    throw CliError{
      ErrorCode::CompilerMissing,
      fmt::format(
        "{0} is not configured: set compilers.{0} in tbcli.json or pass --{0}", name)};
  }
  if (auto found = findExecutable(*configured))
  {
    return std::filesystem::absolute(*found);
  }
  throw CliError{
    ErrorCode::CompilerMissing, fmt::format("{} not found at {}", name, *configured)};
}

ToolRun runTool(
  const Tool& tool,
  const std::filesystem::path& executable,
  const std::filesystem::path& workDir,
  const std::string& mapName,
  const std::optional<std::chrono::milliseconds> timeoutOverride)
{
  const auto& compiler = tool.compiler;
  const auto input = mapName + std::string{compiler.input};
  const auto arguments = compiler.flag.empty()
                           ? std::vector<std::string>{input}
                           : std::vector<std::string>{std::string{compiler.flag}, input};
  const auto timeout =
    timeoutOverride.value_or(tool.config.timeout.value_or(compiler.defaultTimeout));

  auto result = ToolRun{std::string{compiler.name}, {}, timeout};
  const auto start = std::chrono::steady_clock::now();
  result.process = runProcess(executable, arguments, workDir, timeout);
  result.ms = msSince(start);
  if (const auto log = readFile(workDir / (mapName + std::string{compiler.log})))
  {
    result.messages = parseCompilerLog(*log, workDir);
  }
  return result;
}

/**
 * Copies the artifacts of the compile in `workDir` to `artifacts`, removes those of an
 * earlier compile that this one did not produce, and writes the sidecar. A failed run
 * keeps only its log and pointfile, since a .bsp or .prt it left may be partial.
 */
void keepArtifacts(
  const std::filesystem::path& workDir,
  const std::string& mapName,
  const std::filesystem::path& artifacts,
  const std::string& rev,
  const CompileResult& result)
{
  const auto failedRun =
    std::ranges::any_of(result.runs, [](const auto& run) { return run.failed(); });
  const auto failed = [&](const std::string& what, const std::error_code& ec) {
    return CliError{
      ErrorCode::WriteFailed,
      fmt::format("cannot {} in {}: {}", what, artifacts, ec.message())};
  };

  auto ec = std::error_code{};
  std::filesystem::create_directories(artifacts, ec);
  if (ec)
  {
    throw failed("create the artifacts directory", ec);
  }

  for (const auto extension : ArtifactExtensions)
  {
    const auto fileName = mapName + std::string{extension};
    const auto source = workDir / fileName;
    const auto target = artifacts / fileName;
    const auto usable =
      !failedRun
      || std::ranges::find(CompiledExtensions, extension) == CompiledExtensions.end();
    if (usable && std::filesystem::is_regular_file(source, ec))
    {
      std::filesystem::copy_file(
        source, target, std::filesystem::copy_options::overwrite_existing, ec);
      if (ec)
      {
        throw failed(fmt::format("copy {}", fileName), ec);
      }
    }
    else
    {
      std::filesystem::remove(target, ec);
      if (ec)
      {
        throw failed(fmt::format("remove the stale {}", fileName), ec);
      }
    }
  }

  auto sidecar = Json::object();
  sidecar.set("rev", rev);
  sidecar.set("leaked", result.leaked);
  writeOrThrow(artifacts / (mapName + ".bsp.tbcli.json"), sidecar.dump() + "\n");
}

} // namespace

std::vector<CompilerMessage> parseCompilerLog(
  const std::string_view log, const std::filesystem::path& workDir)
{
  const auto prefixes = workDirPrefixes(workDir);
  auto result = std::vector<CompilerMessage>{};
  auto errorFollows = false;
  for (const auto range : std::views::split(log, '\n'))
  {
    const auto line = trimEnd(std::string_view{range.begin(), range.end()});
    const auto add = [&](const Severity severity, const std::string_view message) {
      result.push_back(parseMessage(severity, removeAll(std::string{message}, prefixes)));
    };

    if (errorFollows)
    {
      if (!line.empty())
      {
        add(Severity::Error, line);
        errorFollows = false;
      }
    }
    else if (line == ErrorBanner)
    {
      errorFollows = true;
    }
    else if (line.starts_with(WarningPrefix))
    {
      add(Severity::Warning, line.substr(WarningPrefix.size()));
    }
    else if (line.starts_with(ErrorPrefix))
    {
      add(Severity::Error, line.substr(ErrorPrefix.size()));
    }
  }
  return result;
}

const CompilerTool* findCompilerTool(const std::string_view name)
{
  const auto it = std::ranges::find(CompilerTools, name, &CompilerTool::name);
  return it != CompilerTools.end() ? &*it : nullptr;
}

std::string withoutLineNumbers(const std::string& message)
{
  return std::regex_replace(message, linePattern(), "");
}

bool ToolRun::failed() const
{
  return process.timedOut || process.exitCode != 0;
}

CompileResult compile(
  const std::string_view mapBytes,
  const std::string& mapName,
  const std::string& rev,
  const CompileOptions& options)
{
  const auto tools = toolsOf(options);
  // every tool is looked up before any runs, so that a missing one fails fast
  const auto executables = tools | std::views::transform(executableOf)
                           | kdl::ranges::to<std::vector<std::filesystem::path>>();

  const auto workDir = TemporaryDirectory{};
  writeOrThrow(workDir.path() / (mapName + ".map"), mapBytes);

  // qbsp runs first
  auto result = CompileResult{};
  result.runs.push_back(
    runTool(tools[0], executables[0], workDir.path(), mapName, options.timeout));
  const auto qbspFailed = result.runs.front().failed();

  const auto pointFile = workDir.path() / (mapName + ".pts");
  auto ec = std::error_code{};
  result.leaked = std::filesystem::is_regular_file(pointFile, ec);
  if (const auto text = result.leaked ? readFile(pointFile) : std::nullopt)
  {
    result.leakTrail = parsePointFile(*text).value_or(std::vector<vm::vec3d>{});
  }

  // then vis and light, unless there is no usable bsp
  for (size_t i = 1; i < tools.size(); ++i)
  {
    if (result.leaked || qbspFailed)
    {
      result.skipped.emplace_back(tools[i].compiler.name);
    }
    else
    {
      result.runs.push_back(
        runTool(tools[i], executables[i], workDir.path(), mapName, options.timeout));
    }
  }

  if (options.artifacts)
  {
    keepArtifacts(workDir.path(), mapName, *options.artifacts, rev, result);
  }
  return result;
}

} // namespace tb::cli
