/******************************************************************************
    SPAZZbs - self updater
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "SpazzUpdater.hpp"

#include <OBSApp.hpp>
#include <utility/RemoteTextThread.hpp>
#include <utility/platform.hpp>
#include <widgets/OBSBasic.hpp>
#include <qt-wrappers.hpp>

#include <json11.hpp>
#include <util/platform.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>

#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include "moc_SpazzUpdater.cpp"

#ifndef SPAZZBS_COMMIT
#define SPAZZBS_COMMIT ""
#endif

static constexpr const char *kLatestRef = "https://api.github.com/repos/PeachTempura/SPAZZbs/git/ref/tags/latest";

SpazzUpdater::SpazzUpdater(QWidget *window_) : QObject(window_), window(window_) {}

QString SpazzUpdater::BuildCommit()
{
	return QString::fromLatin1(SPAZZBS_COMMIT).trimmed().toLower();
}

void SpazzUpdater::Check(bool manual)
{
	if (checking.exchange(true))
		return;

	QPointer<SpazzUpdater> self(this);
	std::thread([self, manual]() {
		std::string body, error;
		long code = 0;
		bool ok = GetRemoteFile(kLatestRef, body, error, &code, nullptr, "", nullptr,
					{"Accept: application/vnd.github+json"}, nullptr, 15);

		QString latest;
		if (ok && code == 200) {
			std::string err;
			json11::Json json = json11::Json::parse(body, err);
			latest = QString::fromStdString(json["object"]["sha"].string_value()).toLower();
			if (latest.isEmpty()) {
				ok = false;
				error = "unexpected reply from GitHub";
			}
		} else if (ok) {
			ok = false;
			error = "GitHub answered " + std::to_string(code);
		}

		QString qerror = QString::fromStdString(error);
		QMetaObject::invokeMethod(
			qApp,
			[self, manual, ok, latest, qerror]() {
				if (self)
					self->CheckDone(manual, ok, latest, qerror);
			},
			Qt::QueuedConnection);
	}).detach();
}

void SpazzUpdater::CheckDone(bool manual, bool ok, const QString &latest, const QString &error)
{
	checking = false;

	if (!ok) {
		blog(LOG_WARNING, "[SPAZZbs update] check failed: %s", QT_TO_UTF8(error));
		if (manual && window)
			OBSMessageBox::warning(window, QTStr("SpazzUpdate.Title"), QTStr("SpazzUpdate.CheckFailed"));
		return;
	}

	const QString mine = BuildCommit();
	const bool known = mine.length() >= 7;
	const bool newer = known && !latest.startsWith(mine) && !mine.startsWith(latest);
	blog(LOG_INFO, "[SPAZZbs update] this build %s, newest %s", known ? QT_TO_UTF8(mine.left(8)) : "unknown",
	     QT_TO_UTF8(latest.left(8)));

	if (!window)
		return;

	if (known && !newer) {
		if (manual)
			OBSMessageBox::information(window, QTStr("SpazzUpdate.Title"), QTStr("SpazzUpdate.UpToDate"));
		return;
	}
	if (!known && !manual)
		return; /* a local build: never nag */

	OBSBasic *main = OBSBasic::Get();
	if (!manual && main && main->Active())
		return; /* don't interrupt a stream; it asks again next launch */

	/* another dialog is up (e.g. the first-launch import): ask after it */
	if (!manual && QApplication::activeModalWidget()) {
		QPointer<SpazzUpdater> self(this);
		QTimer::singleShot(5000, this, [self, latest]() {
			if (self)
				self->CheckDone(false, true, latest, QString());
		});
		return;
	}

	QMessageBox box(window);
	box.setIcon(QMessageBox::Information);
	box.setWindowTitle(QTStr("SpazzUpdate.Title"));
	box.setText(known ? QTStr("SpazzUpdate.Available") : QTStr("SpazzUpdate.Unknown"));
	box.setInformativeText(QTStr("SpazzUpdate.Explain"));
	QPushButton *now = box.addButton(QTStr("SpazzUpdate.Now"), QMessageBox::AcceptRole);
	box.addButton(QTStr("SpazzUpdate.Later"), QMessageBox::RejectRole);
	box.setDefaultButton(now);
	box.exec();

	if (box.clickedButton() != now)
		return;

	if (main && main->Active()) {
		OBSMessageBox::warning(window, QTStr("SpazzUpdate.Title"), QTStr("SpazzUpdate.WhileLive"));
		return;
	}

	StartUpdate();
}

void SpazzUpdater::StartUpdate()
{
#ifdef _WIN32
	std::string script;
	if (!GetDataFilePath("spazzbs-update.ps1", script)) {
		OBSMessageBox::warning(window, QTStr("SpazzUpdate.Title"), QTStr("SpazzUpdate.Failed"));
		return;
	}

	/* run a copy, since the update replaces the original */
	const QString temp = QDir::toNativeSeparators(QDir::tempPath() + "/spazzbs-update.ps1");
	QFile::remove(temp);
	if (!QFile::copy(QString::fromStdString(script), temp)) {
		OBSMessageBox::warning(window, QTStr("SpazzUpdate.Title"), QTStr("SpazzUpdate.Failed"));
		return;
	}

	/* ...\bin\64bit\SPAZZbs.exe -> the folder holding bin, data, core */
	QDir dir(QCoreApplication::applicationDirPath());
	dir.cdUp();
	dir.cdUp();
	const QString appDir = QDir::toNativeSeparators(dir.absolutePath());

	const QString cmd = QString("powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%1\" -AppDir \"%2\" "
				    "-WaitPid %3")
				    .arg(temp, appDir)
				    .arg(QCoreApplication::applicationPid());
	std::wstring wcmd = cmd.toStdWString();

	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si,
			    &pi)) {
		blog(LOG_WARNING, "[SPAZZbs update] could not start the updater (%lu)", GetLastError());
		OBSMessageBox::warning(window, QTStr("SpazzUpdate.Title"), QTStr("SpazzUpdate.Failed"));
		return;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);

	blog(LOG_INFO, "[SPAZZbs update] updater started for %s, closing", QT_TO_UTF8(appDir));
	if (window)
		QMetaObject::invokeMethod(window, "close", Qt::QueuedConnection);
#else
	OBSMessageBox::information(window, QTStr("SpazzUpdate.Title"), QTStr("SpazzUpdate.WindowsOnly"));
#endif
}
