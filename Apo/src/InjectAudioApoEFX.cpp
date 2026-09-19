#include <atlbase.h>
#include <atlcom.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <audioenginebaseapo.h>
#include <baseaudioprocessingobject.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <resource.h>
#include <string>
#include <vector>

#include "InjectAudioApo.h"

namespace
{
    constexpr wchar_t InjectAudioLogDirectory[] = L"C:\\ProgramData\\InjectAudio";
    constexpr wchar_t InjectAudioLogPath[] = L"C:\\ProgramData\\InjectAudio\\InjectAudio.log";

    const GUID kInjectAudioEffectId =
    { 0x8b7c1d2e, 0x4a3f, 0x4e5b, { 0x9c, 0x6d, 0x7e, 0x8f, 0x90, 0xa1, 0xb2, 0xc3 } };
    const GUID kReplaceMicEffectId =
    { 0x1a2b3c4d, 0x5e6f, 0x4708, { 0x9a, 0x0b, 0x1c, 0x2d, 0x3e, 0x4f, 0x50, 0x61 } };
}

void InjectAudioLogHresult(const wchar_t* operation, HRESULT hr)
{
    wchar_t message[256] = {};
    _snwprintf_s(message, _countof(message), _TRUNCATE,
        L"%s hr=0x%08lX", operation, static_cast<unsigned long>(hr));
    InjectAudioLog(message);
}

void InjectAudioLog(const wchar_t* message)
{
        SECURITY_ATTRIBUTES attributes = {};
        SECURITY_DESCRIPTOR descriptor = {};
        InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&descriptor, TRUE, nullptr, FALSE);
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = &descriptor;
        attributes.bInheritHandle = FALSE;

        CreateDirectoryW(InjectAudioLogDirectory, &attributes);

        HANDLE file = CreateFileW(
            InjectAudioLogPath,
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &attributes,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return;
        }

        SYSTEMTIME time = {};
        GetLocalTime(&time);
        wchar_t line[512] = {};
        int length = _snwprintf_s(
            line,
            _countof(line),
            _TRUNCATE,
            L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] [InjectAudio] %s\r\n",
            time.wYear,
            time.wMonth,
            time.wDay,
            time.wHour,
            time.wMinute,
            time.wSecond,
            time.wMilliseconds,
            message);
        if (length > 0)
        {
            int bytes = WideCharToMultiByte(
                CP_UTF8,
                0,
                line,
                length,
                nullptr,
                0,
                nullptr,
                nullptr);
            if (bytes > 0)
            {
                std::string utf8(static_cast<size_t>(bytes), '\0');
                WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    line,
                    length,
                    utf8.data(),
                    bytes,
                    nullptr,
                    nullptr);
                DWORD written = 0;
                WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
            }
        }
        CloseHandle(file);
}

inline float DbToLinear(float db)
{
    return powf(10.0f, db / 20.0f);
}


namespace
{

    void InjectAudioLogError(const wchar_t* operation)
    {
        wchar_t message[128] = {};
        _snwprintf_s(message, _countof(message), _TRUNCATE, L"%s failed, error=%lu", operation, GetLastError());
        InjectAudioLog(message);
    }
}
namespace
{
    void InjectAudioLogFormatted(const wchar_t* level, const wchar_t* format, ...)
    {
        wchar_t message[512] = {};
        va_list args;
        va_start(args, format);
        _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
        va_end(args);

        wchar_t line[640] = {};
        _snwprintf_s(line, _countof(line), _TRUNCATE, L"[%s] %s", level, message);
        InjectAudioLog(line);
    }
}

namespace
{
    const PROPERTYKEY kEndpointGuidKey =
    { { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 4 };

    HRESULT GetEndpointGuidFromInit(UINT32 cbDataSize, BYTE* pbyData, std::wstring& endpointGuid)
    {
        endpointGuid.clear();
        if (pbyData == nullptr || cbDataSize < sizeof(APOInitSystemEffects))
        {
            return E_INVALIDARG;
        }

        const auto* systemEffects = reinterpret_cast<const APOInitSystemEffects*>(pbyData);
        if (systemEffects->pAPOEndpointProperties != nullptr)
        {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(systemEffects->pAPOEndpointProperties->GetValue(kEndpointGuidKey, &value)) &&
                value.vt == VT_LPWSTR && value.pwszVal != nullptr)
            {
                endpointGuid = value.pwszVal;
            }
            PropVariantClear(&value);
        }

        if (endpointGuid.empty() && cbDataSize >= sizeof(APOInitSystemEffects2))
        {
            const auto* systemEffects2 = reinterpret_cast<const APOInitSystemEffects2*>(pbyData);
            if (systemEffects2->pDeviceCollection != nullptr)
            {
                IMMDevice* device = nullptr;
                if (SUCCEEDED(systemEffects2->pDeviceCollection->Item(
                        systemEffects2->nSoftwareIoDeviceInCollection, &device)) && device != nullptr)
                {
                    LPWSTR id = nullptr;
                    if (SUCCEEDED(device->GetId(&id)) && id != nullptr)
                    {
                        const wchar_t* brace = wcsrchr(id, L'{');
                        if (brace != nullptr)
                        {
                            endpointGuid = brace;
                        }
                        CoTaskMemFree(id);
                    }
                    device->Release();
                }
            }
        }

        return endpointGuid.empty() ? E_FAIL : S_OK;
    }

