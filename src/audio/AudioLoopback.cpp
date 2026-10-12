#include "AudioLoopback.h"

#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

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
// Helpers
// ---------------------------------------------------------------------------
static std::wstring DeviceId(IMMDevice* dev) {
    std::wstring out;
    LPWSTR id = nullptr;
    if (dev && SUCCEEDED(dev->GetId(&id))) {
        out = id;
        CoTaskMemFree(id);
    }
    return out;
}

// Copies a WAVEFORMATEX (including any WAVEFORMATEXTENSIBLE tail) into owned storage.
static std::vector<BYTE> CopyFormat(const WAVEFORMATEX* wf) {
    const BYTE* p = reinterpret_cast<const BYTE*>(wf);
    return std::vector<BYTE>(p, p + sizeof(WAVEFORMATEX) + wf->cbSize);
}

static std::vector<BYTE> MixFormat(IAudioClient* client) {
    WAVEFORMATEX* wf = nullptr;
    if (FAILED(client->GetMixFormat(&wf)) || !wf) return {};
    std::vector<BYTE> out = CopyFormat(wf);
    CoTaskMemFree(wf);
    return out;
}

static const WAVEFORMATEX* AsFormat(const std::vector<BYTE>& v) {
    return reinterpret_cast<const WAVEFORMATEX*>(v.data());
}

// ---------------------------------------------------------------------------
// InitStandardLoopback — standard WASAPI loopback tap on a render endpoint.
// Captures everything played on that endpoint, including our own playback,
// so it must only be used when we play back on a DIFFERENT endpoint.
// ---------------------------------------------------------------------------
static bool InitStandardLoopback(IMMDevice* device, ComPtr<IAudioClient>& out,
                                 std::vector<BYTE>& fmtOut) {
    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(client.GetAddressOf())))) {
        Log("  Activate(IAudioClient) for loopback failed");
        return false;
    }

    std::vector<BYTE> fmt = MixFormat(client.Get());
    if (fmt.empty()) {
        Log("  GetMixFormat failed on capture endpoint");
        return false;
    }

    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK, 200000, 0, AsFormat(fmt), nullptr);
    if (FAILED(hr)) {
        Log("  Standard loopback Initialize failed hr=0x%08X", (unsigned)hr);
        return false;
    }

    out    = client;
    fmtOut = std::move(fmt);
    return true;
}

// ---------------------------------------------------------------------------
// InitProcessExclusionLoopback — activates and initialises a process-loopback
// client that captures all system audio EXCEPT this process tree's output.
//
// A process-loopback client cannot report a mix format (GetMixFormat returns
// E_NOTIMPL), so we pass one explicitly: the playback endpoint's mix format
// first, then 16-bit stereo PCM (the format Microsoft's ApplicationLoopback
// sample uses).  AUTOCONVERTPCM lets the engine convert to whichever we ask
// for.  The client must be event-driven, so captureEvent receives the handle.
// ---------------------------------------------------------------------------
static bool InitProcessExclusionLoopback(ComPtr<IAudioClient>& out,
                                         std::vector<BYTE>& fmtOut,
                                         const std::vector<BYTE>& preferredFmt,
                                         HANDLE stopEvent, HANDLE captureEvent) {
    WAVEFORMATEX pcm16 = {};
    pcm16.wFormatTag      = WAVE_FORMAT_PCM;
    pcm16.nChannels       = 2;
    pcm16.nSamplesPerSec  = 48000;
    pcm16.wBitsPerSample  = 16;
    pcm16.nBlockAlign     = pcm16.nChannels * pcm16.wBitsPerSample / 8;
    pcm16.nAvgBytesPerSec = pcm16.nSamplesPerSec * pcm16.nBlockAlign;

    std::vector<std::vector<BYTE>> candidates;
    if (!preferredFmt.empty()) candidates.push_back(preferredFmt);
    candidates.push_back(CopyFormat(&pcm16));

    const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK
                      | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                      | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                      | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    for (const auto& fmt : candidates) {
        // An activated client can only be initialised once, so activate a
        // fresh one per attempt.
        ComPtr<IAudioClient> client;
        if (!TryProcessExclusionCapture(client, stopEvent)) return false;

        const WAVEFORMATEX* wf = AsFormat(fmt);
        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                        200000, 0, wf, nullptr);
        Log("  Initialize(%u ch %u Hz %u bits) hr=0x%08X",
            wf->nChannels, wf->nSamplesPerSec, wf->wBitsPerSample, (unsigned)hr);
        if (FAILED(hr)) continue;

        if (FAILED(hr = client->SetEventHandle(captureEvent))) {
            Log("  SetEventHandle failed hr=0x%08X", (unsigned)hr);
            continue;
        }
        out    = client;
        fmtOut = fmt;
        return true;
    }
    return false;
}

