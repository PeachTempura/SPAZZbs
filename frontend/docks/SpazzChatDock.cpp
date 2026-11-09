/******************************************************************************
    SPAZZbs - Chat dock: every platform's live chat in one list.
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "SpazzChatDock.hpp"

#include <OBSApp.hpp>
#include <docks/OBSDock.hpp>
#include <utility/SpazzChatNet.hpp>
#include <widgets/OBSBasic.hpp>

#ifdef BROWSER_AVAILABLE
#include <browser-panel.hpp>
extern QCef *cef;
extern QCefCookieManager *panel_cookies;
#endif

#include <qt-wrappers.hpp>

#include <QAbstractListModel>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QSvgRenderer>
#include <QTextLayout>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <deque>

using namespace spazzchat;

static constexpr int kMaxMessages = 500;
static constexpr const char *kSection = "SpazzChat";

/* ------------------------------------------------------------------------ */
/* platform look                                                             */

struct PlatformInfo {
	const char *key;  /* config key + locale: SpazzChat.<key> */
	const char *name; /* shown as is */
	QColor color;     /* name colour when the user has none */
	const char *svg;  /* our own simple glyph, 24x24 */
};

/* simple recognisable glyphs drawn for SPAZZbs (not the platforms' files) */
static const PlatformInfo kPlatforms[PlatformCount] = {
	{"YouTube", "YouTube", QColor(0xff, 0x40, 0x40),
	 R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><rect x="1" y="4.5" width="22" height="15" rx="4.5" fill="#FF0033"/><path d="M10 8.6v6.8l5.8-3.4z" fill="#fff"/></svg>)"},
	{"Kick", "Kick", QColor(0x53, 0xfc, 0x18),
	 R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><rect x="1" y="1" width="22" height="22" rx="5" fill="#53FC18"/><path d="M6.5 5.5h3.6v4.2h1.6V8h1.8V6.2h1.8V5.5h2.2v3.6h-1.8v1.8h-1.8v2.2h1.8v1.8h1.8v3.6h-2.2v-.7h-1.8V16h-1.8v-1.7h-1.6v4.2H6.5z" fill="#0B0E0F"/></svg>)"},
	{"Twitch", "Twitch", QColor(0xa9, 0x70, 0xff),
	 R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path d="M4.5 2 2.8 6v14h4.6v2.6h2.6l2.6-2.6h3.8L21.2 15V2z" fill="#9146FF"/><path d="M5.6 3.8h13.8v10.4l-3 3h-4.4l-2.6 2.6v-2.6H5.6z" fill="#fff"/><path d="M11 7.2h1.8v5.2H11zm4.6 0h1.8v5.2h-1.8z" fill="#9146FF"/></svg>)"},
	{"Rumble", "Rumble", QColor(0x85, 0xc7, 0x42),
	 R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path d="M5.2 3.6c1-1.3 2.8-1.6 4.2-.8l9.6 5.6c2.4 1.4 2.4 4.8 0 6.2l-9.6 5.6c-1.4.8-3.2.5-4.2-.8C3.3 16.9 2.4 14.5 2.4 12s.9-4.9 2.8-8.4z" fill="#85C742"/><path d="M9.3 8.6c0-.8.9-1.3 1.6-.9l4.5 2.7c.7.4.7 1.4 0 1.8l-4.5 2.7c-.7.4-1.6-.1-1.6-.9z" fill="#fff"/></svg>)"},
	{"X", "X", QColor(0xe7, 0xe9, 0xea),
	 R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><rect x="1" y="1" width="22" height="22" rx="5" fill="#000"/><path d="M6 5.5h3.4l9 13H15z" fill="#fff"/><path d="M17.4 5.5h1.4L13 12.2l-.8-1.1zM6 18.5l5.3-6.1.8 1.1-4.7 5z" fill="#fff"/></svg>)"},
};

static QPixmap Logo(int platform, int px, qreal dpr)
{
	static QHash<qint64, QPixmap> cache;
	const qint64 key = (qint64(platform) << 32) | (qint64(px) << 8) | qint64(dpr * 4);
	auto it = cache.find(key);
	if (it != cache.end()) {
		return *it;
	}
	const int real = qMax(1, int(px * dpr + 0.5));
	QPixmap pm(real, real);
	pm.fill(Qt::transparent);
	QSvgRenderer svg(QByteArray(kPlatforms[platform].svg));
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	svg.render(&p);
	p.end();
	pm.setDevicePixelRatio(dpr);
	cache.insert(key, pm);
	return pm;
}

