/* -LICENSE-START-
** Copyright (c) 2013 Blackmagic Design
**
** Permission is hereby granted, free of charge, to any person or organization
** obtaining a copy of the software and accompanying documentation covered by
** this license (the "Software") to use, reproduce, display, distribute,
** execute, and transmit the Software, and to prepare derivative works of the
** Software, and to permit third-parties to whom the Software is furnished to
** do so, all subject to the following:
**
** The copyright notices in the Software and this entire statement, including
** the above license grant, this restriction and the following disclaimer,
** must be included in all copies of the Software, in whole or in part, and
** all derivative works of the Software, unless such copies or derivative
** works are solely in the form of machine-executable object code generated by
** a source language processor.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
** -LICENSE-END-
*/
#include "stdafx.h"

#if USE_DECKLINK || USE_DECKLINK_SHUTTLE

#include <comutil.h>
#include "DeckLinkDevice.h"

using namespace std;

DeckLinkDevice::DeckLinkDevice(IDeckLink* device) :
    m_deckLink(device),
    m_deckLinkInput(NULL),
    m_deckLinkOutput(NULL),
    m_supportsFormatDetection(false),
    m_refCount(1),
    m_currentlyCapturing(false),
    m_playbackTimeScale(600)
{
    for (int i = 0; i < MAX_NUM_CACHED_BUFFERS; i++)
    {
        bufferCache[i].buffer = new BYTE[FRAME_BUFSIZE];
        bufferCache[i].timeStamp = 0;
    }

    captureFrameIndex = 0;

    if (m_deckLink != NULL)
    {
        m_deckLink->AddRef();
    }

    InitializeCriticalSection(&m_captureCardCriticalSection);
    InitializeCriticalSection(&m_frameAccessCriticalSection);
    InitializeCriticalSection(&m_outputCriticalSection);
}

DeckLinkDevice::~DeckLinkDevice()
{
    StopCapture();

    if (m_deckLinkInput != NULL)
    {
        m_deckLinkInput->Release();
        m_deckLinkInput = NULL;
    }

    if (supportsOutput && m_deckLinkOutput != NULL)
    {
        m_deckLinkOutput->Release();
        m_deckLinkOutput = NULL;
    }

    if (m_deckLink != NULL)
    {
        m_deckLink->Release();
        m_deckLink = NULL;
    }

    DeleteCriticalSection(&m_captureCardCriticalSection);
    DeleteCriticalSection(&m_outputCriticalSection);
    DeleteCriticalSection(&m_frameAccessCriticalSection);

    for (int i = 0; i < MAX_NUM_CACHED_BUFFERS; i++)
    {
        delete[] bufferCache[i].buffer;
    }

    delete[] latestBuffer;
    delete[] outputBuffer;
}

HRESULT    STDMETHODCALLTYPE DeckLinkDevice::QueryInterface(REFIID iid, LPVOID *ppv)
{
    HRESULT result = E_NOINTERFACE;

    if (ppv == NULL)
    {
        return E_INVALIDARG;
    }

    // Initialise the return result
    *ppv = NULL;

    // Obtain the IUnknown interface and compare it the provided REFIID
    if (iid == IID_IUnknown)
    {
        *ppv = this;
        AddRef();
        result = S_OK;
    }
    else if (iid == IID_IDeckLinkInputCallback)
    {
        *ppv = (IDeckLinkInputCallback*)this;
        AddRef();
        result = S_OK;
    }
    else if (iid == IID_IDeckLinkNotificationCallback)
    {
        *ppv = (IDeckLinkNotificationCallback*)this;
        AddRef();
        result = S_OK;
    }

    return result;
}

ULONG STDMETHODCALLTYPE DeckLinkDevice::AddRef(void)
{
    return InterlockedIncrement((LONG*)&m_refCount);
}

ULONG STDMETHODCALLTYPE DeckLinkDevice::Release(void)
{
    int newRefValue;

    newRefValue = InterlockedDecrement((LONG*)&m_refCount);
    if (newRefValue == 0)
    {
        delete this;
        return 0;
    }

    return newRefValue;
}

