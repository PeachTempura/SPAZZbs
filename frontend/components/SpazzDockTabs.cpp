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

#include "SpazzDockTabs.hpp"

#include <OBSApp.hpp>
#include <dialogs/NameDialog.hpp>

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QGuiApplication>
#include <QChildEvent>
#include <QJsonArray>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QTabBar>
#include <QToolButton>
#include <QToolTip>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "moc_SpazzDockTabs.cpp"

static constexpr const char *kConfigSection = "SPAZZbs";
static constexpr const char *kConfigKey = "DockTabs";
static constexpr int kPollMs = 50;
static constexpr int kHoverSwitchTicks = 10; /* ~0.5s hovering a tab switches to it */
static constexpr int kRightDefaultWidth = 340;

static bool LeftButtonDown()
{
#ifdef _WIN32
	/* During a native window drag Qt doesn't see mouse events, so ask Windows. */
	int vk = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
	return (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
	return (QGuiApplication::mouseButtons() & Qt::LeftButton) != 0;
#endif
}

/* ------------------------------------------------------------------------ */

SpazzDockTabPage::SpazzDockTabPage(QWidget *parent) : QMainWindow(parent)
{
	setWindowFlags(Qt::Widget);
	setObjectName("spazzDockTabPage");
	setDockNestingEnabled(true);
	setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);

	/* Qt needs a central widget. It shows a hint while the tab is empty and
	 * collapses to zero height once docks are in, so they get the space. */
	hint = new QLabel(QTStr("SPAZZ.DockTabs.EmptyHint"), this);
	hint->setObjectName("spazzDockTabHint");
	hint->setAlignment(Qt::AlignCenter);
	hint->setWordWrap(true);
	setCentralWidget(hint);
	UpdateEmptyHint();
}

void SpazzDockTabPage::UpdateEmptyHint()
{
	hintQueued = false;

	bool empty = true;
	for (QDockWidget *dock : Docks()) {
		if (!dock->isFloating() && !dock->isHidden()) {
			empty = false;
			break;
		}
	}

	hint->setVisible(empty);
	hint->setMaximumHeight(empty ? QWIDGETSIZE_MAX : 0);
	hint->setSizePolicy(QSizePolicy::Ignored, empty ? QSizePolicy::Expanding : QSizePolicy::Fixed);
}

void SpazzDockTabPage::childEvent(QChildEvent *event)
{
	QMainWindow::childEvent(event);

	if (!hintQueued && (event->added() || event->removed())) {
		hintQueued = true;
		QTimer::singleShot(0, this, &SpazzDockTabPage::UpdateEmptyHint);
	}
}

QList<QDockWidget *> SpazzDockTabPage::Docks() const
{
	QList<QDockWidget *> result;
	for (QDockWidget *dock : findChildren<QDockWidget *>(Qt::FindDirectChildrenOnly)) {
		if (!dock->objectName().isEmpty()) {
			result.push_back(dock);
		}
	}
	return result;
}

/* ------------------------------------------------------------------------ */

SpazzDockTabs::SpazzDockTabs(QMainWindow *mainWindow_) : QDockWidget(mainWindow_), mainWindow(mainWindow_)
{
	owner = this;
	area = Area::Bottom;
	setObjectName("spazzTabsDock");
	setWindowTitle(QTStr("SPAZZ.DockTabs"));
	setAllowedAreas(Qt::BottomDockWidgetArea);
	BuildUi();

	AddTab(QTStr("SPAZZ.DockTabs.Main"));

	dragPoll.setInterval(kPollMs);
	connect(&dragPoll, &QTimer::timeout, this, &SpazzDockTabs::PollDrag);

	/* the side area, right of the canvas */
	right = new SpazzDockTabs(mainWindow_, this);
	if (mainWindow_) {
		mainWindow_->addDockWidget(Qt::RightDockWidgetArea, right);
	}
}

SpazzDockTabs::SpazzDockTabs(QMainWindow *mainWindow_, SpazzDockTabs *owner_)
	: QDockWidget(mainWindow_),
	  mainWindow(mainWindow_),
	  owner(owner_)
{
	area = Area::Right;
	setObjectName("spazzRightTabsDock");
	setWindowTitle(QTStr("SPAZZ.DockTabs.Right"));
	setAllowedAreas(Qt::RightDockWidgetArea);
	BuildUi();

	AddTab(QTStr("SPAZZ.DockTabs.Chat"));
}

