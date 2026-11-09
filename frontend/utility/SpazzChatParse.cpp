/******************************************************************************
    SPAZZbs - unified chat: per-platform parsers (see SpazzChatParse.hpp).
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "SpazzChatParse.hpp"

#include <json11.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

using json11::Json;

namespace spazzchat {

/* ------------------------------------------------------------------------ */
/* helpers                                                                   */

static bool StartsWith(const std::string &s, const char *prefix)
{
	return s.compare(0, strlen(prefix), prefix) == 0;
}

static std::string Lower(std::string s)
{
	for (char &c : s) {
		c = char(tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

static std::string Trim(const std::string &s)
{
	size_t a = 0, b = s.size();
	while (a < b && isspace(static_cast<unsigned char>(s[a]))) {
		a++;
	}
	while (b > a && isspace(static_cast<unsigned char>(s[b - 1]))) {
		b--;
	}
	return s.substr(a, b - a);
}

/* days since 1970-01-01 for a civil date (Howard Hinnant's algorithm) */
static int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d)
{
	y -= m <= 2;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = unsigned(y - era * 400);
	const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + int64_t(doe) - 719468;
}

/* "2026-11-09T01:23:45+00:00", "...Z", "....123456Z" -> ms since epoch (UTC);
 * 0 when it can't be read */
static int64_t ParseIsoTime(const std::string &s)
{
	int Y, M, D, h, m, sec;
	if (s.size() < 19 || sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) {
		return 0;
	}
	int64_t ms = (DaysFromCivil(Y, unsigned(M), unsigned(D)) * 86400 + h * 3600 + m * 60 + sec) * 1000;
	size_t p = 19;
	if (p < s.size() && s[p] == '.') {
		int64_t frac = 0;
		int digits = 0;
		for (p++; p < s.size() && isdigit(static_cast<unsigned char>(s[p])); p++) {
			if (digits < 3) {
				frac = frac * 10 + (s[p] - '0');
				digits++;
			}
		}
		while (digits < 3) {
			frac *= 10;
			digits++;
		}
		ms += frac;
	}
	if (p < s.size() && (s[p] == '+' || s[p] == '-')) {
		int oh = 0, om = 0;
		if (sscanf(s.c_str() + p + 1, "%2d:%2d", &oh, &om) >= 1) {
			const int64_t off = (oh * 60 + om) * 60000;
			ms += s[p] == '+' ? -off : off;
		}
	}
	return ms;
}

/* ------------------------------------------------------------------------ */
/* Twitch                                                                    */

std::string TwitchUnescapeTag(const std::string &v)
{
	std::string out;
	out.reserve(v.size());
	for (size_t i = 0; i < v.size(); i++) {
		if (v[i] != '\\' || i + 1 >= v.size()) {
			out += v[i];
			continue;
		}
		const char n = v[++i];
		switch (n) {
		case 's':
			out += ' ';
			break;
		case ':':
			out += ';';
			break;
		case '\\':
			out += '\\';
			break;
		case 'r':
			out += '\r';
			break;
		case 'n':
			out += '\n';
			break;
		default:
			out += n;
		}
	}
	return out;
}

static void TwitchBadges(const std::string &badges, Message &msg)
{
	size_t start = 0;
	while (start < badges.size()) {
		size_t end = badges.find(',', start);
		if (end == std::string::npos) {
			end = badges.size();
		}
		std::string b = badges.substr(start, end - start);
		const std::string type = b.substr(0, b.find('/'));
		if (!type.empty()) {
			std::string label = type;
			if (type == "broadcaster") {
				label = "Streamer";
			} else if (type == "moderator") {
				label = "Mod";
			} else if (type == "subscriber" || type == "founder") {
				label = "Sub";
			} else if (type == "vip") {
				label = "VIP";
			} else if (type == "staff" || type == "admin") {
				label = "Staff";
			} else {
				label.clear(); /* game / event badges: not worth the space */
			}
			if (!label.empty()) {
				msg.badges.push_back({type == "founder" ? "subscriber" : type, label});
			}
		}
		start = end + 1;
	}
}

TwitchLine ParseTwitchLine(const std::string &rawLine)
{
	TwitchLine out;
	std::string line = rawLine;
	while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
		line.pop_back();
	}
	if (line.empty()) {
		return out;
	}

	/* tags */
	std::vector<std::pair<std::string, std::string>> tags;
	size_t pos = 0;
	if (line[0] == '@') {
		const size_t sp = line.find(' ');
		if (sp == std::string::npos) {
			return out;
		}
		const std::string tagStr = line.substr(1, sp - 1);
		size_t t = 0;
		while (t < tagStr.size()) {
			size_t e = tagStr.find(';', t);
			if (e == std::string::npos) {
				e = tagStr.size();
			}
			const std::string kv = tagStr.substr(t, e - t);
			const size_t eq = kv.find('=');
			if (eq == std::string::npos) {
				tags.emplace_back(kv, "");
			} else {
				tags.emplace_back(kv.substr(0, eq), TwitchUnescapeTag(kv.substr(eq + 1)));
			}
			t = e + 1;
		}
		pos = sp + 1;
	}
	auto tag = [&](const char *k) -> std::string {
		for (auto &kv : tags) {
			if (kv.first == k) {
				return kv.second;
			}
		}
		return std::string();
	};

	/* prefix */
	std::string prefix;
	if (pos < line.size() && line[pos] == ':') {
		const size_t sp = line.find(' ', pos);
		if (sp == std::string::npos) {
			return out;
		}
		prefix = line.substr(pos + 1, sp - pos - 1);
		pos = sp + 1;
	}

	/* command and params */
	size_t sp = line.find(' ', pos);
	const std::string command = line.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
	std::string trailing;
	if (sp != std::string::npos) {
		const size_t colon = line.find(" :", sp);
		if (colon != std::string::npos) {
			trailing = line.substr(colon + 2);
		} else if (line.compare(sp + 1, 1, ":") == 0) {
			trailing = line.substr(sp + 2);
		}
	}

	if (command == "PING") {
		out.type = TwitchLine::Ping;
		out.pingPayload = trailing.empty() ? "tmi.twitch.tv" : trailing;
		return out;
	}
	if (command == "366" || command == "ROOMSTATE") {
		out.type = TwitchLine::Joined;
		return out;
	}
	if (command == "NOTICE" && (trailing.find("Login") != std::string::npos ||
				    trailing.find("auth") != std::string::npos)) {
		out.type = TwitchLine::Error;
		out.msg.text = trailing;
		return out;
	}
	if (command != "PRIVMSG" && command != "USERNOTICE") {
		return out;
	}

	Message &m = out.msg;
	m.platform = Twitch;
	m.id = tag("id");
	m.user = tag("display-name");
	if (m.user.empty()) {
		m.user = prefix.substr(0, prefix.find('!'));
	}
	m.color = tag("color");
	m.timeMs = atoll(tag("tmi-sent-ts").c_str());
	TwitchBadges(tag("badges"), m);

	/* /me: \x01ACTION text\x01 */
	if (StartsWith(trailing, "\x01" "ACTION ")) {
		trailing = trailing.substr(8);
		if (!trailing.empty() && trailing.back() == '\x01') {
			trailing.pop_back();
		}
	}

	if (command == "USERNOTICE") {
		const std::string sys = tag("system-msg");
		m.kind = KindMember;
		m.text = trailing.empty() ? sys : sys + " — " + trailing;
		out.type = TwitchLine::Notice;
		return out;
	}
	const std::string bits = tag("bits");
	if (!bits.empty()) {
		m.kind = KindPaid;
		m.amount = bits + " bits";
	}
	m.text = trailing;
	out.type = TwitchLine::Chat;
	return out;
}

/* ------------------------------------------------------------------------ */
/* Kick                                                                      */

std::string KickStripEmotes(const std::string &content)
{
	std::string out;
	size_t i = 0;
	while (i < content.size()) {
		if (content.compare(i, 7, "[emote:") == 0) {
			const size_t close = content.find(']', i);
			if (close != std::string::npos) {
				const std::string inner = content.substr(i + 7, close - i - 7); /* id:name */
				const size_t colon = inner.find(':');
				out += colon == std::string::npos ? inner : inner.substr(colon + 1);
				i = close + 1;
				continue;
			}
		}
		out += content[i++];
	}
	return out;
}

std::string KickSubscribe(const std::string &chatroomId)
{
	return Json(Json::object{{"event", "pusher:subscribe"},
				 {"data", Json::object{{"auth", ""}, {"channel", "chatrooms." + chatroomId + ".v2"}}}})
		.dump();
}

std::string KickPong()
{
	return R"({"event":"pusher:pong","data":{}})";
}

std::string KickChatroomId(const std::string &channelJson)
{
	std::string err;
	const Json j = Json::parse(channelJson, err);
	if (!err.empty()) {
		return std::string();
	}
	const Json &id = j["chatroom"]["id"];
	if (id.is_number()) {
		return std::to_string(int64_t(id.number_value()));
	}
	return id.string_value();
}

/* Pusher wraps the event's data as a JSON string inside the JSON */
static Json PusherData(const Json &frame)
{
	const Json &d = frame["data"];
	if (d.is_string()) {
		std::string err;
		Json inner = Json::parse(d.string_value(), err);
		return err.empty() ? inner : Json();
	}
	return d;
}

KickEvent ParseKickFrame(const std::string &frame)
{
	KickEvent out;
	std::string err;
	const Json j = Json::parse(frame, err);
	if (!err.empty()) {
		return out;
	}
	const std::string event = j["event"].string_value();
	if (event == "pusher:connection_established") {
		out.type = KickEvent::Connected;
		return out;
	}
	if (event == "pusher_internal:subscription_succeeded") {
		out.type = KickEvent::Subscribed;
		return out;
	}
	if (event == "pusher:ping") {
		out.type = KickEvent::Ping;
		return out;
	}
	if (event == "pusher:error") {
		out.type = KickEvent::Error;
		const Json d = PusherData(j);
		out.error = d["message"].string_value();
		if (out.error.empty()) {
			out.error = "Kick chat error";
		}
		return out;
	}

	const bool chat = event == "App\\Events\\ChatMessageEvent" || event == "App\\Events\\ChatMessageSentEvent";
	const bool sub = event == "App\\Events\\SubscriptionEvent" || event == "App\\Events\\GiftedSubscriptionsEvent" ||
			 event == "App\\Events\\LuckyUsersWhoGotGiftSubscriptionsEvent";
	if (!chat && !sub) {
		return out;
	}
	const Json d = PusherData(j);
	Message &m = out.msg;
	m.platform = Kick;

	if (sub) {
		m.kind = KindMember;
		m.user = d["username"].string_value();
		if (m.user.empty()) {
			m.user = d["gifter_username"].string_value();
		}
		if (event == "App\\Events\\GiftedSubscriptionsEvent") {
			const size_t n = d["gifted_usernames"].array_items().size();
			m.text = "gifted " + std::to_string(n) + (n == 1 ? " sub" : " subs");
		} else {
			const int months = int(d["months"].number_value());
			m.text = months > 1 ? "subscribed for " + std::to_string(months) + " months" : "subscribed";
		}
		m.id = event + ":" + m.user + ":" + std::to_string(int64_t(d["chatroom_id"].number_value()));
		out.type = KickEvent::Chat;
		return out;
	}

	const Json &msgObj = d["message"].is_object() ? d["message"] : d; /* older ChatMessageSentEvent nests */
	const Json &sender = d["sender"].is_object() ? d["sender"] : d["user"];
	m.id = msgObj["id"].is_string() ? msgObj["id"].string_value()
					 : std::to_string(int64_t(msgObj["id"].number_value()));
	m.user = sender["username"].string_value();
	m.color = sender["identity"]["color"].string_value();
	m.text = KickStripEmotes(msgObj["content"].is_string() ? msgObj["content"].string_value()
							       : msgObj["message"].string_value());
	m.timeMs = ParseIsoTime(msgObj["created_at"].string_value());
	for (const Json &b : sender["identity"]["badges"].array_items()) {
		const std::string type = b["type"].string_value();
		std::string label = b["text"].string_value();
		if (type == "broadcaster") {
			label = "Streamer";
		} else if (type == "moderator") {
			label = "Mod";
		} else if (type == "subscriber" || type == "founder" || type == "sub_gifter") {
			label = type == "sub_gifter" ? "Gifter" : "Sub";
		} else if (type == "vip" || type == "og") {
			label = type == "vip" ? "VIP" : "OG";
		} else if (type == "verified") {
			label = "Verified";
		}
		if (!type.empty()) {
			m.badges.push_back({type, label});
		}
	}
	out.type = KickEvent::Chat;
	return out;
}

/* ------------------------------------------------------------------------ */
/* Rumble                                                                    */

static void RumbleMessage(const Json &jm, bool rant, std::vector<Message> &out)
{
	Message m;
	m.platform = Rumble;
	m.user = jm["username"].string_value();
	m.text = jm["text"].string_value();
	m.timeMs = ParseIsoTime(jm["created_on"].string_value());
	const std::string created = jm["created_on"].string_value();
	m.id = created + "|" + m.user + "|" + m.text;
	if (rant) {
		m.kind = KindPaid;
		const double dollars = jm["amount_dollars"].is_number() ? jm["amount_dollars"].number_value()
									 : jm["amount_cents"].number_value() / 100.0;
		char buf[32];
		snprintf(buf, sizeof(buf), "$%.2f", dollars);
		m.amount = buf;
		m.id = "rant|" + m.id;
	}
	/* badges: a list of strings, or an object of name -> info */
	auto addBadge = [&m](const std::string &b) {
		if (b == "admin" || b == "moderator") {
			m.badges.push_back({"moderator", "Mod"});
		} else if (b == "recurring_subscription" || b == "locals_supporter" || b == "locals") {
			m.badges.push_back({"subscriber", "Sub"});
		} else if (b == "premium") {
			m.badges.push_back({"member", "Premium"});
		} else if (b == "verified") {
			m.badges.push_back({"verified", "Verified"});
		}
	};
	if (jm["badges"].is_array()) {
		for (const Json &b : jm["badges"].array_items()) {
			addBadge(b.string_value());
		}
	} else if (jm["badges"].is_object()) {
		for (const auto &kv : jm["badges"].object_items()) {
			addBadge(kv.first);
		}
	}
	out.push_back(std::move(m));
}

RumbleResult ParseRumble(const std::string &json)
{
	RumbleResult out;
	std::string err;
	const Json j = Json::parse(json, err);
	if (!err.empty() || !j.is_object()) {
		out.error = "Rumble sent something that isn't the Live Stream API";
		return out;
	}
	if (!j["error"].is_null() || !j["errors"].is_null()) {
		out.error = j["error"].is_string() ? j["error"].string_value() : "Rumble rejected the API key";
		return out;
	}
	out.ok = true;
	for (const Json &ls : j["livestreams"].array_items()) {
		if (!ls["is_live"].bool_value()) {
			continue;
		}
		out.live = true;
		for (const Json &jm : ls["chat"]["recent_messages"].array_items()) {
			RumbleMessage(jm, false, out.messages);
		}
		for (const Json &jm : ls["chat"]["recent_rants"].array_items()) {
			RumbleMessage(jm, true, out.messages);
		}
	}
	std::stable_sort(out.messages.begin(), out.messages.end(),
			 [](const Message &a, const Message &b) { return a.timeMs < b.timeMs; });
	return out;
}

/* ------------------------------------------------------------------------ */
/* YouTube                                                                   */

static bool IsVideoIdChar(char c)
{
	return isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
}

static std::string IdAt(const std::string &s, size_t p)
{
	std::string id;
	while (p < s.size() && IsVideoIdChar(s[p]) && id.size() < 11) {
		id += s[p++];
	}
	return id.size() == 11 ? id : std::string();
}

std::string YouTubeIdFromInput(const std::string &raw)
{
	const std::string in = Trim(raw);
	if (in.size() == 11 && std::all_of(in.begin(), in.end(), IsVideoIdChar)) {
		return in;
	}
	size_t p;
	if ((p = in.find("v=")) != std::string::npos) {
		return IdAt(in, p + 2);
	}
	if ((p = in.find("youtu.be/")) != std::string::npos) {
		return IdAt(in, p + 9);
	}
	if ((p = in.find("/live/")) != std::string::npos) {
		return IdAt(in, p + 6);
	}
	if ((p = in.find("/shorts/")) != std::string::npos) {
		return IdAt(in, p + 8);
	}
	return std::string();
}

std::string YouTubeLivePageUrl(const std::string &raw)
{
	std::string in = Trim(raw);
	if (in.empty() || !YouTubeIdFromInput(in).empty()) {
		return std::string();
	}
	while (!in.empty() && in.back() == '/') {
		in.pop_back();
	}
	if (in.find("youtube.com/") != std::string::npos) {
		const std::string lower = Lower(in);
		if (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, "/live") == 0) {
			return in;
		}
		return in + "/live";
	}
	if (in[0] == '@') {
		return "https://www.youtube.com/" + in + "/live";
	}
	if (StartsWith(in, "UC") && in.size() == 24) {
		return "https://www.youtube.com/channel/" + in + "/live";
	}
	return "https://www.youtube.com/@" + in + "/live";
}

