#include "ApoBindingGuard.h"

#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace
{
    
    constexpr wchar_t kPropertiesSubKey[] = L"Properties";
    constexpr wchar_t kFxPropertiesSubKey[] = L"FxProperties";

    constexpr wchar_t kEfxValueName[] =
        L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7";
    constexpr wchar_t kEfxModesValueName[] =
        L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},7";

    
    constexpr wchar_t kPackGuid[] =
        L"{c876062a-a276-4ed6-b15b-3962e69007f8}";
    const std::wstring kPackValue2 = std::wstring(kPackGuid) + L",2";

    
    constexpr DWORD kServicePollIntervalMs = 100;
    constexpr DWORD kRestartPauseMs = 500;
    constexpr DWORD kReopenDelayMs = 500;
    constexpr DWORD kThreadShutdownTimeoutMs = 3000;
    constexpr size_t kLogBufferChars = 512;

    const PROPERTYKEY kEfxKey =
    { { 0xd04e05a6, 0x594b, 0x4fb6, { 0xa8, 0x0d, 0x01, 0xaf, 0x5e, 0xed, 0x7d, 0x1d } }, 7 };

    std::vector<BYTE> StringBytes(const std::wstring& text)
    {
        std::vector<BYTE> data((text.size() + 1) * sizeof(wchar_t), 0);
        memcpy(data.data(), text.c_str(), text.size() * sizeof(wchar_t));
        return data;
    }

    std::vector<BYTE> MultiStringBytes(const std::wstring& text)
    {
        std::vector<BYTE> data((text.size() + 2) * sizeof(wchar_t), 0);
        memcpy(data.data(), text.c_str(), text.size() * sizeof(wchar_t));
        return data;
    }

    bool ReadRegistryValue(HKEY root, const std::wstring& subKey, const std::wstring& name,
        DWORD& type, std::vector<BYTE>& data, bool& present)
    {
        present = false;
        type = 0;
        data.clear();

        HKEY key = nullptr;
        if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ | KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        {
            return false;
        }

        DWORD valueType = 0;
        DWORD size = 0;
        LONG result = RegQueryValueExW(key, name.c_str(), nullptr, &valueType, nullptr, &size);
        if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND)
        {
            RegCloseKey(key);
            return true;
        }
        if (result != ERROR_SUCCESS)
        {
            RegCloseKey(key);
            return false;
        }

        data.resize(size);
        result = RegQueryValueExW(key, name.c_str(), nullptr, &valueType, data.data(), &size);
        RegCloseKey(key);
        if (result != ERROR_SUCCESS)
        {
            return false;
        }

        type = valueType;
        present = true;
        return true;
    }

    bool WriteRegistryValue(HKEY root, const std::wstring& subKey, const std::wstring& name,
        DWORD type, const std::vector<BYTE>& data)
    {
        HKEY key = nullptr;
        if (RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, 0,
                KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        {
            return false;
        }
        LONG result = RegSetValueExW(key, name.c_str(), 0, type,
            data.empty() ? nullptr : data.data(), (DWORD)data.size());
        RegCloseKey(key);
        return result == ERROR_SUCCESS;
    }

    
    bool DeletePackValues(const std::wstring& propertiesKey)
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, propertiesKey.c_str(), 0,
                KEY_SET_VALUE | KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        {
            return false;
        }

        bool deleted = false;
        wchar_t name[256] = {};
        DWORD index = 0;
        while (true)
        {
            DWORD size = _countof(name);
            const LONG result = RegEnumValueW(key, index, name, &size, nullptr, nullptr, nullptr, nullptr);
            if (result != ERROR_SUCCESS)
            {
                break;
            }
            if (_wcsnicmp(name, kPackGuid, wcslen(kPackGuid)) == 0)
            {
                deleted = RegDeleteValueW(key, name) == ERROR_SUCCESS || deleted;
            }
            else
            {
                ++index;
            }
        }
        RegCloseKey(key);
        return deleted;
    }

    bool ControlServiceByName(const wchar_t* name, bool start, DWORD timeoutMs)
    {
        bool ok = false;
        SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (manager == nullptr)
        {
            return false;
        }

        SC_HANDLE service = OpenServiceW(manager, name,
            SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
        if (service != nullptr)
        {
            SERVICE_STATUS status = {};
            if (start)
            {
                StartServiceW(service, 0, nullptr);
            }
            else
            {
                ControlService(service, SERVICE_CONTROL_STOP, &status);
            }

            const DWORD targetState = start ? SERVICE_RUNNING : SERVICE_STOPPED;
            const DWORD iterations = (timeoutMs + kServicePollIntervalMs - 1) / kServicePollIntervalMs;
            for (DWORD i = 0; i < iterations; ++i)
            {
                if (!QueryServiceStatus(service, &status) || status.dwCurrentState == targetState)
                {
                    ok = true;
                    break;
                }
                Sleep(kServicePollIntervalMs);
            }
            CloseServiceHandle(service);
        }
        CloseServiceHandle(manager);
        return ok;
    }

    std::wstring EndpointKeyPath(const std::wstring& base, const std::wstring& endpointId,
        const wchar_t* leaf)
    {
        std::wstring path = base + L"\\" + endpointId;
        if (leaf != nullptr && leaf[0] != L'\0')
        {
            path += L"\\";
            path += leaf;
        }
        return path;
    }

    std::wstring GetDeviceFriendlyName(IMMDevice* device)
    {
        std::wstring name;
        IPropertyStore* store = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store != nullptr)
        {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) &&
                value.vt == VT_LPWSTR && value.pwszVal != nullptr)
            {
                name = value.pwszVal;
            }
            PropVariantClear(&value);
            store->Release();
        }
        return name;
    }

    void EnumActiveCaptureEndpoints(std::vector<std::pair<std::wstring, std::wstring>>& endpoints)
    {
        endpoints.clear();
        IMMDeviceEnumerator* enumerator = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        {
            return;
        }
        IMMDeviceCollection* collection = nullptr;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection)) && collection != nullptr)
        {
            UINT count = 0;
            collection->GetCount(&count);
            for (UINT index = 0; index < count; ++index)
            {
                IMMDevice* device = nullptr;
                if (SUCCEEDED(collection->Item(index, &device)) && device != nullptr)
                {
                    LPWSTR id = nullptr;
                    if (SUCCEEDED(device->GetId(&id)) && id != nullptr)
                    {
                        endpoints.emplace_back(id, GetDeviceFriendlyName(device));
                        CoTaskMemFree(id);
                    }
                    device->Release();
                }
            }
            collection->Release();
        }
        enumerator->Release();
    }
}