    HRESULT ReadEndpointOriginalApoClsid(const std::wstring& endpointGuid, GUID& clsid)
    {
        clsid = GUID_NULL;
        if (endpointGuid.empty())
        {
            return E_INVALIDARG;
        }

        std::wstring keyPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\";
        keyPath += endpointGuid;
        keyPath += L"\\InjectAudio";

        HKEY key = nullptr;
        LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, KEY_READ, &key);
        if (result != ERROR_SUCCESS)
        {
            return HRESULT_FROM_WIN32(result);
        }

        wchar_t buffer[64] = {};
        DWORD type = 0;
        DWORD size = sizeof(buffer);
        result = RegQueryValueExW(key, L"OriginalApoClsid", nullptr, &type,
            reinterpret_cast<BYTE*>(buffer), &size);
        RegCloseKey(key);

        if (result != ERROR_SUCCESS || type != REG_SZ || buffer[0] == L'\0')
        {
            return E_FAIL;
        }

        return CLSIDFromString(buffer, &clsid);
    }
}

#define LOG_INFO(...)  InjectAudioLogFormatted(L"INFO ", __VA_ARGS__)
#define LOG_WARN(...)  InjectAudioLogFormatted(L"WARN ", __VA_ARGS__)
#define LOG_ERROR(...) InjectAudioLogFormatted(L"ERROR", __VA_ARGS__)
#pragma warning (disable : 4815)
const AVRT_DATA CRegAPOProperties<1> CInjectAudioEFX::sm_RegProperties(
    __uuidof(InjectAudioEFX),
    L"CInjectAudioEFX",
    L"FUCK Copyright (c)",
    1,
    0,
    __uuidof(IAudioProcessingObject),
    (APO_FLAG)(APO_FLAG_SAMPLESPERFRAME_MUST_MATCH |
        APO_FLAG_BITSPERSAMPLE_MUST_MATCH |
        APO_FLAG_INPLACE),
    DEFAULT_APOREG_MININPUTCONNECTIONS,
    DEFAULT_APOREG_MAXINPUTCONNECTIONS,
    DEFAULT_APOREG_MINOUTPUTCONNECTIONS,
    DEFAULT_APOREG_MAXOUTPUTCONNECTIONS,
    DEFAULT_APOREG_MAXINSTANCES);
