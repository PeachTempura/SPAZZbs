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
#include <docks/OBSDock.hpp>
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
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>
#include <QWheelEvent>
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

#include <algorithm>
#include <cmath>
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

static const QString kImageFilter =
	"Logos and videos (*.png *.jpg *.jpeg *.gif *.webp *.svg *.bmp *.mp4 *.webm *.mov *.m4v);;All files (*.*)";

static bool IsVideoFile(const QString &path)
{
	static const QStringList exts = {"mp4", "webm", "mov", "m4v"};
	return exts.contains(QFileInfo(path).suffix().toLower());
}

static bool IsMediaFile(const QString &path)
{
	static const QStringList exts = {"png", "jpg", "jpeg", "gif", "webp", "svg", "bmp"};
	return exts.contains(QFileInfo(path).suffix().toLower()) || IsVideoFile(path);
}

static const char *MaskPath(const QString &id)
{
	for (const spn_shape *m = spn_masks; m->id; m++) {
		if (id == QLatin1String(m->id)) {
			return m->path;
		}
	}
	return nullptr;
}

/* how the page cuts the picture, so the card thumbnail can show the same */
struct LogoLook {
	QString mask = "frame";
	QString aspect = "square";
	QString fit = "contain";
	double zoom = 1.0, panX = 0.0, panY = 0.0;
};

/* ------------------------------------------------------------------------ */
/* the logo: drop a file on it, or click to pick one. With a picture in it,  */
/* drag to move the crop and scroll to zoom; it shows the shape it'll have. */

class LogoDrop : public QWidget {
public:
	std::function<void(const QString &)> picked;
	std::function<void(double panX, double panY, double zoom)> cropped;

	explicit LogoDrop(QWidget *parent) : QWidget(parent)
	{
		setAcceptDrops(true);
		setFixedSize(116, 116);
		setCursor(Qt::PointingHandCursor);
		setToolTip(QTStr("Sponsorcenter.Card.LogoTip"));
	}

	void Show(const QString &file)
	{
		path = file;
		video = IsVideoFile(file);
		pix = QPixmap();
		if (!file.isEmpty() && !video) {
			pix.load(file);
		}
		setCursor(pix.isNull() ? Qt::PointingHandCursor : Qt::OpenHandCursor);
		update();
	}

	void SetLook(const LogoLook &l)
	{
		look = l;
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		const QRectF area = QRectF(rect()).adjusted(4, 4, -4, -4);

		if (path.isEmpty()) {
			p.setPen(QPen(palette().color(QPalette::Mid), 2, Qt::DashLine));
			p.drawRoundedRect(area, 8, 8);
			p.setPen(palette().color(QPalette::WindowText));
			p.drawText(area, Qt::AlignCenter | Qt::TextWordWrap, QTStr("Sponsorcenter.Card.DropLogo"));
			return;
		}

		const QRectF box = BoxRect(area);

		/* the picture, cut like the page cuts it */
		const qreal dpr = devicePixelRatioF();
		QImage layer(QSize(int(box.width() * dpr), int(box.height() * dpr)),
			     QImage::Format_ARGB32_Premultiplied);
		layer.setDevicePixelRatio(dpr);
		layer.fill(Qt::transparent);
		{
			QPainter lp(&layer);
			lp.setRenderHint(QPainter::Antialiasing);
			lp.setRenderHint(QPainter::SmoothPixmapTransform);
			const QRectF b(0, 0, box.width(), box.height());
			if (look.mask == "frame") {
				lp.fillRect(b, QColor(255, 255, 255, 30));
			}
			if (!pix.isNull()) {
				lp.drawPixmap(PictureRect(b), pix, QRectF(pix.rect()));
			} else {
				/* videos: no frame to show here; the preview plays it */
				QLinearGradient g(b.topLeft(), b.bottomRight());
				g.setColorAt(0, QColor(60, 40, 90));
				g.setColorAt(1, QColor(20, 15, 35));
				lp.fillRect(b, g);
				lp.setPen(Qt::white);
				lp.drawText(b.adjusted(4, 4, -4, -4), Qt::AlignCenter | Qt::TextWordWrap,
					    QString::fromUtf8("\xE2\x96\xB6 ") + QFileInfo(path).fileName());
			}
			/* keep only what's inside the shape */
			lp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
			const char *svgPath = MaskPath(look.mask);
			if (svgPath) {
				QByteArray svg =
					QByteArray("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100' "
						   "preserveAspectRatio='none'><path d='") +
					svgPath + "'/></svg>";
				QSvgRenderer r(svg);
				r.render(&lp, b);
			} else {
				QPainterPath clip;
				clip.addRoundedRect(b, look.mask == "frame" ? 12 : 0, look.mask == "frame" ? 12 : 0);
				lp.fillPath(clip, Qt::black);
			}
		}

		/* chequerboard behind, so transparent PNGs read as transparent */
		p.save();
		p.setClipRect(box);
		const int cell = 8;
		for (int y = 0; y * cell < box.height(); y++) {
			for (int x = 0; x * cell < box.width(); x++) {
				p.fillRect(QRectF(box.left() + x * cell, box.top() + y * cell, cell, cell),
					   (x + y) % 2 ? QColor(70, 70, 80, 90) : QColor(110, 110, 120, 90));
			}
		}
		p.restore();
		p.drawImage(box.topLeft(), layer);
		p.setPen(QPen(palette().color(QPalette::Mid), 1, Qt::DotLine));
		p.setBrush(Qt::NoBrush);
		p.drawRect(box);
	}

