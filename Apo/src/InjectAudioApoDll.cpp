
#include <atlbase.h>
#include <atlcom.h>
#include <atlcoll.h>
#include <atlsync.h>
#include <mmreg.h>
#include <string>

#include "resource.h"
#include "InjectAudioApoDll.h"
#include "InjectAudioApo.h"

#include "InjectAudioApoDll_i.c"

#include <audioenginebaseapo.h>

APO_REG_PROPERTIES const *gCoreAPOs[] =
{
    &CInjectAudioEFX::sm_RegProperties.m_Properties
};

class CInjectAudioDllModule : public CAtlDllModuleT< CInjectAudioDllModule >
{
public :
    DECLARE_LIBID(LIBID_StandardApoDlllib)
    DECLARE_REGISTRY_APPID_RESOURCEID(IDR_InjectAudioDLL, "{87DBA1B1-9014-4499-A3F6-A24C622DEAFF}")

};

CInjectAudioDllModule _AtlModule;
inline HINSTANCE g_hModule = nullptr;


extern "C" BOOL WINAPI DllMain(HINSTANCE hInstance , DWORD dwReason, LPVOID lpReserved)
{
    if (DLL_PROCESS_ATTACH == dwReason)
    {
        g_hModule = hInstance;
        DisableThreadLibraryCalls(hInstance);
    }
    else if ((DLL_PROCESS_DETACH == dwReason) && (NULL == lpReserved))
    {
    }

    return _AtlModule.DllMain(dwReason, lpReserved);
}
inline std::wstring GuidToString(const GUID& g)
{
    wchar_t buf[64]{};
    StringCchPrintfW(buf, 64,
        L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}


STDAPI DllCanUnloadNow(void)
{
    return _AtlModule.DllCanUnloadNow();
}


STDAPI DllGetClassObject(_In_ REFCLSID rclsid, _In_ REFIID riid, _Outptr_ LPVOID FAR* ppv)
{
    return _AtlModule.DllGetClassObject(rclsid, riid, ppv);
}
HRESULT RegisterClsid(REFCLSID clsid, const wchar_t* friendlyName)
{
    wchar_t modulePath[MAX_PATH]{};
    if (!GetModuleFileNameW(g_hModule, modulePath, MAX_PATH))
        return HRESULT_FROM_WIN32(GetLastError());

    const std::wstring clsidStr = GuidToString(clsid);
    std::wstring keyPath = L"CLSID\\" + clsidStr;
    std::wstring inproc = keyPath + L"\\InprocServer32";
    std::wstring apoPath = std::wstring(L"AudioEngine\\AudioProcessingObjects\\") + clsidStr;

    HKEY key = nullptr;
    LONG err = RegCreateKeyExW(HKEY_CLASSES_ROOT, keyPath.c_str(), 0, nullptr, 0,
        KEY_WRITE, nullptr, &key, nullptr);
    if (err != ERROR_SUCCESS) return HRESULT_FROM_WIN32(err);
    RegSetValueExW(key, nullptr, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(friendlyName),
        (DWORD)((wcslen(friendlyName) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);

    err = RegCreateKeyExW(HKEY_CLASSES_ROOT, inproc.c_str(), 0, nullptr, 0,
        KEY_WRITE, nullptr, &key, nullptr);
    if (err != ERROR_SUCCESS) return HRESULT_FROM_WIN32(err);
    RegSetValueExW(key, nullptr, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(modulePath),
        (DWORD)((wcslen(modulePath) + 1) * sizeof(wchar_t)));
    const wchar_t* threading = L"Both";
    RegSetValueExW(key, L"ThreadingModel", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(threading),
        (DWORD)((wcslen(threading) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);

    err = RegCreateKeyExW(HKEY_CLASSES_ROOT, apoPath.c_str(), 0, nullptr, 0,
        KEY_WRITE, nullptr, &key, nullptr);
    if (err == ERROR_SUCCESS)
    {
        RegSetValueExW(key, L"FriendlyName", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(friendlyName),
            (DWORD)((wcslen(friendlyName) + 1) * sizeof(wchar_t)));
        const wchar_t* copyright = L"FUCK Copyright (c)";
        RegSetValueExW(key, L"Copyright", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(copyright),
            (DWORD)((wcslen(copyright) + 1) * sizeof(wchar_t)));
        DWORD major = 1, minor = 1;
        DWORD flags =
            APO_FLAG_SAMPLESPERFRAME_MUST_MATCH |
            APO_FLAG_BITSPERSAMPLE_MUST_MATCH |
            APO_FLAG_INPLACE;
        DWORD one = 1;
        RegSetValueExW(key, L"MajorVersion", 0, REG_DWORD, reinterpret_cast<BYTE*>(&major), sizeof(major));
        RegSetValueExW(key, L"MinorVersion", 0, REG_DWORD, reinterpret_cast<BYTE*>(&minor), sizeof(minor));
        RegSetValueExW(key, L"Flags", 0, REG_DWORD, reinterpret_cast<BYTE*>(&flags), sizeof(flags));
        RegSetValueExW(key, L"MinInputConnections", 0, REG_DWORD, reinterpret_cast<BYTE*>(&one), sizeof(one));
        RegSetValueExW(key, L"MaxInputConnections", 0, REG_DWORD, reinterpret_cast<BYTE*>(&one), sizeof(one));
        RegSetValueExW(key, L"MinOutputConnections", 0, REG_DWORD, reinterpret_cast<BYTE*>(&one), sizeof(one));
        RegSetValueExW(key, L"MaxOutputConnections", 0, REG_DWORD, reinterpret_cast<BYTE*>(&one), sizeof(one));
        DWORD maxInst = 0xFFFFFFFF;
        RegSetValueExW(key, L"MaxInstances", 0, REG_DWORD, reinterpret_cast<BYTE*>(&maxInst), sizeof(maxInst));
        DWORD numIf = 1;
        RegSetValueExW(key, L"NumAPOInterfaces", 0, REG_DWORD, reinterpret_cast<BYTE*>(&numIf), sizeof(numIf));
        const std::wstring iid = GuidToString(__uuidof(IAudioProcessingObject));
        RegSetValueExW(key, L"APOInterface0", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(iid.c_str()),
            (DWORD)((iid.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    else
    {
        return HRESULT_FROM_WIN32(err);
    }

    return S_OK;
}

HRESULT UnregisterClsid(REFCLSID clsid)
{
    const std::wstring clsidStr = GuidToString(clsid);
    std::wstring inproc = L"CLSID\\" + clsidStr + L"\\InprocServer32";
    std::wstring keyPath = L"CLSID\\" + clsidStr;
    std::wstring apoPath = std::wstring(L"AudioEngine\\AudioProcessingObjects\\") + clsidStr;

    RegDeleteKeyW(HKEY_CLASSES_ROOT, inproc.c_str());
    RegDeleteKeyW(HKEY_CLASSES_ROOT, keyPath.c_str());
    RegDeleteKeyW(HKEY_CLASSES_ROOT, apoPath.c_str());
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, apoPath.c_str());
    return S_OK;
}
HRESULT RegisterApoServer()
{
    auto hr = RegisterClsid(CLSID_InjectAudioEFX,L"InjectAudioEFX");

    return hr;
}

HRESULT UnregisterApoServer()
{
    UnregisterClsid(CLSID_InjectAudioEFX);
    return S_OK;
}

STDAPI DllRegisterServer()
{
    return RegisterApoServer();
}

STDAPI DllUnregisterServer()
{
    return UnregisterApoServer();
}
