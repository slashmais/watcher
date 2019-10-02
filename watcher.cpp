
#include "watcher.h"
#include <utilfuncs/utilfuncs.h>
#include <vector>
#include <queue>
#include <deque>
#include <exception>
#include <cerrno>
#include <thread>
#include <chrono>
#include <mutex>
#include <unistd.h>
#include <utility>
#include <sys/inotify.h>


namespace watcher
{

//==================================================================================================
//DEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUG
const std::string evt_name(int evt)
{
	std::string sfp("");
	auto bs=[=](int n)->bool{ return ((evt&n)==n); };

	if (bs(IN_ISDIR)) sfp+=" IN_ISDIR";
	if (bs(IN_ACCESS)) sfp+=" IN_ACCESS";
	if (bs(IN_ATTRIB)) sfp+=" IN_ATTRIB";
	if (bs(IN_MODIFY)) sfp+=" IN_MODIFY";
	if (bs(IN_OPEN)) sfp+=" IN_OPEN";
	if (bs(IN_CLOSE_NOWRITE)) sfp+=" IN_CLOSE_NOWRITE";
	if (bs(IN_CLOSE_WRITE)) sfp+=" IN_CLOSE_WRITE";
	if (bs(IN_CREATE)) sfp+=" IN_CREATE";
	if (bs(IN_DELETE)) sfp+=" IN_DELETE";
	if (bs(IN_MOVED_FROM)) sfp+=" IN_MOVED_FROM";
	if (bs(IN_MOVED_TO)) sfp+=" IN_MOVED_TO";
	if (bs(IN_Q_OVERFLOW)) sfp+=" IN_Q_OVERFLOW";
	if (bs(IN_MOVE_SELF)) sfp+=" IN_MOVE_SELF";
	if (bs(IN_DELETE_SELF)) sfp+=" IN_DELETE_SELF";
	if (bs(IN_IGNORED)) sfp+=" IN_IGNORED";
	if (bs(IN_UNMOUNT)) sfp+=" IN_UNMOUNT";
	return sfp;
}

const std::string act_name(FSACTION act)
{
	std::string s{"error:unknown action"};
	switch(act)
	{
		case FS_UPDATED: s="FS_UPDATED"; break;
		case FS_RENAMED: s="FS_RENAMED"; break;
		case FS_CREATED: s="FS_CREATED"; break;
		case FS_DELETED: s="FS_DELETED"; break;
		case FS_REFRESH: s="FS_REFRESH"; break;
		//case FS_REMOVED: s="FS_REMOVED"; break;
		case FS_NONE:	 s="FS_NONE"; break;
		default: {}
	}
	return s;
}

//GUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBED
//===================================================================================================


//==================================================================================================
int fdINOTIFY=(-1);
typedef int WDINOTIFY; //inotify-wd

//uint32_t MONITOR_MASK=(IN_DONT_FOLLOW|IN_ALL_EVENTS);
uint32_t MONITOR_MASK=(IN_DONT_FOLLOW|IN_MODIFY|IN_ATTRIB|IN_MOVED_FROM|IN_MOVED_TO|IN_CREATE|IN_DELETE|IN_DELETE_SELF|IN_MOVE_SELF);

bool bWatcherPaused{false};

std::thread T_Event;
volatile bool b_stop_event_handler;

std::thread T_Filter;
volatile bool b_stop_filter_handler;

std::thread T_Apply;
volatile bool b_stop_apply_handler;

std::mutex MUX_WATCH;

enum //primes for time-outs (microseconds)
{
	TO_DEL_WATCH=17,
	TO_EVENT=53, //loop-period: handling inotify events
	TO_FILTER=109, //loop-period: converting events to actions
	TO_APPLY=197, //loop-period: do callbacks for actions
	TO_ADD_WATCH=251,
};



//===================================================================================================
#define EVENT_SIZE  (sizeof(struct inotify_event))
#define BUF_LEN     (1024*(EVENT_SIZE+16))
char EventBuf[BUF_LEN] __attribute__ ((aligned(__alignof__(struct inotify_event))));

struct INEventCopy //copy of struct inotify_event
{
	WDINOTIFY wd;
	int mask, cookie, len;
	std::string name;
	void clear() { wd=0; mask=cookie=len=0; name.clear(); }
	virtual ~INEventCopy() {}
	INEventCopy() { clear(); }
	INEventCopy(int w, int m, int c, int l, std::string n) { wd=w; mask=m; cookie=c; len=l; name=n; }
	INEventCopy(const INEventCopy &E) { *this=E; }
	INEventCopy(struct inotify_event *pie) { wd=pie->wd; mask=pie->mask; cookie=pie->cookie; len=pie->len; name=pie->name; }
	INEventCopy& operator=(const INEventCopy &E) { wd=E.wd; mask=E.mask; cookie=E.cookie; len=E.len; name=E.name; return *this; }
	bool isnull() { return (wd==0); }
};

//WHEN_PAUSE...test this ...todo...
INEventCopy LOSTEVENTS(WDINOTIFY w) { return INEventCopy(w, IN_Q_OVERFLOW, 0, 0, ""); }

struct EventList
{
private: //no copying..
	EventList(const EventList&);
	void operator=(const EventList&);
public:
	std::queue<INEventCopy> Q{};
	~EventList() {}
	EventList() {}
	bool empty() { return Q.empty(); }
	void clear() { while (Q.size()) Q.pop(); }
	void put(const INEventCopy &E) { Q.push(E); }
	INEventCopy get() { INEventCopy E{}; if (!empty()) { E=Q.front(); Q.pop(); } return E; }
};

EventList EventQ{}; //only instance


//===================================================================================================
//reducing multiple inotify-events to single action..
struct Action
{
	FSACTION act;
	std::string sf;
	std::string st;
	int last, cookie;
	bool ready;
	virtual ~Action() {}
	Action() : act{FS_NONE}, sf{}, st{}, last{0}, cookie{0}, ready{false} {}
	Action(FSACTION a, const std::string &f, const std::string &t, int l, int c, bool r) : act{a}, sf{f}, st{t}, last{l}, cookie{c}, ready{r} {}
	Action(const Action &ea) { *this=ea; }
	Action& operator=(const Action &ea) { act=ea.act; sf=ea.sf; st=ea.st; last=ea.last; cookie=ea.cookie; ready=ea.ready; return *this; }
	void set(FSACTION a, int l, bool b=false) { act=a; last=l; ready=b; }
};

typedef std::vector<Action> Actions;
struct WDActionsPair
{
	WDINOTIFY first;
	Actions second;
	WDActionsPair() {}
	WDActionsPair(WDINOTIFY wd, Actions a) : first(wd), second(a) {} // ((std::pair<WDINOTIFY, Actions>)*this)=std::make_pair(wd, a); }
	WDActionsPair(const WDActionsPair &P) { *this=P; }
	WDActionsPair& operator=(const WDActionsPair &P) { first=P.first; second=P.second; return *this; }
};

//WHEN_PAUSE...test this ...todo...
WDActionsPair APREFRESH(WDINOTIFY w) { return WDActionsPair(w, Actions{Action(FS_REFRESH, "", "", 0, 0, true)}); }

struct ActionList
{
private: //no copying..
	ActionList(const ActionList&);
	void operator=(const ActionList&);
public:
	std::deque<WDActionsPair> Q{};
	~ActionList() { Q.clear(); }
	ActionList() { Q.clear(); }
	bool empty() { return Q.empty(); }
	void clear() { Q.clear(); }
	void put(const WDActionsPair &wap) { Q.push_back(wap); }
	//todo: replace get() with specific: e.g.: bool getready() {...
	WDActionsPair get() { WDActionsPair wap{}; if (!empty()) { wap=Q.front(); Q.pop_front(); } return wap; }
	void removeWD(WDINOTIFY wd) { auto it=Q.begin(); while (it!=Q.end()) { if ((*it).first==wd) { Q.erase(it); break; } it++; }}
};

ActionList ActionQ{}; //only instance


//===================================================================================================
struct UserWatch
{
	std::string owner;	//user-watch owner
	std::string ku;		//user's key
	std::string diru;	//user's directory to be watched
	CBWATCH cbu;		//user's callback
	virtual ~UserWatch()						{}
	UserWatch() : owner{}, ku{}, diru{}, cbu{}			{}
	UserWatch(std::string ko, std::string kw, std::string dir, CBWATCH cb) : owner{ko}, ku{kw}, diru{dir}, cbu{cb} {}
	UserWatch(const UserWatch &U)				{ *this=U; }
	UserWatch& operator=(const UserWatch &U)	{ owner=U.owner; ku=U.ku; diru=U.diru; cbu=U.cbu; return *this; }
	void reset() { diru.clear(); }
	bool isvalid() { return !diru.empty(); }
	void DoCallback(Action A)
	{
		if (cbu)
		{
			std::string sf=path_append(diru, A.sf);
			std::string st=((A.st.empty())?"":path_append(diru, A.st));
			int tout=10;
			while ((tout>0)&&!cbu(ku, A.act, sf, st)) { tout--; kipu(250); }
			//cbu(ku, A.act, sf, st);
			//std::thread(cbu, ku, A.act, sf, st).detach();
		}
	}
};

typedef std::vector<UserWatch> UserWatches;
typedef std::map<WDINOTIFY, UserWatches> WatchList;


//===================================================================================================
struct Watches
{
	WatchList WL{};

