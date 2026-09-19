







#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "wintrust.lib")

namespace fs = std::filesystem;





static const wchar_t* kApoClsidText = L"{5F2EC245-A357-4858-80A2-5E575C904D6F}";
static const wchar_t* kDefaultProcessingMode = L"{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}";
static const wchar_t* kCaptureSubKey =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture";
static const wchar_t* kEndpointEffectValue = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7";
static const wchar_t* kEfxModesValue = L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},7";
static const wchar_t* kInprocServer32SubKey =
    L"SOFTWARE\\Classes\\CLSID\\{5F2EC245-A357-4858-80A2-5E575C904D6F}\\InprocServer32";
static const wchar_t* kCertSubject = L"CN=InjectAudio Dev Test";
static const wchar_t* kCertCommonName = L"InjectAudio Dev Test";
static const wchar_t* kKeyContainer = L"InjectAudioDevTestKey";





struct SetupError
{
    std::wstring message;
    explicit SetupError(std::wstring text) : message(std::move(text)) {}
};

[[noreturn]] static void Throw(const std::wstring& message)
{
    throw SetupError(message);
}

static HANDLE g_stdout = INVALID_HANDLE_VALUE;
static HANDLE g_stderr = INVALID_HANDLE_VALUE;

static void WriteLine(HANDLE handle, const std::wstring& text)
{
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr)
    {
        return;
    }

    std::wstring line = text + L"\r\n";
    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode))
    {
        DWORD written = 0;
        WriteConsoleW(handle, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
        return;
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return;
    }
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
        utf8.data(), size, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

static void Log(const std::wstring& text) { WriteLine(g_stdout, text); }
static void LogError(const std::wstring& text) { WriteLine(g_stderr, L"[ERROR] " + text); }

static std::wstring FormatWin32Error(DWORD error)
{
    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    std::wstring message = (length != 0 && buffer != nullptr) ? std::wstring(buffer, length) : L"";
    if (buffer != nullptr)
    {
        LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' '))
    {
        message.pop_back();
    }
    wchar_t code[32] = {};
    swprintf_s(code, L"0x%08X", error);
    return message.empty() ? std::wstring(code) : (std::wstring(code) + L": " + message);
}





static LONG RegSetRawValue(
    HKEY root,
    const std::wstring& subKey,
    const std::wstring& valueName,
    DWORD type,
    const BYTE* data,
    DWORD size)
{
    HKEY key = nullptr;
    DWORD disposition = 0;
    LONG status = RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, 0,
        KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, &disposition);
    if (status != ERROR_SUCCESS)
    {
        return status;
    }
    status = RegSetValueExW(key, valueName.c_str(), 0, type, data, size);
    RegCloseKey(key);
    return status;
}

static LONG RegSetString(HKEY root, const std::wstring& subKey, const std::wstring& valueName,
    const std::wstring& value)
{
    const DWORD size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetRawValue(root, subKey, valueName, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()), size);
}

static LONG RegSetMultiString(HKEY root, const std::wstring& subKey, const std::wstring& valueName,
    const std::vector<std::wstring>& values)
{
    std::wstring data;
    for (const std::wstring& value : values)
    {
        data += value;
        data.push_back(L'\0');
    }
    data.push_back(L'\0');
    const DWORD size = static_cast<DWORD>(data.size() * sizeof(wchar_t));
    return RegSetRawValue(root, subKey, valueName, REG_MULTI_SZ,
        reinterpret_cast<const BYTE*>(data.data()), size);
}

static bool RegQueryString(HKEY root, const std::wstring& subKey, const std::wstring& valueName,
    std::wstring& value)
{
    HKEY key = nullptr;
    LONG status = RegOpenKeyExW(root, subKey.c_str(), 0, KEY_QUERY_VALUE, &key);
    if (status != ERROR_SUCCESS)
    {
        return false;
    }

    DWORD type = 0;
    DWORD size = 0;
    status = RegQueryValueExW(key, valueName.c_str(), nullptr, &type, nullptr, &size);
    if (status != ERROR_SUCCESS && status != ERROR_MORE_DATA)
    {
        RegCloseKey(key);
        return false;
    }

    std::vector<BYTE> buffer(static_cast<size_t>(size) + sizeof(wchar_t), 0);
    status = RegQueryValueExW(key, valueName.c_str(), nullptr, &type, buffer.data(), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ)
    {
        return false;
    }

    value.assign(reinterpret_cast<const wchar_t*>(buffer.data()));
    return true;
}

