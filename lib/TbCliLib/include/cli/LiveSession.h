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

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tb::mdl
{
class Map;
class Node;
} // namespace tb::mdl

namespace tb::cli
{

class AgentHistory;
class ChangeLog;
class NodeHandleTable;
class ViewImages;
struct CameraPose;
struct ShownView;
struct ViewCapture;

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

  /** Which nodes changed since a rev of this session, and who changed them. */
  virtual ChangeLog& changeLog() = 0;

  /**
   * Captures the map view that the mapper last focused, as it is on screen.
   *
   * @throw CliError `view_failed` if there is no view to capture or the capture failed
   */
  virtual ViewCapture captureMapperView() = 0;

  /**
   * Renders the map as a camera with the given pose sees it, into an image of the size
   * of the pose's viewport, without moving or redrawing the mapper's views. It shows the
   * mapper's hidden, isolated and selected nodes as the mapper's views do.
   *
   * @throw CliError `view_failed` if the editor cannot render the view
   */
  virtual ViewCapture renderAgentView(const CameraPose& pose) = 0;

  /**
   * The images of this session's views, in a temporary directory that goes away when
   * the session ends.
   */
  virtual ViewImages& viewImages() = 0;

  /**
   * The view that the mapper last showed the agent, if the agent has not seen it yet.
   * Only the latest one is kept.
   */
  virtual const ShownView* shownView() const = 0;

  /** Takes the view that the mapper last showed the agent, so that it is seen once. */
  virtual std::optional<ShownView> takeShownView() = 0;
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

/** A live rev, `live-<epoch>:<counter>`, split into its parts. */
struct ParsedLiveRev
{
  std::string_view epoch;
  uint64_t counter = 0;
};

/** The parts of a live rev, or nothing if the text is not one. */
std::optional<ParsedLiveRev> parseLiveRev(std::string_view rev);

/** The live rev `live-<epoch>:<counter>`. */
std::string formatLiveRev(std::string_view epoch, uint64_t counter);

/** What a change in the change log did to a node. */
enum class ChangeKind
{
  Created,
  Modified,
  Removed,
};

/** Who changed a node: the mapper in the editor, or the agent over the live link. */
enum class ChangeOrigin
{
  Mapper,
  Agent,
};

/** How a change came about: done anew, or by undoing or redoing an undo step. */
enum class ChangeVia
{
  Do,
  Undo,
  Redo,
};

/** One node's change in the change log. */
struct Change
{
  /** The live rev's counter after the change. */
  uint64_t counter = 0;
  uint64_t serialNumber = 0;
  ChangeKind kind = ChangeKind::Modified;
  ChangeOrigin origin = ChangeOrigin::Mapper;
  ChangeVia via = ChangeVia::Do;
  /** For a removed node, the nodes below it, which left the map with it. */
  std::vector<uint64_t> removedDescendants;
};

std::string_view toString(ChangeKind kind);
std::string_view toString(ChangeOrigin origin);

/**
 * Counts the changes to a map for a live session's rev, and records which nodes each of
 * them created, modified or removed, who made it, and whether it was an undo or a redo.
 *
 * The counter goes up whenever a change to the map is done, undone or redone, and never
 * goes down, so that a rev never comes back to an old value. Changes that only touch
 * the selection don't count, and neither does hiding or showing nodes, which changes no
 * node in the log. A transaction counts once, when it is committed or undone, and a
 * long-running one once per nested transaction that it commits.
 *
 * The log keeps the most recent changes only, about `capacity` nodes' worth, and of
 * the mapper's modifications of a node one after another only the latest. Changes
 * are the mapper's unless they happen while an AgentChanges lives; while it lives, the
 * log keeps every change, so that the agent's changes can be taken out again without
 * losing older ones.
 */
class ChangeLog
{
public:
  static constexpr size_t DefaultCapacity = 1000;

private:
  mdl::Map& m_map;
  size_t m_capacity;
  uint64_t m_counter = 0;
  /** The log has every change after this counter. */
  uint64_t m_floor = 0;
  std::deque<Change> m_changes;
  /** The number of changes ever recorded, of which m_changes are the latest. */
  uint64_t m_recorded = 0;
  /** The nodes changed by commands whose transaction has not counted yet. */
  std::vector<Change> m_pending;
  std::set<std::pair<uint64_t, ChangeKind>> m_pendingKeys;
  ChangeOrigin m_origin = ChangeOrigin::Mapper;
  NotifierConnection m_notifierConnection;

public:
  explicit ChangeLog(mdl::Map& map, size_t capacity = DefaultCapacity);

  // observes the map through `this`
  ChangeLog(const ChangeLog&) = delete;
  ChangeLog& operator=(const ChangeLog&) = delete;

  /** The number of changes to the map so far, the live rev's counter. */
  uint64_t counter() const;

  /**
   * Whether the log has every change after the given counter: false once changes after
   * it have been dropped to make room.
   */
  bool covers(uint64_t since) const;

  /** The changes after the given counter that the log still has, oldest first. */
  std::vector<Change> changesSince(uint64_t since) const;

  /**
   * The serial numbers of the nodes that changed after the given counter, including the
   * nodes that left the map below a removed node.
   */
  std::unordered_set<uint64_t> changedSince(uint64_t since) const;

  /**
   * Marks the changes made while it lives as the agent's, and can take them out of the
   * log again for a request that left the map as it was.
   */
  class AgentChanges
  {
  private:
    ChangeLog& m_log;
    uint64_t m_recordedBefore;

  public:
    explicit AgentChanges(ChangeLog& log);
    ~AgentChanges();

    AgentChanges(const AgentChanges&) = delete;
    AgentChanges& operator=(const AgentChanges&) = delete;

    /**
     * Takes the changes made so far out of the log, for a request that undid them again
     * and so left the map as it was: a dry run, or a batch that failed.
     */
    void discard();
  };

private:
  void nodesChanged(const std::vector<mdl::Node*>& nodes, ChangeKind kind);
  void transactionEnded(bool observable, bool isModification, ChangeVia via);
  void clearPending();
  /** Drops the oldest changes beyond the capacity, unless the agent is changing the map.
   */
  void trim();
  /** Takes out the changes recorded after the first `recorded` ones. */
  void discardAfter(uint64_t recorded);
};

/** How changes touched a node. */
enum class Touch
{
  /** the node itself changed */
  Changed,
  /** an ancestor of the node below the world changed, such as a group or a layer */
  AncestorChanged,
  /** a node below the group or entity changed */
  DescendantChanged,
};

std::string_view toString(Touch touch);

/**
 * Whether and how the given changes (see ChangeLog::changedSince) touched the given
 * node: it, one of its ancestors below the world, or, for a group or an entity, one of
 * its descendants changed. Changes to its siblings don't touch it.
 */
std::optional<Touch> howTouched(
  const mdl::Node& node, const std::unordered_set<uint64_t>& changed);

/**
 * A live session's rev, `live-<epoch>:<counter>`, with the change log whose counter it
 * shows. The epoch is random, so revs of different sessions differ.
 */
class LiveRev
{
private:
  std::string m_epoch;
  ChangeLog m_changeLog;

public:
  explicit LiveRev(mdl::Map& map, size_t changeLogCapacity = ChangeLog::DefaultCapacity);

  const std::string& epoch() const;

  /** The current rev. */
  std::string str() const;

  ChangeLog& changeLog();
};

} // namespace tb::cli
