/******************************************************************************
    SPAZZbs - unified chat: connections to each platform's live chat.
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "SpazzChatNet.hpp"

#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSslSocket>

#include <algorithm>

using namespace spazzchat;

static const char *kUserAgent =
	"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";

/* ------------------------------------------------------------------------ */
/* websocket                                                                 */

SpazzWebSocket::SpazzWebSocket(QObject *parent) : QObject(parent)
{
	handshakeTimer.setSingleShot(true);
	handshakeTimer.setInterval(15000);
	connect(&handshakeTimer, &QTimer::timeout, this, [this]() { Fail(QStringLiteral("timed out connecting")); });
}

SpazzWebSocket::~SpazzWebSocket()
{
	onOpen = nullptr;
	onText = nullptr;
	onClosed = nullptr;
	if (socket) {
		socket->abort();
	}
}

void SpazzWebSocket::Open(const QUrl &url_)
{
	url = url_;
	handshakeDone = false;
	open = false;
	closing = false;
	header.clear();
	decoder = WsDecoder();

	if (socket) {
		socket->abort();
		socket->deleteLater();
	}
	socket = new QSslSocket(this);
	connect(socket, &QSslSocket::encrypted, this, [this]() {
		QByteArray raw(16, 0);
		for (char &c : raw) {
			c = char(QRandomGenerator::global()->bounded(256));
		}
		key = raw.toBase64();
		QByteArray path = url.path(QUrl::FullyEncoded).toUtf8();
		if (path.isEmpty()) {
			path = "/";
		}
		if (url.hasQuery()) {
			path += "?" + url.query(QUrl::FullyEncoded).toUtf8();
		}
		QByteArray req = "GET " + path + " HTTP/1.1\r\nHost: " + url.host().toUtf8() +
				 "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
				 "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: " + kUserAgent + "\r\nOrigin: https://" +
				 url.host().toUtf8() + "\r\n\r\n";
		socket->write(req);
	});
	connect(socket, &QSslSocket::readyRead, this, &SpazzWebSocket::OnReadable);
	connect(socket, &QSslSocket::disconnected, this, [this]() { Fail(QStringLiteral("connection closed")); });
	connect(socket, &QSslSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
		if (socket) {
			Fail(socket->errorString());
		}
	});
	handshakeTimer.start();
	socket->connectToHostEncrypted(url.host(), quint16(url.port(443)));
}

void SpazzWebSocket::SendFrame(int opcode, const QByteArray &payload)
{
	if (!socket || !handshakeDone) {
		return;
	}
	unsigned char mask[4];
	for (unsigned char &m : mask) {
		m = static_cast<unsigned char>(QRandomGenerator::global()->bounded(256));
	}
	const std::string frame = WsEncode(opcode, payload.toStdString(), mask);
	socket->write(frame.data(), qint64(frame.size()));
}

void SpazzWebSocket::SendText(const QString &text)
{
	SendFrame(1, text.toUtf8());
}

void SpazzWebSocket::Close()
{
	if (!socket) {
		return;
	}
	closing = true;
	SendFrame(8, QByteArray("\x03\xe8", 2)); /* 1000: normal */
	socket->disconnectFromHost();
	open = false;
}

void SpazzWebSocket::Fail(const QString &reason)
{
	handshakeTimer.stop();
	const bool wasClosing = closing;
	open = false;
	handshakeDone = false;
	closing = true;
	if (socket) {
		QSslSocket *s = socket;
		socket = nullptr;
		s->disconnect(this);
		s->abort();
		s->deleteLater();
	}
	if (!wasClosing && onClosed) {
		onClosed(reason);
	}
}