/* readable on the list's background */
static QColor Readable(QColor c, const QColor &bg)
{
	const bool darkBg = bg.lightnessF() < 0.5;
	float h, s, l, a;
	c.getHslF(&h, &s, &l, &a);
	if (darkBg && l < 0.55f) {
		l = 0.55f + l * 0.3f;
	} else if (!darkBg && l > 0.45f) {
		l = 0.25f + (1.0f - l) * 0.3f;
	}
	c.setHslF(h, s, l, 1.0f);
	return c;
}

static QColor NameColor(const Message &m)
{
	const QColor given(QString::fromStdString(m.color));
	if (given.isValid()) {
		return given;
	}
	/* a steady colour per name, like most chat apps do */
	static const QColor pool[] = {
		QColor(0xff, 0x6b, 0x6b), QColor(0xff, 0xa9, 0x4d), QColor(0xff, 0xd9, 0x3d), QColor(0x8c, 0xe9, 0x9a),
		QColor(0x4d, 0xd4, 0xc8), QColor(0x5c, 0xb8, 0xff), QColor(0x9c, 0x8c, 0xff), QColor(0xf0, 0x7c, 0xe0),
		QColor(0xff, 0x8f, 0xab), QColor(0xb5, 0xe0, 0x5a),
	};
	const size_t h = qHash(QString::fromStdString(m.user));
	return pool[h % (sizeof(pool) / sizeof(pool[0]))];
}

/* ------------------------------------------------------------------------ */
/* model                                                                     */

struct ChatEntry {
	Message m;
	QString user;
	QString text;
	QString amount;
	QString time;
	QColor color;
	QStringList badges;
	/* layout cache */
	int cachedWidth = -1;
	int cachedFont = -1;
	int cachedHeight = 0;
};

class SpazzChatModel : public QAbstractListModel {
public:
	using QAbstractListModel::QAbstractListModel;

	int rowCount(const QModelIndex &parent = QModelIndex()) const override
	{
		return parent.isValid() ? 0 : int(entries.size());
	}
	QVariant data(const QModelIndex &index, int role) const override
	{
		if (!index.isValid() || index.row() >= int(entries.size())) {
			return QVariant();
		}
		const ChatEntry &e = entries[size_t(index.row())];
		if (role == Qt::DisplayRole) {
			return e.user.isEmpty() ? e.text : e.user + ": " + e.text;
		}
		if (role == Qt::ToolTipRole) {
			return QString::fromUtf8(kPlatforms[e.m.platform].name) + " · " + e.time;
		}
		return QVariant();
	}

	void Append(std::vector<Message> &msgs)
	{
		if (msgs.empty()) {
			return;
		}
		/* drop the oldest first so the list never grows past the cap */
		const int over = int(entries.size() + msgs.size()) - kMaxMessages;
		if (over > 0) {
			const int drop = qMin(over, int(entries.size()));
			if (drop > 0) {
				beginRemoveRows(QModelIndex(), 0, drop - 1);
				entries.erase(entries.begin(), entries.begin() + drop);
				endRemoveRows();
			}
			if (int(msgs.size()) > kMaxMessages) {
				msgs.erase(msgs.begin(), msgs.end() - kMaxMessages);
			}
		}
		const int first = int(entries.size());
		beginInsertRows(QModelIndex(), first, first + int(msgs.size()) - 1);
		for (Message &m : msgs) {
			ChatEntry e;
			e.user = QString::fromStdString(m.user);
			e.text = QString::fromStdString(m.text);
			e.amount = QString::fromStdString(m.amount);
			const qint64 ms = m.timeMs > 0 ? m.timeMs : QDateTime::currentMSecsSinceEpoch();
			e.time = QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("h:mm"));
			e.color = NameColor(m);
			for (const Badge &b : m.badges) {
				if (!b.label.empty()) {
					e.badges << QString::fromStdString(b.label);
				}
			}
			e.m = std::move(m);
			entries.push_back(std::move(e));
		}
		endInsertRows();
	}

	void Clear()
	{
		beginResetModel();
		entries.clear();
		endResetModel();
	}

	const ChatEntry *At(int row) const
	{
		return row >= 0 && row < int(entries.size()) ? &entries[size_t(row)] : nullptr;
	}
	ChatEntry *At(int row) { return row >= 0 && row < int(entries.size()) ? &entries[size_t(row)] : nullptr; }

