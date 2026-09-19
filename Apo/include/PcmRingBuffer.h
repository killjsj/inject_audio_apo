#pragma once
#include <windows.h>

#define PCM_RING_BUFFER_MAGIC 0x50434D52u
#define PCM_RING_BUFFER_VERSION 4u
#define PCM_RING_BUFFER_SAMPLE_RATE 48000u
#define PCM_RING_BUFFER_MAX_CHANNELS 8u
#define PCM_RING_BUFFER_CAPACITY_FRAMES 48000u
#define PCM_RING_BUFFER_MAPPING_NAME L"Global\\InjectAudioPcmRingBuffer"

#define PCM_RING_FLAG_FILTER_BLOCK_VOICE 0x00000001u



#define PCM_RING_FLAG_DISABLE_INJECT 0x00000002u

#define PCM_RING_MAX_CONSUMERS_PER_THREAD 8

struct PcmRingBufferShared
{
    DWORD magic;
    DWORD version;
    DWORD sampleRate;
    DWORD channels;
    DWORD capacityFrames;

    DWORD flags;
    volatile float micAmpInDb;
    DWORD logLevel;

    volatile LONG writeFrame;
    volatile LONG readFrame;

    volatile float musicVolume;
    DWORD reserved0;

    FLOAT32 samples[PCM_RING_BUFFER_CAPACITY_FRAMES * PCM_RING_BUFFER_MAX_CHANNELS];
};

inline void PcmRingBufferInitialize(
    PcmRingBufferShared* ring,
    UINT32 channels)
{
    if (ring == nullptr || channels == 0 || channels > PCM_RING_BUFFER_MAX_CHANNELS)
    {
        return;
    }

    ZeroMemory(ring, sizeof(PcmRingBufferShared));
    ring->magic = PCM_RING_BUFFER_MAGIC;
    ring->version = PCM_RING_BUFFER_VERSION;
    ring->sampleRate = PCM_RING_BUFFER_SAMPLE_RATE;
    ring->channels = channels;
    ring->capacityFrames = PCM_RING_BUFFER_CAPACITY_FRAMES;
    ring->flags = 0;
    ring->micAmpInDb = 0.0f;
    ring->logLevel = 0;
    ring->musicVolume = 1.0f;
    ring->reserved0 = 0;
    InterlockedExchange(&ring->readFrame, 0);
    InterlockedExchange(&ring->writeFrame, 0);
}

inline bool PcmRingBufferIsValid(const PcmRingBufferShared* ring)
{
    return ring != nullptr &&
        ring->magic == PCM_RING_BUFFER_MAGIC &&
        ring->version == PCM_RING_BUFFER_VERSION &&
        ring->sampleRate == PCM_RING_BUFFER_SAMPLE_RATE &&
        ring->channels >= 1 &&
        ring->channels <= PCM_RING_BUFFER_MAX_CHANNELS &&
        ring->capacityFrames == PCM_RING_BUFFER_CAPACITY_FRAMES;
}

inline void PcmRingBufferSetConfig(
    PcmRingBufferShared* ring,
    DWORD flags,
    float micAmpInDb,
    DWORD logLevel)
{
    if (!PcmRingBufferIsValid(ring)) return;
    ring->flags = flags;
    ring->micAmpInDb = micAmpInDb;
    ring->logLevel = logLevel;
}

inline void PcmRingBufferSetMusicVolume(
    PcmRingBufferShared* ring,
    float volume)
{
    if (!PcmRingBufferIsValid(ring)) return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    ring->musicVolume = volume;
}

struct PcmRingConsumerState
{
    PcmRingBufferShared* ring;
    LONG readFrame;
};