#pragma AVRT_CODE_BEGIN
STDMETHODIMP_(void) CInjectAudioEFX::APOProcess(
    UINT32 u32NumInputConnections,
    APO_CONNECTION_PROPERTY** ppInputConnections,
    UINT32 u32NumOutputConnections,
    APO_CONNECTION_PROPERTY** ppOutputConnections)
{
    if (m_pOriginalRt)
    {
        m_pOriginalRt->APOProcess(
            u32NumInputConnections, ppInputConnections,
            u32NumOutputConnections, ppOutputConnections);
    }

    if (!u32NumInputConnections || !u32NumOutputConnections) return;
    if (!ppInputConnections || !ppOutputConnections) return;
    if (!ppInputConnections[0] || !ppOutputConnections[0]) return;

    auto* in = ppInputConnections[0];
    auto* out = ppOutputConnections[0];

    if (in->u32BufferFlags != BUFFER_VALID && in->u32BufferFlags != BUFFER_SILENT)
        return;

    const bool inputSilent = (in->u32BufferFlags == BUFFER_SILENT);
    const UINT32 ch = GetSamplesPerFrame();
    const UINT32 frames = in->u32ValidFrameCount;
    const UINT32 count = frames * ch;
    if (count == 0) return;

    float* inBuf = reinterpret_cast<float*>(in->pBuffer);
    float* outBuf = reinterpret_cast<float*>(out->pBuffer);
    if (!inBuf || !outBuf) return;

    float* work = inBuf;
    if (m_pOriginalRt && outBuf != inBuf)
        work = outBuf;

    if (inputSilent)
        memset(work, 0, count * sizeof(float));

    PcmRingBufferShared* ring = m_pcmRingBuffer;
    const bool hasRing = ring != nullptr && PcmRingBufferIsValid(ring);
    bool ringFramesRead = false;
    UINT32 ringFramesReadCount = 0;

    const bool injectEnabled = m_injectEnabled.load(std::memory_order_relaxed);
    const bool ringInjectDisabled =
        hasRing && (ring->flags & PCM_RING_FLAG_DISABLE_INJECT) != 0;
    const bool replaceMic = m_replaceMicEnabled.load(std::memory_order_relaxed) ||
        (hasRing && (ring->flags & PCM_RING_FLAG_FILTER_BLOCK_VOICE) != 0);
    const float injectTarget = (injectEnabled && !ringInjectDisabled) ? 1.0f : 0.0f;
    const float replaceTarget = replaceMic ? 1.0f : 0.0f;
    const float framesPerSecond = GetFramesPerSecond();
    const float rampStep = framesPerSecond > 0.0f
        ? 1.0f / (framesPerSecond * 0.030f)
        : 0.001f;

    if (hasRing)
    {
        const float micGain = DbToLinear(ring->micAmpInDb);
        const bool hasMicGain = fabsf(micGain - 1.0f) > 0.0001f;

        float musicVolume = ring->musicVolume;
        if (musicVolume < 0.0f) musicVolume = 0.0f;
        if (musicVolume > 1.0f) musicVolume = 1.0f;
        const float musicTarget = musicVolume;

        FLOAT32 pcmFrame[PCM_RING_BUFFER_MAX_CHANNELS];
        for (UINT32 frame = 0; frame < frames; ++frame)
        {
            if (!PcmRingBufferReadFrame(ring, pcmFrame, ch))
                break;

            ringFramesRead = true;
            ++ringFramesReadCount;

            if (m_injectMix < injectTarget) { m_injectMix += rampStep; if (m_injectMix > injectTarget) m_injectMix = injectTarget; }
            else if (m_injectMix > injectTarget) { m_injectMix -= rampStep; if (m_injectMix < injectTarget) m_injectMix = injectTarget; }
            if (m_replaceMix < replaceTarget) { m_replaceMix += rampStep; if (m_replaceMix > replaceTarget) m_replaceMix = replaceTarget; }
            else if (m_replaceMix > replaceTarget) { m_replaceMix -= rampStep; if (m_replaceMix < replaceTarget) m_replaceMix = replaceTarget; }
            if (m_musicGain < musicTarget) { m_musicGain += rampStep; if (m_musicGain > musicTarget) m_musicGain = musicTarget; }
            else if (m_musicGain > musicTarget) { m_musicGain -= rampStep; if (m_musicGain < musicTarget) m_musicGain = musicTarget; }

            float* dst = work + frame * ch;
            const float micPart = micGain * (1.0f - m_replaceMix);
            for (UINT32 channel = 0; channel < ch; ++channel)
            {
                float mixed = dst[channel] * micPart + pcmFrame[channel] * m_musicGain * m_injectMix;
                if (mixed > 1.0f) mixed = 1.0f;
                else if (mixed < -1.0f) mixed = -1.0f;
                dst[channel] = mixed;
            }
        }

        if (!ringFramesRead)
        {
            if (m_replaceMix > 0.0f)
            {
                const float fade = 1.0f - m_replaceMix;
                for (UINT32 i = 0; i < count; ++i)
                {
                    work[i] *= fade;
                }
            }
            if (!inputSilent && hasMicGain)
            {
                for (UINT32 i = 0; i < count; ++i)
                {
                    work[i] *= micGain;
                }
            }
        }
    }

    if (work != outBuf)
        memcpy(outBuf, work, count * sizeof(float));

    if (inputSilent)
        out->u32BufferFlags = ringFramesRead ? BUFFER_VALID : BUFFER_SILENT;
    else
        out->u32BufferFlags = BUFFER_VALID;
    out->u32ValidFrameCount = frames;

    if (hasRing && ring->logLevel >= 3)
    {
        m_rtFramesProcessed += frames;
        if (!m_rtLoggedFirst || m_rtFramesProcessed >= 48000)
        {
            m_rtLoggedFirst = true;
            m_rtFramesProcessed = 0;
            InjectAudioLogFormatted(L"RT  ",
                L"endpoint=%s frames=%u read=%u write=%u flags=0x%X outFlags=0x%X musicVol=%.2f",
                m_endpointGuid.empty() ? L"(unknown)" : m_endpointGuid.c_str(),
                frames, ringFramesReadCount,
                static_cast<unsigned>(InterlockedCompareExchange(&ring->writeFrame, 0, 0)),
                in->u32BufferFlags, out->u32BufferFlags, ring->musicVolume);
        }
    }
}
#pragma AVRT_CODE_END
void LogFormat(IAudioMediaType* inFmt, IAudioMediaType* outFmt)
{
    UNCOMPRESSEDAUDIOFORMAT inU{}, outU{};
    if (inFmt) inFmt->GetUncompressedAudioFormat(&inU);
    if (outFmt) outFmt->GetUncompressedAudioFormat(&outU);
    wchar_t message[256] = {};
    _snwprintf_s(message, _countof(message), _TRUNCATE,
        L"In/out format: Type=%04x/%04x, Channels=%d/%d, BitsPerSample=%d/%d, Samplerate=%.0f/%.0f\n",
        inU.guidFormatType.Data1 & 0xFFFF, outU.guidFormatType.Data1 & 0xFFFF,
        (int)inU.dwSamplesPerFrame, (int)outU.dwSamplesPerFrame,
        (int)inU.dwValidBitsPerSample, (int)outU.dwValidBitsPerSample,
        inU.fFramesPerSecond, outU.fFramesPerSecond);

    InjectAudioLog(message);
}
static HRESULT ValidateFloatPcmFormat(IAudioMediaType* mediaType)
{
    if (mediaType == nullptr)
    {
        return E_POINTER;
    }
    LogFormat(mediaType, mediaType);
    UNCOMPRESSEDAUDIOFORMAT format = {};
    HRESULT hr = mediaType->GetUncompressedAudioFormat(&format);
    if (FAILED(hr))
    {
        return hr;
    }

    if (format.guidFormatType != KSDATAFORMAT_SUBTYPE_IEEE_FLOAT ||
        format.dwBytesPerSampleContainer != sizeof(FLOAT32) ||
        format.dwValidBitsPerSample != sizeof(FLOAT32) * 8 ||
        format.dwSamplesPerFrame == 0 ||
        format.dwSamplesPerFrame > PCM_RING_BUFFER_MAX_CHANNELS)
    {
        wchar_t message[256] = {};
        _snwprintf_s(message, _countof(message), _TRUNCATE,
            L"container=%u, valid=%u, channels=%u, maxChannels=%d\n",
            format.dwBytesPerSampleContainer,
            format.dwValidBitsPerSample,
            format.dwSamplesPerFrame,
            PCM_RING_BUFFER_MAX_CHANNELS);
        InjectAudioLog(message);
        return APOERR_FORMAT_NOT_SUPPORTED;
    }

    return S_OK;
}

