#include "Updater.h"
#include "../settings/Settings.h"
#include <bcrypt.h>
#include <algorithm>
#include <cctype>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace Updater {

namespace {

constexpr size_t kMaxZip = 200u << 20;   // a release is a few MB

// Shared between the download thread and the UI thread.  Never destroyed, so
// a download still running at exit can't touch a destroyed mutex.
struct State {
    std::mutex        mutex;
    Progress          progress;
    std::atomic<bool> running{ false };
    bool              installed = false;
};
State& Shared() {
    static State* s = new State;
    return *s;
}

void Set(Status status, const std::string& version, float fraction) {
    State& s = Shared();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.progress = { status, version, fraction };
}

fs::path ExePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}
fs::path ExeDir()     { return ExePath().parent_path(); }
fs::path StagingDir() { return Settings::DataDir() / L"update"; }
fs::path StagedFiles() { return StagingDir() / L"files"; }

// The staged update's version, or empty if there's none.
std::string StagedVersion() {
    std::ifstream f(StagingDir() / L"version.txt");
    std::string v;
    if (f) std::getline(f, v);
    std::error_code ec;
    if (!fs::exists(StagedFiles() / L"SeeRTOverlay.exe", ec)) v.clear();
    return v;
}

// SHA-256 of data as lowercase hex.
std::string Sha256Hex(const std::string& data) {
    BCRYPT_ALG_HANDLE  alg  = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32] = {};
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0)) &&
              BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                            static_cast<ULONG>(data.size()), 0)) &&
              BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash) BCryptDestroyHash(hash);
    if (alg)  BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return {};
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char b : digest) { out += kHex[b >> 4]; out += kHex[b & 15]; }
    return out;
}

// Unpacks a zip with the tar.exe that ships with Windows 10 (1803) and later.
bool Unzip(const fs::path& zip, const fs::path& into) {
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    const fs::path tar = fs::path(sys) / L"tar.exe";
    std::wstring cmd = L"\"" + tar.wstring() + L"\" -xf \"" + zip.wstring() + L"\" -C \"" + into.wstring() + L"\"";
    STARTUPINFOW        si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi))
        return false;
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

// The whole download: checksum, zip, verify, unpack.  Runs on its own thread.
bool Fetch(const UpdateCheck::Result& release) {
    std::string sum, zip;
    if (!UpdateCheck::Download(release.shaUrl, sum, 4096)) return false;
    if (!UpdateCheck::Download(release.zipUrl, zip, kMaxZip, [&](size_t done, size_t total) {
            if (total) Set(Status::Downloading, release.latestVersion, static_cast<float>(done) / total);
        }))
        return false;

    // "<64 hex digits>  <file name>"
    std::string expected = sum.substr(0, std::min<size_t>(64, sum.size()));
    for (char& c : expected) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (expected.size() != 64 || Sha256Hex(zip) != expected) return false;

    std::error_code ec;
    fs::remove_all(StagingDir(), ec);
    fs::create_directories(StagedFiles(), ec);
    if (ec) return false;
    const fs::path zipPath = StagingDir() / L"update.zip";
    {
        std::ofstream f(zipPath, std::ios::binary);
        if (!f.write(zip.data(), static_cast<std::streamsize>(zip.size()))) return false;
    }
    if (!Unzip(zipPath, StagedFiles()) || !fs::exists(StagedFiles() / L"SeeRTOverlay.exe", ec)) return false;
    fs::remove(zipPath, ec);
    std::ofstream(StagingDir() / L"version.txt") << release.latestVersion << '\n';
    return true;
}

} // namespace

bool CanInstall() {
    const fs::path dir = ExeDir();
    std::error_code ec;
    // A developer build lives in a CMake build folder (e.g. build/bin/Release);
    // never overwrite one with a release.
    fs::path p = dir;
    for (int i = 0; i < 4 && p.has_parent_path() && p != p.parent_path(); ++i, p = p.parent_path())
        if (fs::exists(p / L"CMakeCache.txt", ec)) return false;
    // Can we write here?  (Not, for example, under Program Files.)
    const fs::path probe = dir / L".seert-update-test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

void Download(HWND notify, const UpdateCheck::Result& release) {
    State& s = Shared();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.progress.status == Status::Ready && s.progress.version == release.latestVersion) return;
    }
    bool expected = false;
    if (!s.running.compare_exchange_strong(expected, true)) return;   // already downloading

    if (release.zipUrl.empty() || release.shaUrl.empty()) {
        Set(Status::Failed, release.latestVersion, 0.f);
        s.running = false;
        PostMessageW(notify, WM_UPDATE_READY, 0, 0);
        return;
    }
    Set(Status::Downloading, release.latestVersion, 0.f);
    std::thread([notify, release, &s] {
        const bool ok = Fetch(release);
        Set(ok ? Status::Ready : Status::Failed, release.latestVersion, ok ? 1.f : 0.f);
        s.running = false;
        PostMessageW(notify, WM_UPDATE_READY, 0, 0);
    }).detach();
}

Progress Current() {
    State& s = Shared();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.progress;
}

bool Install() {
    State& s = Shared();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.installed) return true;
        if (s.progress.status != Status::Ready) return false;
    }
    if (!CanInstall()) return false;

    const fs::path from = StagedFiles(), to = ExeDir();
    std::vector<std::pair<fs::path, fs::path>> setAside;   // (file, where the old one went)
    std::vector<fs::path>                      added;      // files that didn't exist before
    std::error_code ec;
    bool ok = true;
    for (fs::recursive_directory_iterator it(from, ec), end; ok && !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path dest = to / fs::relative(it->path(), from, ec);
        fs::create_directories(dest.parent_path(), ec);
        if (fs::exists(dest, ec)) {
            fs::path aside = dest;
            aside += L".old";
            DeleteFileW(aside.c_str());
            if (!MoveFileExW(dest.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING)) { ok = false; break; }
            setAside.emplace_back(dest, aside);
        } else {
            added.push_back(dest);
        }
        if (!CopyFileW(it->path().c_str(), dest.c_str(), FALSE)) ok = false;
    }
    if (ec) ok = false;

    if (!ok) {   // put everything back as it was
        for (const fs::path& f : added) DeleteFileW(f.c_str());
        for (auto r = setAside.rbegin(); r != setAside.rend(); ++r) {
            DeleteFileW(r->first.c_str());
            MoveFileExW(r->second.c_str(), r->first.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        return false;
    }

    fs::remove_all(StagingDir(), ec);
    std::lock_guard<std::mutex> lock(s.mutex);
    s.installed = true;
    return true;
}

bool Installed() {
    State& s = Shared();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.installed;
}

bool Restart() {
    const fs::path exe = ExePath();
    std::wstring cmd = L"\"" + exe.wstring() + L"\" --updated";
    STARTUPINFOW        si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        exe.parent_path().c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void CleanUp() {
    std::error_code ec;
    // What the last update set aside: in the app's folder and licenses/ only.
    for (const fs::path& dir : { ExeDir(), ExeDir() / L"licenses" }) {
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == L".old") fs::remove(it->path(), ec);
        ec.clear();
    }
    // A staged update: keep it, ready, if it's still newer; otherwise drop it.
    const std::string staged = StagedVersion();
    if (!staged.empty() && UpdateCheck::IsNewer(staged, UpdateCheck::CurrentVersion()))
        Set(Status::Ready, staged, 1.f);
    else
        fs::remove_all(StagingDir(), ec);
}

} // namespace Updater
