
#ifndef WKJELLYWORM_DEBUGLOG_H
#define WKJELLYWORM_DEBUGLOG_H

#include <cstdio>
#include <cstring>
#include <windows.h>

// #region agent log
inline void agentDebugLog(const char *hypothesisId, const char *location, const char *message, const char *dataJson = "{}") {
	static CRITICAL_SECTION cs;
	static bool csInit = false;
	if(!csInit) {
		InitializeCriticalSection(&cs);
		csInit = true;
	}
	EnterCriticalSection(&cs);
	FILE *f = nullptr;
	fopen_s(&f, "C:\\Users\\Admin\\Projects\\worms-modding-lab\\debug-51d698.log", "a");
	if(f) {
		fprintf(f,
			"{\"sessionId\":\"51d698\",\"hypothesisId\":\"%s\",\"location\":\"%s\",\"message\":\"%s\",\"data\":%s,\"timestamp\":%llu}\n",
			hypothesisId ? hypothesisId : "",
			location ? location : "",
			message ? message : "",
			dataJson ? dataJson : "{}",
			(unsigned long long)GetTickCount64());
		fclose(f);
	}
	LeaveCriticalSection(&cs);
}
// #endregion

#endif //WKJELLYWORM_DEBUGLOG_H