STDMETHODIMP CInjectAudioEFX::GetLatency(HNSTIME* pTime)
{
    if (pTime == nullptr)
    {
        return E_POINTER;
    }

    *pTime = 0;
    return S_OK;
}

STDMETHODIMP CInjectAudioEFX::LockForProcess(
    UINT32 u32NumInputConnections,
    APO_CONNECTION_DESCRIPTOR** ppInputConnections,
    UINT32 u32NumOutputConnections,
    APO_CONNECTION_DESCRIPTOR** ppOutputConnections)
{
    LOG_INFO(L"CInjectAudioEFX::LockForProcess endpoint=%s",
        m_endpointGuid.empty() ? L"(unknown)" : m_endpointGuid.c_str());

    if (m_pcmRingBuffer == nullptr)
    {
        HRESULT ringHr = OpenRingBuffer();
        if (FAILED(ringHr))
            LOG_WARN(L"LockForProcess: ring buffer still unavailable - audio will pass through");
    }

    if (m_pOriginalCfg)
    {
        HRESULT hr = m_pOriginalCfg->LockForProcess(
            u32NumInputConnections, ppInputConnections,
            u32NumOutputConnections, ppOutputConnections);
        if (FAILED(hr))
        {
            LOG_ERROR(L"Failed in LockForProcess of original apo [Code=%x]", hr);
            return hr;
        }
    }

    HRESULT hr = CBaseAudioProcessingObject::LockForProcess(
        u32NumInputConnections, ppInputConnections,
        u32NumOutputConnections, ppOutputConnections);
    if (FAILED(hr))
    {
        LOG_ERROR(L"Failed in LockForProcess [Code=%x]", hr);
        return hr;
    }

    if (u32NumInputConnections > 0 && ppInputConnections && ppInputConnections[0] && ppInputConnections[0]->pFormat)
    {
        LogFormat(ppInputConnections[0]->pFormat,
            (u32NumOutputConnections && ppOutputConnections && ppOutputConnections[0])
            ? ppOutputConnections[0]->pFormat : nullptr);
    }

    LOG_INFO(L"LockForProcess finished successfully");
    return hr;
}
STDMETHODIMP CInjectAudioEFX::UnlockForProcess()
{
    InjectAudioLog(L"CInjectAudioEFX::UnlockForProcess");

    if (m_pOriginalCfg)
    {
        HRESULT hr = m_pOriginalCfg->UnlockForProcess();
        if (FAILED(hr))
        {
            LOG_ERROR(L"Failed in UnlockForProcess of original apo [Code=%x]", hr);
            return hr;
        }
    }

    return CBaseAudioProcessingObject::UnlockForProcess();
}
STDMETHODIMP CInjectAudioEFX::Initialize(UINT32 cbDataSize, BYTE* pbyData)
{
    InjectAudioLog(L"Initialize");
    if ((cbDataSize == 0) != (pbyData == nullptr))
    {
        InjectAudioLogHresult(L"Initialize invalid arguments", E_INVALIDARG);
        return E_INVALIDARG;
    }

    if (pbyData != nullptr)
    {
        const bool acceptedInitStruct =
            cbDataSize == sizeof(APOInitBaseStruct) ||
            cbDataSize == sizeof(APOInitSystemEffects) ||
            cbDataSize == sizeof(APOInitSystemEffects2) ||
            cbDataSize == sizeof(APOInitSystemEffects3);

        if (!acceptedInitStruct)
        {
            wchar_t message[128] = {};
            _snwprintf_s(message, _countof(message), _TRUNCATE,
                L"Initialize unexpected cbDataSize=%u", cbDataSize);
            InjectAudioLog(message);
            InjectAudioLogHresult(L"Initialize rejected init data", E_INVALIDARG);
            return E_INVALIDARG;
        }
    }
    OpenRingBuffer();

    auto hr =  CBaseAudioProcessingObject::Initialize(cbDataSize, pbyData);
    if (FAILED(hr))
    {
        LOG_ERROR(L"Failed in Initialize [Code=%x]", hr);
        return hr;
    }

    const bool replaceFromRing =
        m_pcmRingBuffer != nullptr && PcmRingBufferIsValid(m_pcmRingBuffer) &&
        (m_pcmRingBuffer->flags & PCM_RING_FLAG_FILTER_BLOCK_VOICE) != 0;
    m_injectEnabled.store(true);
    m_replaceMicEnabled.store(replaceFromRing);
    m_injectMix = 1.0f;
    m_replaceMix = replaceFromRing ? 1.0f : 0.0f;
    m_musicGain = (m_pcmRingBuffer != nullptr && PcmRingBufferIsValid(m_pcmRingBuffer))
        ? m_pcmRingBuffer->musicVolume : 1.0f;
    m_effectInfos[0] = { kInjectAudioEffectId, TRUE,
        AUDIO_SYSTEMEFFECT_STATE_ON };
    m_effectInfos[1] = { kReplaceMicEffectId, TRUE,
        replaceFromRing ? AUDIO_SYSTEMEFFECT_STATE_ON : AUDIO_SYSTEMEFFECT_STATE_OFF };

    std::wstring endpointGuid;
    GUID originalClsid = GUID_NULL;
    HRESULT guidHr = GetEndpointGuidFromInit(cbDataSize, pbyData, endpointGuid);
    if (SUCCEEDED(guidHr))
    {
        m_endpointGuid = endpointGuid;
        LOG_INFO(L"Initialize cbDataSize=%u endpoint=%s", cbDataSize, endpointGuid.c_str());

        HRESULT regHr = ReadEndpointOriginalApoClsid(endpointGuid, originalClsid);
        if (SUCCEEDED(regHr) && originalClsid != GUID_NULL)
        {
            wchar_t guidText[64] = {};
            StringFromGUID2(originalClsid, guidText, _countof(guidText));
            LOG_INFO(L"Wrapping original APO %s", guidText);
            hr = InitializeOriginalApo(originalClsid, cbDataSize, pbyData);
            if (FAILED(hr))
                LOG_WARN(L"Continuing without original APO [hr=0x%08X]", static_cast<unsigned>(hr));
        }
        else
        {
            LOG_INFO(L"No original APO configured [hr=0x%08X]", static_cast<unsigned>(regHr));
        }
    }
    else
    {
        LOG_WARN(L"Initialize cbDataSize=%u endpoint GUID unavailable [hr=0x%08X]",
            cbDataSize, static_cast<unsigned>(guidHr));
    }

    LOG_INFO(L"Initialize completed successfully");
    return S_OK;

}