void SpazzWebSocket::OnReadable()
{
	if (!socket) {
		return;
	}
	QByteArray data = socket->readAll();
	if (!handshakeDone) {
		header += data;
		const int end = header.indexOf("\r\n\r\n");
		if (end < 0) {
			if (header.size() > 32768) {
				Fail(QStringLiteral("bad handshake"));
			}
			return;
		}
		const QByteArray head = header.left(end);
		data = header.mid(end + 4);
		header.clear();
		const QByteArray status = head.left(head.indexOf("\r\n"));
		if (!status.contains(" 101")) {
			Fail(QStringLiteral("server refused (%1)").arg(QString::fromUtf8(status.mid(9, 3))));
			return;
		}
		const QByteArray expect =
			QCryptographicHash::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1)
				.toBase64();
		if (!head.contains(expect)) {
			Fail(QStringLiteral("bad handshake"));
			return;
		}
		handshakeTimer.stop();
		handshakeDone = true;
		open = true;
		if (onOpen) {
			onOpen();
		}
		if (!socket) {
			return; /* closed from onOpen */
		}
	}
	if (data.isEmpty()) {
		return;
	}
	for (const WsFrame &f : decoder.Feed(data.toStdString())) {
		if (!socket) {
			return;
		}
		switch (f.opcode) {
		case 1:
			if (onText) {
				onText(QString::fromStdString(f.payload));
			}
			break;
		case 8:
			Fail(QStringLiteral("server closed the connection"));
			return;
		case 9:
			SendFrame(10, QByteArray::fromStdString(f.payload));
			break;
		default:
			break;
		}
	}
	if (decoder.Failed()) {
		Fail(QStringLiteral("bad data from server"));
	}
}

/* ------------------------------------------------------------------------ */
/* connector base                                                            */

SpazzChatConnector::SpazzChatConnector(int platform_, QNetworkAccessManager *net_, QObject *parent)
	: QObject(parent),
	  platform(platform_),
	  net(net_)
{
	retryTimer.setSingleShot(true);
	connect(&retryTimer, &QTimer::timeout, this, [this]() {
		if (running) {
			Start();
		}
	});
}

void SpazzChatConnector::Configure(const QString &t)
{
	target = t.trimmed();
}

void SpazzChatConnector::Stop()
{
	running = false;
	retryTimer.stop();
	Abort();
	attempt = 0;
	SetState(Off);
}

void SpazzChatConnector::SetState(State s, const QString &why)
{
	if (s == Live) {
		attempt = 0;
	}
	if (state == s && detail == why) {
		return;
	}
	state = s;
	detail = why;
	if (onStatus) {
		onStatus();
	}
}

void SpazzChatConnector::RetryLater(const QString &why, int fixedSec)
{
	if (!running) {
		return;
	}
	const int sec = fixedSec > 0 ? fixedSec : std::min(60, 2 << std::min(attempt, 5));
	attempt++;
	SetState(fixedSec > 0 ? Waiting : Failed, why);
	retryTimer.start(sec * 1000);
}

bool SpazzChatConnector::FirstTime(const std::string &id)
{
	const QString key = QString::fromStdString(id);
	if (key.isEmpty()) {
		return true;
	}
	if (seen.contains(key)) {
		return false;
	}
	seen.insert(key);
	seenOrder.push_back(key);
	while (seenOrder.size() > 3000) {
		seen.remove(seenOrder.front());
		seenOrder.pop_front();
	}
	return true;
}

void SpazzChatConnector::Emit(const Message &m)
{
	if (onMessage) {
		onMessage(m);
	}
}

static void BrowserHeaders(QNetworkRequest &req)
{
	req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromUtf8(kUserAgent));
	req.setRawHeader("Accept-Language", "en-US,en;q=0.9");
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
	req.setTransferTimeout(20000);
}

QNetworkReply *SpazzChatConnector::Get(const QUrl &url, bool browserLike)
{
	if (!net) {
		return nullptr;
	}
	QNetworkRequest req(url);
	BrowserHeaders(req);
	if (browserLike) {
		/* past the EU cookie wall on YouTube */
		req.setRawHeader("Cookie", "CONSENT=YES+cb; SOCS=CAI");
	}
	QNetworkReply *r = net->get(req);
	pending.append(r);
	return r;
}

