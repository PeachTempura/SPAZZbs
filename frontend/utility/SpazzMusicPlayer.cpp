/******************************************************************************
    SPAZZbs - music player engine behind the SPAZZ Music dock
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "SpazzMusicPlayer.hpp"

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>

#include <util/platform.h>

extern "C" {
#include <spazz-analyzer.h>
}

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QHash>
#include <QSet>
#include <QTextStream>
#include <QThread>

#include <algorithm>
#include <cmath>

#include "moc_SpazzMusicPlayer.cpp"

namespace {
constexpr const char *kConfigSection = "SPAZZbs";
constexpr const char *kConfigPlaylist = "MusicPlaylist";
constexpr uint64_t kSilenceNs = 250000000ULL;

const QStringList &AudioExtensions()
{
	static const QStringList exts{"mp3", "wav",  "flac", "ogg", "oga", "opus", "m4a", "aac", "wma",
				      "aif", "aiff", "alac", "ape", "wv",  "mka", "mp4", "m4v", "mkv",
				      "webm", "mov", "flv"};
	return exts;
}

QString RepeatToString(SpazzMusicPlayer::Repeat r)
{
	switch (r) {
	case SpazzMusicPlayer::Repeat::Off:
		return "off";
	case SpazzMusicPlayer::Repeat::One:
		return "one";
	default:
		return "all";
	}
}

SpazzMusicPlayer::Repeat RepeatFromString(const QString &s)
{
	if (s == "off")
		return SpazzMusicPlayer::Repeat::Off;
	if (s == "one")
		return SpazzMusicPlayer::Repeat::One;
	return SpazzMusicPlayer::Repeat::All;
}
} // namespace

SpazzMusicPlayer::SpazzMusicPlayer(QObject *parent) : QObject(parent), analyzer(new spazz_analyzer())
{
	struct obs_audio_info oai = {};
	uint32_t rate = obs_get_audio_info(&oai) ? oai.samples_per_sec : 48000;
	spazz_analyzer_init(analyzer.get(), rate);

	saveTimer.setSingleShot(true);
	saveTimer.setInterval(600);
	connect(&saveTimer, &QTimer::timeout, this, &SpazzMusicPlayer::SavePlaylist);

	LoadPlaylist();

	obs_frontend_add_event_callback(FrontendEvent, this);
	callbackRegistered = true;

	/* the scene collection is already loaded by the time we're created */
	Bind();
}

SpazzMusicPlayer::~SpazzMusicPlayer()
{
	if (callbackRegistered) {
		obs_frontend_remove_event_callback(FrontendEvent, this);
		callbackRegistered = false;
	}

	if (saveTimer.isActive())
		SavePlaylist();

	Unbind();
}

bool SpazzMusicPlayer::IsAudioFile(const QString &path)
{
	return AudioExtensions().contains(QFileInfo(path).suffix().toLower());
}

QString SpazzMusicPlayer::TitleFor(const QString &path)
{
	return QFileInfo(path).completeBaseName();
}

/* ------------------------------------------------------------------------ */
/* source binding                                                            */