STDMETHODIMP CInjectAudioEFX::IsInputFormatSupported(
    IAudioMediaType* pOutputFormat,
    IAudioMediaType* pRequestedInputFormat,
    IAudioMediaType** ppSupportedInputFormat)
{
    if (pRequestedInputFormat == nullptr || ppSupportedInputFormat == nullptr)
    {
        InjectAudioLogHresult(L"IsInputFormatSupported invalid arguments", E_POINTER);
        return E_POINTER;
    }
    InjectAudioLog(L"[IsInputFormatSupported]");
    LogFormat(pOutputFormat, pRequestedInputFormat);
    LOG_INFO(L"CInjectAudioEFX::IsInputFormatSupported");
    LogFormat(pRequestedInputFormat, pOutputFormat);
    if (m_pOriginalApo)
    {

        HRESULT hr = m_pOriginalApo->IsInputFormatSupported(
            pOutputFormat, pRequestedInputFormat, ppSupportedInputFormat);
        if (FAILED(hr))
            LOG_ERROR(L"Input format is not supported by original apo [Code=%x]", hr);
        else
            LOG_INFO(L"Input format is supported");
        return hr;
    }
    return CBaseAudioProcessingObject::IsInputFormatSupported(
        pOutputFormat, pRequestedInputFormat, ppSupportedInputFormat);
}
STDMETHODIMP CInjectAudioEFX::IsOutputFormatSupported(
    IAudioMediaType* pInputFormat,
    IAudioMediaType* pRequestedOutputFormat,
    IAudioMediaType** ppSupportedOutputFormat)
{
    if (pRequestedOutputFormat == nullptr || ppSupportedOutputFormat == nullptr)
    {
        InjectAudioLogHresult(L"IsOutputFormatSupported invalid arguments", E_POINTER);
        return E_POINTER;
    }
    LOG_INFO(L"CInjectAudioEFX::IsOutputFormatSupported");
    LogFormat(pRequestedOutputFormat, pInputFormat);
    if (m_pOriginalApo)
    {
        HRESULT hr = m_pOriginalApo->IsOutputFormatSupported(
            pInputFormat, pRequestedOutputFormat, ppSupportedOutputFormat);
        if (FAILED(hr))
            LOG_ERROR(L"Output format is not supported by original apo [Code=%x]", hr);
        else
            LOG_INFO(L"Output format is supported");
        return hr;
    }
    return CBaseAudioProcessingObject::IsOutputFormatSupported(
        pInputFormat, pRequestedOutputFormat, ppSupportedOutputFormat);
}