std::string YouTubeIdFromLivePage(const std::string &html)
{
	/* when the channel is live, /live is the watch page */
	static const char *marks[] = {"<link rel=\"canonical\" href=\"https://www.youtube.com/watch?v=",
				      "\"canonicalBaseUrl\":\"/watch?v="};
	for (const char *mark : marks) {
		const size_t p = html.find(mark);
		if (p != std::string::npos) {
			std::string id = IdAt(html, p + strlen(mark));
			/* only if it's actually live right now */
			if (!id.empty() && (html.find("\"isLiveNow\":true") != std::string::npos ||
					    html.find("\"isLive\":true") != std::string::npos)) {
				return id;
			}
		}
	}
	return std::string();
}

static std::string JsonStringAfter(const std::string &html, const std::string &key, size_t from = 0)
{
	const std::string mark = "\"" + key + "\":\"";
	const size_t p = html.find(mark, from);
	if (p == std::string::npos) {
		return std::string();
	}
	const size_t s = p + mark.size();
	std::string out;
	for (size_t i = s; i < html.size(); i++) {
		if (html[i] == '\\' && i + 1 < html.size()) {
			/* continuations are base64url-ish; %3D etc. stay as they are */
			if (html[i + 1] == 'u' && i + 5 < html.size()) {
				const long cp = strtol(html.substr(i + 2, 4).c_str(), nullptr, 16);
				if (cp < 0x80) {
					out += char(cp);
				}
				i += 5;
				continue;
			}
			out += html[++i];
			continue;
		}
		if (html[i] == '"') {
			return out;
		}
		out += html[i];
	}
	return std::string();
}