static bool IsGuidName(const std::wstring& name)
{
    if (name.size() != 38 || name.front() != L'{' || name.back() != L'}')
    {
        return false;
    }
    for (size_t i = 1; i + 1 < name.size(); ++i)
    {
        const wchar_t character = name[i];
        if (character == L'-')
        {
            continue;
        }
        if (!iswxdigit(character))
        {
            return false;
        }
    }
    return true;
}

static std::vector<std::wstring> EnumCaptureEndpoints()
{
    std::vector<std::wstring> endpoints;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kCaptureSubKey, 0, KEY_ENUMERATE_SUB_KEYS, &key) != ERROR_SUCCESS)
    {
        return endpoints;
    }

    wchar_t name[256] = {};
    for (DWORD index = 0;; ++index)
    {
        DWORD length = _countof(name);
        if (RegEnumKeyExW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
        {
            break;
        }
        if (IsGuidName(name))
        {
            endpoints.emplace_back(name);
        }
    }
    RegCloseKey(key);
    return endpoints;
}





static bool RunProcess(const std::wstring& executable, const std::wstring& arguments, DWORD& exitCode)
{
    std::wstring commandLine = L"\"" + executable + L"\" " + arguments;
    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo = {};

    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
            &startupInfo, &processInfo))
    {
        return false;
    }

    WaitForSingleObject(processInfo.hProcess, INFINITE);
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
}

static std::wstring System32Path(const wchar_t* fileName)
{
    wchar_t directory[MAX_PATH] = {};
    GetSystemDirectoryW(directory, MAX_PATH);
    return std::wstring(directory) + L"\\" + fileName;
}

static bool IsProcessElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        return false;
    }
    TOKEN_ELEVATION elevation = {};
    DWORD size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

static void StopDependentServices(SC_HANDLE manager, SC_HANDLE service)
{
    DWORD needed = 0;
    DWORD count = 0;
    if (!EnumDependentServicesW(service, SERVICE_ACTIVE, nullptr, 0, &needed, &count) || needed == 0)
    {
        return;
    }

    std::vector<BYTE> buffer(needed);
    if (!EnumDependentServicesW(service, SERVICE_ACTIVE,
            reinterpret_cast<LPENUM_SERVICE_STATUSW>(buffer.data()), needed, &needed, &count))
    {
        return;
    }

    auto* entries = reinterpret_cast<LPENUM_SERVICE_STATUSW>(buffer.data());
    for (DWORD index = 0; index < count; ++index)
    {
        SC_HANDLE dependent = OpenServiceW(manager, entries[index].lpServiceName,
            SERVICE_STOP | SERVICE_QUERY_STATUS);
        if (dependent != nullptr)
        {
            SERVICE_STATUS status = {};
            ControlService(dependent, SERVICE_CONTROL_STOP, &status);
            CloseServiceHandle(dependent);
        }
    }
}