void SpazzDockTabs::BuildUi()
{
	setFeatures(QDockWidget::NoDockWidgetFeatures);
	setTitleBarWidget(new QWidget(this));

	tabs = new QTabWidget(this);
	tabs->setObjectName("spazzTabs");
	tabs->setDocumentMode(true);
	tabs->setMovable(true);
	tabs->setTabsClosable(false);
	tabs->setTabPosition(QTabWidget::North);

	addTabButton = new QToolButton(tabs);
	addTabButton->setObjectName("spazzAddTab");
	addTabButton->setText("+");
	addTabButton->setToolTip(QTStr("SPAZZ.DockTabs.NewTab.Tooltip"));
	addTabButton->setAutoRaise(true);
	tabs->setCornerWidget(addTabButton, Qt::TopRightCorner);
	connect(addTabButton, &QToolButton::clicked, this, [this]() { NewTabPrompt(); });

	QTabBar *bar = tabs->tabBar();
	bar->setContextMenuPolicy(Qt::CustomContextMenu);
	bar->installEventFilter(this);
	connect(bar, &QTabBar::customContextMenuRequested, this, &SpazzDockTabs::ShowTabMenu);
	connect(bar, &QTabBar::tabBarDoubleClicked, this, [this](int index) {
		if (index >= 0) {
			RenameTab(index);
		}
	});

	setWidget(tabs);
}

/* ---- the two areas ---- */

SpazzDockTabs *SpazzDockTabs::Owner() const
{
	return owner ? owner.data() : const_cast<SpazzDockTabs *>(this);
}

QList<SpazzDockTabs *> SpazzDockTabs::Areas() const
{
	SpazzDockTabs *o = Owner();
	QList<SpazzDockTabs *> list{o};
	if (o->right && o->right != o) {
		list.push_back(o->right);
	}
	return list;
}

SpazzDockTabs *SpazzDockTabs::Other() const
{
	for (SpazzDockTabs *a : Areas()) {
		if (a != this) {
			return a;
		}
	}
	return nullptr;
}

SpazzDockTabs *SpazzDockTabs::AreaDock(Area which) const
{
	for (SpazzDockTabs *a : Areas()) {
		if (a->area == which) {
			return a;
		}
	}
	return nullptr;
}

SpazzDockTabs *SpazzDockTabs::AreaOfPage(SpazzDockTabPage *page) const
{
	if (!page) {
		return nullptr;
	}
	for (SpazzDockTabs *a : Areas()) {
		if (a->tabs->indexOf(page) >= 0) {
			return a;
		}
	}
	return nullptr;
}

SpazzDockTabs *SpazzDockTabs::AreaUnderCursor(const QPoint &global) const
{
	for (SpazzDockTabs *a : Areas()) {
		if (a->isVisible() && a->rect().contains(a->mapFromGlobal(global))) {
			return a;
		}
	}
	return nullptr;
}

void SpazzDockTabs::UpdateAreaVisibility()
{
	/* an area with no tabs gets out of the way, and comes back when a tab
	 * lands in it; one you hid yourself stays hidden */
	if (tabs->count() == 0) {
		if (isVisible() || !autoHidden) {
			autoHidden = true;
			hide();
		}
	} else if (autoHidden) {
		autoHidden = false;
		show();
	}
}

bool SpazzDockTabs::IsSetUp() const
{
	return Owner()->setUp;
}

SpazzDockTabPage *SpazzDockTabs::pageAt(int index) const
{
	return qobject_cast<SpazzDockTabPage *>(tabs->widget(index));
}

SpazzDockTabPage *SpazzDockTabs::mainPage() const
{
	/* The Main tab can be dragged anywhere, even to the other side, so
	 * find it by marker. */
	for (SpazzDockTabs *a : Areas()) {
		for (int i = 0; i < a->tabs->count(); i++) {
			SpazzDockTabPage *page = a->pageAt(i);
			if (page && page->property("spazzMain").toBool()) {
				return page;
			}
		}
	}
	SpazzDockTabs *o = Owner();
	return o->tabs->count() ? o->pageAt(0) : nullptr;
}

SpazzDockTabPage *SpazzDockTabs::AddTab(const QString &name)
{
	bool hasMain = false;
	for (SpazzDockTabs *a : Areas()) {
		for (int i = 0; i < a->tabs->count(); i++) {
			if (a->pageAt(i) && a->pageAt(i)->property("spazzMain").toBool()) {
				hasMain = true;
			}
		}
	}

	SpazzDockTabPage *page = new SpazzDockTabPage(tabs);
	if (!hasMain && area == Area::Bottom) {
		page->setProperty("spazzMain", true);
	}
	tabs->addTab(page, name);
	UpdateAreaVisibility();
	return page;
}

void SpazzDockTabs::MoveTabToArea(SpazzDockTabPage *page, Area which, int index)
{
	SpazzDockTabs *from = AreaOfPage(page);
	SpazzDockTabs *to = AreaDock(which);
	if (!from || !to || from == to) {
		return;
	}

	const int at = from->tabs->indexOf(page);
	const QString name = from->tabs->tabText(at);
	/* the page is a little main window: its docks and their layout go
	 * with it, the saved state just puts the sizes back after the move */
	const QByteArray state = page->saveState();

	from->tabs->removeTab(at);
	const int dest = (index < 0 || index > to->tabs->count()) ? to->tabs->count() : index;
	to->tabs->insertTab(dest, page, name);
	page->restoreState(state);
	to->tabs->setCurrentWidget(page);

	from->UpdateAreaVisibility();
	to->UpdateAreaVisibility();
	to->show();
	page->UpdateEmptyHint();
}

/* ---- docks ---- */

