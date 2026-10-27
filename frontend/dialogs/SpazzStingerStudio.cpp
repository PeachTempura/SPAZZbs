/******************************************************************************
    SPAZZbs - Stinger Studio
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "SpazzStingerStudio.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSQTDisplay.hpp>
#include <utility/display-helpers.hpp>
#include <widgets/OBSBasic.hpp>
#include <properties-view.hpp>
#include <qt-wrappers.hpp>

#include <obs-frontend-api.h>

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

#include "moc_SpazzStingerStudio.cpp"

static constexpr const char *kStingerId = "spazz_stinger";

QPointer<SpazzStingerStudio> SpazzStingerStudio::instance;

void SpazzStingerStudio::Open(QWidget *parent)
{
	if (instance) {
		instance->show();
		instance->raise();
		instance->activateWindow();
		return;
	}

	if (!obs_source_get_display_name(kStingerId)) {
		OBSMessageBox::warning(parent, QTStr("SpazzStudio.Title"), QTStr("SpazzStudio.Missing"));
		return;
	}

	instance = new SpazzStingerStudio(parent);
	instance->show();
}

SpazzStingerStudio::SpazzStingerStudio(QWidget *parent) : QDialog(parent, Qt::Window)
{
	setAttribute(Qt::WA_DeleteOnClose);
	setWindowTitle(QTStr("SpazzStudio.Title"));
	setMinimumSize(900, 560);

	/* the transition being designed: private, never in the scene list */
	preview = obs_source_create_private(kStingerId, "Stinger Studio preview", nullptr);
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi))
		obs_transition_set_size(preview, ovi.base_width, ovi.base_height);
	obs_source_inc_showing(preview);

	/* ---- left: preview ---- */
	QWidget *left = new QWidget(this);
	QVBoxLayout *ll = new QVBoxLayout(left);
	ll->setContentsMargins(0, 0, 0, 0);

	display = new OBSQTDisplay(left);
	display->setMinimumSize(480, 270);
	display->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	ll->addWidget(display, 1);

	QHBoxLayout *scenesRow = new QHBoxLayout();
	fromScene = new QComboBox(left);
	toScene = new QComboBox(left);
	playButton = new QPushButton(QTStr("SpazzStudio.Pause"), left);
	QPushButton *replay = new QPushButton(QTStr("SpazzStudio.Replay"), left);
	scenesRow->addWidget(new QLabel(QTStr("SpazzStudio.From"), left));
	scenesRow->addWidget(fromScene, 1);
	scenesRow->addWidget(new QLabel(QStringLiteral("→"), left));
	scenesRow->addWidget(toScene, 1);
	scenesRow->addWidget(replay);
	scenesRow->addWidget(playButton);
	ll->addLayout(scenesRow);

	QLabel *hint = new QLabel(QTStr("SpazzStudio.Hint"), left);
	hint->setWordWrap(true);
	hint->setProperty("class", "text-muted");
	ll->addWidget(hint);

	/* ---- right: what to edit, the settings, saving ---- */
	QWidget *right = new QWidget(this);
	QVBoxLayout *rl = new QVBoxLayout(right);
	rl->setContentsMargins(0, 0, 0, 0);

	QFormLayout *top = new QFormLayout();
	editing = new QComboBox(right);
	top->addRow(QTStr("SpazzStudio.Editing"), editing);
	rl->addLayout(top);

	OBSDataAutoRelease settings = obs_source_get_settings(preview);
	view = new OBSPropertiesView(settings.Get(), preview.Get(), (PropertiesReloadCallback)obs_source_properties,
				     (PropertiesUpdateCallback) nullptr, (PropertiesVisualUpdateCb)obs_source_update);
	view->setMinimumWidth(360);
	rl->addWidget(view, 1);

	QFormLayout *bottom = new QFormLayout();
	nameEdit = new QLineEdit(right);
	bottom->addRow(QTStr("SpazzStudio.Name"), nameEdit);
	rl->addLayout(bottom);

	makeCurrent = new QCheckBox(QTStr("SpazzStudio.MakeCurrent"), right);
	makeCurrent->setChecked(true);
	rl->addWidget(makeCurrent);

	QHBoxLayout *buttons = new QHBoxLayout();
	status = new QLabel(right);
	status->setWordWrap(true);
	saveButton = new QPushButton(right);
	saveButton->setDefault(true);
	QPushButton *close = new QPushButton(QTStr("Close"), right);
	buttons->addWidget(status, 1);
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

	const char *geom = config_get_string(App()->GetUserConfig(), "SpazzStingerStudio", "geometry");
	if (!(geom && *geom && restoreGeometry(QByteArray::fromBase64(QByteArray(geom)))))
		resize(1180, 680);

	RefreshScenes();
	RefreshEditing();

	connect(display, &OBSQTDisplay::DisplayCreated, this,
		[this]() { obs_display_add_draw_callback(display->GetDisplay(), SpazzStingerStudio::Draw, this); });
	connect(editing, &QComboBox::currentIndexChanged, this, &SpazzStingerStudio::EditingChanged);
	connect(fromScene, &QComboBox::currentIndexChanged, this, &SpazzStingerStudio::ResetPreview);
	connect(toScene, &QComboBox::currentIndexChanged, this, &SpazzStingerStudio::ResetPreview);
	connect(replay, &QPushButton::clicked, this, [this]() {
		paused = false;
		playButton->setText(QTStr("SpazzStudio.Pause"));
		ResetPreview();
	});
	connect(playButton, &QPushButton::clicked, this, [this]() {
		paused = !paused;
		playButton->setText(paused ? QTStr("SpazzStudio.Play") : QTStr("SpazzStudio.Pause"));
	});
	connect(saveButton, &QPushButton::clicked, this, &SpazzStingerStudio::Save);
	connect(close, &QPushButton::clicked, this, &QDialog::close);
	/* changing the look restarts the loop so you see it from the top */
	connect(view, &OBSPropertiesView::Changed, this, [this]() {
		status->clear();
		if (!paused)
			nextStartMs = QDateTime::currentMSecsSinceEpoch() + 250;
	});

	connect(&loopTimer, &QTimer::timeout, this, &SpazzStingerStudio::Loop);
	loopTimer.start(50);
	ResetPreview();
}

