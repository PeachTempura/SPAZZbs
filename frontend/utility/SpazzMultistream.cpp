/******************************************************************************
    SPAZZbs - multistream to Kick, X and Rumble
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "SpazzMultistream.hpp"

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
#include <QTabWidget>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <thread>

#include "moc_SpazzMultistream.cpp"

/* What each platform accepts from a shared encoder. A separate encoder is
 * always H.264, CBR, 2 second keyframes, which all of them take.
 *   Kick:   H.264, CBR, up to 8000 kbps, 2 s keyframes, 1080p60
 *   X:      H.264, up to 40 Mbps, 3 s keyframes recommended
 *   Rumble: H.264, keyframes every few seconds */
static const SpazzDestInfo kDests[] = {
	{"Kick", "SpazzKick", "Kick", "rtmps://fa723fc1b171.global-contribute.live-video.net", 8000, 6000, 2, true,
	 true},
	{"X", "SpazzX", "X", "rtmps://va.pscp.tv:443/x", 40000, 9000, 3, false, false},
	{"Rumble", "SpazzRumble", "Rumble", "rtmp://...", 20000, 6000, 4, false, false},
};

/* worker threads still holding outputs; OBS must not shut down under them */
static std::atomic<int> pendingWorkers{0};

static void RunWorker(std::function<void()> fn)
{
	pendingWorkers++;
	std::thread([fn]() {
		fn();
		pendingWorkers--;
	}).detach();
}

static void WaitForWorkers()
{
	/* a stream that's still timing out gets a little while to finish */
	for (int i = 0; i < 300 && pendingWorkers.load() > 0; i++) {
		QThread::msleep(50);
	}
}

static QString DestStr(const SpazzDestInfo *info, const char *what)
{
	return QTStr((std::string("SpazzMulti.") + info->id + "." + what).c_str());
}

/* ------------------------------------------------------------------------ */
/* Settings > Stream > Multistream                                           */

SpazzMultistreamSettings::SpazzMultistreamSettings(QWidget *parent)
	: QGroupBox(QTStr("SpazzMulti.Settings.Title"), parent)
{
	QVBoxLayout *v = new QVBoxLayout(this);
	QLabel *about = new QLabel(QTStr("SpazzMulti.Settings.About"), this);
	about->setWordWrap(true);
	about->setProperty("class", "text-muted");
	v->addWidget(about);

	QTabWidget *tabs = new QTabWidget(this);
	v->addWidget(tabs);

	for (const SpazzDestInfo &info : kDests) {
		Page pg;
		pg.info = &info;
		QWidget *page = new QWidget(tabs);
		QFormLayout *form = new QFormLayout(page);
		form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

		pg.enable = new QCheckBox(QTStr("SpazzMulti.Settings.Enable").arg(QString::fromUtf8(info.name)), page);
		form->addRow(pg.enable);

		pg.server = new QLineEdit(page);
		pg.server->setPlaceholderText(QString::fromUtf8(info.serverHint));
		form->addRow(QTStr("SpazzMulti.Settings.Server"), pg.server);

		QHBoxLayout *keyRow = new QHBoxLayout();
		pg.key = new QLineEdit(page);
		pg.key->setEchoMode(QLineEdit::Password);
		QPushButton *showKey = new QPushButton(QTStr("Show"), page);
		showKey->setCheckable(true);
		keyRow->addWidget(pg.key, 1);
		keyRow->addWidget(showKey);
		form->addRow(QTStr("SpazzMulti.Settings.Key"), keyRow);
		QLineEdit *key = pg.key;
		connect(showKey, &QPushButton::toggled, this, [key, showKey](bool on) {
			key->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
			showKey->setText(QTStr(on ? "Hide" : "Show"));
		});

		pg.encoder = new QComboBox(page);
		pg.encoder->addItem(QTStr("SpazzMulti.Settings.Encoder.Share"), "share");
		pg.encoder->addItem(QTStr("SpazzMulti.Settings.Encoder.Own"), "own");
		form->addRow(QTStr("SpazzMulti.Settings.Encoder"), pg.encoder);

		pg.bitrate = new QSpinBox(page);
		pg.bitrate->setRange(1000, info.maxBitrate);
		pg.bitrate->setSingleStep(250);
		pg.bitrate->setSuffix(" kbps");
		pg.bitrate->setToolTip(QTStr("SpazzMulti.Settings.Bitrate.Tip"));
		form->addRow(QTStr("SpazzMulti.Settings.Bitrate"), pg.bitrate);

		QLabel *where = new QLabel(DestStr(&info, "Where"), page);
		where->setWordWrap(true);
		where->setOpenExternalLinks(true);
		where->setTextFormat(Qt::RichText);
		where->setProperty("class", "text-muted");
		form->addRow(where);

		QComboBox *enc = pg.encoder;
		QSpinBox *br = pg.bitrate;
		connect(enc, &QComboBox::currentIndexChanged, this,
			[enc, br]() { br->setEnabled(enc->currentData().toString() == "own"); });

		tabs->addTab(page, QString::fromUtf8(info.name));
		pages.push_back(pg);
	}
}

