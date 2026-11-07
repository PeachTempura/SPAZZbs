/******************************************************************************
    SPAZZbs - multistream: extra stream outputs that go live on Kick, X and
    Rumble alongside the main (YouTube) stream, reusing its encoder when
    the platform accepts it.
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <obs.hpp>
#include <util/config-file.h>

#include <QGroupBox>
#include <QObject>
#include <QPointer>
#include <QString>

#include <atomic>
#include <memory>
#include <vector>

class OBSBasic;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

/* one platform SPAZZbs can multistream to */
struct SpazzDestInfo {
	const char *id;      /* locale key part: SpazzMulti.<id>.* */
	const char *section; /* profile config section */
	const char *name;
	const char *serverHint;
	int maxBitrate;   /* kbps the platform takes */
	int defBitrate;   /* for a separate encoder */
	int maxKeyint;    /* seconds; shared encoder must be 1..max */
	bool exactKeyint; /* must be exactly maxKeyint */
	bool needCBR;
};

/* the Multistream section in Settings > Stream: a tab per platform */
class SpazzMultistreamSettings : public QGroupBox {
public:
	struct Page {
		const SpazzDestInfo *info = nullptr;
		QCheckBox *enable = nullptr;
		QLineEdit *server = nullptr;
		QLineEdit *key = nullptr;
		QComboBox *encoder = nullptr;
		QSpinBox *bitrate = nullptr;
	};

	explicit SpazzMultistreamSettings(QWidget *parent);

	void Load(config_t *config);
	void Save(config_t *config) const;

	/* hooked to Settings' "something changed" by the caller */
	std::vector<Page> pages;
};

class SpazzMultistream : public QObject {
	Q_OBJECT

public:
	explicit SpazzMultistream(OBSBasic *main);
	~SpazzMultistream();

	/* the "Also go live on" switches under Start Streaming */
	QWidget *MakeToggles(QWidget *parent);

	/* settings were saved, or the profile changed */
	void Refresh();

	enum class State { Off, Connecting, Live, Reconnecting, Failed };

	struct Dest {
		const SpazzDestInfo *info = nullptr;
		SpazzMultistream *owner = nullptr;
		QPointer<QCheckBox> toggle;
		QPointer<QLabel> dot;

		OBSOutputAutoRelease output;
		OBSServiceAutoRelease service;
		OBSEncoderAutoRelease ownVideo;
		OBSEncoderAutoRelease ownAudio;
		OBSSignal startSig, stopSig, reconnectSig, reconnectOkSig;
		bool stopping = false;
		bool restartPending = false;  /* asked to start while still stopping */
		std::atomic<uint64_t> gen{0}; /* which output the signals belong to */
		int ownBitrate = 0;
		QString note; /* why it has its own encoder */
	};

private:
	OBSBasic *main;
	std::vector<std::unique_ptr<Dest>> dests;

	void Hook();
	void MainStarted();
	void MainStopping();
	bool Enabled(const Dest &d) const;
	bool Configured(const Dest &d) const;
	bool MainLive() const;
	void Toggled(Dest &d, bool on);
	void Start(Dest &d);
	void Stop(Dest &d);
	void Release(Dest &d, bool async = true);
	void SetState(Dest &d, State state, const QString &detail = QString());
	obs_encoder_t *OwnVideo(Dest &d, int bitrate);
	obs_encoder_t *OwnAudio(Dest &d, size_t mixer);

	static void OnStart(void *data, calldata_t *cd);
	static void OnStop(void *data, calldata_t *cd);
	static void OnReconnect(void *data, calldata_t *cd);
	static void OnReconnectOk(void *data, calldata_t *cd);
};