	void dragEnterEvent(QDragEnterEvent *e) override
	{
		if (!FirstMedia(e->mimeData()).isEmpty()) {
			e->acceptProposedAction();
		}
	}
	void dropEvent(QDropEvent *e) override
	{
		const QString file = FirstMedia(e->mimeData());
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
		if (path.isEmpty()) {
			Browse();
			return;
		}
		dragFrom = e->position();
		dragging = true;
		setCursor(Qt::ClosedHandCursor);
	}
	void mouseMoveEvent(QMouseEvent *e) override
	{
		if (!dragging || pix.isNull()) {
			return;
		}
		/* move the picture with the mouse: the inverse of the page's
		 * object-position + scale(zoom) maths */
		const QRectF b = BoxRect(QRectF(rect()).adjusted(4, 4, -4, -4));
		const QSizeF d = DrawnSize(b.size());
		const QPointF delta = e->position() - dragFrom;
		dragFrom = e->position();
		auto step = [](double box, double drawn, double z, double moved) {
			const double denom = box - drawn * z;
			return std::abs(denom) < 1.0 ? 0.0 : 2.0 * moved / denom;
		};
		look.panX = std::clamp(look.panX + step(b.width(), d.width(), look.zoom, delta.x()), -1.0, 1.0);
		look.panY = std::clamp(look.panY + step(b.height(), d.height(), look.zoom, delta.y()), -1.0, 1.0);
		update();
		if (cropped) {
			cropped(look.panX, look.panY, look.zoom);
		}
	}
	void mouseReleaseEvent(QMouseEvent *) override
	{
		dragging = false;
		setCursor(path.isEmpty() ? Qt::PointingHandCursor : Qt::OpenHandCursor);
	}
	void mouseDoubleClickEvent(QMouseEvent *) override { Browse(); }
	void wheelEvent(QWheelEvent *e) override
	{
		if (path.isEmpty()) {
			return;
		}
		const double steps = e->angleDelta().y() / 120.0;
		look.zoom = std::clamp(look.zoom * std::pow(1.1, steps), 1.0, 5.0);
		update();
		if (cropped) {
			cropped(look.panX, look.panY, look.zoom);
		}
		e->accept();
	}

private:
	QString path;
	QPixmap pix;
	bool video = false;
	LogoLook look;
	bool dragging = false;
	QPointF dragFrom;

	void Browse()
	{
		const QString file = QFileDialog::getOpenFileName(this, QTStr("Sponsorcenter.Card.PickLogo"), QString(),
								  kImageFilter);
		if (!file.isEmpty() && picked) {
			picked(file);
		}
	}

	/* the logo's box inside the thumbnail, in the chosen proportions */
	QRectF BoxRect(const QRectF &area) const
	{
		double ar = 1.0;
		if (look.aspect == "wide") {
			ar = 16.0 / 9.0;
		} else if (look.aspect == "landscape") {
			ar = 4.0 / 3.0;
		} else if (look.aspect == "portrait") {
			ar = 3.0 / 4.0;
		} else if (look.aspect == "original" && !pix.isNull() && pix.height() > 0) {
			ar = double(pix.width()) / double(pix.height());
		}
		double w = area.width(), h = area.height();
		if (ar >= 1.0) {
			h = w / ar;
		} else {
			w = h * ar;
		}
		return QRectF(area.center().x() - w / 2, area.center().y() - h / 2, w, h);
	}

	/* the picture's size in the box before zoom (object-fit) */
	QSizeF DrawnSize(const QSizeF &box) const
	{
		if (pix.isNull()) {
			return box;
		}
		const double sx = box.width() / pix.width(), sy = box.height() / pix.height();
		const double sc = look.fit == "cover" ? std::max(sx, sy) : std::min(sx, sy);
		return QSizeF(pix.width() * sc, pix.height() * sc);
	}

	/* object-position + transform: scale(zoom) around that same point */
	QRectF PictureRect(const QRectF &b) const
	{
		const QSizeF d = DrawnSize(b.size());
		const double px = 0.5 + look.panX * 0.5, py = 0.5 + look.panY * 0.5;
		const double x = px * (b.width() - d.width() * look.zoom);
		const double y = py * (b.height() - d.height() * look.zoom);
		return QRectF(x, y, d.width() * look.zoom, d.height() * look.zoom);
	}

	static QString FirstMedia(const QMimeData *md)
	{
		if (!md || !md->hasUrls()) {
			return QString();
		}
		for (const QUrl &u : md->urls()) {
			const QString f = u.toLocalFile();
			if (!f.isEmpty() && IsMediaFile(f)) {
				return f;
			}
		}
		return QString();
	}
};