private:
	std::deque<ChatEntry> entries;
};

/* ------------------------------------------------------------------------ */
/* delegate: [logo] time [badges] [amount] Name: message, wrapped            */

class SpazzChatDelegate : public QStyledItemDelegate {
public:
	SpazzChatDelegate(SpazzChatModel *model_, QObject *parent) : QStyledItemDelegate(parent), model(model_) {}

	bool showTime = true;
	int fontVersion = 0;

	QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		ChatEntry *e = model->At(index.row());
		if (!e) {
			return QSize(0, 0);
		}
		const int width = qMax(80, option.rect.width() > 0 ? option.rect.width() : 300);
		if (e->cachedWidth != width || e->cachedFont != fontVersion) {
			e->cachedHeight = Layout(*e, option, width, nullptr);
			e->cachedWidth = width;
			e->cachedFont = fontVersion;
		}
		return QSize(width, e->cachedHeight);
	}

	void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		const ChatEntry *e = model->At(index.row());
		if (!e) {
			return;
		}
		p->save();
		p->setRenderHint(QPainter::Antialiasing);
		Layout(*e, option, option.rect.width(), p);
		p->restore();
	}

private:
	SpazzChatModel *model;

	/* lays the message out at option.rect; paints when p is set; returns height */
	int Layout(const ChatEntry &e, const QStyleOptionViewItem &option, int width, QPainter *p) const
	{
		const QFont font = option.font;
		QFont small = font;
		small.setPointSizeF(qMax(6.0, font.pointSizeF() * 0.78));
		QFont bold = font;
		bold.setBold(true);
		const QFontMetrics fm(font);
		const QFontMetrics sfm(small);
		const int padX = 6, padY = 3, gap = 5;
		const int logo = fm.height();
		const QRect r = option.rect;
		const int top = r.top() + padY;
		const bool system = e.m.kind == KindSystem;
		const QColor base = option.palette.color(QPalette::Base);
		const QColor textColor = option.palette.color(QPalette::Text);
		QColor muted = textColor;
		muted.setAlphaF(0.55f);

		/* row background for paid / member messages */
		if (p && (e.m.kind == KindPaid || e.m.kind == KindMember)) {
			QColor tint = e.m.kind == KindPaid ? QColor(255, 196, 0) : QColor(160, 110, 255);
			tint.setAlphaF(0.16f);
			p->fillRect(r.adjusted(0, 1, 0, -1), tint);
			QColor bar = tint;
			bar.setAlphaF(0.9f);
			p->fillRect(QRect(r.left(), r.top() + 1, 3, r.height() - 2), bar);
		}

		int x = r.left() + padX;
		if (p) {
			p->drawPixmap(QRect(x, top + (fm.height() - logo) / 2, logo, logo),
				      Logo(e.m.platform, logo, p->device()->devicePixelRatioF()));
		}
		x += logo + gap;
		const int hang = x; /* wrapped lines start here */

		if (showTime && !system) {
			const int tw = sfm.horizontalAdvance(e.time);
			if (p) {
				p->setFont(small);
				p->setPen(muted);
				p->drawText(QRect(x, top, tw, fm.height()), Qt::AlignVCenter | Qt::AlignLeft, e.time);
			}
			x += tw + gap;
		}

		auto pill = [&](const QString &label, const QColor &fill, const QColor &ink) {
			const int w = sfm.horizontalAdvance(label) + 8;
			const int h = sfm.height();
			if (p) {
				const QRect pr(x, top + (fm.height() - h) / 2, w, h);
				p->setPen(Qt::NoPen);
				p->setBrush(fill);
				p->drawRoundedRect(pr, h / 2.5, h / 2.5);
				p->setFont(small);
				p->setPen(ink);
				p->drawText(pr, Qt::AlignCenter, label);
			}
			x += w + 3;
		};
		for (const QString &b : e.badges) {
			QColor fill(90, 90, 100);
			if (b == "Mod") {
				fill = QColor(0, 173, 3);
			} else if (b == "Owner" || b == "Streamer") {
				fill = QColor(230, 60, 60);
			} else if (b == "Member" || b == "Sub" || b == "Premium") {
				fill = QColor(120, 80, 220);
			} else if (b == "VIP" || b == "OG") {
				fill = QColor(220, 40, 160);
			} else if (b == "Verified") {
				fill = QColor(60, 130, 230);
			}
			pill(b, fill, Qt::white);
		}
		if (!e.amount.isEmpty()) {
			pill(e.amount, QColor(255, 196, 0), QColor(30, 20, 0));
		}

		/* name + message, wrapped with a hanging indent */
		QString full;
		QList<QTextLayout::FormatRange> formats;
		if (!system && !e.user.isEmpty()) {
			full = e.user;
			QTextLayout::FormatRange fr;
			fr.start = 0;
			fr.length = int(e.user.size());
			fr.format.setFont(bold);
			fr.format.setForeground(Readable(e.color, base));
			formats << fr;
			full += e.m.kind == KindChat || e.m.kind == KindPaid ? QStringLiteral(": ")
									     : QStringLiteral(" ");
		}
		const int msgStart = int(full.size());
		full += e.text;
		if (system) {
			QTextLayout::FormatRange fr;
			fr.start = msgStart;
			fr.length = int(e.text.size());
			QFont it = font;
			it.setItalic(true);
			fr.format.setFont(it);
			fr.format.setForeground(muted);
			formats << fr;
		}

		QTextLayout layout(full, font);
		layout.setFormats(formats);
		QTextOption opt;
		opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
		layout.setTextOption(opt);
		const int right = r.left() + width - padX;
		qreal y = top;
		layout.beginLayout();
		bool firstLine = true;
		for (;;) {
			QTextLine line = layout.createLine();
			if (!line.isValid()) {
				break;
			}
			const int lx = firstLine ? x : hang;
			line.setLineWidth(qMax(20, right - lx));
			line.setPosition(QPointF(lx - r.left(), y - r.top()));
			y += line.height();
			firstLine = false;
		}
		layout.endLayout();
		if (p) {
			p->setPen(textColor);
			layout.draw(p, r.topLeft());
		}
		const int height = int(qMax<qreal>(y - top, fm.height())) + padY * 2;
		return height;
	}
};

