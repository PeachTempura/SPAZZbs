/******************************************************************************
    SPAZZbs - bring scene collections, profiles and browser docks over from
    a regular OBS Studio install. OBS's own files are only ever read.
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "OBSBasic.hpp"

#include <OBSApp.hpp>
#include <models/SceneCollection.hpp>
#include <qt-wrappers.hpp>

#include <json11.hpp>
#include <util/platform.h>

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <filesystem>
#include <set>

namespace fs = std::filesystem;
using json11::Json;

namespace {

struct ObsCollection {
	std::string name;
	fs::path file;
};

struct ObsProfileDir {
	std::string name;
	fs::path dir;
};

struct ObsDock {
	std::string title;
	std::string url;
};

struct ObsSetup {
	fs::path root; /* the "obs-studio" config folder */
	std::vector<ObsCollection> collections;
	std::vector<ObsProfileDir> profiles;
	std::vector<ObsDock> docks;
	std::string currentCollection;
	std::string currentProfile;

	bool Found() const { return !collections.empty() || !profiles.empty() || !docks.empty(); }
};

fs::path DefaultObsRoot()
{
	BPtr<char> path = os_get_config_path_ptr("obs-studio");
	return path ? fs::u8path(path.Get()) : fs::path();
}

/* accept the "obs-studio" folder itself, or a folder that has one inside
 * (e.g. a portable OBS's "config" folder) */
fs::path NormalizeRoot(const fs::path &picked)
{
	std::error_code ec;
	if (fs::is_directory(picked / "basic", ec))
		return picked;
	if (fs::is_directory(picked / "obs-studio" / "basic", ec))
		return picked / "obs-studio";
	if (fs::is_directory(picked / "config" / "obs-studio" / "basic", ec))
		return picked / "config" / "obs-studio";
	return picked;
}

ObsSetup ScanObsSetup(const fs::path &root)
{
	ObsSetup s;
	s.root = root;

	std::error_code ec;
	if (root.empty() || !fs::is_directory(root, ec))
		return s;

	/* OBS 31+ can keep collections and profiles somewhere else */
	fs::path scenesDir = root / "basic" / "scenes";
	fs::path profilesDir = root / "basic" / "profiles";
	fs::path userIni = root / "user.ini";

	ConfigFile global;
	const bool haveGlobal = global.Open((root / "global.ini").u8string().c_str(), CONFIG_OPEN_EXISTING) ==
				CONFIG_SUCCESS;
	if (haveGlobal) {
		auto relocated = [&](const char *key, const char *sub, fs::path &out) {
			const char *v = config_get_string(global, "Locations", key);
			if (!v || !*v)
				return;
			fs::path p = fs::u8path(v) / "obs-studio" / fs::u8path(sub);
			if (fs::exists(p, ec))
				out = p;
		};
		relocated("SceneCollections", "basic/scenes", scenesDir);
		relocated("Profiles", "basic/profiles", profilesDir);
		relocated("Configuration", "user.ini", userIni);
	}

	/* what OBS had open: user.ini on OBS 31+, global.ini before that */
	ConfigFile user;
	config_t *basic = nullptr;
	if (user.Open(userIni.u8string().c_str(), CONFIG_OPEN_EXISTING) == CONFIG_SUCCESS)
		basic = user;
	else if (haveGlobal)
		basic = global;

	if (basic) {
		const char *c = config_get_string(basic, "Basic", "SceneCollection");
		const char *p = config_get_string(basic, "Basic", "Profile");
		s.currentCollection = c ? c : "";
		s.currentProfile = p ? p : "";

		const char *docks = config_get_string(basic, "BasicWindow", "ExtraBrowserDocks");
		std::string err;
		Json json = Json::parse(docks ? docks : "[]", err);
		if (err.empty()) {
			for (const Json &item : json.array_items()) {
				ObsDock d{item["title"].string_value(), item["url"].string_value()};
				if (!d.url.empty())
					s.docks.push_back(std::move(d));
			}
		}
	}

	if (fs::is_directory(scenesDir, ec)) {
		for (const auto &entry : fs::directory_iterator(scenesDir, ec)) {
			if (entry.is_directory() || entry.path().extension().u8string() != ".json")
				continue;

			OBSDataAutoRelease data =
				obs_data_create_from_json_file_safe(entry.path().u8string().c_str(), "bak");
			if (!data)
				continue;

			std::string name = obs_data_get_string(data, "name");
			if (name.empty())
				name = entry.path().stem().u8string();
			s.collections.push_back({name, entry.path()});
		}
	}

	if (fs::is_directory(profilesDir, ec)) {
		for (const auto &entry : fs::directory_iterator(profilesDir, ec)) {
			if (!entry.is_directory())
				continue;

			ConfigFile cfg;
			if (cfg.Open((entry.path() / "basic.ini").u8string().c_str(), CONFIG_OPEN_EXISTING) !=
			    CONFIG_SUCCESS)
				continue;

			const char *name = config_get_string(cfg, "General", "Name");
			s.profiles.push_back({name && *name ? name : entry.path().filename().u8string(), entry.path()});
		}
	}

	auto byName = [](const auto &a, const auto &b) {
		return QString::localeAwareCompare(QString::fromStdString(a.name), QString::fromStdString(b.name)) < 0;
	};
	std::sort(s.collections.begin(), s.collections.end(), byName);
	std::sort(s.profiles.begin(), s.profiles.end(), byName);
	return s;
}

