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

#include <QWidget>
#include <QPointer>

#include <functional>
#include <vector>

class OBSDock;
class OBSPropertiesView;
class QDockWidget;
class QTabWidget;
class OBSQTDisplay;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;
class SponsorCard;

class SpazzSponsorCenter : public QWidget {
	Q_OBJECT

public:
	/* SPONSORCENTER lives in a dock (its own tab in the bottom area) */
	static OBSDock *CreateDock(QWidget *mainWindow);
	static std::function<void(QDockWidget *)> showDock;
	static void Open(QWidget *parent, const QString &uuid = QString());
	/* lets a spot's own Properties window open it here */
	static void RegisterOpenProc(QWidget *mainWindow);
	~SpazzSponsorCenter();

protected:
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;

private:
	explicit SpazzSponsorCenter(QWidget *parent);

	OBSSourceAutoRelease preview;
	bool previewShowing = false;
	QString pendingUuid;
	QTabWidget *lookTabs = nullptr;
	void Start();
	OBSPropertiesView *view = nullptr;
	OBSQTDisplay *display = nullptr;

	QComboBox *editing = nullptr;
	QLineEdit *nameEdit = nullptr;
	QPushButton *saveButton = nullptr;
	QPushButton *liveButton = nullptr;
	QLabel *status = nullptr;

	/* one card per sponsor */
	QScrollArea *cardScroll = nullptr;
	QVBoxLayout *cardsLayout = nullptr;
	QPushButton *addButton = nullptr;
	std::vector<SponsorCard *> cards;
	int lastTouched = -1; /* "+" copies this one */
	int focusCard = -1;   /* the preview holds this one on screen */
	bool dirty = false;   /* edits not yet saved to the spot */
	QTimer *pushTimer = nullptr;

	void RefreshEditing(const QString &selectUuid = QString());
	void EditingChanged();
	void Replay();
	void Save();
	void PlayLive();
	QString UniqueName(const QString &base) const;

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

	static QPointer<SpazzSponsorCenter> instance;
	static QPointer<OBSDock> dock;
};