SpazzDockTabPage *SpazzDockTabs::PageForDock(QDockWidget *dock) const
{
	if (!dock) {
		return nullptr;
	}
	return qobject_cast<SpazzDockTabPage *>(dock->parentWidget());
}

QList<QDockWidget *> SpazzDockTabs::AllDocks() const
{
	QList<QDockWidget *> result;
	if (!mainWindow) {
		return result;
	}

	for (QDockWidget *dock : mainWindow->findChildren<QDockWidget *>()) {
		/* the tab areas themselves are never moved into tabs */
		if (qobject_cast<SpazzDockTabs *>(dock) || dock->objectName().isEmpty()) {
			continue;
		}
		result.push_back(dock);
	}
	return result;
}

void SpazzDockTabs::MoveDockToPage(QDockWidget *dock, SpazzDockTabPage *page)
{
	/* docks without a name can't be saved or given back to the main
	 * window when their tab closes, so they stay where they are */
	if (!dock || !page || qobject_cast<SpazzDockTabs *>(dock) || dock->objectName().isEmpty()) {
		return;
	}

	SpazzDockTabPage *oldPage = PageForDock(dock);
	if (oldPage == page && !dock->isFloating()) {
		return;
	}

	if (oldPage) {
		oldPage->removeDockWidget(dock);
	} else if (mainWindow && dock->parentWidget() == mainWindow) {
		mainWindow->removeDockWidget(dock);
	}

	dock->setParent(page);

	/* Put new docks to the right of whatever is already in the tab. */
	QList<QDockWidget *> existing = page->Docks();
	existing.removeAll(dock);
	QDockWidget *last = nullptr;
	for (QDockWidget *d : existing) {
		if (!d->isFloating() && d->isVisible()) {
			last = d;
		}
	}

	page->addDockWidget(Qt::TopDockWidgetArea, dock);
	if (last) {
		page->splitDockWidget(last, dock, Qt::Horizontal);
	}

	dock->setFloating(false);
	dock->show();
	TrackDock(dock);
}

void SpazzDockTabs::MoveDockToMainWindow(QDockWidget *dock)
{
	if (!dock || !mainWindow) {
		return;
	}

	SpazzDockTabPage *oldPage = PageForDock(dock);
	if (oldPage) {
		oldPage->removeDockWidget(dock);
	}

	dock->setParent(mainWindow);
	mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
	dock->setFloating(false);
	dock->show();
}

void SpazzDockTabs::ClearTabs()
{
	SpazzDockTabs *o = Owner();
	SpazzDockTabPage *keep = mainPage();

	for (SpazzDockTabs *a : Areas()) {
		for (int i = a->tabs->count() - 1; i >= 0; i--) {
			SpazzDockTabPage *page = a->pageAt(i);
			if (!page || page == keep) {
				continue;
			}

			/* Send docks back to the main window, floating and hidden,
			 * the same way stock OBS resets extra docks. */
			for (QDockWidget *dock : page->Docks()) {
				page->removeDockWidget(dock);
				dock->setParent(mainWindow);
				mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
				dock->setFloating(true);
				dock->hide();
			}

			a->tabs->removeTab(i);
			page->deleteLater();
		}
	}

	if (keep) {
		/* Main always starts again at the bottom, first */
		if (AreaOfPage(keep) != o) {
			MoveTabToArea(keep, Area::Bottom, 0);
		}
		o->tabs->setTabText(o->tabs->indexOf(keep), QTStr("SPAZZ.DockTabs.Main"));
		o->tabs->tabBar()->moveTab(o->tabs->indexOf(keep), 0);
	}

	for (SpazzDockTabs *a : Areas()) {
		a->UpdateAreaVisibility();
	}
}

void SpazzDockTabs::ResetToDefault(const DefaultDocks &docks)
{
	SpazzDockTabs *o = Owner();
	if (o != this) {
		o->ResetToDefault(docks);
		return;
	}

	ClearTabs();
	pendingDocks.clear();
	SpazzDockTabPage *page = mainPage();

	/* Anything else living in Main goes back to the main window, hidden. */
	for (QDockWidget *dock : page->Docks()) {
		if (dock == docks.scenes || dock == docks.sources || dock == docks.mixer || dock == docks.transitions ||
		    dock == docks.controls) {
			continue;
		}
		page->removeDockWidget(dock);
		dock->setParent(mainWindow);
		mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
		dock->setFloating(true);
		dock->hide();
	}

	QList<QDockWidget *> row{docks.scenes, docks.sources, docks.mixer, docks.transitions, docks.controls};

	for (QDockWidget *dock : row) {
		SpazzDockTabPage *oldPage = PageForDock(dock);
		if (oldPage) {
			oldPage->removeDockWidget(dock);
		} else if (dock->parentWidget() == mainWindow) {
			mainWindow->removeDockWidget(dock);
		}
		dock->setParent(page);
	}

	page->addDockWidget(Qt::TopDockWidgetArea, row[0]);
	for (int i = 1; i < row.size(); i++) {
		page->splitDockWidget(row[i - 1], row[i], Qt::Horizontal);
	}

	for (QDockWidget *dock : row) {
		dock->setFloating(false);
		dock->show();
		TrackDock(dock);
	}

	int cx = mainWindow ? mainWindow->width() : 1280;
	page->resizeDocks(row, {cx * 15 / 100, cx * 20 / 100, cx * 35 / 100, cx * 14 / 100, cx * 16 / 100},
			  Qt::Horizontal);

	if (mainWindow) {
		mainWindow->addDockWidget(Qt::BottomDockWidgetArea, this);
		autoHidden = false;
		show();
		mainWindow->resizeDocks({this}, {mainWindow->height() * 28 / 100}, Qt::Vertical);
	}

	tabs->setCurrentWidget(page);

	/* the right side starts with an empty Chat tab */
	if (right) {
		if (right->tabs->count() == 0) {
			right->AddTab(QTStr("SPAZZ.DockTabs.Chat"));
		}
		if (mainWindow) {
			mainWindow->addDockWidget(Qt::RightDockWidgetArea, right);
		}
		right->autoHidden = false;
		right->show();
		if (mainWindow) {
			mainWindow->resizeDocks({right}, {kRightDefaultWidth}, Qt::Horizontal);
		}
	}

	setUp = true;
}

