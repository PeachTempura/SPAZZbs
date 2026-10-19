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

#include "SpazzDockTabs.hpp"

#include <OBSApp.hpp>
#include <dialogs/NameDialog.hpp>

#include <QAction>
#include <QCursor>
#include <QEvent>
#include <QGuiApplication>
#include <QChildEvent>
#include <QJsonArray>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
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
		if (!dock->objectName().isEmpty())
			result.push_back(dock);
	}
	return result;
}

/* ------------------------------------------------------------------------ */

SpazzDockTabs::SpazzDockTabs(QMainWindow *mainWindow_) : QDockWidget(mainWindow_), mainWindow(mainWindow_)
{
	setObjectName("spazzTabsDock");
	setWindowTitle(QTStr("SPAZZ.DockTabs"));
	setFeatures(QDockWidget::NoDockWidgetFeatures);
	setTitleBarWidget(new QWidget(this));
	setAllowedAreas(Qt::BottomDockWidgetArea);

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
	connect(bar, &QTabBar::customContextMenuRequested, this, &SpazzDockTabs::ShowTabMenu);
	connect(bar, &QTabBar::tabBarDoubleClicked, this, [this](int index) {
		if (index > 0)
			RenameTab(index);
	});

	setWidget(tabs);

	AddTab(QTStr("SPAZZ.DockTabs.Main"));

	dragPoll.setInterval(kPollMs);
	connect(&dragPoll, &QTimer::timeout, this, &SpazzDockTabs::PollDrag);
}

SpazzDockTabPage *SpazzDockTabs::pageAt(int index) const
{
	return qobject_cast<SpazzDockTabPage *>(tabs->widget(index));
}

SpazzDockTabPage *SpazzDockTabs::mainPage() const
{
	/* The Main tab can be dragged to another position, so find it by marker. */
	for (int i = 0; i < tabs->count(); i++) {
		SpazzDockTabPage *page = pageAt(i);
		if (page && page->property("spazzMain").toBool())
			return page;
	}
	return pageAt(0);
}

SpazzDockTabPage *SpazzDockTabs::AddTab(const QString &name)
{
	SpazzDockTabPage *page = new SpazzDockTabPage(tabs);
	if (tabs->count() == 0)
		page->setProperty("spazzMain", true);
	tabs->addTab(page, name);
	return page;
}

SpazzDockTabPage *SpazzDockTabs::PageForDock(QDockWidget *dock) const
{
	if (!dock)
		return nullptr;
	return qobject_cast<SpazzDockTabPage *>(dock->parentWidget());
}

QList<QDockWidget *> SpazzDockTabs::AllDocks() const
{
	QList<QDockWidget *> result;
	if (!mainWindow)
		return result;

	for (QDockWidget *dock : mainWindow->findChildren<QDockWidget *>()) {
		if (dock == this || dock->objectName().isEmpty())
			continue;
		result.push_back(dock);
	}
	return result;
}

void SpazzDockTabs::MoveDockToPage(QDockWidget *dock, SpazzDockTabPage *page)
{
	if (!dock || !page || dock == this)
		return;

	SpazzDockTabPage *oldPage = PageForDock(dock);
	if (oldPage == page && !dock->isFloating())
		return;

	if (oldPage)
		oldPage->removeDockWidget(dock);
	else if (mainWindow && dock->parentWidget() == mainWindow)
		mainWindow->removeDockWidget(dock);

	dock->setParent(page);

	/* Put new docks to the right of whatever is already in the tab. */
	QList<QDockWidget *> existing = page->Docks();
	existing.removeAll(dock);
	QDockWidget *last = nullptr;
	for (QDockWidget *d : existing) {
		if (!d->isFloating() && d->isVisible())
			last = d;
	}

	page->addDockWidget(Qt::TopDockWidgetArea, dock);
	if (last)
		page->splitDockWidget(last, dock, Qt::Horizontal);

	dock->setFloating(false);
	dock->show();
	TrackDock(dock);
}

void SpazzDockTabs::MoveDockToMainWindow(QDockWidget *dock)
{
	if (!dock || !mainWindow)
		return;

	SpazzDockTabPage *oldPage = PageForDock(dock);
	if (oldPage)
		oldPage->removeDockWidget(dock);

	dock->setParent(mainWindow);
	mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
	dock->setFloating(false);
	dock->show();
}

void SpazzDockTabs::ClearTabs()
{
	SpazzDockTabPage *keep = mainPage();

	for (int i = tabs->count() - 1; i >= 0; i--) {
		SpazzDockTabPage *page = pageAt(i);
		if (page == keep)
			continue;

		/* Send docks back to the main window, floating and hidden,
		 * the same way stock OBS resets extra docks. */
		for (QDockWidget *dock : page->Docks()) {
			page->removeDockWidget(dock);
			dock->setParent(mainWindow);
			mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
			dock->setFloating(true);
			dock->hide();
		}

		tabs->removeTab(i);
		page->deleteLater();
	}

	if (keep) {
		tabs->setTabText(tabs->indexOf(keep), QTStr("SPAZZ.DockTabs.Main"));
		tabs->tabBar()->moveTab(tabs->indexOf(keep), 0);
	}
}

