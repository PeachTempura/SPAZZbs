/******************************************************************************
    SPAZZbs - unified chat: connections to each platform's live chat.
    Everything runs on the UI thread's event loop with Qt's async network
    classes; nothing blocks.
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Connector approaches (no code copied):
    - YouTube: innertube live chat, as in chat-downloader (xenova, MIT).
    - Kick: Pusher websocket, as in the kick-api crate (MIT/Apache-2.0).
    - Rumble: the streamer's Live Stream API, as in
      rumble-livestream-lib-go (MIT) and cocorum (Apache-2.0).
    - Twitch: anonymous IRC (justinfan), per Twitch's IRC documentation.
******************************************************************************/

#pragma once

#include "SpazzChatParse.hpp"

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <deque>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QSslSocket;

/* a minimal RFC 6455 client over TLS (Qt WebSockets isn't in OBS's deps) */
class SpazzWebSocket : public QObject {
public:
	explicit SpazzWebSocket(QObject *parent = nullptr);
	~SpazzWebSocket();

	void Open(const QUrl &url);
	void SendText(const QString &text);
	void Close();
	bool IsOpen() const { return open; }

	std::function<void()> onOpen;
	std::function<void(const QString &)> onText;
	std::function<void(const QString &reason)> onClosed;

private:
	QSslSocket *socket = nullptr;
	QUrl url;
	QByteArray key;
	QByteArray header;
	bool handshakeDone = false;
	bool open = false;
	bool closing = false;
	spazzchat::WsDecoder decoder;
	QTimer handshakeTimer;

	void SendFrame(int opcode, const QByteArray &payload);
	void OnReadable();
	void Fail(const QString &reason);
};

class SpazzChatConnector : public QObject {
public:
	enum State { Off, Connecting, Live, Waiting, Failed };

	SpazzChatConnector(int platform, QNetworkAccessManager *net, QObject *parent);
	virtual ~SpazzChatConnector() = default;

	/* channel / handle / URL / key, depending on the platform */
	void Configure(const QString &target);
	QString Target() const { return target; }
	virtual void Start() = 0;
	virtual void Stop();

	int Platform() const { return platform; }
	State GetState() const { return state; }
	QString Detail() const { return detail; }

	std::function<void(const spazzchat::Message &)> onMessage;
	std::function<void()> onStatus;

protected:
	int platform;
	QString target;
	QPointer<QNetworkAccessManager> net;
	State state = Off;
	QString detail;
	int attempt = 0;
	bool running = false;
	QTimer retryTimer;
	QSet<QString> seen;
	std::deque<QString> seenOrder;

	void SetState(State s, const QString &why = QString());
	/* tries again after 2, 4, 8 ... 60 s */
	void RetryLater(const QString &why, int fixedSec = 0);
	/* false when this message was already shown (polling platforms) */
	bool FirstTime(const std::string &id);
	void Emit(const spazzchat::Message &m);
	QNetworkReply *Get(const QUrl &url, bool browserLike = true);
	QNetworkReply *Post(const QUrl &url, const QByteArray &body, const QList<QPair<QByteArray, QByteArray>> &hdrs);
	void Abort();
	QList<QPointer<QNetworkReply>> pending;
};

class SpazzTwitchChat : public SpazzChatConnector {
public:
	SpazzTwitchChat(QNetworkAccessManager *net, QObject *parent);
	void Start() override;
	void Stop() override;

private:
	QSslSocket *socket = nullptr;
	QByteArray lineBuf;
	QTimer pingTimer;
	void OnReadable();
	void SendLine(const QByteArray &line);
};

class SpazzKickChat : public SpazzChatConnector {
public:
	SpazzKickChat(QNetworkAccessManager *net, QObject *parent);
	void Start() override;
	void Stop() override;

private:
	SpazzWebSocket *ws = nullptr;
	QString chatroom;
	QString chatroomSlug; /* the channel the chatroom id belongs to */
	QTimer pingTimer;
	void Connect();
};

class SpazzRumbleChat : public SpazzChatConnector {
public:
	SpazzRumbleChat(QNetworkAccessManager *net, QObject *parent);
	void Start() override;
	void Stop() override;

private:
	QTimer pollTimer;
	bool firstPoll = true;
	void Poll();
};

class SpazzYouTubeChat : public SpazzChatConnector {
public:
	SpazzYouTubeChat(QNetworkAccessManager *net, QObject *parent);
	void Start() override;
	void Stop() override;

private:
	QString videoId;
	spazzchat::YouTubeConfig cfg;
	QTimer pollTimer;
	void FindVideo();
	void OpenChat();
	void Poll();
};
