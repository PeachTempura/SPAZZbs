/******************************************************************************
    SPAZZbs - "Live Controls": a small always-on-top floater with a source's
    settings that apply instantly, for tweaking things while live.
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <obs.hpp>

#include <QHash>
#include <QPointer>
#include <QWidget>

class OBSPropertiesView;
class QLabel;
class QToolButton;

class SpazzLiveControls : public QWidget {
	Q_OBJECT

public:
	/* one floater per source: opening it again brings it to the front */
	static void Open(obs_source_t *source, QWidget *parent);
	~SpazzLiveControls();

protected:
	void closeEvent(QCloseEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	bool eventFilter(QObject *obj, QEvent *e) override;

private:
	SpazzLiveControls(obs_source_t *source, QWidget *parent);

	OBSSource source;
	QString uuid;
	OBSSignal removedSignal;
	OBSSignal renamedSignal;
	OBSSignal updatePropertiesSignal;
	OBSData oldSettings;
	bool removed = false;

	QWidget *header = nullptr;
	QLabel *title = nullptr;
	QToolButton *pinButton = nullptr;
	OBSPropertiesView *view = nullptr;
	QPoint dragOffset;
	bool dragging = false;

	void SetPinned(bool pinned);
	void UpdateTitle();
	void RecordUndo();

	static void SourceRemoved(void *data, calldata_t *params);
	static void SourceRenamed(void *data, calldata_t *params);
	static void UpdateProperties(void *data, calldata_t *params);

	static QHash<QString, QPointer<SpazzLiveControls>> openWindows;
};
