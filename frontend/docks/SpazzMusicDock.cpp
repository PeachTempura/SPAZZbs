/******************************************************************************
    SPAZZbs - "SPAZZamp" music dock, a modern take on the classic Winamp skin
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "SpazzMusicDock.hpp"

#include <OBSApp.hpp>
#include <utility/SpazzMusicPlayer.hpp>

#include <QBoxLayout>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QScrollBar>
#include <QSet>
#include <QDir>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "moc_SpazzMusicDock.cpp"

/* ======================================================================== */
/* look                                                                      */

namespace {
float g_hue = 0.0f;

const QColor kBodyTop(0x1e, 0x1c, 0x2b);
const QColor kBodyBottom(0x12, 0x11, 0x1a);
const QColor kEdgeDark(0x05, 0x05, 0x08);
const QColor kLcd(0x04, 0x05, 0x0a);
const QColor kText(0xd6, 0xd2, 0xe6);
const QColor kTextDim(0x6c, 0x67, 0x84);

enum Roles { IdRole = Qt::UserRole + 1, DurationRole, CurrentRole };

/* the theme sets fonts with a stylesheet, which beats setFont(), so the
 * LCD-style labels set theirs the same way */
const char *kLabelStyle = "color: #6c6784; background: transparent; font-weight: bold; font-size: 10px;"
			  "font-family: Consolas, 'Lucida Console', 'DejaVu Sans Mono', 'Courier New', monospace;";

QString FormatTime(qint64 ms, bool padMinutes = false)
{
	if (ms < 0)
		ms = 0;
	qint64 total = ms / 1000;
	qint64 h = total / 3600;
	qint64 m = (total / 60) % 60;
	qint64 s = total % 60;
	if (h > 0)
		return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
	return QString("%1:%2").arg(m, padMinutes ? 2 : 1, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
}

QColor WithAlpha(QColor c, int a)
{
	c.setAlpha(a);
	return c;
}

void DrawBevelPanel(QPainter &p, const QRectF &r, qreal radius = 5.0)
{
	QLinearGradient g(r.topLeft(), r.bottomLeft());
	g.setColorAt(0.0, kBodyTop);
	g.setColorAt(1.0, kBodyBottom);
	p.setPen(QPen(kEdgeDark, 1.0));
	p.setBrush(g);
	p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);

	/* soft light along the top-left edge, like the old skins */
	p.setPen(QPen(QColor(255, 255, 255, 26), 1.0));
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(r.adjusted(1.5, 1.5, -1.5, -1.5), radius - 1, radius - 1);
}

void DrawLcdWell(QPainter &p, const QRectF &r, qreal radius = 3.0)
{
	p.setPen(QPen(QColor(0, 0, 0), 1.0));
	p.setBrush(kLcd);
	p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);

	/* faint dot grid, one 3x3 tile repeated instead of a rect per dot */
	static const QBrush dots = []() {
		QPixmap tile(3, 3);
		tile.fill(Qt::transparent);
		QPainter tp(&tile);
		tp.fillRect(QRect(0, 0, 1, 1), QColor(255, 255, 255, 7));
		return QBrush(tile);
	}();
	p.save();
	p.setBrushOrigin(r.left() + 3, r.top() + 3);
	p.fillRect(QRectF(r.left() + 3, r.top() + 3, r.width() - 5, r.height() - 5), dots);
	p.restore();

	/* inner bevel: dark top, faint light bottom */
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(QColor(255, 255, 255, 18), 1.0));
	p.drawLine(QPointF(r.left() + 2, r.bottom() - 0.5), QPointF(r.right() - 2, r.bottom() - 0.5));
}
} // namespace

namespace SpazzAmp {
void SetHue(float hue)
{
	g_hue = hue - std::floor(hue);
}

float Hue()
{
	return g_hue;
}

QColor Accent(float offset)
{
	float h = g_hue + offset;
	h -= std::floor(h);
	return QColor::fromHsvF(h, 0.62f, 1.0f);
}

QFont LcdFont(int pixelSize, bool bold)
{
	QFont f;
	f.setFamilies({"Consolas", "Lucida Console", "DejaVu Sans Mono", "Courier New"});
	f.setStyleHint(QFont::TypeWriter);
	f.setPixelSize(pixelSize);
	f.setBold(bold);
	return f;
}

void DrawGlowText(QPainter &p, const QRectF &r, int flags, const QString &text, const QColor &c, float glow)
{
	if (glow > 0.0f) {
		QColor halo = WithAlpha(c, (int)(40 * glow));
		p.setPen(halo);
		static const QPointF offs[] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-2, 0}, {2, 0}, {0, -2}, {0, 2}};
		for (const QPointF &o : offs)
			p.drawText(r.translated(o), flags, text);
	}
	p.setPen(c);
	p.drawText(r, flags, text);
}
} // namespace SpazzAmp

using namespace SpazzAmp;

/* ======================================================================== */
/* small painted widgets                                                     */

namespace {
class AmpBody : public QWidget {
public:
	using QWidget::QWidget;

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.fillRect(rect(), QColor(0x0b, 0x0a, 0x10));
	}
};

class AmpPanel : public QWidget {
public:
	using QWidget::QWidget;

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		DrawBevelPanel(p, QRectF(rect()));
	}
};

/* "SPAZZamp" strip with the ridged lines from the classic title bars */
class AmpTitle : public QWidget {
public:
	AmpTitle(const QString &text, QWidget *parent) : QWidget(parent), text(text)
	{
		setFixedHeight(16);
		setAttribute(Qt::WA_TransparentForMouseEvents);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);

		QFont f = LcdFont(11, true);
		f.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
		p.setFont(f);
		int tw = p.fontMetrics().horizontalAdvance(text) + 16;
		QRectF r(rect());
		qreal cx = r.center().x();
		QRectF textRect(cx - tw / 2.0, r.top(), tw, r.height());

		QColor a = Accent();
		for (int i = 0; i < 3; i++) {
			qreal y = r.center().y() - 3 + i * 3;
			QLinearGradient g(r.left(), 0, r.right(), 0);
			g.setColorAt(0.0, WithAlpha(a, 20));
			g.setColorAt(0.5, WithAlpha(a, 110));
			g.setColorAt(1.0, WithAlpha(a, 20));
			p.setPen(QPen(QBrush(g), 1.0));
			p.drawLine(QPointF(r.left() + 6, y), QPointF(textRect.left() - 2, y));
			p.drawLine(QPointF(textRect.right() + 2, y), QPointF(r.right() - 6, y));
		}

		DrawGlowText(p, textRect, Qt::AlignCenter, text, a.lighter(115), 1.0f);
	}

private:
	QString text;
};

class PlaylistDelegate : public QStyledItemDelegate {
public:
	using QStyledItemDelegate::QStyledItemDelegate;

	QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return QSize(100, 19); }

	void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const override
	{
		p->save();
		p->setRenderHint(QPainter::Antialiasing);

		QRectF r(opt.rect);
		bool selected = opt.state & QStyle::State_Selected;
		bool current = index.data(CurrentRole).toBool();
		QColor a = Accent();

		if (selected) {
			p->setPen(QPen(WithAlpha(a, 110), 1.0));
			p->setBrush(WithAlpha(a, 45));
			p->drawRoundedRect(r.adjusted(1.5, 0.5, -1.5, -0.5), 3, 3);
		}

		p->setFont(LcdFont(12, current));
		QString num = QString("%1.").arg(index.row() + 1);
		QString title = index.data(Qt::DisplayRole).toString();
		qint64 dur = index.data(DurationRole).toLongLong();
		QString time = dur > 0 ? FormatTime(dur) : QString();

		QFontMetrics fm(p->font());
		int numW = fm.horizontalAdvance("9999.");
		int timeW = fm.horizontalAdvance("88:88:88");
		QRectF numRect(r.left() + 6, r.top(), numW, r.height());
		QRectF timeRect(r.right() - timeW - 8, r.top(), timeW, r.height());
		QRectF titleRect(numRect.right() + 4, r.top(), timeRect.left() - numRect.right() - 10, r.height());
		QString elided = fm.elidedText(title, Qt::ElideRight, (int)titleRect.width());

		if (current) {
			DrawGlowText(*p, numRect, Qt::AlignVCenter | Qt::AlignRight, num, a, 0.8f);
			DrawGlowText(*p, titleRect, Qt::AlignVCenter | Qt::AlignLeft, elided, a, 0.8f);
			DrawGlowText(*p, timeRect, Qt::AlignVCenter | Qt::AlignRight, time, a, 0.8f);
		} else {
			p->setPen(kTextDim);
			p->drawText(numRect, Qt::AlignVCenter | Qt::AlignRight, num);
			p->setPen(selected ? QColor(255, 255, 255) : kText);
			p->drawText(titleRect, Qt::AlignVCenter | Qt::AlignLeft, elided);
			p->setPen(kTextDim);
			p->drawText(timeRect, Qt::AlignVCenter | Qt::AlignRight, time);
		}

		p->restore();
	}
};

/* 7-segment digits for the LCD clock */
void DrawSegments(QPainter &p, const QRectF &r, int mask, const QColor &on)
{
	const qreal t = r.width() * 0.2;
	const qreal hh = r.height() / 2.0;
	const QRectF segs[7] = {
		QRectF(r.left() + t * 0.7, r.top(), r.width() - t * 1.4, t),                  // a
		QRectF(r.right() - t, r.top() + t * 0.7, t, hh - t * 0.9),                    // b
		QRectF(r.right() - t, r.top() + hh + t * 0.2, t, hh - t * 0.9),               // c
		QRectF(r.left() + t * 0.7, r.bottom() - t, r.width() - t * 1.4, t),           // d
		QRectF(r.left(), r.top() + hh + t * 0.2, t, hh - t * 0.9),                    // e
		QRectF(r.left(), r.top() + t * 0.7, t, hh - t * 0.9),                         // f
		QRectF(r.left() + t * 0.7, r.top() + hh - t / 2, r.width() - t * 1.4, t),    // g
	};

	p.setPen(Qt::NoPen);
	for (int i = 0; i < 7; i++) {
		bool lit = mask & (1 << i);
		if (lit) {
			p.setBrush(WithAlpha(on, 45));
			p.drawRoundedRect(segs[i].adjusted(-t * 0.6, -t * 0.6, t * 0.6, t * 0.6), t, t);
			p.setBrush(on);
		} else {
			p.setBrush(WithAlpha(on, 16));
		}
		p.drawRoundedRect(segs[i], t / 2, t / 2);
	}
}

int DigitMask(QChar c)
{
	static const int masks[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
	if (c.isDigit())
		return masks[c.digitValue()];
	if (c == '-')
		return 0x40;
	return 0;
}
} // namespace

/* ======================================================================== */
/* AmpButton                                                                 */