void SpazzMultistreamSettings::Load(config_t *config)
{
	for (Page &pg : pages) {
		const char *sec = pg.info->section;
		pg.enable->setChecked(config_get_bool(config, sec, "Enabled"));
		pg.server->setText(QT_UTF8(config_get_string(config, sec, "Server")));
		pg.key->setText(QT_UTF8(config_get_string(config, sec, "Key")));
		const char *mode = config_get_string(config, sec, "Encoder");
		int idx = pg.encoder->findData(QT_UTF8(mode && *mode ? mode : "share"));
		pg.encoder->setCurrentIndex(idx < 0 ? 0 : idx);
		int br = (int)config_get_int(config, sec, "Bitrate");
		pg.bitrate->setValue(br > 0 ? br : pg.info->defBitrate);
		pg.bitrate->setEnabled(pg.encoder->currentData().toString() == "own");
	}
}

void SpazzMultistreamSettings::Save(config_t *config) const
{
	for (const Page &pg : pages) {
		const char *sec = pg.info->section;
		config_set_bool(config, sec, "Enabled", pg.enable->isChecked());
		config_set_string(config, sec, "Server", QT_TO_UTF8(pg.server->text().trimmed()));
		config_set_string(config, sec, "Key", QT_TO_UTF8(pg.key->text().trimmed()));
		config_set_string(config, sec, "Encoder", QT_TO_UTF8(pg.encoder->currentData().toString()));
		config_set_int(config, sec, "Bitrate", pg.bitrate->value());
	}
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

/* empty when the platform takes the main stream's video encoder as it is */
static QString WhyNotShare(const SpazzDestInfo *info, obs_encoder_t *video)
{
	if (!video) {
		return QTStr("SpazzMulti.Why.NoEncoder");
	}
	const char *codec = obs_encoder_get_codec(video);
	if (!codec || strcmp(codec, "h264") != 0) {
		return QTStr("SpazzMulti.Why.Codec").arg(QT_UTF8(codec).toUpper(), QString::fromUtf8(info->name));
	}

	OBSDataAutoRelease es = obs_encoder_get_settings(video);
	const long long br = obs_data_get_int(es, "bitrate");
	const char *rc = obs_data_get_string(es, "rate_control");
	const long long keyint = obs_data_get_int(es, "keyint_sec");
	if (br > info->maxBitrate) {
		return QTStr("SpazzMulti.Why.Bitrate").arg(br).arg(QString::fromUtf8(info->name)).arg(info->maxBitrate);
	}
	if (info->needCBR && rc && *rc && strcmp(rc, "CBR") != 0) {
		return QTStr("SpazzMulti.Why.RateControl").arg(QT_UTF8(rc), QString::fromUtf8(info->name));
	}
	const bool keyOk = info->exactKeyint ? keyint == info->maxKeyint : (keyint >= 1 && keyint <= info->maxKeyint);
	if (!keyOk) {
		return QTStr("SpazzMulti.Why.Keyframes").arg(QString::fromUtf8(info->name)).arg(info->maxKeyint);
	}
	return QString();
}

SpazzMultistream::SpazzMultistream(OBSBasic *main_) : QObject(main_), main(main_)
{
	for (const SpazzDestInfo &info : kDests) {
		auto d = std::make_unique<Dest>();
		d->info = &info;
		d->owner = this;
		dests.push_back(std::move(d));
	}
	QMetaObject::invokeMethod(this, &SpazzMultistream::Hook, Qt::QueuedConnection);
}

SpazzMultistream::~SpazzMultistream()
{
	for (auto &d : dests) {
		d->stopping = true;
		if (d->output && obs_output_active(d->output)) {
			obs_output_force_stop(d->output);
		}
		Release(*d, false);
	}
}

void SpazzMultistream::Hook()
{
	/* the frontend API only exists once OBS has finished starting */
	if (!obs_frontend_get_main_window()) {
		QTimer::singleShot(500, this, &SpazzMultistream::Hook);
		return;
	}
	obs_frontend_add_event_callback(
		[](enum obs_frontend_event event, void *data) {
			SpazzMultistream *self = static_cast<SpazzMultistream *>(data);
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
				for (auto &d : self->dests) {
					d->stopping = true;
					d->restartPending = false;
					if (d->output && obs_output_active(d->output)) {
						obs_output_force_stop(d->output);
					}
					/* OBS is shutting down: finish here, not on a thread */
					self->Release(*d, false);
				}
				WaitForWorkers();
				break;
			default:
				break;
			}
		},
		this);
	Refresh();
}

