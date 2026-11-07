/******************************************************************************
    SPAZZbs - the Sponsors switch: turns every SPONSORCENTER spot on or off,
    and tells spots that wait for it whether you're streaming / recording.
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <obs.hpp>
#include <obs-frontend-api.h>

#include <QObject>
#include <QPointer>

class OBSBasic;
class QCheckBox;
class QWidget;

class SpazzSponsorSwitch : public QObject {
	Q_OBJECT

public:
	explicit SpazzSponsorSwitch(OBSBasic *main);
	~SpazzSponsorSwitch();

	/* "Sponsors" switch for the Controls dock */
	QWidget *MakeToggle(QWidget *parent);

	bool Enabled() const { return enabled; }
	void SetEnabled(bool on);

private:
	OBSBasic *main;
	QPointer<QCheckBox> toggle;
	bool enabled = true;
	bool hooked = false;
	obs_hotkey_pair_id hotkeys = OBS_INVALID_HOTKEY_PAIR_ID;

	void Hook();
	void LoadHotkeys();
	void Push();

	static void FrontendEvent(enum obs_frontend_event event, void *data);
	static void StateProc(void *data, calldata_t *cd);
};
