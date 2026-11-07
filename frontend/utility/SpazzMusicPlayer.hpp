/******************************************************************************
    SPAZZbs - music player engine behind the SPAZZ Music dock
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <obs.hpp>
#include <obs-frontend-api.h>

#include <QList>
#include <QObject>
#include <QStringList>
#include <QSet>
#include <QTimer>

#include <memory>
#include <mutex>
#include <vector>

struct spazz_analyzer;

/* Owns the "SPAZZ Music" audio source: a Media Source on its own global
 * audio channel, so it shows in the Audio Mixer with a fader, goes to the
 * stream, and is monitored so you hear it at the same volume. The dock is
 * only UI on top of this. */
class SpazzMusicPlayer : public QObject {
	Q_OBJECT

public:
	enum class Repeat { Off, All, One };

	struct Track {
		quint64 id = 0;
		QString path;
		QString title;
		qint64 durationMs = -1;
	};

	/* playlists ("folders"); one is active: its songs are Tracks() */
	struct Folder {
		QString name;
		QString syncPath; /* kept in step with this folder on disk, if set */
		QList<Track> tracks;
	};

	/* a high output channel, clear of the ones plugins usually take */
	static constexpr uint32_t kChannel = 60;
	static constexpr const char *kSourceName = "SPAZZ Music";

	explicit SpazzMusicPlayer(QObject *parent = nullptr);
	~SpazzMusicPlayer();

	static bool IsAudioFile(const QString &path);

	/* playlist */
	const QList<Track> &Tracks() const { return tracks; }
	int CurrentIndex() const;
	quint64 CurrentId() const { return currentId; }
	/* returns the index of the first track added, or -1 */
	int AddPaths(const QStringList &paths, int insertAt = -1);
	void RemoveIds(const QList<quint64> &ids);
	void Clear();
	void SetOrder(const QList<quint64> &ids);

	/* folders */
	int FolderCount() const { return folders.size(); }
	QString FolderName(int i) const { return i >= 0 && i < folders.size() ? folders[i].name : QString(); }
	QString FolderSync(int i) const { return i >= 0 && i < folders.size() ? folders[i].syncPath : QString(); }
	int FolderTrackCount(int i) const;
	int ActiveFolder() const { return activeFolder; }
	void SelectFolder(int i);
	int AddFolder(const QString &name);
	void RenameFolder(int i, const QString &name);
	void RemoveFolder(int i);
	void MoveFolder(int from, int to);
	void SetFolderSync(int i, const QString &dir);
	void SyncFolderNow(int i);
	/* drag songs onto another folder (copy keeps them here too) */
	void MoveTracksToFolder(const QList<quint64> &ids, int folder, bool copy);
	int AddPathsToFolder(const QStringList &paths, int folder);
	/* the song playing, even when it's from another folder */
	QString NowPlayingTitle() const;

	/* transport */
	void PlayIndex(int index);
	void Play();
	void Pause();
	void TogglePlayPause();
	void Stop();
	void Next();
	void Previous();
	void Seek(qint64 ms);

	bool Shuffle() const { return shuffle; }
	void SetShuffle(bool on);
	Repeat RepeatMode() const { return repeat; }
	void SetRepeat(Repeat mode);

	obs_media_state State() const;
	qint64 PositionMs() const;
	qint64 DurationMs() const;
	int EstimatedKbps() const { return kbps; }

	float Volume() const;
	void SetVolume(float mul);
	float Balance() const;
	void SetBalance(float balance);

	OBSSource Source() const { return source; }

	/* 0..1 levels for the dock's mini spectrum, updated by UpdateSpectrum() */
	void UpdateSpectrum(float dt, float *bars, int count, float *peaks);

signals:
	void PlaylistChanged();
	void FoldersChanged();
	void CurrentChanged();
	void StateChanged();
	void VolumeChanged(float mul);
	void SourceChanged();
	/* one track's details changed (e.g. its length became known) */
	void TrackUpdated(int index);

private slots:
	void HandleMediaEnded();
	void HandleMediaStarted();
	void SavePlaylist();
	void RestoreFileAfterSave();
	/* refresh the snapshot spazz_music_now hands out */
	void UpdateNowPlaying();

private:
	OBSSource source;
	std::vector<OBSSignal> sourceSignals;

	QList<Track> tracks; /* the active folder's songs (its copy in folders[] is stale) */
	QList<Folder> folders;
	int activeFolder = 0;
	class QFileSystemWatcher *watcher = nullptr;
	QTimer syncTimer;
	QSet<QString> dirtyDirs;
	quint64 nextId = 1;
	quint64 currentId = 0;
	QList<quint64> shuffleHistory;
	QString loadedPath; /* the file the source has open */
	int durationRetries = 0;
	uint64_t startedNs = 0;   /* when the current song was asked for */
	bool keepPlaying = false; /* removing the playing song without stopping it */

	bool shuffle = false;
	Repeat repeat = Repeat::All;
	int kbps = 0;

	QTimer saveTimer;
	QTimer nowTimer;
	bool callbackRegistered = false;

	std::mutex analyzerMutex;
	std::unique_ptr<spazz_analyzer> analyzer;
	std::unique_ptr<spazz_analyzer> spectrum; /* the UI thread's copy, analysed outside the lock */
	std::vector<float> monoScratch;
	uint64_t lastAudioNs = 0;

	void Bind();
	void Unbind();
	void LoadPlaylist();
	void QueueSave();
	void StashActive();
	void UpdateWatcher();
	void RunPendingSyncs();
	QList<Track> MakeTracks(const QStringList &files);
	void StartTrack(int index);
	int PickNextIndex(bool userAction) const;
	int IndexOf(quint64 id) const;
	void ExpandPath(const QString &path, QStringList &out, int depth) const;
	static QString TitleFor(const QString &path);

	static void FrontendEvent(enum obs_frontend_event event, void *param);
	static void MediaEndedSignal(void *param, calldata_t *data);
	static void MediaStartedSignal(void *param, calldata_t *data);
	static void VolumeSignal(void *param, calldata_t *data);
	static void SaveSignal(void *param, calldata_t *data);
	static void ApplySafeSettings(obs_source_t *src, const char *localFile);
	static void AudioCapture(void *param, obs_source_t *src, const struct audio_data *audio, bool muted);
};
