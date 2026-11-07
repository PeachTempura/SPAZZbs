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

#include <memory>
#include <vector>

class OBSPropertiesView;
class QLabel;
class QTabWidget;
class QToolButton;

class SpazzLiveControls : public QWidget {
	Q_OBJECT

public:
	/* one floater per source: opening it again brings it to the front */
	static void Open(obs_source_t *source, QWidget *parent);
	/* has settings, or can take Effects+ */
	static bool CanOpen(obs_source_t *source);
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
	OBSSignal filterAddSignal;
	OBSSignal filterRemoveSignal;
	OBSSignal filterReorderSignal;
	OBSData oldSettings;
	bool removed = false;

	QWidget *header = nullptr;
	QLabel *title = nullptr;
	QToolButton *pinButton = nullptr;
	OBSPropertiesView *view = nullptr; /* the source's own settings */
	QTabWidget *tabs = nullptr;
	/* its Effects+ filters, one tab each: the same controls as in Filters */
	struct FilterTab {
		OBSWeakSource filter;
		QWidget *page = nullptr;
		OBSSignal renamed;
	};
	std::vector<std::unique_ptr<FilterTab>> filterTabs;
	void RebuildFilterTabs();
	void AddEffects();
	/* Apply, for whichever tab's source only takes changes when told to */
	class QPushButton *applyButton = nullptr;
	void UpdateApply();
	void WatchLoaded(OBSPropertiesView *v);
	static bool Loaded(QWidget *v);
	QPoint dragOffset;
	bool dragging = false;

	void SetPinned(bool pinned);
	void UpdateTitle();
	void RecordUndo();

	static void SourceRemoved(void *data, calldata_t *params);
	static void SourceRenamed(void *data, calldata_t *params);
	static void UpdateProperties(void *data, calldata_t *params);
	static void FiltersChanged(void *data, calldata_t *params);

	static QHash<QString, QPointer<SpazzLiveControls>> openWindows;
};
