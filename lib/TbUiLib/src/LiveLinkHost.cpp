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
#include "base/PreferenceManager.h"
#include "cli/Apply.h"
#include "cli/CliError.h"
#include "cli/FileUtils.h"
#include "cli/Json.h"
#include "cli/LiveLink.h"
#include "cli/NodeHandles.h"
#include "cli/Run.h"
#include "cli/View.h"
#include "mdl/CommandProcessor.h"
#include "mdl/Map.h"
#include "prefs/Preferences.h"
#include "ui/MapDocument.h"

#include "kd/overload.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <utility>
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

// an em dash in UTF-8, since MSVC reads the source in the system code page
constexpr auto Dash = "\xE2\x80\x94";

/** The string member of a JSON object with the given key, if there is one. */
std::optional<std::string> stringMember(const cli::Json& json, const std::string_view key)
{
  if (json.isObject())
  {
    if (const auto* member = json.find(key); member && member->isString())
    {
      return member->asString();
    }
  }
  return std::nullopt;
}

/** The output of a result, if it is JSON. */
cli::Json output(const cli::RunResult& result)
{
  auto json = cli::parseJson(result.out);
  return json ? std::move(json).value() : cli::Json{};
}

/** Why a request failed, `<code>: <message>` from its error envelope or its exit code. */
std::string failure(const cli::RunResult& result)
{
  if (const auto json = output(result); json.isObject())
  {
    if (const auto* error = json.find("error"))
    {
      if (const auto code = stringMember(*error, "code"))
      {
        const auto message = stringMember(*error, "message");
        return message ? fmt::format("{}: {}", *code, *message) : *code;
      }
    }
  }
  return fmt::format("exit {}", result.exitCode);
}

/** How many nodes a request created, modified and removed, as the Console shows it. */
std::string countChanges(const std::vector<cli::NodeChange>& changes)
{
  const auto count = [&](const cli::ChangeKind kind) {
    return std::ranges::count_if(
      changes, [&](const auto& change) { return change.kind == kind; });
  };

  auto counts = std::vector<std::string>{};
  for (const auto& [kind, name] : {
         std::pair{cli::ChangeKind::Created, "created"},
         std::pair{cli::ChangeKind::Modified, "modified"},
         std::pair{cli::ChangeKind::Removed, "removed"},
       })
  {
    if (const auto n = count(kind); n > 0)
    {
      counts.push_back(fmt::format("{} {}", n, name));
    }
  }
  return counts.empty() ? std::string{"no changes"}
                        : fmt::format("{}", fmt::join(counts, ", "));
}

} // namespace

struct LiveLinkHost::Request
{
  QPointer<QLocalSocket> socket;
  cli::Invocation invocation;
  /** See cli::requestLabel. */
  std::string label;
  Clock::time_point deadline;
};

LiveLinkHost::LiveLinkHost(
  MapDocument& document, std::filesystem::path rendezvousDir, QObject* parent)
  : QObject{parent}
  , m_document{document}
  , m_rendezvousDir{std::move(rendezvousDir)}
  , m_retryTimer{new QTimer{this}}
  , m_idleTimer{new QTimer{this}}
  , m_lookedTimer{new QTimer{this}}
{
  m_retryTimer->setSingleShot(true);
  connect(m_retryTimer, &QTimer::timeout, this, &LiveLinkHost::serveQueue);
  m_idleTimer->setSingleShot(true);
  connect(m_idleTimer, &QTimer::timeout, this, &LiveLinkHost::statusChanged);
  m_lookedTimer->setSingleShot(true);
  connect(m_lookedTimer, &QTimer::timeout, this, &LiveLinkHost::statusChanged);

  m_notifierConnection +=
    m_document.documentWasLoadedNotifier.connect(this, &LiveLinkHost::documentWasLoaded);
  m_notifierConnection +=
    m_document.documentWasSavedNotifier.connect(this, &LiveLinkHost::documentWasSaved);

  m_acceptConnections = pref(Preferences::AcceptAiConnections);
  start();
}