/* ------------------------------------------------------------------------ */
/* one sponsor: its picture and how it's cut, and its two lines of text.     */
/* How it moves comes from the Look & motion tab, the same for every one.   */

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

		/* how the picture is cut */
		QHBoxLayout *shapeRow = new QHBoxLayout();
		maskBox = new QComboBox(this);
		FillCombo(maskBox, spn_mask);
		maskBox->setToolTip(QTStr("Sponsorcenter.Card.ShapeTip"));
		aspectBox = new QComboBox(this);
		FillCombo(aspectBox, spn_aspect);
		aspectBox->setToolTip(QTStr("Sponsorcenter.Card.BoxTip"));
		shapeRow->addWidget(maskBox, 1);
		shapeRow->addWidget(aspectBox, 1);
		form->addRow(QTStr("Sponsorcenter.Card.Shape"), shapeRow);

		QHBoxLayout *cropRow = new QHBoxLayout();
		fitBox = new QComboBox(this);
		FillCombo(fitBox, spn_fit);
		zoom = new QSlider(Qt::Horizontal, this);
		zoom->setRange(100, 500);
		zoom->setToolTip(QTStr("Sponsorcenter.Card.ZoomTip"));
		QPushButton *resetCrop = new QPushButton(QTStr("Sponsorcenter.Card.ResetCrop"), this);
		resetCrop->setFlat(true);
		cropRow->addWidget(fitBox, 1);
		cropRow->addWidget(zoom, 1);
		cropRow->addWidget(resetCrop);
		form->addRow(QTStr("Sponsorcenter.Card.Crop"), cropRow);

		QHBoxLayout *moveRow = new QHBoxLayout();
		panX = new QSlider(Qt::Horizontal, this);
		panY = new QSlider(Qt::Horizontal, this);
		for (QSlider *sl : {panX, panY}) {
			sl->setRange(-100, 100);
			moveRow->addWidget(sl, 1);
		}
		panX->setToolTip(QTStr("Sponsorcenter.Card.MoveX"));
		panY->setToolTip(QTStr("Sponsorcenter.Card.MoveY"));
		form->addRow(QTStr("Sponsorcenter.Card.Move"), moveRow);

		/* videos: how loud */
		QHBoxLayout *soundRow = new QHBoxLayout();
		volume = new QSlider(Qt::Horizontal, this);
		volume->setRange(0, 100);
		volumeLabel = new QLabel(this);
		volumeLabel->setMinimumWidth(44);
		soundRow->addWidget(volume, 1);
		soundRow->addWidget(volumeLabel);
		soundLabel = new QLabel(QTStr("Sponsorcenter.Card.Sound"), this);
		form->addRow(soundLabel, soundRow);
		soundWidgets = {soundLabel, volume, volumeLabel};

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

		logo->picked = [this](const QString &file) {
			image = file;
			logo->Show(image);
			UpdateMediaRows();
			Changed();
		};
		/* dragging / scrolling the thumbnail moves the sliders */
		logo->cropped = [this](double x, double y, double z) {
			QSignalBlocker b1(panX), b2(panY), b3(zoom);
			panX->setValue(int(std::lround(x * 100.0)));
			panY->setValue(int(std::lround(y * 100.0)));
			zoom->setValue(int(std::lround(z * 100.0)));
			Changed();
		};
		connect(clearLogo, &QPushButton::clicked, this, [this]() {
			image.clear();
			logo->Show(image);
			UpdateMediaRows();
			Changed();
		});
		connect(resetCrop, &QPushButton::clicked, this, [this]() {
			QSignalBlocker b1(panX), b2(panY), b3(zoom);
			panX->setValue(0);
			panY->setValue(0);
			zoom->setValue(100);
			LookChanged();
		});
		for (QComboBox *c : {maskBox, aspectBox, fitBox}) {
			connect(c, &QComboBox::currentIndexChanged, this, [this]() { LookChanged(); });
		}
		for (QSlider *sl : {zoom, panX, panY}) {
			connect(sl, &QSlider::valueChanged, this, [this]() { LookChanged(); });
		}
		connect(volume, &QSlider::valueChanged, this, [this]() {
			volumeLabel->setText(QString("%1%").arg(volume->value()));
			Changed();
		});
		auto onChange = [this]() {
			Changed();
		};
		for (QComboBox *c : {layoutBox}) {
			connect(c, &QComboBox::currentIndexChanged, this, onChange);
		}
		for (QLineEdit *e : {head, sub, badge}) {
			connect(e, &QLineEdit::textChanged, this, onChange);
		}
		connect(size, &QSlider::valueChanged, this, [this]() {
			sizeLabel->setText(QString("%1%").arg(size->value()));
			Changed();
		});
		connect(showBox, &QCheckBox::toggled, this, onChange);
	}

	void Load(obs_data_t *it)
	{
		loading = true;
		image = QT_UTF8(obs_data_get_string(it, "image"));
		logo->Show(image);
		const char *mask = obs_data_get_string(it, "mask");
		SetCombo(maskBox, *mask ? mask : "frame");
		const char *aspect = obs_data_get_string(it, "aspect");
		SetCombo(aspectBox, *aspect ? aspect : "square");
		const char *fit = obs_data_get_string(it, "fit");
		SetCombo(fitBox, *fit ? fit : "contain");
		const double z = obs_data_get_double(it, "zoom");
		zoom->setValue(int(std::lround((z >= 1.0 ? z : 1.0) * 100.0)));
		panX->setValue(int(std::lround(obs_data_get_double(it, "pan_x") * 100.0)));
		panY->setValue(int(std::lround(obs_data_get_double(it, "pan_y") * 100.0)));
		const double vol = obs_data_has_user_value(it, "volume") ? obs_data_get_double(it, "volume") : 1.0;
		volume->setValue(int(std::lround(vol * 100.0)));
		volumeLabel->setText(QString("%1%").arg(volume->value()));
		UpdateMediaRows();
		logo->SetLook(Look());
		SetCombo(layoutBox, obs_data_get_string(it, "layout"));
		double sz = obs_data_get_double(it, "image_size");
		size->setValue(int((sz > 0.0 ? sz : 1.0) * 100.0 + 0.5));
		sizeLabel->setText(QString("%1%").arg(size->value()));
		head->setText(QT_UTF8(obs_data_get_string(it, "head")));
		sub->setText(QT_UTF8(obs_data_get_string(it, "sub")));
		badge->setText(QT_UTF8(obs_data_get_string(it, "badge")));
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
		obs_data_set_bool(it, "off", !showBox->isChecked());
		obs_data_set_string(it, "mask", QT_TO_UTF8(maskBox->currentData().toString()));
		obs_data_set_string(it, "aspect", QT_TO_UTF8(aspectBox->currentData().toString()));
		obs_data_set_string(it, "fit", QT_TO_UTF8(fitBox->currentData().toString()));
		obs_data_set_double(it, "zoom", zoom->value() / 100.0);
		obs_data_set_double(it, "pan_x", panX->value() / 100.0);
		obs_data_set_double(it, "pan_y", panY->value() / 100.0);
		obs_data_set_double(it, "volume", volume->value() / 100.0);
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
	QComboBox *layoutBox = nullptr;
	QSlider *size = nullptr;
	QLabel *sizeLabel = nullptr;
	QLineEdit *head = nullptr, *sub = nullptr, *badge = nullptr;
	QComboBox *maskBox = nullptr, *aspectBox = nullptr, *fitBox = nullptr;
	QSlider *zoom = nullptr, *panX = nullptr, *panY = nullptr, *volume = nullptr;
	QLabel *volumeLabel = nullptr, *soundLabel = nullptr;
	std::vector<QWidget *> soundWidgets;
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

	LogoLook Look() const
	{
		LogoLook l;
		l.mask = maskBox->currentData().toString();
		l.aspect = aspectBox->currentData().toString();
		l.fit = fitBox->currentData().toString();
		l.zoom = zoom->value() / 100.0;
		l.panX = panX->value() / 100.0;
		l.panY = panY->value() / 100.0;
		return l;
	}

	void LookChanged()
	{
		logo->SetLook(Look());
		Changed();
	}

	/* the logo-only rows show when there's a logo; Sound only for videos */
	void UpdateMediaRows()
	{
		clearLogo->setVisible(!image.isEmpty());
		const bool vid = IsVideoFile(image);
		for (QWidget *w : soundWidgets) {
			w->setVisible(vid);
		}
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
/* the one Sponsors source                                                   */

static constexpr const char *kMarker = "spazz_sponsorcenter";

static QString UniqueName(const QString &base)
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

static std::string ItemKey(obs_data_t *it)
{
	std::string k = obs_data_get_string(it, "image");
	k += '\n';
	k += obs_data_get_string(it, "head");
	k += '\n';
	k += obs_data_get_string(it, "sub");
	return k;
}

/* every scene `extra` sits in gets `main` in its place (same spot, size and
 * order), unless that scene has `main` already */
static void ReplaceInScenes(obs_source_t *extra, obs_source_t *main)
{
	struct Hit {
		OBSScene scene;
		OBSSceneItem item;
	};
	struct Ctx {
		obs_source_t *extra;
		std::vector<Hit> hits;
	} ctx{extra, {}};

	/* groups too: obs_enum_scenes lists them, and a spot inside a group
	 * would otherwise just vanish when the extra source is removed */
	obs_enum_scenes(
		[](void *param, obs_source_t *src) {
			obs_scene_t *scene = obs_group_or_scene_from_source(src);
			if (!scene) {
				return true;
			}
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *sc, obs_sceneitem_t *item, void *p) {
					Ctx *c = static_cast<Ctx *>(p);
					if (obs_sceneitem_get_source(item) == c->extra) {
						c->hits.push_back({OBSScene(sc), OBSSceneItem(item)});
					}
					return true;
				},
				param);
			return true;
		},
		&ctx);

	for (const Hit &h : ctx.hits) {
		if (!obs_scene_find_source(h.scene, obs_source_get_name(main))) {
			struct obs_transform_info info;
			struct obs_sceneitem_crop crop;
			obs_sceneitem_get_info2(h.item, &info);
			obs_sceneitem_get_crop(h.item, &crop);
			const int order = obs_sceneitem_get_order_position(h.item);
			obs_sceneitem_t *added = obs_scene_add(h.scene, main);
			if (added) {
				obs_sceneitem_set_info2(added, &info);
				obs_sceneitem_set_crop(added, &crop);
				obs_sceneitem_set_visible(added, obs_sceneitem_visible(h.item));
				obs_sceneitem_set_order_position(added, order);
			}
		}
		obs_sceneitem_remove(h.item);
	}
}