YouTubeConfig ParseYouTubeChatPage(const std::string &html)
{
	YouTubeConfig cfg;
	cfg.apiKey = JsonStringAfter(html, "INNERTUBE_API_KEY");
	cfg.clientVersion = JsonStringAfter(html, "INNERTUBE_CLIENT_VERSION");
	if (cfg.clientVersion.empty()) {
		cfg.clientVersion = JsonStringAfter(html, "clientVersion");
	}
	/* the first continuation after the live chat renderer: live (not replay) */
	size_t from = html.find("\"liveChatRenderer\"");
	if (from == std::string::npos) {
		from = 0;
	}
	for (const char *kind : {"invalidationContinuationData", "timedContinuationData", "reloadContinuationData"}) {
		const size_t p = html.find(std::string("\"") + kind + "\"", from);
		if (p != std::string::npos) {
			cfg.continuation = JsonStringAfter(html, "continuation", p);
			if (!cfg.continuation.empty()) {
				break;
			}
		}
	}
	return cfg;
}

std::string YouTubeChatRequestBody(const YouTubeConfig &cfg)
{
	return Json(Json::object{
			    {"context",
			     Json::object{{"client", Json::object{{"clientName", "WEB"},
								  {"clientVersion", cfg.clientVersion.empty()
											    ? "2.20250101.00.00"
											    : cfg.clientVersion},
								  {"hl", "en"}}}}},
			    {"continuation", cfg.continuation},
		    })
		.dump();
}