void SpazzDockTabs::ResetToDefault(const DefaultDocks &docks)
{
	ClearTabs();
	SpazzDockTabPage *page = mainPage();

	/* Anything else living in Main goes back to the main window, hidden. */
	for (QDockWidget *dock : page->Docks()) {
		if (dock == docks.scenes || dock == docks.sources || dock == docks.mixer ||
		    dock == docks.transitions || dock == docks.controls)
			continue;
		page->removeDockWidget(dock);
		dock->setParent(mainWindow);
		mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
		dock->setFloating(true);
		dock->hide();
	}

	QList<QDockWidget *> row{docks.scenes, docks.sources, docks.mixer, docks.transitions, docks.controls};

	for (QDockWidget *dock : row) {
		SpazzDockTabPage *oldPage = PageForDock(dock);
		if (oldPage)
			oldPage->removeDockWidget(dock);
		else if (dock->parentWidget() == mainWindow)
			mainWindow->removeDockWidget(dock);
		dock->setParent(page);
	}

	page->addDockWidget(Qt::TopDockWidgetArea, row[0]);
	for (int i = 1; i < row.size(); i++)
		page->splitDockWidget(row[i - 1], row[i], Qt::Horizontal);

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
		show();
		mainWindow->resizeDocks({this}, {mainWindow->height() * 28 / 100}, Qt::Vertical);
	}

	tabs->setCurrentWidget(page);
}

void SpazzDockTabs::Save() const
{
	QJsonArray tabList;
	for (int i = 0; i < tabs->count(); i++) {
		SpazzDockTabPage *page = pageAt(i);
		if (!page)
			continue;

		QJsonArray dockNames;
		for (QDockWidget *dock : page->Docks())
			dockNames.append(dock->objectName());

		/* Docks that haven't been created yet this session (a plugin
		 * that failed to load, say) keep their spot for next time. */
		for (auto it = pendingDocks.cbegin(); it != pendingDocks.cend(); ++it) {
			if (it.value() == page)
				dockNames.append(it.key());
		}

		QJsonObject tab;
		tab["name"] = tabs->tabText(i);
		tab["main"] = page->property("spazzMain").toBool();
		tab["docks"] = dockNames;
		tab["state"] = QString::fromLatin1(page->saveState().toBase64());
		tabList.append(tab);
	}

	QJsonObject root;
	root["version"] = 1;
	root["current"] = tabs->currentIndex();
	root["tabs"] = tabList;

	QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
	config_set_string(App()->GetUserConfig(), kConfigSection, kConfigKey, json.constData());
}

bool SpazzDockTabs::Load(const DefaultDocks &docks)
{
	const char *json = config_get_string(App()->GetUserConfig(), kConfigSection, kConfigKey);
	if (!json || !*json)
		return false;

	QJsonDocument doc = QJsonDocument::fromJson(QByteArray(json));
	QJsonArray tabList = doc.object()["tabs"].toArray();
	if (tabList.isEmpty())
		return false;

	ClearTabs();

	QHash<QString, QDockWidget *> byName;
	for (QDockWidget *dock : AllDocks())
		byName.insert(dock->objectName(), dock);

	bool mainSeen = false;
	for (const QJsonValue &value : tabList) {
		QJsonObject tab = value.toObject();
		bool isMain = tab["main"].toBool() && !mainSeen;

		SpazzDockTabPage *page;
		if (isMain) {
			page = mainPage();
			tabs->setTabText(tabs->indexOf(page), tab["name"].toString());
			tabs->tabBar()->moveTab(tabs->indexOf(page), tabs->count() - 1);
			mainSeen = true;
		} else {
			page = AddTab(tab["name"].toString());
		}

		for (const QJsonValue &nameValue : tab["docks"].toArray()) {
			QString name = nameValue.toString();
			QDockWidget *dock = byName.value(name);
			if (dock)
				MoveDockToPage(dock, page);
			else
				pendingDocks.insert(name, page);
		}

		QByteArray state = QByteArray::fromBase64(tab["state"].toString().toLatin1());
		page->restoreState(state);
	}

	/* The stock segments must always live somewhere. If one is missing
	 * from the saved tabs, put it back in Main. */
	for (QDockWidget *dock : {docks.scenes, docks.sources, docks.mixer, docks.transitions, docks.controls}) {
		if (!PageForDock(dock) && dock->parentWidget() == mainWindow && !dock->isFloating() &&
		    mainWindow->dockWidgetArea(dock) == Qt::NoDockWidgetArea)
			MoveDockToPage(dock, mainPage());
	}

	int current = doc.object()["current"].toInt();
	if (current >= 0 && current < tabs->count())
		tabs->setCurrentIndex(current);

	return true;
}

