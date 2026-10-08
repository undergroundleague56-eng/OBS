#include "dock.hpp"
#include <obs-frontend-api.h>
#include <QFileDialog>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDateTime>
#include <QDir>
#include <QRegularExpression>
static bool en(void*p,obs_source_t*s){auto*b=(QComboBox*)p;if(!obs_source_is_scene(s)&&!obs_source_is_group(s))b->addItem(obs_source_get_name(s),QString::fromUtf8(obs_source_get_name(s)));return true;}
SourceReplayDock::SourceReplayDock(QWidget*p):QDockWidget(p){setObjectName("SourceReplayDock");auto*r=new QWidget(this);auto*l=new QVBoxLayout(r);sourceBox_=new QComboBox(r);seconds_=new QSpinBox(r);seconds_->setRange(1,120);seconds_->setValue(15);path_=new QLineEdit(QDir::homePath()+"/Videos",r);auto*b=new QPushButton("...",r);startStop_=new QPushButton("Start buffer",r);auto*s=new QPushButton("Save replay (F8)",r);status_=new QLabel("Stopped",r);auto*f=new QFormLayout;f->addRow("Video source",sourceBox_);f->addRow("Buffer (sec)",seconds_);auto*pr=new QHBoxLayout;pr->addWidget(path_);pr->addWidget(b);f->addRow("Folder",pr);l->addLayout(f);l->addWidget(startStop_);l->addWidget(s);l->addWidget(status_);setWidget(r);connect(b,&QPushButton::clicked,this,&SourceReplayDock::choosePath);connect(startStop_,&QPushButton::clicked,this,&SourceReplayDock::toggle);connect(s,&QPushButton::clicked,this,&SourceReplayDock::saveReplay);connect(&timer_,&QTimer::timeout,this,&SourceReplayDock::updateStatus);timer_.start(500);refreshSources();}
SourceReplayDock::~SourceReplayDock(){replay_.stop();if(selected_)obs_source_release(selected_);}
void SourceReplayDock::refreshSources(){sourceBox_->clear();obs_enum_sources([](void*p,obs_source_t*s){return en(p,s);},sourceBox_);}
void SourceReplayDock::toggle(){if(replay_.running()){replay_.stop();startStop_->setText("Start buffer");return;}const auto name=sourceBox_->currentData().toString();if(name.isEmpty())return;auto*s=obs_get_source_by_name(name.toUtf8().constData());if(!s)return;if(selected_)obs_source_release(selected_);selected_=s;if(replay_.start(selected_,seconds_->value()))startStop_->setText("Stop buffer");else{status_->setText("Failed to start");obs_source_release(selected_);selected_=nullptr;} }
void SourceReplayDock::saveReplay(){if(!replay_.running())return;QDir().mkpath(path_->text());auto n=sourceBox_->currentText();n.replace(QRegularExpression("[^A-Za-zА-Яа-я0-9._-]+"),"_");auto file=path_->text()+"/replay_"+n+"_"+QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")+".mkv";status_->setText(replay_.save(file.toStdString())?"Saved: "+file:"Save failed");}
void SourceReplayDock::saveReplayFromHotkey(){saveReplay();}
void SourceReplayDock::choosePath(){auto p=QFileDialog::getExistingDirectory(this,"Replay folder",path_->text());if(!p.isEmpty())path_->setText(p);}
void SourceReplayDock::updateStatus(){if(replay_.running())status_->setText(QString("Buffer: %1 sec").arg((int)replay_.buffered_seconds()));else refreshSources();}