AmpButton::AmpButton(Glyph glyph_, const QString &text, QWidget *parent) : QAbstractButton(parent), glyph(glyph_)
{
	setText(text);
	setCursor(Qt::PointingHandCursor);
	setFocusPolicy(Qt::NoFocus);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

QSize AmpButton::sizeHint() const
{
	if (glyph == Glyph::Text) {
		QFontMetrics fm(LcdFont(10, true));
		return QSize(fm.horizontalAdvance(text()) + (hasLed ? 26 : 16), 22);
	}
	if (glyph == Glyph::Repeat)
		return QSize(40, 22);
	return QSize(34, 26);
}

void AmpButton::enterEvent(QEnterEvent *)
{
	hovered = true;
	update();
}

void AmpButton::leaveEvent(QEvent *)
{
	hovered = false;
	update();
}

void AmpButton::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);

	QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
	bool down = isDown();
	QColor a = Accent();

	QLinearGradient g(r.topLeft(), r.bottomLeft());
	if (down) {
		g.setColorAt(0.0, QColor(0x16, 0x15, 0x1e));
		g.setColorAt(1.0, QColor(0x2a, 0x28, 0x38));
	} else {
		g.setColorAt(0.0, hovered ? QColor(0x4a, 0x47, 0x63) : QColor(0x3c, 0x3a, 0x52));
		g.setColorAt(0.5, hovered ? QColor(0x33, 0x31, 0x45) : QColor(0x2c, 0x2a, 0x3c));
		g.setColorAt(1.0, QColor(0x20, 0x1f, 0x2b));
	}

	p.setPen(QPen(hovered ? WithAlpha(a, 160) : kEdgeDark, 1.0));
	p.setBrush(g);
	p.drawRoundedRect(r, 4, 4);

	if (!down) {
		p.setPen(QPen(QColor(255, 255, 255, 40), 1.0));
		p.drawLine(QPointF(r.left() + 4, r.top() + 1.5), QPointF(r.right() - 4, r.top() + 1.5));
	}

	QColor ink = (hovered || isChecked()) ? a : kText;
	QPointF c = r.center() + (down ? QPointF(0.5, 0.5) : QPointF());
	qreal s = std::min(r.height() * 0.38, r.width() * 0.34);

	QPainterPath path;
	auto tri = [&](qreal cx, qreal cy, qreal size, bool right) {
		QPolygonF poly;
		if (right)
			poly << QPointF(cx - size * 0.45, cy - size * 0.55) << QPointF(cx + size * 0.55, cy)
			     << QPointF(cx - size * 0.45, cy + size * 0.55);
		else
			poly << QPointF(cx + size * 0.45, cy - size * 0.55) << QPointF(cx - size * 0.55, cy)
			     << QPointF(cx + size * 0.45, cy + size * 0.55);
		path.addPolygon(poly);
		path.closeSubpath();
	};

	switch (glyph) {
	case Glyph::Play:
		tri(c.x(), c.y(), s * 1.5, true);
		break;
	case Glyph::Pause:
		path.addRoundedRect(QRectF(c.x() - s * 0.6, c.y() - s * 0.75, s * 0.42, s * 1.5), 1, 1);
		path.addRoundedRect(QRectF(c.x() + s * 0.18, c.y() - s * 0.75, s * 0.42, s * 1.5), 1, 1);
		break;
	case Glyph::Stop:
		path.addRoundedRect(QRectF(c.x() - s * 0.68, c.y() - s * 0.68, s * 1.36, s * 1.36), 1.5, 1.5);
		break;
	case Glyph::Prev:
		path.addRect(QRectF(c.x() - s * 0.95, c.y() - s * 0.7, s * 0.28, s * 1.4));
		tri(c.x() + s * 0.15, c.y(), s * 1.25, false);
		break;
	case Glyph::Next:
		path.addRect(QRectF(c.x() + s * 0.67, c.y() - s * 0.7, s * 0.28, s * 1.4));
		tri(c.x() - s * 0.15, c.y(), s * 1.25, true);
		break;
	case Glyph::Eject: {
		QPolygonF up;
		up << QPointF(c.x() - s * 0.85, c.y() + s * 0.1) << QPointF(c.x(), c.y() - s * 0.8)
		   << QPointF(c.x() + s * 0.85, c.y() + s * 0.1);
		path.addPolygon(up);
		path.closeSubpath();
		path.addRect(QRectF(c.x() - s * 0.85, c.y() + s * 0.4, s * 1.7, s * 0.32));
		break;
	}
	default:
		break;
	}

	qreal textLeft = r.left() + 6;

	if (hasLed) {
		QRectF led(r.left() + 6, c.y() - 3, 6, 6);
		bool lit = isChecked();
		if (lit) {
			p.setPen(Qt::NoPen);
			p.setBrush(WithAlpha(a, 70));
			p.drawEllipse(led.adjusted(-3, -3, 3, 3));
		}
		p.setPen(QPen(QColor(0, 0, 0), 1.0));
		p.setBrush(lit ? a : QColor(0x24, 0x23, 0x30));
		p.drawRoundedRect(led, 1.5, 1.5);
		textLeft = led.right() + 5;
	}

	if (glyph == Glyph::Repeat) {
		/* loop arrow */
		QRectF loop(textLeft + 2, c.y() - 5, r.right() - textLeft - 10, 10);
		QPainterPath arc;
		arc.addRoundedRect(loop, 4, 4);
		p.setBrush(Qt::NoBrush);
		if (hovered || isChecked()) {
			p.setPen(QPen(WithAlpha(ink, 60), 3.5));
			p.drawPath(arc);
		}
		p.setPen(QPen(ink, 1.6));
		p.drawPath(arc);
		QPolygonF head;
		head << QPointF(loop.right() - 3, loop.top() - 3) << QPointF(loop.right() + 1, loop.top())
		     << QPointF(loop.right() - 3, loop.top() + 3);
		p.setBrush(ink);
		p.setPen(Qt::NoPen);
		p.drawPolygon(head);
		if (repeatOne) {
			p.setFont(LcdFont(9, true));
			p.setPen(ink);
			p.drawText(loop, Qt::AlignCenter, "1");
		}
		return;
	}

	if (glyph == Glyph::Text) {
		p.setFont(LcdFont(10, true));
		QRectF tr(textLeft, r.top(), r.right() - textLeft - 4, r.height());
		if (hovered || isChecked())
			DrawGlowText(p, tr, Qt::AlignCenter, text(), ink, 0.7f);
		else {
			p.setPen(ink);
			p.drawText(tr, Qt::AlignCenter, text());
		}
		return;
	}

	if (hovered) {
		p.setPen(QPen(WithAlpha(ink, 55), 3.0));
		p.setBrush(Qt::NoBrush);
		p.drawPath(path);
	}
	p.setPen(Qt::NoPen);
	p.setBrush(ink);
	p.drawPath(path);
}

/* ======================================================================== */
/* AmpSlider                                                                 */

AmpSlider::AmpSlider(Kind kind_, QWidget *parent) : QWidget(parent), kind(kind_)
{
	setCursor(Qt::PointingHandCursor);
	setSizePolicy(kind == Kind::Seek ? QSizePolicy::Expanding : QSizePolicy::Expanding, QSizePolicy::Fixed);
	if (kind == Kind::Balance)
		value = 0.5f;
}

QSize AmpSlider::sizeHint() const
{
	switch (kind) {
	case Kind::Seek:
		return QSize(240, 16);
	case Kind::Volume:
		return QSize(110, 14);
	default:
		return QSize(50, 14);
	}
}

void AmpSlider::SetValue(float v)
{
	v = std::clamp(v, 0.0f, 1.0f);
	if (std::fabs(v - value) > 0.0005f) {
		value = v;
		update();
	}
}

QRectF AmpSlider::Track() const
{
	qreal thumbW = kind == Kind::Seek ? 28.0 : 14.0;
	return QRectF(thumbW / 2, 0, width() - thumbW, height());
}

float AmpSlider::ValueAt(qreal x) const
{
	QRectF t = Track();
	if (t.width() <= 0)
		return 0.0f;
	return std::clamp((float)((x - t.left()) / t.width()), 0.0f, 1.0f);
}

void AmpSlider::mousePressEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton)
		return;
	if (kind == Kind::Balance && e->modifiers() & Qt::ControlModifier) {
		value = 0.5f;
	} else {
		dragging = true;
		value = ValueAt(e->position().x());
	}
	update();
	emit Moved(value);
}

void AmpSlider::mouseMoveEvent(QMouseEvent *e)
{
	if (!dragging)
		return;
	float v = ValueAt(e->position().x());

	/* balance snaps to the middle, like the original */
	if (kind == Kind::Balance && std::fabs(v - 0.5f) < 0.06f)
		v = 0.5f;

	value = v;
	update();
	emit Moved(value);
}

void AmpSlider::mouseReleaseEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton || !dragging)
		return;
	dragging = false;
	update();
	emit Released(value);
}

void AmpSlider::wheelEvent(QWheelEvent *e)
{
	float step = kind == Kind::Seek ? 0.02f : 0.03f;
	float v = std::clamp(value + (e->angleDelta().y() > 0 ? step : -step), 0.0f, 1.0f);
	value = v;
	update();
	emit Moved(value);
	emit Released(value);
	e->accept();
}

