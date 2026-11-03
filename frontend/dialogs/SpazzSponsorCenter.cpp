/******************************************************************************
    SPAZZbs - SPONSORCENTER
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "SpazzSponsorCenter.hpp"

#include <OBSApp.hpp>
#include <utility/display-helpers.hpp>
#include <widgets/OBSBasic.hpp>
#include <widgets/OBSQTDisplay.hpp>
#include <properties-view.hpp>
#include <qt-wrappers.hpp>

#include <obs-frontend-api.h>

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

/* the option lists the sponsor page understands, shared with the source */
#include "../../plugins/spazz-sponsor/spazz-sponsor-spec.h"

#include "moc_SpazzSponsorCenter.cpp"

static constexpr const char *kSponsorId = "spazz_sponsor";

QPointer<SpazzSponsorCenter> SpazzSponsorCenter::instance;

static void FillCombo(QComboBox *box, const spn_option *opts)
{
	for (const spn_option *o = opts; o->value; o++) {
		box->addItem(QString::fromUtf8(o->label), QString::fromUtf8(o->value));
	}
}

static void SetCombo(QComboBox *box, const char *value)
{
	int i = box->findData(QString::fromUtf8(value ? value : ""));
	box->setCurrentIndex(i < 0 ? 0 : i);
}

/* obs_data arrays are shared by reference, so the studio works on its own copy */
static obs_data_array_t *CloneArray(obs_data_array_t *src)
{
	obs_data_array_t *out = obs_data_array_create();
	for (size_t i = 0; i < obs_data_array_count(src); i++) {
		OBSDataAutoRelease it = obs_data_array_item(src, i);
		OBSDataAutoRelease copy = obs_data_create_from_json(obs_data_get_json(it));
		obs_data_array_push_back(out, copy);
	}
	return out;
}

static void CloneItems(obs_data_t *settings)
{
	OBSDataArrayAutoRelease items = obs_data_get_array(settings, "items");
	if (!items) {
		return;
	}
	OBSDataArrayAutoRelease copy = CloneArray(items);
	obs_data_set_array(settings, "items", copy);
}

static const QString kImageFilter = "Images (*.png *.jpg *.jpeg *.gif *.webp *.svg *.bmp);;All files (*.*)";

static bool IsImageFile(const QString &path)
{
	static const QStringList exts = {"png", "jpg", "jpeg", "gif", "webp", "svg", "bmp"};
	return exts.contains(QFileInfo(path).suffix().toLower());
}

/* ------------------------------------------------------------------------ */
/* the logo square: drop a file on it, or click to pick one                   */

class LogoDrop : public QLabel {
public:
	std::function<void(const QString &)> picked;

	explicit LogoDrop(QWidget *parent) : QLabel(parent)
	{
		setAcceptDrops(true);
		setFixedSize(104, 104);
		setAlignment(Qt::AlignCenter);
		setWordWrap(true);
		setCursor(Qt::PointingHandCursor);
		setToolTip(QTStr("Sponsorcenter.Card.LogoTip"));
		Show(QString());
	}

	void Show(const QString &path)
	{
		QPixmap pm;
		if (!path.isEmpty()) {
			pm.load(path);
		}
		if (!pm.isNull()) {
			setPixmap(pm.scaled(QSize(92, 92) * devicePixelRatioF(), Qt::KeepAspectRatio,
					    Qt::SmoothTransformation));
			setStyleSheet("QLabel { border: 1px solid palette(mid); border-radius: 8px; }");
		} else {
			setPixmap(QPixmap());
			setText(path.isEmpty() ? QTStr("Sponsorcenter.Card.DropLogo") : QFileInfo(path).fileName());
			setStyleSheet("QLabel { border: 2px dashed palette(mid); border-radius: 8px; padding: 6px; }");
		}
	}

protected:
	void dragEnterEvent(QDragEnterEvent *e) override
	{
		if (FirstImage(e->mimeData()).isEmpty()) {
			return;
		}
		e->acceptProposedAction();
	}
	void dropEvent(QDropEvent *e) override
	{
		const QString file = FirstImage(e->mimeData());
		if (!file.isEmpty() && picked) {
			e->acceptProposedAction();
			picked(file);
		}
	}
	void mousePressEvent(QMouseEvent *e) override
	{
		if (e->button() != Qt::LeftButton) {
			return;
		}
		const QString file = QFileDialog::getOpenFileName(this, QTStr("Sponsorcenter.Card.PickLogo"), QString(),
								  kImageFilter);
		if (!file.isEmpty() && picked) {
			picked(file);
		}
	}

private:
	static QString FirstImage(const QMimeData *md)
	{
		if (!md || !md->hasUrls()) {
			return QString();
		}
		for (const QUrl &u : md->urls()) {
			const QString f = u.toLocalFile();
			if (!f.isEmpty() && IsImageFile(f)) {
				return f;
			}
		}
		return QString();
	}
};

