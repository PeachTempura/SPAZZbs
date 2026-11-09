/******************************************************************************
    SPAZZbs - unified chat: message model and per-platform parsers.
    Plain C++ (json11 only) so the parsers can be tested outside the app.
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Protocol references (no code copied; all written for SPAZZbs):
    - YouTube innertube live chat: the approach used by chat-downloader
      (xenova, MIT) and pytchat (MIT).
    - Kick Pusher chat: kick-api crate (MIT OR Apache-2.0).
    - Rumble Live Stream API: rumble-livestream-lib-go (MIT) and
      cocorum (Apache-2.0).
    - Twitch: the public IRC / tags documentation.
******************************************************************************/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace spazzchat {

enum Platform { YouTube = 0, Kick, Twitch, Rumble, X, PlatformCount };

enum Kind {
	KindChat = 0,
	KindPaid,   /* super chat / rant / bits */
	KindMember, /* new member / sub / gifted */
	KindSystem, /* our own notes: connected, offline... */
};

struct Badge {
	std::string type; /* moderator, owner, member, subscriber, vip, verified, broadcaster... */
	std::string label;
};

struct Message {
	int platform = YouTube;
	int kind = KindChat;
	std::string id;
	std::string user;
	std::string color; /* "#rrggbb" or empty */
	std::string text;
	std::string amount; /* "$5.00" for paid messages */
	std::vector<Badge> badges;
	int64_t timeMs = 0;
};

/* ---- Twitch IRC ---- */
struct TwitchLine {
	enum Type { None, Ping, Chat, Notice, Joined, Error } type = None;
	std::string pingPayload;
	Message msg;
};
std::string TwitchUnescapeTag(const std::string &v);
TwitchLine ParseTwitchLine(const std::string &line);

/* ---- Kick (Pusher protocol over a websocket) ---- */
struct KickEvent {
	enum Type { Other, Connected, Subscribed, Ping, Chat, Error } type = Other;
	std::string error;
	Message msg;
};
KickEvent ParseKickFrame(const std::string &frame);
std::string KickSubscribe(const std::string &chatroomId);
std::string KickPong();
/* "hi [emote:37226:KEKW]" -> "hi KEKW" */
std::string KickStripEmotes(const std::string &content);
/* reads chatroom.id from kick.com/api/v2/channels/{slug} */
std::string KickChatroomId(const std::string &channelJson);

/* ---- Rumble Live Stream API ---- */
struct RumbleResult {
	bool ok = false;
	bool live = false;
	std::string error;
	std::vector<Message> messages; /* oldest first */
};
RumbleResult ParseRumble(const std::string &json);

/* ---- YouTube (innertube live chat) ---- */
/* a watch URL, youtu.be link, /live/ID link or a bare 11-char id */
std::string YouTubeIdFromInput(const std::string &input);
/* the URL to fetch to find the current live video: @handle, channel id,
 * channel URL. Empty when the input already is a video. */
std::string YouTubeLivePageUrl(const std::string &input);
/* the live video id from a /live page (canonical watch link), or "" */
std::string YouTubeIdFromLivePage(const std::string &html);

struct YouTubeConfig {
	std::string apiKey;
	std::string clientVersion;
	std::string continuation;
};
YouTubeConfig ParseYouTubeChatPage(const std::string &html);

struct YouTubeChat {
	bool ok = false;
	bool ended = false;
	std::string continuation;
	int timeoutMs = 2000;
	std::vector<Message> messages;
};
YouTubeChat ParseYouTubeChat(const std::string &json);
std::string YouTubeChatRequestBody(const YouTubeConfig &cfg);

/* ---- RFC 6455 websocket framing ---- */
struct WsFrame {
	int opcode = 0; /* 1 text, 2 binary, 8 close, 9 ping, 10 pong */
	std::string payload;
};

class WsDecoder {
public:
	/* appends bytes; returns complete (defragmented) frames */
	std::vector<WsFrame> Feed(const std::string &bytes);
	bool Failed() const { return failed; }

private:
	std::string buf;
	std::string fragment;
	int fragmentOpcode = 0;
	bool failed = false;
};

/* a masked client frame (FIN set) */
std::string WsEncode(int opcode, const std::string &payload, const unsigned char mask[4]);

} // namespace spazzchat