HRESULT CInjectAudioEFX::InitializeOriginalApo(const GUID& originalClsid, UINT32 cbDataSize, BYTE* pbyData)
{
    LOG_INFO(L"Initialize original apo");
    LOG_INFO(L"CInjectAudioEFX::InitializeOriginalApo");

    IAudioProcessingObject* apo = nullptr;
    HRESULT hr = CoCreateInstance(originalClsid, nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IAudioProcessingObject), reinterpret_cast<void**>(&apo));
    if (FAILED(hr) || !apo)
    {
        LOG_ERROR(L"Failed to init original apo [Line=%d, Code=%x]", __LINE__, hr);
        return hr;
    }

    
    
    
    auto tryInitialize = [&](UINT32 size, BYTE* data, const wchar_t* name) -> HRESULT
    {
        const HRESULT result = apo->Initialize(size, data);
        if (SUCCEEDED(result))
        {
            LOG_INFO(L"original APO initialized with %s (size=%u)", name, size);
        }
        return result;
    };

    if (pbyData != nullptr && cbDataSize >= sizeof(APOInitBaseStruct))
    {
        std::vector<BYTE> copy(pbyData, pbyData + cbDataSize);
        reinterpret_cast<APOInitBaseStruct*>(copy.data())->clsid = originalClsid;
        hr = tryInitialize(cbDataSize, copy.data(), L"engine data (clsid replaced)");
        if (FAILED(hr))
        {
            hr = tryInitialize(cbDataSize, pbyData, L"engine data (as-is)");
        }
    }
    
    
    
    if (FAILED(hr) && pbyData != nullptr && cbDataSize >= 0x18 + sizeof(void*))
    {
        LOG_INFO(L"init struct sizes: base=%u se=%u se2=%u se3=%u cbDataSize=%u",
            static_cast<unsigned>(sizeof(APOInitBaseStruct)),
            static_cast<unsigned>(sizeof(APOInitSystemEffects)),
            static_cast<unsigned>(sizeof(APOInitSystemEffects2)),
            static_cast<unsigned>(sizeof(APOInitSystemEffects3)), cbDataSize);

        auto* endpointProps = *reinterpret_cast<IPropertyStore* const*>(pbyData + 0x18);
        if (endpointProps != nullptr)
        {
            auto buildVariant = [&](UINT32 size, const wchar_t* name) -> HRESULT
            {
                std::vector<BYTE> buffer(size, 0);
                *reinterpret_cast<UINT32*>(buffer.data()) = size;
                *reinterpret_cast<GUID*>(buffer.data() + sizeof(UINT32)) = originalClsid;
                *reinterpret_cast<IPropertyStore**>(buffer.data() + 0x18) = endpointProps;
                return tryInitialize(size, buffer.data(), name);
            };

            hr = buildVariant(88, L"SystemEffects2(88) props-only");
            if (FAILED(hr))
            {
                hr = buildVariant(56, L"SystemEffects(56) props-only");
            }
        }
    }
    if (FAILED(hr))
    {
        APOInitSystemEffects3 se3 = {};
        se3.APOInit.cbSize = sizeof(se3);
        se3.APOInit.clsid = originalClsid;
        hr = tryInitialize(sizeof(se3), reinterpret_cast<BYTE*>(&se3), L"APOInitSystemEffects3");
    }
    if (FAILED(hr))
    {
        APOInitSystemEffects2 se2 = {};
        se2.APOInit.cbSize = sizeof(se2);
        se2.APOInit.clsid = originalClsid;
        hr = tryInitialize(sizeof(se2), reinterpret_cast<BYTE*>(&se2), L"APOInitSystemEffects2");
    }
    if (FAILED(hr))
    {
        APOInitSystemEffects se = {};
        se.APOInit.cbSize = sizeof(se);
        se.APOInit.clsid = originalClsid;
        hr = tryInitialize(sizeof(se), reinterpret_cast<BYTE*>(&se), L"APOInitSystemEffects");
    }
    if (FAILED(hr))
    {
        APOInitBaseStruct base = {};
        base.cbSize = sizeof(base);
        base.clsid = originalClsid;
        hr = tryInitialize(sizeof(base), reinterpret_cast<BYTE*>(&base), L"APOInitBaseStruct");
    }
    if (FAILED(hr))
    {
        hr = tryInitialize(0, nullptr, L"(0, nullptr)");
    }

    if (FAILED(hr))
    {
        LOG_WARN(L"original APO rejected every init variant [Code=%x] - continuing without it", hr);
        apo->Release();
        return hr;
    }

    m_pOriginalApo = apo;
    apo->QueryInterface(__uuidof(IAudioProcessingObjectRT), reinterpret_cast<void**>(&m_pOriginalRt));
    apo->QueryInterface(__uuidof(IAudioProcessingObjectConfiguration), reinterpret_cast<void**>(&m_pOriginalCfg));
    LOG_INFO(L"Successfully created and initialized original APO");
    return S_OK;
}

