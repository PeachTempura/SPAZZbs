/******************************************************************************
    SPAZZbs - SPONSORCENTER: design animated sponsor spots with a live
    preview over your scene, then drop them into a scene.
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <obs.hpp>

#include <QDialog>
#include <QPointer>

class OBSPropertiesView;
class OBSQTDisplay;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

class SpazzSponsorCenter : public QDialog {
	Q_OBJECT

public:
	static void Open(QWidget *parent);
	~SpazzSponsorCenter();

protected:
	void closeEvent(QCloseEvent *e) override;

private:
	explicit SpazzSponsorCenter(QWidget *parent);

	OBSSourceAutoRelease preview;
	OBSPropertiesView *view = nullptr;
	OBSQTDisplay *display = nullptr;

	QComboBox *editing = nullptr;
	QLineEdit *nameEdit = nullptr;
	QPushButton *saveButton = nullptr;
	QPushButton *liveButton = nullptr;
	QLabel *status = nullptr;

	void RefreshEditing(const QString &selectUuid = QString());
	void EditingChanged();
	void Replay();
	void Save();
	void PlayLive();
	QString UniqueName(const QString &base) const;

	static void Draw(void *data, uint32_t cx, uint32_t cy);

	static QPointer<SpazzSponsorCenter> instance;
};