/* ------------------------------------------------------------------------ */
/* the view                                                                  */

static config_t *Cfg()
{
	return App()->GetUserConfig();
}

OBSDock *SpazzChatView::CreateDock(QWidget *mainWindow)
{
	OBSDock *dock = new OBSDock(mainWindow);
	dock->setObjectName(QStringLiteral("spazzChatDock"));
	dock->setWindowTitle(QTStr("SpazzChat.Title"));
	dock->setAllowedAreas(Qt::AllDockWidgetAreas);
	dock->setMinimumWidth(220);
	dock->setWidget(new SpazzChatView(dock));
	return dock;
}

SpazzChatView::SpazzChatView(QWidget *parent) : QWidget(parent)
{
	config_set_default_bool(Cfg(), kSection, "ShowTime", true);
	config_set_default_int(Cfg(), kSection, "FontSize", 10);

	net = new QNetworkAccessManager(this);
	shown.fill(true);
	lastState.fill(SpazzChatConnector::Off);

	connectors[YouTube] = new SpazzYouTubeChat(net, this);
	connectors[Kick] = new SpazzKickChat(net, this);
	connectors[Twitch] = new SpazzTwitchChat(net, this);
	connectors[Rumble] = new SpazzRumbleChat(net, this);
	for (SpazzChatConnector *c : connectors) {
		if (!c) {
			continue;
		}
		c->onMessage = [this](const Message &m) {
			Receive(m);
		};
		c->onStatus = [this, c]() {
			StatusChanged(c->Platform());
		};
	}

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(4);

	/* header: a chip per platform (click = show/hide its messages), then tools */
	QHBoxLayout *header = new QHBoxLayout();
	header->setSpacing(2);
	for (int i = 0; i < PlatformCount; i++) {
		QToolButton *chip = new QToolButton(this);
		chip->setAutoRaise(true);
		chip->setCheckable(i != X);
		chip->setChecked(true);
		chip->setIconSize(QSize(18, 18));
		chip->setContextMenuPolicy(Qt::CustomContextMenu);
		chips[size_t(i)] = chip;
		header->addWidget(chip);
		if (i == X) {
			connect(chip, &QToolButton::clicked, this, [this]() { OpenX(); });
		} else {
			connect(chip, &QToolButton::toggled, this, [this, i](bool on) {
				shown[size_t(i)] = on;
				ApplyFilter();
			});
		}
		connect(chip, &QToolButton::customContextMenuRequested, this, [this, i, chip](const QPoint &pos) {
			QMenu menu;
			if (i != X && connectors[size_t(i)]) {
				menu.addAction(QTStr("SpazzChat.Reconnect"), this, [this, i]() {
					SpazzChatConnector *c = connectors[size_t(i)];
					c->Stop();
					c->Start();
				});
			}
			menu.addAction(QTStr("SpazzChat.Settings"), this, [this]() { OpenSettings(); });
			menu.exec(chip->mapToGlobal(pos));
		});
	}
	header->addStretch(1);

	auto tool = [this, header](const QString &text, const QString &tip) {
		QToolButton *b = new QToolButton(this);
		b->setAutoRaise(true);
		b->setText(text);
		b->setToolTip(tip);
		header->addWidget(b);
		return b;
	};
	QToolButton *smaller = tool(QStringLiteral("A−"), QTStr("SpazzChat.Smaller"));
	QToolButton *bigger = tool(QStringLiteral("A+"), QTStr("SpazzChat.Bigger"));
	QToolButton *clear = tool(QStringLiteral("✕"), QTStr("SpazzChat.Clear"));
	QToolButton *settings = tool(QStringLiteral("⚙"), QTStr("SpazzChat.Settings"));
	connect(smaller, &QToolButton::clicked, this, [this]() { SetFontPt(fontPt - 1); });
	connect(bigger, &QToolButton::clicked, this, [this]() { SetFontPt(fontPt + 1); });
	connect(clear, &QToolButton::clicked, this, [this]() {
		model->Clear();
		unseen = 0;
		jumpButton->hide();
	});
	connect(settings, &QToolButton::clicked, this, [this]() { OpenSettings(); });
	layout->addLayout(header);

	/* the messages */
	model = new SpazzChatModel(this);
	delegate = new SpazzChatDelegate(model, this);
	delegate->showTime = config_get_bool(Cfg(), kSection, "ShowTime");
	list = new QListView(this);
	list->setModel(model);
	list->setItemDelegate(delegate);
	list->setUniformItemSizes(false);
	list->setWordWrap(true);
	list->setSelectionMode(QAbstractItemView::NoSelection);
	list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	list->setResizeMode(QListView::Adjust);
	list->setContextMenuPolicy(Qt::CustomContextMenu);
	list->setFocusPolicy(Qt::NoFocus);
	layout->addWidget(list, 1);
	connect(list, &QListView::customContextMenuRequested, this, [this](const QPoint &pos) {
		const ChatEntry *e = model->At(list->indexAt(pos).row());
		if (!e) {
			return;
		}
		QMenu menu;
		menu.addAction(QTStr("SpazzChat.CopyMessage"), this,
			       [e]() { QApplication::clipboard()->setText(e->text); });
		if (!e->user.isEmpty()) {
			menu.addAction(QTStr("SpazzChat.CopyName"), this,
				       [e]() { QApplication::clipboard()->setText(e->user); });
		}
		menu.exec(list->viewport()->mapToGlobal(pos));
	});

	/* shown instead of the list until the first message arrives */
	emptyHint = new QLabel(QTStr("SpazzChat.EmptyHint"), this);
	emptyHint->setWordWrap(true);
	emptyHint->setAlignment(Qt::AlignCenter);
	emptyHint->setProperty("class", "text-muted");
	emptyHint->setContentsMargins(12, 12, 12, 12);
	layout->addWidget(emptyHint, 1);

	/* when you scroll up to read, new messages wait below */
	jumpButton = new QPushButton(this);
	jumpButton->hide();
	layout->addWidget(jumpButton);
	connect(jumpButton, &QPushButton::clicked, this, [this]() {
		unseen = 0;
		jumpButton->hide();
		list->scrollToBottom();
	});
	connect(list->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
		if (AtBottom() && jumpButton->isVisible()) {
			unseen = 0;
			jumpButton->hide();
		}
	});

	flushTimer = new QTimer(this);
	flushTimer->setSingleShot(true);
	flushTimer->setInterval(80);
	connect(flushTimer, &QTimer::timeout, this, &SpazzChatView::Flush);

	SetFontPt(int(config_get_int(Cfg(), kSection, "FontSize")));
	for (int i = 0; i < PlatformCount; i++) {
		UpdateChip(i);
	}
	UpdateEmptyHint();
	obs_frontend_add_event_callback(SpazzChatView::FrontendEvent, this);

	/* connect once the window is up, not during startup */
	QTimer::singleShot(1500, this, [this]() { Reconfigure(); });
}