/* ------------------------------------------------------------------------ */
/* one sponsor: logo, how it sits, its text, its little animations          */

class SponsorCard : public QFrame {
public:
	std::function<void()> changed;
	std::function<void(int)> action; /* 0 up, 1 down, 2 duplicate, 3 delete */

	explicit SponsorCard(QWidget *parent) : QFrame(parent)
	{
		setObjectName("sponsorCard");
		setFrameShape(QFrame::StyledPanel);
		SetFocused(false);

		QVBoxLayout *v = new QVBoxLayout(this);
		v->setContentsMargins(10, 8, 10, 10);
		v->setSpacing(8);

		/* header: number, on/off, reorder, copy, remove */
		QHBoxLayout *hdr = new QHBoxLayout();
		title = new QLabel(this);
		title->setStyleSheet("font-weight: bold;");
		showBox = new QCheckBox(QTStr("Sponsorcenter.Card.Show"), this);
		showBox->setToolTip(QTStr("Sponsorcenter.Card.ShowTip"));
		hdr->addWidget(title, 1);
		hdr->addWidget(showBox);
		auto tool = [this, hdr](QChar glyph, const char *tip, int act) {
			QToolButton *b = new QToolButton(this);
			b->setText(QString(glyph));
			b->setToolTip(QTStr(tip));
			b->setAutoRaise(true);
			hdr->addWidget(b);
			connect(b, &QToolButton::clicked, this, [this, act]() {
				if (action) {
					action(act);
				}
			});
			return b;
		};
		up = tool(QChar(0x25B2), "Sponsorcenter.Card.Up", 0);
		down = tool(QChar(0x25BC), "Sponsorcenter.Card.Down", 1);
		tool(QChar(0x29C9), "Sponsorcenter.Card.Duplicate", 2);
		tool(QChar(0x2715), "Sponsorcenter.Card.Delete", 3);
		v->addLayout(hdr);

		/* logo and how it's shown */
		QHBoxLayout *mid = new QHBoxLayout();
		QVBoxLayout *logoCol = new QVBoxLayout();
		logo = new LogoDrop(this);
		clearLogo = new QPushButton(QTStr("Sponsorcenter.Card.ClearLogo"), this);
		clearLogo->setFlat(true);
		logoCol->addWidget(logo);
		logoCol->addWidget(clearLogo);
		logoCol->addStretch(1);
		mid->addLayout(logoCol);

		QFormLayout *form = new QFormLayout();
		form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		layoutBox = new QComboBox(this);
		FillCombo(layoutBox, spn_layout);
		form->addRow(QTStr("Sponsorcenter.Card.Layout"), layoutBox);
		QHBoxLayout *sizeRow = new QHBoxLayout();
		size = new QSlider(Qt::Horizontal, this);
		size->setRange(40, 250);
		size->setSingleStep(5);
		sizeLabel = new QLabel(this);
		sizeLabel->setMinimumWidth(44);
		sizeRow->addWidget(size, 1);
		sizeRow->addWidget(sizeLabel);
		form->addRow(QTStr("Sponsorcenter.Card.Size"), sizeRow);
		head = new QLineEdit(this);
		head->setPlaceholderText(QTStr("Sponsorcenter.Card.HeadHint"));
		form->addRow(QTStr("Sponsorcenter.Card.Head"), head);
		sub = new QLineEdit(this);
		sub->setPlaceholderText(QTStr("Sponsorcenter.Card.SubHint"));
		form->addRow(QTStr("Sponsorcenter.Card.Sub"), sub);
		badge = new QLineEdit(this);
		badge->setPlaceholderText(QTStr("Sponsorcenter.Card.BadgeHint"));
		form->addRow(QTStr("Sponsorcenter.Card.Badge"), badge);
		mid->addLayout(form, 1);
		v->addLayout(mid);

		/* its animations */
		QGridLayout *grid = new QGridLayout();
		grid->setHorizontalSpacing(8);
		auto combo = [this](const spn_option *opts) {
			QComboBox *c = new QComboBox(this);
			FillCombo(c, opts);
			return c;
		};
		textAnim = combo(spn_textAnim);
		logoAnim = combo(spn_logoAnim);
		enter = combo(spn_enter);
		leave = combo(spn_enter);
		showSec = new QDoubleSpinBox(this);
		showSec->setRange(0.5, 600.0);
		showSec->setSingleStep(0.5);
		showSec->setDecimals(1);
		showSec->setSuffix(" s");
		auto label = [this](const char *key) {
			return new QLabel(QTStr(key), this);
		};
		grid->addWidget(label("Sponsorcenter.Card.In"), 0, 0);
		grid->addWidget(enter, 0, 1);
		grid->addWidget(label("Sponsorcenter.Card.Out"), 0, 2);
		grid->addWidget(leave, 0, 3);
		grid->addWidget(label("Sponsorcenter.Card.TextAnim"), 1, 0);
		grid->addWidget(textAnim, 1, 1);
		grid->addWidget(label("Sponsorcenter.Card.LogoAnim"), 1, 2);
		grid->addWidget(logoAnim, 1, 3);
		grid->addWidget(label("Sponsorcenter.Card.ShowSec"), 2, 0);
		grid->addWidget(showSec, 2, 1);
		grid->setColumnStretch(1, 1);
		grid->setColumnStretch(3, 1);
		v->addLayout(grid);

		logo->picked = [this](const QString &file) {
			image = file;
			logo->Show(image);
			clearLogo->setVisible(true);
			Changed();
		};
		connect(clearLogo, &QPushButton::clicked, this, [this]() {
			image.clear();
			logo->Show(image);
			clearLogo->setVisible(false);
			Changed();
		});
		auto onChange = [this]() {
			Changed();
		};
		for (QComboBox *c : {layoutBox, textAnim, logoAnim, enter, leave}) {
			connect(c, &QComboBox::currentIndexChanged, this, onChange);
		}
		for (QLineEdit *e : {head, sub, badge}) {
			connect(e, &QLineEdit::textChanged, this, onChange);
		}
		connect(size, &QSlider::valueChanged, this, [this]() {
			sizeLabel->setText(QString("%1%").arg(size->value()));
			Changed();
		});
		connect(showSec, &QDoubleSpinBox::valueChanged, this, onChange);
		connect(showBox, &QCheckBox::toggled, this, onChange);
	}

