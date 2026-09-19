#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <string>
#include <vector>









class ApoBindingGuard
{
public:
    struct Options
    {
        
        std::wstring apoClsid;

        
        std::wstring captureKeyBase =
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture";

        
        std::wstring processingMode = L"{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}";

        
        std::wstring audioService = L"Audiosrv";

        DWORD monitorIntervalMs = 2000;     
        DWORD changeDebounceMs = 200;       
        DWORD restartDebounceMs = 10000;    
        DWORD serviceTimeoutMs = 5000;      
    };

    static ApoBindingGuard& Instance();

    
    
    bool Start(const Options& options);
    void Stop();
    bool IsRunning() const { return m_running; }

private:
    struct EndpointGuard
    {
        std::wstring endpointId;    
        HANDLE stopEvent = nullptr;
        HANDLE changeEvent = nullptr;
        HANDLE thread = nullptr;
    };

    ApoBindingGuard() = default;
    ~ApoBindingGuard();
    ApoBindingGuard(const ApoBindingGuard&) = delete;
    ApoBindingGuard& operator=(const ApoBindingGuard&) = delete;

    static DWORD WINAPI ThreadProc(LPVOID param);
    static DWORD WINAPI MonitorProc(LPVOID param);
    void Run(EndpointGuard* guard);
    void MonitorEndpoints();
    void RestoreEndpoint(EndpointGuard* guard);
    void RestartAudioStack();
    void Log(const wchar_t* format, ...);

    bool m_running = false;
    Options m_options;
    std::vector<EndpointGuard*> m_guards;
    HANDLE m_monitorStopEvent = nullptr;
    HANDLE m_monitorThread = nullptr;
    std::vector<std::wstring> m_knownEndpoints;
};
