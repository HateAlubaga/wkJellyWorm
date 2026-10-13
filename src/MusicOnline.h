#ifndef WKJELLYWORM_MUSICONLINE_H
#define WKJELLYWORM_MUSICONLINE_H

#include <functional>
#include <random>
#include <string>

// Network side of the tracker music player: plain WinHTTP downloads and a random pick
// from The Mod Archive (modarchive.org). No game or Lua dependencies, so it can be
// tested from a console program.
namespace MusicOnline {
	struct HttpResult {
		bool ok = false; // the whole body is saved in destPath
		unsigned status = 0;
		std::string contentType; // lower case
		std::string fileName;    // from Content-Disposition, empty if none
		unsigned long long contentLength = 0; // 0 = unknown
		std::string error;
	};

	// Returns an empty string to accept the response, or the reason to reject it.
	// Called after the headers arrived and before the body is read.
	using AcceptFn = std::function<std::string(const HttpResult &)>;
	using LogFn = std::function<void(const std::string &)>;

	// GET `url` into destPath. The body goes to destPath + ".part" first and is renamed on
	// success, so a half-written file is never visible. The query string of the URL is
	// kept, the "#fragment" is dropped. Redirects are followed. Short timeouts: the call
	// never blocks for long. A body larger than maxBytes is rejected.
	HttpResult download(const std::string &url, const std::string &destPath, unsigned long long maxBytes,
	                    const AcceptFn &accept = nullptr);

	struct Fetched {
		bool ok = false;
		int id = 0;
		std::string path; // cached file, named ma<id>_<name>.<ext>
		std::string name; // file name from the server
		std::string error;
	};

	// One module of The Mod Archive by id, into cacheDir. An already cached id is reused.
	Fetched fetchModArchiveId(const std::string &cacheDir, int id, unsigned long long maxBytes, const LogFn &log);

	// A random id in [1, maxId]; ids rejected by skipId are not tried. Tries up to `attempts`
	// ids because about one in five ids is empty, too big or not a tracker format.
	// cancelled() is polled between attempts.
	Fetched fetchRandomModArchive(const std::string &cacheDir, int maxId, unsigned long long maxBytes, int attempts,
	                              std::mt19937 &rng, const std::function<bool(int)> &skipId,
	                              const std::function<bool()> &cancelled, const LogFn &log);

	// Keeps the newest `keepFiles` ma<id>_* files, removes the rest and stale *.part / *.dl
	// leftovers. Files of other origin (URL cache, user files) are never touched.
	// Returns the number of removed files.
	int pruneCache(const std::string &cacheDir, size_t keepFiles);

	bool isTrackerExtension(const std::string &ext); // ".xm", case insensitive
	// "Some Song (v2).XM" -> "Some_Song__v2_.XM": safe as a file name, extension kept.
	std::string sanitizeFileName(const std::string &name);
}

#endif // WKJELLYWORM_MUSICONLINE_H
