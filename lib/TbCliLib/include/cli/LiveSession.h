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
#include <string>

namespace tb::mdl
{
class Map;
} // namespace tb::mdl

namespace tb::cli
{

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
