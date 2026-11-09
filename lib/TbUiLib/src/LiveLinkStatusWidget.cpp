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

#include "ui/LiveLinkStatusWidget.h"

#include <QDateTime>
#include <QLocale>

#include "base/Macros.h"
#include "ui/LiveLinkHost.h"

#include <chrono>

namespace tb::ui
{
namespace
{

QString statusText(const LiveLinkStatus& status)
{
  using State = LiveLinkStatus::State;
  switch (status.state)
  {
  case State::Off:
  case State::Disabled:
    return QObject::tr("Live link off");
  case State::Ready:
    return QObject::tr("Live link ready");
  case State::Active:
    return QObject::tr("AI active");
  case State::Working:
    // an ellipsis as a code point, since MSVC reads the source in the system code page
    return QObject::tr("AI working: %1%2")
      .arg(QString::fromStdString(status.workingOn))
      .arg(QChar{0x2026});
  case State::Looked:
    return QObject::tr("AI looked");
    switchDefault();
  }
}

QString statusToolTip(const LiveLinkStatus& status)
{
  using State = LiveLinkStatus::State;
  switch (status.state)
  {
  case State::Off:
    return QObject::tr(
      "AI agents can work on this map in the editor with tbcli once it is saved.");
  case State::Disabled:
    return QObject::tr(
      "AI agents can't work on this map in the editor: \"Accept AI connections\" "
      "(Preferences > View) was off when the map was opened.");
  case State::Ready:
  case State::Active:
  case State::Working:
  case State::Looked:
    if (const auto& request = status.lastRequest)
    {
      const auto arrivedAt = QDateTime::fromMSecsSinceEpoch(
        std::chrono::duration_cast<std::chrono::milliseconds>(
          request->arrivedAt.time_since_epoch())
          .count());
      return QObject::tr("Last AI request: %1, at %2")
        .arg(QString::fromStdString(request->label))
        .arg(QLocale{}.toString(arrivedAt.time(), QLocale::LongFormat));
    }
    return QObject::tr(
      "AI agents can work on this map in the editor with tbcli. None has yet.");
    switchDefault();
  }
}

} // namespace

LiveLinkStatusWidget::LiveLinkStatusWidget(LiveLinkHost& host, QWidget* parent)
  : QLabel{parent}
  , m_host{host}
{
  connect(
    &m_host, &LiveLinkHost::statusChanged, this, &LiveLinkStatusWidget::updateStatus);
  updateStatus();
}

void LiveLinkStatusWidget::updateStatus()
{
  const auto status = m_host.status();
  setText(statusText(status));
  setToolTip(statusToolTip(status));

  // dim while no agent is around
  using State = LiveLinkStatus::State;
  const auto dim = status.state == State::Off || status.state == State::Disabled
                   || status.state == State::Ready;
  setForegroundRole(dim ? QPalette::PlaceholderText : QPalette::WindowText);

  if (status.state == State::Working)
  {
    // a batch is running on the main thread, so the event loop can't paint this later
    repaint();
  }
}

} // namespace tb::ui