bool DeckLinkDevice::Init(ID3D11ShaderResourceView* colorSRV)
{
    IDeckLinkAttributes*            deckLinkAttributes = NULL;
    IDeckLinkDisplayModeIterator*   displayModeIterator = NULL;
    IDeckLinkDisplayMode*           displayMode = NULL;
    BSTR                            deviceNameBSTR = NULL;

    ZeroMemory(rawBuffer, FRAME_BUFSIZE_RAW);
    ZeroMemory(latestBuffer, FRAME_BUFSIZE);
    ZeroMemory(outputBuffer, FRAME_BUFSIZE);

    for (int i = 0; i < MAX_NUM_CACHED_BUFFERS; i++)
    {
        ZeroMemory(bufferCache[i].buffer, FRAME_BUFSIZE);
    }

    captureFrameIndex = 0;

    _colorSRV = colorSRV;

    if (colorSRV != nullptr)
    {
        colorSRV->GetDevice(&device);
    }

    // Get input interface
    if (m_deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&m_deckLinkInput) != S_OK)
    {
        return false;
    }

    if (m_deckLink->QueryInterface(IID_IDeckLinkOutput, (void**)&m_deckLinkOutput) != S_OK)
    {
        supportsOutput = false;
    }

    // Check if input mode detection is supported.
    if (m_deckLink->QueryInterface(IID_IDeckLinkAttributes, (void**)&deckLinkAttributes) == S_OK)
    {
        if (deckLinkAttributes->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &m_supportsFormatDetection) != S_OK)
        {
            m_supportsFormatDetection = false;
        }

        deckLinkAttributes->Release();
    }

    // Set your camera to output in 1080p (This may be 24Hz instead of 60, depending on your camera)
    // 1080i output causes horizontal artifacts on screen.
    if (m_deckLinkOutput != NULL)
    {
        m_deckLinkOutput->CreateVideoFrame(FRAME_WIDTH, FRAME_HEIGHT, FRAME_WIDTH * FRAME_BPP, bmdFormat8BitBGRA, bmdFrameFlagDefault, &outputFrame);
        outputFrame->GetBytes((void**)&outputBuffer);
    }

    return true;
}

bool DeckLinkDevice::StartCapture(BMDDisplayMode videoDisplayMode)
{
    if (m_deckLinkInput == NULL)
    {
        return false;
    }

    OutputDebugString(L"Start Capture.\n");
    BMDVideoInputFlags videoInputFlags = bmdVideoInputFlagDefault;
    BMDVideoOutputFlags videoOutputFlags = bmdVideoOutputFlagDefault;

    // Enable input video mode detection if the device supports it
    if (m_supportsFormatDetection == TRUE)
    {
        videoInputFlags |= bmdVideoInputEnableFormatDetection;
    }

    // Set capture callback
    m_deckLinkInput->SetCallback(this);

    // Set the video input mode
    if (m_deckLinkInput->EnableVideoInput(videoDisplayMode, bmdFormat8BitYUV, videoInputFlags) != S_OK)
    {
        OutputDebugString(L"Unable to set the chosen video mode.\n");
        return false;
    }

    if (supportsOutput && m_deckLinkOutput != NULL && m_deckLinkOutput->EnableVideoOutput(videoDisplayMode, videoOutputFlags) != S_OK)
    {
        OutputDebugString(L"Unable to set video output.\n");
        supportsOutput = false;
    }

    if (supportsOutput && m_deckLinkOutput != NULL && m_deckLinkOutput->StartScheduledPlayback(0, m_playbackTimeScale, 1.0) != S_OK)
    {
        OutputDebugString(L"Unable to start output playback.\n");
        supportsOutput = false;
    }

    // Start the capture
    if (m_deckLinkInput->StartStreams() != S_OK)
    {
        OutputDebugString(L"Unable to start capture.\n");
        return false;
    }

    m_currentlyCapturing = true;
    captureFrameIndex = 0;

    return true;
}

void DeckLinkDevice::StopCapture()
{
    EnterCriticalSection(&m_captureCardCriticalSection);

    OutputDebugString(L"Stop Capture.\n");

    if (m_deckLinkInput != NULL)
    {
        // Stop the capture
        m_deckLinkInput->StopStreams();

        // Delete capture callback
        m_deckLinkInput->SetCallback(NULL);
    }

    if (supportsOutput && m_deckLinkOutput != NULL)
    {
        m_deckLinkOutput->StopScheduledPlayback(0, NULL, 0);
        m_deckLinkOutput->DisableVideoOutput();
    }

    m_currentlyCapturing = false;
    LeaveCriticalSection(&m_captureCardCriticalSection);
}