/* source and filter types a collection uses that this build doesn't have */
void CollectMissingTypes(obs_data_t *collection, std::set<std::string> &missing)
{
	auto check = [&](obs_data_t *item) {
		const char *id = obs_data_get_string(item, "id");
		if (id && *id && !obs_source_get_display_name(id))
			missing.insert(id);
	};

	for (const char *list : {"sources", "transitions"}) {
		OBSDataArrayAutoRelease arr = obs_data_get_array(collection, list);
		const size_t n = obs_data_array_count(arr);
		for (size_t i = 0; i < n; i++) {
			OBSDataAutoRelease item = obs_data_array_item(arr, i);
			check(item);

			OBSDataArrayAutoRelease filters = obs_data_get_array(item, "filters");
			const size_t nf = obs_data_array_count(filters);
			for (size_t f = 0; f < nf; f++) {
				OBSDataAutoRelease filter = obs_data_array_item(filters, f);
				check(filter);
			}
		}
	}
}

bool CopyProfileFiles(const fs::path &from, const fs::path &to)
{
	std::error_code ec;
	fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
	if (ec) {
		blog(LOG_WARNING, "[SPAZZbs import] could not copy profile '%s': %s", from.u8string().c_str(),
		     ec.message().c_str());
		return false;
	}
	return true;
}

enum ItemKind { KindCollection = 1, KindProfile, KindDock };

} // namespace

bool OBSBasic::SpazzObsSetupFound()
{
	return ScanObsSetup(DefaultObsRoot()).Found();
}

