#include "FrameScheduler.h"
#include "packages/PackageManager.h"
#include "Chat.h"
#include "Hud.h"
#include "Input.h"
#include "Lua.h"
#include "Music.h"
#include <sol/sol.hpp>

void FrameScheduler::install() {
	CTaskTurnGame::registerBuiltinCallbackVt8(&onTurnGameVt8);

	auto *lua = Lua::getInstance().getState();
	lua->set_function("jellySchedule", [](unsigned int delayMs, unsigned int intervalMs, sol::protected_function fn) -> int {
		if(!fn.valid())
			return 0;
		return FrameScheduler::schedule(delayMs, intervalMs, [fn]() mutable {
			try {
				// Protected: an unprotected Lua error here would longjmp out of a WA render hook.
				sol::protected_function_result res = fn();
				if(!res.valid()) {
					sol::error err = res;
					printf("jellySchedule callback: %s\n", err.what());
				}
			} catch(const std::exception &e) {
				printf("jellySchedule callback: %s\n", e.what());
			} catch(...) {
				printf("jellySchedule callback: unknown error\n");
			}
		});
	});
	lua->set_function("jellyCancel", [](int id) -> bool {
		return FrameScheduler::cancel(id);
	});
}

int FrameScheduler::onTurnGameVt8(CTaskTurnGame *This, CTask *sender, Constants::TaskMessage mtype, size_t size, void *data) {
	(void)This;
	(void)sender;
	(void)size;
	(void)data;
	if(mtype == Constants::TaskMessage_RenderScene)
		onRenderScene();
	else if(mtype == Constants::TaskMessage_TurnStarted)
		PackageManager::Callbacks<&PackageManager::onTurnStart_t>::callCallbacks();
	return 0;
}

void FrameScheduler::onRenderScene() {
	static unsigned int lastTick = 0;
	unsigned int tick = Input::getTickCount();
	if(tick == lastTick)
		return;
	lastTick = tick;

	tickTimers(tick);
	if(Music::hasAnnouncements()) {
		for(const auto &note : Music::takeAnnouncements())
			Chat::callShowChatMessage(note, 6);
	}
	PackageManager::Callbacks<&PackageManager::onFrame_t>::callCallbacks();
	Hud::flushDrawQueue();
}

int FrameScheduler::schedule(unsigned int delayMs, unsigned int intervalMs, TimerFn fn) {
	if(!fn)
		return 0;
	unsigned int now = Input::getTickCount();
	std::lock_guard<std::mutex> lock(timerMutex);
	int id = nextTimerId++;
	if(nextTimerId <= 0)
		nextTimerId = 1;
	timers.push_back({id, now + delayMs, intervalMs, std::move(fn), false});
	return id;
}

bool FrameScheduler::cancel(int id) {
	if(id <= 0)
		return false;
	std::lock_guard<std::mutex> lock(timerMutex);
	for(auto &t : timers) {
		if(t.id == id && !t.cancelled) {
			t.cancelled = true;
			return true;
		}
	}
	return false;
}

void FrameScheduler::clearTimers() {
	std::lock_guard<std::mutex> lock(timerMutex);
	timers.clear();
}

void FrameScheduler::tickTimers(unsigned int now) {
	std::vector<TimerFn> due;
	{
		std::lock_guard<std::mutex> lock(timerMutex);
		for(auto it = timers.begin(); it != timers.end();) {
			if(it->cancelled) {
				it = timers.erase(it);
				continue;
			}
			if(now < it->nextFire) {
				++it;
				continue;
			}
			due.push_back(it->fn);
			if(it->intervalMs > 0) {
				it->nextFire = now + it->intervalMs;
				++it;
			} else {
				it = timers.erase(it);
			}
		}
	}
	for(auto &fn : due) {
		try {
			fn();
		} catch(const std::exception &e) {
			printf("FrameScheduler timer: %s\n", e.what());
		} catch(...) {
			printf("FrameScheduler timer: unknown error\n");
		}
	}
}
