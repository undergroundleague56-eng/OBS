#include "dock.hpp"
#include <obs-frontend-api.h>
#include <QFileDialog>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDateTime>
#include <QDir>
#include <QRegularExpression>
#include <QSignalBlocker>

static bool en(void *p, obs_source_t *s)
{
	auto *b = static_cast<QComboBox *>(p);
	if (!obs_source_is_scene(s) && !obs_source_is_group(s) &&
	    (obs_source_get_output_flags(s) & OBS_SOURCE_VIDEO)) {
		const char *name = obs_source_get_name(s);
		const char *uuid = obs_source_get_uuid(s);
		if (name && uuid)
			b->addItem(QString::fromUtf8(name), QString::fromUtf8(uuid));
	}
	return true;
}

static bool find_selected_item(obs_scene_t *, obs_sceneitem_t *item, void *p)
{
	auto *uuid = static_cast<QString *>(p);
	if (obs_sceneitem_selected(item)) {
		auto *source = obs_sceneitem_get_source(item);
		if (source && (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO)) {
			const char *id = obs_source_get_uuid(source);
			if (id) {
				*uuid = QString::fromUtf8(id);
				return false;
			}
		}
	}
	return true;
}

static void frontend_event(enum obs_frontend_event event, void *p)
{
	if (event != OBS_FRONTEND_EVENT_SCENE_CHANGED &&
	    event != OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED &&
	    event != OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED &&
	    event != OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP)
		return;

	auto *dock = static_cast<SourceReplayDock *>(p);
	QMetaObject::invokeMethod(dock, "refreshSources", Qt::QueuedConnection);
}

SourceReplayDock::SourceReplayDock(QWidget *p) : QDockWidget(p)
{
	setObjectName("SourceReplayDock");
	auto *r = new QWidget(this);
	auto *l = new QVBoxLayout(r);
	sourceBox_ = new QComboBox(r);
	seconds_ = new QSpinBox(r);
	seconds_->setRange(1, 120);
	seconds_->setValue(15);
	path_ = new QLineEdit(QDir::homePath() + "/Videos", r);
	auto *b = new QPushButton("...", r);
	startStop_ = new QPushButton("Start buffer", r);
	auto *s = new QPushButton("Save replay (F8)", r);
	status_ = new QLabel("Stopped", r);
	auto *f = new QFormLayout;
	f->addRow("Video source", sourceBox_);
	f->addRow("Buffer (sec)", seconds_);
	auto *pr = new QHBoxLayout;
	pr->addWidget(path_);
	pr->addWidget(b);
	f->addRow("Folder", pr);
	l->addLayout(f);
	l->addWidget(startStop_);
	l->addWidget(s);
	l->addWidget(status_);
	setWidget(r);
	connect(b, &QPushButton::clicked, this, &SourceReplayDock::choosePath);
	connect(startStop_, &QPushButton::clicked, this, &SourceReplayDock::toggle);
	connect(s, &QPushButton::clicked, this, &SourceReplayDock::saveReplay);
	connect(&timer_, &QTimer::timeout, this, &SourceReplayDock::updateStatus);
	timer_.start(500);
	obs_frontend_add_event_callback(frontend_event, this);
	refreshSources();
}

SourceReplayDock::~SourceReplayDock()
{
	obs_frontend_remove_event_callback(frontend_event, this);
	replay_.stop();
	if (selected_)
		obs_source_release(selected_);
}

void SourceReplayDock::refreshSources()
{
	if (replay_.running())
		return;

	const QString keepUuid = sourceBox_->currentData().toString();
	const QSignalBlocker blocker(sourceBox_);
	sourceBox_->clear();
	obs_enum_sources([](void *p, obs_source_t *s) { return en(p, s); }, sourceBox_);

	int index = keepUuid.isEmpty() ? -1 : sourceBox_->findData(keepUuid);
	if (index < 0 && sourceBox_->count() > 0)
		index = 0;
	if (index >= 0)
		sourceBox_->setCurrentIndex(index);
}

void SourceReplayDock::toggle()
{
	if (replay_.running()) {
		replay_.stop();
		startStop_->setText("Start buffer");
		return;
	}

	const auto uuid = sourceBox_->currentData().toString();
	if (uuid.isEmpty())
		return;

	auto *s = obs_get_source_by_uuid(uuid.toUtf8().constData());
	if (!s)
		return;

	if (selected_)
		obs_source_release(selected_);
	selected_ = s;

	if (replay_.start(selected_, seconds_->value())) {
		startStop_->setText("Stop buffer");
	} else {
		status_->setText(QString("Failed to start: %1").arg(QString::fromStdString(replay_.last_error())));
		obs_source_release(selected_);
		selected_ = nullptr;
	}
}

void SourceReplayDock::saveReplay()
{
	if (!replay_.running())
		return;

	QDir().mkpath(path_->text());
	auto n = sourceBox_->currentText();
	n.replace(QRegularExpression("[^A-Za-zА-Яа-я0-9._-]+"), "_");
	auto file = path_->text() + "/replay_" + n + "_" +
		    QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".mkv";
	status_->setText(replay_.save(file.toStdString()) ? "Saved: " + file : "Save failed");
}

void SourceReplayDock::saveReplayFromHotkey()
{
	saveReplay();
}

void SourceReplayDock::choosePath()
{
	auto p = QFileDialog::getExistingDirectory(this, "Replay folder", path_->text());
	if (!p.isEmpty())
		path_->setText(p);
}

void SourceReplayDock::updateStatus()
{
	if (replay_.running()) {
		status_->setText(QString("Buffer: %1 sec").arg((int)replay_.buffered_seconds()));
		return;
	}

	// Follow the source currently selected in OBS's Sources dock.
	auto *sceneSource = obs_frontend_get_current_scene();
	if (!sceneSource)
		return;

	auto *scene = obs_scene_from_source(sceneSource);
	if (scene) {
		QString selectedUuid;
		obs_scene_enum_items(scene, find_selected_item, &selectedUuid);
		if (!selectedUuid.isEmpty()) {
			const int index = sourceBox_->findData(selectedUuid);
			if (index >= 0 && index != sourceBox_->currentIndex()) {
				const QSignalBlocker blocker(sourceBox_);
				sourceBox_->setCurrentIndex(index);
			}
		}
	}
	obs_source_release(sceneSource);
}
