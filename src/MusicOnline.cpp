#include "MusicOnline.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace fs = std::filesystem;

namespace {
	struct Handle {
		HINTERNET h = nullptr;
		explicit Handle(HINTERNET handle) : h(handle) {}
		~Handle() {
			if (h) {
				WinHttpCloseHandle(h);
			}
		}
		Handle(const Handle &) = delete;
		Handle &operator=(const Handle &) = delete;
		operator HINTERNET() const { return h; }
	};

	std::wstring widen(const std::string &value) {
		if (value.empty()) {
			return {};
		}
		const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
		if (size <= 1) {
			return {};
		}
		std::wstring wide(size - 1, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), size);
		return wide;
	}

	std::string narrow(const std::wstring &value) {
		if (value.empty()) {
			return {};
		}
		const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
		if (size <= 0) {
			return {};
		}
		std::string out(size, '\0');
		WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr);
		return out;
	}

	std::string lower(std::string value) {
		for (auto &ch : value) {
			ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
		}
		return value;
	}

	std::string trimmed(const std::string &value) {
		size_t b = 0, e = value.size();
		while (b < e && isspace(static_cast<unsigned char>(value[b]))) ++b;
		while (e > b && isspace(static_cast<unsigned char>(value[e - 1]))) --e;
		return value.substr(b, e - b);
	}

	std::string queryHeaderString(HINTERNET request, DWORD level) {
		DWORD size = 0;
		WinHttpQueryHeaders(request, level, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size,
		                    WINHTTP_NO_HEADER_INDEX);
		if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
			return {};
		}
		std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
		if (!WinHttpQueryHeaders(request, level, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &size,
		                         WINHTTP_NO_HEADER_INDEX)) {
			return {};
		}
		buffer.resize(size / sizeof(wchar_t));
		return narrow(buffer);
	}

	// attachment; filename="Some Song.xm"  ->  Some Song.xm
	std::string fileNameFromDisposition(const std::string &disposition) {
		const std::string low = lower(disposition);
		size_t pos = low.find("filename=");
		if (pos == std::string::npos) {
			return {};
		}
		std::string value = disposition.substr(pos + 9);
		if (!value.empty() && value[0] == '"') {
			value.erase(0, 1);
			const size_t quote = value.find('"');
			if (quote != std::string::npos) {
				value.erase(quote);
			}
		} else {
			const size_t semi = value.find(';');
			if (semi != std::string::npos) {
				value.erase(semi);
			}
		}
		value = trimmed(value);
		const size_t slash = value.find_last_of("\\/");
		if (slash != std::string::npos) {
			value.erase(0, slash + 1);
		}
		return value;
	}

	std::string extensionOf(const std::string &name) {
		const size_t dot = name.find_last_of('.');
		return dot == std::string::npos ? std::string() : name.substr(dot);
	}

	// ma<digits>_<anything>
	bool isOurCacheName(const std::string &name) {
		if (name.size() < 4 || name[0] != 'm' || name[1] != 'a') {
			return false;
		}
		size_t i = 2;
		while (i < name.size() && isdigit(static_cast<unsigned char>(name[i]))) ++i;
		return i > 2 && i < name.size() && name[i] == '_';
	}

	bool endsWith(const std::string &value, const std::string &suffix) {
		return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	void doLog(const MusicOnline::LogFn &log, const std::string &message) {
		if (log) {
			log(message);
		}
	}
}

bool MusicOnline::isTrackerExtension(const std::string &ext) {
	static const std::set<std::string> kExt = {
		".mod", ".xm", ".it", ".s3m", ".stm", ".669", ".mtm", ".far", ".ult", ".okt", ".med",
		".amf", ".dbm", ".dsm", ".gdm", ".imf", ".mdl", ".psm", ".ptm", ".rtm", ".umx", ".dmf",
		".digi", ".sfx", ".mmd0", ".mmd1", ".mmd2", ".mmd3", ".j2b"};
	return kExt.count(lower(ext)) != 0;
}

