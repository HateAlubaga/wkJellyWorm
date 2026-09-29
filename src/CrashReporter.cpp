#include "CrashReporter.h"
#include "Config.h"
#include "packages/PackageManager.h"
#include <git.h>
#include <DbgHelp.h>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

static LONG WINAPI jellyUnhandledExceptionFilter(EXCEPTION_POINTERS *info) {
	try {
		fs::create_directories("jelly\\crash");

		char timestamp[64] = {};
		time_t now = time(nullptr);
		tm localTm = {};
		localtime_s(&localTm, &now);
		strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &localTm);

		std::string dmpPath = std::string("jelly\\crash\\") + timestamp + ".dmp";
		std::string txtPath = std::string("jelly\\crash\\") + timestamp + "_crash.txt";

		HANDLE hFile = CreateFileA(dmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hFile != INVALID_HANDLE_VALUE) {
			MINIDUMP_EXCEPTION_INFORMATION mei;
			mei.ThreadId = GetCurrentThreadId();
			mei.ExceptionPointers = info;
			mei.ClientPointers = FALSE;
			MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
				MiniDumpNormal, info ? &mei : nullptr, nullptr, nullptr);
			CloseHandle(hFile);
		}

		std::ofstream out(txtPath);
		if (out) {
			out << "wkJellyWorm crash report\n";
			out << "version: " << Config::getJellyVersionStr() << "\n";
			out << "built: " << Config::getJellyVersionMessage() << "\n";
			if (GitMetadata::Populated()) {
				out << "git: " << GitMetadata::Describe()
					<< " " << GitMetadata::CommitSHA1()
					<< " (" << GitMetadata::CommitDate() << ")\n";
				out << "git subject: " << GitMetadata::CommitSubject() << "\n";
				if (GitMetadata::AnyUncommittedChanges())
					out << "git: uncommitted changes present\n";
			} else {
				out << "git: (not populated)\n";
			}
			if (info && info->ExceptionRecord) {
				out << "exception code: 0x" << std::hex << info->ExceptionRecord->ExceptionCode << std::dec << "\n";
				out << "exception address: 0x" << std::hex
					<< (uintptr_t)info->ExceptionRecord->ExceptionAddress << std::dec << "\n";
			}
			out << "enabled packages:\n";
			for (auto &id : PackageManager::getInstance().listEnabledPackageIds()) {
				out << "  " << id << "\n";
			}
			out << "minidump: " << dmpPath << "\n";
		}
	} catch (...) {
		// never throw from the filter
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

void CrashReporter::install() {
	fs::create_directories("jelly\\crash");
	SetUnhandledExceptionFilter(jellyUnhandledExceptionFilter);
}