QNetworkReply *SpazzChatConnector::Post(const QUrl &url, const QByteArray &body,
					const QList<QPair<QByteArray, QByteArray>> &hdrs)
{
	if (!net) {
		return nullptr;
	}
	QNetworkRequest req(url);
	BrowserHeaders(req);
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	req.setRawHeader("Cookie", "CONSENT=YES+cb; SOCS=CAI");
	for (const auto &h : hdrs) {
		req.setRawHeader(h.first, h.second);
	}
	QNetworkReply *r = net->post(req, body);
	pending.append(r);
	return r;
}

void SpazzChatConnector::Abort()
{
	const auto list = pending;
	pending.clear();
	for (const QPointer<QNetworkReply> &r : list) {
		if (r) {
			r->disconnect(this);
			r->abort();
			r->deleteLater();
		}
	}
}

/* a finished reply: takes it off the pending list; null if it failed */
static QByteArray TakeReply(QNetworkReply *r, QList<QPointer<QNetworkReply>> &pending, QString &error, int &status)
{
	pending.removeAll(r);
	r->deleteLater();
	status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (r->error() != QNetworkReply::NoError) {
		error = status ? QStringLiteral("HTTP %1").arg(status) : r->errorString();
		return QByteArray();
	}
	return r->readAll();
}

/* ------------------------------------------------------------------------ */
/* Twitch: anonymous IRC over TLS                                            */

SpazzTwitchChat::SpazzTwitchChat(QNetworkAccessManager *net_, QObject *parent)
	: SpazzChatConnector(Twitch, net_, parent)
{
	/* if the server goes quiet, ping it; it answers PONG */
	pingTimer.setInterval(240000);
	connect(&pingTimer, &QTimer::timeout, this, [this]() { SendLine("PING :spazzbs"); });
}

void SpazzTwitchChat::SendLine(const QByteArray &line)
{
	if (socket) {
		socket->write(line + "\r\n");
	}
}

void SpazzTwitchChat::Start()
{
	QString channel = target.toLower();
	channel.remove(QStringLiteral("https://"));
	channel.remove(QStringLiteral("www."));
	channel.remove(QStringLiteral("twitch.tv/"));
	channel.remove('#');
	channel = channel.section('/', 0, 0).trimmed();
	if (channel.isEmpty()) {
		SetState(Off);
		return;
	}
	running = true;
	if (socket) {
		socket->disconnect(this);
		socket->abort();
		socket->deleteLater();
	}
	lineBuf.clear();
	SetState(Connecting);
	socket = new QSslSocket(this);
	connect(socket, &QSslSocket::encrypted, this, [this, channel]() {
		SendLine("CAP REQ :twitch.tv/tags twitch.tv/commands");
		SendLine("PASS SCHMOOPIIE");
		SendLine("NICK justinfan" + QByteArray::number(10000 + QRandomGenerator::global()->bounded(80000)));
		SendLine("JOIN #" + channel.toUtf8());
		pingTimer.start();
	});
	connect(socket, &QSslSocket::readyRead, this, &SpazzTwitchChat::OnReadable);
	auto lost = [this](const QString &why) {
		pingTimer.stop();
		if (socket) {
			socket->disconnect(this);
			socket->deleteLater();
			socket = nullptr;
		}
		RetryLater(why);
	};
	connect(socket, &QSslSocket::disconnected, this, [lost]() { lost(QStringLiteral("disconnected")); });
	connect(socket, &QSslSocket::errorOccurred, this, [this, lost](QAbstractSocket::SocketError) {
		lost(socket ? socket->errorString() : QStringLiteral("connection error"));
	});
	socket->connectToHostEncrypted(QStringLiteral("irc.chat.twitch.tv"), 6697);
}

void SpazzTwitchChat::Stop()
{
	pingTimer.stop();
	if (socket) {
		socket->disconnect(this);
		socket->abort();
		socket->deleteLater();
		socket = nullptr;
	}
	SpazzChatConnector::Stop();
}