QJsonArray SpazzDockTabs::SaveArea() const
{
	const SpazzDockTabs *o = Owner();
	QJsonArray tabList;
	for (int i = 0; i < tabs->count(); i++) {
		SpazzDockTabPage *page = pageAt(i);
		if (!page) {
			continue;
		}

		QJsonArray dockNames;
		for (QDockWidget *dock : page->Docks()) {
			dockNames.append(dock->objectName());
		}

		/* Docks that haven't been created yet this session (a plugin
		 * that failed to load, say) keep their spot for next time. */
		for (auto it = o->pendingDocks.cbegin(); it != o->pendingDocks.cend(); ++it) {
			if (it.value() == page) {
				dockNames.append(it.key());
			}
		}

		QJsonObject tab;
		tab["name"] = tabs->tabText(i);
		tab["main"] = page->property("spazzMain").toBool();
		tab["docks"] = dockNames;
		tab["state"] = QString::fromLatin1(page->saveState().toBase64());
		tabList.append(tab);
	}
	return tabList;
}

void SpazzDockTabs::Save() const
{
	const SpazzDockTabs *o = Owner();
	if (o != this) {
		o->Save();
		return;
	}

	QJsonObject root;
	root["version"] = 2;
	root["current"] = tabs->currentIndex();
	root["tabs"] = SaveArea();

	/* where the right side is and how wide lives in the main window's own
	 * dock state; its tabs live here */
	if (right) {
		QJsonObject side;
		side["current"] = right->tabs->currentIndex();
		side["tabs"] = right->SaveArea();
		root["right"] = side;
	}

	QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
	config_set_string(App()->GetUserConfig(), kConfigSection, kConfigKey, json.constData());
}

void SpazzDockTabs::LoadArea(const QJsonArray &tabList, const QHash<QString, QDockWidget *> &byName, bool &mainSeen)
{
	SpazzDockTabs *o = Owner();

	for (const QJsonValue &value : tabList) {
		QJsonObject tab = value.toObject();
		bool isMain = tab["main"].toBool() && !mainSeen;

		SpazzDockTabPage *page;
		if (isMain) {
			page = mainPage();
			if (AreaOfPage(page) != this) {
				MoveTabToArea(page, area);
			}
			tabs->setTabText(tabs->indexOf(page), tab["name"].toString());
			tabs->tabBar()->moveTab(tabs->indexOf(page), tabs->count() - 1);
			mainSeen = true;
		} else {
			page = AddTab(tab["name"].toString());
		}

		for (const QJsonValue &nameValue : tab["docks"].toArray()) {
			QString name = nameValue.toString();
			QDockWidget *dock = byName.value(name);
			if (dock) {
				MoveDockToPage(dock, page);
			} else {
				o->pendingDocks.insert(name, page);
			}
		}

		QByteArray state = QByteArray::fromBase64(tab["state"].toString().toLatin1());
		page->restoreState(state);
	}
}

