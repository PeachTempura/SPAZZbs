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

#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

#include "moc_SpazzSponsorCenter.cpp"

static constexpr const char *kSponsorId = "spazz_sponsor";

QPointer<SpazzSponsorCenter> SpazzSponsorCenter::instance;

void SpazzSponsorCenter::Open(QWidget *parent)
{
	if (instance) {
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
	instance->show();
}

SpazzSponsorCenter::SpazzSponsorCenter(QWidget *parent) : QDialog(parent, Qt::Window)
{
	setAttribute(Qt::WA_DeleteOnClose);
	setWindowTitle(QTStr("Sponsorcenter.Title"));
	setMinimumSize(900, 560);

	/* the sponsor being designed: private, sized to the canvas, and it
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
	QLabel *hint = new QLabel(QTStr("Sponsorcenter.Hint"), left);
	hint->setWordWrap(true);
	hint->setProperty("class", "text-muted");
	row->addWidget(replay);
	row->addWidget(hint, 1);
	ll->addLayout(row);

	/* ---- right: what to edit, every option, saving ---- */
	QWidget *right = new QWidget(this);
	QVBoxLayout *rl = new QVBoxLayout(right);
	rl->setContentsMargins(0, 0, 0, 0);

	QFormLayout *top = new QFormLayout();
	editing = new QComboBox(right);
	top->addRow(QTStr("Sponsorcenter.Editing"), editing);
	rl->addLayout(top);

	OBSDataAutoRelease settings = obs_source_get_settings(preview);
	view = new OBSPropertiesView(settings.Get(), preview.Get(), (PropertiesReloadCallback)obs_source_properties,
				     (PropertiesUpdateCallback) nullptr, (PropertiesVisualUpdateCb)obs_source_update);
	view->setMinimumWidth(380);
	rl->addWidget(view, 1);

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
	if (!(geom && *geom && restoreGeometry(QByteArray::fromBase64(QByteArray(geom)))))
		resize(1240, 720);

	RefreshEditing();

	connect(display, &OBSQTDisplay::DisplayCreated, this,
		[this]() { obs_display_add_draw_callback(display->GetDisplay(), SpazzSponsorCenter::Draw, this); });
	connect(editing, &QComboBox::currentIndexChanged, this, &SpazzSponsorCenter::EditingChanged);
	connect(replay, &QPushButton::clicked, this, &SpazzSponsorCenter::Replay);
	connect(saveButton, &QPushButton::clicked, this, &SpazzSponsorCenter::Save);
	connect(liveButton, &QPushButton::clicked, this, &SpazzSponsorCenter::PlayLive);
	connect(close, &QPushButton::clicked, this, &QDialog::close);
	connect(view, &OBSPropertiesView::Changed, this, [this]() { status->clear(); });
}

SpazzSponsorCenter::~SpazzSponsorCenter()
{
	if (display && display->GetDisplay())
		obs_display_remove_draw_callback(display->GetDisplay(), SpazzSponsorCenter::Draw, this);
	if (preview)
		obs_source_dec_showing(preview);
}

void SpazzSponsorCenter::closeEvent(QCloseEvent *e)
{
	config_set_string(App()->GetUserConfig(), "SpazzSponsorCenter", "geometry",
			  saveGeometry().toBase64().constData());
	QDialog::closeEvent(e);
}

/* ---- which sponsor we're editing ---- */

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
		obs_data_set_bool(mine, "studio_preview", true);
		obs_source_update(preview, nullptr);
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

	view->ReloadProperties();
	status->clear();
}

QString SpazzSponsorCenter::UniqueName(const QString &base) const
{
	QString name = base;
	for (int i = 2;; i++) {
		OBSSourceAutoRelease taken = obs_get_source_by_name(QT_TO_UTF8(name));
		if (!taken)
			return name;
		name = QString("%1 %2").arg(base).arg(i);
	}
}

/* ---- preview ---- */

void SpazzSponsorCenter::Replay()
{
	/* the preview plays again whenever it gets its settings */
	obs_source_update(preview, nullptr);
}

void SpazzSponsorCenter::Draw(void *data, uint32_t cx, uint32_t cy)
{
	SpazzSponsorCenter *w = static_cast<SpazzSponsorCenter *>(data);
	obs_source_t *sp = w->preview.Get();
	if (!sp)
		return;

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return;
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
	if (program)
		obs_source_video_render(program);

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
	obs_data_erase(settings, "studio_preview");

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
		if (name.isEmpty())
			name = QTStr("Sponsorcenter.DefaultName");
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

	if (OBSBasic *main = OBSBasic::Get())
		main->SaveProject();
}

void SpazzSponsorCenter::PlayLive()
{
	const QString uuid = editing->currentData().toString();
	OBSSourceAutoRelease existing = uuid.isEmpty() ? nullptr : obs_get_source_by_uuid(QT_TO_UTF8(uuid));
	if (!existing)
		return;
	calldata_t cd;
	calldata_init(&cd);
	proc_handler_call(obs_source_get_proc_handler(existing), "play", &cd);
	calldata_free(&cd);
	status->setText(QTStr("Sponsorcenter.Playing").arg(QT_UTF8(obs_source_get_name(existing))));
}
