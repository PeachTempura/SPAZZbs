/******************************************************************************
    SPAZZbs - tabbed bottom dock area
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

/* The full-width dock at the bottom of the main window that holds the tabs. */
class SpazzDockTabs : public QDockWidget {
	Q_OBJECT

public:
	explicit SpazzDockTabs(QMainWindow *mainWindow);

	struct DefaultDocks {
		QDockWidget *scenes;
		QDockWidget *sources;
		QDockWidget *mixer;
		QDockWidget *transitions;
		QDockWidget *controls;
	};

	/* Puts the Main tab back to the stock layout and removes other tabs. */
	void ResetToDefault(const DefaultDocks &docks);

	/* Restores tabs saved by Save(). Returns false if nothing was saved. */
	bool Load(const DefaultDocks &docks);
	void Save() const;

	/* Call for every dock the app creates so it can be dragged into tabs
	 * and so docks restored late (plugins) land in their saved tab. */
	void TrackDock(QDockWidget *dock);

	SpazzDockTabPage *AddTab(const QString &name);
	SpazzDockTabPage *PageForDock(QDockWidget *dock) const;
	void MoveDockToPage(QDockWidget *dock, SpazzDockTabPage *page);
	void MoveDockToMainWindow(QDockWidget *dock);

	/* Fills a menu with "move dock to tab" actions, used by the Docks menu. */
	void PopulateDocksMenu(QMenu *menu);

protected:
	bool eventFilter(QObject *obj, QEvent *event) override;

private:
	QPointer<QMainWindow> mainWindow;
	QTabWidget *tabs;
	QToolButton *addTabButton;

	QHash<QString, QPointer<SpazzDockTabPage>> pendingDocks;

	QPointer<QDockWidget> draggingDock;
	QTimer dragPoll;
	int hoverTab = -1;
	int hoverTicks = 0;

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
};