	void clear() { if (!WL.empty()&&(fdINOTIFY>=0)) { for (auto& p:WL) inotify_rm_watch(fdINOTIFY, p.first); } WL.clear(); }
	bool empty() { return WL.empty(); }

	~Watches() { clear(); }
	Watches() { WL.clear(); }

	void add(WDINOTIFY wd, const UserWatch &uw) //also modifies by replacing
	{
		UserWatches &v=WL[wd];
		auto it=v.begin(); while (it!=v.end()) { if (it->ku==uw.ku) { v.erase(it); break; } it++; };
		v.push_back(uw);
	}

	void getcopy(WDINOTIFY wd, UserWatches &U) { if (WL.find(wd)!=WL.end()) U=WL[wd]; else U.clear(); }

	void remove_wd(WDINOTIFY wd)
	{
		if (WL.find(wd)==WL.end()) return;
		if (fdINOTIFY>=0) inotify_rm_watch(fdINOTIFY, wd);
		WL.erase(wd);
		ActionQ.removeWD(wd);
	}

	void remove_owner(std::string kOwn)
	{
		if (kOwn.empty()) return;
		auto it=WL.begin();
		while (it!=WL.end())
		{
			auto uit=it->second.begin();
			while (uit!=it->second.end()) { if (kOwn==uit->owner) uit=it->second.erase(uit); else uit++; }
			if (it->second.empty()) { ActionQ.removeWD(it->first); it=WL.erase(it); } else it++;
		}
	}