	void Load(obs_data_t *it)
	{
		loading = true;
		image = QT_UTF8(obs_data_get_string(it, "image"));
		logo->Show(image);
		clearLogo->setVisible(!image.isEmpty());
		SetCombo(layoutBox, obs_data_get_string(it, "layout"));
		double sz = obs_data_get_double(it, "image_size");
		size->setValue(int((sz > 0.0 ? sz : 1.0) * 100.0 + 0.5));
		sizeLabel->setText(QString("%1%").arg(size->value()));
		head->setText(QT_UTF8(obs_data_get_string(it, "head")));
		sub->setText(QT_UTF8(obs_data_get_string(it, "sub")));
		badge->setText(QT_UTF8(obs_data_get_string(it, "badge")));
		SetCombo(textAnim, obs_data_get_string(it, "text_anim"));
		SetCombo(logoAnim, obs_data_get_string(it, "logo_anim"));
		SetCombo(enter, obs_data_get_string(it, "enter"));
		SetCombo(leave, obs_data_get_string(it, "exit"));
		double secs = obs_data_get_double(it, "show_sec");
		showSec->setValue(secs > 0.0 ? secs : 8.0);
		showBox->setChecked(!obs_data_get_bool(it, "off"));
		loading = false;
		UpdateTitle();
	}

	/* a new obs_data the caller releases */
	obs_data_t *Save() const
	{
		obs_data_t *it = obs_data_create();
		obs_data_set_string(it, "image", QT_TO_UTF8(image));
		obs_data_set_string(it, "layout", QT_TO_UTF8(layoutBox->currentData().toString()));
		obs_data_set_double(it, "image_size", size->value() / 100.0);
		obs_data_set_string(it, "head", QT_TO_UTF8(head->text()));
		obs_data_set_string(it, "sub", QT_TO_UTF8(sub->text()));
		obs_data_set_string(it, "badge", QT_TO_UTF8(badge->text()));
		obs_data_set_string(it, "text_anim", QT_TO_UTF8(textAnim->currentData().toString()));
		obs_data_set_string(it, "logo_anim", QT_TO_UTF8(logoAnim->currentData().toString()));
		obs_data_set_string(it, "enter", QT_TO_UTF8(enter->currentData().toString()));
		obs_data_set_string(it, "exit", QT_TO_UTF8(leave->currentData().toString()));
		obs_data_set_double(it, "show_sec", showSec->value());
		obs_data_set_bool(it, "off", !showBox->isChecked());
		return it;
	}

	void SetNumber(int n, int count)
	{
		number = n;
		up->setEnabled(n > 1);
		down->setEnabled(n < count);
		UpdateTitle();
	}

	void SetFocused(bool on)
	{
		setStyleSheet(on ? "#sponsorCard { border: 2px solid palette(highlight); border-radius: 8px; }"
				 : "#sponsorCard { border: 1px solid palette(mid); border-radius: 8px; }");
	}

