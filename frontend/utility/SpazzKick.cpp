/******************************************************************************
    SPAZZbs - multistream to Kick
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "SpazzKick.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>
#include <qt-wrappers.hpp>

#include <obs-frontend-api.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>

#include <algorithm>

#include "moc_SpazzKick.cpp"

/* profile settings (basic.ini), next to the main stream's own */
static constexpr const char *kSection = "SpazzKick";
static constexpr int kMaxBitrate = 8000; /* Kick's ceiling */

/* ------------------------------------------------------------------------ */
/* Settings > Stream > Multistream to Kick                                   */

SpazzKickSettings::SpazzKickSettings(QWidget *parent) : QGroupBox(QTStr("SpazzKick.Settings.Title"), parent)
{
	QFormLayout *form = new QFormLayout(this);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	QLabel *about = new QLabel(QTStr("SpazzKick.Settings.About"), this);
	about->setWordWrap(true);
	about->setProperty("class", "text-muted");
	form->addRow(about);

	enable = new QCheckBox(QTStr("SpazzKick.Settings.Enable"), this);
	form->addRow(enable);

	server = new QLineEdit(this);
	server->setPlaceholderText("rtmps://fa723fc1b171.global-contribute.live-video.net");
	form->addRow(QTStr("SpazzKick.Settings.Server"), server);

	QHBoxLayout *keyRow = new QHBoxLayout();
	key = new QLineEdit(this);
	key->setEchoMode(QLineEdit::Password);
	key->setPlaceholderText(QTStr("SpazzKick.Settings.KeyHint"));
	QPushButton *showKey = new QPushButton(QTStr("Show"), this);
	showKey->setCheckable(true);
	keyRow->addWidget(key, 1);
	keyRow->addWidget(showKey);
	form->addRow(QTStr("SpazzKick.Settings.Key"), keyRow);
	connect(showKey, &QPushButton::toggled, this, [this, showKey](bool on) {
		key->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
		showKey->setText(QTStr(on ? "Hide" : "Show"));
	});

	encoder = new QComboBox(this);
	encoder->addItem(QTStr("SpazzKick.Settings.Encoder.Share"), "share");
	encoder->addItem(QTStr("SpazzKick.Settings.Encoder.Own"), "own");
	form->addRow(QTStr("SpazzKick.Settings.Encoder"), encoder);

	bitrate = new QSpinBox(this);
	bitrate->setRange(1000, kMaxBitrate);
	bitrate->setSingleStep(250);
	bitrate->setSuffix(" kbps");
	form->addRow(QTStr("SpazzKick.Settings.Bitrate"), bitrate);

	QLabel *where = new QLabel(QTStr("SpazzKick.Settings.Where"), this);
	where->setWordWrap(true);
	where->setOpenExternalLinks(true);
	where->setProperty("class", "text-muted");
	form->addRow(where);

	connect(encoder, &QComboBox::currentIndexChanged, this, [this]() { UpdateEncoderRow(); });
}

void SpazzKickSettings::UpdateEncoderRow()
{
	bitrate->setEnabled(encoder->currentData().toString() == "own");
}

void SpazzKickSettings::Load(config_t *config)
{
	enable->setChecked(config_get_bool(config, kSection, "Enabled"));
	server->setText(QT_UTF8(config_get_string(config, kSection, "Server")));
	key->setText(QT_UTF8(config_get_string(config, kSection, "Key")));
	const char *mode = config_get_string(config, kSection, "Encoder");
	int idx = encoder->findData(QT_UTF8(mode && *mode ? mode : "share"));
	encoder->setCurrentIndex(idx < 0 ? 0 : idx);
	int br = (int)config_get_int(config, kSection, "Bitrate");
	bitrate->setValue(br > 0 ? br : 6000);
	UpdateEncoderRow();
}

void SpazzKickSettings::Save(config_t *config) const
{
	config_set_bool(config, kSection, "Enabled", enable->isChecked());
	config_set_string(config, kSection, "Server", QT_TO_UTF8(server->text().trimmed()));
	config_set_string(config, kSection, "Key", QT_TO_UTF8(key->text().trimmed()));
	config_set_string(config, kSection, "Encoder", QT_TO_UTF8(encoder->currentData().toString()));
	config_set_int(config, kSection, "Bitrate", bitrate->value());
}

/* ------------------------------------------------------------------------ */

static bool HasEncoder(const char *id)
{
	const char *type;
	for (size_t i = 0; obs_enum_encoder_types(i, &type); i++) {
		if (strcmp(type, id) == 0) {
			return true;
		}
	}
	return false;
}

SpazzKick::SpazzKick(OBSBasic *main_) : QObject(main_), main(main_)
{
	QMetaObject::invokeMethod(this, &SpazzKick::Hook, Qt::QueuedConnection);
}