void AmpSlider::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);

	QRectF t = Track();
	QColor a = Accent(kind == Kind::Seek ? 0.0f : 0.08f);
	qreal grooveH = kind == Kind::Seek ? 6.0 : 5.0;
	QRectF groove(t.left() - 2, height() / 2.0 - grooveH / 2, t.width() + 4, grooveH);

	/* inset groove */
	p.setPen(QPen(QColor(0, 0, 0), 1.0));
	p.setBrush(kLcd);
	p.drawRoundedRect(groove, grooveH / 2, grooveH / 2);

	/* lit fill */
	qreal x = t.left() + t.width() * value;
	QRectF fill;
	if (kind == Kind::Balance) {
		qreal mid = t.center().x();
		fill = QRectF(std::min(mid, x), groove.top() + 1, std::fabs(x - mid), grooveH - 2);
	} else {
		fill = QRectF(groove.left() + 1, groove.top() + 1, x - groove.left() - 1, grooveH - 2);
	}

	if (fill.width() > 0.5) {
		QLinearGradient g(groove.topLeft(), groove.topRight());
		g.setColorAt(0.0, a.darker(170));
		g.setColorAt(1.0, a);
		p.setPen(Qt::NoPen);
		p.setBrush(WithAlpha(a, 50));
		p.drawRoundedRect(fill.adjusted(-1, -2, 1, 2), grooveH / 2, grooveH / 2);
		p.setBrush(g);
		p.drawRoundedRect(fill, (grooveH - 2) / 2, (grooveH - 2) / 2);
	}

	if (kind == Kind::Balance) {
		p.setPen(QPen(WithAlpha(kText, 90), 1.0));
		p.drawLine(QPointF(t.center().x(), groove.top() - 2), QPointF(t.center().x(), groove.bottom() + 2));
	}

	/* thumb: brushed gold for seek like the classic, silver for the rest */
	qreal tw = kind == Kind::Seek ? 28.0 : 14.0;
	qreal th = kind == Kind::Seek ? 12.0 : 11.0;
	QRectF thumb(x - tw / 2, height() / 2.0 - th / 2, tw, th);
	QLinearGradient tg(thumb.topLeft(), thumb.bottomLeft());
	if (kind == Kind::Seek) {
		tg.setColorAt(0.0, QColor(0xf4, 0xdc, 0x96));
		tg.setColorAt(0.45, QColor(0xc9, 0xa1, 0x48));
		tg.setColorAt(1.0, QColor(0x7c, 0x5d, 0x1f));
	} else {
		tg.setColorAt(0.0, QColor(0xf0, 0xef, 0xf7));
		tg.setColorAt(0.5, QColor(0xb6, 0xb3, 0xc8));
		tg.setColorAt(1.0, QColor(0x6c, 0x69, 0x80));
	}
	if (dragging) {
		p.setPen(Qt::NoPen);
		p.setBrush(WithAlpha(a, 70));
		p.drawRoundedRect(thumb.adjusted(-3, -3, 3, 3), 5, 5);
	}
	p.setPen(QPen(QColor(0x18, 0x12, 0x06), 1.0));
	p.setBrush(tg);
	p.drawRoundedRect(thumb, 3.5, 3.5);

	/* grip lines */
	p.setPen(QPen(QColor(0, 0, 0, 80), 1.0));
	int lines = kind == Kind::Seek ? 3 : 1;
	for (int i = 0; i < lines; i++) {
		qreal lx = thumb.center().x() + (i - (lines - 1) / 2.0) * 3.0;
		p.drawLine(QPointF(lx, thumb.top() + 3), QPointF(lx, thumb.bottom() - 3));
	}
}

/* ======================================================================== */
/* AmpDisplay: LCD clock, spectrum, scrolling title, kbps/kHz               */

AmpDisplay::AmpDisplay(SpazzMusicPlayer *player_, QWidget *parent) : QWidget(parent), player(player_)
{
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setToolTip(QTStr("SPAZZ.Music.TimeTooltip"));
}

QSize AmpDisplay::sizeHint() const
{
	return QSize(380, 80);
}

QSize AmpDisplay::minimumSizeHint() const
{
	return QSize(260, 80);
}

void AmpDisplay::Tick(float dt)
{
	if (player)
		player->UpdateSpectrum(dt, bars, 19, peaks);
	marquee += dt * 28.0f;
	update();
}

bool AmpDisplay::IsAnimating() const
{
	for (int i = 0; i < 19; i++) {
		if (bars[i] > 0.002f || peaks[i] > 0.002f)
			return true;
	}
	return false;
}

void AmpDisplay::mousePressEvent(QMouseEvent *e)
{
	if (e->position().x() < width() * 0.42) {
		showRemaining = !showRemaining;
		update();
	}
}