	bool IsOn() const { return showBox->isChecked(); }
	void FocusHeadline() { head->setFocus(Qt::OtherFocusReason); }

private:
	QLabel *title = nullptr;
	QCheckBox *showBox = nullptr;
	QToolButton *up = nullptr, *down = nullptr;
	LogoDrop *logo = nullptr;
	QPushButton *clearLogo = nullptr;
	QComboBox *layoutBox = nullptr, *textAnim = nullptr, *logoAnim = nullptr, *enter = nullptr, *leave = nullptr;
	QSlider *size = nullptr;
	QLabel *sizeLabel = nullptr;
	QLineEdit *head = nullptr, *sub = nullptr, *badge = nullptr;
	QDoubleSpinBox *showSec = nullptr;
	QString image;
	int number = 1;
	bool loading = false;

	void UpdateTitle()
	{
		QString t = QTStr("Sponsorcenter.Card.Title").arg(number);
		const QString name = head->text().trimmed();
		if (!name.isEmpty()) {
			t += QString::fromUtf8(" \xE2\x80\x94 ") + name;
		} else if (!image.isEmpty()) {
			t += QString::fromUtf8(" \xE2\x80\x94 ") + QFileInfo(image).completeBaseName();
		}
		title->setText(t);
	}

	void Changed()
	{
		UpdateTitle();
		if (!loading && changed) {
			changed();
		}
	}
};

/* ------------------------------------------------------------------------ */

void SpazzSponsorCenter::Open(QWidget *parent, const QString &uuid)
{
	if (instance) {
		if (!uuid.isEmpty()) {
			instance->RefreshEditing(uuid);
		}
		instance->show();
		instance->raise();
		instance->activateWindow();
		return;
	}
	if (!obs_source_get_display_name(kSponsorId)) {
		OBSMessageBox::warning(parent, QTStr("Sponsorcenter.Title"), QTStr("Sponsorcenter.Missing"));
		return;
	}
	instance = new SpazzSponsorCenter(parent);
	if (!uuid.isEmpty()) {
		instance->RefreshEditing(uuid);
	}
	instance->show();
}

void SpazzSponsorCenter::RegisterOpenProc(QWidget *mainWindow)
{
	static QPointer<QWidget> owner;
	owner = mainWindow;
	auto open = [](void *, calldata_t *cd) {
		const QString uuid = QT_UTF8(calldata_string(cd, "uuid"));
		QMetaObject::invokeMethod(
			qApp,
			[uuid]() {
				if (owner) {
					SpazzSponsorCenter::Open(owner, uuid);
				}
			},
			Qt::QueuedConnection);
	};
	proc_handler_add(obs_get_proc_handler(), "void spazz_sponsorcenter_open(in string uuid)", open, nullptr);
}