void AudioLoopback::Run() {
    LogOpen();
    Log("AudioLoopback::Run() — thread started, PID=%u", GetCurrentProcessId());

    HANDLE captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    RunPump(captureEvent);
    if (captureEvent) CloseHandle(captureEvent);

    Log("AudioLoopback::Run() — thread exiting");
    LogClose();
}

void AudioLoopback::RunPump(HANDLE captureEvent) {
    HRESULT hr;

    // -----------------------------------------------------------------------
    // Resolve the playback endpoint (Windows default — unmuted so Discord's
    // per-app capture picks this session up when the stream window is shared)
    // and the capture endpoint (the user's selection, or the default).
    // -----------------------------------------------------------------------
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        Log("CoCreateInstance(MMDeviceEnumerator) failed");
        return;
    }

    ComPtr<IMMDevice> renderDevice;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &renderDevice))) {
        Log("Failed to get default render device");
        return;
    }

    ComPtr<IMMDevice> captureDevice;
    if (!m_captureDeviceId.empty()) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, m_captureDeviceId.c_str(), -1, nullptr, 0);
        std::wstring wid(wlen > 0 ? wlen - 1 : 0, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, m_captureDeviceId.c_str(), -1, wid.data(), wlen);
        if (FAILED(enumerator->GetDevice(wid.c_str(), &captureDevice))) {
            Log("GetDevice('%s') failed — using the default device", m_captureDeviceId.c_str());
            captureDevice = nullptr;
        }
    }
    if (!captureDevice) captureDevice = renderDevice;

    // -----------------------------------------------------------------------
    // Pick the capture strategy.  A standard loopback tap on the endpoint we
    // play back on would capture our own output and feed it back forever, so:
    //   different endpoint → standard loopback on the selected device
    //   same endpoint      → process-exclusion loopback (skips our output)
    // If process exclusion is unavailable we stop rather than create feedback.
    // -----------------------------------------------------------------------
    std::vector<BYTE> fmt;
    bool eventDriven = false;

    if (DeviceId(captureDevice.Get()) != DeviceId(renderDevice.Get())) {
        Log("Capture endpoint differs from playback — using standard loopback");
        if (!InitStandardLoopback(captureDevice.Get(), m_captureAudioClient, fmt)) {
            Log("FATAL: standard loopback failed — exiting");
            return;
        }
    } else {
        Log("Capture endpoint is the playback endpoint — using process-exclusion loopback");
        std::vector<BYTE> renderMix;
        {
            ComPtr<IAudioClient> probe;
            if (SUCCEEDED(renderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                 reinterpret_cast<void**>(probe.GetAddressOf()))))
                renderMix = MixFormat(probe.Get());
        }
        if (!captureEvent ||
            !InitProcessExclusionLoopback(m_captureAudioClient, fmt, renderMix,
                                          m_stopEvent, captureEvent)) {
            Log("FATAL: process-exclusion loopback unavailable — not falling back to "
                "standard loopback on the playback device, which would feed back");
            return;
        }
        eventDriven = true;
    }

    const WAVEFORMATEX* pwfx = AsFormat(fmt);
    m_frameSize = pwfx->nBlockAlign;
    Log("Format: %u ch, %u Hz, %u bits, frameSize=%u, method=%s",
        pwfx->nChannels, pwfx->nSamplesPerSec, pwfx->wBitsPerSample, m_frameSize,
        eventDriven ? "process-exclusion" : "standard-loopback");

    if (FAILED(hr = m_captureAudioClient->GetService(IID_PPV_ARGS(&m_captureClient)))) {
        Log("GetService(IAudioCaptureClient) failed hr=0x%08X", (unsigned)hr);
        return;
    }
    Log("Capture client ready");

    // -----------------------------------------------------------------------
    // Playback client.  AUTOCONVERTPCM lets it accept the capture format even
    // when the two endpoints run at different sample rates / channel counts.
    // -----------------------------------------------------------------------
    if (FAILED(hr = renderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(m_renderAudioClient.GetAddressOf())))) {
        Log("Activate render failed hr=0x%08X", (unsigned)hr);
        return;
    }

    if (FAILED(hr = m_renderAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            200000, 0, pwfx, nullptr))) {
        Log("Render Initialize failed hr=0x%08X", (unsigned)hr);
        return;
    }

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
    // Main pump loop.  Event-driven clients wake us when a packet is ready;
    // the 10 ms timeout doubles as the polling interval for standard loopback.
    // -----------------------------------------------------------------------
    UINT64 totalCaptured = 0, totalRendered = 0;
    DWORD  lastReport    = GetTickCount();

    HANDLE waitHandles[] = { m_stopEvent, captureEvent };
    const DWORD waitCount = eventDriven ? 2 : 1;

    for (;;) {
        if (WaitForMultipleObjects(waitCount, waitHandles, FALSE, 10) == WAIT_OBJECT_0)
            break; // stop requested

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