void SpazzKick::Hook()
{
	/* the frontend API only exists once OBS has finished starting */
	if (!obs_frontend_get_main_window()) {
		QTimer::singleShot(500, this, &SpazzKick::Hook);
		return;
	}
	{
		{
			obs_frontend_add_event_callback(
				[](enum obs_frontend_event event, void *data) {
					SpazzKick *self = static_cast<SpazzKick *>(data);
					switch (event) {
					case OBS_FRONTEND_EVENT_STREAMING_STARTED:
						self->MainStarted();
						break;
					case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
					case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
						self->MainStopping();
						break;
					case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
					case OBS_FRONTEND_EVENT_FINISHED_LOADING:
						self->Refresh();
						break;
					case OBS_FRONTEND_EVENT_EXIT:
						self->stopping = true;
						if (self->output && obs_output_active(self->output)) {
							obs_output_force_stop(self->output);
						}
						self->ReleaseOutput();
						break;
					default:
						break;
					}
				},
				this);
			Refresh();
		}
	}
}

SpazzKick::~SpazzKick()
{
	stopping = true;
	if (output && obs_output_active(output)) {
		obs_output_force_stop(output);
	}
	ReleaseOutput();
}

QWidget *SpazzKick::MakeToggle(QWidget *parent)
{
	QWidget *w = new QWidget(parent);
	QHBoxLayout *l = new QHBoxLayout(w);
	l->setContentsMargins(4, 3, 4, 4);
	l->setSpacing(8);
	toggle = new QCheckBox(QTStr("SpazzKick.Toggle"), w);
	toggle->setToolTip(QTStr("SpazzKick.Toggle.Tip"));
	stateLabel = new QLabel(w);
	l->addWidget(toggle);
	l->addWidget(stateLabel);
	l->addStretch(1);
	connect(toggle, &QCheckBox::toggled, this, &SpazzKick::Toggled);
	return w;
}

bool SpazzKick::Wanted() const
{
	config_t *cfg = main->Config();
	return cfg && config_get_bool(cfg, kSection, "Enabled") && Configured();
}

bool SpazzKick::Configured() const
{
	config_t *cfg = main->Config();
	if (!cfg) {
		return false;
	}
	const char *server = config_get_string(cfg, kSection, "Server");
	const char *key = config_get_string(cfg, kSection, "Key");
	return server && *server && key && *key;
}

bool SpazzKick::Active() const
{
	return output && obs_output_active(output);
}

void SpazzKick::Refresh()
{
	if (!toggle) {
		return;
	}
	config_t *cfg = main->Config();
	QSignalBlocker block(toggle);
	toggle->setChecked(cfg && config_get_bool(cfg, kSection, "Enabled"));
	if (!output) {
		SetState(State::Off);
	}
}

void SpazzKick::Toggled(bool on)
{
	config_t *cfg = main->Config();
	if (!cfg) {
		return;
	}

	if (on && !Configured()) {
		{
			QSignalBlocker block(toggle);
			toggle->setChecked(false);
		}
		OBSMessageBox::information(main, QTStr("SpazzKick.Title"), QTStr("SpazzKick.NeedsKey"));
		return;
	}

	config_set_bool(cfg, kSection, "Enabled", on);
	config_save_safe(cfg, "tmp", nullptr);

	/* already live: join or leave Kick right now */
	OBSOutputAutoRelease mainOut = obs_frontend_get_streaming_output();
	const bool live = mainOut && obs_output_active(mainOut);
	if (on && live) {
		StartKick();
	} else if (!on) {
		StopKick();
	}
}

void SpazzKick::MainStarted()
{
	if (Wanted()) {
		StartKick();
	}
}

void SpazzKick::MainStopping()
{
	StopKick();
}

obs_encoder_t *SpazzKick::MakeVideoEncoder(const char *id)
{
	config_t *cfg = main->Config();
	int br = (int)config_get_int(cfg, kSection, "Bitrate");
	br = br > 0 ? std::min(br, kMaxBitrate) : 6000;

	/* what Kick asks for: H.264, CBR, 2 second keyframes, up to 1080p60 */
	OBSDataAutoRelease s = obs_data_create();
	obs_data_set_string(s, "rate_control", "CBR");
	obs_data_set_int(s, "bitrate", br);
	obs_data_set_int(s, "keyint_sec", 2);
	obs_data_set_string(s, "profile", "main");
	if (strcmp(id, "obs_x264") == 0) {
		obs_data_set_string(s, "preset", "veryfast");
	}

	obs_encoder_t *enc = obs_video_encoder_create(id, "SPAZZbs Kick video", s, nullptr);
	if (!enc) {
		return nullptr;
	}
	obs_encoder_set_video(enc, obs_get_video());

	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		if (ovi.output_height > 1080) {
			uint32_t w = (ovi.output_width * 1080 / ovi.output_height) & ~1u;
			obs_encoder_set_scaled_size(enc, w, 1080);
		}
		if (ovi.fps_den && ovi.fps_num / ovi.fps_den > 60) {
			obs_encoder_set_frame_rate_divisor(enc, 2);
		}
	}
	return enc;
}