OBSSource SpazzSponsorCenter::Sponsors()
{
	std::vector<OBSSource> spots;
	obs_enum_sources(
		[](void *param, obs_source_t *src) {
			if (strcmp(obs_source_get_unversioned_id(src), kSponsorId) == 0 && !obs_source_removed(src)) {
				static_cast<std::vector<OBSSource> *>(param)->emplace_back(src);
			}
			return true;
		},
		&spots);
	if (spots.empty()) {
		return nullptr;
	}

	size_t pick = 0;
	for (size_t i = 0; i < spots.size(); i++) {
		OBSDataAutoRelease priv = obs_source_get_private_settings(spots[i]);
		if (obs_data_get_bool(priv, kMarker)) {
			pick = i;
			break;
		}
	}
	OBSSource main = spots[pick];
	{
		OBSDataAutoRelease priv = obs_source_get_private_settings(main);
		obs_data_set_bool(priv, kMarker, true);
	}
	if (spots.size() == 1) {
		return main;
	}

	/* more than one sponsor spot (from before SPONSORCENTER was one tool, or
	 * a pasted copy): their sponsors join the one list, and the one source
	 * takes their place in the scenes, so sponsors never stack */
	OBSDataAutoRelease mainSettings = obs_source_get_settings(main);
	OBSDataArrayAutoRelease mine = obs_data_get_array(mainSettings, "items");
	OBSDataArrayAutoRelease merged = mine ? CloneArray(mine) : obs_data_array_create();
	std::vector<std::string> have;
	for (size_t i = 0; i < obs_data_array_count(merged); i++) {
		OBSDataAutoRelease it = obs_data_array_item(merged, i);
		have.push_back(ItemKey(it));
	}
	bool grew = false;
	for (size_t i = 0; i < spots.size(); i++) {
		if (i == pick) {
			continue;
		}
		OBSDataAutoRelease theirs = obs_source_get_settings(spots[i]);
		OBSDataArrayAutoRelease items = obs_data_get_array(theirs, "items");
		for (size_t k = 0; k < obs_data_array_count(items); k++) {
			OBSDataAutoRelease it = obs_data_array_item(items, k);
			const std::string key = ItemKey(it);
			if (std::find(have.begin(), have.end(), key) != have.end()) {
				continue;
			}
			have.push_back(key);
			OBSDataAutoRelease copy = obs_data_create_from_json(obs_data_get_json(it));
			obs_data_array_push_back(merged, copy);
			grew = true;
		}
		blog(LOG_INFO, "[SPONSORCENTER] folding sponsor spot '%s' into '%s'", obs_source_get_name(spots[i]),
		     obs_source_get_name(main));
		ReplaceInScenes(spots[i], main);
		obs_source_remove(spots[i]);
	}
	if (grew) {
		OBSDataAutoRelease update = obs_data_create();
		obs_data_set_array(update, "items", merged);
		obs_source_update(main, update);
	}
	return main;
}