SpazzChatView::~SpazzChatView()
{
	obs_frontend_remove_event_callback(SpazzChatView::FrontendEvent, this);
	exiting = true;
	for (SpazzChatConnector *c : connectors) {
		if (c) {
			c->onMessage = nullptr;
			c->onStatus = nullptr;
			c->Stop();
		}
	}
}

void SpazzChatView::FrontendEvent(enum obs_frontend_event event, void *data)
{
	SpazzChatView *self = static_cast<SpazzChatView *>(data);
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		self->exiting = true;
		for (SpazzChatConnector *c : self->connectors) {
			if (c) {
				c->onStatus = nullptr;
				c->Stop();
			}
		}
	}
}

void SpazzChatView::Reconfigure()
{
	if (exiting) {
		return;
	}
	for (int i = 0; i < PlatformCount; i++) {
		const PlatformInfo &info = kPlatforms[i];
		const QString target = QT_UTF8(config_get_string(Cfg(), kSection, info.key)).trimmed();
		const bool on = config_get_bool(Cfg(), kSection, (std::string(info.key) + "On").c_str());
		if (i == X) {
			xTarget = on ? target : QString();
			UpdateChip(i);
			continue;
		}
		SpazzChatConnector *c = connectors[size_t(i)];
		const QString want = on ? target : QString();
		if (c->Target() == want && (want.isEmpty() || c->GetState() != SpazzChatConnector::Off)) {
			continue;
		}
		c->Stop();
		c->Configure(want);
		if (!want.isEmpty()) {
			c->Start();
		}
		UpdateChip(i);
	}
	UpdateEmptyHint();
}

