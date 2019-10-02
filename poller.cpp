
#include "watcher.h"
#include <utilfuncs/utilfuncs.h>
#include <vector>
#include <mutex>
#include <exception>
#include <cerrno>
#include <unistd.h>
#include <string.h>
#include <atomic>

namespace watcher
{

std::atomic<bool> atmpoll;
std::thread poll_thread;
std::mutex MUX_B_POLL;
int poll_interval=1000; //500; //milliseconds

struct PollItem
{
	std::string se;
	CBPOLL f;
	virtual void clear() { se.clear(); f=nullptr; }
	virtual ~PollItem() { clear(); }
	//PollItem() { clear(); }
	virtual void check() {}
	virtual std::string getse() { return "\\_oo_/"; }
};

struct PollItems
{
	typedef std::vector<PollItem*> VP;
	VP vp{};
	void clear() { while (vp.size()) { delete (*vp.begin()); vp.erase(vp.begin()); } vp.clear(); }
	~PollItems() { clear(); }
	PollItems() { vp.clear(); }
	void add(PollItem *pi) { vp.push_back(pi); }
	void remove(PollItem *pi)
	{
		auto it=vp.begin();
		while (it!=vp.end()) { if ((*it)==pi) break; else it++; }
		if (it!=vp.end()) { delete (*it); vp.erase(it); }
	}
	void remove(std::string sp)
	{
		PollItem *pi=nullptr;
		for (auto& p:vp) { if (seqs(p->getse(), sp)) { pi=p; break; }}
		if (pi) remove(pi);
	}
	bool has(std::string sp) const
	{
		for (auto p:vp) { if (seqs(p->getse(), sp)) return true; }
		return false;
	}
};

PollItems Polls;

struct PIDir : public PollItem
{
	DirEntries D;
	virtual ~PIDir() { clear(); D.clear(); }
	PIDir(const std::string &sd, CBPOLL cb) { se=sd; f=cb; dir_read(se, D); }
	virtual void check() { DirEntries T; dir_read(se, T); if (D!=T) { D=T; f(se); }}
	virtual std::string getse() { return se; }
};

struct PIFile : public PollItem
{
	uint32_t D;
	virtual ~PIFile() { clear(); }
	PIFile(const std::string &sd, CBPOLL cb) { se=sd; f=cb; file_crc32(se, D); }
	virtual void check() { uint32_t T; file_crc32(se, T); if (D!=T) { D=T; f(se); }}
	virtual std::string getse() { return se; }
};

bool getbpoll()			{ return atmpoll.load(); }
void setbpoll(bool b)	{ std::atomic_exchange(&atmpoll, b); }

void do_poll() { while (getbpoll()) { for (auto p:Polls.vp) { p->check(); } kipm(poll_interval); }}

//==================================================================================================
bool StartPoll(std::string se, CBPOLL cb)
{
	if (isdir(se)) { while (!MUX_B_POLL.try_lock()) kipm(7); try { Polls.add(new PIDir(se, cb)); } catch(...){} MUX_B_POLL.unlock(); }
	if (isfile(se)) { while (!MUX_B_POLL.try_lock()) kipm(11); try { Polls.add(new PIFile(se, cb)); } catch(...){} MUX_B_POLL.unlock(); }
	if ((Polls.vp.size()>0)&&!poll_thread.joinable()) { setbpoll(true); poll_thread=std::thread(do_poll); }
	return poll_thread.joinable();
}

bool IsPoll(std::string se)
{
	if (se.empty()) return poll_thread.joinable();
	else return Polls.has(se);
}

void StopPoll(std::string se)
{
	if (!se.empty())
	{
		while (!MUX_B_POLL.try_lock()) { kipm(13); } try { Polls.remove(se);  } catch(...){} MUX_B_POLL.unlock();
	}
	else //if (se.empty())
	{
		if (poll_thread.joinable()) { setbpoll(false); poll_thread.join(); }
		Polls.clear();
	}
}

//===================================================================================================
//DEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUGDEBUG

void show_active_polls()
{
	std::string sws{};
	if (Polls.vp.empty()) sws="POLL: <empty>\n";
	else for (auto p:Polls.vp)
	{
		spfs(sws, "POLL = ", p->getse(), "\n");
	}
	telluser(sws);
}


//GUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBEDGUBED
//===================================================================================================


} //namespace watcher

