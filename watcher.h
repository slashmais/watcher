#ifndef _watcher_h_
#define _watcher_h_

#include <string>
#include <functional>


///todo use a class so that each caller(==tab) of watcher has own instance of watches
//use caller's this as key for watches-owners-list
// make_watch(this);
// add_watch(this, dir);
// del_watch(this, dir);
// destroy_watch(this)
//
// same for polling
//

//using UKID=std::string; //user's key or identifier
//typedef std::function<bool(UKID, FSACTION, std::string, std::string)> WATCHCALLBACK;
//
//struct Watcher
//{
//	WATCHCALLBACK fcb;
//
//};
//



namespace watcher
{

//===================================================================================================
typedef std::function<void(std::string)> CBPOLL;

bool StartPoll(std::string se, CBPOLL cb);
bool IsPoll(std::string se=""); //""=>any
void StopPoll(std::string se=""); //""=>all


//===================================================================================================
enum FSACTION { FS_NONE, FS_UPDATED, FS_RENAMED, FS_CREATED, FS_DELETED, FS_REFRESH, FS_REMOVED };

typedef std::function<bool(std::string, FSACTION, std::string, std::string)> CBWATCH;

//caveat: event-logger in a monitored directory => infinite tail-chasing :)
bool StartWatch(std::string kOwn, std::string kW, const std::string sdir, CBWATCH cb);
void StopWatch(std::string kOwn, std::string kW);
void StopWatchDir(std::string kOwn, std::string sdir);
void StopWatches(std::string kOwn);

int IsWatching(); // paused < (0=no) > yes
void StopAllWatches();

void PauseWatcher(bool bPause=true); //applies to all watches
bool IsWatcherPaused();


//===================================================================================================
//DEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUG

const std::string act_name(FSACTION act);
void show_active_watches();
void show_active_polls();

//GUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBED
//===================================================================================================


} //namespace watcher

#endif