SpazzSponsorCenter::SpazzSponsorCenter(QWidget *parent) : QDialog(parent, Qt::Window)
{
	setAttribute(Qt::WA_DeleteOnClose);
	setWindowTitle(QTStr("Sponsorcenter.Title"));
	setMinimumSize(960, 600);

	/* the spot being designed: private, sized to the canvas, and it
	 * replays every time a setting changes */
	OBSDataAutoRelease init = obs_data_create();
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		obs_data_set_int(init, "width", ovi.base_width);
		obs_data_set_int(init, "height", ovi.base_height);
	}
	obs_data_set_bool(init, "studio_preview", true);
	preview = obs_source_create_private(kSponsorId, "SPONSORCENTER preview", init);
	obs_source_inc_showing(preview);

	pushTimer = new QTimer(this);
	pushTimer->setSingleShot(true);
	pushTimer->setInterval(250);
	connect(pushTimer, &QTimer::timeout, this, &SpazzSponsorCenter::Push);

	/* ---- left: preview over the live scene ---- */
	QWidget *left = new QWidget(this);
	QVBoxLayout *ll = new QVBoxLayout(left);
	ll->setContentsMargins(0, 0, 0, 0);
	display = new OBSQTDisplay(left);
	display->setMinimumSize(480, 270);
	display->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	ll->addWidget(display, 1);

	QHBoxLayout *row = new QHBoxLayout();
	QPushButton *replay = new QPushButton(QTStr("Sponsorcenter.Replay"), left);
	replay->setToolTip(QTStr("Sponsorcenter.ReplayTip"));
	QLabel *hint = new QLabel(QTStr("Sponsorcenter.Hint"), left);
	hint->setWordWrap(true);
	hint->setProperty("class", "text-muted");
	row->addWidget(replay);
	row->addWidget(hint, 1);
	ll->addLayout(row);

	/* ---- right: the sponsors as cards, then the shared look ---- */
	QWidget *right = new QWidget(this);
	QVBoxLayout *rl = new QVBoxLayout(right);
	rl->setContentsMargins(0, 0, 0, 0);

	QFormLayout *top = new QFormLayout();
	editing = new QComboBox(right);
	top->addRow(QTStr("Sponsorcenter.Editing"), editing);
	rl->addLayout(top);

	QTabWidget *tabs = new QTabWidget(right);

	cardScroll = new QScrollArea(tabs);
	cardScroll->setWidgetResizable(true);
	cardScroll->setFrameShape(QFrame::NoFrame);
	QWidget *host = new QWidget(cardScroll);
	cardsLayout = new QVBoxLayout(host);
	cardsLayout->setContentsMargins(6, 6, 6, 6);
	cardsLayout->setSpacing(10);
	addButton = new QPushButton(QTStr("Sponsorcenter.AddSponsor"), host);
	addButton->setToolTip(QTStr("Sponsorcenter.AddSponsorTip"));
	addButton->setMinimumHeight(36);
	cardsLayout->addWidget(addButton);
	cardsLayout->addStretch(1);
	cardScroll->setWidget(host);
	tabs->addTab(cardScroll, QTStr("Sponsorcenter.Tab.Sponsors"));

	OBSDataAutoRelease settings = obs_source_get_settings(preview);
	view = new OBSPropertiesView(settings.Get(), preview.Get(), (PropertiesReloadCallback)obs_source_properties,
				     (PropertiesUpdateCallback) nullptr, (PropertiesVisualUpdateCb)obs_source_update);
	view->setMinimumWidth(400);
	tabs->addTab(view, QTStr("Sponsorcenter.Tab.Look"));
	rl->addWidget(tabs, 1);

	QFormLayout *bottom = new QFormLayout();
	nameEdit = new QLineEdit(right);
	bottom->addRow(QTStr("Sponsorcenter.Name"), nameEdit);
	rl->addLayout(bottom);

	QHBoxLayout *buttons = new QHBoxLayout();
	status = new QLabel(right);
	status->setWordWrap(true);
	liveButton = new QPushButton(QTStr("Sponsorcenter.PlayLive"), right);
	saveButton = new QPushButton(right);
	saveButton->setDefault(true);
	QPushButton *close = new QPushButton(QTStr("Close"), right);
	buttons->addWidget(status, 1);
	buttons->addWidget(liveButton);
	buttons->addWidget(saveButton);
	buttons->addWidget(close);
	rl->addLayout(buttons);

	QSplitter *split = new QSplitter(Qt::Horizontal, this);
	split->addWidget(left);
	split->addWidget(right);
	split->setStretchFactor(0, 3);
	split->setStretchFactor(1, 2);
	split->setChildrenCollapsible(false);
	QVBoxLayout *main = new QVBoxLayout(this);
	main->addWidget(split);

	const char *geom = config_get_string(App()->GetUserConfig(), "SpazzSponsorCenter", "geometry");
	if (!(geom && *geom && restoreGeometry(QByteArray::fromBase64(QByteArray(geom))))) {
		resize(1320, 780);
	}

	RefreshEditing();

	connect(display, &OBSQTDisplay::DisplayCreated, this,
		[this]() { obs_display_add_draw_callback(display->GetDisplay(), SpazzSponsorCenter::Draw, this); });
	connect(editing, &QComboBox::currentIndexChanged, this, &SpazzSponsorCenter::EditingChanged);
	connect(replay, &QPushButton::clicked, this, &SpazzSponsorCenter::Replay);
	connect(saveButton, &QPushButton::clicked, this, &SpazzSponsorCenter::Save);
	connect(liveButton, &QPushButton::clicked, this, &SpazzSponsorCenter::PlayLive);
	connect(addButton, &QPushButton::clicked, this, &SpazzSponsorCenter::AddSponsor);
	connect(close, &QPushButton::clicked, this, &QDialog::close);
	/* a preset in the Look tab restyles every sponsor's motion too */
	connect(view, &OBSPropertiesView::Changed, this, [this]() {
		status->clear();
		SyncCardsFromSettings();
	});
	/* clicking into a sponsor's card shows that sponsor in the preview */
	connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
		for (QWidget *w = now; w; w = w->parentWidget()) {
			if (SponsorCard *card = dynamic_cast<SponsorCard *>(w)) {
				Touched(card);
				return;
			}
			if (w == this) {
				return;
			}
		}
	});
}

SpazzSponsorCenter::~SpazzSponsorCenter()
{
	if (display && display->GetDisplay()) {
		obs_display_remove_draw_callback(display->GetDisplay(), SpazzSponsorCenter::Draw, this);
	}
	if (preview) {
		obs_source_dec_showing(preview);
	}
}