inline bool PcmRingBufferReadFrame(
    PcmRingBufferShared* ring,
    FLOAT32* frame,
    UINT32 outputChannels)
{
    if (!PcmRingBufferIsValid(ring) || frame == nullptr || outputChannels == 0 || outputChannels > PCM_RING_BUFFER_MAX_CHANNELS)
    {
        return false;
    }

    static thread_local PcmRingConsumerState states[PCM_RING_MAX_CONSUMERS_PER_THREAD];
    static thread_local int stateCount = 0;

    int foundIndex = -1;
    for (int i = 0; i < stateCount; ++i)
    {
        if (states[i].ring == ring)
        {
            foundIndex = i;
            break;
        }
    }

    LONG localRead;
    if (foundIndex < 0)
    {
        LONG writeFrame = InterlockedCompareExchange(&ring->writeFrame, 0, 0);
        if (stateCount >= PCM_RING_MAX_CONSUMERS_PER_THREAD)
        {
            return false;
        }
        foundIndex = stateCount++;
        states[foundIndex].ring = ring;
        states[foundIndex].readFrame = writeFrame;
        localRead = writeFrame;
    }
    else
    {
        localRead = states[foundIndex].readFrame;
    }

    LONG writeFrame = InterlockedCompareExchange(&ring->writeFrame, 0, 0);
    if (localRead == writeFrame)
    {
        return false;
    }

    const ULONG distance = static_cast<ULONG>(writeFrame) - static_cast<ULONG>(localRead);
    if (distance > ring->capacityFrames)
    {
        localRead = static_cast<LONG>(static_cast<ULONG>(writeFrame) - ring->capacityFrames);
    }

    const UINT32 ringFrame = static_cast<UINT32>(localRead) % ring->capacityFrames;
    const FLOAT32* source = &ring->samples[ringFrame * ring->channels];

    if (ring->channels == 1)
    {
        
        for (UINT32 channel = 0; channel < outputChannels; ++channel)
        {
            frame[channel] = source[0];
        }
    }
    else if (outputChannels == 1)
    {
        
        FLOAT32 sum = 0.0f;
        for (UINT32 channel = 0; channel < ring->channels; ++channel)
        {
            sum += source[channel];
        }
        frame[0] = sum / static_cast<FLOAT32>(ring->channels);
    }
    else
    {
        for (UINT32 channel = 0; channel < outputChannels; ++channel)
        {
            frame[channel] = source[channel % ring->channels];
        }
    }

    localRead = static_cast<LONG>(static_cast<ULONG>(localRead) + 1u);
    states[foundIndex].readFrame = localRead;

    return true;
}

inline bool PcmRingBufferWriteFrame(
    PcmRingBufferShared* ring,
    const FLOAT32* frame,
    UINT32 inputChannels)
{
    if (!PcmRingBufferIsValid(ring) ||
        frame == nullptr ||
        inputChannels != ring->channels)
    {
        return false;
    }

    const LONG writeFrame = InterlockedCompareExchange(&ring->writeFrame, 0, 0);
    const UINT32 ringFrame = static_cast<UINT32>(writeFrame) % ring->capacityFrames;
    FLOAT32* destination = &ring->samples[ringFrame * ring->channels];

    for (UINT32 channel = 0; channel < ring->channels; ++channel)
    {
        destination[channel] = frame[channel];
    }

    MemoryBarrier();
    InterlockedExchange(
        &ring->writeFrame,
        static_cast<LONG>(static_cast<ULONG>(writeFrame) + 1u));
    return true;
}

inline UINT32 PcmRingBufferWriteFrames(
    PcmRingBufferShared* ring,
    const FLOAT32* interleavedFrames,
    UINT32 frameCount,
    UINT32 inputChannels)
{
    if (interleavedFrames == nullptr || inputChannels == 0)
    {
        return 0;
    }

    UINT32 writtenFrames = 0;
    while (writtenFrames < frameCount)
    {
        const FLOAT32* frame = interleavedFrames +
            writtenFrames * inputChannels;

        if (!PcmRingBufferWriteFrame(ring, frame, inputChannels))
        {
            break;
        }

        ++writtenFrames;
    }

    return writtenFrames;
}