ApoBindingGuard& ApoBindingGuard::Instance()
{
    static ApoBindingGuard instance;
    return instance;
}

ApoBindingGuard::~ApoBindingGuard()
{
    Stop();
}

void ApoBindingGuard::Log(const wchar_t* format, ...)
{
    wchar_t message[kLogBufferChars] = {};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
    va_end(args);

    wprintf(L"[guard] %s\n", message);
    fflush(stdout);
}

void ApoBindingGuard::RestoreEndpoint(EndpointGuard* guard)
{
    const std::wstring endpointKey = EndpointKeyPath(m_options.captureKeyBase, guard->endpointId, nullptr);
    const std::wstring fxKey = EndpointKeyPath(m_options.captureKeyBase, guard->endpointId, kFxPropertiesSubKey);

    
    DWORD type = 0;
    std::vector<BYTE> data;
    bool present = false;
    const std::vector<BYTE> efxBytes = StringBytes(m_options.apoClsid);
    if (ReadRegistryValue(HKEY_LOCAL_MACHINE, fxKey, kEfxValueName, type, data, present) &&
        (!present || type != REG_SZ || data != efxBytes))
    {
        Log(L"[ALERT] APO binding was modified (expected %s)", m_options.apoClsid.c_str());
    }

    
    const std::vector<BYTE> modeBytes = MultiStringBytes(m_options.processingMode);
    if (ReadRegistryValue(HKEY_LOCAL_MACHINE, fxKey, kEfxModesValueName, type, data, present) &&
        (!present || type != REG_MULTI_SZ || data != modeBytes))
    {
        if (WriteRegistryValue(HKEY_LOCAL_MACHINE, fxKey, kEfxModesValueName, REG_MULTI_SZ, modeBytes))
        {
            Log(L"restored EFX processing mode");
        }
    }

    
    
    
    bool packChanged = false;
    {
        const std::wstring propertiesKey =
            EndpointKeyPath(m_options.captureKeyBase, guard->endpointId, kPropertiesSubKey);
        DWORD currentType = 0;
        std::vector<BYTE> currentData;
        bool currentPresent = false;
        if (ReadRegistryValue(HKEY_LOCAL_MACHINE, propertiesKey, kPackValue2, currentType,
                currentData, currentPresent) &&
            currentPresent)
        {
            packChanged = true;
            Log(L"effect pack selected (%s), resetting to device default",
                currentType == REG_SZ && currentData.size() >= sizeof(wchar_t)
                    ? reinterpret_cast<const wchar_t*>(currentData.data())
                    : L"(non-string)");
            DeletePackValues(propertiesKey);
        }
    }

    if (packChanged)
    {
        RestartAudioStack();
    }
}

