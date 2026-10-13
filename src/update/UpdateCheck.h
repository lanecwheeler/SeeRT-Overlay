#pragma once
#include <Windows.h>
#include <string>

// Checks GitHub for a newer SeeRT Overlay release.
//
// Start() asks the GitHub releases API for the latest published release on a
// background thread, then posts WM_UPDATE_CHECKED to the given window; read
// the outcome with Latest() on that window's thread.  Drafts and pre-releases
// are never offered — GitHub's "latest release" excludes them.
namespace UpdateCheck {

constexpr UINT WM_UPDATE_CHECKED = WM_APP + 101;

enum class Status { NotChecked, UpToDate, Available, Failed };

struct Result {
    Status      status = Status::NotChecked;
    std::string latestVersion;   // the latest release's tag without the leading "v", e.g. "1.2.0"
    std::string releaseUrl;      // that release's GitHub page
};

// This build's version, e.g. "1.1.0".
const char* CurrentVersion();

// The project's releases page on GitHub.
const wchar_t* ReleasesPageUrl();

// True if version a is newer than b.  Compares dot-separated numbers
// ("1.10.0" > "1.9.2"); a leading "v" and any "-suffix" are ignored.
bool IsNewer(const std::string& a, const std::string& b);

// Starts a check in the background; does nothing if one is already running.
void Start(HWND notify);
bool Running();

// The most recent finished check.
Result Latest();

// The check itself, synchronously (what Start runs on its thread).
Result CheckNow();

} // namespace UpdateCheck
