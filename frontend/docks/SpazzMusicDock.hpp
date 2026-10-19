/******************************************************************************
    SPAZZbs - "SPAZZamp" music dock, a modern take on the classic Winamp skin
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "OBSDock.hpp"

#include <QAbstractButton>
#include <QElapsedTimer>
#include <QListWidget>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class SpazzMusicPlayer;
class QBoxLayout;
class QLabel;

/* Shared look: night colours plus an accent that slowly cycles through RGB. */
namespace SpazzAmp {
QColor Accent(float offset = 0.0f);
void SetHue(float hue);
float Hue();
QFont LcdFont(int pixelSize, bool bold = true);
void DrawGlowText(class QPainter &p, const QRectF &r, int flags, const QString &text, const QColor &c,
		  float glow = 1.0f);
} // namespace SpazzAmp

class AmpButton : public QAbstractButton {
	Q_OBJECT

public:
	enum class Glyph { Prev, Play, Pause, Stop, Next, Eject, Text, Repeat };

	AmpButton(Glyph glyph, const QString &text = QString(), QWidget *parent = nullptr);
	QSize sizeHint() const override;
	void SetLed(bool led) { hasLed = led; }
	void SetRepeatOne(bool one)
	{
		repeatOne = one;
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override;
	void enterEvent(QEnterEvent *) override;
	void leaveEvent(QEvent *) override;

private:
	Glyph glyph;
	bool hasLed = false;
	bool hovered = false;
	bool repeatOne = false;
};

class AmpSlider : public QWidget {
	Q_OBJECT

public:
	enum class Kind { Seek, Volume, Balance };

	AmpSlider(Kind kind, QWidget *parent = nullptr);
	float Value() const { return value; }
	void SetValue(float v);
	bool IsDragging() const { return dragging; }
	QSize sizeHint() const override;

signals:
	void Moved(float value);
	void Released(float value);

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void wheelEvent(QWheelEvent *) override;

private:
	Kind kind;
	float value = 0.0f;
	bool dragging = false;
	float ValueAt(qreal x) const;
	QRectF Track() const;
};

class AmpDisplay : public QWidget {
	Q_OBJECT

public:
	explicit AmpDisplay(SpazzMusicPlayer *player, QWidget *parent = nullptr);
	void Tick(float dt);
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;

private:
	SpazzMusicPlayer *player;
	float bars[19] = {};
	float peaks[19] = {};
	bool showRemaining = false;
	float marquee = 0.0f;
};

class AmpPlaylist : public QListWidget {
	Q_OBJECT

public:
	explicit AmpPlaylist(QWidget *parent = nullptr);

signals:
	void FilesDropped(const QStringList &paths, int row);
	void OrderChanged();
	void DeletePressed();
	void ActivatePressed();

protected:
	void dragEnterEvent(QDragEnterEvent *e) override;
	void dragMoveEvent(QDragMoveEvent *e) override;
	void dropEvent(QDropEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
};

class SpazzMusicDock : public OBSDock {
	Q_OBJECT

public:
	SpazzMusicDock(SpazzMusicPlayer *player, QWidget *parent = nullptr);

protected:
	void resizeEvent(QResizeEvent *e) override;
	void dragEnterEvent(QDragEnterEvent *e) override;
	void dropEvent(QDropEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;

private:
	QPointer<SpazzMusicPlayer> player;

	QWidget *body = nullptr;
	QBoxLayout *mainLayout = nullptr;
	QWidget *playerPanel = nullptr;
	QWidget *listPanel = nullptr;

	AmpDisplay *display = nullptr;
	AmpSlider *seek = nullptr;
	AmpSlider *volume = nullptr;
	AmpSlider *balance = nullptr;
	AmpButton *playBtn = nullptr;
	AmpButton *pauseBtn = nullptr;
	AmpButton *shuffleBtn = nullptr;
	AmpButton *repeatBtn = nullptr;
	AmpPlaylist *list = nullptr;
	QLabel *totals = nullptr;

	QTimer frameTimer;
	QElapsedTimer clock;

	void Frame();
	void RebuildList();
	void SyncCurrent();
	void SyncState();
	void AddFilesDialog();
	void AddFolderDialog();
	QList<quint64> SelectedIds() const;
};