void SpazzDockTabs::TrackDock(QDockWidget *dock)
{
	if (!dock || dock == this)
		return;

	if (!dock->property("spazzTracked").toBool()) {
		dock->setProperty("spazzTracked", true);
		dock->installEventFilter(this);

		auto refreshHints = [this]() {
			for (int i = 0; i < tabs->count(); i++) {
				if (SpazzDockTabPage *page = pageAt(i))
					page->UpdateEmptyHint();
			}
		};
		connect(dock, &QDockWidget::topLevelChanged, this, refreshHints, Qt::QueuedConnection);
		connect(dock, &QDockWidget::visibilityChanged, this, refreshHints, Qt::QueuedConnection);

		/* Showing a dock from the Docks menu jumps to its tab. */
		connect(dock->toggleViewAction(), &QAction::triggered, this, [this, dock](bool checked) {
			if (checked)
				EnsureDockVisible(dock);
		});
	}

	auto pending = pendingDocks.find(dock->objectName());
	if (pending != pendingDocks.end()) {
		QPointer<SpazzDockTabPage> page = pending.value();
		pendingDocks.erase(pending);
		if (page)
			MoveDockToPage(dock, page);
	}
}

void SpazzDockTabs::EnsureDockVisible(QDockWidget *dock)
{
	SpazzDockTabPage *page = PageForDock(dock);
	if (page && !dock->isFloating())
		tabs->setCurrentWidget(page);
}

bool SpazzDockTabs::eventFilter(QObject *obj, QEvent *event)
{
	QDockWidget *dock = qobject_cast<QDockWidget *>(obj);
	if (dock && event->type() == QEvent::Move && dock->isFloating() && LeftButtonDown()) {
		/* A floating dock is being dragged; watch for it to be dropped
		 * on a tab, the + button, or the current tab's area. */
		if (draggingDock != dock) {
			draggingDock = dock;
			hoverTab = -1;
			hoverTicks = 0;
		}
		if (!dragPoll.isActive())
			dragPoll.start();
	}
	return QDockWidget::eventFilter(obj, event);
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

	QTabBar *bar = tabs->tabBar();
	int index = bar->tabAt(bar->mapFromGlobal(QCursor::pos()));
	bool overPlus = addTabButton->rect().contains(addTabButton->mapFromGlobal(QCursor::pos()));

	if (index != hoverTab) {
		hoverTab = index;
		hoverTicks = 0;
		if (index >= 0)
			QToolTip::showText(QCursor::pos(), QTStr("SPAZZ.DockTabs.DropHint").arg(tabs->tabText(index)));
		else if (overPlus)
			QToolTip::showText(QCursor::pos(), QTStr("SPAZZ.DockTabs.DropNewHint"));
		else
			QToolTip::hideText();
	} else if (index >= 0 && ++hoverTicks == kHoverSwitchTicks) {
		/* Hovering a tab while dragging opens it so you can aim inside. */
		tabs->setCurrentIndex(index);
	}
}

void SpazzDockTabs::FinishDrag()
{
	dragPoll.stop();
	QToolTip::hideText();

	QPointer<QDockWidget> dock = draggingDock;
	draggingDock = nullptr;
	hoverTab = -1;
	hoverTicks = 0;

	if (!dock || !dock->isFloating())
		return; /* Qt docked it normally, nothing to do */

	QPoint pos = QCursor::pos();
	QTabBar *bar = tabs->tabBar();
	int index = bar->tabAt(bar->mapFromGlobal(pos));

	/* Defer: the OS may still be finishing the window move. */
	if (index >= 0) {
		QPointer<SpazzDockTabPage> page = pageAt(index);
		QTimer::singleShot(0, this, [this, dock, page]() {
			if (dock && page) {
				MoveDockToPage(dock, page);
				tabs->setCurrentWidget(page);
			}
		});
		return;
	}

	if (addTabButton->rect().contains(addTabButton->mapFromGlobal(pos))) {
		QTimer::singleShot(0, this, [this, dock]() {
			if (dock)
				NewTabPrompt(dock);
		});
		return;
	}

	QWidget *current = tabs->currentWidget();
	SpazzDockTabPage *currentPage = qobject_cast<SpazzDockTabPage *>(current);
	if (currentPage && PageForDock(dock) != currentPage && current->rect().contains(current->mapFromGlobal(pos))) {
		QPointer<SpazzDockTabPage> page = currentPage;
		QTimer::singleShot(0, this, [this, dock, page]() {
			if (dock && page)
				MoveDockToPage(dock, page);
		});
	}
}