void AmpDisplay::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);

	QColor a = Accent();
	QRectF r(rect());
	qreal lcdW = std::min(160.0, r.width() * 0.42);
	QRectF lcd(r.left(), r.top(), lcdW, r.height());
	DrawLcdWell(p, lcd, 4);

	obs_media_state state = player ? player->State() : OBS_MEDIA_STATE_NONE;
	bool playing = state == OBS_MEDIA_STATE_PLAYING;
	bool paused = state == OBS_MEDIA_STATE_PAUSED;
	bool active = playing || paused;

	/* play state glyph */
	QRectF glyph(lcd.left() + 8, lcd.top() + 9, 9, 11);
	p.setPen(Qt::NoPen);
	if (playing) {
		QPolygonF tri;
		tri << glyph.topLeft() << QPointF(glyph.right(), glyph.center().y()) << glyph.bottomLeft();
		p.setBrush(WithAlpha(a, 60));
		p.drawEllipse(glyph.center(), 8, 8);
		p.setBrush(a);
		p.drawPolygon(tri);
	} else if (paused) {
		/* blink while paused */
		bool on = std::fmod(marquee / 28.0f, 1.0f) < 0.6f;
		p.setBrush(on ? a : WithAlpha(a, 50));
		p.drawRect(QRectF(glyph.left(), glyph.top(), 3, glyph.height()));
		p.drawRect(QRectF(glyph.left() + 5, glyph.top(), 3, glyph.height()));
	} else {
		p.setBrush(WithAlpha(a, 90));
		p.drawRect(QRectF(glyph.left(), glyph.top() + 1, 9, 9));
	}

	/* clock */
	qint64 pos = player ? player->PositionMs() : 0;
	qint64 dur = player ? player->DurationMs() : 0;
	qint64 shown = (showRemaining && dur > 0) ? dur - pos : pos;
	QString text = active ? FormatTime(shown, true) : QString("00:00");
	if (showRemaining && active && dur > 0)
		text.prepend('-');
	if (text.length() > 6)
		text = text.right(6);

	qreal dh = 24, dw = 13, gap = 4;
	qreal x = lcd.right() - 10;
	qreal y = lcd.top() + 7;
	for (int i = text.length() - 1; i >= 0; i--) {
		QChar ch = text[i];
		if (ch == ':') {
			x -= 6;
			p.setPen(Qt::NoPen);
			p.setBrush(active ? a : WithAlpha(a, 40));
			p.drawEllipse(QPointF(x + 2, y + dh * 0.32), 1.6, 1.6);
			p.drawEllipse(QPointF(x + 2, y + dh * 0.72), 1.6, 1.6);
			x -= 1;
			continue;
		}
		x -= dw;
		DrawSegments(p, QRectF(x, y, dw, dh), DigitMask(ch), a);
		x -= gap;
	}

	/* mini spectrum */
	QRectF spec(lcd.left() + 8, lcd.top() + 38, lcd.width() - 16, lcd.height() - 46);
	const int n = 19;
	qreal bw = spec.width() / n;
	for (int i = 0; i < n; i++) {
		qreal bh = std::max(1.0, (double)bars[i] * spec.height());
		QRectF bar(spec.left() + i * bw + 0.5, spec.bottom() - bh, bw - 1.5, bh);
		QLinearGradient g(spec.bottomLeft(), spec.topLeft());
		QColor c = Accent(i / (float)n * 0.25f);
		g.setColorAt(0.0, c.darker(220));
		g.setColorAt(0.6, c);
		g.setColorAt(1.0, c.lighter(150));
		p.setPen(Qt::NoPen);
		p.setBrush(g);
		p.drawRect(bar);

		qreal py = spec.bottom() - peaks[i] * spec.height();
		p.setBrush(c.lighter(160));
		p.drawRect(QRectF(bar.left(), py - 1, bar.width(), 1.2));
	}

	/* right column: marquee + info */
	QRectF right(lcd.right() + 8, r.top(), r.right() - lcd.right() - 8, r.height());
	QRectF marqueeRect(right.left(), right.top(), right.width(), 22);
	DrawLcdWell(p, marqueeRect, 3);

	QString title;
	if (player) {
		int idx = player->CurrentIndex();
		if (idx >= 0) {
			const auto &t = player->Tracks()[idx];
			title = QString("%1. %2").arg(idx + 1).arg(t.title);
			qint64 d = t.durationMs > 0 ? t.durationMs : dur;
			if (d > 0)
				title += QString(" (%1)").arg(FormatTime(d));
		}
	}
	if (title.isEmpty())
		title = QTStr("SPAZZ.Music.Idle");

	QFont mf = LcdFont(12, true);
	p.setFont(mf);
	QFontMetricsF fm(mf);
	QRectF inner = marqueeRect.adjusted(6, 0, -6, 0);
	p.save();
	p.setClipRect(inner);
	qreal tw = fm.horizontalAdvance(title);
	if (tw <= inner.width()) {
		DrawGlowText(p, inner, Qt::AlignVCenter | Qt::AlignLeft, title, a, 0.9f);
	} else {
		QString loop = title + "  ***  ";
		qreal lw = fm.horizontalAdvance(loop);
		qreal off = std::fmod((double)marquee, lw);
		for (qreal sx = inner.left() - off; sx < inner.right(); sx += lw)
			DrawGlowText(p, QRectF(sx, inner.top(), lw + 4, inner.height()), Qt::AlignVCenter | Qt::AlignLeft,
				     loop, a, 0.9f);
	}
	p.restore();

	/* kbps / kHz / mono / stereo */
	QRectF info(right.left(), marqueeRect.bottom() + 7, right.width(), 16);
	struct obs_audio_info oai = {};
	bool haveAudio = obs_get_audio_info(&oai);
	int khz = haveAudio ? (int)std::lround(oai.samples_per_sec / 1000.0) : 48;
	int kbps = player ? player->EstimatedKbps() : 0;

	auto box = [&](qreal left, qreal w, const QString &value, const QString &unit) {
		QRectF b(left, info.top(), w, info.height());
		DrawLcdWell(p, b, 2);
		p.setFont(LcdFont(11, true));
		if (active && !value.isEmpty())
			DrawGlowText(p, b.adjusted(0, 0, -3, 0), Qt::AlignVCenter | Qt::AlignRight, value, a, 0.7f);
		p.setFont(LcdFont(10, false));
		p.setPen(kTextDim);
		QRectF u(b.right() + 4, b.top(), 34, b.height());
		p.drawText(u, Qt::AlignVCenter | Qt::AlignLeft, unit);
		return u.left() + QFontMetricsF(LcdFont(10, false)).horizontalAdvance(unit) + 8;
	};

	qreal cx = info.left();
	cx = box(cx, 30, kbps > 0 ? QString::number(std::min(kbps, 9999)) : QString(), "kbps");
	cx = box(cx, 22, QString::number(khz), "kHz");

	bool stereo = haveAudio && oai.speakers != SPEAKERS_MONO;
	p.setFont(LcdFont(10, true));
	QRectF monoR(cx, info.top(), 30, info.height());
	QRectF stereoR(monoR.right() + 2, info.top(), 42, info.height());
	if (monoR.right() < right.right()) {
		if (active && !stereo)
			DrawGlowText(p, monoR, Qt::AlignCenter, "mono", a, 0.7f);
		else {
			p.setPen(WithAlpha(kTextDim, 140));
			p.drawText(monoR, Qt::AlignCenter, "mono");
		}
	}
	if (stereoR.right() < right.right() + 4) {
		if (active && stereo)
			DrawGlowText(p, stereoR, Qt::AlignCenter, "stereo", a, 0.7f);
		else {
			p.setPen(WithAlpha(kTextDim, 140));
			p.drawText(stereoR, Qt::AlignCenter, "stereo");
		}
	}

	/* little vertical brand stamp, a nod to the old clutterbar */
	QRectF stamp(right.left(), info.bottom() + 6, right.width(), r.bottom() - info.bottom() - 6);
	if (stamp.height() > 10) {
		p.setFont(LcdFont(9, true));
		p.setPen(WithAlpha(kTextDim, 160));
		int count = player ? player->Tracks().size() : 0;
		p.drawText(stamp, Qt::AlignLeft | Qt::AlignVCenter,
			   count ? QTStr("SPAZZ.Music.TrackOf").arg(player->CurrentIndex() + 1).arg(count)
				 : QTStr("SPAZZ.Music.DropHint"));
	}
}

/* ======================================================================== */
/* AmpPlaylist                                                               */

AmpPlaylist::AmpPlaylist(QWidget *parent) : QListWidget(parent)
{
	setObjectName("spazzPlaylist");
	setItemDelegate(new PlaylistDelegate(this));
	setSelectionMode(QAbstractItemView::ExtendedSelection);
	setDragDropMode(QAbstractItemView::InternalMove);
	setDefaultDropAction(Qt::MoveAction);
	setAcceptDrops(true);
	setDropIndicatorShown(true);
	setUniformItemSizes(true);
	setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	setFrameShape(QFrame::NoFrame);
	setStyleSheet("#spazzPlaylist { background: #05060a; border: 1px solid #000000; border-radius: 3px; }"
		      "#spazzPlaylist::item { background: transparent; border: none; }");
}

static QStringList LocalPaths(const QMimeData *mime)
{
	QStringList paths;
	if (!mime || !mime->hasUrls())
		return paths;
	for (const QUrl &url : mime->urls()) {
		if (url.isLocalFile())
			paths.push_back(url.toLocalFile());
	}
	return paths;
}

void AmpPlaylist::dragEnterEvent(QDragEnterEvent *e)
{
	if (!LocalPaths(e->mimeData()).isEmpty()) {
		e->setDropAction(Qt::CopyAction);
		e->accept();
		return;
	}
	QListWidget::dragEnterEvent(e);
}