static bool StopServiceAndWait(const wchar_t* serviceName, DWORD timeoutMs)
{
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr)
    {
        return false;
    }
    SC_HANDLE service = OpenServiceW(manager, serviceName, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (service == nullptr)
    {
        CloseServiceHandle(manager);
        return false;
    }

    StopDependentServices(manager, service);

    bool stopped = true;
    SERVICE_STATUS status = {};
    if (ControlService(service, SERVICE_CONTROL_STOP, &status))
    {
        const DWORD start = GetTickCount();
        for (;;)
        {
            if (!QueryServiceStatus(service, &status) || status.dwCurrentState == SERVICE_STOPPED)
            {
                break;
            }
            if (GetTickCount() - start > timeoutMs)
            {
                stopped = false;
                break;
            }
            Sleep(200);
        }
    }
    else if (GetLastError() != ERROR_SERVICE_NOT_ACTIVE)
    {
        stopped = false;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return stopped;
}

static void StartServiceByName(const wchar_t* serviceName)
{
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr)
    {
        return;
    }
    SC_HANDLE service = OpenServiceW(manager, serviceName, SERVICE_START);
    if (service != nullptr)
    {
        if (!StartServiceW(service, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
        {
            LogError(std::wstring(L"StartService(") + serviceName + L") failed: " + FormatWin32Error(GetLastError()));
        }
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
}

static void KillProcessByName(const wchar_t* processName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return;
    }

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
    {
        if (_wcsicmp(entry.szExeFile, processName) == 0)
        {
            HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
            if (process != nullptr)
            {
                TerminateProcess(process, 1);
                CloseHandle(process);
            }
        }
    }
    CloseHandle(snapshot);
}





static std::wstring FindSigntool()
{
    wchar_t programFilesX86[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", programFilesX86, MAX_PATH) == 0)
    {
        return L"";
    }

    const fs::path base = fs::path(programFilesX86) / L"Windows Kits" / L"10" / L"bin";
    std::vector<std::wstring> candidates;
    std::error_code error;
    for (auto it = fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied, error);
         it != fs::recursive_directory_iterator(); it.increment(error))
    {
        if (error)
        {
            error.clear();
            continue;
        }
        const fs::path& path = it->path();
        if (!it->is_regular_file(error) || error)
        {
            error.clear();
            continue;
        }
        if (_wcsicmp(path.filename().c_str(), L"signtool.exe") != 0)
        {
            continue;
        }
        if (_wcsicmp(path.parent_path().filename().c_str(), L"x64") == 0)
        {
            candidates.push_back(path.wstring());
        }
    }

    if (candidates.empty())
    {
        return L"";
    }
    std::sort(candidates.begin(), candidates.end(), std::greater<std::wstring>());
    return candidates.front();
}





static PCCERT_CONTEXT FindDevCertificate()
{
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
        CERT_SYSTEM_STORE_LOCAL_MACHINE, L"My");
    if (store == nullptr)
    {
        return nullptr;
    }

    PCCERT_CONTEXT certificate = CertFindCertificateInStore(store,
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR_W, kCertCommonName, nullptr);
    CertCloseStore(store, 0);
    return certificate;
}

static PCCERT_CONTEXT CreateDevCertificate()
{
    HCRYPTPROV provider = 0;
    if (!CryptAcquireContextW(&provider, kKeyContainer, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
            CRYPT_MACHINE_KEYSET))
    {
        if (!CryptAcquireContextW(&provider, kKeyContainer, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
                CRYPT_MACHINE_KEYSET | CRYPT_NEWKEYSET))
        {
            Throw(L"CryptAcquireContext failed: " + FormatWin32Error(GetLastError()));
        }
    }

    HCRYPTKEY key = 0;
    if (!CryptGenKey(provider, AT_SIGNATURE, (2048u << 16) | CRYPT_EXPORTABLE, &key))
    {
        const DWORD error = GetLastError();
        CryptReleaseContext(provider, 0);
        Throw(L"CryptGenKey failed: " + FormatWin32Error(error));
    }
    CryptDestroyKey(key);

    DWORD nameSize = 0;
    if (!CertStrToNameW(X509_ASN_ENCODING, kCertSubject, CERT_X500_NAME_STR, nullptr, nullptr,
            &nameSize, nullptr))
    {
        const DWORD error = GetLastError();
        CryptReleaseContext(provider, 0);
        Throw(L"CertStrToName failed: " + FormatWin32Error(error));
    }
    std::vector<BYTE> nameBuffer(nameSize, 0);
    CERT_NAME_BLOB subject = {};
    subject.cbData = nameSize;
    subject.pbData = nameBuffer.data();
    if (!CertStrToNameW(X509_ASN_ENCODING, kCertSubject, CERT_X500_NAME_STR, nullptr, subject.pbData,
            &subject.cbData, nullptr))
    {
        const DWORD error = GetLastError();
        CryptReleaseContext(provider, 0);
        Throw(L"CertStrToName failed: " + FormatWin32Error(error));
    }

    
    BYTE usageBits = CERT_DIGITAL_SIGNATURE_KEY_USAGE;
    CRYPT_BIT_BLOB usage = {};
    usage.cbData = 1;
    usage.pbData = &usageBits;
    usage.cUnusedBits = 7;
    DWORD usageSize = 0;
    if (!CryptEncodeObject(X509_ASN_ENCODING, X509_KEY_USAGE, &usage, nullptr, &usageSize))
    {
        const DWORD error = GetLastError();
        CryptReleaseContext(provider, 0);
        Throw(L"CryptEncodeObject(X509_KEY_USAGE) failed: " + FormatWin32Error(error));
    }
    std::vector<BYTE> usageEncoded(usageSize, 0);
    CryptEncodeObject(X509_ASN_ENCODING, X509_KEY_USAGE, &usage, usageEncoded.data(), &usageSize);

    
    LPSTR codeSigningOid = const_cast<LPSTR>(szOID_PKIX_KP_CODE_SIGNING);
    CERT_ENHKEY_USAGE enhancedUsage = {};
    enhancedUsage.cUsageIdentifier = 1;
    enhancedUsage.rgpszUsageIdentifier = &codeSigningOid;
    DWORD enhancedUsageSize = 0;
    if (!CryptEncodeObject(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, &enhancedUsage, nullptr,
            &enhancedUsageSize))
    {
        const DWORD error = GetLastError();
        CryptReleaseContext(provider, 0);
        Throw(L"CryptEncodeObject(X509_ENHANCED_KEY_USAGE) failed: " + FormatWin32Error(error));
    }
    std::vector<BYTE> enhancedUsageEncoded(enhancedUsageSize, 0);
    CryptEncodeObject(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, &enhancedUsage,
        enhancedUsageEncoded.data(), &enhancedUsageSize);

    CERT_EXTENSION extensions[2] = {};
    extensions[0].pszObjId = const_cast<LPSTR>(szOID_KEY_USAGE);
    extensions[0].fCritical = FALSE;
    extensions[0].Value.cbData = usageSize;
    extensions[0].Value.pbData = usageEncoded.data();
    extensions[1].pszObjId = const_cast<LPSTR>(szOID_ENHANCED_KEY_USAGE);
    extensions[1].fCritical = FALSE;
    extensions[1].Value.cbData = enhancedUsageSize;
    extensions[1].Value.pbData = enhancedUsageEncoded.data();
    CERT_EXTENSIONS extensionList = {};
    extensionList.cExtension = 2;
    extensionList.rgExtension = extensions;

    CRYPT_KEY_PROV_INFO providerInfo = {};
    providerInfo.pwszContainerName = const_cast<LPWSTR>(kKeyContainer);
    providerInfo.pwszProvName = const_cast<LPWSTR>(MS_ENH_RSA_AES_PROV_W);
    providerInfo.dwProvType = PROV_RSA_AES;
    providerInfo.dwFlags = CRYPT_MACHINE_KEYSET;
    providerInfo.dwKeySpec = AT_SIGNATURE;

    CRYPT_ALGORITHM_IDENTIFIER signatureAlgorithm = {};
    signatureAlgorithm.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

    SYSTEMTIME startTime = {};
    GetSystemTime(&startTime);
    SYSTEMTIME endTime = startTime;
    endTime.wYear += 5;

    PCCERT_CONTEXT certificate = CertCreateSelfSignCertificate(provider, &subject, 0, &providerInfo,
        &signatureAlgorithm, &startTime, &endTime, &extensionList);
    const DWORD createError = GetLastError();
    CryptReleaseContext(provider, 0);

    if (certificate == nullptr)
    {
        Throw(L"CertCreateSelfSignCertificate failed: " + FormatWin32Error(createError));
    }
    return certificate;
}

static bool StoreContainsCertificate(HCERTSTORE store, PCCERT_CONTEXT certificate)
{
    BYTE hash[20] = {};
    DWORD hashSize = sizeof(hash);
    if (!CertGetCertificateContextProperty(certificate, CERT_SHA1_HASH_PROP_ID, hash, &hashSize))
    {
        return false;
    }

    CRYPT_HASH_BLOB hashBlob = {};
    hashBlob.cbData = hashSize;
    hashBlob.pbData = hash;
    PCCERT_CONTEXT existing = CertFindCertificateInStore(store,
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_HASH, &hashBlob, nullptr);
    if (existing != nullptr)
    {
        CertFreeCertificateContext(existing);
        return true;
    }
    return false;
}

static void AddCertificateToStore(PCCERT_CONTEXT certificate, const wchar_t* storeName)
{
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
        CERT_SYSTEM_STORE_LOCAL_MACHINE, storeName);
    if (store == nullptr)
    {
        Throw(std::wstring(L"CertOpenStore(LocalMachine\\") + storeName + L") failed: " +
            FormatWin32Error(GetLastError()));
    }

    if (StoreContainsCertificate(store, certificate))
    {
        Log(std::wstring(L"Cert already in LocalMachine\\") + storeName);
    }
    else
    {
        if (!CertAddCertificateContextToStore(store, certificate, CERT_STORE_ADD_REPLACE_EXISTING, nullptr))
        {
            const DWORD error = GetLastError();
            CertCloseStore(store, 0);
            Throw(std::wstring(L"CertAddCertificateContextToStore(") + storeName + L") failed: " +
                FormatWin32Error(error));
        }
        Log(std::wstring(L"Added cert to LocalMachine\\") + storeName);
    }
    CertCloseStore(store, 0);
}