HRESULT DeckLinkDevice::VideoInputFormatChanged(/* in */ BMDVideoInputFormatChangedEvents notificationEvents, /* in */ IDeckLinkDisplayMode *newMode, /* in */ BMDDetectedVideoInputFormatFlags detectedSignalFlags)
{
    EnterCriticalSection(&m_captureCardCriticalSection);
    EnterCriticalSection(&m_outputCriticalSection);

    OutputDebugString(L"Changing Formats to: ");
    OutputDebugString(std::to_wstring(newMode->GetDisplayMode()).c_str());
    OutputDebugString(L"\n");

    // If we do not have the correct dimension frames - loop until user changes camera settings.
    if (newMode->GetWidth() != FRAME_WIDTH || newMode->GetHeight() != FRAME_HEIGHT)
    {
        OutputDebugString(L"Invalid frame dimensions detected.\n");
        OutputDebugString(L"Actual Frame Dimensions: ");
        OutputDebugString(std::to_wstring(newMode->GetWidth()).c_str());
        OutputDebugString(L", ");
        OutputDebugString(std::to_wstring(newMode->GetHeight()).c_str());
        OutputDebugString(L"\n");

        OutputDebugString(L"Expected Frame Dimensions: ");
        OutputDebugString(std::to_wstring(FRAME_WIDTH).c_str());
        OutputDebugString(L", ");
        OutputDebugString(std::to_wstring(FRAME_HEIGHT).c_str());
        OutputDebugString(L"\n");

        LeaveCriticalSection(&m_captureCardCriticalSection);
        LeaveCriticalSection(&m_outputCriticalSection);
        return E_PENDING;
    }

    pixelFormat = PixelFormat::YUV;
    BMDPixelFormat bmdPixelFormat = bmdFormat8BitYUV;

    if ((detectedSignalFlags & bmdDetectedVideoInputRGB444) != 0)
    {
        pixelFormat = PixelFormat::BGRA;
        bmdPixelFormat = bmdFormat8BitBGRA;
    }

    // Stop the capture
    m_currentlyCapturing = false;

    m_deckLinkInput->StopStreams();
    m_deckLinkInput->FlushStreams();

    if (supportsOutput && m_deckLinkOutput != NULL)
    {
        m_deckLinkOutput->StopScheduledPlayback(0, NULL, 0);
        m_deckLinkOutput->DisableVideoOutput();
    }

    // Set the video input mode
    if (m_deckLinkInput->EnableVideoInput(newMode->GetDisplayMode(), bmdPixelFormat, bmdVideoInputEnableFormatDetection) != S_OK)
    {
        OutputDebugString(L"Could not enable video input when restarting capture with detected input.\n");
        goto bail;
    }

    // Start the capture
    if (m_deckLinkInput->StartStreams() != S_OK)
    {
        OutputDebugString(L"Could not start streams when restarting capture with detected input.\n");
        goto bail;
    }

    if (m_deckLinkOutput != NULL && m_deckLinkOutput->EnableVideoOutput(newMode->GetDisplayMode(), bmdVideoOutputFlagDefault) != S_OK)
    {
        OutputDebugString(L"Unable to set video output.\n");
        supportsOutput = false;
    }

    m_currentlyCapturing = true;

bail:
    OutputDebugString(L"Done changing formats.\n");

    LeaveCriticalSection(&m_captureCardCriticalSection);
    LeaveCriticalSection(&m_outputCriticalSection);
    return S_OK;
}

HRESULT DeckLinkDevice::VideoInputFrameArrived(/* in */ IDeckLinkVideoInputFrame* frame, /* in */ IDeckLinkAudioInputPacket* audioPacket)
{
    if (frame == nullptr)
    {
        return S_OK;
    }

    LARGE_INTEGER time;
    QueryPerformanceCounter(&time);

    BMDPixelFormat framePixelFormat = frame->GetPixelFormat();

    EnterCriticalSection(&m_captureCardCriticalSection);

    if (frame->GetBytes((void**)&localFrameBuffer) == S_OK)
    {
        captureFrameIndex++;
        BYTE* buffer = bufferCache[captureFrameIndex % MAX_NUM_CACHED_BUFFERS].buffer;

        memcpy(buffer, localFrameBuffer, frame->GetRowBytes() * frame->GetHeight());
    }
    

    LONGLONG t;
    frame->GetStreamTime(&t, &frameDuration, S2HNS);

    
    // Get frame time.
    bufferCache[captureFrameIndex % MAX_NUM_CACHED_BUFFERS].timeStamp = t;

    dirtyFrame = false;

    if (supportsOutput && m_deckLinkOutput != NULL && outputFrame != NULL)
    {
        EnterCriticalSection(&m_outputCriticalSection);
        m_deckLinkOutput->DisplayVideoFrameSync(outputFrame);
        LeaveCriticalSection(&m_outputCriticalSection);
    }

    LeaveCriticalSection(&m_captureCardCriticalSection);

    return S_OK;
}