QWidget *SpazzMultistream::MakeToggles(QWidget *parent)
{
	QWidget *w = new QWidget(parent);
	QHBoxLayout *l = new QHBoxLayout(w);
	l->setContentsMargins(4, 3, 4, 4);
	l->setSpacing(6);
	QLabel *also = new QLabel(QTStr("SpazzMulti.AlsoOn"), w);
	also->setToolTip(QTStr("SpazzMulti.AlsoOn.Tip"));
	l->addWidget(also);
	for (auto &dp : dests) {
		Dest *d = dp.get();
		d->toggle = new QCheckBox(QString::fromUtf8(d->info->name), w);
		d->toggle->setToolTip(DestStr(d->info, "Tip"));
		d->dot = new QLabel(w);
		d->dot->setMinimumWidth(10);
		l->addWidget(d->toggle);
		l->addWidget(d->dot);
		l->addSpacing(4);
		connect(d->toggle, &QCheckBox::toggled, this, [this, d](bool on) { Toggled(*d, on); });
	}
	l->addStretch(1);
	return w;
}

bool SpazzMultistream::Enabled(const Dest &d) const
{
	config_t *cfg = main->Config();
	return cfg && config_get_bool(cfg, d.info->section, "Enabled");
}

bool SpazzMultistream::Configured(const Dest &d) const
{
	config_t *cfg = main->Config();
	if (!cfg) {
		return false;
	}
	const char *server = config_get_string(cfg, d.info->section, "Server");
	const char *key = config_get_string(cfg, d.info->section, "Key");
	return server && *server && key && *key;
}

bool SpazzMultistream::MainLive() const
{
	OBSOutputAutoRelease mainOut = obs_frontend_get_streaming_output();
	return mainOut && obs_output_active(mainOut);
}

void SpazzMultistream::Refresh()
{
	for (auto &d : dests) {
		if (!d->toggle) {
			continue;
		}
		QSignalBlocker block(d->toggle);
		d->toggle->setChecked(Enabled(*d) && Configured(*d));
		if (!d->output) {
			SetState(*d, State::Off);
		}
	}
}

void SpazzMultistream::Toggled(Dest &d, bool on)
{
	config_t *cfg = main->Config();
	if (!cfg) {
		return;
	}

	if (on && !Configured(d)) {
		{
			QSignalBlocker block(d.toggle);
			d.toggle->setChecked(false);
		}
		OBSMessageBox::information(main, QTStr("SpazzMulti.Title"),
					   QTStr("SpazzMulti.NeedsKey").arg(QString::fromUtf8(d.info->name)) + "\n\n" +
						   DestStr(d.info, "WherePlain"));
		return;
	}

	config_set_bool(cfg, d.info->section, "Enabled", on);
	config_save_safe(cfg, "tmp", nullptr);

	/* already live: join or leave right now */
	if (on && MainLive()) {
		Start(d);
	} else if (!on) {
		Stop(d);
	}
}

void SpazzMultistream::MainStarted()
{
	for (auto &d : dests) {
		if (Enabled(*d) && Configured(*d)) {
			Start(*d);
		}
	}
}