std::string MusicOnline::sanitizeFileName(const std::string &name) {
	std::string ext = extensionOf(name);
	std::string stem = ext.empty() ? name : name.substr(0, name.size() - ext.size());
	auto clean = [](std::string &part, size_t limit) {
		for (auto &ch : part) {
			const unsigned char u = static_cast<unsigned char>(ch);
			if (!(isalnum(u) || ch == '-' || ch == '_' || ch == '.')) {
				ch = '_';
			}
		}
		if (part.size() > limit) {
			part.resize(limit);
		}
	};
	clean(stem, 48);
	clean(ext, 8);
	if (stem.empty()) {
		stem = "track";
	}
	return stem + ext;
}

MusicOnline::HttpResult MusicOnline::download(const std::string &url, const std::string &destPath,
                                              unsigned long long maxBytes, const AcceptFn &accept) {
	HttpResult result;
	const std::wstring wideUrl = widen(url);

	URL_COMPONENTS components{};
	components.dwStructSize = sizeof(components);
	wchar_t hostName[256]{};
	wchar_t urlPath[2048]{};
	wchar_t extraInfo[2048]{}; // "?query#fragment": WinHttpCrackUrl keeps it out of the path
	components.lpszHostName = hostName;
	components.dwHostNameLength = _countof(hostName);
	components.lpszUrlPath = urlPath;
	components.dwUrlPathLength = _countof(urlPath);
	components.lpszExtraInfo = extraInfo;
	components.dwExtraInfoLength = _countof(extraInfo);
	if (wideUrl.empty() || !WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &components)) {
		result.error = "bad url";
		return result;
	}
	if (components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS) {
		result.error = "only http(s) urls are supported";
		return result;
	}

	std::wstring target = urlPath[0] ? urlPath : L"/";
	std::wstring extra = extraInfo;
	const size_t fragment = extra.find(L'#');
	if (fragment != std::wstring::npos) {
		extra.erase(fragment);
	}
	target += extra;

	Handle session(WinHttpOpen(L"wkJellyWorm/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
	                           WINHTTP_NO_PROXY_BYPASS, 0));
	if (!session) {
		result.error = "request failed (WinHttpOpen " + std::to_string(GetLastError()) + ")";
		return result;
	}
	// resolve, connect, send, receive (ms)
	WinHttpSetTimeouts(session, 5000, 5000, 10000, 15000);
	// Windows 7 offers only SSL3/TLS 1.0 by default and modarchive.org needs TLS 1.2. The call fails
	// harmlessly where TLS 1.2 is not available, then the system default stays.
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 0x00000080
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 0x00000200
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif
	DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
	WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

	Handle connect(WinHttpConnect(session, hostName, components.nPort, 0));
	if (!connect) {
		result.error = "request failed (WinHttpConnect " + std::to_string(GetLastError()) + ")";
		return result;
	}
	const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
	Handle request(WinHttpOpenRequest(connect, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
	                                  WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
	if (!request) {
		result.error = "request failed (WinHttpOpenRequest " + std::to_string(GetLastError()) + ")";
		return result;
	}
	if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
	    !WinHttpReceiveResponse(request, nullptr)) {
		result.error = "request failed (" + std::to_string(GetLastError()) + ")";
		return result;
	}

	DWORD status = 0;
	DWORD statusSize = sizeof(status);
	if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
	                         WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
		status = 0;
	}
	result.status = status;
	result.contentType = lower(trimmed(queryHeaderString(request, WINHTTP_QUERY_CONTENT_TYPE)));
	result.fileName = fileNameFromDisposition(queryHeaderString(request, WINHTTP_QUERY_CONTENT_DISPOSITION));
	DWORD length = 0;
	DWORD lengthSize = sizeof(length);
	if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
	                        WINHTTP_HEADER_NAME_BY_INDEX, &length, &lengthSize, WINHTTP_NO_HEADER_INDEX)) {
		result.contentLength = length;
	}

	if (status != 200) {
		result.error = "HTTP " + std::to_string(status);
		return result;
	}
	if (result.contentLength > maxBytes) {
		result.error = "too big (" + std::to_string(result.contentLength / 1024) + " KB)";
		return result;
	}
	if (accept) {
		const std::string reason = accept(result);
		if (!reason.empty()) {
			result.error = reason;
			return result;
		}
	}

	const std::string partPath = destPath + ".part";
	std::error_code ec;
	bool complete = false;
	unsigned long long total = 0;
	{
		std::ofstream out(partPath, std::ios::binary | std::ios::trunc);
		if (!out.good()) {
			result.error = "cannot write " + partPath;
			return result;
		}
		std::vector<char> chunk(64 * 1024);
		const DWORD startTick = GetTickCount();
		for (;;) {
			// Per-operation timeouts do not stop a server that sends a few bytes now and then.
			if (GetTickCount() - startTick > 120000) {
				result.error = "download timed out";
				break;
			}
			DWORD available = 0;
			if (!WinHttpQueryDataAvailable(request, &available)) {
				result.error = "connection lost";
				break;
			}
			if (available == 0) {
				complete = true;
				break;
			}
			DWORD read = 0;
			const DWORD toRead = (std::min)(available, static_cast<DWORD>(chunk.size()));
			if (!WinHttpReadData(request, chunk.data(), toRead, &read) || read == 0) {
				result.error = "connection lost";
				break;
			}
			total += read;
			if (total > maxBytes) {
				result.error = "too big (more than " + std::to_string(maxBytes / 1024) + " KB)";
				break;
			}
			out.write(chunk.data(), read);
		}
		out.close();
		if (complete && out.fail()) {
			complete = false;
			result.error = "write failed";
		}
	}
	if (!complete || total == 0) {
		if (result.error.empty()) {
			result.error = "empty response";
		}
		fs::remove(partPath, ec);
		return result;
	}
	fs::rename(partPath, destPath, ec);  // replaces an existing file
	if (ec) {
		std::error_code retry;
		fs::remove(destPath, retry);
		fs::rename(partPath, destPath, ec);
	}
	if (ec) {
		result.error = "cannot rename: " + ec.message();
		fs::remove(partPath, ec);
		return result;
	}
	result.ok = true;
	return result;
}