void ApoBindingGuard::RestartAudioStack()
{
    static DWORD lastRestart = 0;
    const DWORD now = GetTickCount();
    if (lastRestart != 0 && now - lastRestart < m_options.restartDebounceMs)
    {
        Log(L"audio stack restart skipped (debounce)");
        return;
    }
    lastRestart = now;

    Log(L"restarting %s to apply revert...", m_options.audioService.c_str());
    ControlServiceByName(m_options.audioService.c_str(), false, m_options.serviceTimeoutMs);
    Sleep(kRestartPauseMs);
    ControlServiceByName(m_options.audioService.c_str(), true, m_options.serviceTimeoutMs);
    Log(L"%s restarted", m_options.audioService.c_str());
}

DWORD WINAPI ApoBindingGuard::MonitorProc(LPVOID)
{
    Instance().MonitorEndpoints();
    return 0;
}

void ApoBindingGuard::MonitorEndpoints()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::vector<std::pair<std::wstring, std::wstring>> endpoints;
    EnumActiveCaptureEndpoints(endpoints);
    m_knownEndpoints.clear();
    for (const auto& endpoint : endpoints)
    {
        m_knownEndpoints.push_back(endpoint.first);
    }
    Log(L"endpoint monitor started (%u active capture endpoints)", (unsigned)endpoints.size());

    while (WaitForSingleObject(m_monitorStopEvent, m_options.monitorIntervalMs) == WAIT_TIMEOUT)
    {
        EnumActiveCaptureEndpoints(endpoints);

        for (const auto& endpoint : endpoints)
        {
            bool known = false;
            for (const auto& id : m_knownEndpoints)
            {
                if (id == endpoint.first)
                {
                    known = true;
                    break;
                }
            }
            if (!known)
            {
                Log(L"[ALERT] new capture endpoint: %s (%s)",
                    endpoint.second.c_str(), endpoint.first.c_str());
                m_knownEndpoints.push_back(endpoint.first);
            }
        }

        for (auto it = m_knownEndpoints.begin(); it != m_knownEndpoints.end(); )
        {
            bool present = false;
            for (const auto& endpoint : endpoints)
            {
                if (endpoint.first == *it)
                {
                    present = true;
                    break;
                }
            }
            if (!present)
            {
                Log(L"[ALERT] capture endpoint removed: %s", it->c_str());
                it = m_knownEndpoints.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    CoUninitialize();
}

DWORD WINAPI ApoBindingGuard::ThreadProc(LPVOID param)
{
    auto* guard = static_cast<EndpointGuard*>(param);
    Instance().Run(guard);
    return 0;
}

void ApoBindingGuard::Run(EndpointGuard* guard)
{
    const std::wstring endpointKey = EndpointKeyPath(m_options.captureKeyBase, guard->endpointId, nullptr);
    HKEY key = nullptr;

    while (true)
    {
        if (key == nullptr)
        {
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, endpointKey.c_str(), 0, KEY_NOTIFY | KEY_READ, &key) != ERROR_SUCCESS)
            {
                if (WaitForSingleObject(guard->stopEvent, kReopenDelayMs) == WAIT_OBJECT_0)
                {
                    break;
                }
                continue;
            }
        }

        ResetEvent(guard->changeEvent);
        if (RegNotifyChangeKeyValue(key, TRUE,
                REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME,
                guard->changeEvent, TRUE) != ERROR_SUCCESS)
        {
            RegCloseKey(key);
            key = nullptr;
            if (WaitForSingleObject(guard->stopEvent, kReopenDelayMs) == WAIT_OBJECT_0)
            {
                break;
            }
            continue;
        }

        HANDLE events[2] = { guard->stopEvent, guard->changeEvent };
        const DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0)
        {
            break;
        }
        if (wait != WAIT_OBJECT_0 + 1)
        {
            break;
        }

        Sleep(m_options.changeDebounceMs);
        RestoreEndpoint(guard);

        
        RegCloseKey(key);
        key = nullptr;
        Sleep(kReopenDelayMs);
    }

    if (key != nullptr)
    {
        RegCloseKey(key);
    }
}

