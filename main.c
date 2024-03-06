#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define CINTERFACE
#define COBJMACROS
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <stdio.h>
#include <signal.h>
#include <math.h>

#pragma comment(lib, "winmm.lib")

#ifndef ARRSIZE
#define ARRSIZE(arr) (sizeof(arr) / sizeof(arr[0]))
#endif

#define my_assert(cond) (cond) ? (void)0 : __debugbreak()

#ifdef __cplusplus
#define WTF_IS_A_REFERENCE(obj) obj
#else
#define WTF_IS_A_REFERENCE(obj) &obj
#endif

///////////
// AUDIO //
///////////

struct
{
    // Devices
    IMMDeviceEnumerator* pIMMDeviceEnumerator;
    IMMDevice*           pIMMDevice;
    // Process
    IAudioClient*       pIAudioClient;
    IAudioRenderClient* pIAudioRenderClient;
    HANDLE              hAudioEvent;
    HANDLE              hAudioProcessThread;
    UINT32              FlagExitAudioThread;

    SIZE_T ProcessBufferCap;
    BYTE*  ProcessBuffer;
    UINT32 ProcessBufferMaxFrames;
    UINT32 ProcessBufferNumOverprocessedFrames;
    // Config
    UINT32 NumChannels;
    UINT32 SampleRate;
    UINT32 BlockSize;
} g_Audio;
// Pass a deviceIdx < 0 for default device
void Audio_SetDevice(int deviceIdx);
void Audio_Stop();
void Audio_Start();

static inline UINT64 RoundUp(UINT64 v, UINT64 align)
{
    UINT64 inc = (align - (v % align)) % align;
    return v + inc;
}

int         shouldExit = 0;
static void quit(int ignore)
{
    fprintf(stderr, "Shutting down\n");
    shouldExit = 1;
}

