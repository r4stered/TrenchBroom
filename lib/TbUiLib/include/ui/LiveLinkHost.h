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

#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

class QLocalServer;
class QLocalSocket;
class QTimer;

namespace tb::cli
{
struct Invocation;
struct RunResult;
} // namespace tb::cli

namespace tb::ui
{
class MapDocument;

/**
 * The editor host of a live session: lets tbcli run commands against the map that a
 * MapDocument has open.
 *
 * While the document has a saved map open, the host listens on a local socket that only
 * the current user can reach, under a random name, and publishes a rendezvous file
 * with that name, a random token and the editor's pid in the rendezvous dir, named
 * after the map's path. Each connection carries one request. Requests are served one at
 * a time in the order they arrive, on the main thread. A request waits while a modal
 * dialog is open, a mouse button is held or a transaction is open, and fails with
 * `busy` if that lasts longer than the busy timeout.
 */
class LiveLinkHost : public QObject, public cli::LiveSession
{
  Q_OBJECT
public:
  static constexpr auto DefaultBusyTimeout = std::chrono::milliseconds{60'000};
  static constexpr auto BusyRetryInterval = std::chrono::milliseconds{100};

private:
  struct Request;

  MapDocument& m_document;
  std::filesystem::path m_rendezvousDir;
  std::chrono::milliseconds m_busyTimeout = DefaultBusyTimeout;

  QLocalServer* m_server = nullptr;
  QTimer* m_retryTimer = nullptr;
  std::string m_token;
  std::unique_ptr<cli::LiveRev> m_rev;
  std::optional<std::filesystem::path> m_rendezvousFile;
  std::deque<std::unique_ptr<Request>> m_queue;

  NotifierConnection m_notifierConnection;

public:
  explicit LiveLinkHost(
    MapDocument& document,
    std::filesystem::path rendezvousDir,
    QObject* parent = nullptr);
  ~LiveLinkHost() override;

  /** How long a request may wait for the mapper before it fails with `busy`. */
  void setBusyTimeout(std::chrono::milliseconds busyTimeout);

  /** The rendezvous file of the current session, if there is one. */
  const std::optional<std::filesystem::path>& rendezvousFile() const;

  /** The number of requests that have arrived and wait to be served. */
  size_t queuedRequests() const;

  mdl::Map& map() override;
  std::string epoch() const override;
  std::string rev() const override;

private:
  void documentWasLoaded();

  void start();
  void stop();

  void newConnection();
  void readRequest(QLocalSocket* socket, std::string& buffer);
  void serveQueue();
  bool isMapperBusy() const;
  cli::RunResult serve(const cli::Invocation& invocation);
  void reply(QLocalSocket* socket, const cli::RunResult& result);
};

} // namespace tb::ui
