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

#include "ui/LiveLinkHost.h"

#include <QApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRandomGenerator>
#include <QTimer>

#include "base/Logger.h"
#include "cli/CliError.h"
#include "cli/FileUtils.h"
#include "cli/Json.h"
#include "cli/LiveLink.h"
#include "cli/Run.h"
#include "mdl/CommandProcessor.h"
#include "mdl/Map.h"
#include "ui/MapDocument.h"

#include "kd/overload.h"

#include <fmt/format.h>

#include <variant>

namespace tb::ui
{
namespace
{

using Clock = std::chrono::steady_clock;

/** A random hex string of the given number of 32 bit words, for names and tokens. */
std::string randomHex(const int words)
{
  auto result = std::string{};
  for (int i = 0; i < words; ++i)
  {
    result += fmt::format("{:08x}", QRandomGenerator::system()->generate());
  }
  return result;
}

/** The error code that a result reports, if it failed with an error envelope. */
std::string errorCode(const cli::RunResult& result)
{
  if (const auto json = cli::parseJson(result.out); json && json.value().isObject())
  {
    if (const auto* error = json.value().find("error"); error && error->isObject())
    {
      if (const auto* code = error->find("code"); code && code->isString())
      {
        return code->asString();
      }
    }
  }
  return fmt::format("exit {}", result.exitCode);
}

} // namespace

struct LiveLinkHost::Request
{
  QPointer<QLocalSocket> socket;
  cli::Invocation invocation;
  Clock::time_point deadline;
};

LiveLinkHost::LiveLinkHost(
  MapDocument& document, std::filesystem::path rendezvousDir, QObject* parent)
  : QObject{parent}
  , m_document{document}
  , m_rendezvousDir{std::move(rendezvousDir)}
  , m_retryTimer{new QTimer{this}}
{
  m_retryTimer->setSingleShot(true);
  connect(m_retryTimer, &QTimer::timeout, this, &LiveLinkHost::serveQueue);

  m_notifierConnection +=
    m_document.documentWasLoadedNotifier.connect(this, &LiveLinkHost::documentWasLoaded);

  start();
}

LiveLinkHost::~LiveLinkHost()
{
  stop();
}

void LiveLinkHost::setBusyTimeout(const std::chrono::milliseconds busyTimeout)
{
  m_busyTimeout = busyTimeout;
}

const std::optional<std::filesystem::path>& LiveLinkHost::rendezvousFile() const
{
  return m_rendezvousFile;
}

size_t LiveLinkHost::queuedRequests() const
{
  return m_queue.size();
}

mdl::Map& LiveLinkHost::map()
{
  return m_document.map();
}

std::string LiveLinkHost::epoch() const
{
  return m_rev->epoch();
}

std::string LiveLinkHost::rev() const
{
  return m_rev->str();
}

void LiveLinkHost::documentWasLoaded()
{
  // a loaded or reloaded map is a new map, so its revs start a new epoch
  stop();
  start();
}

void LiveLinkHost::start()
{
  if (!cli::HasLinkClient)
  {
    // tbcli could not reach the editor, so it would edit the file under the editor
    return;
  }

  const auto& path = m_document.map().path();
  if (
    auto error = std::error_code{};
    !path.is_absolute() || !std::filesystem::is_regular_file(path, error))
  {
    // a map that was never saved has no path to name its session by
    return;
  }

  m_rev = std::make_unique<cli::LiveRev>(m_document.map());
  m_token = randomHex(4);

  m_server = new QLocalServer{this};
  // on Windows, this restricts the pipe to the current user
  m_server->setSocketOptions(QLocalServer::UserAccessOption);
  const auto name = QString::fromStdString("TrenchBroom-live-" + randomHex(4));
  if (!m_server->listen(name))
  {
    m_document.logger().warn() << "Live link: cannot listen on a local socket: "
                               << m_server->errorString().toStdString();
    stop();
    return;
  }
  connect(m_server, &QLocalServer::newConnection, this, &LiveLinkHost::newConnection);

  const auto rendezvousFile = cli::rendezvousPath(m_rendezvousDir, path);
  auto error = std::error_code{};
  std::filesystem::create_directories(m_rendezvousDir, error);
  const auto rendezvous = cli::Rendezvous{
    .map = path.string(),
    .endpoint = m_server->fullServerName().toStdString(),
    .token = m_token,
    .pid = int64_t(QCoreApplication::applicationPid()),
  };
  if (
    const auto writeError =
      cli::writeFileAtomically(rendezvousFile, cli::encodeRendezvous(rendezvous)))
  {
    m_document.logger().warn() << "Live link: cannot write " << rendezvousFile.string()
                               << ": " << *writeError;
    stop();
    return;
  }
  m_rendezvousFile = rendezvousFile;
}

void LiveLinkHost::stop()
{
  m_retryTimer->stop();
  m_queue.clear();
  if (m_server)
  {
    // closes the connections of the requests that were still waiting
    m_server->close();
    delete m_server;
    m_server = nullptr;
  }
  if (m_rendezvousFile)
  {
    auto error = std::error_code{};
    std::filesystem::remove(*m_rendezvousFile, error);
    m_rendezvousFile.reset();
  }
}

void LiveLinkHost::newConnection()
{
  while (auto* socket = m_server->nextPendingConnection())
  {
    connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
    auto buffer = std::make_shared<std::string>();
    connect(socket, &QLocalSocket::readyRead, this, [this, socket, buffer]() {
      // readRequest disconnects this slot when the request is complete
      const auto keepAlive = buffer;
      readRequest(socket, *keepAlive);
    });
  }
}

void LiveLinkHost::readRequest(QLocalSocket* socket, std::string& buffer)
{
  const auto bytes = socket->readAll();
  buffer.append(bytes.constData(), size_t(bytes.size()));

  try
  {
    const auto frame = cli::decodeFrame(buffer);
    if (!frame)
    {
      return;
    }
    // one request per connection
    socket->disconnect(this);
    buffer.clear();

    std::visit(
      kdl::overload(
        [&](cli::Invocation invocation) {
          m_queue.push_back(
            std::make_unique<Request>(
              socket, std::move(invocation), Clock::now() + m_busyTimeout));
          serveQueue();
        },
        [&](const cli::RunResult& refusal) { reply(socket, refusal); }),
      cli::decodeRequest(frame->first, m_token));
  }
  catch (const cli::CliError& e)
  {
    socket->disconnect(this);
    reply(socket, cli::errorResult(e));
  }
}

void LiveLinkHost::serveQueue()
{
  while (!m_queue.empty())
  {
    auto& request = *m_queue.front();
    if (!request.socket)
    {
      // tbcli gave up waiting
      m_queue.pop_front();
      continue;
    }

    if (isMapperBusy())
    {
      if (Clock::now() < request.deadline)
      {
        if (!m_retryTimer->isActive())
        {
          m_retryTimer->start(BusyRetryInterval);
        }
        return;
      }
      reply(
        request.socket,
        cli::errorResult(
          cli::CliError{
            cli::ErrorCode::Busy,
            fmt::format(
              "the mapper has been busy (in a dialog, a drag or an action) for {:g} s; "
              "retry later",
              std::chrono::duration<double>(m_busyTimeout).count())}));
      m_queue.pop_front();
      continue;
    }

    const auto served = std::move(m_queue.front());
    m_queue.pop_front();
    reply(served->socket, serve(served->invocation));
  }
}

bool LiveLinkHost::isMapperBusy() const
{
  return QApplication::activeModalWidget() != nullptr
         || QApplication::mouseButtons() != Qt::NoButton
         || m_document.map().commandProcessor().isTransactionActive();
}

cli::RunResult LiveLinkHost::serve(const cli::Invocation& invocation)
{
  auto result = cli::run(invocation.args, invocation.in, *this);
  if (const auto command = cli::mutatingCommand(invocation.args))
  {
    // every change that the agent makes gets a line in the Console
    const auto outcome =
      result.exitCode == 0 ? std::string{"done"} : "failed: " + errorCode(result);
    m_document.logger().info() << fmt::format("AI: {} {} ({})", *command, outcome, rev());
  }
  return result;
}

void LiveLinkHost::reply(QLocalSocket* socket, const cli::RunResult& result)
{
  if (!socket)
  {
    return;
  }
  const auto frame = cli::encodeReply(result);
  socket->write(frame.data(), qint64(frame.size()));
  // waits until the reply is written
  socket->disconnectFromServer();
}

} // namespace tb::ui