LiveLinkHost::~LiveLinkHost()
{
  // the status widget may be gone already
  blockSignals(true);
  endSession();
}

void LiveLinkHost::setBusyTimeout(const std::chrono::milliseconds busyTimeout)
{
  m_busyTimeout = busyTimeout;
}

void LiveLinkHost::setIdleTimeout(const std::chrono::milliseconds idleTimeout)
{
  m_idleTimeout = idleTimeout;
}

void LiveLinkHost::setLookedDuration(const std::chrono::milliseconds lookedDuration)
{
  m_lookedDuration = lookedDuration;
}

void LiveLinkHost::setWorkingDelay(const std::chrono::milliseconds workingDelay)
{
  m_workingDelay = workingDelay;
}

LiveLinkStatus LiveLinkHost::status() const
{
  using State = LiveLinkStatus::State;
  const auto state = !m_sessionMap               ? State::Off
                     : !m_acceptConnections      ? State::Disabled
                     : m_working                 ? State::Working
                     : m_lookedTimer->isActive() ? State::Looked
                     : m_idleTimer->isActive()   ? State::Active
                                                 : State::Ready;
  return LiveLinkStatus{state, m_working.value_or(std::string{}), m_lastRequest};
}

void LiveLinkHost::setViewCapturer(ViewCapturer viewCapturer)
{
  m_viewCapturer = std::move(viewCapturer);
}

void LiveLinkHost::setAgentViewRenderer(AgentViewRenderer agentViewRenderer)
{
  m_agentViewRenderer = std::move(agentViewRenderer);
}