void SpazzDockTabs::NewTabPrompt(QDockWidget *withDock)
{
	QString suggestion = withDock ? withDock->windowTitle() : QTStr("SPAZZ.DockTabs.NewTab.Default");

	std::string name;
	bool accepted = NameDialog::AskForName(mainWindow, QTStr("SPAZZ.DockTabs.NewTab.Title"),
					       QTStr("SPAZZ.DockTabs.NewTab.Text"), name, suggestion);
	if (!accepted)
		return;
	if (name.empty())
		name = suggestion.toStdString();

	SpazzDockTabPage *page = AddTab(QString::fromStdString(name));
	if (withDock)
		MoveDockToPage(withDock, page);
	tabs->setCurrentWidget(page);
}

void SpazzDockTabs::RenameTab(int index)
{
	std::string name;
	bool accepted = NameDialog::AskForName(mainWindow, QTStr("SPAZZ.DockTabs.Rename.Title"),
					       QTStr("SPAZZ.DockTabs.NewTab.Text"), name, tabs->tabText(index));
	if (accepted && !name.empty())
		tabs->setTabText(index, QString::fromStdString(name));
}

void SpazzDockTabs::CloseTab(int index)
{
	SpazzDockTabPage *page = pageAt(index);
	if (!page || page == mainPage())
		return;

	/* Nothing gets lost: the tab's docks go back to Main. */
	for (QDockWidget *dock : page->Docks())
		MoveDockToPage(dock, mainPage());

	for (auto it = pendingDocks.begin(); it != pendingDocks.end(); ++it) {
		if (it.value() == page)
			it.value() = mainPage();
	}

	tabs->removeTab(index);
	page->deleteLater();
}

void SpazzDockTabs::ShowTabMenu(const QPoint &pos)
{
	int index = tabs->tabBar()->tabAt(pos);
	QMenu menu(this);

	menu.addAction(QTStr("SPAZZ.DockTabs.NewTab"), this, [this]() { NewTabPrompt(); });

	if (index >= 0) {
		SpazzDockTabPage *page = pageAt(index);

		menu.addAction(QTStr("SPAZZ.DockTabs.Rename"), this, [this, index]() { RenameTab(index); });

		QAction *close = menu.addAction(QTStr("SPAZZ.DockTabs.Close"), this, [this, index]() { CloseTab(index); });
		close->setEnabled(page != mainPage());

		menu.addSeparator();

		QMenu *addMenu = menu.addMenu(QTStr("SPAZZ.DockTabs.AddDock").arg(tabs->tabText(index)));
		for (QDockWidget *dock : AllDocks()) {
			if (PageForDock(dock) == page && !dock->isFloating())
				continue;
			QString title = dock->windowTitle().isEmpty() ? dock->objectName() : dock->windowTitle();
			addMenu->addAction(title, this, [this, dock, page]() {
				MoveDockToPage(dock, page);
				tabs->setCurrentWidget(page);
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
	menu->clear();
	menu->addAction(QTStr("SPAZZ.DockTabs.NewTab"), this, [this]() { NewTabPrompt(); });
	menu->addSeparator();

	for (QDockWidget *dock : AllDocks()) {
		QString title = dock->windowTitle().isEmpty() ? dock->objectName() : dock->windowTitle();
		QMenu *dockMenu = menu->addMenu(title);
		SpazzDockTabPage *current = PageForDock(dock);

		for (int i = 0; i < tabs->count(); i++) {
			SpazzDockTabPage *page = pageAt(i);
			QAction *action = dockMenu->addAction(tabs->tabText(i), this, [this, dock, page]() {
				MoveDockToPage(dock, page);
				tabs->setCurrentWidget(page);
			});
			action->setCheckable(true);
			action->setChecked(page == current && !dock->isFloating());
		}

		dockMenu->addSeparator();
		dockMenu->addAction(QTStr("SPAZZ.DockTabs.NewTabWith"), this, [this, dock]() { NewTabPrompt(dock); });
		dockMenu->addAction(QTStr("SPAZZ.DockTabs.PopOutOne"), this,
				    [this, dock]() { MoveDockToMainWindow(dock); });
	}
}

void SpazzDockTabs::PlaceInTab(QDockWidget *dock, const QString &tabName)
{
	if (!dock)
		return;

	SpazzDockTabPage *page = nullptr;
	for (int i = 0; i < tabs->count(); i++) {
		if (tabs->tabText(i) == tabName)
			page = pageAt(i);
	}

	QWidget *current = tabs->currentWidget();
	if (!page)
		page = AddTab(tabName);

	pendingDocks.remove(dock->objectName());
	MoveDockToPage(dock, page);

	if (current)
		tabs->setCurrentWidget(current);
}