void SpazzMusicPlayer::Bind()
{
	if (source)
		return;

	/* find our source in this scene collection by its marker, so renaming
	 * it in OBS doesn't lose it */
	struct FindData {
		obs_source_t *found = nullptr;
	} find;

	obs_enum_sources(
		[](void *param, obs_source_t *src) {
			auto *data = static_cast<FindData *>(param);
			OBSDataAutoRelease priv = obs_source_get_private_settings(src);
			if (obs_data_get_bool(priv, "spazz_music")) {
				data->found = obs_source_get_ref(src);
				return false;
			}
			return true;
		},
		&find);

	OBSSourceAutoRelease src = find.found;

	if (src) {
		/* don't start blasting the last song the moment OBS opens, and undo
		 * any property tweaks that would break the playlist */
		ApplySafeSettings(src, "");
	} else {
		QString name = kSourceName;
		for (int i = 2;; i++) {
			OBSSourceAutoRelease taken = obs_get_source_by_name(QT_TO_UTF8(name));
			if (!taken)
				break;
			name = QString("%1 %2").arg(kSourceName).arg(i);
		}

		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_bool(settings, "is_local_file", true);
		obs_data_set_string(settings, "local_file", "");
		obs_data_set_bool(settings, "looping", false);
		obs_data_set_bool(settings, "restart_on_activate", true);
		obs_data_set_bool(settings, "close_when_inactive", false);
		obs_data_set_bool(settings, "clear_on_media_end", true);
		obs_data_set_bool(settings, "hw_decode", false);
		obs_data_set_int(settings, "speed_percent", 100);

		src = obs_source_create("ffmpeg_source", QT_TO_UTF8(name), settings, nullptr);
		if (!src) {
			blog(LOG_WARNING, "[SPAZZ Music] could not create the music source");
			return;
		}

		/* hear it yourself, at the same volume as the mixer fader */
		obs_source_set_monitoring_enabled(src, true);

		OBSDataAutoRelease priv = obs_source_get_private_settings(src);
		obs_data_set_bool(priv, "spazz_music", true);
		obs_data_set_bool(priv, "mixer_pinned", true);
	}

	source = src.Get();

	/* its own global channel: in the mixer and on stream, without having
	 * to be in a scene */
	obs_set_output_source(kChannel, source);

	signal_handler_t *sh = obs_source_get_signal_handler(source);
	sourceSignals.emplace_back(sh, "media_ended", MediaEndedSignal, this);
	sourceSignals.emplace_back(sh, "media_started", MediaStartedSignal, this);
	sourceSignals.emplace_back(sh, "volume", VolumeSignal, this);
	sourceSignals.emplace_back(sh, "save", SaveSignal, this);

	obs_source_add_audio_capture_callback(source, AudioCapture, this);

	emit SourceChanged();
	emit VolumeChanged(Volume());
	emit StateChanged();
}

void SpazzMusicPlayer::Unbind()
{
	if (!source)
		return;

	obs_source_remove_audio_capture_callback(source, AudioCapture, this);
	sourceSignals.clear();

	OBSSourceAutoRelease onChannel = obs_get_output_source(kChannel);
	if (onChannel == source)
		obs_set_output_source(kChannel, nullptr);

	source = nullptr;

	emit SourceChanged();
	emit StateChanged();
}

void SpazzMusicPlayer::ApplySafeSettings(obs_source_t *src, const char *localFile)
{
	/* a looping or sped-up source would break the playlist; anything else
	 * in its properties (filters, sync offset, tracks) is left as you set it */
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_string(settings, "local_file", localFile);
	obs_data_set_bool(settings, "looping", false);
	obs_data_set_bool(settings, "restart_on_activate", true);
	obs_data_set_bool(settings, "close_when_inactive", false);
	obs_data_set_int(settings, "speed_percent", 100);
	obs_source_update(src, settings);
}

/* OBS is about to write the source into the scene collection. Leave the song
 * path out of the file, or a later moved/deleted song makes OBS show its
 * Missing Files window at startup. The playing song isn't affected, and the
 * path is put back right after the save finishes. */
void SpazzMusicPlayer::SaveSignal(void *param, calldata_t *)
{
	auto *self = static_cast<SpazzMusicPlayer *>(param);

	/* scene collections are saved on the UI thread; leave any other save alone */
	if (QThread::currentThread() != self->thread() || !self->source)
		return;

	OBSDataAutoRelease settings = obs_source_get_settings(self->source);
	obs_data_set_string(settings, "local_file", "");
	QMetaObject::invokeMethod(self, "RestoreFileAfterSave", Qt::QueuedConnection);
}

void SpazzMusicPlayer::RestoreFileAfterSave()
{
	if (!source)
		return;

	int index = CurrentIndex();
	obs_media_state state = State();
	bool loaded = state == OBS_MEDIA_STATE_PLAYING || state == OBS_MEDIA_STATE_PAUSED ||
		      state == OBS_MEDIA_STATE_ENDED || state == OBS_MEDIA_STATE_STOPPED;

	/* only the stored value; no update, so playback isn't touched */
	if (index >= 0 && loaded) {
		OBSDataAutoRelease settings = obs_source_get_settings(source);
		obs_data_set_string(settings, "local_file", QT_TO_UTF8(tracks[index].path));
	}
}

