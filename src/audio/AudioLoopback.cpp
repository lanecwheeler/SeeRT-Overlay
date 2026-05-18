#include "AudioLoopback.h"

#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cstring>
#include <cstdio>
#include <string>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "Mmdevapi.lib")

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Simple file logger — writes to %TEMP%\SeeRTOverlay_Audio.log
// ---------------------------------------------------------------------------
static FILE* s_log = nullptr;

static void LogOpen() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%sSeeRTOverlay_Audio.log", tmp);
    _wfopen_s(&s_log, path, L"w");
}

static void Log(const char* fmt, ...) {
    if (!s_log) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(s_log, fmt, args);
    fputc('\n', s_log);
    fflush(s_log);
    va_end(args);
    // Mirror to OutputDebugString
    char buf[512];
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

static void LogClose() { if (s_log) { fclose(s_log); s_log = nullptr; } }

// ---------------------------------------------------------------------------
// ActivationHandler — synchronises ActivateAudioInterfaceAsync completion.
// Must implement IAgileObject so ActivateAudioInterfaceAsync can invoke the
// callback from a thread-pool thread without marshaling; without it the API
// returns E_ILLEGAL_METHOD_CALL immediately in a WinRT-initialized process.
// ---------------------------------------------------------------------------
// {94EA2B94-E9CC-49E0-C0FF-EE64CA8F5B90}
static const GUID kIID_IAgileObject =
    { 0x94EA2B94, 0xE9CC, 0x49E0, { 0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90 } };

struct ActivationHandler : public IActivateAudioInterfaceCompletionHandler {
    HANDLE               event  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT              result = E_PENDING;
    ComPtr<IAudioClient> client;
    LONG                 refs   = 1;

    ~ActivationHandler() { if (event) CloseHandle(event); }

    ULONG STDMETHODCALLTYPE AddRef()  override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refs);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** ppv) override {
        if (id == IID_IUnknown ||
            id == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
            id == kIID_IAgileObject) {   // must be agile for WinRT process context
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT hr;
        ComPtr<IUnknown> unk;
        op->GetActivateResult(&hr, &unk);
        if (SUCCEEDED(hr)) unk.As(&client);
        result = hr;
        SetEvent(event);
        return S_OK;
    }
};

// ---------------------------------------------------------------------------

bool AudioLoopback::Start(std::string deviceId) {
    if (m_thread) return true;
    m_captureDeviceId = std::move(deviceId);
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_thread    = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (!m_thread) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
        return false;
    }
    return true;
}