MusicOnline::Fetched MusicOnline::fetchModArchiveId(const std::string &cacheDir, int id, unsigned long long maxBytes,
                                                    const LogFn &log) {
	Fetched fetched;
	fetched.id = id;
	std::error_code ec;
	fs::create_directories(cacheDir, ec);

	const std::string prefix = "ma" + std::to_string(id) + "_";
	for (fs::directory_iterator it(cacheDir, ec), end; !ec && it != end; it.increment(ec)) {
		std::error_code fec;
		const std::string name = it->path().filename().string();
		if (it->is_regular_file(fec) && name.rfind(prefix, 0) == 0 && isTrackerExtension(extensionOf(name)) &&
		    fs::file_size(it->path(), fec) > 0) {
			fetched.ok = true;
			fetched.path = it->path().string();
			fetched.name = name.substr(prefix.size());
			doLog(log, "mod archive #" + std::to_string(id) + " already cached: " + name);
			return fetched;
		}
	}

	// One temp file per call: two workers that fetch the same id at once must not share a .part file.
	static std::atomic<unsigned> tempCounter{0};
	const std::string tmp =
		(fs::path(cacheDir) / ("ma" + std::to_string(id) + "." + std::to_string(++tempCounter) + ".dl")).string();
	const std::string url = "https://api.modarchive.org/downloads.php?moduleid=" + std::to_string(id);
	const HttpResult http = download(url, tmp, maxBytes, [](const HttpResult &h) -> std::string {
		// An unknown id answers 200 with a short text/html "Invalid ID Error".
		if (h.contentType.rfind("text/", 0) == 0 || h.fileName.empty()) {
			return "no such module";
		}
		const std::string ext = extensionOf(h.fileName);
		if (!isTrackerExtension(ext)) {
			return "unsupported format '" + ext + "'";
		}
		return {};
	});
	if (!http.ok) {
		fetched.error = http.error;
		fs::remove(tmp + ".part", ec);
		return fetched;
	}

	const std::string safe = sanitizeFileName(http.fileName);
	const std::string finalPath = (fs::path(cacheDir) / (prefix + safe)).string();
	fs::rename(tmp, finalPath, ec);  // replaces an existing file
	if (ec) {
		// Another worker may have delivered the same module and be reading it: use that copy.
		std::error_code sec;
		const auto existing = fs::file_size(finalPath, sec);
		if (!sec && existing > 0) {
			fs::remove(tmp, sec);
			ec.clear();
		} else {
			fetched.error = "cannot rename: " + ec.message();
			fs::remove(tmp, ec);
			return fetched;
		}
	}
	fetched.ok = true;
	fetched.path = finalPath;
	fetched.name = safe;
	doLog(log, "mod archive #" + std::to_string(id) + " downloaded: " + safe + " (" +
	               std::to_string(fs::file_size(finalPath, ec) / 1024) + " KB)");
	return fetched;
}

