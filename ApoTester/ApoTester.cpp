






















#include <windows.h>
#include <conio.h>
#include <cmath>
#include <cwchar>
#include <iostream>
#include <string>
#include <vector>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioenginebaseapo.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include "PcmRingBuffer.h"
#include "ApoBindingGuard.h"
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Mmdevapi.lib")
#pragma comment(lib, "Propsys.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "winmm.lib")

namespace
{
    constexpr wchar_t ApoClsidText[] = L"{5F2EC245-A357-4858-80A2-5E575C904D6F}";
    const PROPERTYKEY EndpointEffectClsidKey =
    { { 0xD04E05A6, 0x594B, 0x4fb6, { 0xA8, 0x0D, 0x01, 0xAF, 0x5E, 0xED, 0x7D, 0x1D } }, 7 };

    void PrintWin32Error(const wchar_t* operation)
    {
        std::wcerr << operation << L" failed, error=0x"
            << std::hex << GetLastError() << std::dec << L"\n";
    }

    void PrintHresult(const wchar_t* operation, HRESULT hr)
    {
        std::wcerr << operation << L" failed, hr=0x"
            << std::hex << static_cast<unsigned long>(hr) << std::dec << L"\n";
    }

    bool AllowAudioServiceAccess(SECURITY_ATTRIBUTES& attributes, PSECURITY_DESCRIPTOR& descriptor)
    {
        descriptor = static_cast<PSECURITY_DESCRIPTOR>(LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH));
        if (descriptor == nullptr || !InitializeSecurityDescriptor(descriptor, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorDacl(descriptor, TRUE, nullptr, FALSE))
        {
            if (descriptor != nullptr) LocalFree(descriptor);
            descriptor = nullptr;
            return false;
        }

        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor;
        attributes.bInheritHandle = FALSE;
        return true;
    }