static OBSSource CreateSponsors(obs_data_t *settings)
{
	OBSDataAutoRelease s = obs_data_create();
	if (settings) {
		obs_data_apply(s, settings);
		CloneItems(s);
	}
	obs_data_erase(s, "studio_preview");
	obs_data_erase(s, "studio_focus");
	/* full canvas, top left: Position decides where it shows */
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		obs_data_set_int(s, "width", ovi.base_width);
		obs_data_set_int(s, "height", ovi.base_height);
	}
	OBSSourceAutoRelease created =
		obs_source_create(kSponsorId, QT_TO_UTF8(UniqueName(QTStr("Sponsorcenter.DefaultName"))), s, nullptr);
	if (created) {
		OBSDataAutoRelease priv = obs_source_get_private_settings(created);
		obs_data_set_bool(priv, kMarker, true);
	}
	return created.Get();
}

void SpazzSponsorCenter::AddToCurrentScene(QWidget *parent)
{
	if (!obs_source_get_display_name(kSponsorId)) {
		OBSMessageBox::warning(parent, QTStr("Sponsorcenter.Title"), QTStr("Sponsorcenter.Missing"));
		return;
	}
	OBSSourceAutoRelease sceneSource = obs_frontend_get_current_scene();
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	QString msg;
	if (!scene) {
		msg = QTStr("Sponsorcenter.NoScene");
	} else {
		OBSSource sp = Sponsors();
		if (!sp) {
			/* the first time: what's in SPONSORCENTER right now */
			OBSDataAutoRelease from;
			if (instance && instance->preview) {
				from = obs_source_get_settings(instance->preview);
			}
			sp = CreateSponsors(from);
			if (instance && sp) {
				instance->SetDirty(false);
			}
		}
		if (!sp) {
			msg = QTStr("Sponsorcenter.Failed");
		} else if (obs_scene_find_source(scene, obs_source_get_name(sp))) {
			msg = QTStr("Sponsorcenter.AlreadyIn").arg(QT_UTF8(obs_source_get_name(sceneSource)));
		} else {
			obs_scene_add(scene, sp);
			msg = QTStr("Sponsorcenter.Added").arg(QT_UTF8(obs_source_get_name(sceneSource)));
			if (OBSBasic *main = OBSBasic::Get()) {
				main->SaveProject();
			}
		}
	}
	if (instance) {
		instance->status->setText(msg);
		instance->UpdateWhere();
	}
}

/* ------------------------------------------------------------------------ */

QPointer<OBSDock> SpazzSponsorCenter::dock;
std::function<void(QDockWidget *)> SpazzSponsorCenter::showDock;

OBSDock *SpazzSponsorCenter::CreateDock(QWidget *mainWindow)
{
	dock = new OBSDock(mainWindow);
	dock->setObjectName("spazzSponsorDock");
	dock->setWindowTitle(QTStr("Sponsorcenter.Title"));
	instance = new SpazzSponsorCenter(dock);
	dock->setWidget(instance);
	return dock;
}

void SpazzSponsorCenter::Open(QWidget *parent)
{
	if (!obs_source_get_display_name(kSponsorId)) {
		OBSMessageBox::warning(parent, QTStr("Sponsorcenter.Title"), QTStr("Sponsorcenter.Missing"));
		return;
	}
	if (!instance || !dock) {
		return;
	}
	if (showDock) {
		showDock(dock);
	} else {
		dock->show();
		dock->raise();
	}
}

void SpazzSponsorCenter::RegisterOpenProc(QWidget *mainWindow)
{
	static QPointer<QWidget> owner;
	owner = mainWindow;
	auto open = [](void *, calldata_t *) {
		QMetaObject::invokeMethod(
			qApp,
			[]() {
				if (owner) {
					SpazzSponsorCenter::Open(owner);
				}
			},
			Qt::QueuedConnection);
	};
	proc_handler_add(obs_get_proc_handler(), "void spazz_sponsorcenter_open(in string uuid)", open, nullptr);
}