void AmpPlaylist::dragMoveEvent(QDragMoveEvent *e)
{
	if (!LocalPaths(e->mimeData()).isEmpty()) {
		e->setDropAction(Qt::CopyAction);
		e->accept();
		return;
	}
	QListWidget::dragMoveEvent(e);
}

void AmpPlaylist::dropEvent(QDropEvent *e)
{
	QStringList paths = LocalPaths(e->mimeData());
	if (!paths.isEmpty()) {
		QModelIndex at = indexAt(e->position().toPoint());
		int row = at.isValid() ? at.row() : count();
		e->setDropAction(Qt::CopyAction);
		e->accept();
		emit FilesDropped(paths, row);
		return;
	}

	QListWidget::dropEvent(e);
	emit OrderChanged();
}

void AmpPlaylist::keyPressEvent(QKeyEvent *e)
{
	switch (e->key()) {
	case Qt::Key_Delete:
	case Qt::Key_Backspace:
		emit DeletePressed();
		return;
	case Qt::Key_Return:
	case Qt::Key_Enter:
		emit ActivatePressed();
		return;
	default:
		QListWidget::keyPressEvent(e);
	}
}

void AmpPlaylist::paintEvent(QPaintEvent *e)
{
	QListWidget::paintEvent(e);

	if (count() == 0) {
		QPainter p(viewport());
		p.setRenderHint(QPainter::Antialiasing);
		QRectF r(viewport()->rect());
		QColor a = Accent();

		QRectF box = r.adjusted(14, 14, -14, -14);
		QPen dash(WithAlpha(a, 90), 1.2, Qt::DashLine);
		p.setPen(dash);
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(box, 6, 6);

		p.setFont(LcdFont(13, true));
		DrawGlowText(p, box, Qt::AlignCenter, QTStr("SPAZZ.Music.EmptyList"), a, 0.8f);
	}
}

/* ======================================================================== */
/* SpazzMusicDock                                                            */