void SpazzChatView::Receive(const Message &m)
{
	if (exiting) {
		return;
	}
	incoming.push_back(m);
	if (incoming.size() > size_t(kMaxMessages)) {
		incoming.erase(incoming.begin(), incoming.end() - kMaxMessages);
	}
	if (!flushTimer->isActive()) {
		flushTimer->start();
	}
}

bool SpazzChatView::AtBottom() const
{
	const QScrollBar *sb = list->verticalScrollBar();
	return sb->value() >= sb->maximum() - 6;
}

void SpazzChatView::Flush()
{
	if (incoming.empty()) {
		return;
	}
	const bool follow = AtBottom();
	const int before = model->rowCount();
	const int count = int(incoming.size());
	std::vector<Message> batch;
	batch.swap(incoming);
	model->Append(batch);
	/* rows of hidden platforms stay hidden */
	const int rows = model->rowCount();
	for (int row = qMax(0, rows - count); row < rows; row++) {
		const ChatEntry *e = model->At(row);
		if (e && !shown[size_t(e->m.platform)]) {
			list->setRowHidden(row, true);
		}
	}
	UpdateEmptyHint();
	if (follow || before == 0) {
		QTimer::singleShot(0, list, [this]() { list->scrollToBottom(); });
	} else {
		unseen += count;
		jumpButton->setText(QTStr("SpazzChat.NewMessages").arg(unseen));
		jumpButton->show();
	}
}

void SpazzChatView::SystemNote(int platform, const QString &text)
{
	Message m;
	m.platform = platform;
	m.kind = KindSystem;
	m.text = text.toStdString();
	Receive(m);
}