SpazzSponsorCenter::SpazzSponsorCenter(QWidget *parent) : QWidget(parent)
{
	setMinimumSize(420, 220);

	pushTimer = new QTimer(this);
	pushTimer->setSingleShot(true);
	pushTimer->setInterval(250);
	connect(pushTimer, &QTimer::timeout, this, &SpazzSponsorCenter::Push);

	/* another sponsor source turned up (pasted, or an old collection): fold
	 * it into the one, once things are quiet */
	tidyTimer = new QTimer(this);
	tidyTimer->setSingleShot(true);
	tidyTimer->setInterval(300);
	connect(tidyTimer, &QTimer::timeout, this, [this]() {
		if (loading) {
			return;
		}
		Sponsors();
		if (!dirty) {
			LoadFromLive(true);
		}
		UpdateWhere();
	});
	createdSig.Connect(obs_get_signal_handler(), "source_create", SpazzSponsorCenter::SourceCreated, this);
	obs_frontend_add_event_callback(SpazzSponsorCenter::FrontendEvent, this);

	/* ---- left: preview over the live scene ---- */
	QWidget *left = new QWidget(this);
	QVBoxLayout *ll = new QVBoxLayout(left);
	ll->setContentsMargins(0, 0, 0, 0);
	display = new OBSQTDisplay(left);
	display->setMinimumSize(256, 144);
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
	lookTabs = tabs;
	rl->addWidget(tabs, 1);

	where = new QLabel(right);
	where->setWordWrap(true);
	where->setProperty("class", "text-muted");
	rl->addWidget(where);

	QHBoxLayout *buttons = new QHBoxLayout();
	status = new QLabel(right);
	status->setWordWrap(true);
	liveButton = new QPushButton(QTStr("Sponsorcenter.PlayLive"), right);
	liveButton->setToolTip(QTStr("Sponsorcenter.PlayLiveTip"));
	sceneButton = new QPushButton(QTStr("Sponsorcenter.AddToScene"), right);
	sceneButton->setToolTip(QTStr("Sponsorcenter.AddToSceneTip"));
	revertButton = new QPushButton(QTStr("Sponsorcenter.Revert"), right);
	revertButton->setToolTip(QTStr("Sponsorcenter.RevertTip"));
	saveButton = new QPushButton(QTStr("Sponsorcenter.Save"), right);
	saveButton->setToolTip(QTStr("Sponsorcenter.SaveTip"));
	saveButton->setDefault(true);
	buttons->addWidget(status, 1);
	buttons->addWidget(liveButton);
	buttons->addWidget(sceneButton);
	buttons->addWidget(revertButton);
	buttons->addWidget(saveButton);
	rl->addLayout(buttons);

	QSplitter *split = new QSplitter(Qt::Horizontal, this);
	split->addWidget(left);
	split->addWidget(right);
	split->setStretchFactor(0, 3);
	split->setStretchFactor(1, 2);
	split->setChildrenCollapsible(false);
	QVBoxLayout *main = new QVBoxLayout(this);
	main->addWidget(split);

	connect(display, &OBSQTDisplay::DisplayCreated, this, [this]() {
		if (!exiting) {
			obs_display_add_draw_callback(display->GetDisplay(), SpazzSponsorCenter::Draw, this);
		}
	});
	connect(replay, &QPushButton::clicked, this, &SpazzSponsorCenter::Replay);
	connect(saveButton, &QPushButton::clicked, this, &SpazzSponsorCenter::Save);
	connect(revertButton, &QPushButton::clicked, this, [this]() {
		LoadFromLive(true);
		status->setText(QTStr("Sponsorcenter.Reverted"));
	});
	connect(liveButton, &QPushButton::clicked, this, &SpazzSponsorCenter::PlayLive);
	connect(sceneButton, &QPushButton::clicked, this, [this]() { AddToCurrentScene(this); });
	connect(addButton, &QPushButton::clicked, this, &SpazzSponsorCenter::AddSponsor);

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
	SetDirty(false);
}

SpazzSponsorCenter::~SpazzSponsorCenter()
{
	obs_frontend_remove_event_callback(SpazzSponsorCenter::FrontendEvent, this);
	createdSig.Disconnect();
	if (display && display->GetDisplay()) {
		obs_display_remove_draw_callback(display->GetDisplay(), SpazzSponsorCenter::Draw, this);
	}
	if (preview && previewShowing) {
		obs_source_dec_showing(preview);
	}
}

void SpazzSponsorCenter::SourceCreated(void *data, calldata_t *cd)
{
	obs_source_t *src = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!src || strcmp(obs_source_get_unversioned_id(src), kSponsorId) != 0) {
		return;
	}
	SpazzSponsorCenter *self = static_cast<SpazzSponsorCenter *>(data);
	QMetaObject::invokeMethod(self->tidyTimer, qOverload<>(&QTimer::start), Qt::QueuedConnection);
}

void SpazzSponsorCenter::FrontendEvent(enum obs_frontend_event event, void *data)
{
	SpazzSponsorCenter *self = static_cast<SpazzSponsorCenter *>(data);
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		self->loading = true;
		self->tidyTimer->stop();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		/* everything OBS holds is being let go before shutdown: the
		 * preview (a browser) goes now too, not whenever the dock is
		 * deleted, and nothing new gets made after this */
		self->loading = true;
		self->exiting = true;
		self->tidyTimer->stop();
		self->pushTimer->stop();
		if (self->display && self->display->GetDisplay()) {
			obs_display_remove_draw_callback(self->display->GetDisplay(), SpazzSponsorCenter::Draw, self);
		}
		self->SetPreview(nullptr);
		break;
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		self->loading = false;
		/* a different collection: its own sponsors, nothing pending */
		self->SetDirty(false);
		self->tidyTimer->start();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		self->UpdateWhere();
		break;
	default:
		break;
	}
}

/* The preview (a browser) only exists once the tab is first opened, and
 * only renders while the tab is on screen. */
