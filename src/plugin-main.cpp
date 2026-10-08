#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs-hotkey.h>
#include "dock.hpp"
OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-source-replay","en-US")
static SourceReplayDock*dock=nullptr; static obs_hotkey_id hotkey=OBS_INVALID_HOTKEY_ID;
static void hot(void*,obs_hotkey_id,obs_hotkey_t*,bool p){if(p&&dock)dock->saveReplayFromHotkey();}
bool obs_module_load(){dock=new SourceReplayDock();obs_frontend_add_dock_by_id("obs-source-replay","Source Replay",dock);hotkey=obs_hotkey_register_frontend("source_replay_save","Save Source Replay",hot,nullptr);return true;}
void obs_module_unload(){if(hotkey!=OBS_INVALID_HOTKEY_ID)obs_hotkey_unregister(hotkey);delete dock;dock=nullptr;}
MODULE_EXPORT const char*obs_module_description(){return "Independent source replay buffer with audio and F8 hotkey";}