void SpazzSponsorCenter::closeEvent(QCloseEvent *e)
{
	config_set_string(App()->GetUserConfig(), "SpazzSponsorCenter", "geometry",
			  saveGeometry().toBase64().constData());
	QDialog::closeEvent(e);
}

/* ---- which spot we're editing ---- */

void SpazzSponsorCenter::RefreshEditing(const QString &selectUuid)
{
	QSignalBlocker block(editing);
	editing->clear();
	editing->addItem(QTStr("Sponsorcenter.New"), QString());

	auto add = [](void *param, obs_source_t *src) {
		if (strcmp(obs_source_get_unversioned_id(src), kSponsorId) == 0) {
			QComboBox *box = static_cast<QComboBox *>(param);
			box->addItem(QT_UTF8(obs_source_get_name(src)), QT_UTF8(obs_source_get_uuid(src)));
		}
		return true;
	};
	obs_enum_sources(add, editing);

	int idx = selectUuid.isEmpty() ? 0 : editing->findData(selectUuid);
	editing->setCurrentIndex(idx < 0 ? 0 : idx);
	EditingChanged();
}

void SpazzSponsorCenter::EditingChanged()
{
	const QString uuid = editing->currentData().toString();
	OBSSourceAutoRelease existing = uuid.isEmpty() ? nullptr : obs_get_source_by_uuid(QT_TO_UTF8(uuid));

	OBSDataAutoRelease mine = obs_source_get_settings(preview);
	if (existing) {
		OBSDataAutoRelease theirs = obs_source_get_settings(existing);
		obs_data_clear(mine);
		obs_data_apply(mine, theirs);
		CloneItems(mine);
		obs_data_set_bool(mine, "studio_preview", true);
		nameEdit->setText(QT_UTF8(obs_source_get_name(existing)));
		nameEdit->setEnabled(false);
		saveButton->setText(QTStr("Sponsorcenter.Update"));
		liveButton->setEnabled(true);
	} else {
		nameEdit->setText(UniqueName(QTStr("Sponsorcenter.DefaultName")));
		nameEdit->setEnabled(true);
		saveButton->setText(QTStr("Sponsorcenter.AddToScene"));
		liveButton->setEnabled(false);
	}

	focusCard = -1;
	lastTouched = -1;
	obs_data_set_int(mine, "studio_focus", -1);
	obs_source_update(preview, nullptr);
	LoadCards();
	view->ReloadProperties();
	status->clear();
}

QString SpazzSponsorCenter::UniqueName(const QString &base) const
{
	QString name = base;
	for (int i = 2;; i++) {
		OBSSourceAutoRelease taken = obs_get_source_by_name(QT_TO_UTF8(name));
		if (!taken) {
			return name;
		}
		name = QString("%1 %2").arg(base).arg(i);
	}
}

/* ---- the sponsor cards ---- */

int SpazzSponsorCenter::IndexOf(const SponsorCard *card) const
{
	for (size_t i = 0; i < cards.size(); i++) {
		if (cards[i] == card) {
			return int(i);
		}
	}
	return -1;
}

SponsorCard *SpazzSponsorCenter::MakeCard(obs_data_t *item)
{
	SponsorCard *card = new SponsorCard(cardScroll->widget());
	card->Load(item);
	card->changed = [this, card]() {
		CardChanged(card);
	};
	card->action = [this, card](int act) {
		CardAction(card, act);
	};
	return card;
}

void SpazzSponsorCenter::LoadCards()
{
	for (SponsorCard *c : cards) {
		c->deleteLater();
	}
	cards.clear();

	OBSDataAutoRelease settings = obs_source_get_settings(preview);
	OBSDataArrayAutoRelease items = obs_data_get_array(settings, "items");
	for (size_t i = 0; i < obs_data_array_count(items); i++) {
		OBSDataAutoRelease it = obs_data_array_item(items, i);
		cards.push_back(MakeCard(it));
	}
	Relayout();
}

void SpazzSponsorCenter::SyncCardsFromSettings()
{
	OBSDataAutoRelease settings = obs_source_get_settings(preview);
	OBSDataArrayAutoRelease items = obs_data_get_array(settings, "items");
	if (obs_data_array_count(items) != cards.size()) {
		LoadCards();
		return;
	}
	for (size_t i = 0; i < cards.size(); i++) {
		OBSDataAutoRelease it = obs_data_array_item(items, i);
		OBSDataAutoRelease mine = cards[i]->Save();
		if (strcmp(obs_data_get_json(it), obs_data_get_json(mine)) != 0) {
			cards[i]->Load(it);
		}
	}
}