void SpazzSponsorCenter::Start()
{
	LoadFromLive(true);
}

/* swap the preview (null lets it go); the old one is released outside the
 * lock, after the draw callback can no longer pick it up */
void SpazzSponsorCenter::SetPreview(obs_source_t *src)
{
	if (preview && previewShowing) {
		obs_source_dec_showing(preview);
	}
	previewShowing = false;
	OBSSource old = preview.Get();
	{
		std::lock_guard<std::mutex> lock(previewMutex);
		preview = src;
	}
}

void SpazzSponsorCenter::MakePreview(obs_data_t *from)
{
	if (exiting) {
		return;
	}
	if (view) {
		delete view;
		view = nullptr;
	}

	OBSDataAutoRelease init = obs_data_create();
	if (from) {
		obs_data_apply(init, from);
		CloneItems(init);
	}
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		obs_data_set_int(init, "width", ovi.base_width);
		obs_data_set_int(init, "height", ovi.base_height);
	}
	obs_data_set_bool(init, "studio_preview", true);
	obs_data_set_int(init, "studio_focus", -1);
	SetPreview(obs_source_create_private(kSponsorId, "SPONSORCENTER preview", init));
	if (!preview) {
		return;
	}
	if (isVisible()) {
		obs_source_inc_showing(preview);
		previewShowing = true;
	}

	OBSDataAutoRelease settings = obs_source_get_settings(preview);
	view = new OBSPropertiesView(settings.Get(), preview.Get(), (PropertiesReloadCallback)obs_source_properties,
				     (PropertiesUpdateCallback) nullptr, (PropertiesVisualUpdateCb)obs_source_update);
	view->setMinimumWidth(300);
	lookTabs->addTab(view, QTStr("Sponsorcenter.Tab.Look"));
	/* a preset in the Look tab restyles every sponsor's motion too */
	connect(view, &OBSPropertiesView::Changed, this, [this]() {
		status->clear();
		SetDirty(true);
		SyncCardsFromSettings();
	});
}

/* what's on stream now, into the editor. With no Sponsors source yet, the
 * editor keeps what it has unless `fresh` asks for a clean start. */
void SpazzSponsorCenter::LoadFromLive(bool fresh)
{
	if (exiting || (!isVisible() && !preview)) {
		return; /* built the first time the tab opens */
	}
	OBSSource live = loading ? nullptr : Sponsors();
	OBSDataAutoRelease theirs = live ? obs_source_get_settings(live) : nullptr;
	if (!preview || (!theirs && fresh)) {
		MakePreview(theirs);
	} else if (theirs) {
		/* same page, new settings: no reload */
		OBSDataAutoRelease mine = obs_source_get_settings(preview);
		obs_data_clear(mine);
		obs_data_apply(mine, theirs);
		CloneItems(mine);
		obs_data_set_bool(mine, "studio_preview", true);
		obs_data_set_int(mine, "studio_focus", -1);
		if (view) {
			view->ReloadProperties();
		}
	} else if (!fresh) {
		UpdateWhere();
		return;
	}

	focusCard = -1;
	lastTouched = -1;
	LoadCards();
	/* cards save only what they show, so the editor starts from that */
	WriteItems();
	Push();
	SetDirty(false);
	UpdateWhere();
}

void SpazzSponsorCenter::SetDirty(bool on)
{
	dirty = on;
	saveButton->setEnabled(on);
	revertButton->setEnabled(on);
	saveButton->setText(QTStr(on ? "Sponsorcenter.Save" : "Sponsorcenter.Saved"));
}

/* which scenes the Sponsors source is in */
void SpazzSponsorCenter::UpdateWhere()
{
	if (loading) {
		return;
	}
	OBSSource live;
	obs_enum_sources(
		[](void *param, obs_source_t *src) {
			if (strcmp(obs_source_get_unversioned_id(src), kSponsorId) == 0 && !obs_source_removed(src)) {
				*static_cast<OBSSource *>(param) = src;
				return false;
			}
			return true;
		},
		&live);

	OBSSourceAutoRelease current = obs_frontend_get_current_scene();
	if (!live) {
		where->setText(QTStr("Sponsorcenter.WhereNone"));
		sceneButton->setEnabled(current != nullptr);
		liveButton->setEnabled(false);
		return;
	}

	struct Ctx {
		obs_source_t *sp;
		QStringList names;
	} ctx{live, {}};
	obs_enum_scenes(
		[](void *param, obs_source_t *src) {
			Ctx *c = static_cast<Ctx *>(param);
			if (!obs_source_is_group(src) &&
			    obs_scene_find_source_recursive(obs_scene_from_source(src), obs_source_get_name(c->sp))) {
				c->names << QT_UTF8(obs_source_get_name(src));
			}
			return true;
		},
		&ctx);

	const bool here = current &&
			  obs_scene_find_source(obs_scene_from_source(current), obs_source_get_name(live)) != nullptr;
	sceneButton->setEnabled(current && !here);
	liveButton->setEnabled(true);
	where->setText(ctx.names.isEmpty() ? QTStr("Sponsorcenter.WhereNoScenes")
					   : QTStr("Sponsorcenter.Where").arg(ctx.names.join(", ")));
}

void SpazzSponsorCenter::showEvent(QShowEvent *e)
{
	QWidget::showEvent(e);
	if (!preview) {
		Start();
	} else if (!dirty) {
		/* changed from its Properties window while this was closed */
		LoadFromLive(false);
	}
	if (preview && !previewShowing) {
		obs_source_inc_showing(preview);
		previewShowing = true;
	}
	UpdateWhere();
}

