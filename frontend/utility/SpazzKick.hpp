/******************************************************************************
    SPAZZbs - multistream to Kick: a second stream output that goes live on
    Kick alongside the main (YouTube) stream, reusing its encoder when it can.
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

class OBSBasic;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

/* the Kick section in Settings > Stream */
class SpazzKickSettings : public QGroupBox {
public:
	explicit SpazzKickSettings(QWidget *parent);

	void Load(config_t *config);
	void Save(config_t *config) const;

	/* hooked to Settings' "something changed" by the caller */
	QCheckBox *enable = nullptr;
	QLineEdit *server = nullptr;
	QLineEdit *key = nullptr;
	QComboBox *encoder = nullptr;
	QSpinBox *bitrate = nullptr;

private:
	void UpdateEncoderRow();
};

class SpazzKick : public QObject {
	Q_OBJECT

public:
	explicit SpazzKick(OBSBasic *main);
	~SpazzKick();

	/* the "Multistream to Kick" switch under Start Streaming */
	QWidget *MakeToggle(QWidget *parent);

	/* the main stream connected / is stopping */
	void MainStarted();
	void MainStopping();

	/* settings were saved, or the profile changed */
	void Refresh();

	bool Active() const;

private:
	enum class State { Off, Connecting, Live, Reconnecting, Failed };

	OBSBasic *main;
	QPointer<QCheckBox> toggle;
	QPointer<QLabel> stateLabel;

	OBSOutputAutoRelease output;
	OBSServiceAutoRelease service;
	OBSEncoderAutoRelease ownVideo;
	OBSEncoderAutoRelease ownAudio;
	OBSSignal startSig, stopSig, reconnectSig, reconnectOkSig;
	bool stopping = false;

	void Hook();
	bool Wanted() const;
	bool Configured() const;
	void Toggled(bool on);
	void StartKick();
	void StopKick();
	void ReleaseOutput();
	void SetState(State state, const QString &detail = QString());

	obs_encoder_t *MakeVideoEncoder(const char *id);

	static void OnStart(void *data, calldata_t *cd);
	static void OnStop(void *data, calldata_t *cd);
	static void OnReconnect(void *data, calldata_t *cd);
	static void OnReconnectOk(void *data, calldata_t *cd);
};
