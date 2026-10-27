/******************************************************************************
    SPAZZbs - "Stinger Studio": design SPAZZ Stinger transitions with a live
    preview between your real scenes, presets and sliders.
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
#include <QTimer>

class OBSPropertiesView;
class OBSQTDisplay;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

class SpazzStingerStudio : public QDialog {
	Q_OBJECT

public:
	static void Open(QWidget *parent);
	~SpazzStingerStudio();

protected:
	void closeEvent(QCloseEvent *e) override;

private:
	explicit SpazzStingerStudio(QWidget *parent);

	OBSSourceAutoRelease preview;
	OBSPropertiesView *view = nullptr;
	OBSQTDisplay *display = nullptr;

	QComboBox *editing = nullptr;
	QComboBox *fromScene = nullptr;
	QComboBox *toScene = nullptr;
	QLineEdit *nameEdit = nullptr;
	QPushButton *saveButton = nullptr;
	QPushButton *playButton = nullptr;
	QCheckBox *makeCurrent = nullptr;
	QLabel *status = nullptr;

	QTimer loopTimer;
	bool forward = true;
	bool paused = false;
	bool wasActive = false;
	qint64 nextStartMs = 0;

	void RefreshScenes();
	void RefreshEditing(const QString &selectUuid = QString());
	void EditingChanged();
	void ResetPreview();
	void Loop();
	void Save();
	QString UniqueName(const QString &base) const;
	OBSSource SceneFor(QComboBox *combo) const;

	static void Draw(void *data, uint32_t cx, uint32_t cy);

	static QPointer<SpazzStingerStudio> instance;
};