void SpazzMultistream::MainStopping()
{
	for (auto &d : dests) {
		Stop(*d);
	}
}

/* a separate H.264 encoder; platforms wanting the same bitrate share one */
obs_encoder_t *SpazzMultistream::OwnVideo(Dest &d, int bitrate)
{
	for (auto &o : dests) {
		if (o.get() != &d && o->ownVideo && o->ownBitrate == bitrate) {
			d.ownVideo = obs_encoder_get_ref(o->ownVideo);
			d.ownBitrate = bitrate;
			return d.ownVideo;
		}
	}

	OBSDataAutoRelease s = obs_data_create();
	obs_data_set_string(s, "rate_control", "CBR");
	obs_data_set_int(s, "bitrate", bitrate);
	obs_data_set_int(s, "keyint_sec", 2);
	obs_data_set_string(s, "profile", "main");

	static const char *ids[] = {"obs_nvenc_h264_tex", "jim_nvenc", "h264_texture_amf", "obs_qsv11_v2", "obs_x264"};
	for (const char *id : ids) {
		if (!HasEncoder(id)) {
			continue;
		}
		if (strcmp(id, "obs_x264") == 0) {
			obs_data_set_string(s, "preset", "veryfast");
		}
		std::string name = std::string("SPAZZbs ") + d.info->name + " video";
		obs_encoder_t *enc = obs_video_encoder_create(id, name.c_str(), s, nullptr);
		if (!enc) {
			continue;
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
		d.ownVideo = enc;
		d.ownBitrate = bitrate;
		return enc;
	}
	return nullptr;
}

obs_encoder_t *SpazzMultistream::OwnAudio(Dest &d, size_t mixer)
{
	for (auto &o : dests) {
		if (o.get() != &d && o->ownAudio && obs_encoder_get_mixer_index(o->ownAudio) == mixer) {
			d.ownAudio = obs_encoder_get_ref(o->ownAudio);
			return d.ownAudio;
		}
	}
	OBSDataAutoRelease as = obs_data_create();
	obs_data_set_int(as, "bitrate", 160);
	d.ownAudio = obs_audio_encoder_create("ffmpeg_aac", "SPAZZbs multistream audio", as, mixer, nullptr);
	if (d.ownAudio) {
		obs_encoder_set_audio(d.ownAudio, obs_get_audio());
	}
	return d.ownAudio;
}

void SpazzMultistream::Start(Dest &d)
{
	if (d.output) {
		/* still finishing the last stream: go again once it has */
		if (d.stopping) {
			d.restartPending = true;
		}
		return;
	}
	config_t *cfg = main->Config();
	if (!cfg || !Configured(d)) {
		return;
	}
	const char *sec = d.info->section;

	OBSOutputAutoRelease mainOut = obs_frontend_get_streaming_output();
	obs_encoder_t *mainVideo = mainOut ? obs_output_get_video_encoder(mainOut) : nullptr;
	obs_encoder_t *mainAudio = mainOut ? obs_output_get_audio_encoder(mainOut, 0) : nullptr;

	/* share YouTube's encoder when the platform takes it (no extra load),
	 * otherwise give it its own */
	const char *mode = config_get_string(cfg, sec, "Encoder");
	const bool wantOwn = mode && strcmp(mode, "own") == 0;
	d.note.clear();
	if (!wantOwn) {
		QString why = WhyNotShare(d.info, mainVideo);
		if (!why.isEmpty()) {
			d.note = QTStr("SpazzMulti.OwnEncoder").arg(why);
			blog(LOG_INFO, "[SPAZZbs %s] separate encoder: %s", d.info->name, QT_TO_UTF8(why));
		}
	}

	obs_encoder_t *video = mainVideo;
	if (wantOwn || !d.note.isEmpty()) {
		int br = (int)config_get_int(cfg, sec, "Bitrate");
		br = br > 0 ? std::min(br, d.info->maxBitrate) : d.info->defBitrate;
		video = OwnVideo(d, br);
	}
	obs_encoder_t *audio = mainAudio;
	if (!audio || strcmp(obs_encoder_get_codec(audio), "aac") != 0) {
		/* same audio track the main stream sends */
		audio = OwnAudio(d, audio ? obs_encoder_get_mixer_index(audio) : 0);
	}
	if (!video || !audio) {
		SetState(d, State::Failed, QTStr("SpazzMulti.Why.NoEncoder"));
		Release(d);
		return;
	}

	OBSDataAutoRelease ss = obs_data_create();
	obs_data_set_string(ss, "server", config_get_string(cfg, sec, "Server"));
	obs_data_set_string(ss, "key", config_get_string(cfg, sec, "Key"));
	obs_data_set_bool(ss, "use_auth", false);
	std::string name = std::string("SPAZZbs ") + d.info->name;
	d.service = obs_service_create_private("rtmp_custom", (name + " service").c_str(), ss);
	d.output = obs_output_create("rtmp_output", name.c_str(), nullptr, nullptr);
	if (!d.service || !d.output) {
		SetState(d, State::Failed, QTStr("SpazzMulti.Failed").arg(QString::fromUtf8(d.info->name)));
		Release(d);
		return;
	}

	obs_output_set_service(d.output, d.service);
	obs_output_set_video_encoder(d.output, video);
	obs_output_set_audio_encoder(d.output, audio, 0);
	obs_output_set_reconnect_settings(d.output, 20, 3);

	d.gen++;
	signal_handler_t *sh = obs_output_get_signal_handler(d.output);
	d.startSig.Connect(sh, "start", OnStart, &d);
	d.stopSig.Connect(sh, "stop", OnStop, &d);
	d.reconnectSig.Connect(sh, "reconnect", OnReconnect, &d);
	d.reconnectOkSig.Connect(sh, "reconnect_success", OnReconnectOk, &d);

	d.stopping = false;
	SetState(d, State::Connecting);
	if (!obs_output_start(d.output)) {
		const char *err = obs_output_get_last_error(d.output);
		SetState(d, State::Failed,
			 err && *err ? QT_UTF8(err) : QTStr("SpazzMulti.Failed").arg(QString::fromUtf8(d.info->name)));
		Release(d);
		return;
	}
	blog(LOG_INFO, "[SPAZZbs %s] starting (%s encoder)", d.info->name,
	     video == mainVideo ? "shared" : obs_encoder_get_id(video));
}

void SpazzMultistream::Stop(Dest &d)
{
	if (!d.output) {
		SetState(d, State::Off);
		return;
	}
	d.stopping = true;
	d.restartPending = false;
	if (obs_output_active(d.output) || obs_output_reconnecting(d.output)) {
		/* stopping a stream that's reconnecting waits for its connection
		 * attempt to time out, so do it off the UI thread; "stop" fires
		 * when it's done and the output is released then */
		OBSOutput out = d.output.Get();
		RunWorker([out]() { obs_output_stop(out); });
	} else {
		Release(d);
		SetState(d, State::Off);
	}
}

void SpazzMultistream::Release(Dest &d, bool async)
{
	d.startSig.Disconnect();
	d.stopSig.Disconnect();
	d.reconnectSig.Disconnect();
	d.reconnectOkSig.Disconnect();
	d.ownBitrate = 0;

	/* destroying an output that's still connecting waits for the attempt
	 * to give up, so let a worker thread hold the last references */
	OBSOutput out = d.output.Get();
	OBSService svc = d.service.Get();
	OBSEncoder vid = d.ownVideo.Get();
	OBSEncoder aud = d.ownAudio.Get();
	d.output = nullptr;
	d.service = nullptr;
	d.ownVideo = nullptr;
	d.ownAudio = nullptr;
	if (async && out) {
		RunWorker([out, svc, vid, aud]() mutable {
			out = nullptr; /* output first: it holds the others */
			svc = nullptr;
			vid = nullptr;
			aud = nullptr;
		});
	}
}

void SpazzMultistream::SetState(Dest &d, State state, const QString &detail)
{
	if (!d.dot) {
		return;
	}
	QString word, color;
	switch (state) {
	case State::Off:
		break;
	case State::Connecting:
		word = QTStr("SpazzMulti.State.Connecting");
		color = "#9a9a9a";
		break;
	case State::Live:
		word = QTStr("SpazzMulti.State.Live");
		color = "#53fc18";
		break;
	case State::Reconnecting:
		word = QTStr("SpazzMulti.State.Reconnecting");
		color = "#f0a020";
		break;
	case State::Failed:
		word = QTStr("SpazzMulti.State.Failed");
		color = "#ff4d4d";
		break;
	}
	d.dot->setText(color.isEmpty() ? QString() : QString::fromUtf8("\xE2\x97\x8F"));
	d.dot->setStyleSheet(color.isEmpty() ? QString() : QString("color: %1;").arg(color));

	QString tip = word.isEmpty() ? QString() : QString("%1: %2").arg(QString::fromUtf8(d.info->name), word);
	if (!detail.isEmpty()) {
		tip += "\n\n" + detail;
	}
	if (!d.note.isEmpty() && state != State::Off) {
		tip += "\n\n" + d.note;
	}
	d.dot->setToolTip(tip);
	if (d.toggle) {
		d.toggle->setToolTip(tip.isEmpty() ? DestStr(d.info, "Tip") : tip);
	}
	if (state == State::Failed && !detail.isEmpty()) {
		blog(LOG_WARNING, "[SPAZZbs %s] %s", d.info->name, QT_TO_UTF8(detail));
	}
}

/* output signals arrive on OBS's threads; hop to the UI */

void SpazzMultistream::OnStart(void *data, calldata_t *)
{
	Dest *d = static_cast<Dest *>(data);
	QMetaObject::invokeMethod(d->owner, [d]() { d->owner->SetState(*d, State::Live); }, Qt::QueuedConnection);
}

void SpazzMultistream::OnReconnect(void *data, calldata_t *)
{
	Dest *d = static_cast<Dest *>(data);
	QMetaObject::invokeMethod(
		d->owner, [d]() { d->owner->SetState(*d, State::Reconnecting); }, Qt::QueuedConnection);
}

void SpazzMultistream::OnReconnectOk(void *data, calldata_t *)
{
	Dest *d = static_cast<Dest *>(data);
	QMetaObject::invokeMethod(d->owner, [d]() { d->owner->SetState(*d, State::Live); }, Qt::QueuedConnection);
}

void SpazzMultistream::OnStop(void *data, calldata_t *cd)
{
	Dest *d = static_cast<Dest *>(data);
	const uint64_t gen = d->gen.load();
	const long long code = calldata_int(cd, "code");
	obs_output_t *out = static_cast<obs_output_t *>(calldata_ptr(cd, "output"));
	const char *err = out ? obs_output_get_last_error(out) : nullptr;
	const QString lastError = err ? QT_UTF8(err) : QString();

	QMetaObject::invokeMethod(
		d->owner,
		[d, gen, code, lastError]() {
			SpazzMultistream *self = d->owner;
			if (gen != d->gen.load() || !d->output) {
				return; /* from an output that's already been replaced */
			}
			const bool wanted = d->stopping || code == OBS_OUTPUT_SUCCESS;
			self->Release(*d);
			if (wanted) {
				self->SetState(*d, State::Off);
				const bool again = d->restartPending;
				d->restartPending = false;
				if (again && self->Enabled(*d) && self->Configured(*d) && self->MainLive()) {
					self->Start(*d);
				}
				return;
			}
			QString why;
			switch (code) {
			case OBS_OUTPUT_BAD_PATH:
				why = QTStr("SpazzMulti.Error.BadPath").arg(QString::fromUtf8(d->info->name));
				break;
			case OBS_OUTPUT_INVALID_STREAM:
				why = QTStr("SpazzMulti.Error.BadKey").arg(QString::fromUtf8(d->info->name));
				break;
			case OBS_OUTPUT_CONNECT_FAILED:
				why = QTStr("SpazzMulti.Error.Connect").arg(QString::fromUtf8(d->info->name));
				break;
			case OBS_OUTPUT_DISCONNECTED:
				why = QTStr("SpazzMulti.Error.Disconnected").arg(QString::fromUtf8(d->info->name));
				break;
			default:
				why = QTStr("SpazzMulti.Failed").arg(QString::fromUtf8(d->info->name));
			}
			if (!lastError.isEmpty()) {
				why += "\n\n" + lastError;
			}
			self->SetState(*d, State::Failed, why);
		},
		Qt::QueuedConnection);
}
