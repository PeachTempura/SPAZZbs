/******************************************************************************
    SPAZZbs - "Live Controls" floater
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "SpazzLiveControls.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>
#include <properties-view.hpp>
#include <qt-wrappers.hpp>

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QScreen>
#include <QSizeGrip>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include "moc_SpazzLiveControls.cpp"

QHash<QString, QPointer<SpazzLiveControls>> SpazzLiveControls::openWindows;

static const char *kStyle = R"(
#spazzLiveFrame {
	background: #17121F;
	border: 1px solid #0B0712;
	border-radius: 6px;
}
#spazzLiveHeader {
	background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
		stop:0 #3A1C71, stop:0.35 #B8336A, stop:0.7 #F2545B, stop:1 #FFC15E);
	border-top: 1px solid rgba(255, 255, 255, 110);
	border-bottom: 1px solid #0B0712;
	border-top-left-radius: 6px;
	border-top-right-radius: 6px;
}
#spazzLiveTitle {
	color: #FFFFFF;
	font-weight: bold;
	background: transparent;
}
#spazzLiveHeader QToolButton {
	color: #FFFFFF;
	background: rgba(0, 0, 0, 60);
	border: 1px solid rgba(255, 255, 255, 70);
	border-radius: 7px;
	min-width: 20px;
	max-width: 20px;
	min-height: 18px;
	max-height: 18px;
	padding: 0px;
	font-weight: bold;
}
#spazzLiveHeader QToolButton:hover {
	background: rgba(255, 255, 255, 60);
}
#spazzLiveHeader QToolButton:checked {
	background: rgba(255, 255, 255, 110);
	color: #3A1C71;
}
#spazzLiveClose:hover {
	background: #F2545B;
}
#spazzLiveHint {
	color: rgba(255, 255, 255, 120);
	background: transparent;
}
)";

void SpazzLiveControls::Open(obs_source_t *source, QWidget *parent)
{
	if (!source || !obs_source_configurable(source))
		return;

	QString id = QT_UTF8(obs_source_get_uuid(source));
	QPointer<SpazzLiveControls> existing = openWindows.value(id);
	if (existing) {
		existing->show();
		existing->raise();
		existing->activateWindow();
		return;
	}

	SpazzLiveControls *w = new SpazzLiveControls(source, parent);
	openWindows.insert(id, w);
	w->show();
	w->raise();
	w->activateWindow();
}

SpazzLiveControls::SpazzLiveControls(obs_source_t *source_, QWidget *parent)
	: QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
	  source(source_),
	  uuid(QT_UTF8(obs_source_get_uuid(source_))),
	  removedSignal(obs_source_get_signal_handler(source_), "remove", SpazzLiveControls::SourceRemoved, this),
	  renamedSignal(obs_source_get_signal_handler(source_), "rename", SpazzLiveControls::SourceRenamed, this),
	  updatePropertiesSignal(obs_source_get_signal_handler(source_), "update_properties",
				 SpazzLiveControls::UpdateProperties, this),
	  oldSettings(obs_data_create())
{
	setAttribute(Qt::WA_DeleteOnClose);
	setAttribute(Qt::WA_TranslucentBackground);
	setObjectName("spazzLiveWindow");
	setStyleSheet(kStyle);

	/* OBSData's constructor took its own reference */
	obs_data_release(oldSettings);
	OBSDataAutoRelease settings = obs_source_get_settings(source);
	obs_data_apply(oldSettings, settings);

	QWidget *frame = new QWidget(this);
	frame->setObjectName("spazzLiveFrame");
	frame->setAttribute(Qt::WA_StyledBackground);

	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->addWidget(frame);

	QVBoxLayout *layout = new QVBoxLayout(frame);
	layout->setContentsMargins(1, 1, 1, 1);
	layout->setSpacing(0);

	/* header: drag to move, pin, close */
	header = new QWidget(frame);
	header->setObjectName("spazzLiveHeader");
	header->setAttribute(Qt::WA_StyledBackground);
	header->setCursor(Qt::SizeAllCursor);
	header->installEventFilter(this);

	QHBoxLayout *hl = new QHBoxLayout(header);
	hl->setContentsMargins(8, 4, 5, 4);
	hl->setSpacing(4);

	title = new QLabel(header);
	title->setObjectName("spazzLiveTitle");
	title->setAttribute(Qt::WA_TransparentForMouseEvents);
	hl->addWidget(title, 1);

	pinButton = new QToolButton(header);
	pinButton->setText(QStringLiteral("▲"));
	pinButton->setCheckable(true);
	pinButton->setToolTip(QTStr("SpazzLive.Pin"));
	pinButton->setCursor(Qt::ArrowCursor);
	hl->addWidget(pinButton);

	QToolButton *close = new QToolButton(header);
	close->setObjectName("spazzLiveClose");
	close->setText(QStringLiteral("×"));
	close->setToolTip(QTStr("SpazzLive.Close"));
	close->setCursor(Qt::ArrowCursor);
	hl->addWidget(close);

	layout->addWidget(header);

	/* the source's own settings, applied the moment they change */
	view = new OBSPropertiesView(settings.Get(), source, (PropertiesReloadCallback)obs_source_properties,
				     (PropertiesUpdateCallback) nullptr, (PropertiesVisualUpdateCb)obs_source_update);
	view->setMinimumHeight(120);
	layout->addWidget(view, 1);

	/* footer: hint + resize grip */
	QWidget *footer = new QWidget(frame);
	QHBoxLayout *fl = new QHBoxLayout(footer);
	fl->setContentsMargins(8, 2, 2, 2);
	QLabel *hint = new QLabel(QTStr("SpazzLive.Hint"), footer);
	hint->setObjectName("spazzLiveHint");
	fl->addWidget(hint, 1);
	fl->addWidget(new QSizeGrip(footer), 0, Qt::AlignBottom | Qt::AlignRight);
	layout->addWidget(footer);

	connect(close, &QToolButton::clicked, this, &QWidget::close);
	connect(pinButton, &QToolButton::toggled, this, &SpazzLiveControls::SetPinned);

	UpdateTitle();

	bool pinned = true;
	config_t *cfg = App()->GetUserConfig();
	if (config_has_user_value(cfg, "SpazzLiveControls", "Pinned"))
		pinned = config_get_bool(cfg, "SpazzLiveControls", "Pinned");
	pinButton->blockSignals(true);
	pinButton->setChecked(pinned);
	pinButton->blockSignals(false);
	if (!pinned)
		setWindowFlag(Qt::WindowStaysOnTopHint, false);

	const char *geom = config_get_string(cfg, "SpazzLiveControls", "geometry");
	bool placed = false;
	if (geom && *geom)
		placed = restoreGeometry(QByteArray::fromBase64(QByteArray(geom)));
	if (!placed || !screen()) {
		resize(380, 560);
		QPoint at = QCursor::pos() + QPoint(16, -40);
		if (QScreen *s = QGuiApplication::screenAt(QCursor::pos())) {
			QRect r = s->availableGeometry();
			at.setX(std::min(at.x(), r.right() - width()));
			at.setY(std::max(r.top(), std::min(at.y(), r.bottom() - height())));
		}
		move(at);
	}
}