bool SpazzDockTabs::Load(const DefaultDocks &docks)
{
	SpazzDockTabs *o = Owner();
	if (o != this) {
		return o->Load(docks);
	}

	const char *json = config_get_string(App()->GetUserConfig(), kConfigSection, kConfigKey);
	if (!json || !*json) {
		return false;
	}

	QJsonObject root = QJsonDocument::fromJson(QByteArray(json)).object();
	QJsonArray tabList = root["tabs"].toArray();
	const bool hasRight = root.contains("right") && root["right"].isObject();
	QJsonObject side = root["right"].toObject();
	QJsonArray rightList = side["tabs"].toArray();
	if (tabList.isEmpty() && rightList.isEmpty()) {
		return false;
	}

	/* the main window's own restore already hid the side if you had hidden
	 * it; rebuilding its tabs mustn't bring it back (isHidden, not
	 * isVisible: the window isn't on screen yet at startup) */
	const bool rightUserHidden = right && right->isHidden() && !right->autoHidden;

	ClearTabs();

	QHash<QString, QDockWidget *> byName;
	for (QDockWidget *dock : AllDocks()) {
		byName.insert(dock->objectName(), dock);
	}

	bool mainSeen = false;
	LoadArea(tabList, byName, mainSeen);
	if (right) {
		if (hasRight) {
			right->LoadArea(rightList, byName, mainSeen);
		} else if (right->tabs->count() == 0) {
			/* saved before there was a right side: give it its Chat tab */
			right->AddTab(QTStr("SPAZZ.DockTabs.Chat"));
		}
	}

	/* The stock segments must always live somewhere. If one is missing
	 * from the saved tabs, put it back in Main. */
	for (QDockWidget *dock : {docks.scenes, docks.sources, docks.mixer, docks.transitions, docks.controls}) {
		if (!PageForDock(dock) && dock->parentWidget() == mainWindow && !dock->isFloating() &&
		    mainWindow->dockWidgetArea(dock) == Qt::NoDockWidgetArea) {
			MoveDockToPage(dock, mainPage());
		}
	}

	int current = root["current"].toInt();
	if (current >= 0 && current < tabs->count()) {
		tabs->setCurrentIndex(current);
	}
	if (right) {
		int rightCurrent = side["current"].toInt();
		if (rightCurrent >= 0 && rightCurrent < right->tabs->count()) {
			right->tabs->setCurrentIndex(rightCurrent);
		}
	}

	for (SpazzDockTabs *a : Areas()) {
		a->UpdateAreaVisibility();
	}

	if (right && !hasRight && mainWindow) {
		right->autoHidden = false;
		right->show();
		mainWindow->resizeDocks({right}, {kRightDefaultWidth}, Qt::Horizontal);
	} else if (right && rightUserHidden && right->tabs->count()) {
		right->autoHidden = false;
		right->hide();
	}

	setUp = true;
	return true;
}

void SpazzDockTabs::TrackDock(QDockWidget *dock)
{
	SpazzDockTabs *o = Owner();
	if (o != this) {
		o->TrackDock(dock);
		return;
	}

	if (!dock || qobject_cast<SpazzDockTabs *>(dock)) {
		return;
	}

	if (!dock->property("spazzTracked").toBool()) {
		dock->setProperty("spazzTracked", true);
		dock->installEventFilter(this);

		auto refreshHints = [this]() {
			for (SpazzDockTabs *a : Areas()) {
				for (int i = 0; i < a->tabs->count(); i++) {
					if (SpazzDockTabPage *page = a->pageAt(i)) {
						page->UpdateEmptyHint();
					}
				}
			}
		};
		connect(dock, &QDockWidget::topLevelChanged, this, refreshHints, Qt::QueuedConnection);
		connect(dock, &QDockWidget::visibilityChanged, this, refreshHints, Qt::QueuedConnection);

		/* Showing a dock from the Docks menu jumps to its tab. */
		connect(dock->toggleViewAction(), &QAction::triggered, this, [this, dock](bool checked) {
			if (checked) {
				EnsureDockVisible(dock);
			}
		});
	}

	auto pending = pendingDocks.find(dock->objectName());
	if (pending != pendingDocks.end()) {
		QPointer<SpazzDockTabPage> page = pending.value();
		pendingDocks.erase(pending);
		if (page) {
			MoveDockToPage(dock, page);
		}
	}
}

void SpazzDockTabs::EnsureDockVisible(QDockWidget *dock)
{
	SpazzDockTabPage *page = PageForDock(dock);
	SpazzDockTabs *a = AreaOfPage(page);
	if (page && a && !dock->isFloating()) {
		a->show();
		a->tabs->setCurrentWidget(page);
	}
}

/* ---- dragging ---- */

bool SpazzDockTabs::eventFilter(QObject *obj, QEvent *event)
{
	if (obj == tabs->tabBar()) {
		TabBarEvent(event);
		return QDockWidget::eventFilter(obj, event);
	}

	QDockWidget *dock = qobject_cast<QDockWidget *>(obj);
	if (dock && this == Owner() && event->type() == QEvent::Move && dock->isFloating() && LeftButtonDown()) {
		/* A floating dock is being dragged; watch for it to be dropped
		 * on a tab, the + button, or the current tab's area. */
		if (draggingDock != dock) {
			draggingDock = dock;
			hoverArea = nullptr;
			hoverTab = -1;
			hoverTicks = 0;
		}
		if (!dragPoll.isActive()) {
			dragPoll.start();
		}
	}
	return QDockWidget::eventFilter(obj, event);
}

/* A whole tab dragged out of this tab strip and let go over the other
 * area moves there, docks and all. Inside the strip, Qt's own tab moving
 * still reorders it. */
