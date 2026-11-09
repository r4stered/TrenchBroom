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

#include "base/NotifierConnection.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tb::mdl
{
class Map;
} // namespace tb::mdl

namespace tb::cli
{

class AgentHistory;
class NodeHandleTable;

/**
 * A map that a running editor has open, as tbcli's commands see it when they run against
 * it instead of loading the map file: the editor host, or a fake one in tests.
 *
 * Commands call this on the thread that owns the map.
 */
class LiveSession
{
public:
  virtual ~LiveSession();

  /** The open map. Its path is the path that names the session. */
  virtual mdl::Map& map() = 0;

  /**
   * The session's epoch: random, and new whenever the map is loaded, reloaded or
   * reopened.
   */
  virtual std::string epoch() const = 0;

  /**
   * The current live rev, `live-<epoch>:<counter>`. It only goes up, and changes on
   * every change to the map (do, undo and redo), never on selection changes.
   */
  virtual std::string rev() const = 0;

  /** The node handles of this session, which share its epoch. */
  virtual NodeHandleTable& handles() = 0;

  /** Which undo steps of this session are the agent's own batches. */
  virtual AgentHistory& agentHistory() = 0;
};

/**
 * Which undo steps of a live session are the agent's own batches, so that the agent can
 * undo its own most recent batch and nothing else.
 *
 * The agent's batches count only while nothing else has changed the map since the
 * agent's last request: once the rev has moved on without the agent, because the mapper
 * edited, undid or redid something, none of them is the most recent change any more.
 * The undo steps are told apart by their serial numbers, which also catches steps that
 * don't change the rev, such as the mapper's selection changes.
 */
class AgentHistory
{
private:
  /** The agent's batches on top of the undo stack, most recent last. */
  std::vector<uint64_t> m_batches;
  /** The rev at which m_batches are on top of the undo stack. */
  std::string m_rev;

public:
  /**
   * Records a request of the agent that ran from `revBefore` to `revAfter` and stored
   * the undo step with the given serial number, or none, such as a dry run or a failed
   * batch, which leave the undo stack as it was.
   */
  void requestDone(
    const std::string& revBefore,
    const std::string& revAfter,
    std::optional<uint64_t> storedBatch);

  /**
   * The serial number of the agent's batch that is the most recent change to the map at
   * the given rev, if there is one: the agent's most recent batch, if nothing else
   * changed the map since the agent's last request.
   */
  std::optional<uint64_t> mostRecentBatch(const std::string& rev) const;

  /**
   * Records that the agent undid its most recent batch, which took the map to the given
   * rev.
   */
  void batchUndone(const std::string& revAfter);
};

/**
 * Counts the changes to a map for a live session's rev, `live-<epoch>:<counter>`.
 *
 * The counter goes up whenever a change to the map is done, undone or redone, and never
 * goes down, so that a rev never comes back to an old value. Changes that only touch
 * the selection don't count. The epoch is random, so revs of different sessions differ.
 * A transaction counts once, when it is committed or undone.
 */
class LiveRev
{
private:
  std::string m_epoch;
  uint64_t m_counter = 0;
  NotifierConnection m_notifierConnection;

public:
  explicit LiveRev(mdl::Map& map);

  // observes the map through `this`
  LiveRev(const LiveRev&) = delete;
  LiveRev& operator=(const LiveRev&) = delete;

  const std::string& epoch() const;

  /** The current rev. */
  std::string str() const;
};

} // namespace tb::cli
