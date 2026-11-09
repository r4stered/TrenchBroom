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

#include <QObject>
#include <QPointer>

#include "base/NotifierConnection.h"
#include "cli/LiveSession.h"
#include "cli/View.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QLocalServer;
class QLocalSocket;
class QTimer;

namespace tb::cli
{
struct Invocation;
struct RunResult;
} // namespace tb::cli

namespace tb::mdl
{
class Node;
} // namespace tb::mdl

namespace tb::ui
{
class MapDocument;

/** What the mapper sees of a live link: whether and what an agent is doing. */
struct LiveLinkStatus
{
  enum class State
  {
    /** The map has no session, such as when it was never saved. */
    Off,
    /** The "Accept AI connections" preference was off when the map was opened. */
    Disabled,
    /** Agents can connect, but none has sent a request lately. */
    Ready,
    /** An agent sent a request lately. */
    Active,
    /** An agent's batch is taking a while. */
    Working,
    /** An agent just looked at the map. */
    Looked,
  };

  /** A request that an agent sent. */
  struct Request
  {
    /** What the request does, see cli::requestLabel. */
    std::string label;
    std::chrono::system_clock::time_point arrivedAt;

    bool operator==(const Request&) const = default;
  };

  State state = State::Off;
  /** While working, the label of the request that the agent is working on. */
  std::string workingOn;
  /** The agent's last request in this session, if it sent one. */
  std::optional<Request> lastRequest;
};

/**
 * The editor host of a live session: lets tbcli run commands against the map that a
 * MapDocument has open.
 *
 * While the document has a saved map open, the host listens on a local socket that only
 * the current user can reach, under a random name, and publishes a rendezvous file
 * with that name, a random token and the editor's pid in the rendezvous dir, named
 * after the map's path. A map that was never saved has no session; saving it, or saving
 * it under another path, starts one under its new path. If the "Accept AI connections"
 * preference is off when the map opens, the host publishes a rendezvous file that says
 * so instead of listening.
 *
 * When the host ends a session that an agent sent a request to, because the map was
 * closed or saved under another path, it leaves an ended session file behind so that
 * the agent's next command fails with `session_ended` instead of editing the file.
 *
 * Each connection carries one request. Requests are served one at a time in the order
 * they arrive, on the main thread. A request waits while a modal dialog is open, a mouse
 * button is held or a transaction is open, and fails with `busy` if that lasts longer
 * than the busy timeout.
 *
 * `tbcli view` captures the map view that the mapper last focused, or with an agent
 * camera renders that camera's view offscreen, both of which the map window provides.
 * Its images go into a directory next to the rendezvous file that is deleted when the
 * session ends.
 *
 * The mapper can show the agent a view, which waits for the agent's next `tbcli view`,
 * even if no agent has connected yet. Only the latest one is kept, and it is dropped
 * when the session ends.
 *
 * The host tells the mapper what the agent does: its status says whether an agent sent
 * a request lately, is working on a long batch or just looked at the map; every
 * request that changes the map or its file gets a Console line; and after each one, the
 * host names the nodes that it created or modified, so that the map views can flash
 * them.
 */
class LiveLinkHost : public QObject, public cli::LiveSession
{
  Q_OBJECT
public:
  static constexpr auto DefaultBusyTimeout = std::chrono::milliseconds{60'000};
  static constexpr auto BusyRetryInterval = std::chrono::milliseconds{100};
  static constexpr auto DefaultIdleTimeout = std::chrono::milliseconds{120'000};
  static constexpr auto DefaultLookedDuration = std::chrono::milliseconds{2'000};
  static constexpr auto DefaultWorkingDelay = std::chrono::milliseconds{500};

  /** Captures the map view that the mapper last focused. */
  using ViewCapturer = std::function<cli::ViewCapture()>;

  /** Renders the map as an agent camera with the given pose sees it. */
  using AgentViewRenderer = std::function<cli::ViewCapture(const cli::CameraPose&)>;

  /** What became of a view that the mapper showed the agent. */
  enum class ShowOutcome
  {
    /** Kept for the agent, which has sent requests in this session. */
    Shown,
    /** Kept for the agent, but no agent has sent a request in this session yet. */
    ShownNoAgent,
    /** Not kept, since the map has no session, such as when it was never saved. */
    NoSession,
    /** Not kept, since the "Accept AI connections" preference was off. */
    LinkDisabled,
  };

private:
  struct Request;

  MapDocument& m_document;
  std::filesystem::path m_rendezvousDir;
  std::chrono::milliseconds m_busyTimeout = DefaultBusyTimeout;
  bool m_acceptConnections = true;

  QLocalServer* m_server = nullptr;
  QTimer* m_retryTimer = nullptr;
  std::string m_token;
  std::unique_ptr<cli::LiveRev> m_rev;
  std::unique_ptr<cli::NodeHandleTable> m_handles;
  cli::AgentHistory m_agentHistory;
  std::unique_ptr<cli::ViewImages> m_viewImages;
  ViewCapturer m_viewCapturer;
  AgentViewRenderer m_agentViewRenderer;
  std::optional<cli::ShownView> m_shownView;
  std::optional<std::filesystem::path> m_sessionMap;
  std::optional<std::filesystem::path> m_rendezvousFile;
  bool m_attached = false;
  std::deque<std::unique_ptr<Request>> m_queue;

  std::chrono::milliseconds m_idleTimeout = DefaultIdleTimeout;
  std::chrono::milliseconds m_lookedDuration = DefaultLookedDuration;
  std::chrono::milliseconds m_workingDelay = DefaultWorkingDelay;
  QTimer* m_idleTimer = nullptr;
  QTimer* m_lookedTimer = nullptr;
  std::optional<std::string> m_working;
  std::optional<LiveLinkStatus::Request> m_lastRequest;

  NotifierConnection m_notifierConnection;

public:
  explicit LiveLinkHost(
    MapDocument& document,
    std::filesystem::path rendezvousDir,
    QObject* parent = nullptr);
  ~LiveLinkHost() override;

  /** How long a request may wait for the mapper before it fails with `busy`. */
  void setBusyTimeout(std::chrono::milliseconds busyTimeout);

  /** How long after the agent's last request the status stops showing it as active. */
  void setIdleTimeout(std::chrono::milliseconds idleTimeout);

  /** How long the status shows that the agent looked at the map. */
  void setLookedDuration(std::chrono::milliseconds lookedDuration);

  /** How long a batch must run before the status shows the agent as working. */
  void setWorkingDelay(std::chrono::milliseconds workingDelay);

  /** What the mapper sees of the link now. */
  LiveLinkStatus status() const;

  /** Sets how to capture the mapper's view; without one, `view` fails. */
  void setViewCapturer(ViewCapturer viewCapturer);

  /** Sets how to render an agent camera's view; without one, such a `view` fails. */
  void setAgentViewRenderer(AgentViewRenderer agentViewRenderer);

  /**
   * Shows the agent the given capture of a view, replacing a view shown before, so that
   * the agent's next `tbcli view` returns it.
   */
  ShowOutcome show(const cli::ViewCapture& capture);

  /** The rendezvous file of the current session, if there is one. */
  const std::optional<std::filesystem::path>& rendezvousFile() const;

  /** The number of requests that have arrived and wait to be served. */
  size_t queuedRequests() const;

  mdl::Map& map() override;
  std::string epoch() const override;
  std::string rev() const override;
  cli::NodeHandleTable& handles() override;
  cli::AgentHistory& agentHistory() override;
  cli::ChangeLog& changeLog() override;
  cli::ViewCapture captureMapperView() override;
  cli::ViewCapture renderAgentView(const cli::CameraPose& pose) override;
  cli::ViewImages& viewImages() override;
  const cli::ShownView* shownView() const override;
  std::optional<cli::ShownView> takeShownView() override;

signals:
  /**
   * The status changed. While a batch runs, this is sent from within it, so a widget
   * that shows the status must repaint at once.
   */
  void statusChanged();

  /** A request of the agent created or modified the given nodes, which are in the map. */
  void agentChangedNodes(const std::vector<mdl::Node*>& nodes);

private:
  void documentWasLoaded();
  void documentWasSaved();
  bool hasSessionAt(const std::filesystem::path& path) const;

  void start();
  /**
   * Ends the session and tells an agent that used it, both with requests that were
   * still waiting and with commands to come, that the map was closed or moved.
   */
  void endSession(std::optional<std::filesystem::path> movedTo = std::nullopt);
  void stop();

  void newConnection();
  void readRequest(QLocalSocket* socket, std::string& buffer);
  void serveQueue();
  bool isMapperBusy() const;
  cli::RunResult serve(const cli::Invocation& invocation, const std::string& label);
  /**
   * Tells the mapper what a request did that changes the map or its file with the given
   * command (see cli::mutatingCommand).
   */
  void report(
    const std::string& command,
    std::string label,
    const cli::RunResult& result,
    uint64_t since);
  void reply(QLocalSocket* socket, const cli::RunResult& result);
};

} // namespace tb::ui