int main()
{
    // Init COM (WASAPI uses COM)
    if (FAILED(CoInitializeEx(0, COINIT_MULTITHREADED)))
    {
        fprintf(stderr, "Failed initialising COM\n");
        return 1;
    }

    // Using a sample rate of 44100 is very stuttery and I haven't figured out why yet
    // Using 48000 seems to work fine
    memset(&g_Audio, 0, sizeof(g_Audio));
    g_Audio.SampleRate  = 48000;
    g_Audio.BlockSize   = 512;
    g_Audio.NumChannels = 2;
    my_assert(g_Audio.NumChannels == 1 || g_Audio.NumChannels == 2);

    // Scan for device
    static const GUID _CLSID_MMDeviceEnumerator =
        {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
    static const GUID _IID_IMMDeviceEnumerator =
        {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
    HRESULT hr = CoCreateInstance(
        (REFCLSID)WTF_IS_A_REFERENCE(_CLSID_MMDeviceEnumerator),
        0,
        CLSCTX_ALL,
        (REFCLSID)WTF_IS_A_REFERENCE(_IID_IMMDeviceEnumerator),
        (void**)&g_Audio.pIMMDeviceEnumerator);
    my_assert(! FAILED(hr));

    Audio_SetDevice(-1); // -1 == default device
    Audio_Start();

    fprintf(stderr, "Quit with Ctrl-C.\n");
    signal(SIGINT, quit);
    while (shouldExit == 0)
    {
        Sleep(10);
    }

    // Shutdown audio
    if (g_Audio.hAudioEvent)
        Audio_Stop();
    my_assert(g_Audio.ProcessBuffer != NULL);
    VirtualFree(g_Audio.ProcessBuffer, g_Audio.ProcessBufferCap, 0);
    my_assert(g_Audio.pIMMDevice != NULL);
    g_Audio.pIMMDevice->lpVtbl->Release(g_Audio.pIMMDevice);
    my_assert(g_Audio.pIMMDeviceEnumerator != NULL);
    g_Audio.pIMMDeviceEnumerator->lpVtbl->Release(g_Audio.pIMMDeviceEnumerator);

    CoUninitialize();
    return 0;
}

#pragma region AUDIO

void process_noninterleaved(float** output)
{
    static const float pi = 3.14159265f;
    static float phase = 0.0f;

    const float freq = 500.0f;

    float inc = freq / g_Audio.SampleRate;

    for (int i = 0; i < g_Audio.BlockSize; i++)
    {
        float sample = sin(2 * pi * phase);
        for (int ch = 0; ch < g_Audio.NumChannels; ch++)
            output[ch][i] = sample;

        phase += inc;
        phase -= (int)phase;
    }
}

void Audio_Process(const UINT32 blockSize)
{
    BYTE*   outBuffer            = NULL;
    UINT32  remainingBlockFrames = blockSize;
    HRESULT hr = g_Audio.pIAudioRenderClient->lpVtbl->GetBuffer(g_Audio.pIAudioRenderClient, blockSize, &outBuffer);
    my_assert(outBuffer != NULL);
    if (FAILED(hr))
        return;

    if (g_Audio.ProcessBufferNumOverprocessedFrames)
    {
        // Our remaining samples are already in a deinterleaved format
        UINT32 framesToCopy = g_Audio.ProcessBufferNumOverprocessedFrames < remainingBlockFrames
                                  ? g_Audio.ProcessBufferNumOverprocessedFrames
                                  : remainingBlockFrames;
        SIZE_T bytesToCopy  = sizeof(float) * g_Audio.NumChannels * framesToCopy;
        memcpy(outBuffer, g_Audio.ProcessBuffer, bytesToCopy);

        remainingBlockFrames                        -= framesToCopy;
        g_Audio.ProcessBufferNumOverprocessedFrames -= framesToCopy;
        outBuffer                                   += bytesToCopy;
        my_assert(remainingBlockFrames < blockSize); // check overflow
    }

    SIZE_T processBufferOffset = sizeof(float) * g_Audio.NumChannels * g_Audio.ProcessBufferMaxFrames;
    processBufferOffset        = RoundUp(processBufferOffset, 32);
    float* output[2];
    output[0]              = (float*)(g_Audio.ProcessBuffer + processBufferOffset);
    output[1]              = output[0] + g_Audio.BlockSize;

    while (remainingBlockFrames > 0)
    {
        my_assert(g_Audio.ProcessBufferNumOverprocessedFrames == 0);

        process_noninterleaved(output);

        UINT32 framesToCopy = remainingBlockFrames < g_Audio.BlockSize ? remainingBlockFrames : g_Audio.BlockSize;
        SIZE_T bytesToCopy  = sizeof(float) * g_Audio.NumChannels * framesToCopy;

        int    i                 = 0;
        float* outputInterleaved = (float*)outBuffer;
        for (; i < framesToCopy; i++)
            for (int ch = 0; ch < g_Audio.NumChannels; ch++)
                *outputInterleaved++ = output[ch][i];

        float* remainingInterleaved = (float*)g_Audio.ProcessBuffer;
        for (; i < g_Audio.BlockSize; i++)
            for (int ch = 0; ch < g_Audio.NumChannels; ch++)
                *remainingInterleaved++ = output[ch][i];
        g_Audio.ProcessBufferNumOverprocessedFrames = g_Audio.BlockSize - framesToCopy;

        remainingBlockFrames -= framesToCopy;
        outBuffer            += bytesToCopy;

        my_assert(remainingBlockFrames < blockSize); // check overflow
    }

    // This has a scary name 'Release', however I don't think any resources are deallocated,
    // rather space within a preallocated block is marked reserved/unreserved
    // This is just how you hand the buffer back to windows
    g_Audio.pIAudioRenderClient->lpVtbl->ReleaseBuffer(g_Audio.pIAudioRenderClient, blockSize, 0);
}

DWORD WINAPI Audio_RunProcessThread(LPVOID data)
{
    // NOTE: requested sizes do not come in the size requested, or even in a multiple of 32
    // On my machine, requesting a block size of 512 at 44100Hz gives me a max frame size of 1032 and variable
    // block sizes, usually consisting of 441 frames.The windows docs say this to guarantee enough audio in reserve to
    // prevent audible glicthes:
    // https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize
    // Unfortunately for us, this means we need to play silly games caching audio within a preallocated buffer to
    // make sure our callback recieves a block size we requested
    Audio_Process(g_Audio.ProcessBufferMaxFrames);

    g_Audio.pIAudioClient->lpVtbl->Start(g_Audio.pIAudioClient);

    while (! g_Audio.FlagExitAudioThread)
    {
        WaitForSingleObject(g_Audio.hAudioEvent, INFINITE);

        UINT32  padding = 0;
        HRESULT hr      = g_Audio.pIAudioClient->lpVtbl->GetCurrentPadding(g_Audio.pIAudioClient, &padding);

        if (FAILED(hr))
            continue;

        my_assert(g_Audio.ProcessBufferMaxFrames >= padding);
        UINT32 blockSize = g_Audio.ProcessBufferMaxFrames - padding;
        if (blockSize == 0)
            continue;

        Audio_Process(blockSize);
    }

    return 0;
}

void Audio_Stop()
{
    if (g_Audio.hAudioProcessThread == NULL)
    {
        fprintf(stderr, "[WARNING] Called Audio_Stop() when audio is not running\n");
        return;
    }
    my_assert(g_Audio.FlagExitAudioThread == 0);
    g_Audio.FlagExitAudioThread = 1;
    my_assert(g_Audio.hAudioEvent);
    SetEvent(g_Audio.hAudioEvent);

    my_assert(g_Audio.hAudioProcessThread != NULL);
    WaitForSingleObject(g_Audio.hAudioProcessThread, INFINITE);
    CloseHandle(g_Audio.hAudioProcessThread);
    g_Audio.hAudioProcessThread = NULL;

    my_assert(g_Audio.pIAudioClient != NULL);
    g_Audio.pIAudioClient->lpVtbl->Stop(g_Audio.pIAudioClient);
    my_assert(g_Audio.pIAudioRenderClient != NULL);
    g_Audio.pIAudioRenderClient->lpVtbl->Release(g_Audio.pIAudioRenderClient);
    my_assert(g_Audio.pIAudioClient != NULL);
    g_Audio.pIAudioClient->lpVtbl->Release(g_Audio.pIAudioClient);
    g_Audio.pIAudioClient       = NULL;
    g_Audio.pIAudioRenderClient = NULL;

    my_assert(g_Audio.hAudioEvent != NULL);
    CloseHandle(g_Audio.hAudioEvent);
    g_Audio.hAudioEvent = NULL;
}

void Audio_SetDevice(int deviceIdx)
{
    my_assert(g_Audio.hAudioProcessThread == NULL);

    if (g_Audio.pIMMDevice != NULL)
        g_Audio.pIMMDevice->lpVtbl->Release(g_Audio.pIMMDevice);

    if (deviceIdx >= 0)
    {
        IMMDeviceCollection* pCollection = NULL;
        g_Audio.pIMMDeviceEnumerator->lpVtbl
            ->EnumAudioEndpoints(g_Audio.pIMMDeviceEnumerator, eRender, DEVICE_STATE_ACTIVE, &pCollection);
        my_assert(pCollection != NULL);

        UINT numDevices = 0;
        pCollection->lpVtbl->GetCount(pCollection, &numDevices);

        if (deviceIdx < numDevices)
            pCollection->lpVtbl->Item(pCollection, deviceIdx, &g_Audio.pIMMDevice);

        pCollection->lpVtbl->Release(pCollection);
    }

    if (g_Audio.pIMMDevice == NULL)
    {
        // Set default device
        // eConsole or eMultimedia? Microsoft say console is for games, multimedia for playing live music
        // https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-roles
        HRESULT hr = g_Audio.pIMMDeviceEnumerator->lpVtbl->GetDefaultAudioEndpoint(
            g_Audio.pIMMDeviceEnumerator,
            eRender,
            eMultimedia,
            &g_Audio.pIMMDevice);
        my_assert(! FAILED(hr));
    }
}

void Audio_Start()
{
    my_assert(g_Audio.SampleRate != 0);
    my_assert(g_Audio.BlockSize != 0);
    static const IID _IID_IAudioClient = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
    static const GUID _KSDATAFORMAT_SUBTYPE_IEEE_FLOAT =
        {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    static const IID _IID_IAudioRenderClient =
        {0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};

    my_assert(g_Audio.pIMMDevice != NULL);
    my_assert(g_Audio.pIAudioClient == NULL);
    HRESULT hr = g_Audio.pIMMDevice->lpVtbl->Activate(
        g_Audio.pIMMDevice,
        WTF_IS_A_REFERENCE(_IID_IAudioClient),
        CLSCTX_ALL,
        0,
        (void**)&g_Audio.pIAudioClient);
    my_assert(! FAILED(hr));

    // https://learn.microsoft.com/en-us/windows/win32/api/mmreg/ns-mmreg-waveformatextensible
    WAVEFORMATEXTENSIBLE fmtex;
    memset(&fmtex, 0, sizeof(fmtex));
    fmtex.Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
    fmtex.Format.nChannels            = g_Audio.NumChannels;
    fmtex.Format.nSamplesPerSec       = g_Audio.SampleRate;
    fmtex.Format.wBitsPerSample       = 32;
    fmtex.Format.nBlockAlign          = (fmtex.Format.nChannels * fmtex.Format.wBitsPerSample) / 8;
    fmtex.Format.nAvgBytesPerSec      = fmtex.Format.nSamplesPerSec * fmtex.Format.nBlockAlign;
    fmtex.Format.cbSize               = 22;
    fmtex.Samples.wValidBitsPerSample = 32;

    if (fmtex.Format.nChannels == 1)
        fmtex.dwChannelMask = SPEAKER_FRONT_CENTER;
    else
        fmtex.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

    fmtex.SubFormat = _KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    REFERENCE_TIME reftime = (double)g_Audio.BlockSize / ((double)g_Audio.SampleRate * 1.e-7);

    // https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize
    hr = g_Audio.pIAudioClient->lpVtbl->Initialize(
        g_Audio.pIAudioClient,
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        reftime,
        0,
        (WAVEFORMATEX*)&fmtex,
        0);
    my_assert(! FAILED(hr));

    hr = g_Audio.pIAudioClient->lpVtbl->GetBufferSize(g_Audio.pIAudioClient, &g_Audio.ProcessBufferMaxFrames);
    my_assert(! FAILED(hr));

    g_Audio.pIAudioClient->lpVtbl->GetService(
        g_Audio.pIAudioClient,
        WTF_IS_A_REFERENCE(_IID_IAudioRenderClient),
        (void**)&g_Audio.pIAudioRenderClient);

    my_assert(g_Audio.hAudioEvent == NULL);
    g_Audio.hAudioEvent = CreateEventA(0, 0, 0, 0);
    my_assert(g_Audio.hAudioEvent != NULL);
    g_Audio.pIAudioClient->lpVtbl->SetEventHandle(g_Audio.pIAudioClient, g_Audio.hAudioEvent);

    SIZE_T req_bytes_reserve    = sizeof(float) * g_Audio.NumChannels * g_Audio.ProcessBufferMaxFrames;
    SIZE_T req_bytes_processing = sizeof(float) * g_Audio.NumChannels * g_Audio.BlockSize;
    req_bytes_reserve           = RoundUp(req_bytes_reserve, 32);
    req_bytes_processing        = RoundUp(req_bytes_processing, 32);

    SIZE_T requiredCap = RoundUp(req_bytes_reserve + req_bytes_processing, 4096);
    if (requiredCap > g_Audio.ProcessBufferCap)
    {
        if (g_Audio.ProcessBuffer != NULL)
            VirtualFree(g_Audio.ProcessBuffer, g_Audio.ProcessBufferCap, 0);

        g_Audio.ProcessBufferCap = requiredCap;
        g_Audio.ProcessBuffer =
            (BYTE*)VirtualAlloc(NULL, g_Audio.ProcessBufferCap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        my_assert(g_Audio.ProcessBuffer != NULL);
    }

    g_Audio.ProcessBufferNumOverprocessedFrames = 0;
    g_Audio.FlagExitAudioThread                 = 0;

    g_Audio.hAudioProcessThread = CreateThread(NULL, 0, Audio_RunProcessThread, NULL, 0, 0);
    my_assert(g_Audio.hAudioProcessThread != NULL);
}
#pragma endregion AUDIO