static PCCERT_CONTEXT EnsureDevCertificate()
{
    PCCERT_CONTEXT certificate = FindDevCertificate();
    if (certificate != nullptr)
    {
        Log(L"Using existing code signing certificate");
        return certificate;
    }

    Log(L"Creating self-signed code signing certificate...");
    certificate = CreateDevCertificate();
    AddCertificateToStore(certificate, L"My");
    return certificate;
}





static bool SignWithSigntool(const std::wstring& signtool, const std::wstring& dllPath)
{
    const std::wstring baseArguments =
        L"sign /fd SHA256 /sm /s My /n \"" + std::wstring(kCertCommonName) + L"\" ";

    DWORD exitCode = 0;
    std::wstring arguments = baseArguments +
        L"/tr http://timestamp.digicert.com /td SHA256 \"" + dllPath + L"\"";
    if (!RunProcess(signtool, arguments, exitCode))
    {
        Throw(L"failed to launch signtool");
    }
    if (exitCode == 0)
    {
        return true;
    }

    Log(L"Timestamp failed, signing without timestamp...");
    arguments = baseArguments + L"\"" + dllPath + L"\"";
    if (!RunProcess(signtool, arguments, exitCode))
    {
        Throw(L"failed to launch signtool");
    }
    return exitCode == 0;
}