    std::wstring GetDeviceName(IMMDevice* device)
    {
        IPropertyStore* propertyStore = nullptr;
        std::wstring name;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &propertyStore)))
        {
            return name;
        }

        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(propertyStore->GetValue(PKEY_Device_FriendlyName, &value)))
        {
            LPWSTR text = nullptr;
            if (SUCCEEDED(PropVariantToStringAlloc(value, &text)) && text != nullptr)
            {
                name.assign(text);
                CoTaskMemFree(text);
            }
        }
        PropVariantClear(&value);
        propertyStore->Release();
        return name;
    }

    bool IsBoundToApo(IMMDevice* device)
    {
        IPropertyStore* propertyStore = nullptr;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &propertyStore)))
        {
            return false;
        }

        bool bound = false;
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(propertyStore->GetValue(EndpointEffectClsidKey, &value)) &&
            value.vt == VT_LPWSTR && value.pwszVal != nullptr &&
            _wcsicmp(value.pwszVal, ApoClsidText) == 0)
        {
            bound = true;
        }
        PropVariantClear(&value);
        propertyStore->Release();
        return bound;
    }

    HRESULT OpenBoundMicrophone(IMMDevice** deviceOut)
    {
        *deviceOut = nullptr;
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDeviceCollection* collection = nullptr;
        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator));
        if (FAILED(hr))
        {
            return hr;
        }

        hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
        if (SUCCEEDED(hr))
        {
            UINT count = 0;
            hr = collection->GetCount(&count);
            for (UINT index = 0; index < count && *deviceOut == nullptr; ++index)
            {
                IMMDevice* device = nullptr;
                if (SUCCEEDED(collection->Item(index, &device)))
                {
                    if (IsBoundToApo(device))
                    {
                        *deviceOut = device;
                    }
                    else
                    {
                        device->Release();
                    }
                }
            }
        }

        if (collection != nullptr) collection->Release();

        if (*deviceOut == nullptr && SUCCEEDED(hr))
        {
            hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, deviceOut);
        }
        enumerator->Release();
        return hr;
    }

    std::wstring GuidToString(const GUID& guid)
    {
        wchar_t text[64] = {};
        StringFromGUID2(guid, text, _countof(text));
        return text;
    }

    
    
    
    bool WriteEndpointOriginalApo(IMMDevice* device, const GUID& clsid)
    {
        LPWSTR deviceId = nullptr;
        if (FAILED(device->GetId(&deviceId)) || deviceId == nullptr)
        {
            return false;
        }
        const wchar_t* brace = wcsrchr(deviceId, L'{');
        const std::wstring endpointKey = brace != nullptr ? brace : deviceId;
        CoTaskMemFree(deviceId);

        std::wstring subKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\";
        subKey += endpointKey;
        subKey += L"\\InjectAudio";

        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, subKey.c_str(), 0, nullptr, 0,
            KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        {
            return false;
        }

        const std::wstring clsidText = GuidToString(clsid);
        const DWORD bytes = static_cast<DWORD>((clsidText.size() + 1) * sizeof(wchar_t));
        const LONG result = RegSetValueExW(key, L"OriginalApoClsid", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(clsidText.c_str()), bytes);
        RegCloseKey(key);
        return result == ERROR_SUCCESS;
    }

    
    
    
    
    
    float ComputeCaptureRms(
        const BYTE* data,
        UINT32 frames,
        const WAVEFORMATEX* format,
        DWORD bufferFlags)
    {
        if (data == nullptr || frames == 0 || format == nullptr)
        {
            return 0.0f;
        }
        if (bufferFlags & AUDCLNT_BUFFERFLAGS_SILENT)
        {
            return 0.0f;
        }

        const UINT32 channels = format->nChannels;
        if (channels == 0)
        {
            return 0.0f;
        }

        WORD bits = format->wBitsPerSample;
        WORD tag = format->wFormatTag;
        if (tag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22)
        {
            const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
            if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) tag = WAVE_FORMAT_IEEE_FLOAT;
            else if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) tag = WAVE_FORMAT_PCM;
        }

        const UINT32 totalSamples = frames * channels;
        double sum = 0.0;

        if (tag == WAVE_FORMAT_IEEE_FLOAT && bits == 32)
        {
            const FLOAT32* p = reinterpret_cast<const FLOAT32*>(data);
            for (UINT32 i = 0; i < totalSamples; ++i)
            {
                const double v = p[i];
                sum += v * v;
            }
        }
        else if (tag == WAVE_FORMAT_PCM && bits == 16)
        {
            const INT16* p = reinterpret_cast<const INT16*>(data);
            for (UINT32 i = 0; i < totalSamples; ++i)
            {
                const double v = static_cast<double>(p[i]) / 32768.0;
                sum += v * v;
            }
        }
        else if (tag == WAVE_FORMAT_PCM && bits == 32)
        {
            const INT32* p = reinterpret_cast<const INT32*>(data);
            for (UINT32 i = 0; i < totalSamples; ++i)
            {
                const double v = static_cast<double>(p[i]) / 2147483648.0;
                sum += v * v;
            }
        }
        else
        {
            
            return 0.0f;
        }

        return static_cast<float>(sqrt(sum / static_cast<double>(totalSamples)));
    }

    
    
    
    struct Mp3Data
    {
        std::vector<FLOAT32> samples;  
        UINT32 channels = 0;
        UINT32 sampleRate = 0;

        size_t FrameCount() const { return channels ? samples.size() / channels : 0; }
    };

    HRESULT LoadMp3(const wchar_t* path, UINT32 wantedChannels, UINT32 wantedSampleRate, Mp3Data& out)
    {
        IMFSourceReader* reader = nullptr;
        HRESULT hr = MFCreateSourceReaderFromURL(path, nullptr, &reader);
        if (FAILED(hr))
        {
            return hr;
        }

        IMFMediaType* outType = nullptr;
        hr = MFCreateMediaType(&outType);
        if (FAILED(hr))
        {
            reader->Release();
            return hr;
        }

        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, wantedChannels);
        outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, wantedSampleRate);
        outType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

        hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, outType);
        outType->Release();
        if (FAILED(hr))
        {
            PrintHresult(L"SetCurrentMediaType", hr);
            reader->Release();
            return hr;
        }

        IMFMediaType* actualType = nullptr;
        hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actualType);
        if (FAILED(hr))
        {
            reader->Release();
            return hr;
        }

        UINT32 gotChannels = 0, gotRate = 0;
        actualType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &gotChannels);
        actualType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &gotRate);
        actualType->Release();

        out.channels = gotChannels;
        out.sampleRate = gotRate;

        if (gotChannels == 0 || gotRate == 0)
        {
            reader->Release();
            return E_FAIL;
        }

        std::wcout << L"MP3 decoded as: " << gotChannels << L" ch, "
            << gotRate << L" Hz, float32\n";

        while (true)
        {
            DWORD streamIndex = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            IMFSample* sample = nullptr;

            hr = reader->ReadSample(
                MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                0,
                &streamIndex,
                &flags,
                &timestamp,
                &sample);
            if (FAILED(hr))
            {
                break;
            }

            if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            {
                if (sample) sample->Release();
                break;
            }

            if (sample == nullptr)
            {
                continue;
            }

            IMFMediaBuffer* mediaBuffer = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&mediaBuffer)))
            {
                BYTE* data = nullptr;
                DWORD maxLen = 0;
                DWORD curLen = 0;
                if (SUCCEEDED(mediaBuffer->Lock(&data, &maxLen, &curLen)) && curLen > 0)
                {
                    size_t oldCount = out.samples.size();
                    size_t addCount = curLen / sizeof(FLOAT32);
                    out.samples.resize(oldCount + addCount);
                    memcpy(out.samples.data() + oldCount, data, curLen);
                    mediaBuffer->Unlock();
                }
                mediaBuffer->Release();
            }
            sample->Release();
        }

        reader->Release();

        if (out.samples.empty())
        {
            return E_FAIL;
        }

        std::wcout << L"Loaded " << out.FrameCount() << L" frames ("
            << (static_cast<double>(out.FrameCount()) / gotRate) << L" s)\n";
        return S_OK;
    }
}