void AudioLoopback::Stop() {
    if (m_stopEvent) SetEvent(m_stopEvent);
    if (m_thread) {
        WaitForSingleObject(m_thread, 3000);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
    if (m_stopEvent) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
    if (m_captureAudioClient) m_captureAudioClient->Stop();
    if (m_renderAudioClient)  m_renderAudioClient->Stop();
    m_captureClient.Reset();
    m_captureAudioClient.Reset();
    m_renderClient.Reset();
    m_renderAudioClient.Reset();
    m_renderBufferFrames = 0;
    m_frameSize          = 0;
    LogClose();
}

DWORD WINAPI AudioLoopback::ThreadProc(LPVOID param) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    static_cast<AudioLoopback*>(param)->Run();
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------------------
// TryProcessExclusionCapture — activates a process-exclusion loopback capture
// client that captures all system audio EXCEPT this process's own render output,
// breaking any feedback loop.  Returns true on success.
// ---------------------------------------------------------------------------
static bool TryProcessExclusionCapture(ComPtr<IAudioClient>& out, HANDLE stopEvent) {
    AUDIOCLIENT_ACTIVATION_PARAMS params = {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId    = GetCurrentProcessId();
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT pv = {};
    pv.vt             = VT_BLOB;
    pv.blob.cbSize    = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto* handler = new ActivationHandler();
    ComPtr<IActivateAudioInterfaceAsyncOperation> asyncOp;

    HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &pv,
        handler,
        asyncOp.GetAddressOf());

    if (FAILED(hr)) {
        Log("  ActivateAudioInterfaceAsync returned hr=0x%08X", (unsigned)hr);
        handler->Release();
        return false;
    }

    Log("  ActivateAudioInterfaceAsync queued, waiting for completion...");
    HANDLE waitHandles[] = { handler->event, stopEvent };
    DWORD  waitResult    = WaitForMultipleObjects(2, waitHandles, FALSE, 10000);

    if (waitResult == WAIT_TIMEOUT) {
        Log("  Completion timed out after 10 s");
        handler->Release();
        return false;
    }
    if (waitResult != WAIT_OBJECT_0) {
        Log("  Stopped while waiting for activation");
        handler->Release();
        return false;
    }
    if (FAILED(handler->result)) {
        Log("  Completion handler reported hr=0x%08X", (unsigned)handler->result);
        handler->Release();
        return false;
    }

    Log("  Process-exclusion activation succeeded");
    out = handler->client;
    handler->Release();
    return true;
}

// ---------------------------------------------------------------------------
// TryStandardLoopbackCapture — falls back to a standard WASAPI loopback tap
// on the default render endpoint.  All desktop audio is captured (including
// our own render output), but this is the approach that is known to work even
// when ActivateAudioInterfaceAsync / process-exclusion loopback is unavailable.
// ---------------------------------------------------------------------------
static bool TryStandardLoopbackCapture(ComPtr<IAudioClient>& out,
                                        WAVEFORMATEX** pwfxOut,
                                        const std::string& deviceId) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        Log("  CoCreateInstance(MMDeviceEnumerator) failed");
        return false;
    }

    ComPtr<IMMDevice> device;
    if (!deviceId.empty()) {
        // Convert UTF-8 device ID to wide string and activate that endpoint.
        int wlen = MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, nullptr, 0);
        std::wstring wid(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, wid.data(), wlen);
        if (FAILED(enumerator->GetDevice(wid.c_str(), &device))) {
            Log("  GetDevice('%s') failed — falling back to default", deviceId.c_str());
            device = nullptr;
        }
    }
    if (!device) {
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
            Log("  GetDefaultAudioEndpoint failed");
            return false;
        }
    }

    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(client.GetAddressOf())))) {
        Log("  Activate(IAudioClient) for loopback failed");
        return false;
    }

    client->GetMixFormat(pwfxOut);

    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK, 200000, 0, *pwfxOut, nullptr);
    if (FAILED(hr)) {
        Log("  Standard loopback Initialize failed hr=0x%08X", (unsigned)hr);
        CoTaskMemFree(*pwfxOut); *pwfxOut = nullptr;
        return false;
    }

    out = client;
    return true;
}