void SpazzDockTabs::TabBarEvent(QEvent *event)
{
	QTabBar *bar = tabs->tabBar();

	switch (event->type()) {
	case QEvent::MouseButtonPress: {
		QMouseEvent *me = static_cast<QMouseEvent *>(event);
		if (me->button() != Qt::LeftButton) {
			return;
		}
		tabPressPage = pageAt(bar->tabAt(me->position().toPoint()));
		tabPressGlobal = me->globalPosition().toPoint();
		tabDragOut = false;
		otherShownForDrag = false;
		return;
	}
	case QEvent::MouseMove: {
		QMouseEvent *me = static_cast<QMouseEvent *>(event);
		if (!tabPressPage || !(me->buttons() & Qt::LeftButton)) {
			return;
		}
		const QPoint global = me->globalPosition().toPoint();
		const bool outside = !bar->rect().adjusted(-24, -24, 24, 24).contains(bar->mapFromGlobal(global));

		if (!tabDragOut && outside &&
		    (global - tabPressGlobal).manhattanLength() >= QApplication::startDragDistance()) {
			tabDragOut = true;
			/* an empty, tucked-away area opens up as somewhere to drop */
			SpazzDockTabs *other = Other();
			if (other && other->isHidden()) {
				other->show();
				otherShownForDrag = true;
			}
		}

		if (tabDragOut) {
			SpazzDockTabs *target = AreaUnderCursor(global);
			if (target && target != this) {
				const QString name = tabs->tabText(tabs->indexOf(tabPressPage));
				QToolTip::showText(global,
						   QTStr(target->area == Area::Right ? "SPAZZ.DockTabs.TabDropRight"
										     : "SPAZZ.DockTabs.TabDropBottom")
							   .arg(name));
			} else {
				QToolTip::hideText();
			}
		}
		return;
	}
	case QEvent::MouseButtonRelease: {
		QMouseEvent *me = static_cast<QMouseEvent *>(event);
		if (me->button() != Qt::LeftButton) {
			return;
		}
		QPointer<SpazzDockTabPage> page = tabPressPage;
		const bool wasOut = tabDragOut;
		const bool shown = otherShownForDrag;
		tabPressPage = nullptr;
		tabDragOut = false;
		otherShownForDrag = false;

		if (!wasOut) {
			return;
		}
		QToolTip::hideText();

		const QPoint global = me->globalPosition().toPoint();
		SpazzDockTabs *target = AreaUnderCursor(global);
		if (target && target != this && page) {
			QTabBar *targetBar = target->tabs->tabBar();
			const int index = targetBar->tabAt(targetBar->mapFromGlobal(global));
			const Area which = target->area;
			/* after the tab strip has finished with this release */
			QTimer::singleShot(0, this, [this, page, which, index]() {
				if (page) {
					MoveTabToArea(page, which, index);
				}
			});
		} else if (shown) {
			QPointer<SpazzDockTabs> other = Other();
			QTimer::singleShot(0, this, [other]() {
				if (other) {
					other->UpdateAreaVisibility();
				}
			});
		}
		return;
	}
	default:
		return;
	}
}

void SpazzDockTabs::PollDrag()
{
	if (!draggingDock) {
		dragPoll.stop();
		return;
	}

	if (!LeftButtonDown()) {
		FinishDrag();
		return;
	}

	const QPoint pos = QCursor::pos();
	SpazzDockTabs *overArea = nullptr;
	int index = -1;
	bool overPlus = false;
	for (SpazzDockTabs *a : Areas()) {
		if (!a->isVisible()) {
			continue;
		}
		QTabBar *bar = a->tabs->tabBar();
		int i = bar->tabAt(bar->mapFromGlobal(pos));
		if (i >= 0) {
			overArea = a;
			index = i;
			break;
		}
		if (a->addTabButton->rect().contains(a->addTabButton->mapFromGlobal(pos))) {
			overArea = a;
			overPlus = true;
			break;
		}
	}

	if (overArea != hoverArea || index != hoverTab) {
		hoverArea = overArea;
		hoverTab = index;
		hoverTicks = 0;
		if (overArea && index >= 0) {
			QToolTip::showText(pos, QTStr("SPAZZ.DockTabs.DropHint").arg(overArea->tabs->tabText(index)));
		} else if (overPlus) {
			QToolTip::showText(pos, QTStr("SPAZZ.DockTabs.DropNewHint"));
		} else {
			QToolTip::hideText();
		}
	} else if (overArea && index >= 0 && ++hoverTicks == kHoverSwitchTicks) {
		/* Hovering a tab while dragging opens it so you can aim inside. */
		overArea->tabs->setCurrentIndex(index);
	}
}