void SpazzTwitchChat::OnReadable()
{
	if (!socket) {
		return;
	}
	lineBuf += socket->readAll();
	int nl;
	while ((nl = lineBuf.indexOf('\n')) >= 0) {
		const std::string line = lineBuf.left(nl).toStdString();
		lineBuf.remove(0, nl + 1);
		const TwitchLine t = ParseTwitchLine(line);
		switch (t.type) {
		case TwitchLine::Ping:
			SendLine("PONG :" + QByteArray::fromStdString(t.pingPayload));
			break;
		case TwitchLine::Joined:
			SetState(Live);
			break;
		case TwitchLine::Chat:
		case TwitchLine::Notice:
			SetState(Live);
			Emit(t.msg);
			break;
		case TwitchLine::Error:
			RetryLater(QString::fromStdString(t.msg.text));
			return;
		default:
			break;
		}
		if (!socket) {
			return;
		}
	}
	if (lineBuf.size() > 1 << 20) {
		lineBuf.clear();
	}
}

/* ------------------------------------------------------------------------ */
/* Kick: chatroom id, then Pusher over a websocket                           */

static const char *kKickPusher =
	"wss://ws-us2.pusher.com/app/32cbd69e4b950bf97679?protocol=7&client=js&version=8.4.0&flash=false";

SpazzKickChat::SpazzKickChat(QNetworkAccessManager *net_, QObject *parent) : SpazzChatConnector(Kick, net_, parent)
{
	/* Pusher wants a sign of life within its 120 s activity timeout */
	pingTimer.setInterval(90000);
	connect(&pingTimer, &QTimer::timeout, this, [this]() {
		if (ws && ws->IsOpen()) {
			ws->SendText(QStringLiteral(R"({"event":"pusher:ping","data":{}})"));
		}
	});
}

void SpazzKickChat::Start()
{
	QString t = target;
	t.remove(QStringLiteral("https://"));
	t.remove(QStringLiteral("www."));
	t.remove(QStringLiteral("kick.com/"));
	t = t.section('/', 0, 0).trimmed();
	if (t.isEmpty()) {
		SetState(Off);
		return;
	}
	running = true;
	SetState(Connecting);

	/* a number is the chatroom id itself */
	bool numeric = false;
	t.toLongLong(&numeric);
	if (numeric) {
		chatroom = t;
		Connect();
		return;
	}
	if (!chatroom.isEmpty() && chatroomSlug == t.toLower()) {
		Connect(); /* already looked up */
		return;
	}

	QNetworkReply *r = Get(QUrl(QStringLiteral("https://kick.com/api/v2/channels/") + t.toLower()), false);
	if (!r) {
		return;
	}
	r->setProperty("slug", t);
	connect(r, &QNetworkReply::finished, this, [this, r]() {
		QString err;
		int status = 0;
		const QByteArray body = TakeReply(r, pending, err, status);
		if (!running) {
			return;
		}
		const std::string id = KickChatroomId(body.toStdString());
		if (id.empty()) {
			RetryLater(status == 403 || status == 429
					   ? QStringLiteral("Kick blocked the channel lookup. Put your "
							    "chatroom id in Chat settings.")
				   : status == 404 ? QStringLiteral("No Kick channel with that name")
						   : QStringLiteral("Couldn't look up the Kick channel (%1)")
							     .arg(err.isEmpty() ? QStringLiteral("no chatroom") : err));
			return;
		}
		chatroom = QString::fromStdString(id);
		chatroomSlug = r->property("slug").toString().toLower();
		Connect();
	});
}