void SpazzChatView::StatusChanged(int platform)
{
	if (exiting) {
		return;
	}
	UpdateChip(platform);
	SpazzChatConnector *c = connectors[size_t(platform)];
	const int st = c->GetState();
	const int was = lastState[size_t(platform)];
	lastState[size_t(platform)] = st;
	const QString name = QString::fromUtf8(kPlatforms[platform].name);
	if (st == SpazzChatConnector::Live && was != SpazzChatConnector::Live) {
		SystemNote(platform, QTStr("SpazzChat.Connected").arg(name));
	} else if ((st == SpazzChatConnector::Failed || st == SpazzChatConnector::Waiting) && st != was &&
		   !c->Detail().isEmpty()) {
		SystemNote(platform, c->Detail());
	}
}

void SpazzChatView::UpdateChip(int platform)
{
	QToolButton *chip = chips[size_t(platform)];
	if (!chip) {
		return;
	}
	const QString name = QString::fromUtf8(kPlatforms[platform].name);
	QColor dot(120, 120, 120);
	QString status;
	bool configured;
	if (platform == X) {
		configured = !xTarget.isEmpty();
		status = configured ? QTStr("SpazzChat.XOpens") : QTStr("SpazzChat.NotSetUp");
		dot = configured ? QColor(60, 130, 230) : dot;
	} else {
		SpazzChatConnector *c = connectors[size_t(platform)];
		configured = !c->Target().isEmpty();
		switch (c->GetState()) {
		case SpazzChatConnector::Live:
			dot = QColor(40, 200, 90);
			status = QTStr("SpazzChat.Live");
			break;
		case SpazzChatConnector::Connecting:
			dot = QColor(240, 190, 40);
			status = QTStr("SpazzChat.Connecting");
			break;
		case SpazzChatConnector::Waiting:
			dot = QColor(240, 190, 40);
			status = c->Detail();
			break;
		case SpazzChatConnector::Failed:
			dot = QColor(230, 60, 60);
			status = c->Detail();
			break;
		default:
			status = configured ? QTStr("SpazzChat.Off") : QTStr("SpazzChat.NotSetUp");
			break;
		}
	}
	const qreal dpr = chip->devicePixelRatioF();
	QPixmap pm(int(22 * dpr), int(22 * dpr));
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);
	{
		QPainter p(&pm);
		p.setRenderHint(QPainter::Antialiasing);
		if (!configured) {
			p.setOpacity(0.35);
		}
		p.drawPixmap(QRect(0, 0, 18, 18), Logo(platform, 18, dpr));
		p.setOpacity(1.0);
		p.setPen(QPen(chip->palette().color(QPalette::Window), 1.5));
		p.setBrush(dot);
		p.drawEllipse(QRectF(13, 13, 8, 8));
	}
	chip->setIcon(QIcon(pm));
	chip->setIconSize(QSize(22, 22));
	QString tip = name + " — " + status;
	if (platform != X) {
		tip += "\n" + QTStr("SpazzChat.ChipTip");
	}
	chip->setToolTip(tip);
}

void SpazzChatView::ApplyFilter()
{
	const int rows = model->rowCount();
	for (int row = 0; row < rows; row++) {
		const ChatEntry *e = model->At(row);
		list->setRowHidden(row, e && !shown[size_t(e->m.platform)]);
	}
	list->scrollToBottom();
}

void SpazzChatView::SetFontPt(int pt)
{
	fontPt = qBound(7, pt, 28);
	QFont f = list->font();
	f.setPointSize(fontPt);
	list->setFont(f);
	delegate->fontVersion++;
	config_set_int(Cfg(), kSection, "FontSize", fontPt);
	/* every row changes height */
	list->doItemsLayout();
	if (AtBottom()) {
		list->scrollToBottom();
	}
}

void SpazzChatView::UpdateEmptyHint()
{
	bool any = !xTarget.isEmpty();
	for (SpazzChatConnector *c : connectors) {
		if (c && !c->Target().isEmpty()) {
			any = true;
		}
	}
	emptyHint->setText(any ? QTStr("SpazzChat.Waiting") : QTStr("SpazzChat.EmptyHint"));
	const bool empty = model->rowCount() == 0;
	emptyHint->setVisible(empty);
	list->setVisible(!empty);
}

