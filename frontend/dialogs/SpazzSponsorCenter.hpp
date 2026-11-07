/******************************************************************************
    SPAZZbs - SPONSORCENTER: your sponsors, one after another, in the one
    Sponsors source; edited with a live preview over your scene.
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <obs.hpp>
#include <obs-frontend-api.h>

#include <QWidget>
#include <QPointer>

#include <functional>
#include <vector>

class OBSDock;
class OBSPropertiesView;
class QDockWidget;
class QTabWidget;
class OBSQTDisplay;
class QLabel;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;
class SponsorCard;

/* SPONSORCENTER is a tool, not a source you make more of: it edits the one
 * Sponsors source, which every sponsor rotates through one at a time. */
class SpazzSponsorCenter : public QWidget {
	Q_OBJECT

public:
	/* SPONSORCENTER lives in a dock (its own tab in the bottom area) */
	static OBSDock *CreateDock(QWidget *mainWindow);
	static std::function<void(QDockWidget *)> showDock;
	static void Open(QWidget *parent);
	/* lets the Sponsors source's own Properties window open it here */
	static void RegisterOpenProc(QWidget *mainWindow);

	/* the one Sponsors source (folding any extra sponsor spots into it), or
	 * null when there isn't one yet */
	static OBSSource Sponsors();
	/* puts it in the current scene, making it first if needed */
	static void AddToCurrentScene(QWidget *parent);

	~SpazzSponsorCenter();

protected:
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;

private:
	explicit SpazzSponsorCenter(QWidget *parent);

	/* a private copy you edit and preview; Save puts it on stream */
	OBSSourceAutoRelease preview;
	bool previewShowing = false;
	QTabWidget *lookTabs = nullptr;
	void Start();
	void MakePreview(obs_data_t *from);
	OBSPropertiesView *view = nullptr;
	OBSQTDisplay *display = nullptr;

	QPushButton *saveButton = nullptr;
	QPushButton *revertButton = nullptr;
	QPushButton *liveButton = nullptr;
	QPushButton *sceneButton = nullptr;
	QLabel *where = nullptr;
	QLabel *status = nullptr;

	/* one card per sponsor */
	QScrollArea *cardScroll = nullptr;
	QVBoxLayout *cardsLayout = nullptr;
	QPushButton *addButton = nullptr;
	std::vector<SponsorCard *> cards;
	int lastTouched = -1; /* "+" copies this one */
	int focusCard = -1;   /* the preview holds this one on screen */
	bool dirty = false;   /* edits not yet on stream */
	QTimer *pushTimer = nullptr;
	QTimer *tidyTimer = nullptr;
	OBSSignal createdSig;
	bool loading = true; /* a scene collection is loading: leave it be */

	void LoadFromLive(bool fresh);
	void SetDirty(bool on);
	void UpdateWhere();
	void Replay();
	void Save();
	void PlayLive();

	void LoadCards();
	void SyncCardsFromSettings();
	SponsorCard *MakeCard(obs_data_t *item);
	void Relayout();
	void WriteItems();
	void Push();
	void CardChanged(SponsorCard *card);
	void Touched(SponsorCard *card);
	void CardAction(SponsorCard *card, int action);
	void AddSponsor();
	int IndexOf(const SponsorCard *card) const;

	static void Draw(void *data, uint32_t cx, uint32_t cy);
	static void FrontendEvent(enum obs_frontend_event event, void *data);
	static void SourceCreated(void *data, calldata_t *cd);

	static QPointer<SpazzSponsorCenter> instance;
	static QPointer<OBSDock> dock;
};