void PrintUsage()
{
    std::wcout << L"Usage: KWSApoAudioTest.exe [--file path] [--volume 0..1] [--channels 1..8]\n"
        << L"                        [--listen] [--replace] [--gain-db dB]\n"
        << L"                        [--log-level 0..4] [--original-apo \"{GUID}\"]\n"
        << L"                        [--duck] [--duck-threshold-db dB]\n"
        << L"                        [--duck-depth 0..1] [--duck-attack-ms ms]\n"
        << L"                        [--duck-release-ms ms]\n"
        << L"  --file              audio file to stream, default .\\test.mp3\n"
        << L"  --volume            base music volume 0..1 written into the ring, default 0.25\n"
        << L"  --channels          ring channels 1..8, default 2\n"
        << L"  --listen            open a capture stream on the bound mic (loads the APO)\n"
        << L"  --replace           set PCM_RING_FLAG_FILTER_BLOCK_VOICE (inject replaces mic)\n"
        << L"  --gain-db           mic gain in dB applied while mixing, default 0.0\n"
        << L"  --log-level         APO log verbosity 0=none..4=debug, default 3\n"
        << L"  --original-apo      CLSID of the OEM APO to wrap; stored per endpoint\n"
        << L"                      in ...\\Capture\\{endpoint}\\InjectAudio\\OriginalApoClsid\n"
        << L"  --duck              auto-duck music when the mic is loud (requires --listen)\n"
        << L"  --duck-threshold-db mic RMS level above which music is ducked, default -35\n"
        << L"  --duck-depth        music gain while ducked (0=mute, 1=no duck), default 0.15\n"
        << L"  --duck-attack-ms    time to duck when speech starts, default 15\n"
        << L"  --duck-release-ms   time to restore after speech stops, default 400\n";
}