SpazzStingerStudio::~SpazzStingerStudio()
{
	loopTimer.stop();
	if (display && display->GetDisplay())
		obs_display_remove_draw_callback(display->GetDisplay(), SpazzStingerStudio::Draw, this);
	if (preview) {
		obs_transition_clear(preview);
		obs_source_dec_showing(preview);
	}
}

void SpazzStingerStudio::closeEvent(QCloseEvent *e)
{
	config_set_string(App()->GetUserConfig(), "SpazzStingerStudio", "geometry",
			  saveGeometry().toBase64().constData());
	QDialog::closeEvent(e);
}

/* ---- scenes to preview between ---- */

void SpazzStingerStudio::RefreshScenes()
{
	QSignalBlocker b1(fromScene), b2(toScene);
	fromScene->clear();
	toScene->clear();

	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		const char *name = obs_source_get_name(scenes.sources.array[i]);
		fromScene->addItem(QT_UTF8(name));
		toScene->addItem(QT_UTF8(name));
	}
	obs_frontend_source_list_free(&scenes);

	/* from what's live now, to the next scene in the list */
	OBSSourceAutoRelease current = obs_frontend_get_current_scene();
	int from = current ? fromScene->findText(QT_UTF8(obs_source_get_name(current))) : 0;
	from = from < 0 ? 0 : from;
	fromScene->setCurrentIndex(from);
	toScene->setCurrentIndex(toScene->count() > 1 ? (from + 1) % toScene->count() : from);
}

OBSSource SpazzStingerStudio::SceneFor(QComboBox *combo) const
{
	OBSSourceAutoRelease src = obs_get_source_by_name(QT_TO_UTF8(combo->currentText()));
	return OBSSource(src.Get());
}

/* ---- which transition we're editing ---- */

void SpazzStingerStudio::RefreshEditing(const QString &selectUuid)
{
	QSignalBlocker block(editing);
	editing->clear();
	editing->addItem(QTStr("SpazzStudio.New"), QString());

	struct obs_frontend_source_list list = {};
	obs_frontend_get_transitions(&list);
	for (size_t i = 0; i < list.sources.num; i++) {
		obs_source_t *tr = list.sources.array[i];
		if (strcmp(obs_source_get_unversioned_id(tr), kStingerId) != 0)
			continue;
		editing->addItem(QT_UTF8(obs_source_get_name(tr)), QT_UTF8(obs_source_get_uuid(tr)));
	}
	obs_frontend_source_list_free(&list);

	int idx = selectUuid.isEmpty() ? 0 : editing->findData(selectUuid);
	editing->setCurrentIndex(idx < 0 ? 0 : idx);
	EditingChanged();
}

