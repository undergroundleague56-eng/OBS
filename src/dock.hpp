#pragma once
#include <QDockWidget>
#include <QComboBox>
#include <QSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QTimer>
#include <obs.h>
#include "replay.hpp"
class SourceReplayDock:public QDockWidget{Q_OBJECT public:explicit SourceReplayDock(QWidget*=nullptr);~SourceReplayDock()override;void saveReplayFromHotkey();private slots:void refreshSources();void toggle();void saveReplay();void choosePath();void updateStatus();private:QComboBox*sourceBox_;QSpinBox*seconds_;QLineEdit*path_;QPushButton*startStop_;QLabel*status_;QTimer timer_;SourceReplay replay_;obs_source_t*selected_=nullptr;};