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

#include <QLabel>

namespace tb::ui
{
class LiveLinkHost;

/**
 * Shows the status of a map's live link in the status bar: whether agents can connect,
 * and whether and what an agent is doing, with the agent's last request in the tooltip.
 */
class LiveLinkStatusWidget : public QLabel
{
  Q_OBJECT
private:
  LiveLinkHost& m_host;

public:
  explicit LiveLinkStatusWidget(LiveLinkHost& host, QWidget* parent = nullptr);

private:
  void updateStatus();
};

} // namespace tb::ui