LiveLinkHost::ShowOutcome LiveLinkHost::show(const cli::ViewCapture& capture)
{
  if (!m_sessionMap)
  {
    return ShowOutcome::NoSession;
  }
  if (!m_acceptConnections)
  {
    // no agent could ever see it
    return ShowOutcome::LinkDisabled;
  }
  m_shownView = cli::makeShownView(*this, capture);
  return m_attached ? ShowOutcome::Shown : ShowOutcome::ShownNoAgent;
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

cli::NodeHandleTable& LiveLinkHost::handles()
{
  return *m_handles;
}

cli::AgentHistory& LiveLinkHost::agentHistory()
{
  return m_agentHistory;
}

cli::ChangeLog& LiveLinkHost::changeLog()
{
  return m_rev->changeLog();
}

cli::ViewCapture LiveLinkHost::captureMapperView()
{
  if (!m_viewCapturer)
  {
    throw cli::CliError{cli::ErrorCode::ViewFailed, "this editor has no map view"};
  }
  return m_viewCapturer();
}

cli::ViewCapture LiveLinkHost::renderAgentView(const cli::CameraPose& pose)
{
  if (!m_agentViewRenderer)
  {
    throw cli::CliError{
      cli::ErrorCode::ViewFailed, "this editor cannot render an agent camera's view"};
  }
  return m_agentViewRenderer(pose);
}

cli::ViewImages& LiveLinkHost::viewImages()
{
  return *m_viewImages;
}

const cli::ShownView* LiveLinkHost::shownView() const
{
  return m_shownView ? &*m_shownView : nullptr;
}

std::optional<cli::ShownView> LiveLinkHost::takeShownView()
{
  return std::exchange(m_shownView, std::nullopt);
}

void LiveLinkHost::documentWasLoaded()
{
  // a loaded or reloaded map is a new map, so its revs start a new epoch
  m_acceptConnections = pref(Preferences::AcceptAiConnections);
  if (hasSessionAt(m_document.map().path()))
  {
    // the map is still open under the same path, so the agent can carry on
    stop();
  }
  else
  {
    endSession();
  }
  start();
}

void LiveLinkHost::documentWasSaved()
{
  // the first save of an untitled map, or a save under another path
  if (const auto& path = m_document.map().path(); !hasSessionAt(path))
  {
    endSession(path);
    start();
  }
}

bool LiveLinkHost::hasSessionAt(const std::filesystem::path& path) const
{
  // compared by session key, so that another spelling of the same path is the same map
  return m_rendezvousFile
         && cli::rendezvousPath(m_rendezvousDir, path) == *m_rendezvousFile;
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

  if (const auto dirError = cli::prepareRendezvousDir(m_rendezvousDir))
  {
    m_document.logger().warn() << "Live link: " << *dirError;
    return;
  }

  m_rev = std::make_unique<cli::LiveRev>(m_document.map());
  // handles of an earlier session give stale_session, since the epoch is new
  m_handles = std::make_unique<cli::NodeHandleTable>(m_document.map(), m_rev->epoch());
  m_agentHistory = cli::AgentHistory{};

  const auto rendezvousFile = cli::rendezvousPath(m_rendezvousDir, path);
  // per session, so that an image of an earlier session is never overwritten
  m_viewImages = std::make_unique<cli::ViewImages>(
    m_rendezvousDir
    / fmt::format("{}-{}-views", rendezvousFile.stem().string(), m_rev->epoch()));

  auto rendezvous = cli::Rendezvous{
    .map = path.string(),
    .pid = int64_t(QCoreApplication::applicationPid()),
    .enabled = m_acceptConnections,
  };
  if (m_acceptConnections)
  {
    m_token = randomHex(4);
    m_server = new QLocalServer{this};
    // restricts the pipe (Windows) or the socket file (macOS, Linux) to the current user
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

    rendezvous.endpoint = m_server->fullServerName().toStdString();
    rendezvous.token = m_token;
  }

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
  m_sessionMap = path;

  // this session replaces the one that ended
  auto error = std::error_code{};
  std::filesystem::remove(cli::endedSessionPath(m_rendezvousDir, path), error);

  emit statusChanged();
}

void LiveLinkHost::endSession(std::optional<std::filesystem::path> movedTo)
{
  if (m_sessionMap && m_attached)
  {
    // written before the session goes away, so that tbcli always finds one of them
    const auto endedSession = cli::EndedSession{
      m_sessionMap->string(), movedTo ? std::optional{movedTo->string()} : std::nullopt};
    const auto endedFile = cli::endedSessionPath(m_rendezvousDir, *m_sessionMap);
    if (
      const auto writeError =
        cli::writeFileAtomically(endedFile, cli::encodeEndedSession(endedSession)))
    {
      m_document.logger().warn()
        << "Live link: cannot write " << endedFile.string() << ": " << *writeError;
    }

    // the requests that were still waiting will not run
    const auto refusal = cli::errorResult(cli::sessionEndedError(endedSession));
    for (const auto& request : m_queue)
    {
      if (request->socket)
      {
        reply(request->socket, refusal);
        // the event loop may not run again before the socket is gone
        request->socket->waitForDisconnected(1000);
      }
    }
  }
  stop();
}

void LiveLinkHost::stop()
{
  m_sessionMap.reset();
  m_attached = false;
  // a new session has no agent yet
  m_lastRequest.reset();
  m_idleTimer->stop();
  m_lookedTimer->stop();
  // its paths and handles are of this session
  m_shownView.reset();
  // deletes the session's view images
  m_viewImages.reset();

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

  emit statusChanged();
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
          // the session has an agent now, which must learn when it ends
          m_attached = true;
          // parsed once, since a batch can be large
          auto label = cli::requestLabel(invocation.args, invocation.in);
          m_lastRequest =
            LiveLinkStatus::Request{label, std::chrono::system_clock::now()};
          m_idleTimer->start(m_idleTimeout);
          emit statusChanged();

          if (
            !invocation.args.empty() && invocation.args.front() == cli::LiveRevCommand)
          {
            // reading the rev changes nothing, so it doesn't wait for a busy mapper,
            // which would hold back a finished compile's result for up to a minute
            reply(socket, serve(invocation, label));
            return;
          }
          m_queue.push_back(
            std::make_unique<Request>(
              socket,
              std::move(invocation),
              std::move(label),
              Clock::now() + m_busyTimeout));
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
      const auto refusal = cli::errorResult(
        cli::CliError{
          cli::ErrorCode::Busy,
          fmt::format(
            "the mapper has been busy (in a dialog, a drag or an action) for {:g} s; "
            "retry later",
            std::chrono::duration<double>(m_busyTimeout).count())});
      if (const auto command = cli::mutatingCommand(request.invocation.args))
      {
        report(*command, request.label, refusal, changeLog().counter());
      }
      reply(request.socket, refusal);
      m_queue.pop_front();
      continue;
    }

    const auto served = std::move(m_queue.front());
    m_queue.pop_front();
    reply(served->socket, serve(served->invocation, served->label));
  }
}