void SpazzMusicPlayer::FrontendEvent(enum obs_frontend_event event, void *param)
{
	auto *self = static_cast<SpazzMusicPlayer *>(param);

	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		self->Unbind();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		self->Bind();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		self->Unbind();
		self->SavePlaylist();
		obs_frontend_remove_event_callback(FrontendEvent, self);
		self->callbackRegistered = false;
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------------ */
/* signals from the source (any thread)                                      */

void SpazzMusicPlayer::MediaEndedSignal(void *param, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<SpazzMusicPlayer *>(param), "HandleMediaEnded", Qt::QueuedConnection);
}

void SpazzMusicPlayer::MediaStartedSignal(void *param, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<SpazzMusicPlayer *>(param), "HandleMediaStarted", Qt::QueuedConnection);
}

void SpazzMusicPlayer::VolumeSignal(void *param, calldata_t *data)
{
	auto *self = static_cast<SpazzMusicPlayer *>(param);
	float volume = (float)calldata_float(data, "volume");
	QMetaObject::invokeMethod(self, [self, volume]() { emit self->VolumeChanged(volume); }, Qt::QueuedConnection);
}

void SpazzMusicPlayer::AudioCapture(void *param, obs_source_t *, const struct audio_data *audio, bool muted)
{
	auto *self = static_cast<SpazzMusicPlayer *>(param);
	if (!audio->frames)
		return;

	size_t channels = audio_output_get_channels(obs_get_audio());
	if (channels > MAX_AV_PLANES)
		channels = MAX_AV_PLANES;

	std::lock_guard<std::mutex> lock(self->analyzerMutex);

	if (muted) {
		spazz_analyzer_push_silence(self->analyzer.get(), audio->frames);
	} else {
		if (self->monoScratch.size() < audio->frames)
			self->monoScratch.resize(audio->frames);

		for (uint32_t i = 0; i < audio->frames; i++) {
			float sum = 0.0f;
			int used = 0;
			for (size_t c = 0; c < channels; c++) {
				if (audio->data[c]) {
					sum += reinterpret_cast<const float *>(audio->data[c])[i];
					used++;
				}
			}
			self->monoScratch[i] = used ? sum / used : 0.0f;
		}
		spazz_analyzer_push(self->analyzer.get(), self->monoScratch.data(), audio->frames);
	}

	self->lastAudioNs = os_gettime_ns();
}

void SpazzMusicPlayer::HandleMediaEnded()
{
	/* switching files can report the old one as ended; only advance when
	 * the source really is sitting at the end of a song */
	if (State() != OBS_MEDIA_STATE_ENDED) {
		emit StateChanged();
		return;
	}

	if (repeat == Repeat::One) {
		int index = CurrentIndex();
		if (index >= 0) {
			StartTrack(index);
			return;
		}
	}

	int next = PickNextIndex(false);
	if (next >= 0)
		StartTrack(next);
	else
		emit StateChanged();
}

void SpazzMusicPlayer::HandleMediaStarted()
{
	int index = CurrentIndex();
	qint64 dur = DurationMs();

	if (index >= 0 && dur > 0) {
		Track &t = tracks[index];
		if (t.durationMs != dur) {
			t.durationMs = dur;
			emit TrackUpdated(index);
			QueueSave();
		}

		qint64 bytes = QFileInfo(t.path).size();
		kbps = bytes > 0 ? (int)std::llround((double)bytes * 8.0 / (double)dur) : 0;
	}

	emit StateChanged();
}

/* ------------------------------------------------------------------------ */
/* playlist                                                                  */

int SpazzMusicPlayer::IndexOf(quint64 id) const
{
	for (int i = 0; i < tracks.size(); i++) {
		if (tracks[i].id == id)
			return i;
	}
	return -1;
}

int SpazzMusicPlayer::CurrentIndex() const
{
	return IndexOf(currentId);
}