SpazzLiveControls::~SpazzLiveControls()
{
	if (openWindows.value(uuid) == this)
		openWindows.remove(uuid);
}

void SpazzLiveControls::UpdateTitle()
{
	const char *name = source ? obs_source_get_name(source) : "";
	title->setText(QTStr("SpazzLive.Title").arg(QT_UTF8(name)));
	setWindowTitle(title->text());
}

void SpazzLiveControls::SetPinned(bool pinned)
{
	/* changing window flags re-creates the native window */
	QRect g = geometry();
	setWindowFlag(Qt::WindowStaysOnTopHint, pinned);
	setGeometry(g);
	show();
	raise();
	config_set_bool(App()->GetUserConfig(), "SpazzLiveControls", "Pinned", pinned);
}

void SpazzLiveControls::RecordUndo()
{
	OBSBasic *main = OBSBasic::Get();
	if (!main || removed || !source)
		return;

	if (view->DeferUpdate())
		view->UpdateSettings();

	OBSDataAutoRelease newSettings = obs_data_create();
	OBSDataAutoRelease cur = obs_source_get_settings(source);
	obs_data_apply(newSettings, cur);
	obs_data_set_string(newSettings, "undo_uuid", obs_source_get_uuid(source));
	obs_data_set_string(oldSettings, "undo_uuid", obs_source_get_uuid(source));

	std::string undo_data(obs_data_get_json(oldSettings));
	std::string redo_data(obs_data_get_json(newSettings));
	if (undo_data == redo_data)
		return;

	auto undo_redo = [](const std::string &data) {
		OBSDataAutoRelease s = obs_data_create_from_json(data.c_str());
		OBSSourceAutoRelease src = obs_get_source_by_uuid(obs_data_get_string(s, "undo_uuid"));
		if (!src)
			return;
		obs_source_reset_settings(src, s);
		obs_source_update_properties(src);
	};

	main->undo_s.add_action(QTStr("Undo.Properties").arg(obs_source_get_name(source)), undo_redo, undo_redo,
				undo_data, redo_data);
}

