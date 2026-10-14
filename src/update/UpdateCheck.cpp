#include "UpdateCheck.h"
#include "version.h"
#include <winhttp.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <thread>
#include <vector>

namespace UpdateCheck {

namespace {

constexpr wchar_t kApiHost[]      = L"api.github.com";
constexpr wchar_t kLatestPath[]   = L"/repos/lanecwheeler/SeeRT-Overlay/releases/latest";
constexpr wchar_t kProjectPage[]  = L"https://github.com/lanecwheeler/SeeRT-Overlay";
constexpr wchar_t kReleasesPage[] = L"https://github.com/lanecwheeler/SeeRT-Overlay/releases";
constexpr char    kReleasesPageA[] = "https://github.com/lanecwheeler/SeeRT-Overlay/releases";
constexpr char    kReleaseUrlPrefix[] = "https://github.com/lanecwheeler/SeeRT-Overlay/releases/";
constexpr char    kDownloadPrefix[]   = "https://github.com/lanecwheeler/SeeRT-Overlay/releases/download/";
constexpr size_t  kMaxResponse    = 1 << 20;   // the API's answer is a few KB

// Shared between the check thread and the UI thread.  Deliberately never
// destroyed: a check still running when the app exits must not touch a
// destroyed mutex.
struct State {
    std::mutex        mutex;
    Result            latest;
    std::atomic<bool> running{ false };
};
State& Shared() {
    static State* s = new State;
    return *s;
}

// Closes a WinHTTP handle when it goes out of scope.
struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET handle) : h(handle) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle&)            = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

// HTTPS GET of host + path, following redirects.  Returns false if the
// request couldn't be made or the body is over maxBytes; otherwise fills in
// the status code and the body.
bool HttpsGet(const wchar_t* host, const wchar_t* path, const wchar_t* accept, size_t maxBytes,
              DWORD& status, std::string& body,
              const std::function<void(size_t, size_t)>& progress = {}) {
    const std::string  ua8 = std::string("SeeRT-Overlay/") + SEERT_VERSION;
    const std::wstring ua(ua8.begin(), ua8.end());   // ASCII

    Handle session(WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return false;
    WinHttpSetTimeouts(session.h, 5000, 5000, 10000, 10000);   // resolve, connect, send, receive (ms)

    Handle connection(WinHttpConnect(session.h, host, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection) return false;
    Handle request(WinHttpOpenRequest(connection.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) return false;

    const std::wstring headers = std::wstring(L"Accept: ") + accept + L"\r\n"
                                 L"X-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(request.h, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        return false;

    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        return false;

    DWORD length = 0;
    size = sizeof(length);
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &length, &size, WINHTTP_NO_HEADER_INDEX))
        length = 0;   // not sent
    if (length > maxBytes) return false;

    body.clear();
    body.reserve(length);
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) return false;
        if (available == 0) break;
        if (body.size() + available > maxBytes) return false;
        std::vector<char> chunk(available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, chunk.data(), available, &read)) return false;
        body.append(chunk.data(), read);
        if (progress) progress(body.size(), length);
    }
    return true;
}

// "v1.10.2-beta" → {1, 10, 2}.
std::vector<int> ParseVersion(const std::string& s) {
    std::vector<int> parts;
    size_t i = (!s.empty() && (s[0] == 'v' || s[0] == 'V')) ? 1 : 0;
    while (i < s.size() && isdigit(static_cast<unsigned char>(s[i]))) {
        int n = 0;
        while (i < s.size() && isdigit(static_cast<unsigned char>(s[i])) && n < 100000)
            n = n * 10 + (s[i++] - '0');
        parts.push_back(n);
        if (i < s.size() && s[i] == '.') ++i;
        else break;
    }
    return parts;
}

} // namespace

const char*    CurrentVersion()  { return SEERT_VERSION; }
const wchar_t* ProjectPageUrl()  { return kProjectPage; }
const wchar_t* ReleasesPageUrl() { return kReleasesPage; }

bool IsNewer(const std::string& a, const std::string& b) {
    const std::vector<int> va = ParseVersion(a), vb = ParseVersion(b);
    for (size_t i = 0; i < std::max(va.size(), vb.size()); ++i) {
        const int x = i < va.size() ? va[i] : 0;
        const int y = i < vb.size() ? vb[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

Result CheckNow() {
    Result      r;
    DWORD       status = 0;
    std::string body;
    if (!HttpsGet(kApiHost, kLatestPath, L"application/vnd.github+json", kMaxResponse, status, body)) {
        r.status = Status::Failed;
        return r;
    }
    if (status == 404) {                 // no releases published yet
        r.status = Status::UpToDate;
        return r;
    }
    if (status != 200) {
        r.status = Status::Failed;
        return r;
    }

    const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("tag_name") || !j["tag_name"].is_string()) {
        r.status = Status::Failed;
        return r;
    }
    const std::string tag = j["tag_name"].get<std::string>();
    if (ParseVersion(tag).empty()) {     // not a version tag
        r.status = Status::Failed;
        return r;
    }
    r.latestVersion = (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;

    // Only ever open this project's own release pages, whatever the response says.
    r.releaseUrl = kReleasesPageA;
    if (j.contains("html_url") && j["html_url"].is_string()) {
        const std::string url = j["html_url"].get<std::string>();
        if (url.rfind(kReleaseUrlPrefix, 0) == 0) r.releaseUrl = url;
    }

    // The download and its checksum — again, only from this project.
    if (j.contains("assets") && j["assets"].is_array()) {
        auto endsWith = [](const std::string& s, const char* tail) {
            const size_t n = strlen(tail);
            return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
        };
        for (const auto& asset : j["assets"]) {
            if (!asset.is_object() || !asset.contains("name") || !asset["name"].is_string() ||
                !asset.contains("browser_download_url") || !asset["browser_download_url"].is_string())
                continue;
            const std::string name = asset["name"].get<std::string>();
            const std::string url  = asset["browser_download_url"].get<std::string>();
            if (url.rfind(kDownloadPrefix, 0) != 0) continue;
            if (endsWith(name, "-win64.zip"))             r.zipUrl = url;
            else if (endsWith(name, "-win64.zip.sha256")) r.shaUrl = url;
        }
    }

    r.status = IsNewer(r.latestVersion, SEERT_VERSION) ? Status::Available : Status::UpToDate;
    return r;
}

bool Download(const std::string& url, std::string& body, size_t maxBytes,
              const std::function<void(size_t, size_t)>& progress) {
    if (url.rfind("https://", 0) != 0) return false;
    const std::wstring wurl(url.begin(), url.end());   // release URLs are ASCII
    wchar_t host[256] = {}, path[2048] = {};
    URL_COMPONENTS uc = {};
    uc.dwStructSize     = sizeof(uc);
    uc.lpszHostName     = host;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
    uc.lpszUrlPath      = path;
    uc.dwUrlPathLength  = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return false;

    DWORD status = 0;
    return HttpsGet(host, path, L"application/octet-stream", maxBytes, status, body, progress) && status == 200;
}

void Start(HWND notify) {
    State& s = Shared();
    bool expected = false;
    if (!s.running.compare_exchange_strong(expected, true)) return;   // already checking
    std::thread([notify, &s] {
        Result r = CheckNow();
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.latest = std::move(r);
        }
        s.running = false;
        PostMessageW(notify, WM_UPDATE_CHECKED, 0, 0);
    }).detach();
}

bool Running() { return Shared().running; }

Result Latest() {
    State& s = Shared();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.latest;
}

} // namespace UpdateCheck