void SpazzSponsorCenter::Relayout()
{
	/* cards first, then the "+" button, then the stretch */
	for (SponsorCard *c : cards) {
		cardsLayout->removeWidget(c);
	}
	const int n = int(cards.size());
	for (int i = 0; i < n; i++) {
		cardsLayout->insertWidget(i, cards[size_t(i)]);
		cards[size_t(i)]->SetNumber(i + 1, n);
		cards[size_t(i)]->SetFocused(i == focusCard);
		cards[size_t(i)]->show();
	}
}

/* every edit goes straight into the preview's settings */
void SpazzSponsorCenter::WriteItems()
{
	OBSDataAutoRelease settings = obs_source_get_settings(preview);
	OBSDataArrayAutoRelease items = obs_data_array_create();
	for (SponsorCard *c : cards) {
		OBSDataAutoRelease it = c->Save();
		obs_data_array_push_back(items, it);
	}
	obs_data_set_array(settings, "items", items);

	/* the page only counts sponsors that are switched on */
	int pageIndex = -1;
	if (focusCard >= 0 && focusCard < int(cards.size()) && cards[size_t(focusCard)]->IsOn()) {
		pageIndex = 0;
		for (int i = 0; i < focusCard; i++) {
			if (cards[size_t(i)]->IsOn()) {
				pageIndex++;
			}
		}
	}
	obs_data_set_int(settings, "studio_focus", pageIndex);
}

void SpazzSponsorCenter::Push()
{
	pushTimer->stop();
	obs_source_update(preview, nullptr);
}

void SpazzSponsorCenter::CardChanged(SponsorCard *card)
{
	const int idx = IndexOf(card);
	if (idx >= 0) {
		lastTouched = idx;
		if (focusCard != idx) {
			focusCard = idx;
			for (size_t i = 0; i < cards.size(); i++) {
				cards[i]->SetFocused(int(i) == focusCard);
			}
		}
	}
	WriteItems();
	pushTimer->start();
	status->clear();
}

void SpazzSponsorCenter::Touched(SponsorCard *card)
{
	const int idx = IndexOf(card);
	if (idx < 0) {
		return;
	}
	lastTouched = idx;
	if (focusCard == idx) {
		return;
	}
	focusCard = idx;
	for (size_t i = 0; i < cards.size(); i++) {
		cards[i]->SetFocused(int(i) == focusCard);
	}
	WriteItems();
	Push();
}

void SpazzSponsorCenter::CardAction(SponsorCard *card, int action)
{
	const int idx = IndexOf(card);
	if (idx < 0) {
		return;
	}
	const int n = int(cards.size());

	switch (action) {
	case 0: /* up */
	case 1: /* down */ {
		const int to = action == 0 ? idx - 1 : idx + 1;
		if (to < 0 || to >= n) {
			return;
		}
		std::swap(cards[size_t(idx)], cards[size_t(to)]);
		focusCard = to;
		lastTouched = to;
		break;
	}
	case 2: /* duplicate: an exact copy right after it */ {
		OBSDataAutoRelease copy = card->Save();
		SponsorCard *dup = MakeCard(copy);
		cards.insert(cards.begin() + idx + 1, dup);
		focusCard = idx + 1;
		lastTouched = idx + 1;
		break;
	}
	case 3: /* delete */
		cards.erase(cards.begin() + idx);
		card->deleteLater();
		focusCard = -1;
		if (lastTouched >= int(cards.size()) || lastTouched == idx) {
			lastTouched = -1;
		}
		break;
	default:
		return;
	}
	Relayout();
	WriteItems();
	Push();
}

void SpazzSponsorCenter::AddSponsor()
{
	/* the new sponsor starts as a copy of the one you last worked on (or
	 * the last one): same layout, size, animations and timing, no logo or text */
	OBSDataAutoRelease item;
	const int from = lastTouched >= 0 && lastTouched < int(cards.size()) ? lastTouched : int(cards.size()) - 1;
	if (from >= 0) {
		item = cards[size_t(from)]->Save();
	} else {
		OBSDataAutoRelease settings = obs_source_get_settings(preview);
		item = obs_data_create();
		obs_data_set_string(item, "layout", "left");
		obs_data_set_double(item, "image_size", 1.0);
		obs_data_set_double(item, "show_sec", 8.0);
		for (const char *k : {"enter", "exit", "text_anim", "logo_anim"}) {
			obs_data_set_string(item, k, obs_data_get_string(settings, k));
		}
	}
	for (const char *k : {"image", "head", "sub", "badge"}) {
		obs_data_set_string(item, k, "");
	}
	obs_data_set_bool(item, "off", false);

	SponsorCard *card = MakeCard(item);
	cards.push_back(card);
	focusCard = int(cards.size()) - 1;
	lastTouched = focusCard;
	Relayout();
	WriteItems();
	Push();

	QTimer::singleShot(0, this, [this, card]() {
		cardScroll->ensureWidgetVisible(card, 0, 20);
		card->FocusHeadline();
	});
}