SpazzMusicDock::SpazzMusicDock(SpazzMusicPlayer *player_, QWidget *parent) : OBSDock(parent), player(player_)
{
	setObjectName("spazzMusicDock");
	setWindowTitle(QTStr("SPAZZ.Music.DockTitle"));
	setAcceptDrops(true);
	setMinimumSize(280, 220);

	body = new AmpBody(this);
	mainLayout = new QBoxLayout(QBoxLayout::LeftToRight, body);
	mainLayout->setContentsMargins(6, 6, 6, 6);
	mainLayout->setSpacing(6);

	/* --- player panel --- */
	playerPanel = new AmpPanel(body);
	auto *pv = new QVBoxLayout(playerPanel);
	pv->setContentsMargins(8, 4, 8, 8);
	pv->setSpacing(6);

	pv->addWidget(new AmpTitle("SPAZZamp", playerPanel));

	display = new AmpDisplay(player, playerPanel);
	pv->addWidget(display);

	auto *mixRow = new QHBoxLayout();
	mixRow->setSpacing(8);
	auto *volLabel = new QLabel("VOL", playerPanel);
	auto *balLabel = new QLabel("BAL", playerPanel);
	for (QLabel *l : {volLabel, balLabel}) {
		l->setFont(LcdFont(10, true));
		l->setStyleSheet(kLabelStyle);
	}
	volume = new AmpSlider(AmpSlider::Kind::Volume, playerPanel);
	volume->setToolTip(QTStr("SPAZZ.Music.VolumeTooltip"));
	balance = new AmpSlider(AmpSlider::Kind::Balance, playerPanel);
	balance->setToolTip(QTStr("SPAZZ.Music.BalanceTooltip"));
	mixRow->addWidget(volLabel);
	mixRow->addWidget(volume, 3);
	mixRow->addSpacing(4);
	mixRow->addWidget(balLabel);
	mixRow->addWidget(balance, 1);
	pv->addLayout(mixRow);

	seek = new AmpSlider(AmpSlider::Kind::Seek, playerPanel);
	seek->setToolTip(QTStr("SPAZZ.Music.SeekTooltip"));
	pv->addWidget(seek);

	auto *transport = new QHBoxLayout();
	transport->setSpacing(3);
	auto *prevBtn = new AmpButton(AmpButton::Glyph::Prev, QString(), playerPanel);
	playBtn = new AmpButton(AmpButton::Glyph::Play, QString(), playerPanel);
	pauseBtn = new AmpButton(AmpButton::Glyph::Pause, QString(), playerPanel);
	auto *stopBtn = new AmpButton(AmpButton::Glyph::Stop, QString(), playerPanel);
	auto *nextBtn = new AmpButton(AmpButton::Glyph::Next, QString(), playerPanel);
	auto *ejectBtn = new AmpButton(AmpButton::Glyph::Eject, QString(), playerPanel);
	prevBtn->setToolTip(QTStr("SPAZZ.Music.Prev"));
	playBtn->setToolTip(QTStr("SPAZZ.Music.Play"));
	pauseBtn->setToolTip(QTStr("SPAZZ.Music.Pause"));
	stopBtn->setToolTip(QTStr("SPAZZ.Music.Stop"));
	nextBtn->setToolTip(QTStr("SPAZZ.Music.Next"));
	ejectBtn->setToolTip(QTStr("SPAZZ.Music.Open"));

	shuffleBtn = new AmpButton(AmpButton::Glyph::Text, "SHUFFLE", playerPanel);
	shuffleBtn->SetLed(true);
	shuffleBtn->setCheckable(true);
	shuffleBtn->setToolTip(QTStr("SPAZZ.Music.Shuffle"));
	repeatBtn = new AmpButton(AmpButton::Glyph::Repeat, QString(), playerPanel);
	repeatBtn->SetLed(true);
	repeatBtn->setCheckable(true);
	repeatBtn->setToolTip(QTStr("SPAZZ.Music.Repeat"));

	for (QWidget *w : {(QWidget *)prevBtn, (QWidget *)playBtn, (QWidget *)pauseBtn, (QWidget *)stopBtn,
			   (QWidget *)nextBtn})
		transport->addWidget(w);
	transport->addSpacing(6);
	transport->addWidget(ejectBtn);
	transport->addStretch(1);
	transport->addWidget(shuffleBtn);
	transport->addWidget(repeatBtn);
	pv->addLayout(transport);
	pv->addStretch(1);

	/* --- playlist panel --- */
	listPanel = new AmpPanel(body);
	auto *lv = new QVBoxLayout(listPanel);
	lv->setContentsMargins(8, 4, 8, 8);
	lv->setSpacing(6);
	lv->addWidget(new AmpTitle(QTStr("SPAZZ.Music.PlaylistTitle"), listPanel));

	list = new AmpPlaylist(listPanel);
	lv->addWidget(list, 1);

	auto *footer = new QHBoxLayout();
	footer->setSpacing(3);
	auto *addBtn = new AmpButton(AmpButton::Glyph::Text, "+ ADD", listPanel);
	auto *remBtn = new AmpButton(AmpButton::Glyph::Text, "- REM", listPanel);
	auto *clrBtn = new AmpButton(AmpButton::Glyph::Text, "CLEAR", listPanel);
	addBtn->setToolTip(QTStr("SPAZZ.Music.AddTooltip"));
	remBtn->setToolTip(QTStr("SPAZZ.Music.RemoveTooltip"));
	clrBtn->setToolTip(QTStr("SPAZZ.Music.ClearTooltip"));
	totals = new QLabel(listPanel);
	totals->setFont(LcdFont(11, true));
	totals->setStyleSheet(kLabelStyle);
	footer->addWidget(addBtn);
	footer->addWidget(remBtn);
	footer->addWidget(clrBtn);
	footer->addStretch(1);
	footer->addWidget(totals);
	lv->addLayout(footer);

	mainLayout->addWidget(playerPanel, 0);
	mainLayout->addWidget(listPanel, 1);
	setWidget(body);

	/* --- wiring --- */
	connect(prevBtn, &QAbstractButton::clicked, this, [this]() {
		if (player)
			player->Previous();
	});
	connect(playBtn, &QAbstractButton::clicked, this, [this]() {
		if (!player)
			return;
		/* like the original: Play while playing starts the song over */
		if (player->State() == OBS_MEDIA_STATE_PLAYING)
			player->Seek(0);
		else
			player->Play();
	});
	connect(pauseBtn, &QAbstractButton::clicked, this, [this]() {
		if (player)
			player->TogglePlayPause();
	});
	connect(stopBtn, &QAbstractButton::clicked, this, [this]() {
		if (player)
			player->Stop();
	});
	connect(nextBtn, &QAbstractButton::clicked, this, [this]() {
		if (player)
			player->Next();
	});
	connect(ejectBtn, &QAbstractButton::clicked, this, &SpazzMusicDock::AddFilesDialog);
	connect(shuffleBtn, &QAbstractButton::clicked, this, [this](bool on) {
		if (player)
			player->SetShuffle(on);
	});
	connect(repeatBtn, &QAbstractButton::clicked, this, [this]() {
		if (!player)
			return;
		using R = SpazzMusicPlayer::Repeat;
		R next = player->RepeatMode() == R::Off ? R::All : (player->RepeatMode() == R::All ? R::One : R::Off);
		player->SetRepeat(next);
	});

	connect(volume, &AmpSlider::Moved, this, [this](float v) {
		if (player)
			player->SetVolume(v * v * v);
	});
	connect(balance, &AmpSlider::Moved, this, [this](float v) {
		if (player)
			player->SetBalance(v);
	});
	connect(seek, &AmpSlider::Released, this, [this](float v) {
		if (player && player->DurationMs() > 0)
			player->Seek((qint64)(v * player->DurationMs()));
	});

	auto *addMenu = new QMenu(this);
	addMenu->addAction(QTStr("SPAZZ.Music.AddFiles"), this, &SpazzMusicDock::AddFilesDialog);
	addMenu->addAction(QTStr("SPAZZ.Music.AddFolder"), this, &SpazzMusicDock::AddFolderDialog);
	connect(addBtn, &QAbstractButton::clicked, this,
		[addBtn, addMenu]() { addMenu->exec(addBtn->mapToGlobal(QPoint(0, addBtn->height()))); });
	connect(remBtn, &QAbstractButton::clicked, this, [this]() {
		if (player)
			player->RemoveIds(SelectedIds());
	});
	connect(clrBtn, &QAbstractButton::clicked, this, [this]() {
		if (player)
			player->Clear();
	});

	connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
		if (player)
			player->PlayIndex(list->row(item));
	});
	connect(list, &AmpPlaylist::ActivatePressed, this, [this]() {
		if (player && list->currentRow() >= 0)
			player->PlayIndex(list->currentRow());
	});
	connect(list, &AmpPlaylist::DeletePressed, this, [this]() {
		if (player)
			player->RemoveIds(SelectedIds());
	});
	connect(list, &AmpPlaylist::FilesDropped, this, [this](const QStringList &paths, int row) {
		if (player)
			player->AddPaths(paths, row);
	});
	connect(
		list, &AmpPlaylist::OrderChanged, this,
		[this]() {
			if (!player)
				return;
			QList<quint64> ids;
			for (int i = 0; i < list->count(); i++)
				ids.push_back(list->item(i)->data(IdRole).toULongLong());
			player->SetOrder(ids);
		},
		Qt::QueuedConnection);

	list->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(list, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
		if (!player)
			return;
		QMenu menu(this);
		QListWidgetItem *item = list->itemAt(pos);
		if (item) {
			int row = list->row(item);
			menu.addAction(QTStr("SPAZZ.Music.Play"), this, [this, row]() {
				if (player)
					player->PlayIndex(row);
			});
			menu.addAction(QTStr("SPAZZ.Music.RemoveTooltip"), this, [this]() {
				if (player)
					player->RemoveIds(SelectedIds());
			});
			menu.addSeparator();
		}
		menu.addAction(QTStr("SPAZZ.Music.AddFiles"), this, &SpazzMusicDock::AddFilesDialog);
		menu.addAction(QTStr("SPAZZ.Music.AddFolder"), this, &SpazzMusicDock::AddFolderDialog);
		menu.addSeparator();
		menu.addAction(QTStr("SPAZZ.Music.ClearTooltip"), this, [this]() {
			if (player)
				player->Clear();
		});
		menu.exec(list->viewport()->mapToGlobal(pos));
	});

	if (player) {
		connect(player, &SpazzMusicPlayer::PlaylistChanged, this, &SpazzMusicDock::RebuildList);
		connect(player, &SpazzMusicPlayer::TrackUpdated, this, &SpazzMusicDock::UpdateTrack);
		connect(player, &SpazzMusicPlayer::CurrentChanged, this, &SpazzMusicDock::SyncCurrent);
		connect(player, &SpazzMusicPlayer::StateChanged, this, &SpazzMusicDock::SyncState);
		connect(player, &SpazzMusicPlayer::SourceChanged, this, &SpazzMusicDock::SyncState);
		connect(player, &SpazzMusicPlayer::VolumeChanged, this, [this](float mul) {
			if (!volume->IsDragging())
				volume->SetValue(std::cbrt(std::max(mul, 0.0f)));
		});
		volume->SetValue(std::cbrt(std::max(player->Volume(), 0.0f)));
		balance->SetValue(player->Balance());
	}

	/* everything that draws with the cycling accent colour */
	for (QWidget *w : body->findChildren<QWidget *>()) {
		if (qobject_cast<AmpButton *>(w) || qobject_cast<AmpSlider *>(w) || dynamic_cast<AmpTitle *>(w))
			accentWidgets.push_back(w);
	}

	frameTimer.setInterval(33);
	connect(&frameTimer, &QTimer::timeout, this, &SpazzMusicDock::Frame);
	clock.start();

	RebuildList();
	SyncState();
}

void SpazzMusicDock::showEvent(QShowEvent *e)
{
	OBSDock::showEvent(e);
	clock.restart();
	frameTimer.start();
}