STDMETHODIMP CInjectAudioEFX::GetInputChannelCount(UINT32* pu32ChannelCount)
{
    if (!m_bIsInitialized)
    {
        return APOERR_NOT_INITIALIZED;
    }
    if (pu32ChannelCount == nullptr)
    {
        return E_POINTER;
    }

    *pu32ChannelCount = GetSamplesPerFrame();
    return S_OK;
}

STDMETHODIMP CInjectAudioEFX::GetEffectsList(
    _Outptr_result_buffer_maybenull_(*pcEffects) LPGUID* ppEffectsIds,
    _Out_ UINT* pcEffects,
    _In_ HANDLE Event)
{
    LOG_INFO(L"GetEffectsList endpoint=%s",
        m_endpointGuid.empty() ? L"(unknown)" : m_endpointGuid.c_str());

    if (ppEffectsIds == nullptr || pcEffects == nullptr)
    {
        return E_POINTER;
    }

    *ppEffectsIds = nullptr;
    *pcEffects = 0;

    CComCritSecLock<CComAutoCriticalSection> lock(m_effectsLock);

    if (m_hEffectsChangedEvent != nullptr)
    {
        CloseHandle(m_hEffectsChangedEvent);
        m_hEffectsChangedEvent = nullptr;
    }
    if (Event != nullptr)
    {
        if (!DuplicateHandle(GetCurrentProcess(), Event, GetCurrentProcess(),
                &m_hEffectsChangedEvent, EVENT_MODIFY_STATE, FALSE, 0))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
    }

    UINT count = 0;
    for (UINT i = 0; i < kEffectCount; ++i)
    {
        if (m_effectInfos[i].state == AUDIO_SYSTEMEFFECT_STATE_ON)
        {
            ++count;
        }
    }

    if (count == 0)
    {
        return S_OK;
    }

    GUID* ids = static_cast<GUID*>(CoTaskMemAlloc(sizeof(GUID) * count));
    if (ids == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    UINT index = 0;
    for (UINT i = 0; i < kEffectCount; ++i)
    {
        if (m_effectInfos[i].state == AUDIO_SYSTEMEFFECT_STATE_ON)
        {
            ids[index++] = m_effectInfos[i].id;
        }
    }

    *ppEffectsIds = ids;
    *pcEffects = count;
    return S_OK;
}

STDMETHODIMP CInjectAudioEFX::GetControllableSystemEffectsList(
    _Outptr_result_buffer_maybenull_(*numEffects) AUDIO_SYSTEMEFFECT** effects,
    _Out_ UINT* numEffects,
    _In_opt_ HANDLE event)
{
    LOG_INFO(L"GetControllableSystemEffectsList endpoint=%s",
        m_endpointGuid.empty() ? L"(unknown)" : m_endpointGuid.c_str());

    if (effects == nullptr || numEffects == nullptr)
    {
        return E_POINTER;
    }

    *effects = nullptr;
    *numEffects = 0;

    CComCritSecLock<CComAutoCriticalSection> lock(m_effectsLock);

    if (m_hEffectsChangedEvent != nullptr)
    {
        CloseHandle(m_hEffectsChangedEvent);
        m_hEffectsChangedEvent = nullptr;
    }
    if (event != nullptr)
    {
        if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(),
                &m_hEffectsChangedEvent, EVENT_MODIFY_STATE, FALSE, 0))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
    }

    AUDIO_SYSTEMEFFECT* list = static_cast<AUDIO_SYSTEMEFFECT*>(
        CoTaskMemAlloc(sizeof(AUDIO_SYSTEMEFFECT) * kEffectCount));
    if (list == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    for (UINT i = 0; i < kEffectCount; ++i)
    {
        list[i] = m_effectInfos[i];
    }

    *effects = list;
    *numEffects = kEffectCount;
    return S_OK;
}