static std::wstring DescribeTrustStatus(LONG status)
{
    switch (status)
    {
    case ERROR_SUCCESS:
        return L"Valid";
    case TRUST_E_NOSIGNATURE:
        return L"NotSigned";
    case TRUST_E_EXPLICIT_DISTRUST:
        return L"Distrusted";
    case TRUST_E_SUBJECT_NOT_TRUSTED:
        return L"NotTrusted";
    case TRUST_E_PROVIDER_UNKNOWN:
        return L"ProviderUnknown";
    case CRYPT_E_SECURITY_SETTINGS:
        return L"SecuritySettings";
    default:
    {
        wchar_t buffer[32] = {};
        swprintf_s(buffer, L"0x%08X", static_cast<unsigned>(status));
        return buffer;
    }
    }
}

static void PrintSignatureStatus(const std::wstring& dllPath)
{
    WINTRUST_FILE_INFO fileInfo = {};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = dllPath.c_str();

    WINTRUST_DATA trustData = {};
    trustData.cbStruct = sizeof(trustData);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;

    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &policy, &trustData);
    trustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &trustData);

    Log(L"Authenticode status: " + DescribeTrustStatus(status));
}





static void SetEndpointOriginalApo(const std::wstring& endpointId, const std::wstring& clsid)
{
    const std::wstring path = std::wstring(kCaptureSubKey) + L"\\" + endpointId + L"\\InjectAudio";
    const LONG status = RegSetString(HKEY_LOCAL_MACHINE, path, L"OriginalApoClsid", clsid);
    if (status != ERROR_SUCCESS)
    {
        Throw(L"set " + path + L" OriginalApoClsid failed: " + FormatWin32Error(status));
    }
}

