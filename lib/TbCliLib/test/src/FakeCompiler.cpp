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

/*
 * A stand-in for an ericw-tools compiler that replays a recording made by
 * scripts/record-ericw-tools.sh, so that tests can run tbcli's compile stages without
 * the real compilers.
 *
 * It is called like the real tool, with the map file name as its last argument, in the
 * working directory that holds the map. It reads its instructions from a file next to
 * itself with the same name and the extension .replay, which holds lines of
 * `<key> <value>`:
 *
 *   recording <dir>  the directory of one tool's recording: the files the tool wrote
 *                    and a file exit_code (required)
 *   name <name>      the name of the map the recording was made from, which the
 *                    recorded file names start with (required)
 *   sleep_ms <n>     wait this long before exiting, to let tests exercise timeouts
 *
 * It copies every recorded file into the working directory, with the recorded map name
 * replaced by the name of the map it is given, and with the ${WORKDIR} placeholder in
 * logs replaced by the working directory. Then it exits with the recorded exit code.
 *
 * For tests to check how it was called, it writes its arguments, one per line, to a
 * file next to itself with the extension .args, its working directory to a file with
 * the extension .cwd, and copies the map it was given to a file with the extension
 * .input.
 */

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <thread>

namespace
{

std::string readText(const std::filesystem::path& path)
{
  auto stream = std::ifstream{path, std::ios::binary};
  return std::string{
    std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void writeText(const std::filesystem::path& path, const std::string& text)
{
  auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
  stream << text;
}

std::string replaceAll(std::string text, const std::string& from, const std::string& to)
{
  for (auto pos = text.find(from); pos != std::string::npos;
       pos = text.find(from, pos + to.size()))
  {
    text.replace(pos, from.size(), to);
  }
  return text;
}

std::map<std::string, std::string> readReplay(const std::filesystem::path& path)
{
  auto result = std::map<std::string, std::string>{};
  auto stream = std::istringstream{readText(path)};
  for (auto line = std::string{}; std::getline(stream, line);)
  {
    if (!line.empty() && line.back() == '\r')
    {
      line.pop_back();
    }
    if (const auto space = line.find(' '); space != std::string::npos)
    {
      result[line.substr(0, space)] = line.substr(space + 1);
    }
  }
  return result;
}

} // namespace

int main(int argc, char** argv)
{
  if (argc < 2)
  {
    std::cerr << "usage: " << argv[0] << " [options] <map>\n";
    return 2;
  }

  const auto self = std::filesystem::absolute(argv[0]);
  auto base = self;
  base.replace_extension();

  auto args = std::string{};
  for (auto i = 1; i < argc; ++i)
  {
    args += argv[i];
    args += '\n';
  }
  writeText(base.string() + ".args", args);
  writeText(base.string() + ".cwd", std::filesystem::current_path().string());

  const auto mapPath = std::filesystem::path{argv[argc - 1]};
  std::filesystem::copy_file(
    mapPath, base.string() + ".input", std::filesystem::copy_options::overwrite_existing);

  const auto replay = readReplay(base.string() + ".replay");
  const auto recording = replay.find("recording");
  const auto recordedName = replay.find("name");
  if (recording == replay.end() || recordedName == replay.end())
  {
    std::cerr << "the .replay file needs a recording and a name\n";
    return 2;
  }

  const auto workDir = std::filesystem::current_path();
  const auto name = mapPath.stem().string();
  auto exitCode = 0;
  for (const auto& entry :
       std::filesystem::directory_iterator{std::filesystem::path{recording->second}})
  {
    const auto fileName = entry.path().filename().string();
    if (fileName == "exit_code")
    {
      exitCode = std::stoi(readText(entry.path()));
      continue;
    }

    const auto targetName = fileName.starts_with(recordedName->second)
                              ? name + fileName.substr(recordedName->second.size())
                              : fileName;
    if (entry.path().extension() == ".log")
    {
      auto log = replaceAll(readText(entry.path()), "${WORKDIR}", workDir.string());
      writeText(
        workDir / targetName, replaceAll(log, recordedName->second + ".", name + "."));
    }
    else
    {
      std::filesystem::copy_file(
        entry.path(),
        workDir / targetName,
        std::filesystem::copy_options::overwrite_existing);
    }
  }

  if (const auto sleep = replay.find("sleep_ms"); sleep != replay.end())
  {
    std::this_thread::sleep_for(std::chrono::milliseconds{std::stoi(sleep->second)});
  }

  return exitCode;
}