MusicOnline::Fetched MusicOnline::fetchRandomModArchive(const std::string &cacheDir, int maxId,
                                                        unsigned long long maxBytes, int attempts, std::mt19937 &rng,
                                                        const std::function<bool(int)> &skipId,
                                                        const std::function<bool()> &cancelled, const LogFn &log) {
	Fetched last;
	last.error = "no attempt made";
	if (maxId < 1) {
		maxId = 1;
	}
	std::uniform_int_distribution<int> dist(1, maxId);
	int networkFailures = 0;
	for (int attempt = 0; attempt < attempts; ++attempt) {
		if (cancelled && cancelled()) {
			last.error = "cancelled";
			return last;
		}
		int id = dist(rng);
		for (int tries = 0; tries < 8 && skipId && skipId(id); ++tries) {
			id = dist(rng);
		}
		last = fetchModArchiveId(cacheDir, id, maxBytes, log);
		if (last.ok) {
			return last;
		}
		doLog(log, "mod archive #" + std::to_string(id) + " skipped: " + last.error);
		if (last.error.rfind("request failed", 0) == 0 && ++networkFailures >= 2) {
			break; // offline: do not keep waiting for timeouts
		}
	}
	last.ok = false;
	return last;
}

int MusicOnline::pruneCache(const std::string &cacheDir, size_t keepFiles) {
	std::error_code ec;
	if (!fs::is_directory(cacheDir, ec)) {
		return 0;
	}
	struct Entry {
		fs::path path;
		fs::file_time_type time;
	};
	std::vector<Entry> ours;
	int removed = 0;
	const auto now = fs::file_time_type::clock::now();
	for (fs::directory_iterator it(cacheDir, ec), end; !ec && it != end; it.increment(ec)) {
		std::error_code fec;
		if (!it->is_regular_file(fec)) {
			continue;
		}
		const std::string name = it->path().filename().string();
		const auto time = fs::last_write_time(it->path(), fec);
		if (fec) {
			continue;
		}
		const bool leftover = endsWith(name, ".part") || endsWith(name, ".dl");
		if (leftover) {
			if (now - time > std::chrono::minutes(10)) {
				fs::remove(it->path(), fec);
				++removed;
			}
			continue;
		}
		if (isOurCacheName(name) && isTrackerExtension(extensionOf(name))) {
			ours.push_back({it->path(), time});
		}
	}
	if (ours.size() > keepFiles) {
		std::sort(ours.begin(), ours.end(), [](const Entry &a, const Entry &b) { return a.time > b.time; });
		for (size_t i = keepFiles; i < ours.size(); ++i) {
			std::error_code fec;
			if (fs::remove(ours[i].path, fec)) {
				++removed;
			}
		}
	}
	return removed;
}