void SpazzSponsorCenter::hideEvent(QHideEvent *e)
{
	QWidget::hideEvent(e);
	if (preview && previewShowing) {
		obs_source_dec_showing(preview);
		previewShowing = false;
	}
}

/* ---- the sponsor cards ---- */

/* a card on its way out: off screen and quiet now (deleteLater, as it may be
 * the one whose button is being handled), so it can't edit the list again */
static void DropCard(SponsorCard *card)
{
	card->changed = nullptr;
	card->hide();
	card->deleteLater();
}

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
	if (!preview) {
		return;
	}
	for (SponsorCard *c : cards) {
		DropCard(c);
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
	if (!preview) {
		return;
	}
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
	if (!preview) {
		return;
	}
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
	if (!preview) {
		return;
	}
	pushTimer->stop();
	obs_source_update(preview, nullptr);
}

void SpazzSponsorCenter::CardChanged(SponsorCard *card)
{
	const int idx = IndexOf(card);
	if (idx < 0) {
		return; /* a card already taken out of the list */
	}
	lastTouched = idx;
	if (focusCard != idx) {
		focusCard = idx;
		for (size_t i = 0; i < cards.size(); i++) {
			cards[i]->SetFocused(int(i) == focusCard);
		}
	}
	WriteItems();
	pushTimer->start();
	status->clear();
	SetDirty(true);
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
		DropCard(card);
		focusCard = -1;
		if (lastTouched >= int(cards.size()) || lastTouched == idx) {
			lastTouched = -1;
		}
		break;
	default:
		return;
	}
	SetDirty(true);
	Relayout();
	WriteItems();
	Push();
}

void SpazzSponsorCenter::AddSponsor()
{
	/* the new sponsor starts with the same shape as the one you last worked
	 * on (or the last one): layout, size and cut, but no logo or text */
	OBSDataAutoRelease item;
	const int from = lastTouched >= 0 && lastTouched < int(cards.size()) ? lastTouched : int(cards.size()) - 1;
	if (from >= 0) {
		item = cards[size_t(from)]->Save();
	} else {
		item = obs_data_create();
		obs_data_set_string(item, "layout", "left");
		obs_data_set_double(item, "image_size", 1.0);
	}
	for (const char *k : {"image", "head", "sub", "badge"}) {
		obs_data_set_string(item, k, "");
	}
	for (const char *k : {"zoom", "pan_x", "pan_y"}) {
		obs_data_erase(item, k);
	}
	obs_data_set_bool(item, "off", false);

	SponsorCard *card = MakeCard(item);
	cards.push_back(card);
	focusCard = int(cards.size()) - 1;
	lastTouched = focusCard;
	SetDirty(true);
	Relayout();
	WriteItems();
	Push();

	QTimer::singleShot(0, this, [this, card = QPointer<SponsorCard>(card)]() {
		if (card) {
			cardScroll->ensureWidgetVisible(card, 0, 20);
			card->FocusHeadline();
		}
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

static void StudioDraw(bool on)
{
	uint8_t stack[128];
	calldata_t cd;
	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_bool(&cd, "on", on);
	proc_handler_call(obs_get_proc_handler(), "spazz_sponsor_studio_draw", &cd);
}

void SpazzSponsorCenter::Draw(void *data, uint32_t cx, uint32_t cy)
{
	SpazzSponsorCenter *w = static_cast<SpazzSponsorCenter *>(data);
	/* graphics thread: hold our own reference, the UI thread may swap the
	 * preview at any moment */
	OBSSourceAutoRelease sp;
	{
		std::lock_guard<std::mutex> lock(w->previewMutex);
		sp = obs_source_get_ref(w->preview.Get());
	}
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

	/* what's live right now, so you see the sponsor where it will sit, but
	 * without the live Sponsors source: otherwise its sponsor and the one
	 * you're editing show on top of each other */
	/* the scene itself, the way projectors draw one (drawing the transition
	 * outside the main render leaves its shaders half set up) */
	OBSSourceAutoRelease transition = obs_get_output_source(0);
	OBSSourceAutoRelease program = transition && obs_source_get_type(transition) == OBS_SOURCE_TYPE_TRANSITION
					       ? obs_transition_get_active_source(transition)
					       : obs_source_get_ref(transition);
	if (program) {
		StudioDraw(true);
		obs_source_video_render(program);
		StudioDraw(false);
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
	if (!preview) {
		return;
	}
	OBSDataAutoRelease settings = obs_data_create();
	OBSDataAutoRelease mine = obs_source_get_settings(preview);
	obs_data_apply(settings, mine);
	CloneItems(settings);
	obs_data_erase(settings, "studio_preview");
	obs_data_erase(settings, "studio_focus");
	obs_data_erase(settings, "width");
	obs_data_erase(settings, "height");

	OBSSource live = Sponsors();
	if (live) {
		obs_source_update(live, settings);
		status->setText(QTStr("Sponsorcenter.Updated"));
		SetDirty(false);
	} else {
		/* the first save makes the Sponsors source and puts it in this scene */
		SetDirty(false);
		AddToCurrentScene(this);
		if (!Sponsors()) {
			SetDirty(true);
		}
	}
	UpdateWhere();

	if (OBSBasic *main = OBSBasic::Get()) {
		main->SaveProject();
	}
}

void SpazzSponsorCenter::PlayLive()
{
	OBSSource live = Sponsors();
	if (!live) {
		return;
	}
	calldata_t cd;
	calldata_init(&cd);
	proc_handler_call(obs_source_get_proc_handler(live), "play", &cd);
	calldata_free(&cd);
	status->setText(QTStr("Sponsorcenter.Playing"));
}