void SpazzMusicPlayer::ExpandPath(const QString &path, QStringList &out, int depth) const
{
	QFileInfo info(path);

	if (info.isDir()) {
		if (depth > 4)
			return;

		QDir dir(path);
		QStringList entries = dir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable,
						    QDir::Name | QDir::DirsLast | QDir::IgnoreCase);
		for (const QString &entry : entries)
			ExpandPath(dir.filePath(entry), out, depth + 1);
		return;
	}

	QString suffix = info.suffix().toLower();
	if (suffix == "m3u" || suffix == "m3u8") {
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
			return;

		QTextStream in(&file);
		QDir base = info.absoluteDir();
		while (!in.atEnd()) {
			QString line = in.readLine().trimmed();
			if (line.isEmpty() || line.startsWith('#'))
				continue;
			QString entry = QDir::isAbsolutePath(line) ? line : base.filePath(line);
			if (IsAudioFile(entry) && QFileInfo::exists(entry))
				out.push_back(QDir::cleanPath(entry));
		}
		return;
	}

	if (IsAudioFile(path) && info.exists())
		out.push_back(info.absoluteFilePath());
}

int SpazzMusicPlayer::AddPaths(const QStringList &paths, int insertAt)
{
	QStringList files;
	for (const QString &p : paths)
		ExpandPath(p, files, 0);

	if (files.isEmpty())
		return -1;

	if (insertAt < 0 || insertAt > tracks.size())
		insertAt = tracks.size();
	const int first = insertAt;

	bool wasEmpty = tracks.isEmpty();

	for (const QString &f : files) {
		Track t;
		t.id = nextId++;
		t.path = f;
		t.title = TitleFor(f);
		tracks.insert(insertAt++, t);
	}

	if (wasEmpty && currentId == 0)
		currentId = tracks.first().id;

	emit PlaylistChanged();
	emit CurrentChanged();
	QueueSave();
	return first;
}

void SpazzMusicPlayer::RemoveIds(const QList<quint64> &ids)
{
	if (ids.isEmpty())
		return;

	const QSet<quint64> gone(ids.begin(), ids.end());
	bool removingCurrent = gone.contains(currentId);
	int oldIndex = CurrentIndex();

	tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [&](const Track &t) { return gone.contains(t.id); }),
		     tracks.end());
	shuffleHistory.erase(std::remove_if(shuffleHistory.begin(), shuffleHistory.end(),
					    [&](quint64 id) { return gone.contains(id); }),
			     shuffleHistory.end());

	if (removingCurrent) {
		Stop();
		if (tracks.isEmpty()) {
			currentId = 0;
		} else {
			int idx = std::min(oldIndex, (int)tracks.size() - 1);
			currentId = tracks[std::max(idx, 0)].id;
		}
		emit CurrentChanged();
	}

	emit PlaylistChanged();
	QueueSave();
}

void SpazzMusicPlayer::Clear()
{
	Stop();
	tracks.clear();
	shuffleHistory.clear();
	currentId = 0;
	emit PlaylistChanged();
	emit CurrentChanged();
	QueueSave();
}

void SpazzMusicPlayer::SetOrder(const QList<quint64> &ids)
{
	QHash<quint64, int> where;
	where.reserve(tracks.size());
	for (int i = 0; i < tracks.size(); i++)
		where.insert(tracks[i].id, i);

	QList<Track> reordered;
	reordered.reserve(tracks.size());
	QSet<quint64> placed;
	for (quint64 id : ids) {
		auto it = where.constFind(id);
		if (it != where.constEnd() && !placed.contains(id)) {
			reordered.push_back(tracks[it.value()]);
			placed.insert(id);
		}
	}

	/* anything not mentioned keeps its place at the end */
	for (const Track &t : tracks) {
		if (!placed.contains(t.id))
			reordered.push_back(t);
	}

	tracks = reordered;
	emit PlaylistChanged();
	QueueSave();
}

/* ------------------------------------------------------------------------ */
/* transport                                                                 */

obs_media_state SpazzMusicPlayer::State() const
{
	return source ? obs_source_media_get_state(source) : OBS_MEDIA_STATE_NONE;
}

qint64 SpazzMusicPlayer::PositionMs() const
{
	return source ? obs_source_media_get_time(source) : 0;
}

qint64 SpazzMusicPlayer::DurationMs() const
{
	return source ? obs_source_media_get_duration(source) : 0;
}