void SpazzLiveControls::closeEvent(QCloseEvent *e)
{
	RecordUndo();
	config_set_string(App()->GetUserConfig(), "SpazzLiveControls", "geometry",
			  saveGeometry().toBase64().constData());
	QWidget::closeEvent(e);
}

void SpazzLiveControls::keyPressEvent(QKeyEvent *e)
{
	if (e->key() == Qt::Key_Escape) {
		close();
		return;
	}
	QWidget::keyPressEvent(e);
}

bool SpazzLiveControls::eventFilter(QObject *obj, QEvent *e)
{
	if (obj != header)
		return QWidget::eventFilter(obj, e);

	switch (e->type()) {
	case QEvent::MouseButtonPress: {
		QMouseEvent *me = static_cast<QMouseEvent *>(e);
		if (me->button() != Qt::LeftButton)
			break;
		/* let the OS move it (snapping, multi-monitor) when it can */
		if (windowHandle() && windowHandle()->startSystemMove())
			return true;
		dragging = true;
		dragOffset = me->globalPosition().toPoint() - frameGeometry().topLeft();
		return true;
	}
	case QEvent::MouseMove:
		if (dragging) {
			QMouseEvent *me = static_cast<QMouseEvent *>(e);
			move(me->globalPosition().toPoint() - dragOffset);
			return true;
		}
		break;
	case QEvent::MouseButtonRelease:
		dragging = false;
		break;
	case QEvent::MouseButtonDblClick:
		/* double-click the bar to snap back to a sensible size */
		resize(380, 560);
		return true;
	default:
		break;
	}
	return QWidget::eventFilter(obj, e);
}

void SpazzLiveControls::SourceRemoved(void *data, calldata_t *)
{
	SpazzLiveControls *w = static_cast<SpazzLiveControls *>(data);
	QMetaObject::invokeMethod(
		w,
		[w]() {
			w->removed = true;
			w->close();
		},
		Qt::QueuedConnection);
}

void SpazzLiveControls::SourceRenamed(void *data, calldata_t *)
{
	SpazzLiveControls *w = static_cast<SpazzLiveControls *>(data);
	QMetaObject::invokeMethod(w, [w]() { w->UpdateTitle(); }, Qt::QueuedConnection);
}

void SpazzLiveControls::UpdateProperties(void *data, calldata_t *)
{
	SpazzLiveControls *w = static_cast<SpazzLiveControls *>(data);
	QMetaObject::invokeMethod(w, [w]() { w->view->ReloadProperties(); }, Qt::QueuedConnection);
}