bool OBSBasic::SpazzImportFromOBS(bool startup)
{
	if (startup) {
		config_set_bool(App()->GetUserConfig(), "SPAZZbs", "ObsImportOffered", true);
		config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
	}

	RefreshSceneCollectionCache();
	RefreshProfileCache();

	ObsSetup setup = ScanObsSetup(DefaultObsRoot());

	QDialog dlg(this);
	dlg.setWindowTitle(QTStr("SpazzImport.Title"));
	dlg.setMinimumSize(520, 520);

	QVBoxLayout *layout = new QVBoxLayout(&dlg);

	QLabel *intro = new QLabel(&dlg);
	intro->setWordWrap(true);
	layout->addWidget(intro);

	QTreeWidget *tree = new QTreeWidget(&dlg);
	tree->setHeaderHidden(true);
	tree->setRootIsDecorated(true);
	tree->setUniformRowHeights(true);
	layout->addWidget(tree, 1);

	QCheckBox *switchTo = new QCheckBox(&dlg);
	layout->addWidget(switchTo);

	QLabel *note = new QLabel(QTStr("SpazzImport.ReadOnly"), &dlg);
	note->setWordWrap(true);
	note->setProperty("class", "text-muted");
	layout->addWidget(note);

	QDialogButtonBox *buttons = new QDialogButtonBox(&dlg);
	QPushButton *browse = buttons->addButton(QTStr("SpazzImport.Browse"), QDialogButtonBox::ResetRole);
	QPushButton *importBtn = buttons->addButton(QTStr("SpazzImport.Import"), QDialogButtonBox::AcceptRole);
	buttons->addButton(startup ? QTStr("SpazzImport.NotNow") : QTStr("Cancel"), QDialogButtonBox::RejectRole);
	layout->addWidget(buttons);

	auto existingCollection = [this](const std::string &n) { return GetSceneCollectionByName(n).has_value(); };
	auto existingProfile = [this](const std::string &n) { return GetProfileByName(n).has_value(); };

	std::set<std::string> currentDockUrls;
#ifdef BROWSER_AVAILABLE
	for (const QString &url : extraBrowserDockTargets)
		currentDockUrls.insert(url.toStdString());
#endif

	auto fill = [&]() {
		tree->clear();

		if (!setup.Found()) {
			intro->setText(QTStr("SpazzImport.NotFound")
					       .arg(QDir::toNativeSeparators(QString::fromStdString(setup.root.u8string()))));
			switchTo->hide();
			importBtn->setEnabled(false);
			return;
		}

		intro->setText(QTStr("SpazzImport.Found")
				       .arg(QDir::toNativeSeparators(QString::fromStdString(setup.root.u8string()))));
		importBtn->setEnabled(true);

		auto addGroup = [&](const QString &title) {
			QTreeWidgetItem *g = new QTreeWidgetItem(tree, {title});
			g->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
			g->setCheckState(0, Qt::Checked);
			g->setExpanded(true);
			QFont f = g->font(0);
			f.setBold(true);
			g->setFont(0, f);
			return g;
		};
		auto addItem = [&](QTreeWidgetItem *g, ItemKind kind, int index, const QString &text, bool checked) {
			QTreeWidgetItem *it = new QTreeWidgetItem(g, {text});
			it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
			it->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
			it->setData(0, Qt::UserRole, (int)kind);
			it->setData(0, Qt::UserRole + 1, index);
		};

		if (!setup.collections.empty()) {
			QTreeWidgetItem *g = addGroup(QTStr("SpazzImport.Collections"));
			for (int i = 0; i < (int)setup.collections.size(); i++) {
				const std::string &n = setup.collections[i].name;
				QString text = QString::fromStdString(n);
				bool checked = true;
				if (existingCollection(n + " (OBS)")) {
					text += "  " + QTStr("SpazzImport.AlreadyImported");
					checked = false;
				} else if (existingCollection(n)) {
					text += "  " + QTStr("SpazzImport.WillRename").arg(QString::fromStdString(n + " (OBS)"));
				}
				addItem(g, KindCollection, i, text, checked);
			}
		}

		if (!setup.profiles.empty()) {
			QTreeWidgetItem *g = addGroup(QTStr("SpazzImport.Profiles"));
			for (int i = 0; i < (int)setup.profiles.size(); i++) {
				const std::string &n = setup.profiles[i].name;
				QString text = QString::fromStdString(n);
				bool checked = true;
				if (existingProfile(n + " (OBS)")) {
					text += "  " + QTStr("SpazzImport.AlreadyImported");
					checked = false;
				} else if (existingProfile(n)) {
					text += "  " + QTStr("SpazzImport.WillRename").arg(QString::fromStdString(n + " (OBS)"));
				}
				addItem(g, KindProfile, i, text, checked);
			}
		}

#ifdef BROWSER_AVAILABLE
		if (!setup.docks.empty()) {
			QTreeWidgetItem *g = addGroup(QTStr("SpazzImport.Docks"));
			for (int i = 0; i < (int)setup.docks.size(); i++) {
				const ObsDock &d = setup.docks[i];
				bool have = currentDockUrls.count(d.url) > 0;
				QString text = QString::fromStdString(d.title.empty() ? d.url : d.title);
				if (have)
					text += "  " + QTStr("SpazzImport.AlreadyHere");
				addItem(g, KindDock, i, text, !have);
			}
		}
#endif

		QString col = QString::fromStdString(setup.currentCollection);
		QString prof = QString::fromStdString(setup.currentProfile);
		if (!col.isEmpty() || !prof.isEmpty()) {
			switchTo->setText(QTStr("SpazzImport.SwitchTo").arg(col.isEmpty() ? "-" : col,
									     prof.isEmpty() ? "-" : prof));
			switchTo->setChecked(true);
			switchTo->show();
		} else {
			switchTo->hide();
			switchTo->setChecked(false);
		}
	};

	fill();

	connect(browse, &QPushButton::clicked, &dlg, [&]() {
		QString start = QString::fromStdString(setup.root.parent_path().u8string());
		QString dir = QFileDialog::getExistingDirectory(&dlg, QTStr("SpazzImport.BrowseTitle"), start);
		if (dir.isEmpty())
			return;
		setup = ScanObsSetup(NormalizeRoot(fs::u8path(dir.toStdString())));
		fill();
	});
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	if (startup && !setup.Found())
		return false;
	if (dlg.exec() != QDialog::Accepted)
		return false;

	/* ---- do it ---- */
	std::vector<int> wantCollections, wantProfiles, wantDocks;
	for (int g = 0; g < tree->topLevelItemCount(); g++) {
		QTreeWidgetItem *group = tree->topLevelItem(g);
		for (int c = 0; c < group->childCount(); c++) {
			QTreeWidgetItem *it = group->child(c);
			if (it->checkState(0) != Qt::Checked)
				continue;
			int kind = it->data(0, Qt::UserRole).toInt();
			int idx = it->data(0, Qt::UserRole + 1).toInt();
			if (kind == KindCollection)
				wantCollections.push_back(idx);
			else if (kind == KindProfile)
				wantProfiles.push_back(idx);
			else if (kind == KindDock)
				wantDocks.push_back(idx);
		}
	}

	auto uniqueName = [](const std::string &base, auto exists) {
		if (!exists(base))
			return base;
		const std::string tagged = base + " (OBS)";
		std::string name = tagged;
		for (int n = 2; exists(name); n++)
			name = tagged + " " + std::to_string(n);
		return name;
	};

	std::string switchCollection, switchProfile;
	std::set<std::string> missingTypes;
	QStringList done, failed;

	for (int idx : wantCollections) {
		const ObsCollection &src = setup.collections[idx];
		OBSDataAutoRelease data = obs_data_create_from_json_file_safe(src.file.u8string().c_str(), "bak");
		if (!data) {
			failed << QString::fromStdString(src.name);
			continue;
		}

		const std::string name = uniqueName(src.name, existingCollection);
		try {
			SceneCollection &col = CreateSceneCollection(name);
			obs_data_set_string(data, "name", name.c_str());
			if (!obs_data_save_json_safe(data, col.getFilePathString().c_str(), "tmp", "bak")) {
				failed << QString::fromStdString(src.name);
				continue;
			}
		} catch (const std::exception &e) {
			blog(LOG_WARNING, "[SPAZZbs import] collection '%s': %s", src.name.c_str(), e.what());
			failed << QString::fromStdString(src.name);
			continue;
		}

		CollectMissingTypes(data, missingTypes);
		blog(LOG_INFO, "[SPAZZbs import] scene collection '%s' -> '%s'", src.name.c_str(), name.c_str());
		done << QTStr("SpazzImport.DoneCollection").arg(QString::fromStdString(name));
		if (src.name == setup.currentCollection)
			switchCollection = name;
	}

	for (int idx : wantProfiles) {
		const ObsProfileDir &src = setup.profiles[idx];
		const std::string name = uniqueName(src.name, existingProfile);
		try {
			const OBSProfile &profile = CreateProfile(name);
			if (!CopyProfileFiles(src.dir, profile.path)) {
				failed << QString::fromStdString(src.name);
				continue;
			}

			ConfigFile cfg;
			if (cfg.Open(profile.profileFile.u8string().c_str(), CONFIG_OPEN_ALWAYS) == CONFIG_SUCCESS) {
				config_set_string(cfg, "General", "Name", name.c_str());
				cfg.SaveSafe("tmp");
			}
		} catch (const std::exception &e) {
			blog(LOG_WARNING, "[SPAZZbs import] profile '%s': %s", src.name.c_str(), e.what());
			failed << QString::fromStdString(src.name);
			continue;
		}

		blog(LOG_INFO, "[SPAZZbs import] profile '%s' -> '%s'", src.name.c_str(), name.c_str());
		done << QTStr("SpazzImport.DoneProfile").arg(QString::fromStdString(name));
		if (src.name == setup.currentProfile)
			switchProfile = name;
	}

#ifdef BROWSER_AVAILABLE
	int docksAdded = 0;
	for (int idx : wantDocks) {
		const ObsDock &d = setup.docks[idx];
		if (currentDockUrls.count(d.url))
			continue;
		QString title = QString::fromStdString(d.title.empty() ? d.url : d.title);
		AddExtraBrowserDock(title, QString::fromStdString(d.url),
				    QUuid::createUuid().toString(QUuid::WithoutBraces), true);
		currentDockUrls.insert(d.url);
		docksAdded++;
	}
	if (docksAdded) {
		SaveExtraBrowserDocks();
		done << QTStr("SpazzImport.DoneDocks").arg(docksAdded);
	}
#endif

	RefreshProfiles(true);
	RefreshSceneCollections(true);
	OnEvent(OBS_FRONTEND_EVENT_PROFILE_LIST_CHANGED);
	OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_LIST_CHANGED);

	/* switch to what OBS was using: profile first (it sets the canvas size
	 * the collection is laid out for), then the scenes */
	QString switchNote;
	if (switchTo->isVisible() && switchTo->isChecked() && (!switchProfile.empty() || !switchCollection.empty())) {
		if (Active()) {
			switchNote = QTStr("SpazzImport.SwitchLater");
		} else {
			if (auto p = GetProfileByName(switchProfile)) {
				OnEvent(OBS_FRONTEND_EVENT_PROFILE_CHANGING);
				ActivateProfile(p.value(), true);
			}
			if (auto c = GetSceneCollectionByName(switchCollection)) {
				OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING);
				ActivateSceneCollection(c.value());
			}
		}
	}

	/* report */
	QString report;
	if (!done.isEmpty())
		report += QTStr("SpazzImport.Imported") + "\n  " + done.join("\n  ");
	if (!failed.isEmpty())
		report += (report.isEmpty() ? "" : "\n\n") + QTStr("SpazzImport.Failed") + "\n  " + failed.join("\n  ");
	if (!missingTypes.empty()) {
		QStringList ids;
		for (const std::string &id : missingTypes)
			ids << QString::fromStdString(id);
		report += "\n\n" + QTStr("SpazzImport.MissingPlugins") + "\n  " + ids.join("\n  ");
	}
	if (!switchNote.isEmpty())
		report += "\n\n" + switchNote;
	if (report.isEmpty())
		report = QTStr("SpazzImport.Nothing");

	OBSMessageBox::information(this, QTStr("SpazzImport.Title"), report);
	return !done.isEmpty();
}