void SpazzStingerStudio::EditingChanged()
{
	const QString uuid = editing->currentData().toString();
	OBSSourceAutoRelease existing = uuid.isEmpty() ? nullptr : obs_get_source_by_uuid(QT_TO_UTF8(uuid));

	OBSDataAutoRelease mine = obs_source_get_settings(preview);
	if (existing) {
		/* start from that transition's own settings */
		OBSDataAutoRelease theirs = obs_source_get_settings(existing);
		obs_data_clear(mine);
		obs_data_apply(mine, theirs);
		obs_source_update(preview, nullptr);
		nameEdit->setText(QT_UTF8(obs_source_get_name(existing)));
		nameEdit->setEnabled(false);
		saveButton->setText(QTStr("SpazzStudio.Update"));
	} else {
		nameEdit->setText(UniqueName(QTStr("SpazzStudio.DefaultName")));
		nameEdit->setEnabled(true);
		saveButton->setText(QTStr("SpazzStudio.SaveNew"));
	}

	view->ReloadProperties();
	status->clear();
	ResetPreview();
}

QString SpazzStingerStudio::UniqueName(const QString &base) const
{
	auto exists = [](const QString &n) {
		bool found = false;
		struct obs_frontend_source_list list = {};
		obs_frontend_get_transitions(&list);
		for (size_t i = 0; i < list.sources.num && !found; i++)
			found = n == QT_UTF8(obs_source_get_name(list.sources.array[i]));
		obs_frontend_source_list_free(&list);
		return found;
	};

	QString name = base;
	for (int i = 2; exists(name); i++)
		name = QString("%1 %2").arg(base).arg(i);
	return name;
}

/* ---- the looping preview ---- */

void SpazzStingerStudio::ResetPreview()
{
	OBSSource from = SceneFor(fromScene);
	if (from)
		obs_transition_set(preview, from);
	forward = true;
	wasActive = false;
	nextStartMs = QDateTime::currentMSecsSinceEpoch() + 300;
}

void SpazzStingerStudio::Loop()
{
	if (!preview)
		return;

	const bool active = obs_transition_is_active(preview);
	const qint64 now = QDateTime::currentMSecsSinceEpoch();

	if (wasActive && !active) {
		/* hold on the new scene for a moment before going back */
		nextStartMs = now + 900;
		forward = !forward;
	}
	wasActive = active;

	if (paused || active || now < nextStartMs)
		return;

	OBSSource from = SceneFor(forward ? fromScene : toScene);
	OBSSource to = SceneFor(forward ? toScene : fromScene);
	if (!from || !to)
		return;

	obs_transition_set(preview, from);
	obs_transition_start(preview, OBS_TRANSITION_MODE_AUTO, 1000, to);
	wasActive = true;
}

void SpazzStingerStudio::Draw(void *data, uint32_t cx, uint32_t cy)
{
	SpazzStingerStudio *w = static_cast<SpazzStingerStudio *>(data);
	obs_source_t *src = w->preview.Get();
	if (!src)
		return;

	uint32_t sourceCX = std::max(obs_source_get_width(src), 1u);
	uint32_t sourceCY = std::max(obs_source_get_height(src), 1u);

	int x, y;
	float scale;
	GetScaleAndCenterPos(sourceCX, sourceCY, cx, cy, x, y, scale);
	int newCX = int(scale * float(sourceCX));
	int newCY = int(scale * float(sourceCY));

	gs_viewport_push();
	gs_projection_push();
	gs_ortho(0.0f, float(sourceCX), 0.0f, float(sourceCY), -100.0f, 100.0f);
	gs_set_viewport(x, y, newCX, newCY);
	obs_source_video_render(src);
	gs_projection_pop();
	gs_viewport_pop();
}

/* ---- saving into the real transition list ---- */

void SpazzStingerStudio::Save()
{
	OBSBasic *main = OBSBasic::Get();
	if (!main)
		return;

	OBSDataAutoRelease settings = obs_source_get_settings(preview);
	const QString uuid = editing->currentData().toString();
	OBSSource target;

	if (!uuid.isEmpty()) {
		OBSSourceAutoRelease existing = obs_get_source_by_uuid(QT_TO_UTF8(uuid));
		if (!existing) {
			status->setText(QTStr("SpazzStudio.Gone"));
			RefreshEditing();
			return;
		}
		obs_source_update(existing, settings);
		target = existing.Get();
		status->setText(QTStr("SpazzStudio.Updated").arg(QT_UTF8(obs_source_get_name(existing))));
	} else {
		QString name = nameEdit->text().trimmed();
		if (name.isEmpty())
			name = QTStr("SpazzStudio.DefaultName");
		name = UniqueName(name);

		target = main->SpazzAddTransition(kStingerId, QT_TO_UTF8(name), settings);
		if (!target) {
			status->setText(QTStr("SpazzStudio.Failed"));
			return;
		}
		status->setText(QTStr("SpazzStudio.Saved").arg(name));
		RefreshEditing(QT_UTF8(obs_source_get_uuid(target)));
	}

	if (makeCurrent->isChecked())
		obs_frontend_set_current_transition(target);

	main->SaveProject();
}