void SpazzDockTabs::FinishDrag()
{
	dragPoll.stop();
	QToolTip::hideText();

	QPointer<QDockWidget> dock = draggingDock;
	draggingDock = nullptr;
	hoverArea = nullptr;
	hoverTab = -1;
	hoverTicks = 0;

	if (!dock || !dock->isFloating()) {
		return; /* Qt docked it normally, nothing to do */
	}

	const QPoint pos = QCursor::pos();

	/* Defer: the OS may still be finishing the window move. */
	for (SpazzDockTabs *a : Areas()) {
		if (!a->isVisible()) {
			continue;
		}
		QTabBar *bar = a->tabs->tabBar();
		int index = bar->tabAt(bar->mapFromGlobal(pos));
		if (index >= 0) {
			QPointer<SpazzDockTabPage> page = a->pageAt(index);
			QPointer<SpazzDockTabs> where = a;
			QTimer::singleShot(0, this, [this, dock, page, where]() {
				if (dock && page) {
					MoveDockToPage(dock, page);
					if (where) {
						where->tabs->setCurrentWidget(page);
					}
				}
			});
			return;
		}

		if (a->addTabButton->rect().contains(a->addTabButton->mapFromGlobal(pos))) {
			QPointer<SpazzDockTabs> where = a;
			QTimer::singleShot(0, this, [dock, where]() {
				if (dock && where) {
					where->NewTabPrompt(dock);
				}
			});
			return;
		}
	}

	for (SpazzDockTabs *a : Areas()) {
		if (!a->isVisible()) {
			continue;
		}
		QWidget *current = a->tabs->currentWidget();
		SpazzDockTabPage *currentPage = qobject_cast<SpazzDockTabPage *>(current);
		if (currentPage && PageForDock(dock) != currentPage &&
		    current->rect().contains(current->mapFromGlobal(pos))) {
			QPointer<SpazzDockTabPage> page = currentPage;
			QTimer::singleShot(0, this, [this, dock, page]() {
				if (dock && page) {
					MoveDockToPage(dock, page);
				}
			});
			return;
		}
	}
}

/* ---- tabs ---- */

void SpazzDockTabs::NewTabPrompt(QDockWidget *withDock)
{
	QString suggestion = withDock ? withDock->windowTitle() : QTStr("SPAZZ.DockTabs.NewTab.Default");

	std::string name;
	bool accepted = NameDialog::AskForName(mainWindow, QTStr("SPAZZ.DockTabs.NewTab.Title"),
					       QTStr("SPAZZ.DockTabs.NewTab.Text"), name, suggestion);
	if (!accepted) {
		return;
	}
	if (name.empty()) {
		name = suggestion.toStdString();
	}

	SpazzDockTabPage *page = AddTab(QString::fromStdString(name));
	show();
	if (withDock) {
		MoveDockToPage(withDock, page);
	}
	tabs->setCurrentWidget(page);
}

void SpazzDockTabs::RenameTab(int index)
{
	std::string name;
	bool accepted = NameDialog::AskForName(mainWindow, QTStr("SPAZZ.DockTabs.Rename.Title"),
					       QTStr("SPAZZ.DockTabs.NewTab.Text"), name, tabs->tabText(index));
	if (accepted && !name.empty()) {
		tabs->setTabText(index, QString::fromStdString(name));
	}
}

void SpazzDockTabs::CloseTab(int index)
{
	SpazzDockTabPage *page = pageAt(index);
	SpazzDockTabPage *main = mainPage();
	if (!page || page == main) {
		return;
	}

	/* Nothing gets lost: the tab's docks go back to Main. */
	for (QDockWidget *dock : page->Docks()) {
		MoveDockToPage(dock, main);
	}

	for (auto it = Owner()->pendingDocks.begin(); it != Owner()->pendingDocks.end(); ++it) {
		if (it.value() == page) {
			it.value() = main;
		}
	}

	tabs->removeTab(index);
	page->deleteLater();
	UpdateAreaVisibility();
}

void SpazzDockTabs::ShowTabMenu(const QPoint &pos)
{
	int index = tabs->tabBar()->tabAt(pos);
	QMenu menu(this);

	menu.addAction(QTStr("SPAZZ.DockTabs.NewTab"), this, [this]() { NewTabPrompt(); });

	if (index >= 0) {
		QPointer<SpazzDockTabPage> page = pageAt(index);

		menu.addAction(QTStr("SPAZZ.DockTabs.Rename"), this, [this, index]() { RenameTab(index); });

		QAction *close =
			menu.addAction(QTStr("SPAZZ.DockTabs.Close"), this, [this, index]() { CloseTab(index); });
		close->setEnabled(page != mainPage());

		if (SpazzDockTabs *other = Other()) {
			const Area which = other->area;
			menu.addAction(QTStr(which == Area::Right ? "SPAZZ.DockTabs.MoveToRight"
								  : "SPAZZ.DockTabs.MoveToBottom"),
				       this, [this, page, which]() {
					       if (page) {
						       MoveTabToArea(page, which);
					       }
				       });
		}

		menu.addSeparator();

		QMenu *addMenu = menu.addMenu(QTStr("SPAZZ.DockTabs.AddDock").arg(tabs->tabText(index)));
		for (QDockWidget *dock : AllDocks()) {
			if (PageForDock(dock) == page && !dock->isFloating()) {
				continue;
			}
			QString title = dock->windowTitle().isEmpty() ? dock->objectName() : dock->windowTitle();
			addMenu->addAction(title, this, [this, dock, page]() {
				if (page) {
					MoveDockToPage(dock, page);
					tabs->setCurrentWidget(page);
				}
			});
		}
		addMenu->setEnabled(!addMenu->isEmpty());

		QMenu *outMenu = menu.addMenu(QTStr("SPAZZ.DockTabs.PopOut"));
		for (QDockWidget *dock : page->Docks()) {
			QString title = dock->windowTitle().isEmpty() ? dock->objectName() : dock->windowTitle();
			outMenu->addAction(title, this, [this, dock]() { MoveDockToMainWindow(dock); });
		}
		outMenu->setEnabled(!outMenu->isEmpty());
	}

	menu.exec(tabs->tabBar()->mapToGlobal(pos));
}

