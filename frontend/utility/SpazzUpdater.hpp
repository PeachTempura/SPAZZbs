/******************************************************************************
    SPAZZbs - self updater: checks the project's GitHub releases and swaps in
    the newest build without the user going anywhere near GitHub.
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

#include <atomic>

class QWidget;

class SpazzUpdater : public QObject {
	Q_OBJECT

public:
	explicit SpazzUpdater(QWidget *window);

	/* manual: also say so when there's nothing new, and allow updating a
	 * build that doesn't know which version it is */
	void Check(bool manual);

	static QString BuildCommit();

private:
	QPointer<QWidget> window;
	std::atomic<bool> checking{false};

	void CheckDone(bool manual, bool ok, const QString &latest, const QString &error);
	void StartUpdate();
};