int wmain(int argc, wchar_t** argv)
{
    std::wstring mp3Path = L".\\test.wav";
    float volume = 0.25f;
    float gainDb = 0.0f;
    UINT32 channels = 2;
    DWORD flags = 0;
    DWORD logLevel = 3;
    GUID originalApo = GUID_NULL;
    bool listen = false;
    bool parseError = false;

    
    bool duckEnabled = false;
    float duckThresholdDb = -35.0f;
    float duckDepth = 0.15f;
    float duckAttackMs = 15.0f;
    float duckReleaseMs = 400.0f;

    for (int i = 1; i < argc; ++i)
    {
        if (_wcsicmp(argv[i], L"--file") == 0 && i + 1 < argc) mp3Path = argv[++i];
        else if (_wcsicmp(argv[i], L"--volume") == 0 && i + 1 < argc) volume = static_cast<float>(_wtof(argv[++i]));
        else if (_wcsicmp(argv[i], L"--amplitude") == 0 && i + 1 < argc) volume = static_cast<float>(_wtof(argv[++i]));
        else if (_wcsicmp(argv[i], L"--gain-db") == 0 && i + 1 < argc) gainDb = static_cast<float>(_wtof(argv[++i]));
        else if (_wcsicmp(argv[i], L"--channels") == 0 && i + 1 < argc) channels = static_cast<UINT32>(_wtoi(argv[++i]));
        else if (_wcsicmp(argv[i], L"--log-level") == 0 && i + 1 < argc) logLevel = static_cast<DWORD>(_wtoi(argv[++i]));
        else if (_wcsicmp(argv[i], L"--replace") == 0) flags |= PCM_RING_FLAG_FILTER_BLOCK_VOICE;
        else if (_wcsicmp(argv[i], L"--listen") == 0) listen = true;
        else if (_wcsicmp(argv[i], L"--duck") == 0) duckEnabled = true;
        else if (_wcsicmp(argv[i], L"--duck-threshold-db") == 0 && i + 1 < argc) duckThresholdDb = static_cast<float>(_wtof(argv[++i]));
        else if (_wcsicmp(argv[i], L"--duck-depth") == 0 && i + 1 < argc) duckDepth = static_cast<float>(_wtof(argv[++i]));
        else if (_wcsicmp(argv[i], L"--duck-attack-ms") == 0 && i + 1 < argc) duckAttackMs = static_cast<float>(_wtof(argv[++i]));
        else if (_wcsicmp(argv[i], L"--duck-release-ms") == 0 && i + 1 < argc) duckReleaseMs = static_cast<float>(_wtof(argv[++i]));
        else if (_wcsicmp(argv[i], L"--original-apo") == 0 && i + 1 < argc)
        {
            if (FAILED(CLSIDFromString(argv[++i], &originalApo)))
            {
                std::wcerr << L"Invalid --original-apo CLSID\n";
                parseError = true;
            }
        }
        else
        {
            parseError = true;
        }
    }

    if (duckDepth < 0.0f) duckDepth = 0.0f;
    if (duckDepth > 1.0f) duckDepth = 1.0f;
    if (duckAttackMs < 1.0f) duckAttackMs = 1.0f;
    if (duckReleaseMs < 1.0f) duckReleaseMs = 1.0f;

    if (parseError ||
        volume < 0.0f || volume > 1.0f ||
        channels == 0 || channels > PCM_RING_BUFFER_MAX_CHANNELS ||
        logLevel > 4)
    {
        PrintUsage();
        return 1;
    }

    if (duckEnabled && !listen)
    {
        std::wcerr << L"--duck requires --listen (no capture stream, no mic data)\n";
        return 1;
    }
    if (duckEnabled && (flags & PCM_RING_FLAG_FILTER_BLOCK_VOICE))
    {
        std::wcerr << L"[WARN] --duck and --replace together: the APO replaces the mic\n"
            << L"       with music, so this capture stream sees only music.\n"
            << L"       Ducking will misbehave. Continuing anyway.\n";
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        PrintHresult(L"CoInitializeEx", hr);
        return 1;
    }

    
    
    ApoBindingGuard::Options guardOptions;
    guardOptions.apoClsid = ApoClsidText;
    ApoBindingGuard::Instance().Start(guardOptions);

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr))
    {
        PrintHresult(L"MFStartup", hr);
        CoUninitialize();
        return 1;
    }

    wchar_t fullPath[MAX_PATH] = {};
    DWORD got = GetFullPathNameW(mp3Path.c_str(), MAX_PATH, fullPath, nullptr);
    if (got == 0 || got >= MAX_PATH)
    {
        PrintWin32Error(L"GetFullPathNameW");
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    Mp3Data mp3;
    hr = LoadMp3(fullPath, channels, PCM_RING_BUFFER_SAMPLE_RATE, mp3);
    if (FAILED(hr))
    {
        std::wcerr << L"Failed to decode audio: " << fullPath << L"\n";
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    SECURITY_ATTRIBUTES securityAttributes = {};
    PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
    if (!AllowAudioServiceAccess(securityAttributes, securityDescriptor))
    {
        PrintWin32Error(L"AllowAudioServiceAccess");
    }
    HANDLE mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        securityDescriptor != nullptr ? &securityAttributes : nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(sizeof(PcmRingBufferShared)),
        PCM_RING_BUFFER_MAPPING_NAME);
    const DWORD mappingError = GetLastError();
    if (securityDescriptor != nullptr) LocalFree(securityDescriptor);
    if (!mapping)
    {
        SetLastError(mappingError);
        PrintWin32Error(L"CreateFileMappingW");
        MFShutdown();
        CoUninitialize();
        return 1;
    }
    if (mappingError == ERROR_ALREADY_EXISTS)
    {
        std::wcout << L"Using existing shared memory mapping: " << PCM_RING_BUFFER_MAPPING_NAME << L"\n";
    }
    auto* ring = static_cast<PcmRingBufferShared*>(
        MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(PcmRingBufferShared)));
    if (!ring)
    {
        PrintWin32Error(L"MapViewOfFile");
        CloseHandle(mapping);
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    PcmRingBufferInitialize(ring, channels);
    PcmRingBufferSetConfig(ring, flags, gainDb, logLevel);
    PcmRingBufferSetMusicVolume(ring, volume);

    std::wcout << L"Ring control: replaceMic="
        << ((ring->flags & PCM_RING_FLAG_FILTER_BLOCK_VOICE) ? L"true" : L"false")
        << L" gainDb=" << ring->micAmpInDb
        << L" musicVolume=" << ring->musicVolume
        << L" logLevel=" << ring->logLevel
        << L" originalApo="
        << (originalApo != GUID_NULL ? GuidToString(originalApo) : L"<none>")
        << L" (per-device registry)\n";
    if (duckEnabled)
    {
        std::wcout << L"Ducking: threshold=" << duckThresholdDb << L" dB"
            << L" depth=" << duckDepth
            << L" attack=" << duckAttackMs << L" ms"
            << L" release=" << duckReleaseMs << L" ms\n";
    }

    IMMDevice* listenDevice = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* captureFormat = nullptr;

    if (listen)
    {
        hr = OpenBoundMicrophone(&listenDevice);
        if (FAILED(hr))
        {
            PrintHresult(L"OpenBoundMicrophone", hr);
            UnmapViewOfFile(ring);
            CloseHandle(mapping);
            MFShutdown();
            CoUninitialize();
            return 1;
        }
        std::wcout << L"Capture endpoint: " << GetDeviceName(listenDevice) << L"\n";

        if (originalApo != GUID_NULL)
        {
            if (WriteEndpointOriginalApo(listenDevice, originalApo))
            {
                std::wcout << L"OriginalApoClsid = " << GuidToString(originalApo)
                    << L" written to the endpoint registry\n";
            }
            else
            {
                std::wcerr << L"Failed to write OriginalApoClsid to the endpoint registry (run elevated?)\n";
            }
        }

        hr = listenDevice->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(&audioClient));
        if (FAILED(hr))
        {
            PrintHresult(L"IAudioClient Activate", hr);
            listenDevice->Release();
            UnmapViewOfFile(ring);
            CloseHandle(mapping);
            MFShutdown();
            CoUninitialize();
            return 1;
        }

        WAVEFORMATEX* mixFormat = nullptr;
        hr = audioClient->GetMixFormat(&mixFormat);

        
        if (SUCCEEDED(hr) && mixFormat != nullptr)
        {
            const size_t fmtSize = sizeof(WAVEFORMATEX) + mixFormat->cbSize;
            captureFormat = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(fmtSize));
            if (captureFormat != nullptr)
            {
                memcpy(captureFormat, mixFormat, fmtSize);
            }
        }

        if (SUCCEEDED(hr))
        {
            REFERENCE_TIME defaultPeriod = 0;
            REFERENCE_TIME minimumPeriod = 0;
            hr = audioClient->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
            if (SUCCEEDED(hr))
            {
                hr = audioClient->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    0,
                    defaultPeriod,
                    0,
                    mixFormat,
                    nullptr);
            }
        }
        if (mixFormat != nullptr) CoTaskMemFree(mixFormat);

        if (FAILED(hr))
        {
            PrintHresult(L"IAudioClient Initialize", hr);
            if (captureFormat) { CoTaskMemFree(captureFormat); captureFormat = nullptr; }
            audioClient->Release();
            listenDevice->Release();
            UnmapViewOfFile(ring);
            CloseHandle(mapping);
            MFShutdown();
            CoUninitialize();
            return 1;
        }

        hr = audioClient->GetService(
            __uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(&captureClient));
        if (FAILED(hr))
        {
            PrintHresult(L"IAudioCaptureClient GetService", hr);
            if (captureFormat) { CoTaskMemFree(captureFormat); captureFormat = nullptr; }
            audioClient->Release();
            listenDevice->Release();
            UnmapViewOfFile(ring);
            CloseHandle(mapping);
            MFShutdown();
            CoUninitialize();
            return 1;
        }

        hr = audioClient->Start();
        if (FAILED(hr))
        {
            PrintHresult(L"IAudioClient Start", hr);
            if (captureFormat) { CoTaskMemFree(captureFormat); captureFormat = nullptr; }
            captureClient->Release();
            audioClient->Release();
            listenDevice->Release();
            UnmapViewOfFile(ring);
            CloseHandle(mapping);
            MFShutdown();
            CoUninitialize();
            return 1;
        }
        std::wcout << L"Capture stream started (the endpoint EFX APO is now loaded by audiodg).\n";
    }

    std::wcout << L"Streaming " << fullPath << L" in a loop. Press Enter to stop.\n";

    const UINT32 fileChannels = mp3.channels;
    const size_t fileFrames = mp3.FrameCount();
    size_t playFrame = 0;

    
    timeBeginPeriod(1);

    LARGE_INTEGER qpcFreq = {};
    LARGE_INTEGER qpcPrev = {};
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpcPrev);
    double framesAccumulator = 0.0;

    const UINT32 maxBurstFrames = PCM_RING_BUFFER_CAPACITY_FRAMES / 4;

    float duckGain = 1.0f;

    while (!_kbhit())
    {
        if (captureClient != nullptr)
        {
            UINT32 packetFrames = 0;
            while (SUCCEEDED(captureClient->GetNextPacketSize(&packetFrames)) && packetFrames > 0)
            {
                BYTE* data = nullptr;
                UINT32 availableFrames = 0;
                DWORD bufFlags = 0;
                UINT64 devicePosition = 0;
                UINT64 qpcPosition = 0;
                if (SUCCEEDED(captureClient->GetBuffer(&data, &availableFrames, &bufFlags, &devicePosition, &qpcPosition)))
                {
                    if (duckEnabled && availableFrames > 0 && captureFormat != nullptr &&
                        captureFormat->nSamplesPerSec > 0)
                    {
                        const float rms = ComputeCaptureRms(data, availableFrames, captureFormat, bufFlags);
                        const float rmsDb = 20.0f * log10f(rms + 1e-9f);

                        const float targetDuck = (rmsDb > duckThresholdDb) ? duckDepth : 1.0f;

                        const float blockSeconds =
                            static_cast<float>(availableFrames) /
                            static_cast<float>(captureFormat->nSamplesPerSec);
                        const float tcMs = (targetDuck < duckGain) ? duckAttackMs : duckReleaseMs;
                        float alpha = (blockSeconds * 1000.0f) / tcMs;
                        if (alpha > 1.0f) alpha = 1.0f;
                        if (alpha < 0.0f) alpha = 0.0f;
                        duckGain += (targetDuck - duckGain) * alpha;

                        
                        PcmRingBufferSetMusicVolume(ring, volume * duckGain);
                    }
                    captureClient->ReleaseBuffer(availableFrames);
                }
                else
                {
                    break;
                }
            }
        }

        if (fileFrames == 0)
        {
            Sleep(1);
            continue;
        }

        LARGE_INTEGER qpcNow = {};
        QueryPerformanceCounter(&qpcNow);
        const double elapsedSeconds =
            static_cast<double>(qpcNow.QuadPart - qpcPrev.QuadPart) /
            static_cast<double>(qpcFreq.QuadPart);
        qpcPrev = qpcNow;

        framesAccumulator += elapsedSeconds * PCM_RING_BUFFER_SAMPLE_RATE;

        UINT32 toWrite = static_cast<UINT32>(framesAccumulator);
        framesAccumulator -= toWrite;

        if (toWrite > maxBurstFrames)
        {
            toWrite = maxBurstFrames;
            framesAccumulator = 0.0;
        }

        for (UINT32 i = 0; i < toWrite; ++i)
        {
            if (playFrame >= fileFrames)
            {
                playFrame = 0;
            }

            FLOAT32 frame[PCM_RING_BUFFER_MAX_CHANNELS] = {};
            const FLOAT32* src = mp3.samples.data() + playFrame * fileChannels;
            for (UINT32 ch = 0; ch < channels; ++ch)
            {
                const UINT32 srcCh = (ch < fileChannels) ? ch : 0;
                frame[ch] = src[srcCh];
            }
            ++playFrame;

            PcmRingBufferWriteFrame(ring, frame, channels);
        }

        Sleep(1);
    }

    timeEndPeriod(1);
    _getch();

    UnmapViewOfFile(ring);
    CloseHandle(mapping);

    if (captureClient != nullptr)
    {
        audioClient->Stop();
        captureClient->Release();
    }
    if (audioClient != nullptr) audioClient->Release();
    if (listenDevice != nullptr) listenDevice->Release();
    if (captureFormat != nullptr) CoTaskMemFree(captureFormat);

    ApoBindingGuard::Instance().Stop();

    MFShutdown();
    CoUninitialize();
    return 0;
}