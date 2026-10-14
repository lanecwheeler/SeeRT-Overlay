#pragma once
#include <Windows.h>
#include <string>
#include "UpdateCheck.h"

// Downloads a newer release and installs it in place.
//
// Download fetches the release's zip and its .sha256 on a background thread,
// checks the hash, and unpacks the zip into a staging folder, then posts
// WM_UPDATE_READY to the given window.  Install swaps the staged files into
// the app's folder: each file already there is renamed aside first (Windows
// lets a running exe be renamed, not overwritten), and if anything fails the
// old files are put back.  Restart starts the updated copy, which waits for
// this one to exit; CleanUp, at the next launch, removes what was set aside.
namespace Updater {

constexpr UINT WM_UPDATE_READY = WM_APP + 103;   // a download finished, or failed

enum class Status { Idle, Downloading, Ready, Failed };

struct Progress {
    Status      status   = Status::Idle;
    std::string version;            // the release being downloaded / staged
    float       fraction = 0.f;     // of the download, 0–1
};

// Whether updates can be installed here: not a developer build (one inside a
// CMake build folder), and the app's folder can be written to.
bool CanInstall();

// Starts downloading a release in the background; does nothing if a download
// is running or that release is already staged.
void Download(HWND notify, const UpdateCheck::Result& release);
Progress Current();

// Installs the staged update into the app's folder.  Runs once; true if the
// files are in place.
bool Install();
bool Installed();

// Starts the updated app (after Install), which waits for this copy to exit —
// so quit right after.
bool Restart();

// At launch: removes files set aside by an earlier update, and the staged
// download once it's no newer than this build.  A staged update that is
// still newer is kept, ready to install.
void CleanUp();

} // namespace Updater