bool ApoBindingGuard::Start(const Options& options)
{
    if (m_running)
    {
        return true;
    }
    if (options.apoClsid.empty())
    {
        Log(L"Start failed: apoClsid is empty");
        return false;
    }

    m_options = options;

    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
    {
        Log(L"MMDeviceEnumerator failed");
        return false;
    }

    IMMDeviceCollection* collection = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection)) && collection != nullptr)
    {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT index = 0; index < count; ++index)
        {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(index, &device)) || device == nullptr)
            {
                continue;
            }

            bool bound = false;
            IPropertyStore* store = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store != nullptr)
            {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(store->GetValue(kEfxKey, &value)) &&
                    value.vt == VT_LPWSTR && value.pwszVal != nullptr &&
                    _wcsicmp(value.pwszVal, m_options.apoClsid.c_str()) == 0)
                {
                    bound = true;
                }
                PropVariantClear(&value);
                store->Release();
            }

            if (bound)
            {
                LPWSTR deviceId = nullptr;
                if (SUCCEEDED(device->GetId(&deviceId)) && deviceId != nullptr)
                {
                    const wchar_t* brace = wcsrchr(deviceId, L'{');
                    auto* guard = new EndpointGuard();
                    guard->endpointId = brace != nullptr ? brace : deviceId;
                    CoTaskMemFree(deviceId);

                    guard->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    guard->changeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    if (guard->stopEvent != nullptr && guard->changeEvent != nullptr)
                    {
                        RestoreEndpoint(guard);
                        guard->thread = CreateThread(nullptr, 0, ThreadProc, guard, 0, nullptr);
                        m_guards.push_back(guard);
                        Log(L"watching endpoint %s", guard->endpointId.c_str());
                    }
                    else
                    {
                        delete guard;
                    }
                }
            }
            device->Release();
        }
        collection->Release();
    }
    enumerator->Release();

    m_monitorStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_monitorStopEvent != nullptr)
    {
        m_monitorThread = CreateThread(nullptr, 0, MonitorProc, nullptr, 0, nullptr);
    }

    m_running = !m_guards.empty();
    if (!m_running)
    {
        Log(L"no bound capture endpoint found");
    }
    return m_running;
}

void ApoBindingGuard::Stop()
{
    if (m_monitorStopEvent != nullptr)
    {
        SetEvent(m_monitorStopEvent);
    }
    if (m_monitorThread != nullptr)
    {
        WaitForSingleObject(m_monitorThread, kThreadShutdownTimeoutMs);
        CloseHandle(m_monitorThread);
        m_monitorThread = nullptr;
    }
    if (m_monitorStopEvent != nullptr)
    {
        CloseHandle(m_monitorStopEvent);
        m_monitorStopEvent = nullptr;
    }

    if (m_guards.empty())
    {
        m_running = false;
        return;
    }

    for (EndpointGuard* guard : m_guards)
    {
        if (guard->stopEvent != nullptr)
        {
            SetEvent(guard->stopEvent);
        }
    }
    for (EndpointGuard* guard : m_guards)
    {
        if (guard->thread != nullptr)
        {
            WaitForSingleObject(guard->thread, kThreadShutdownTimeoutMs);
            CloseHandle(guard->thread);
        }
        if (guard->changeEvent != nullptr) CloseHandle(guard->changeEvent);
        if (guard->stopEvent != nullptr) CloseHandle(guard->stopEvent);
        delete guard;
    }

    m_guards.clear();
    m_running = false;
}