void AudioLoopback::Run() {
    LogOpen();
    Log("AudioLoopback::Run() — thread started, PID=%u", GetCurrentProcessId());

    HRESULT hr;
    WAVEFORMATEX* pwfx = nullptr;

    // -----------------------------------------------------------------------
    // Try process-exclusion loopback first (no feedback loop, preferred).
    // Fall back to standard loopback if unavailable.
    // -----------------------------------------------------------------------
    bool usedProcessExclusion = false;
    Log("Attempting process-exclusion loopback...");
    if (TryProcessExclusionCapture(m_captureAudioClient, m_stopEvent)) {
        HRESULT fmtHr = m_captureAudioClient->GetMixFormat(&pwfx);
        Log("  GetMixFormat hr=0x%08X pwfx=%p", (unsigned)fmtHr, (void*)pwfx);
        if (SUCCEEDED(fmtHr) && pwfx) {
            Log("  Format: %u ch %u Hz %u bits",
                pwfx->nChannels, pwfx->nSamplesPerSec, pwfx->wBitsPerSample);
        }
        // Try flags=0 / duration=0 first, then with LOOPBACK flag, then with 1s buffer.
        const struct { DWORD flags; REFERENCE_TIME dur; } kTries[] = {
            { 0,                            0         },
            { AUDCLNT_STREAMFLAGS_LOOPBACK, 0         },
            { AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000  },
            { 0,                            10000000  },
        };
        for (auto& t : kTries) {
            hr = m_captureAudioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED, t.flags, t.dur, 0, pwfx, nullptr);
            Log("  Initialize(flags=0x%X dur=%lld) hr=0x%08X",
                t.flags, (long long)t.dur, (unsigned)hr);
            if (SUCCEEDED(hr)) { usedProcessExclusion = true; break; }
        }
        if (!usedProcessExclusion) {
            Log("  All Initialize attempts failed — falling back");
            m_captureAudioClient.Reset();
            CoTaskMemFree(pwfx); pwfx = nullptr;
        } else {
            Log("  Capture client initialized (process-exclusion)");
        }
    }

    if (!m_captureAudioClient) {
        Log("Falling back to standard WASAPI loopback (device='%s')...", m_captureDeviceId.c_str());
        if (!TryStandardLoopbackCapture(m_captureAudioClient, &pwfx, m_captureDeviceId)) {
            Log("FATAL: both loopback strategies failed — exiting");
            LogClose();
            return;
        }
        Log("  Capture client initialized (standard loopback)");
    }

    m_frameSize = pwfx->nBlockAlign;
    Log("Format: %u ch, %u Hz, %u bits, frameSize=%u, method=%s",
        pwfx->nChannels, pwfx->nSamplesPerSec, pwfx->wBitsPerSample, m_frameSize,
        usedProcessExclusion ? "process-exclusion" : "standard-loopback");

    if (FAILED(hr = m_captureAudioClient->GetService(IID_PPV_ARGS(&m_captureClient)))) {
        Log("GetService(IAudioCaptureClient) failed hr=0x%08X", (unsigned)hr);
        CoTaskMemFree(pwfx);
        return;
    }
    Log("Capture client ready");

    // -----------------------------------------------------------------------
    // Render to the default output device — unmuted so Discord's per-app
    // process-loopback capture picks up this session when the stream window
    // is shared.
    // -----------------------------------------------------------------------
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> renderDevice;

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))
        || FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &renderDevice))) {
        Log("Failed to get default render device");
        CoTaskMemFree(pwfx);
        return;
    }

    if (FAILED(hr = renderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(m_renderAudioClient.GetAddressOf())))) {
        Log("Activate render failed hr=0x%08X", (unsigned)hr);
        CoTaskMemFree(pwfx);
        return;
    }

    if (FAILED(hr = m_renderAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED, 0, 200000, 0, pwfx, nullptr))) {
        Log("Render Initialize failed hr=0x%08X", (unsigned)hr);
        CoTaskMemFree(pwfx);
        return;
    }
    CoTaskMemFree(pwfx);

    m_renderAudioClient->GetBufferSize(&m_renderBufferFrames);
    Log("Render buffer = %u frames", m_renderBufferFrames);

    if (FAILED(hr = m_renderAudioClient->GetService(IID_PPV_ARGS(&m_renderClient)))) {
        Log("GetService(IAudioRenderClient) failed hr=0x%08X", (unsigned)hr);
        return;
    }

    m_captureAudioClient->Start();
    m_renderAudioClient->Start();
    Log("Both clients started — audio loopback running");

    // -----------------------------------------------------------------------
    // Main pump loop.
    // -----------------------------------------------------------------------
    UINT64 totalCaptured = 0, totalRendered = 0;
    DWORD  lastReport    = GetTickCount();

    for (;;) {
        if (WaitForSingleObject(m_stopEvent, 10) != WAIT_TIMEOUT)
            break;

        DWORD now = GetTickCount();
        if (now - lastReport >= 5000) {
            Log("captured=%llu rendered=%llu frames", totalCaptured, totalRendered);
            lastReport = now;
        }

        UINT32 packetFrames = 0;
        if (FAILED(m_captureClient->GetNextPacketSize(&packetFrames))) {
            Log("GetNextPacketSize failed — exiting loop");
            break;
        }

        while (packetFrames > 0) {
            BYTE*  src       = nullptr;
            UINT32 numFrames = 0;
            DWORD  flags     = 0;

            if (FAILED(m_captureClient->GetBuffer(&src, &numFrames, &flags, nullptr, nullptr)))
                return;

            totalCaptured += numFrames;

            UINT32 padding   = 0;
            m_renderAudioClient->GetCurrentPadding(&padding);
            UINT32 available = m_renderBufferFrames - padding;

            if (numFrames <= available) {
                BYTE* dst = nullptr;
                if (SUCCEEDED(m_renderClient->GetBuffer(numFrames, &dst))) {
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                        memset(dst, 0, static_cast<size_t>(numFrames) * m_frameSize);
                    else
                        memcpy(dst, src, static_cast<size_t>(numFrames) * m_frameSize);
                    m_renderClient->ReleaseBuffer(numFrames, 0);
                    totalRendered += numFrames;
                }
            }

            m_captureClient->ReleaseBuffer(numFrames);

            if (FAILED(m_captureClient->GetNextPacketSize(&packetFrames)))
                return;
        }
    }

    Log("Pump loop exited");
}