static std::string YtText(const Json &t)
{
	if (t["simpleText"].is_string()) {
		return t["simpleText"].string_value();
	}
	std::string out;
	for (const Json &run : t["runs"].array_items()) {
		if (run["text"].is_string()) {
			out += run["text"].string_value();
		} else if (run["emoji"].is_object()) {
			const Json &e = run["emoji"];
			if (e["isCustomEmoji"].bool_value()) {
				const auto &sc = e["shortcuts"].array_items();
				out += sc.empty() ? std::string(":emoji:") : sc[0].string_value();
			} else {
				/* a standard emoji: its id is the character itself */
				const std::string id = e["emojiId"].string_value();
				if (!id.empty() && static_cast<unsigned char>(id[0]) >= 0x80) {
					out += id;
				} else {
					const auto &sc = e["shortcuts"].array_items();
					out += sc.empty() ? id : sc[0].string_value();
				}
			}
		}
	}
	return out;
}

static void YtBadges(const Json &r, Message &m)
{
	for (const Json &b : r["authorBadges"].array_items()) {
		const Json &br = b["liveChatAuthorBadgeRenderer"];
		const std::string icon = br["icon"]["iconType"].string_value();
		const std::string tip = br["tooltip"].string_value();
		if (icon == "OWNER") {
			m.badges.push_back({"owner", "Owner"});
		} else if (icon == "MODERATOR") {
			m.badges.push_back({"moderator", "Mod"});
		} else if (icon == "VERIFIED") {
			m.badges.push_back({"verified", "Verified"});
		} else if (br["customThumbnail"].is_object() || tip.find("Member") != std::string::npos) {
			m.badges.push_back({"member", "Member"});
		}
	}
}

