#ifndef WKJELLYWORM_FRAMESCHEDULER_H
#define WKJELLYWORM_FRAMESCHEDULER_H

#include "entities/tasks/CTaskTurnGame.h"
#include "Constants.h"
#include <functional>
#include <mutex>
#include <vector>

class FrameScheduler {
public:
	using TimerFn = std::function<void()>;

	static void install();
	static int onTurnGameVt8(CTaskTurnGame *This, CTask *sender, Constants::TaskMessage mtype, size_t size, void *data);
	static void onRenderScene();

	// delayMs: fire after this many ms; intervalMs > 0 repeats every intervalMs (0 = one-shot)
	static int schedule(unsigned int delayMs, unsigned int intervalMs, TimerFn fn);
	static bool cancel(int id);
	static void clearTimers();

private:
	struct Timer {
		int id = 0;
		unsigned int nextFire = 0;
		unsigned int intervalMs = 0;
		TimerFn fn;
		bool cancelled = false;
	};

	static void tickTimers(unsigned int now);
	static inline std::mutex timerMutex;
	static inline std::vector<Timer> timers;
	static inline int nextTimerId = 1;
};

#endif //WKJELLYWORM_FRAMESCHEDULER_H
