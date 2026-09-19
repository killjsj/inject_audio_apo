#pragma once

#include <atomic>
#include <string>
#include <audioenginebaseapo.h>
#include <BaseAudioProcessingObject.h>
#include <audioengineextensionapo.h>
#include "InjectAudioApoDll.h"
#include "PcmRingBuffer.h"

#include "commonmacros.h"

_Analysis_mode_(_Analysis_code_type_user_driver_)

void InjectAudioLog(const wchar_t* message);
void InjectAudioLogHresult(const wchar_t* operation, HRESULT hr);

#pragma AVRT_VTABLES_BEGIN
class CInjectAudioEFX :
    public CComObjectRootEx<CComMultiThreadModel>,
    public CComCoClass<CInjectAudioEFX, &CLSID_InjectAudioEFX>,
    public CBaseAudioProcessingObject,
    public IAudioSystemEffects3
{
public:
    CInjectAudioEFX()
    :   CBaseAudioProcessingObject(sm_RegProperties)
    {
        InjectAudioLog(L"CInjectAudioEFX constructed");
    }

    virtual ~CInjectAudioEFX();

DECLARE_REGISTRY_RESOURCEID(IDR_InjectAudioEFX)

BEGIN_COM_MAP(CInjectAudioEFX)
    COM_INTERFACE_ENTRY(IAudioProcessingObjectRT)
    COM_INTERFACE_ENTRY(IAudioProcessingObject)
    COM_INTERFACE_ENTRY(IAudioProcessingObjectConfiguration)
    COM_INTERFACE_ENTRY(IAudioSystemEffects)
    COM_INTERFACE_ENTRY(IAudioSystemEffects2)
    COM_INTERFACE_ENTRY(IAudioSystemEffects3)
END_COM_MAP()

DECLARE_PROTECT_FINAL_CONSTRUCT()

public:
    STDMETHOD_(void, APOProcess)(UINT32 u32NumInputConnections,
        APO_CONNECTION_PROPERTY** ppInputConnections, UINT32 u32NumOutputConnections,
        APO_CONNECTION_PROPERTY** ppOutputConnections);

    STDMETHOD(GetLatency)(HNSTIME* pTime);

    STDMETHOD(LockForProcess)(UINT32 u32NumInputConnections,
        APO_CONNECTION_DESCRIPTOR** ppInputConnections,  
        UINT32 u32NumOutputConnections, APO_CONNECTION_DESCRIPTOR** ppOutputConnections);

    STDMETHOD(Initialize)(UINT32 cbDataSize, BYTE* pbyData);

    STDMETHOD(IsInputFormatSupported)(IAudioMediaType *pOutputFormat, IAudioMediaType *pRequestedInputFormat, IAudioMediaType **ppSupportedInputFormat);
    STDMETHOD(IsOutputFormatSupported)(IAudioMediaType *pInputFormat, IAudioMediaType *pRequestedOutputFormat, IAudioMediaType **ppSupportedOutputFormat);
    STDMETHOD(GetInputChannelCount)(UINT32 *pu32ChannelCount);
    STDMETHODIMP UnlockForProcess() override;

    STDMETHOD(GetEffectsList)(_Outptr_result_buffer_maybenull_(*pcEffects) LPGUID* ppEffectsIds, _Out_ UINT* pcEffects, _In_ HANDLE Event);

    STDMETHOD(GetControllableSystemEffectsList)(_Outptr_result_buffer_maybenull_(*numEffects) AUDIO_SYSTEMEFFECT** effects, _Out_ UINT* numEffects, _In_opt_ HANDLE event);
    STDMETHOD(SetAudioSystemEffectState)(GUID effectId, AUDIO_SYSTEMEFFECT_STATE state);
public:
    static const CRegAPOProperties<1>       sm_RegProperties;
    HRESULT OpenRingBuffer();
    void CloseRingBuffer();
    HRESULT InitializeOriginalApo(const GUID& originalClsid, UINT32 cbDataSize, BYTE* pbyData);

private:
    std::wstring            m_endpointGuid;
    LONGLONG                m_rtFramesProcessed = 0;
    bool                    m_rtLoggedFirst = false;

    HANDLE                  m_pcmRingMapping = nullptr;
    PcmRingBufferShared*    m_pcmRingBuffer = nullptr;

    IAudioProcessingObject* m_pOriginalApo = nullptr;
    IAudioProcessingObjectRT* m_pOriginalRt = nullptr;
    IAudioProcessingObjectConfiguration* m_pOriginalCfg = nullptr;

    static constexpr UINT   kEffectCount = 2;
    AUDIO_SYSTEMEFFECT      m_effectInfos[kEffectCount] = {};
    std::atomic<bool>       m_injectEnabled{ true };
    std::atomic<bool>       m_replaceMicEnabled{ false };
    float                   m_injectMix = 1.0f;
    float                   m_replaceMix = 0.0f;
    float                   m_musicGain = 1.0f;   
    CComAutoCriticalSection m_effectsLock;
    HANDLE                  m_hEffectsChangedEvent = nullptr;
};
#pragma AVRT_VTABLES_END

OBJECT_ENTRY_AUTO(__uuidof(InjectAudioEFX), CInjectAudioEFX)