#pragma once
#include <Windows.h>
#include <wrl/client.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <string>

// Loopback-captures desktop audio and plays it back through a standard WASAPI
// render session on the default device, so Discord's per-application audio
// capture picks it up when the user shares the stream window.
//
// To avoid capturing our own playback (a feedback loop), a selected endpoint
// other than the playback device is tapped with standard loopback, while the
// playback device itself is captured with process-exclusion loopback, which
// skips this process's output.  All WASAPI init happens on the background
// thread after CoInitialize, sidestepping WinRT apartment conflicts.
class AudioLoopback {
public:
    AudioLoopback() = default;
    ~AudioLoopback() { Stop(); }

    AudioLoopback(const AudioLoopback&)            = delete;
    AudioLoopback& operator=(const AudioLoopback&) = delete;

    // deviceId — Windows endpoint ID string to capture from.
    // Pass empty string to use the default render device (eConsole).
    bool Start(std::string deviceId = {});
    void Stop();

    bool IsRunning() const { return m_thread != nullptr; }

private:
    static DWORD WINAPI ThreadProc(LPVOID param);
    void Run();
    void RunPump(HANDLE captureEvent);

    Microsoft::WRL::ComPtr<IAudioClient>        m_captureAudioClient;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> m_captureClient;
    Microsoft::WRL::ComPtr<IAudioClient>        m_renderAudioClient;
    Microsoft::WRL::ComPtr<IAudioRenderClient>  m_renderClient;

    std::string m_captureDeviceId;
    UINT32      m_renderBufferFrames = 0;
    UINT32      m_frameSize          = 0;
    HANDLE      m_stopEvent          = nullptr;
    HANDLE      m_thread             = nullptr;
};