void SpazzKickChat::Connect()
{
	if (!ws) {
		ws = new SpazzWebSocket(this);
		ws->onText = [this](const QString &text) {
			const KickEvent e = ParseKickFrame(text.toStdString());
			switch (e.type) {
			case KickEvent::Connected:
				ws->SendText(QString::fromStdString(KickSubscribe(chatroom.toStdString())));
				break;
			case KickEvent::Subscribed:
				SetState(Live);
				break;
			case KickEvent::Ping:
				ws->SendText(QString::fromStdString(KickPong()));
				break;
			case KickEvent::Chat:
				SetState(Live);
				if (FirstTime(e.msg.id)) {
					Emit(e.msg);
				}
				break;
			case KickEvent::Error:
				ws->Close();
				RetryLater(QString::fromStdString(e.error));
				break;
			default:
				break;
			}
		};
		ws->onClosed = [this](const QString &why) {
			pingTimer.stop();
			RetryLater(QStringLiteral("Kick chat: %1").arg(why));
		};
	}
	pingTimer.start();
	ws->Open(QUrl(QString::fromUtf8(kKickPusher)));
}

void SpazzKickChat::Stop()
{
	pingTimer.stop();
	if (ws) {
		ws->onClosed = nullptr;
		ws->Close();
		ws->deleteLater();
		ws = nullptr;
	}
	chatroom.clear();
	chatroomSlug.clear();
	SpazzChatConnector::Stop();
}

/* ------------------------------------------------------------------------ */
/* Rumble: the streamer's Live Stream API, polled                            */

SpazzRumbleChat::SpazzRumbleChat(QNetworkAccessManager *net_, QObject *parent)
	: SpazzChatConnector(Rumble, net_, parent)
{
	pollTimer.setSingleShot(true);
	connect(&pollTimer, &QTimer::timeout, this, &SpazzRumbleChat::Poll);
}

void SpazzRumbleChat::Start()
{
	if (!target.startsWith(QStringLiteral("http"))) {
		SetState(target.isEmpty() ? Off : Failed,
			 target.isEmpty() ? QString()
					  : QStringLiteral("Paste your Live Stream API link (Rumble → Account → "
							   "Live Stream API)"));
		return;
	}
	running = true;
	SetState(Connecting);
	Poll();
}

void SpazzRumbleChat::Stop()
{
	pollTimer.stop();
	firstPoll = true;
	SpazzChatConnector::Stop();
}

void SpazzRumbleChat::Poll()
{
	QNetworkReply *r = Get(QUrl(target), false);
	if (!r) {
		return;
	}
	connect(r, &QNetworkReply::finished, this, [this, r]() {
		QString err;
		int status = 0;
		const QByteArray body = TakeReply(r, pending, err, status);
		if (!running) {
			return;
		}
		if (body.isEmpty()) {
			RetryLater(
				QStringLiteral("Rumble API: %1").arg(err.isEmpty() ? QStringLiteral("no answer") : err));
			return;
		}
		const RumbleResult res = ParseRumble(body.toStdString());
		if (!res.ok) {
			RetryLater(QString::fromStdString(res.error));
			return;
		}
		if (!res.live) {
			SetState(Waiting, QStringLiteral("Not live on Rumble yet"));
		} else {
			SetState(Live);
		}
		for (const Message &m : res.messages) {
			if (FirstTime(m.id)) {
				Emit(m);
			}
		}
		firstPoll = false;
		/* Rumble asks API users to keep it gentle */
		pollTimer.start(res.live ? 5000 : 20000);
	});
}

/* ------------------------------------------------------------------------ */
/* YouTube: live video, then the innertube live chat                         */

SpazzYouTubeChat::SpazzYouTubeChat(QNetworkAccessManager *net_, QObject *parent)
	: SpazzChatConnector(YouTube, net_, parent)
{
	pollTimer.setSingleShot(true);
	connect(&pollTimer, &QTimer::timeout, this, &SpazzYouTubeChat::Poll);
}

void SpazzYouTubeChat::Start()
{
	if (target.isEmpty()) {
		SetState(Off);
		return;
	}
	running = true;
	SetState(Connecting);
	FindVideo();
}