/* ---- preview ---- */

void SpazzSponsorCenter::Replay()
{
	/* the whole run, every sponsor in order */
	focusCard = -1;
	for (SponsorCard *c : cards) {
		c->SetFocused(false);
	}
	WriteItems();
	Push();
}

void SpazzSponsorCenter::Draw(void *data, uint32_t cx, uint32_t cy)
{
	SpazzSponsorCenter *w = static_cast<SpazzSponsorCenter *>(data);
	obs_source_t *sp = w->preview.Get();
	if (!sp) {
		return;
	}

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		return;
	}
	const uint32_t baseCX = std::max(ovi.base_width, 1u);
	const uint32_t baseCY = std::max(ovi.base_height, 1u);

	int x, y;
	float scale;
	GetScaleAndCenterPos(baseCX, baseCY, cx, cy, x, y, scale);
	int newCX = int(scale * float(baseCX));
	int newCY = int(scale * float(baseCY));

	gs_viewport_push();
	gs_projection_push();
	gs_ortho(0.0f, float(baseCX), 0.0f, float(baseCY), -100.0f, 100.0f);
	gs_set_viewport(x, y, newCX, newCY);

	/* what's live right now (the program output), so you see the
	 * sponsor where it will sit */
	OBSSourceAutoRelease program = obs_get_output_source(0);
	if (program) {
		obs_source_video_render(program);
	}

	/* the sponsor, stretched to the canvas like a full-screen overlay */
	const uint32_t spCX = std::max(obs_source_get_width(sp), 1u);
	const uint32_t spCY = std::max(obs_source_get_height(sp), 1u);
	gs_matrix_push();
	gs_matrix_scale3f(float(baseCX) / float(spCX), float(baseCY) / float(spCY), 1.0f);
	obs_source_video_render(sp);
	gs_matrix_pop();

	gs_projection_pop();
	gs_viewport_pop();
}

/* ---- saving ---- */

void SpazzSponsorCenter::Save()
{
	OBSDataAutoRelease settings = obs_data_create();
	OBSDataAutoRelease mine = obs_source_get_settings(preview);
	obs_data_apply(settings, mine);
	CloneItems(settings);
	obs_data_erase(settings, "studio_preview");
	obs_data_erase(settings, "studio_focus");

	const QString uuid = editing->currentData().toString();
	if (!uuid.isEmpty()) {
		OBSSourceAutoRelease existing = obs_get_source_by_uuid(QT_TO_UTF8(uuid));
		if (!existing) {
			status->setText(QTStr("Sponsorcenter.Gone"));
			RefreshEditing();
			return;
		}
		obs_source_update(existing, settings);
		status->setText(QTStr("Sponsorcenter.Updated").arg(QT_UTF8(obs_source_get_name(existing))));
	} else {
		OBSSourceAutoRelease sceneSource = obs_frontend_get_current_scene();
		obs_scene_t *scene = obs_scene_from_source(sceneSource);
		if (!scene) {
			status->setText(QTStr("Sponsorcenter.NoScene"));
			return;
		}

		QString name = nameEdit->text().trimmed();
		if (name.isEmpty()) {
			name = QTStr("Sponsorcenter.DefaultName");
		}
		name = UniqueName(name);

		/* full canvas, top left: Position decides where it shows */
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi)) {
			obs_data_set_int(settings, "width", ovi.base_width);
			obs_data_set_int(settings, "height", ovi.base_height);
		}

		OBSSourceAutoRelease created = obs_source_create(kSponsorId, QT_TO_UTF8(name), settings, nullptr);
		if (!created) {
			status->setText(QTStr("Sponsorcenter.Failed"));
			return;
		}
		obs_scene_add(scene, created);
		status->setText(QTStr("Sponsorcenter.Added").arg(name, QT_UTF8(obs_source_get_name(sceneSource))));
		RefreshEditing(QT_UTF8(obs_source_get_uuid(created)));
	}

	if (OBSBasic *main = OBSBasic::Get()) {
		main->SaveProject();
	}
}

void SpazzSponsorCenter::PlayLive()
{
	const QString uuid = editing->currentData().toString();
	OBSSourceAutoRelease existing = uuid.isEmpty() ? nullptr : obs_get_source_by_uuid(QT_TO_UTF8(uuid));
	if (!existing) {
		return;
	}
	calldata_t cd;
	calldata_init(&cd);
	proc_handler_call(obs_source_get_proc_handler(existing), "play", &cd);
	calldata_free(&cd);
	status->setText(QTStr("Sponsorcenter.Playing").arg(QT_UTF8(obs_source_get_name(existing))));
}