bool LiveLinkHost::isMapperBusy() const
{
  return QApplication::activeModalWidget() != nullptr
         || QApplication::mouseButtons() != Qt::NoButton
         || m_document.map().commandProcessor().isTransactionActive();
}

cli::RunResult LiveLinkHost::serve(
  const cli::Invocation& invocation, const std::string& label)
{
  const auto since = changeLog().counter();
  const auto command = cli::mutatingCommand(invocation.args);

  auto result = [&]() {
    // a batch refreshes the editor's views, inspectors and browsers once, not per op
    const auto deferredRefresh = MapDocument::DeferredRefresh{m_document};

    // each op of a batch ends a transaction, after which the status can show that the
    // agent is working, although the event loop won't run until the batch is done
    auto progress = NotifierConnection{};
    if (command)
    {
      const auto started = Clock::now();
      progress += m_document.map().commandProcessor().transactionDoneNotifier.connect(
        [&, started](const std::string&, bool, bool) {
          if (!m_working && Clock::now() - started >= m_workingDelay)
          {
            m_working = label;
            emit statusChanged();
          }
        });
    }
    return cli::run(invocation.args, invocation.in, *this);
  }();

  if (m_working)
  {
    m_working.reset();
    emit statusChanged();
  }
  if (command)
  {
    report(*command, label, result, since);
  }
  else if (result.exitCode == 0 && label == "view")
  {
    // a command other than apply is labelled by its name
    // the agent looked at the map, through the mapper's eyes or its own camera
    m_lookedTimer->start(m_lookedDuration);
    emit statusChanged();
  }
  return result;
}

void LiveLinkHost::report(
  const std::string& command,
  std::string label,
  const cli::RunResult& result,
  const uint64_t since)
{
  const auto changes = cli::summarizeChanges(*this, since);

  // every request that changes the map or its file gets a line in the Console
  auto outcome = std::string{};
  if (result.exitCode != 0)
  {
    outcome = "failed: " + failure(result);
  }
  else if (const auto json = output(result); command == "save")
  {
    outcome = "saved " + stringMember(json, "path").value_or("the map");
  }
  else
  {
    if (const auto undone = stringMember(json, "undone"); command == "undo" && undone)
    {
      // the undo step's name without its "AI: " prefix
      label += " " + (undone->starts_with("AI: ") ? undone->substr(4) : *undone);
    }
    outcome = countChanges(changes);
  }
  m_document.logger().info() << fmt::format(
    "AI: {} {} {} ({})", label, Dash, outcome, rev());

  // so that the mapper sees what the agent changed
  auto changedNodes = std::vector<mdl::Node*>{};
  for (const auto& change : changes)
  {
    if (change.node)
    {
      changedNodes.push_back(change.node);
    }
  }
  if (result.exitCode == 0 && !changedNodes.empty())
  {
    emit agentChangedNodes(changedNodes);
  }
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