void SpazzYouTubeChat::Stop()
{
	pollTimer.stop();
	videoId.clear();
	cfg = YouTubeConfig();
	SpazzChatConnector::Stop();
}

void SpazzYouTubeChat::FindVideo()
{
	const std::string direct = YouTubeIdFromInput(target.toStdString());
	if (!direct.empty()) {
		videoId = QString::fromStdString(direct);
		OpenChat();
		return;
	}
	const QString page = QString::fromStdString(YouTubeLivePageUrl(target.toStdString()));
	QNetworkReply *r = Get(QUrl(page));
	if (!r) {
		return;
	}
	connect(r, &QNetworkReply::finished, this, [this, r]() {
		QString err;
		int status = 0;
		const QByteArray body = TakeReply(r, pending, err, status);
		if (!running) {
			return;
		}
		if (body.isEmpty()) {
			RetryLater(status == 404 ? QStringLiteral("No YouTube channel with that name")
						 : QStringLiteral("Couldn't reach YouTube (%1)").arg(err));
			return;
		}
		const std::string id = YouTubeIdFromLivePage(body.toStdString());
		if (id.empty()) {
			RetryLater(QStringLiteral("Not live on YouTube yet — checking every 30 s"), 30);
			return;
		}
		videoId = QString::fromStdString(id);
		OpenChat();
	});
}

void SpazzYouTubeChat::OpenChat()
{
	QNetworkReply *r = Get(QUrl(QStringLiteral("https://www.youtube.com/live_chat?is_popout=1&v=") + videoId));
	if (!r) {
		return;
	}
	connect(r, &QNetworkReply::finished, this, [this, r]() {
		QString err;
		int status = 0;
		const QByteArray body = TakeReply(r, pending, err, status);
		if (!running) {
			return;
		}
		cfg = ParseYouTubeChatPage(body.toStdString());
		if (cfg.continuation.empty()) {
			RetryLater(body.isEmpty() ? QStringLiteral("Couldn't open the YouTube chat (%1)").arg(err)
						  : QStringLiteral("This video has no live chat (yet) — checking "
								   "every 30 s"),
				   30);
			return;
		}
		SetState(Live);
		Poll();
	});
}

void SpazzYouTubeChat::Poll()
{
	QString url = QStringLiteral("https://www.youtube.com/youtubei/v1/live_chat/get_live_chat?prettyPrint=false");
	if (!cfg.apiKey.empty()) {
		url += QStringLiteral("&key=") + QString::fromStdString(cfg.apiKey);
	}
	QNetworkReply *r = Post(QUrl(url), QByteArray::fromStdString(YouTubeChatRequestBody(cfg)),
				{{"X-Youtube-Client-Name", "1"},
				 {"X-Youtube-Client-Version", QByteArray::fromStdString(cfg.clientVersion)},
				 {"Origin", "https://www.youtube.com"}});
	if (!r) {
		return;
	}
	connect(r, &QNetworkReply::finished, this, [this, r]() {
		QString err;
		int status = 0;
		const QByteArray body = TakeReply(r, pending, err, status);
		if (!running) {
			return;
		}
		if (body.isEmpty()) {
			/* a hiccup: try the same continuation again, then start over */
			if (++attempt > 3) {
				cfg = YouTubeConfig();
				RetryLater(QStringLiteral("Lost the YouTube chat (%1)").arg(err));
			} else {
				pollTimer.start(3000);
			}
			return;
		}
		const YouTubeChat chat = ParseYouTubeChat(body.toStdString());
		if (!chat.ok || chat.ended) {
			cfg = YouTubeConfig();
			RetryLater(QStringLiteral("The YouTube stream ended — watching for the next one"), 30);
			return;
		}
		attempt = 0;
		SetState(Live);
		cfg.continuation = chat.continuation;
		for (const Message &m : chat.messages) {
			if (FirstTime(m.id)) {
				Emit(m);
			}
		}
		pollTimer.start(std::clamp(chat.timeoutMs, 1000, 10000));
	});
}