void SpazzMusicDock::hideEvent(QHideEvent *e)
{
	OBSDock::hideEvent(e);
	frameTimer.stop();
}

void SpazzMusicDock::resizeEvent(QResizeEvent *e)
{
	OBSDock::resizeEvent(e);

	/* side by side when there's room, stacked in a narrow dock */
	bool wide = width() >= 660;
	mainLayout->setDirection(wide ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
	playerPanel->setMaximumWidth(wide ? 430 : QWIDGETSIZE_MAX);
	playerPanel->setSizePolicy(QSizePolicy::Preferred, wide ? QSizePolicy::Preferred : QSizePolicy::Maximum);
}

void SpazzMusicDock::dragEnterEvent(QDragEnterEvent *e)
{
	if (!LocalPaths(e->mimeData()).isEmpty()) {
		e->setDropAction(Qt::CopyAction);
		e->accept();
	}
}

void SpazzMusicDock::dropEvent(QDropEvent *e)
{
	QStringList paths = LocalPaths(e->mimeData());
	if (paths.isEmpty() || !player)
		return;

	e->setDropAction(Qt::CopyAction);
	e->accept();

	/* dropped on the player itself: add and play, like the classic */
	int first = player->AddPaths(paths);
	if (first >= 0)
		player->PlayIndex(first);
}

void SpazzMusicDock::Frame()
{
	float dt = clock.restart() / 1000.0f;
	if (dt <= 0.0f || dt > 0.25f)
		dt = 0.033f;

	/* nothing on screen to draw while OBS is minimized */
	if (window()->isMinimized())
		return;

	SetHue(Hue() + dt * 0.045f);
	display->Tick(dt);

	obs_media_state state = player ? player->State() : OBS_MEDIA_STATE_NONE;
	if (player && !seek->IsDragging()) {
		qint64 dur = player->DurationMs();
		bool active = state == OBS_MEDIA_STATE_PLAYING || state == OBS_MEDIA_STATE_PAUSED;
		seek->SetValue(active && dur > 0 ? (float)player->PositionMs() / (float)dur : 0.0f);
	}

	/* the colour cycle is slow, so the rest of the dock only repaints when
	 * the accent has visibly moved (~11 times a second) instead of every
	 * frame; panels and backgrounds never change and aren't touched */
	int hueStep = (int)(Hue() * 240.0f);
	if (hueStep != lastHueStep) {
		lastHueStep = hueStep;
		for (const QPointer<QWidget> &w : accentWidgets) {
			if (w && w->isVisible())
				w->update();
		}
		list->viewport()->update();
	}

	/* playing or settling: 30 fps; idle: 15 fps is plenty for the marquee */
	bool lively = state == OBS_MEDIA_STATE_PLAYING || display->IsAnimating();
	int interval = lively ? 33 : 66;
	if (frameTimer.interval() != interval)
		frameTimer.setInterval(interval);
}

QList<quint64> SpazzMusicDock::SelectedIds() const
{
	QList<quint64> ids;
	for (QListWidgetItem *item : list->selectedItems())
		ids.push_back(item->data(IdRole).toULongLong());
	return ids;
}

void SpazzMusicDock::RebuildList()
{
	if (!player)
		return;

	QSet<quint64> selected;
	for (quint64 id : SelectedIds())
		selected.insert(id);
	int scroll = list->verticalScrollBar() ? list->verticalScrollBar()->value() : 0;

	QSignalBlocker block(list);
	list->clear();

	const auto &tracks = player->Tracks();
	for (const auto &t : tracks) {
		auto *item = new QListWidgetItem(t.title, list);
		item->setData(IdRole, (qulonglong)t.id);
		item->setData(DurationRole, (qlonglong)t.durationMs);
		item->setData(CurrentRole, t.id == player->CurrentId());
		item->setToolTip(t.path);
		item->setSelected(selected.contains(t.id));
	}

	if (list->verticalScrollBar())
		list->verticalScrollBar()->setValue(scroll);

	UpdateTotals();
}

void SpazzMusicDock::UpdateTotals()
{
	if (!player)
		return;

	qint64 total = 0;
	bool unknown = false;
	const auto &tracks = player->Tracks();
	for (const auto &t : tracks) {
		if (t.durationMs > 0)
			total += t.durationMs;
		else
			unknown = true;
	}

	totals->setText(tracks.isEmpty() ? QString()
					 : QTStr("SPAZZ.Music.Totals")
						   .arg(tracks.size())
						   .arg(FormatTime(total) + (unknown ? "+" : "")));
}

void SpazzMusicDock::UpdateTrack(int index)
{
	if (!player || index < 0 || index >= player->Tracks().size())
		return;

	QListWidgetItem *item = list->item(index);
	if (item)
		item->setData(DurationRole, (qlonglong)player->Tracks()[index].durationMs);
	UpdateTotals();
}

void SpazzMusicDock::SyncCurrent()
{
	if (!player)
		return;

	quint64 cur = player->CurrentId();
	for (int i = 0; i < list->count(); i++) {
		QListWidgetItem *item = list->item(i);
		bool isCur = item->data(IdRole).toULongLong() == cur;
		item->setData(CurrentRole, isCur);
		if (isCur)
			list->scrollToItem(item);
	}
	list->viewport()->update();
}

void SpazzMusicDock::SyncState()
{
	if (!player)
		return;

	shuffleBtn->setChecked(player->Shuffle());
	repeatBtn->setChecked(player->RepeatMode() != SpazzMusicPlayer::Repeat::Off);
	repeatBtn->SetRepeatOne(player->RepeatMode() == SpazzMusicPlayer::Repeat::One);
	if (!volume->IsDragging())
		volume->SetValue(std::cbrt(std::max(player->Volume(), 0.0f)));
	if (!balance->IsDragging())
		balance->SetValue(player->Balance());
	display->update();
}

static QString MusicFolder()
{
	QString dir = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
	return dir.isEmpty() ? QDir::homePath() : dir;
}

void SpazzMusicDock::AddFilesDialog()
{
	if (!player)
		return;

	QString filter = QTStr("SPAZZ.Music.AudioFiles") +
			 " (*.mp3 *.wav *.flac *.ogg *.oga *.opus *.m4a *.aac *.wma *.aif *.aiff *.ape *.wv *.mka "
			 "*.m3u *.m3u8);;" +
			 QTStr("SPAZZ.Music.AllFiles") + " (*.*)";
	QStringList files = QFileDialog::getOpenFileNames(this, QTStr("SPAZZ.Music.AddFiles"), MusicFolder(), filter);
	if (!files.isEmpty())
		player->AddPaths(files);
}

void SpazzMusicDock::AddFolderDialog()
{
	if (!player)
		return;

	QString dir = QFileDialog::getExistingDirectory(this, QTStr("SPAZZ.Music.AddFolder"), MusicFolder());
	if (!dir.isEmpty())
		player->AddPaths({dir});
}