void SpazzKick::StartKick()
{
	if (output) {
		return;
	}
	config_t *cfg = main->Config();
	if (!cfg || !Configured()) {
		return;
	}

	OBSOutputAutoRelease mainOut = obs_frontend_get_streaming_output();
	obs_encoder_t *mainVideo = mainOut ? obs_output_get_video_encoder(mainOut) : nullptr;
	obs_encoder_t *mainAudio = mainOut ? obs_output_get_audio_encoder(mainOut, 0) : nullptr;

	/* share YouTube's encoder when it already fits Kick (no extra load),
	 * otherwise give Kick its own */
	const char *mode = config_get_string(cfg, kSection, "Encoder");
	bool share = !(mode && strcmp(mode, "own") == 0);
	QString why;
	if (share) {
		if (!mainVideo) {
			why = QTStr("SpazzKick.Why.NoEncoder");
		} else if (strcmp(obs_encoder_get_codec(mainVideo), "h264") != 0) {
			why = QTStr("SpazzKick.Why.Codec").arg(QT_UTF8(obs_encoder_get_codec(mainVideo)).toUpper());
		} else {
			OBSDataAutoRelease es = obs_encoder_get_settings(mainVideo);
			const long long br = obs_data_get_int(es, "bitrate");
			const char *rc = obs_data_get_string(es, "rate_control");
			const long long keyint = obs_data_get_int(es, "keyint_sec");
			if (br > kMaxBitrate) {
				why = QTStr("SpazzKick.Why.Bitrate").arg(br);
			} else if (rc && *rc && strcmp(rc, "CBR") != 0) {
				why = QTStr("SpazzKick.Why.RateControl").arg(QT_UTF8(rc));
			} else if (keyint != 2) {
				why = QTStr("SpazzKick.Why.Keyframes");
			}
		}
		if (!why.isEmpty()) {
			share = false;
			blog(LOG_INFO, "[SPAZZbs Kick] using a separate encoder: %s", QT_TO_UTF8(why));
		}
	}

	obs_encoder_t *video = mainVideo;
	if (!share) {
		static const char *ids[] = {"obs_nvenc_h264_tex", "jim_nvenc", "h264_texture_amf", "obs_qsv11_v2",
					    "obs_x264"};
		for (const char *id : ids) {
			if (!HasEncoder(id)) {
				continue;
			}
			ownVideo = MakeVideoEncoder(id);
			if (ownVideo) {
				break;
			}
		}
		video = ownVideo;
	}
	obs_encoder_t *audio = mainAudio;
	if (!audio || strcmp(obs_encoder_get_codec(audio), "aac") != 0) {
		OBSDataAutoRelease as = obs_data_create();
		obs_data_set_int(as, "bitrate", 160);
		ownAudio = obs_audio_encoder_create("ffmpeg_aac", "SPAZZbs Kick audio", as, 0, nullptr);
		if (ownAudio) {
			obs_encoder_set_audio(ownAudio, obs_get_audio());
		}
		audio = ownAudio;
	}
	if (!video || !audio) {
		SetState(State::Failed, QTStr("SpazzKick.Why.NoEncoder"));
		ReleaseOutput();
		return;
	}

	OBSDataAutoRelease ss = obs_data_create();
	obs_data_set_string(ss, "server", config_get_string(cfg, kSection, "Server"));
	obs_data_set_string(ss, "key", config_get_string(cfg, kSection, "Key"));
	obs_data_set_bool(ss, "use_auth", false);
	service = obs_service_create_private("rtmp_custom", "SPAZZbs Kick service", ss);
	output = obs_output_create("rtmp_output", "SPAZZbs Kick", nullptr, nullptr);
	if (!service || !output) {
		SetState(State::Failed, QTStr("SpazzKick.Failed"));
		ReleaseOutput();
		return;
	}

	obs_output_set_service(output, service);
	obs_output_set_video_encoder(output, video);
	obs_output_set_audio_encoder(output, audio, 0);
	obs_output_set_reconnect_settings(output, 20, 3);

	signal_handler_t *sh = obs_output_get_signal_handler(output);
	startSig.Connect(sh, "start", OnStart, this);
	stopSig.Connect(sh, "stop", OnStop, this);
	reconnectSig.Connect(sh, "reconnect", OnReconnect, this);
	reconnectOkSig.Connect(sh, "reconnect_success", OnReconnectOk, this);

	stopping = false;
	SetState(State::Connecting, why.isEmpty() ? QString() : QTStr("SpazzKick.OwnEncoder").arg(why));
	if (!obs_output_start(output)) {
		const char *err = obs_output_get_last_error(output);
		SetState(State::Failed, err && *err ? QT_UTF8(err) : QTStr("SpazzKick.Failed"));
		ReleaseOutput();
		return;
	}
	blog(LOG_INFO, "[SPAZZbs Kick] starting (%s encoder)", share ? "shared" : obs_encoder_get_id(video));
}