void DeckLinkDevice::Update(int compositeFrameIndex)
{
    if (_colorSRV != nullptr &&
        device != nullptr)
    {
        const BufferCache& buffer = bufferCache[compositeFrameIndex % MAX_NUM_CACHED_BUFFERS];
        if (buffer.buffer != nullptr)
        {
            EnterCriticalSection(&m_captureCardCriticalSection);
            DirectXHelper::UpdateSRV(device, _colorSRV, buffer.buffer, FRAME_WIDTH * FRAME_BPP);
            LeaveCriticalSection(&m_captureCardCriticalSection);
        }

        if (supportsOutput && device != nullptr && _outputTexture != nullptr)
        {
            EnterCriticalSection(&m_outputCriticalSection);
            if (outputTextureBuffer.IsDataAvailable())
            {
                outputTextureBuffer.FetchTextureData(device, outputBuffer, FRAME_BPP);
            }
            outputTextureBuffer.PrepareTextureFetch(device, _outputTexture);
            LeaveCriticalSection(&m_outputCriticalSection);
        }
    }
}

bool DeckLinkDevice::OutputYUV()
{
    return (pixelFormat == PixelFormat::YUV);
}

DeckLinkDeviceDiscovery::DeckLinkDeviceDiscovery()
    : m_deckLinkDiscovery(NULL), m_refCount(1)
{
    if (CoCreateInstance(CLSID_CDeckLinkDiscovery, NULL, CLSCTX_ALL, IID_IDeckLinkDiscovery, (void**)&m_deckLinkDiscovery) != S_OK)
    {
        m_deckLinkDiscovery = NULL;
    }
}

DeckLinkDeviceDiscovery::~DeckLinkDeviceDiscovery()
{
    if (m_deckLinkDiscovery != NULL)
    {
        // Uninstall device arrival notifications and release discovery object
        m_deckLinkDiscovery->UninstallDeviceNotifications();
        m_deckLinkDiscovery->Release();
        m_deckLinkDiscovery = NULL;
    }

    if (m_deckLink != nullptr)
    {
        m_deckLink->Release();
        m_deckLink = NULL;
    }
}

bool DeckLinkDeviceDiscovery::Enable()
{
    HRESULT result = E_FAIL;

    // Install device arrival notifications
    if (m_deckLinkDiscovery != NULL)
    {
        result = m_deckLinkDiscovery->InstallDeviceNotifications(this);
    }

    return result == S_OK;
}

void DeckLinkDeviceDiscovery::Disable()
{
    // Uninstall device arrival notifications
    if (m_deckLinkDiscovery != NULL)
    {
        m_deckLinkDiscovery->UninstallDeviceNotifications();
    }
}

HRESULT DeckLinkDeviceDiscovery::DeckLinkDeviceArrived(/* in */ IDeckLink* deckLink)
{
    if (m_deckLink == nullptr)
    {
        deckLink->AddRef();

        m_deckLink = deckLink;
    }

    return S_OK;
}

HRESULT DeckLinkDeviceDiscovery::DeckLinkDeviceRemoved(/* in */ IDeckLink* deckLink)
{
    deckLink->Release();
    return S_OK;
}

HRESULT    STDMETHODCALLTYPE DeckLinkDeviceDiscovery::QueryInterface(REFIID iid, LPVOID *ppv)
{
    HRESULT result = E_NOINTERFACE;

    if (ppv == NULL)
    {
        return E_INVALIDARG;
    }

    // Initialise the return result
    *ppv = NULL;

    // Obtain the IUnknown interface and compare it the provided REFIID
    if (iid == IID_IUnknown)
    {
        *ppv = this;
        AddRef();
        result = S_OK;
    }
    else if (iid == IID_IDeckLinkDeviceNotificationCallback)
    {
        *ppv = (IDeckLinkDeviceNotificationCallback*)this;
        AddRef();
        result = S_OK;
    }

    return result;
}

ULONG STDMETHODCALLTYPE DeckLinkDeviceDiscovery::AddRef(void)
{
    return InterlockedIncrement((LONG*)&m_refCount);
}

ULONG STDMETHODCALLTYPE DeckLinkDeviceDiscovery::Release(void)
{
    ULONG newRefValue;

    newRefValue = InterlockedDecrement((LONG*)&m_refCount);
    if (newRefValue == 0)
    {
        delete this;
        return 0;
    }

    return newRefValue;
}

#endif