static void BindCaptureEndpoints(const std::wstring& forcedOriginalApo)
{
    const std::vector<std::wstring> endpoints = EnumCaptureEndpoints();
    if (endpoints.empty())
    {
        Log(L"  (no capture endpoints found)");
        return;
    }

    int failures = 0;
    for (const std::wstring& endpointId : endpoints)
    {
        const std::wstring fxPath = std::wstring(kCaptureSubKey) + L"\\" + endpointId + L"\\FxProperties";
        try
        {
            if (!forcedOriginalApo.empty())
            {
                SetEndpointOriginalApo(endpointId, forcedOriginalApo);
                Log(L"  OriginalApoClsid = " + forcedOriginalApo);
            }
            else
            {
                std::wstring existing;
                if (RegQueryString(HKEY_LOCAL_MACHINE, fxPath, kEndpointEffectValue, existing) &&
                    !existing.empty() && _wcsicmp(existing.c_str(), kApoClsidText) != 0)
                {
                    SetEndpointOriginalApo(endpointId, existing);
                    Log(L"  OriginalApoClsid <- " + existing);
                }
            }

            LONG status = RegSetString(HKEY_LOCAL_MACHINE, fxPath, kEndpointEffectValue, kApoClsidText);
            if (status != ERROR_SUCCESS)
            {
                Throw(L"set " + fxPath + L" " + kEndpointEffectValue + L" failed: " +
                    FormatWin32Error(status));
            }

            status = RegSetMultiString(HKEY_LOCAL_MACHINE, fxPath, kEfxModesValue,
                { kDefaultProcessingMode });
            if (status != ERROR_SUCCESS)
            {
                Throw(L"set " + fxPath + L" " + kEfxModesValue + L" failed: " +
                    FormatWin32Error(status));
            }

            Log(L"  [BOUND] " + endpointId);
        }
        catch (const SetupError& error)
        {
            ++failures;
            Log(L"  [FAILED] " + endpointId + L" - " + error.message);
        }
    }

    if (failures > 0)
    {
        Log(std::to_wstring(failures) + L" endpoint(s) could not be updated.");
    }
}





struct Options
{
    std::wstring dllPath;
    bool skipRegister = false;
    bool skipBind = false;
    std::wstring originalApoClsid;
    bool help = false;
};

static void PrintUsage()
{
    Log(L"Usage: InjectAudioApoSetup.exe [options]");
    Log(L"  --dll <path>                 DLL to sign/register (default: <exe dir>\\InjectAudioApo.dll)");
    Log(L"  --skip-register              Skip regsvr32 + InprocServer32 + audio stack restart");
    Log(L"  --skip-bind                  Skip binding the APO on capture endpoints");
    Log(L"  --original-apo-clsid <guid>  Force OriginalApoClsid for every endpoint");
    Log(L"  --help                       Show this help");
}

static Options ParseCommandLine(int argc, wchar_t** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring argument = argv[index];
        if (_wcsicmp(argument.c_str(), L"--dll") == 0 && index + 1 < argc)
        {
            options.dllPath = argv[++index];
        }
        else if (_wcsicmp(argument.c_str(), L"--skip-register") == 0)
        {
            options.skipRegister = true;
        }
        else if (_wcsicmp(argument.c_str(), L"--skip-bind") == 0)
        {
            options.skipBind = true;
        }
        else if (_wcsicmp(argument.c_str(), L"--original-apo-clsid") == 0 && index + 1 < argc)
        {
            options.originalApoClsid = argv[++index];
        }
        else if (_wcsicmp(argument.c_str(), L"--help") == 0 || _wcsicmp(argument.c_str(), L"-h") == 0)
        {
            options.help = true;
        }
        else
        {
            Throw(L"unknown argument: " + argument);
        }
    }
    return options;
}

