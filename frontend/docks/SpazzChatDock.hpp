/******************************************************************************
    SPAZZbs - Chat dock: every platform's live chat in one list, each
    message marked with its platform's logo.
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <utility/SpazzChatParse.hpp>

#include <obs-frontend-api.h>

#include <QPointer>
#include <QWidget>

#include <array>
#include <vector>

class OBSDock;
class QLabel;
class QListView;
class QNetworkAccessManager;
class QPushButton;
class QTimer;
class QToolButton;
class SpazzChatConnector;
class SpazzChatModel;
class SpazzChatDelegate;

class SpazzChatView : public QWidget {
public:
	explicit SpazzChatView(QWidget *parent);
	~SpazzChatView();

	/* the dock (objectName "spazzChatDock") holding a SpazzChatView */
	static OBSDock *CreateDock(QWidget *mainWindow);

	void OpenSettings();
	void Reconfigure();

private:
	QNetworkAccessManager *net = nullptr;
	std::array<SpazzChatConnector *, spazzchat::PlatformCount> connectors{};
	std::array<QToolButton *, spazzchat::PlatformCount> chips{};
	std::array<bool, spazzchat::PlatformCount> shown{};
	std::array<int, spazzchat::PlatformCount> lastState{};
	QString xTarget;

	SpazzChatModel *model = nullptr;
	SpazzChatDelegate *delegate = nullptr;
	QListView *list = nullptr;
	QPushButton *jumpButton = nullptr;
	QLabel *emptyHint = nullptr;
	QTimer *flushTimer = nullptr;
	std::vector<spazzchat::Message> incoming;
	int unseen = 0;
	int fontPt = 10;
	bool exiting = false;

	void Receive(const spazzchat::Message &m);
	void Flush();
	void StatusChanged(int platform);
	void UpdateChip(int platform);
	void ApplyFilter();
	void SetFontPt(int pt);
	void UpdateEmptyHint();
	void OpenX();
	bool AtBottom() const;
	void SystemNote(int platform, const QString &text);

	static void FrontendEvent(enum obs_frontend_event event, void *data);
};