void SpazzMusicPlayer::StartTrack(int index)
{
	if (!source || index < 0 || index >= tracks.size())
		return;

	const Track &t = tracks[index];
	currentId = t.id;

	if (shuffle && (shuffleHistory.isEmpty() || shuffleHistory.last() != t.id))
		shuffleHistory.push_back(t.id);

	/* stopping first means the old song won't report "ended" */
	obs_media_state state = State();
	if (state == OBS_MEDIA_STATE_PLAYING || state == OBS_MEDIA_STATE_PAUSED)
		obs_source_media_stop(source);

	/* clear then set, so the same file played again still restarts */
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "local_file", "");
	obs_source_update(source, settings);
	obs_data_set_string(settings, "local_file", QT_TO_UTF8(t.path));
	obs_source_update(source, settings);

	kbps = 0;

	emit CurrentChanged();
	emit StateChanged();
	QueueSave();
}

int SpazzMusicPlayer::PickNextIndex(bool userAction) const
{
	const int count = tracks.size();
	if (count == 0)
		return -1;

	int cur = CurrentIndex();

	if (shuffle) {
		if (count == 1)
			return (repeat == Repeat::Off && !userAction) ? -1 : 0;

		/* with repeat off, stop once everything has had a turn */
		const QSet<quint64> played(shuffleHistory.begin(), shuffleHistory.end());
		QList<int> fresh;
		for (int i = 0; i < count; i++) {
			if (i != cur && !played.contains(tracks[i].id))
				fresh.push_back(i);
		}

		if (fresh.isEmpty() && repeat == Repeat::Off && !userAction)
			return -1;

		if (fresh.isEmpty()) {
			int pick = (int)QRandomGenerator::global()->bounded(count - 1);
			return pick >= cur ? pick + 1 : pick;
		}

		return fresh[(int)QRandomGenerator::global()->bounded(fresh.size())];
	}

	int next = cur + 1;
	if (next >= count) {
		if (repeat == Repeat::Off && !userAction)
			return -1;
		next = 0;
	}
	return next;
}

void SpazzMusicPlayer::PlayIndex(int index)
{
	if (shuffle)
		shuffleHistory.clear();
	StartTrack(index);
}

void SpazzMusicPlayer::Play()
{
	if (!source || tracks.isEmpty())
		return;

	obs_media_state state = State();
	if (state == OBS_MEDIA_STATE_PAUSED) {
		obs_source_media_play_pause(source, false);
		emit StateChanged();
		return;
	}

	if (state == OBS_MEDIA_STATE_PLAYING)
		return;

	int index = CurrentIndex();
	if (index < 0)
		index = shuffle ? (int)QRandomGenerator::global()->bounded(tracks.size()) : 0;
	StartTrack(index);
}

void SpazzMusicPlayer::Pause()
{
	if (source && State() == OBS_MEDIA_STATE_PLAYING) {
		obs_source_media_play_pause(source, true);
		emit StateChanged();
	}
}

void SpazzMusicPlayer::TogglePlayPause()
{
	if (State() == OBS_MEDIA_STATE_PLAYING)
		Pause();
	else
		Play();
}

void SpazzMusicPlayer::Stop()
{
	if (!source)
		return;

	obs_media_state state = State();
	if (state == OBS_MEDIA_STATE_PLAYING || state == OBS_MEDIA_STATE_PAUSED)
		obs_source_media_stop(source);

	emit StateChanged();
}

void SpazzMusicPlayer::Next()
{
	int next = PickNextIndex(true);
	if (next >= 0)
		StartTrack(next);
}

void SpazzMusicPlayer::Previous()
{
	if (tracks.isEmpty())
		return;

	/* a few seconds in, "previous" restarts the song like Winamp does */
	obs_media_state state = State();
	if ((state == OBS_MEDIA_STATE_PLAYING || state == OBS_MEDIA_STATE_PAUSED) && PositionMs() > 3000) {
		Seek(0);
		return;
	}

	if (shuffle && shuffleHistory.size() > 1) {
		shuffleHistory.removeLast();
		int idx = IndexOf(shuffleHistory.last());
		shuffleHistory.removeLast();
		if (idx >= 0) {
			StartTrack(idx);
			return;
		}
	}

	int cur = CurrentIndex();
	int prev = cur - 1;
	if (prev < 0)
		prev = tracks.size() - 1;
	StartTrack(prev);
}