static std::wstring GetExecutableDirectory()
{
    wchar_t buffer[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return fs::path(buffer).parent_path().wstring();
}

static bool IsFileLocked(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return GetLastError() == ERROR_SHARING_VIOLATION;
    }
    CloseHandle(file);
    return false;
}

static std::wstring TimestampSuffix()
{
    SYSTEMTIME time = {};
    GetLocalTime(&time);
    wchar_t buffer[16] = {};
    swprintf_s(buffer, L"%02u%02u%02u", time.wHour, time.wMinute, time.wSecond);
    return buffer;
}





int wmain(int argc, wchar_t** argv)
{
    g_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    g_stderr = GetStdHandle(STD_ERROR_HANDLE);

    try
    {
        const Options options = ParseCommandLine(argc, argv);
        if (options.help)
        {
            PrintUsage();
            return 0;
        }

        if (!IsProcessElevated())
        {
            LogError(L"Administrator privileges are required.");
            return 1;
        }

        const std::wstring dllPath = options.dllPath.empty()
            ? GetExecutableDirectory() + L"\\InjectAudioApo.dll"
            : options.dllPath;

        if (!fs::exists(dllPath))
        {
            Throw(L"DLL not found: " + dllPath);
        }

        const std::wstring signtool = FindSigntool();
        if (signtool.empty())
        {
            Throw(L"signtool.exe not found (Windows SDK)");
        }

        Log(L"DLL: " + dllPath);
        Log(L"signtool: " + signtool);

        PCCERT_CONTEXT certificate = EnsureDevCertificate();
        AddCertificateToStore(certificate, L"Root");
        AddCertificateToStore(certificate, L"TrustedPublisher");
        CertFreeCertificateContext(certificate);

        if (IsFileLocked(dllPath))
        {
            const std::wstring backup = dllPath + L".locked_" + TimestampSuffix();
            Log(L"DLL locked - renaming to " + backup);
            MoveFileExW(dllPath.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING);
            Throw(L"Rebuild InjectAudioApo.dll then re-run this tool (file was locked by audiodg).");
        }

        Log(L"Signing...");
        if (!SignWithSigntool(signtool, dllPath))
        {
            Throw(L"signtool failed");
        }
        PrintSignatureStatus(dllPath);

        if (!options.skipRegister)
        {
            Log(L"regsvr32...");
            DWORD exitCode = 0;
            if (!RunProcess(System32Path(L"regsvr32.exe"), L"/s \"" + dllPath + L"\"", exitCode))
            {
                Throw(L"failed to launch regsvr32");
            }
            if (exitCode != 0)
            {
                Throw(L"regsvr32 failed: " + std::to_wstring(exitCode));
            }

            const LONG status = RegSetString(HKEY_LOCAL_MACHINE, kInprocServer32SubKey, L"", dllPath);
            if (status != ERROR_SUCCESS)
            {
                Throw(L"set InprocServer32 failed: " + FormatWin32Error(status));
            }
            Log(L"InprocServer32 -> " + dllPath);
        }

        if (!options.skipBind)
        {
            Log(L"Binding APO on every capture endpoint...");
            BindCaptureEndpoints(options.originalApoClsid);
        }

        if (!options.skipRegister)
        {
            Log(L"Restart audio stack...");
            StopServiceAndWait(L"Audiosrv", 15000);
            StopServiceAndWait(L"AudioEndpointBuilder", 15000);
            Sleep(1000);
            KillProcessByName(L"audiodg.exe");
            Sleep(1000);
            StartServiceByName(L"AudioEndpointBuilder");
            StartServiceByName(L"Audiosrv");
        }

        Log(L"");
        Log(L"Done. Keep SysFx ON.");
        return 0;
    }
    catch (const SetupError& error)
    {
        LogError(error.message);
        return 1;
    }
    catch (const std::exception& error)
    {
        LogError(L"unexpected error: " + std::wstring(error.what(), error.what() + strlen(error.what())));
        return 1;
    }
}