void SpazzKick::StopKick()
{
	if (!output) {
		SetState(State::Off);
		return;
	}
	stopping = true;
	if (obs_output_active(output) || obs_output_reconnecting(output)) {
		obs_output_stop(output); /* "stop" fires, then it's released */
	} else {
		ReleaseOutput();
		SetState(State::Off);
	}
}

void SpazzKick::ReleaseOutput()
{
	startSig.Disconnect();
	stopSig.Disconnect();
	reconnectSig.Disconnect();
	reconnectOkSig.Disconnect();
	output = nullptr;
	service = nullptr;
	ownVideo = nullptr;
	ownAudio = nullptr;
}

void SpazzKick::SetState(State state, const QString &detail)
{
	if (!stateLabel) {
		return;
	}
	QString text, color, tip = detail;
	switch (state) {
	case State::Off:
		break;
	case State::Connecting:
		text = QTStr("SpazzKick.State.Connecting");
		color = "palette(mid)";
		break;
	case State::Live:
		text = QString::fromUtf8("\xE2\x97\x8F ") + QTStr("SpazzKick.State.Live");
		color = "#53fc18"; /* Kick green */
		break;
	case State::Reconnecting:
		text = QTStr("SpazzKick.State.Reconnecting");
		color = "#f0a020";
		break;
	case State::Failed:
		text = QTStr("SpazzKick.State.Failed");
		color = "#ff4d4d";
		break;
	}
	stateLabel->setText(text);
	stateLabel->setStyleSheet(color.isEmpty() ? QString() : QString("color: %1; font-weight: bold;").arg(color));
	stateLabel->setToolTip(tip);
	if (state == State::Failed && !detail.isEmpty()) {
		blog(LOG_WARNING, "[SPAZZbs Kick] %s", QT_TO_UTF8(detail));
	}
}

/* output signals arrive on OBS's threads; hop to the UI */

void SpazzKick::OnStart(void *data, calldata_t *)
{
	SpazzKick *self = static_cast<SpazzKick *>(data);
	QMetaObject::invokeMethod(
		self,
		[self]() { self->SetState(State::Live, self->stateLabel ? self->stateLabel->toolTip() : QString()); },
		Qt::QueuedConnection);
}

void SpazzKick::OnReconnect(void *data, calldata_t *)
{
	SpazzKick *self = static_cast<SpazzKick *>(data);
	QMetaObject::invokeMethod(self, [self]() { self->SetState(State::Reconnecting); }, Qt::QueuedConnection);
}

void SpazzKick::OnReconnectOk(void *data, calldata_t *)
{
	SpazzKick *self = static_cast<SpazzKick *>(data);
	QMetaObject::invokeMethod(self, [self]() { self->SetState(State::Live); }, Qt::QueuedConnection);
}

void SpazzKick::OnStop(void *data, calldata_t *cd)
{
	SpazzKick *self = static_cast<SpazzKick *>(data);
	const long long code = calldata_int(cd, "code");
	obs_output_t *out = static_cast<obs_output_t *>(calldata_ptr(cd, "output"));
	const char *err = out ? obs_output_get_last_error(out) : nullptr;
	const QString lastError = err ? QT_UTF8(err) : QString();

	QMetaObject::invokeMethod(
		self,
		[self, code, lastError]() {
			const bool wanted = self->stopping || code == OBS_OUTPUT_SUCCESS;
			self->ReleaseOutput();
			if (wanted) {
				self->SetState(State::Off);
				return;
			}
			QString why;
			switch (code) {
			case OBS_OUTPUT_BAD_PATH:
				why = QTStr("SpazzKick.Error.BadPath");
				break;
			case OBS_OUTPUT_INVALID_STREAM:
				why = QTStr("SpazzKick.Error.BadKey");
				break;
			case OBS_OUTPUT_CONNECT_FAILED:
				why = QTStr("SpazzKick.Error.Connect");
				break;
			case OBS_OUTPUT_DISCONNECTED:
				why = QTStr("SpazzKick.Error.Disconnected");
				break;
			default:
				why = QTStr("SpazzKick.Failed");
			}
			if (!lastError.isEmpty()) {
				why += "\n\n" + lastError;
			}
			self->SetState(State::Failed, why);
		},
		Qt::QueuedConnection);
}