static bool YtItem(const Json &item, Message &m)
{
	const char *types[] = {"liveChatTextMessageRenderer", "liveChatPaidMessageRenderer",
			       "liveChatMembershipItemRenderer", "liveChatPaidStickerRenderer",
			       "liveChatSponsorshipsGiftPurchaseAnnouncementRenderer"};
	for (const char *type : types) {
		const Json &r = item[type];
		if (!r.is_object()) {
			continue;
		}
		m.platform = YouTube;
		m.id = r["id"].string_value();
		m.user = YtText(r["authorName"]);
		m.timeMs = atoll(r["timestampUsec"].string_value().c_str()) / 1000;
		YtBadges(r, m);
		const std::string t = type;
		if (t == "liveChatTextMessageRenderer") {
			m.text = YtText(r["message"]);
		} else if (t == "liveChatPaidMessageRenderer" || t == "liveChatPaidStickerRenderer") {
			m.kind = KindPaid;
			m.amount = YtText(r["purchaseAmountText"]);
			m.text = YtText(r["message"]);
			if (m.text.empty() && t == "liveChatPaidStickerRenderer") {
				m.text = "sent a Super Sticker";
			}
		} else if (t == "liveChatMembershipItemRenderer") {
			m.kind = KindMember;
			m.text = YtText(r["headerSubtext"]);
			const std::string extra = YtText(r["message"]);
			if (!extra.empty()) {
				m.text += m.text.empty() ? extra : " — " + extra;
			}
			if (m.text.empty()) {
				m.text = "became a member";
			}
		} else {
			const Json &h = r["header"]["liveChatSponsorshipsHeaderRenderer"];
			m.kind = KindMember;
			m.user = YtText(h["authorName"]);
			m.text = YtText(h["primaryText"]);
		}
		return !m.user.empty() || !m.text.empty();
	}
	return false;
}