void SpazzDockTabs::PopulateDocksMenu(QMenu *menu)
{
	SpazzDockTabs *o = Owner();
	if (o != this) {
		o->PopulateDocksMenu(menu);
		return;
	}

	/* clear() doesn't delete submenus, only their actions' links */
	qDeleteAll(menu->findChildren<QMenu *>(Qt::FindDirectChildrenOnly));
	menu->clear();
	menu->addAction(QTStr("SPAZZ.DockTabs.NewTab"), this, [this]() { NewTabPrompt(); });
	if (right) {
		QPointer<SpazzDockTabs> side = right;
		menu->addAction(QTStr("SPAZZ.DockTabs.NewTabRight"), this, [side]() {
			if (side) {
				side->NewTabPrompt();
			}
		});

		QAction *showSide = menu->addAction(QTStr("SPAZZ.DockTabs.ShowRight"));
		showSide->setCheckable(true);
		showSide->setChecked(right->isVisible());
		connect(showSide, &QAction::toggled, this, [side](bool on) {
			if (!side) {
				return;
			}
			if (on && side->tabs->count() == 0) {
				side->AddTab(QTStr("SPAZZ.DockTabs.Chat"));
			}
			side->autoHidden = false;
			side->setVisible(on);
		});
	}
	menu->addSeparator();

	for (QDockWidget *dock : AllDocks()) {
		QString title = dock->windowTitle().isEmpty() ? dock->objectName() : dock->windowTitle();
		QMenu *dockMenu = menu->addMenu(title);
		SpazzDockTabPage *current = PageForDock(dock);

		for (SpazzDockTabs *a : Areas()) {
			if (a != this && a->tabs->count()) {
				dockMenu->addSeparator();
			}
			for (int i = 0; i < a->tabs->count(); i++) {
				SpazzDockTabPage *page = a->pageAt(i);
				QString label = a->area == Area::Right
							? QTStr("SPAZZ.DockTabs.RightSuffix").arg(a->tabs->tabText(i))
							: a->tabs->tabText(i);
				QPointer<SpazzDockTabs> where = a;
				QAction *action = dockMenu->addAction(label, this, [this, dock, page, where]() {
					MoveDockToPage(dock, page);
					if (where) {
						where->show();
						where->tabs->setCurrentWidget(page);
					}
				});
				action->setCheckable(true);
				action->setChecked(page == current && !dock->isFloating());
			}
		}

		dockMenu->addSeparator();
		dockMenu->addAction(QTStr("SPAZZ.DockTabs.NewTabWith"), this, [this, dock]() { NewTabPrompt(dock); });
		dockMenu->addAction(QTStr("SPAZZ.DockTabs.PopOutOne"), this,
				    [this, dock]() { MoveDockToMainWindow(dock); });
	}
}

void SpazzDockTabs::PlaceInTab(QDockWidget *dock, const QString &tabName, Area which)
{
	SpazzDockTabs *o = Owner();
	if (o != this) {
		o->PlaceInTab(dock, tabName, which);
		return;
	}
	if (!dock) {
		return;
	}

	SpazzDockTabs *target = AreaDock(which);
	if (!target) {
		target = this;
	}

	/* the asked-for side first, then the other one (you may have moved
	 * the tab) */
	SpazzDockTabPage *page = nullptr;
	SpazzDockTabs *pageArea = nullptr;
	QList<SpazzDockTabs *> order{target};
	for (SpazzDockTabs *a : Areas()) {
		if (a != target) {
			order.push_back(a);
		}
	}
	for (SpazzDockTabs *a : order) {
		for (int i = 0; i < a->tabs->count() && !page; i++) {
			if (a->tabs->tabText(i) == tabName) {
				page = a->pageAt(i);
				pageArea = a;
			}
		}
		if (page) {
			break;
		}
	}

	if (!page) {
		pageArea = target;
	}
	QWidget *current = pageArea->tabs->currentWidget();
	if (!page) {
		page = pageArea->AddTab(tabName);
	}

	pendingDocks.remove(dock->objectName());
	MoveDockToPage(dock, page);

	if (current) {
		pageArea->tabs->setCurrentWidget(current);
	}
}

void SpazzDockTabs::ShowDock(QDockWidget *dock)
{
	if (!dock) {
		return;
	}
	if (SpazzDockTabPage *page = PageForDock(dock)) {
		SpazzDockTabs *a = AreaOfPage(page);
		if (a) {
			a->autoHidden = false;
			a->show();
			a->tabs->setCurrentWidget(page);
		}
		dock->show();
		return;
	}
	dock->show();
	dock->raise();
	dock->activateWindow();
}