STDMETHODIMP CInjectAudioEFX::SetAudioSystemEffectState(
    GUID effectId,
    AUDIO_SYSTEMEFFECT_STATE state)
{
    CComCritSecLock<CComAutoCriticalSection> lock(m_effectsLock);

    for (UINT i = 0; i < kEffectCount; ++i)
    {
        if (!IsEqualGUID(effectId, m_effectInfos[i].id))
        {
            continue;
        }

        if (m_effectInfos[i].state != state)
        {
            m_effectInfos[i].state = state;
            const bool enabled = (state == AUDIO_SYSTEMEFFECT_STATE_ON);
            if (IsEqualGUID(effectId, kInjectAudioEffectId))
            {
                m_injectEnabled.store(enabled);
            }
            else if (IsEqualGUID(effectId, kReplaceMicEffectId))
            {
                m_replaceMicEnabled.store(enabled);
            }
            if (m_hEffectsChangedEvent != nullptr)
            {
                SetEvent(m_hEffectsChangedEvent);
            }
            LOG_INFO(L"SetAudioSystemEffectState: effect=%08X state=%u",
                effectId.Data1, static_cast<unsigned>(state));
        }
        return S_OK;
    }

    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT CInjectAudioEFX::OpenRingBuffer()
{
    CloseRingBuffer();

    bool weCreated = false;

    m_pcmRingMapping = OpenFileMappingW(
        FILE_MAP_ALL_ACCESS, FALSE, PCM_RING_BUFFER_MAPPING_NAME);

    if (m_pcmRingMapping == nullptr)
    {
        SECURITY_ATTRIBUTES sa = {};
        SECURITY_DESCRIPTOR sd = {};
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = &sd;
        sa.bInheritHandle = FALSE;

        m_pcmRingMapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            &sa,
            PAGE_READWRITE,
            0,
            static_cast<DWORD>(sizeof(PcmRingBufferShared)),
            PCM_RING_BUFFER_MAPPING_NAME);

        if (m_pcmRingMapping == nullptr)
        {
            LOG_ERROR(L"CreateFileMappingW(%s) failed [Code=%x]",
                PCM_RING_BUFFER_MAPPING_NAME, GetLastError());
            return HRESULT_FROM_WIN32(GetLastError());
        }

        weCreated = (GetLastError() != ERROR_ALREADY_EXISTS);
    }

    m_pcmRingBuffer = static_cast<PcmRingBufferShared*>(MapViewOfFile(
        m_pcmRingMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PcmRingBufferShared)));

    if (m_pcmRingBuffer == nullptr)
    {
        LOG_ERROR(L"MapViewOfFile failed [Code=%x]", GetLastError());
        CloseRingBuffer();
        return HRESULT_FROM_WIN32(GetLastError());
    }

    if (weCreated)
    {
        PcmRingBufferInitialize(m_pcmRingBuffer, PCM_RING_BUFFER_MAX_CHANNELS);
        LOG_INFO(L"PcmRingBuffer created by APO (producer not yet running)");
    }
    else if (!PcmRingBufferIsValid(m_pcmRingBuffer))
    {
        LOG_WARN(L"PcmRingBuffer not yet initialized - will retry");
        CloseRingBuffer();
        return E_FAIL;
    }

    LOG_INFO(L"PcmRingBuffer ready: channels=%u rate=%u",
        m_pcmRingBuffer->channels, m_pcmRingBuffer->sampleRate);
    LOG_INFO(L"Control: filterBlockVoice=%s micAmpInDb=%.2f logLevel=%u musicVolume=%.2f",
        (m_pcmRingBuffer->flags & PCM_RING_FLAG_FILTER_BLOCK_VOICE) ? L"true" : L"false",
        m_pcmRingBuffer->micAmpInDb,
        m_pcmRingBuffer->logLevel,
        m_pcmRingBuffer->musicVolume);
    return S_OK;
}
void CInjectAudioEFX::CloseRingBuffer()
{
    if (m_pcmRingBuffer != nullptr)
    {
        UnmapViewOfFile(m_pcmRingBuffer);
        m_pcmRingBuffer = nullptr;
    }
    if (m_pcmRingMapping != nullptr)
    {
        CloseHandle(m_pcmRingMapping);
        m_pcmRingMapping = nullptr;
    }
}
CInjectAudioEFX::~CInjectAudioEFX()
{
    CloseRingBuffer();
    if (m_hEffectsChangedEvent != nullptr)
    {
        CloseHandle(m_hEffectsChangedEvent);
        m_hEffectsChangedEvent = nullptr;
    }
    if (m_pOriginalRt) { m_pOriginalRt->Release(); m_pOriginalRt = nullptr; }
    if (m_pOriginalCfg) { m_pOriginalCfg->Release(); m_pOriginalCfg = nullptr; }
    if (m_pOriginalApo) { m_pOriginalApo->Release(); m_pOriginalApo = nullptr; }
}
