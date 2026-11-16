#include "decoder.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <memory>
#include <new>

using Microsoft::WRL::ComPtr;

namespace {
HRESULT ReadAudio(const wchar_t* path, PcmBuffer& pcm) {
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(path, nullptr, &reader);
    const DWORD audio = MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    ComPtr<IMFMediaType> type;
    if (SUCCEEDED(hr)) hr = reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr)) hr = reader->SetStreamSelection(audio, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
    if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(audio, nullptr, type.Get());

    while (SUCCEEDED(hr)) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(audio, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr)) return hr;
        if (flags & MF_SOURCE_READERF_ERROR) return E_FAIL;
        if (sample) {
            ComPtr<IMFMediaBuffer> buffer;
            hr = sample->ConvertToContiguousBuffer(&buffer);
            if (FAILED(hr)) return hr;
            BYTE* data = nullptr;
            DWORD count = 0;
            hr = buffer->Lock(&data, nullptr, &count);
            if (FAILED(hr)) return hr;
            if (count % 4) hr = MF_E_INVALIDMEDIATYPE;
            else if (count) {
                const short* first = reinterpret_cast<const short*>(data);
                try {
                    pcm.samples.insert(pcm.samples.end(), first, first + count / sizeof(short));
                } catch (const std::bad_alloc&) {
                    hr = E_OUTOFMEMORY;
                }
            }
            buffer->Unlock();
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    if (SUCCEEDED(hr) && pcm.samples.empty()) return MF_E_END_OF_STREAM;
    return hr;
}
}

bool DecodeLocalMusic(const std::wstring& path, PcmBuffer& pcm, std::string& err) {
    pcm.clear();
    err.clear();
    // Keep the bundled Vorbis path available even without Windows codec extensions.
    const wchar_t* ext = wcsrchr(path.c_str(), L'.');
    if (ext && !_wcsicmp(ext, L".ogg")) {
        std::unique_ptr<FILE, decltype(&fclose)> file(_wfopen(path.c_str(), L"rb"), fclose);
        if (file && !fseek(file.get(), 0, SEEK_END)) {
            long size = ftell(file.get());
            rewind(file.get());
            if (size > 0) {
                std::vector<unsigned char> bytes(size);
                if (fread(bytes.data(), 1, size, file.get()) == (size_t)size &&
                    DecodeOgg(bytes.data(), size, pcm, err, 0)) return true;
                pcm.clear();
            }
        }
    }
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr = (init == RPC_E_CHANGED_MODE) ? S_OK : init;
    if (SUCCEEDED(hr)) hr = MFStartup(MF_VERSION);
    if (SUCCEEDED(hr)) {
        hr = ReadAudio(path.c_str(), pcm);
        MFShutdown();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    if (FAILED(hr)) {
        char message[100];
        snprintf(message, sizeof(message), "Windows could not decode this audio file (0x%08lX).", (unsigned long)hr);
        err = message;
        pcm.clear();
        return false;
    }
    return true;
}