	void remove_key(std::string kOwn, std::string kW)
	{
		if (kW.empty()) return;
		bool b=false;
		auto it=WL.begin();
		while (!b&&(it!=WL.end()))
		{
			auto uit=it->second.begin();
			while (!b&&(uit!=it->second.end()))
			{
				if ((kOwn==uit->owner)&&(kW==uit->ku)) { it->second.erase(uit); b=true; } else uit++;
			}
			if (b&&it->second.empty()) { ActionQ.removeWD(it->first); it=WL.erase(it); } else it++;
		}
	}

	void remove_dir(std::string kOwn, std::string sdir)
	{
		if (sdir.empty()) return;
		bool b=false;
		auto it=WL.begin();
		while (!b&&(it!=WL.end()))
		{
			auto uit=it->second.begin();
			while (!b&&(uit!=it->second.end()))
			{
				if ((kOwn==uit->owner)&&(seqs(sdir, uit->diru))) { it->second.erase(uit); b=true; } else uit++;
			}
			if (b&&it->second.empty()) { ActionQ.removeWD(it->first); it=WL.erase(it); } else it++;
		}
	}

};

Watches watches{};
inline bool has_watches() { return !watches.empty(); }


//===================================================================================================
void T_Event_Handler()
{
	while (!b_stop_event_handler)
	{
		int l;
		if ((l=read(fdINOTIFY, EventBuf, BUF_LEN ))>0)
		{
			int n=0;
			while (n<l)
			{
				struct inotify_event *pIE=(struct inotify_event*)&EventBuf[n];
				//WHEN_PAUSE...test this ...todo...
				//if (bWatcherPaused) { EventQ.put(LOSTEVENTS(pIE->wd)); }
				//else
					EventQ.put(INEventCopy(pIE));
				n+=(EVENT_SIZE+pIE->len);
			}
		}
		kipu(TO_EVENT);
	}
}


//===================================================================================================

typedef std::map<WDINOTIFY, Actions> EventActions;

//helper for T_Filter_Handler()-thread..
void filter_action(EventActions &EA)
{
	auto ea_it=EA.begin();
	while (ea_it!=EA.end())
	{
		Actions ready{};
		Actions &waiting=ea_it->second;
		auto wait_it=waiting.begin();
		while (wait_it!=waiting.end())
		{
			if (wait_it->ready) { ready.push_back((*wait_it)); wait_it=waiting.erase(wait_it); }
			else { if (wait_it->last==0) wait_it->ready=true; else wait_it->last=0; wait_it++; }
		}
		if (!ready.empty()) { ActionQ.put(WDActionsPair(ea_it->first, ready)); ready.clear(); }
		if (waiting.empty()) ea_it=EA.erase(ea_it); else ea_it++;
	}
}

#define WATCH_GONE isevent(IN_DELETE_SELF)||isevent(IN_UNMOUNT)||isevent(IN_MOVE_SELF)
void filter_event(EventActions &EA, INEventCopy E)
{
	if (E.isnull()) return;
	auto isevent=[=](int n)->bool{ return ((int(E.mask)&n)==n); };
	Actions &actions=EA[E.wd]; //create if not exist
	if (WATCH_GONE) { actions.emplace_back(Action(FS_REMOVED, E.name, "", 0, 0, true)); } //..watch removed..
	else if (isevent(IN_Q_OVERFLOW)) { actions.emplace_back(Action(FS_REFRESH, "", "", 0, 0, true)); } //..lost events
	else if (E.len>0) //..file-system-events..
	{
		bool bx{false};
		if (isevent(IN_MOVED_FROM)) {  actions.emplace_back(Action(FS_DELETED, E.name, "", IN_MOVED_FROM, E.cookie, false)); } //..deleted?
		else if (isevent(IN_MOVED_TO)) //..renamed/created?
		{
			for (auto& a:actions) { if (a.cookie==(int)E.cookie) { a.act=FS_RENAMED; a.st=E.name; a.last=a.cookie=0; a.ready=true; bx=true; break; }}
			if (!bx) { actions.emplace_back(Action(FS_CREATED, E.name, "", 0, 0, true)); }
		}
		else if (isevent(IN_MODIFY)||isevent(IN_ATTRIB)) //..updated
		{
			for (auto a:actions) { if (a.sf==E.name) { bx=true; break; }}
			if (!bx) { actions.emplace_back(Action(FS_UPDATED, E.name, "", 0, 0, true)); }
		}
		else if (isevent(IN_CREATE)) { actions.emplace_back(Action(FS_CREATED, E.name, "", 0, 0, true)); }
		else if (isevent(IN_DELETE)) { actions.emplace_back(Action(FS_DELETED, E.name, "", 0, 0, true)); }
	} // ignore everything else
}

void T_Filter_Handler()
{
	EventActions EA{};
	int nCleanUp{0}; //filter_action:(to settle in_moved_from/to) effects: (3)set last=0, (2)set ready, (1)do call
	while (!b_stop_filter_handler)
	{
		//WHEN_PAUSE...test this ...todo...
		//if (bWatcherPaused)
		//{
		//	while (!EventQ.empty())
		//	{
		//		bool b{false};
		//		INEventCopy E=EventQ.get();
		//		auto it=ActionQ.begin(); while (!b&&(it!=ActionQ.end())) { if ((b=((*it).first==wd))) break; else it++; }
		//		if (!b) ActionQ.put(APREFRESH(E.wd));
		//	}
		//}
		//else
		//{
			if (EventQ.empty()) { if (nCleanUp) { filter_action(EA); nCleanUp--; }}
			else { while (!EventQ.empty()) { filter_event(EA, EventQ.get()); } nCleanUp=3; }
		//}
		kipu(TO_FILTER);
	}
}


//===================================================================================================
void T_Apply_Handler()
{
	while (!b_stop_apply_handler)
	{
		kipu(TO_APPLY);
		if (!bWatcherPaused)
		{
			WDActionsPair wap;
			while (!ActionQ.empty())
			{
				wap=ActionQ.get();
				UserWatches uws;
//poss need to lock?
				watches.getcopy(wap.first, uws);
				if (!uws.empty()) { for (auto& uw:uws) { for (auto& a:wap.second) { uw.DoCallback(a); }}}
			}
		}
	}
}


//==================================================================================================
bool get_inotify()
{
	if (fdINOTIFY==(-1)) { watches.clear(); fdINOTIFY=inotify_init1(IN_NONBLOCK); }
	if (fdINOTIFY<0) return report_error(spf("Error: get_inotify: cannot initialize inotify [", errno, "]"));
	return true;
}

void release_inotify() { if (fdINOTIFY>(-1)) close(fdINOTIFY); fdINOTIFY=(-1); }

//==================================================================================================
bool start_event_thread()
{
	if (T_Event.joinable()) return true;
	if (!get_inotify()) return false;
	b_stop_event_handler=false;
	T_Event=std::thread(T_Event_Handler);
	return T_Event.joinable();
}

void stop_event_thread() { if (T_Event.joinable()) { b_stop_event_handler=true; T_Event.join(); }}

//----------------------------------------------------------------------------------------------
bool start_filter_thread()
{
	if (T_Filter.joinable()) return true;
	b_stop_filter_handler=false;
	T_Filter=std::thread(T_Filter_Handler);
	return T_Filter.joinable();
}

void stop_filter_thread() { if (T_Filter.joinable()) { b_stop_filter_handler=true; T_Filter.join(); }}

//----------------------------------------------------------------------------------------------
bool start_apply_thread()
{
	if (T_Apply.joinable()) return true;
	b_stop_apply_handler=false;
	T_Apply=std::thread(T_Apply_Handler);
	return T_Apply.joinable();
}

void stop_apply_thread() { if (T_Apply.joinable()) { b_stop_apply_handler=true; T_Apply.join(); }}


//===================================================================================================
bool start_threads() { return (start_event_thread()&&start_filter_thread()&&start_apply_thread()); }

void stop_threads() { stop_event_thread(); stop_filter_thread(); stop_apply_thread(); }


//===================================================================================================
bool StartWatch(std::string kOwn, std::string kW, const std::string sdir, CBWATCH cb)
{
	bool bret=false;
	if (!get_inotify()) return report_error("StartWatch: cannot access inotify");
	if (kOwn.empty()||kW.empty()||!dir_exist(sdir)||!cb) return report_error("StartWatch: invalid arguments");
	while (!MUX_WATCH.try_lock()) kipu(TO_ADD_WATCH+31);
	try
	{
		WDINOTIFY wd=inotify_add_watch(fdINOTIFY, sdir.c_str(), MONITOR_MASK);
		if (!start_threads()) { stop_threads(); } //kill any thread that may have started
		else { if (wd>=0) { watches.add(wd, UserWatch(kOwn, kW, sdir, cb)); bret=true; }}
	} catch(...) {}
	MUX_WATCH.unlock();
	if (!bret) report_error("StartWatch: cannot start threads");
	return bret;
}

void StopWatch(std::string kOwn, std::string kW)
{
	while (!MUX_WATCH.try_lock()) kipu(TO_DEL_WATCH);
	try { watches.remove_key(kOwn, kW); } catch(...) {}
	MUX_WATCH.unlock();
	if (watches.empty()) StopAllWatches();
}

void StopWatchDir(std::string kOwn, std::string sdir)
{
	while (!MUX_WATCH.try_lock()) kipu(TO_DEL_WATCH);
	try { watches.remove_dir(kOwn, sdir); } catch(...) {}
	MUX_WATCH.unlock();
	if (watches.empty()) StopAllWatches();
}

void StopWatches(std::string kOwn)
{
	while (!MUX_WATCH.try_lock()) kipu(TO_DEL_WATCH);
	try { watches.remove_owner(kOwn); } catch(...) {}
	MUX_WATCH.unlock();
	if (watches.empty()) StopAllWatches();
}

int IsWatching() { if (bWatcherPaused) return (-1); return has_watches(); }

void StopAllWatches()
{
	EventQ.clear();
	ActionQ.clear();
	watches.clear();
	stop_threads();
	release_inotify();
}

void PauseWatcher(bool bPause) { if ((bWatcherPaused=bPause)) { EventQ.clear(); ActionQ.clear(); }}

bool IsWatcherPaused() { return bWatcherPaused; }



//===================================================================================================
//DEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUG

void show_active_watches()
{
	std::string sws{};
	if (watches.WL.empty()) sws="(IN): empty\n";
	else
	{
		for (auto p:watches.WL)
		{
			spfs(sws, "WDINOTIFY=", p.first);
			for (auto u:p.second)
			{
				spfs(sws, "\n\t", u.owner, " -> [", u.ku, " + ", u.diru, "]");
			}
			spfs(sws, "\n");
		}
	}
	telluser(sws);
}

//GUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBED
//===================================================================================================


} //namespace watcher