void SpazzMusicPlayer::Seek(qint64 ms)
{
	if (!source)
		return;

	obs_media_state state = State();
	if (state == OBS_MEDIA_STATE_ENDED || state == OBS_MEDIA_STATE_STOPPED || state == OBS_MEDIA_STATE_NONE) {
		int index = CurrentIndex();
		if (index < 0)
			return;
		StartTrack(index);
	}

	obs_source_media_set_time(source, std::max<qint64>(ms, 0));
	emit StateChanged();
}

void SpazzMusicPlayer::SetShuffle(bool on)
{
	if (shuffle == on)
		return;
	shuffle = on;
	shuffleHistory.clear();
	if (on && currentId)
		shuffleHistory.push_back(currentId);
	emit StateChanged();
	QueueSave();
}

void SpazzMusicPlayer::SetRepeat(Repeat mode)
{
	if (repeat == mode)
		return;
	repeat = mode;
	emit StateChanged();
	QueueSave();
}

float SpazzMusicPlayer::Volume() const
{
	return source ? obs_source_get_volume(source) : 1.0f;
}

void SpazzMusicPlayer::SetVolume(float mul)
{
	if (source)
		obs_source_set_volume(source, std::clamp(mul, 0.0f, 1.0f));
}

float SpazzMusicPlayer::Balance() const
{
	return source ? obs_source_get_balance_value(source) : 0.5f;
}

void SpazzMusicPlayer::SetBalance(float balance)
{
	if (source)
		obs_source_set_balance_value(source, std::clamp(balance, 0.0f, 1.0f));
}

void SpazzMusicPlayer::UpdateSpectrum(float dt, float *bars, int count, float *peaks)
{
	std::lock_guard<std::mutex> lock(analyzerMutex);

	if (os_gettime_ns() - lastAudioNs > kSilenceNs)
		spazz_analyzer_push_silence(analyzer.get(), (size_t)(dt * analyzer->sample_rate));

	spazz_analyzer_update(analyzer.get(), dt, 1.0f);

	/* fold 64 log-spaced bands into the dock's few bars */
	for (int i = 0; i < count; i++) {
		int lo = i * SPAZZ_NUM_BANDS / count;
		int hi = std::max(lo + 1, (i + 1) * SPAZZ_NUM_BANDS / count);
		float v = 0.0f, p = 0.0f;
		for (int b = lo; b < hi; b++) {
			v = std::max(v, analyzer->bands[b]);
			p = std::max(p, analyzer->peaks[b]);
		}
		bars[i] = v;
		if (peaks)
			peaks[i] = p;
	}
}

/* ------------------------------------------------------------------------ */
/* persistence (per user, not per scene collection: it's your music)         */

void SpazzMusicPlayer::QueueSave()
{
	saveTimer.start();
}

void SpazzMusicPlayer::SavePlaylist()
{
	saveTimer.stop();

	QJsonArray list;
	for (const Track &t : tracks) {
		QJsonObject o;
		o["path"] = t.path;
		if (t.durationMs > 0)
			o["duration"] = (double)t.durationMs;
		list.append(o);
	}

	QJsonObject root;
	root["version"] = 1;
	root["tracks"] = list;
	root["current"] = CurrentIndex();
	root["shuffle"] = shuffle;
	root["repeat"] = RepeatToString(repeat);

	QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
	config_set_string(App()->GetUserConfig(), kConfigSection, kConfigPlaylist, json.constData());
	config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
}

void SpazzMusicPlayer::LoadPlaylist()
{
	const char *json = config_get_string(App()->GetUserConfig(), kConfigSection, kConfigPlaylist);
	if (!json || !*json)
		return;

	QJsonObject root = QJsonDocument::fromJson(QByteArray(json)).object();

	for (const QJsonValue &value : root["tracks"].toArray()) {
		QJsonObject o = value.toObject();
		Track t;
		t.id = nextId++;
		t.path = o["path"].toString();
		t.title = TitleFor(t.path);
		t.durationMs = o.contains("duration") ? (qint64)o["duration"].toDouble() : -1;
		if (!t.path.isEmpty())
			tracks.push_back(t);
	}

	int current = root["current"].toInt(-1);
	if (current >= 0 && current < tracks.size())
		currentId = tracks[current].id;
	else if (!tracks.isEmpty())
		currentId = tracks.first().id;

	shuffle = root["shuffle"].toBool(false);
	repeat = RepeatFromString(root["repeat"].toString("all"));
}
