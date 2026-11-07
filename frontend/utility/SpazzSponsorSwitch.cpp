/******************************************************************************
    SPAZZbs - the Sponsors switch
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "SpazzSponsorSwitch.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>
#include <qt-wrappers.hpp>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QTimer>

#include "moc_SpazzSponsorSwitch.cpp"

static constexpr const char *kSponsorId = "spazz_sponsor";
static constexpr const char *kHotkeyOn = "SpazzSponsors.On";
static constexpr const char *kHotkeyOff = "SpazzSponsors.Off";

SpazzSponsorSwitch::SpazzSponsorSwitch(OBSBasic *main_) : QObject(main_), main(main_)
{
	enabled = !config_has_user_value(App()->GetUserConfig(), "SpazzSponsors", "Enabled") ||
		  config_get_bool(App()->GetUserConfig(), "SpazzSponsors", "Enabled");

	/* sponsor spots ask this when they're created */
	proc_handler_add(obs_get_proc_handler(),
			 "void spazz_sponsor_state(out bool enabled, out bool streaming, out bool recording)",
			 StateProc, this);

	QMetaObject::invokeMethod(this, &SpazzSponsorSwitch::Hook, Qt::QueuedConnection);
}

SpazzSponsorSwitch::~SpazzSponsorSwitch()
{
	if (hooked) {
		obs_frontend_remove_event_callback(FrontendEvent, this);
	}
}

void SpazzSponsorSwitch::Hook()
{
	/* the frontend API only exists once SPAZZbs has finished starting */
	if (!obs_frontend_get_main_window()) {
		QTimer::singleShot(500, this, &SpazzSponsorSwitch::Hook);
		return;
	}
	obs_frontend_add_event_callback(FrontendEvent, this);
	hooked = true;

	hotkeys = obs_hotkey_pair_register_frontend(
		kHotkeyOn, QT_TO_UTF8(QTStr("SpazzSponsors.HotkeyOn")), kHotkeyOff,
		QT_TO_UTF8(QTStr("SpazzSponsors.HotkeyOff")),
		[](void *data, obs_hotkey_pair_id, obs_hotkey_t *, bool pressed) {
			auto *self = static_cast<SpazzSponsorSwitch *>(data);
			if (!pressed || self->enabled) {
				return false;
			}
			QMetaObject::invokeMethod(self, [self]() { self->SetEnabled(true); }, Qt::QueuedConnection);
			return true;
		},
		[](void *data, obs_hotkey_pair_id, obs_hotkey_t *, bool pressed) {
			auto *self = static_cast<SpazzSponsorSwitch *>(data);
			if (!pressed || !self->enabled) {
				return false;
			}
			QMetaObject::invokeMethod(self, [self]() { self->SetEnabled(false); }, Qt::QueuedConnection);
			return true;
		},
		this, this);
	LoadHotkeys();
	Push();
}

/* hotkeys are kept with the profile, like OBS's own */
void SpazzSponsorSwitch::LoadHotkeys()
{
	config_t *cfg = main->Config();
	if (!cfg || hotkeys == OBS_INVALID_HOTKEY_PAIR_ID) {
		return;
	}
	auto load = [cfg](const char *name) -> obs_data_array_t * {
		const char *json = config_get_string(cfg, "Hotkeys", name);
		if (!json) {
			return nullptr;
		}
		OBSDataAutoRelease data = obs_data_create_from_json(json);
		return data ? obs_data_get_array(data, "bindings") : nullptr;
	};
	OBSDataArrayAutoRelease on = load(kHotkeyOn);
	OBSDataArrayAutoRelease off = load(kHotkeyOff);
	obs_hotkey_pair_load(hotkeys, on, off);
}

QWidget *SpazzSponsorSwitch::MakeToggle(QWidget *parent)
{
	QWidget *w = new QWidget(parent);
	QHBoxLayout *l = new QHBoxLayout(w);
	l->setContentsMargins(4, 0, 4, 4);
	toggle = new QCheckBox(QTStr("SpazzSponsors.Toggle"), w);
	toggle->setToolTip(QTStr("SpazzSponsors.Toggle.Tip"));
	toggle->setChecked(enabled);
	l->addWidget(toggle);
	l->addStretch(1);
	connect(toggle, &QCheckBox::toggled, this, &SpazzSponsorSwitch::SetEnabled);
	return w;
}

void SpazzSponsorSwitch::SetEnabled(bool on)
{
	if (toggle && toggle->isChecked() != on) {
		QSignalBlocker block(toggle);
		toggle->setChecked(on);
	}
	if (enabled == on) {
		return;
	}
	enabled = on;
	config_set_bool(App()->GetUserConfig(), "SpazzSponsors", "Enabled", on);
	Push();
}

/* tell every sponsor spot */
void SpazzSponsorSwitch::Push()
{
	struct State {
		bool enabled, streaming, recording;
	} st{enabled, obs_frontend_streaming_active(), obs_frontend_recording_active()};

	obs_enum_sources(
		[](void *param, obs_source_t *src) {
			if (strcmp(obs_source_get_unversioned_id(src), kSponsorId) != 0) {
				return true;
			}
			const State *s = static_cast<const State *>(param);
			calldata_t cd;
			calldata_init(&cd);
			calldata_set_bool(&cd, "streaming", s->streaming);
			calldata_set_bool(&cd, "recording", s->recording);
			calldata_set_bool(&cd, "enabled", s->enabled);
			proc_handler_call(obs_source_get_proc_handler(src), "set_state", &cd);
			calldata_free(&cd);
			return true;
		},
		&st);
}

void SpazzSponsorSwitch::FrontendEvent(enum obs_frontend_event event, void *data)
{
	auto *self = static_cast<SpazzSponsorSwitch *>(data);
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		self->Push();
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		self->LoadHotkeys();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		obs_frontend_remove_event_callback(FrontendEvent, self);
		self->hooked = false;
		break;
	default:
		break;
	}
}

void SpazzSponsorSwitch::StateProc(void *data, calldata_t *cd)
{
	auto *self = static_cast<SpazzSponsorSwitch *>(data);
	calldata_set_bool(cd, "enabled", self->enabled);
	calldata_set_bool(cd, "streaming", obs_frontend_get_main_window() && obs_frontend_streaming_active());
	calldata_set_bool(cd, "recording", obs_frontend_get_main_window() && obs_frontend_recording_active());
}