YouTubeChat ParseYouTubeChat(const std::string &json)
{
	YouTubeChat out;
	std::string err;
	const Json j = Json::parse(json, err);
	if (!err.empty()) {
		return out;
	}
	const Json &lc = j["continuationContents"]["liveChatContinuation"];
	if (!lc.is_object()) {
		/* no continuation: the stream ended or the chat was turned off */
		out.ok = true;
		out.ended = true;
		return out;
	}
	out.ok = true;
	for (const Json &c : lc["continuations"].array_items()) {
		for (const char *kind : {"invalidationContinuationData", "timedContinuationData",
					 "reloadContinuationData"}) {
			const Json &cd = c[kind];
			if (cd.is_object() && cd["continuation"].is_string()) {
				out.continuation = cd["continuation"].string_value();
				if (cd["timeoutMs"].is_number()) {
					out.timeoutMs = int(cd["timeoutMs"].number_value());
				}
				break;
			}
		}
		if (!out.continuation.empty()) {
			break;
		}
	}
	if (out.continuation.empty()) {
		out.ended = true;
	}
	for (const Json &a : lc["actions"].array_items()) {
		const Json &item = a["addChatItemAction"]["item"];
		if (!item.is_object()) {
			continue;
		}
		Message m;
		if (YtItem(item, m)) {
			out.messages.push_back(std::move(m));
		}
	}
	return out;
}

