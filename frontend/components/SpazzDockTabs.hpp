/******************************************************************************
    SPAZZbs - tabbed dock areas (bottom, and right of the canvas)
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#pragma once

#include <QDockWidget>
#include <QHash>
#include <QJsonArray>
#include <QMainWindow>
#include <QPointer>
#include <QTabWidget>
#include <QTimer>

class QLabel;
class QMenu;
class QToolButton;

/* One tab of the bottom area. It is a small main window of its own, so any
 * dock dropped into it can be split, stacked and resized like normal. */
class SpazzDockTabPage : public QMainWindow {
	Q_OBJECT

public:
	explicit SpazzDockTabPage(QWidget *parent = nullptr);

	QList<QDockWidget *> Docks() const;
	void UpdateEmptyHint();

protected:
	void childEvent(QChildEvent *event) override;

private:
	QLabel *hint = nullptr;
	bool hintQueued = false;
};

/* A tab area: a dock holding a tab strip of pages. The bottom one is the
 * full-width area under the canvas; it makes and owns a second one on the
 * right side of the canvas. Whole tabs can be dragged between the two. Every
 * public call made on the bottom area covers both. */
class SpazzDockTabs : public QDockWidget {
	Q_OBJECT

public:
	enum class Area { Bottom, Right };

	explicit SpazzDockTabs(QMainWindow *mainWindow);

	struct DefaultDocks {
		QDockWidget *scenes;
		QDockWidget *sources;
		QDockWidget *mixer;
		QDockWidget *transitions;
		QDockWidget *controls;
	};

	/* Bottom: the Main tab with the stock layout. Right: an empty Chat tab. */
	void ResetToDefault(const DefaultDocks &docks);

	/* Restores tabs saved by Save(). Returns false if nothing was saved. */
	bool Load(const DefaultDocks &docks);
	void Save() const;

	/* true once Load() or ResetToDefault() has run */
	bool IsSetUp() const;

	/* Call for every dock the app creates so it can be dragged into tabs
	 * and so docks restored late (plugins) land in their saved tab. */
	void TrackDock(QDockWidget *dock);

	SpazzDockTabPage *AddTab(const QString &name); /* in this area */
	SpazzDockTabPage *PageForDock(QDockWidget *dock) const;
	void MoveDockToPage(QDockWidget *dock, SpazzDockTabPage *page);
	void MoveDockToMainWindow(QDockWidget *dock);

	/* Puts a dock in the tab with this name (looked for in `area` first,
	 * then the other side), creating it in `area` if needed, without
	 * switching away from the tab you're on. */
	void PlaceInTab(QDockWidget *dock, const QString &tabName, Area area = Area::Bottom);

	/* Moves a whole tab (and the docks in it, as laid out) to an area. */
	void MoveTabToArea(SpazzDockTabPage *page, Area area, int index = -1);

	/* Shows a dock: switches to its tab, or shows it where it lives. */
	void ShowDock(QDockWidget *dock);

	/* Fills a menu with "move dock to tab" actions, used by the Docks menu. */
	void PopulateDocksMenu(QMenu *menu);

	/* the bottom or right tab area */
	SpazzDockTabs *AreaDock(Area area) const;

protected:
	bool eventFilter(QObject *obj, QEvent *event) override;

private:
	SpazzDockTabs(QMainWindow *mainWindow, SpazzDockTabs *owner); /* the right side */
	void BuildUi();

	QPointer<QMainWindow> mainWindow;
	QPointer<SpazzDockTabs> owner; /* the bottom area (itself, for the bottom) */
	QPointer<SpazzDockTabs> right; /* bottom only */
	Area area = Area::Bottom;
	QTabWidget *tabs = nullptr;
	QToolButton *addTabButton = nullptr;
	bool autoHidden = false; /* hidden because it has no tabs */

	/* kept on the bottom area for both */
	QHash<QString, QPointer<SpazzDockTabPage>> pendingDocks;
	QPointer<QDockWidget> draggingDock;
	QTimer dragPoll;
	QPointer<SpazzDockTabs> hoverArea;
	int hoverTab = -1;
	int hoverTicks = 0;
	bool setUp = false;

	/* dragging a whole tab out of this area's tab strip */
	QPointer<SpazzDockTabPage> tabPressPage;
	QPoint tabPressGlobal;
	bool tabDragOut = false;
	bool otherShownForDrag = false;

	SpazzDockTabs *Owner() const;
	SpazzDockTabs *Other() const;
	QList<SpazzDockTabs *> Areas() const;
	SpazzDockTabs *AreaOfPage(SpazzDockTabPage *page) const;
	SpazzDockTabs *AreaUnderCursor(const QPoint &global) const;
	SpazzDockTabPage *mainPage() const;
	SpazzDockTabPage *pageAt(int index) const;
	QList<QDockWidget *> AllDocks() const;

	void ClearTabs();
	void CloseTab(int index);
	void RenameTab(int index);
	void NewTabPrompt(QDockWidget *withDock = nullptr);
	void ShowTabMenu(const QPoint &pos);
	void PollDrag();
	void FinishDrag();
	void EnsureDockVisible(QDockWidget *dock);
	void UpdateAreaVisibility();
	void TabBarEvent(QEvent *event);
	QJsonArray SaveArea() const;
	void LoadArea(const QJsonArray &tabList, const QHash<QString, QDockWidget *> &byName, bool &mainSeen);
};