void SpazzChatView::OpenX()
{
	if (xTarget.isEmpty()) {
		OpenSettings();
		return;
	}
	QString url = xTarget;
	if (!url.startsWith(QStringLiteral("http"))) {
		url.remove('@');
		url = QStringLiteral("https://x.com/") + url;
	}
#ifdef BROWSER_AVAILABLE
	if (cef) {
		OBSBasic::InitBrowserPanelSafeBlock();
		QDialog *dlg = new QDialog(window());
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		dlg->setWindowTitle(QTStr("SpazzChat.XTitle"));
		dlg->resize(420, 720);
		QVBoxLayout *l = new QVBoxLayout(dlg);
		l->setContentsMargins(0, 0, 0, 0);
		QCefWidget *browser = cef->create_widget(dlg, url.toStdString(), panel_cookies);
		l->addWidget(browser);
		dlg->show();
		return;
	}
#endif
	QDesktopServices::openUrl(QUrl(url));
}

void SpazzChatView::OpenSettings()
{
	QDialog dlg(window());
	dlg.setWindowTitle(QTStr("SpazzChat.SettingsTitle"));
	dlg.setMinimumWidth(520);
	QVBoxLayout *outer = new QVBoxLayout(&dlg);
	QLabel *intro = new QLabel(QTStr("SpazzChat.SettingsIntro"), &dlg);
	intro->setWordWrap(true);
	outer->addWidget(intro);
	QFormLayout *form = new QFormLayout();
	outer->addLayout(form);

	struct Row {
		QCheckBox *on;
		QLineEdit *edit;
	};
	std::array<Row, PlatformCount> rows{};
	const int order[] = {YouTube, Kick, Twitch, Rumble, X};
	for (int i : order) {
		const PlatformInfo &info = kPlatforms[i];
		QWidget *line = new QWidget(&dlg);
		QHBoxLayout *h = new QHBoxLayout(line);
		h->setContentsMargins(0, 0, 0, 0);
		QCheckBox *on = new QCheckBox(&dlg);
		on->setChecked(config_get_bool(Cfg(), kSection, (std::string(info.key) + "On").c_str()));
		QLineEdit *edit = new QLineEdit(QT_UTF8(config_get_string(Cfg(), kSection, info.key)), &dlg);
		edit->setPlaceholderText(QTStr((std::string("SpazzChat.Hint.") + info.key).c_str()));
		if (i == Rumble) {
			edit->setEchoMode(QLineEdit::PasswordEchoOnEdit); /* the link holds a key */
		}
		connect(edit, &QLineEdit::textEdited, on, [on](const QString &t) {
			if (!t.trimmed().isEmpty()) {
				on->setChecked(true);
			}
		});
		h->addWidget(on);
		h->addWidget(edit, 1);
		QLabel *label = new QLabel(&dlg);
		label->setPixmap(Logo(i, 18, devicePixelRatioF()));
		QWidget *labelBox = new QWidget(&dlg);
		QHBoxLayout *lb = new QHBoxLayout(labelBox);
		lb->setContentsMargins(0, 0, 0, 0);
		lb->addWidget(label);
		lb->addWidget(new QLabel(QString::fromUtf8(info.name), &dlg));
		form->addRow(labelBox, line);
		rows[size_t(i)] = {on, edit};
	}
	QLabel *note = new QLabel(QTStr("SpazzChat.SettingsNote"), &dlg);
	note->setWordWrap(true);
	note->setProperty("class", "text-muted");
	outer->addWidget(note);
	QCheckBox *showTime = new QCheckBox(QTStr("SpazzChat.ShowTime"), &dlg);
	showTime->setChecked(delegate->showTime);
	outer->addWidget(showTime);

	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
	outer->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	if (dlg.exec() != QDialog::Accepted) {
		return;
	}
	for (int i = 0; i < PlatformCount; i++) {
		const PlatformInfo &info = kPlatforms[i];
		config_set_string(Cfg(), kSection, info.key, QT_TO_UTF8(rows[size_t(i)].edit->text().trimmed()));
		config_set_bool(Cfg(), kSection, (std::string(info.key) + "On").c_str(),
				rows[size_t(i)].on->isChecked());
	}
	config_set_bool(Cfg(), kSection, "ShowTime", showTime->isChecked());
	delegate->showTime = showTime->isChecked();
	delegate->fontVersion++;
	list->doItemsLayout();
	config_save_safe(Cfg(), "tmp", nullptr);
	Reconfigure();
}