/* ------------------------------------------------------------------------ */
/* websocket framing                                                         */

std::vector<WsFrame> WsDecoder::Feed(const std::string &bytes)
{
	std::vector<WsFrame> frames;
	if (failed) {
		return frames;
	}
	buf += bytes;
	for (;;) {
		if (buf.size() < 2) {
			break;
		}
		const unsigned char b0 = static_cast<unsigned char>(buf[0]);
		const unsigned char b1 = static_cast<unsigned char>(buf[1]);
		const bool fin = (b0 & 0x80) != 0;
		const int opcode = b0 & 0x0f;
		const bool masked = (b1 & 0x80) != 0;
		uint64_t len = b1 & 0x7f;
		size_t head = 2;
		if (len == 126) {
			if (buf.size() < 4) {
				break;
			}
			len = (uint64_t(static_cast<unsigned char>(buf[2])) << 8) | static_cast<unsigned char>(buf[3]);
			head = 4;
		} else if (len == 127) {
			if (buf.size() < 10) {
				break;
			}
			len = 0;
			for (int i = 0; i < 8; i++) {
				len = (len << 8) | static_cast<unsigned char>(buf[2 + i]);
			}
			head = 10;
		}
		if (len > 16u * 1024u * 1024u) {
			failed = true; /* nobody sends chat in 16 MB frames */
			buf.clear();
			break;
		}
		const size_t maskLen = masked ? 4 : 0;
		if (buf.size() < head + maskLen + len) {
			break;
		}
		std::string payload = buf.substr(head + maskLen, size_t(len));
		if (masked) {
			for (size_t i = 0; i < payload.size(); i++) {
				payload[i] = char(payload[i] ^ buf[head + (i & 3)]);
			}
		}
		buf.erase(0, head + maskLen + size_t(len));

		if (opcode >= 8) {
			/* control frames are never fragmented */
			frames.push_back({opcode, std::move(payload)});
			continue;
		}
		if (opcode != 0) {
			fragmentOpcode = opcode;
			fragment = std::move(payload);
		} else {
			fragment += payload;
		}
		if (fin) {
			frames.push_back({fragmentOpcode, std::move(fragment)});
			fragment.clear();
			fragmentOpcode = 0;
		}
	}
	return frames;
}

std::string WsEncode(int opcode, const std::string &payload, const unsigned char mask[4])
{
	std::string out;
	out += char(0x80 | (opcode & 0x0f));
	const size_t len = payload.size();
	if (len < 126) {
		out += char(0x80 | len);
	} else if (len < 65536) {
		out += char(0x80 | 126);
		out += char((len >> 8) & 0xff);
		out += char(len & 0xff);
	} else {
		out += char(0x80 | 127);
		for (int i = 7; i >= 0; i--) {
			out += char((uint64_t(len) >> (8 * i)) & 0xff);
		}
	}
	out.append(reinterpret_cast<const char *>(mask), 4);
	for (size_t i = 0; i < len; i++) {
		out += char(payload[i] ^ char(mask[i & 3]));
	}
	return out;
}

} // namespace spazzchat
