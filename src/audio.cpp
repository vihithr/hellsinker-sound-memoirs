#include "audio.h"

#include <mmsystem.h>

namespace {
const int kBufferCount = 4;
const int kBufferFrames = 2048; // ~46 ms per buffer, ~186 ms total latency
const char* kMciAlias = "hsbgm_mci";

int ClampVol(int v) {
    if (v < 0) return 0;
    if (v > 100) return 100;
    return v;
}

bool Mci(const std::string& cmd, std::string* out) {
    char buf[512];
    buf[0] = 0;
    MCIERROR rc = mciSendStringA(cmd.c_str(), buf, sizeof(buf), NULL);
    if (out) *out = buf;
    return rc == 0;
}

} // namespace

AudioEngine::AudioEngine()
    : hwo_(NULL), pcm_(NULL), totalFrames_(0), cursor_(0), playing_(0),
      wakeEvent_(NULL), feedThread_(NULL), threadRun_(0),
      volume_(80), hwVolume_(false), inited_(false),
      mciActive_(false), mciPlaying_(false), mciPos_(0), mciLen_(0) {
    memset(&fmt_, 0, sizeof(fmt_));
    memset(hdrs_, 0, sizeof(hdrs_));
    for (int i = 0; i < kBufferCount; ++i) bufs_[i] = NULL;
    InitializeCriticalSection(&cs_);
}

AudioEngine::~AudioEngine() {
    shutdown();
    DeleteCriticalSection(&cs_);
}

bool AudioEngine::init(std::string& err) {
    err.clear();
    if (inited_) return true;
    DbgLog("audio.init: enter");

    fmt_.wFormatTag = WAVE_FORMAT_PCM;
    fmt_.nChannels = 2;
    fmt_.nSamplesPerSec = 44100;
    fmt_.wBitsPerSample = 16;
    fmt_.nBlockAlign = (WORD)(fmt_.nChannels * fmt_.wBitsPerSample / 8);
    fmt_.nAvgBytesPerSec = fmt_.nSamplesPerSec * fmt_.nBlockAlign;
    fmt_.cbSize = 0;

    MMRESULT rc = waveOutOpen(&hwo_, WAVE_MAPPER, &fmt_, (DWORD_PTR)&AudioEngine::WaveProc,
                              (DWORD_PTR)this, CALLBACK_FUNCTION);
    if (rc != MMSYSERR_NOERROR) {
        hwo_ = NULL;
        err = "no audio output device (waveOutOpen code " + IntToStr(rc) + ")";
        return false;
    }

    for (int i = 0; i < kBufferCount; ++i) {
        bufs_[i] = (short*)malloc((size_t)kBufferFrames * 4);
        if (!bufs_[i]) {
            freeBuffers();
            waveOutClose(hwo_);
            hwo_ = NULL;
            err = "out of memory for audio buffers";
            return false;
        }
        memset(bufs_[i], 0, (size_t)kBufferFrames * 4);
        memset(&hdrs_[i], 0, sizeof(WAVEHDR));
        hdrs_[i].lpData = (LPSTR)bufs_[i];
        hdrs_[i].dwBufferLength = (DWORD)(kBufferFrames * 4);
        hdrs_[i].dwUser = (DWORD_PTR)i;
    }

    wakeEvent_ = CreateEventA(NULL, FALSE, FALSE, NULL);
    threadRun_ = 1;
    DWORD tid = 0;
    feedThread_ = CreateThread(NULL, 0, &AudioEngine::FeedProc, this, 0, &tid);
    if (!wakeEvent_ || !feedThread_) {
        if (feedThread_) {
            threadRun_ = 0;
            SetEvent(wakeEvent_);
            WaitForSingleObject(feedThread_, 1000);
            CloseHandle(feedThread_);
            feedThread_ = NULL;
        }
        if (wakeEvent_) {
            CloseHandle(wakeEvent_);
            wakeEvent_ = NULL;
        }
        freeBuffers();
        waveOutClose(hwo_);
        hwo_ = NULL;
        err = "could not start audio feeder thread";
        return false;
    }

    WAVEOUTCAPSA caps;
    memset(&caps, 0, sizeof(caps));
    hwVolume_ = (waveOutGetDevCapsA((UINT_PTR)WAVE_MAPPER, &caps, sizeof(caps)) == MMSYSERR_NOERROR) &&
                ((caps.dwSupport & WAVECAPS_VOLUME) != 0);
    inited_ = true;
    setVolume(volume_);
    DbgLog("audio.init: ok hwVolume=%d", (int)hwVolume_);
    return true;
}

void AudioEngine::freeBuffers() {
    for (int i = 0; i < kBufferCount; ++i) {
        if (bufs_[i]) {
            free(bufs_[i]);
            bufs_[i] = NULL;
        }
        hdrs_[i].lpData = NULL;
    }
}

void AudioEngine::shutdown() {
    stop();
    if (feedThread_) {
        threadRun_ = 0;
        SetEvent(wakeEvent_);
        WaitForSingleObject(feedThread_, 2000);
        CloseHandle(feedThread_);
        feedThread_ = NULL;
    }
    if (wakeEvent_) {
        CloseHandle(wakeEvent_);
        wakeEvent_ = NULL;
    }
    if (hwo_) {
        waveOutReset(hwo_);
        unprepareAll();
        waveOutClose(hwo_);
        hwo_ = NULL;
    }
    freeBuffers();
    delete pcm_;
    pcm_ = NULL;
    inited_ = false;
}

void AudioEngine::unprepareAll() {
    if (!hwo_) return;
    for (int i = 0; i < kBufferCount; ++i) {
        if (hdrs_[i].dwFlags & WHDR_PREPARED) {
            waveOutUnprepareHeader(hwo_, &hdrs_[i], sizeof(WAVEHDR));
        }
        hdrs_[i].dwFlags = 0;
    }
}

bool AudioEngine::playPcm(PcmBuffer& pcm, std::string& err) {
    err.clear();
    stop();
    if (!inited_ || !hwo_) {
        err = "audio output not initialized";
        return false;
    }
    if (pcm.frames() <= 0) {
        err = "empty PCM buffer";
        return false;
    }
    if (!pcm_) pcm_ = new std::vector<short>();
    pcm_->clear();
    pcm_->swap(pcm.samples);
    pcm.clear();
    pcm.sampleRate = 44100;

    totalFrames_ = pcm_->size() / 2;
    cursor_ = 0;

    EnterCriticalSection(&cs_);
    DbgLog("playPcm: cs entered, preparing %d buffers", kBufferCount);
    unprepareAll();
    InterlockedExchange(&playing_, 1);
    for (int i = 0; i < kBufferCount; ++i) {
        memset(&hdrs_[i], 0, sizeof(WAVEHDR));
        hdrs_[i].lpData = (LPSTR)bufs_[i];
        hdrs_[i].dwBufferLength = (DWORD)(kBufferFrames * 4);
        hdrs_[i].dwUser = (DWORD_PTR)i;
        fillBuffer(i);
        MMRESULT rc = waveOutPrepareHeader(hwo_, &hdrs_[i], sizeof(WAVEHDR));
        DbgLog("playPcm: buf %d prepare rc=%ld", i, (long)rc);
        if (rc == MMSYSERR_NOERROR) rc = waveOutWrite(hwo_, &hdrs_[i], sizeof(WAVEHDR));
        DbgLog("playPcm: buf %d write rc=%ld", i, (long)rc);
        if (rc != MMSYSERR_NOERROR) {
            InterlockedExchange(&playing_, 0);
            LeaveCriticalSection(&cs_);
            waveOutReset(hwo_);
            unprepareAll();
            err = "waveOutWrite failed (code " + IntToStr((long long)rc) + ")";
            return false;
        }
    }
    LeaveCriticalSection(&cs_);
    return true;
}

void AudioEngine::fillBuffer(int idx) {
    if (!pcm_ || totalFrames_ == 0 || !bufs_[idx]) return;
    short* out = bufs_[idx];
    const std::vector<short>& src = *pcm_;
    const int gain = hwVolume_ ? 100 : volume_;
    size_t pos = cursor_;
    for (int i = 0; i < kBufferFrames; ++i) {
        size_t f = pos % totalFrames_;
        short l = src[f * 2 + 0];
        short r = src[f * 2 + 1];
        if (gain != 100) {
            l = (short)((int)l * gain / 100);
            r = (short)((int)r * gain / 100);
        }
        out[i * 2 + 0] = l;
        out[i * 2 + 1] = r;
        ++pos;
    }
    hdrs_[idx].dwBufferLength = (DWORD)(kBufferFrames * 4);
    cursor_ = pos % totalFrames_;
}

DWORD WINAPI AudioEngine::FeedProc(LPVOID param) {
    AudioEngine* self = (AudioEngine*)param;
    if (self) self->feedLoop();
    return 0;
}

void AudioEngine::feedLoop() {
    while (threadRun_) {
        WaitForSingleObject(wakeEvent_, 50);
        if (!threadRun_) break;
        EnterCriticalSection(&cs_);
        if (playing_ && hwo_ && pcm_ && totalFrames_ > 0) {
            for (int i = 0; i < kBufferCount; ++i) {
                if (!(hdrs_[i].dwFlags & WHDR_PREPARED)) continue;
                if (!(hdrs_[i].dwFlags & WHDR_DONE)) continue;
                fillBuffer(i);
                MMRESULT rc = waveOutWrite(hwo_, &hdrs_[i], sizeof(WAVEHDR));
                if (rc != MMSYSERR_NOERROR) {
                    DbgLog("feed: waveOutWrite failed rc=%ld", (long)rc);
                }
            }
        }
        LeaveCriticalSection(&cs_);
    }
}

void CALLBACK AudioEngine::WaveProc(HWAVEOUT /*hwo*/, UINT msg, DWORD_PTR inst,
                                    DWORD_PTR /*p1*/, DWORD_PTR /*p2*/) {
    if (msg != WOM_DONE) return;
    AudioEngine* self = (AudioEngine*)inst;
    if (!self) return;
    // Never refill or call waveOutWrite from here: some drivers deadlock when a
    // callback issues waveOutWrite while another thread is in waveOutGetPosition.
    if (self->wakeEvent_) SetEvent(self->wakeEvent_);
}

void AudioEngine::stop() {
    if (hwo_) {
        EnterCriticalSection(&cs_);
        InterlockedExchange(&playing_, 0);
        LeaveCriticalSection(&cs_);
        waveOutReset(hwo_);
        unprepareAll();
    }
    if (pcm_) pcm_->clear();
    totalFrames_ = 0;
    cursor_ = 0;
    if (mciActive_) stopMciLocked();
}

bool AudioEngine::isPlaying() const {
    if (mciActive_) return mciPlaying_;
    return playing_ != 0;
}

void AudioEngine::setVolume(int v) {
    volume_ = ClampVol(v);
    if (hwo_ && hwVolume_) {
        DWORD d = (DWORD)((volume_ * 0xFFFF) / 100);
        if (waveOutSetVolume(hwo_, d | (d << 16)) != MMSYSERR_NOERROR) {
            hwVolume_ = false; // fall back to software scaling on future buffers
        }
    }
    if (mciActive_) {
        Mci(std::string("setaudio ") + kMciAlias + " volume to " + IntToStr((long long)volume_ * 10), NULL);
    }
}

double AudioEngine::positionSeconds() const {
    if (mciActive_) return mciPos_;
    if (!hwo_ || !playing_ || totalFrames_ == 0) return 0.0;
    MMTIME mm;
    memset(&mm, 0, sizeof(mm));
    mm.wType = TIME_BYTES;
    if (waveOutGetPosition(hwo_, &mm, sizeof(mm)) != MMSYSERR_NOERROR) return 0.0;
    if (mm.wType != TIME_BYTES) return 0.0;
    // TIME_BYTES is unambiguous across drivers; 4 bytes = 1 stereo 16-bit frame.
    unsigned long long frames = mm.u.cb / 4;
    frames %= (unsigned long long)totalFrames_;
    return (double)frames / 44100.0;
}

double AudioEngine::durationSeconds() const {
    if (mciActive_) return mciLen_;
    return (double)totalFrames_ / 44100.0;
}

bool AudioEngine::playMci(const std::string& path, std::string& err) {
    err.clear();
    stop();
    mciActive_ = false;
    std::string out;
    Mci(std::string("close ") + kMciAlias, NULL);

    std::string cmd = "open \"" + path + "\" type mpegvideo alias " + kMciAlias;
    if (!Mci(cmd, &out)) {
        cmd = "open \"" + path + "\" alias " + kMciAlias;
        if (!Mci(cmd, &out)) {
            err = "MCI could not open mp3: " + out;
            return false;
        }
    }
    Mci(std::string("set ") + kMciAlias + " time format milliseconds", NULL);
    Mci(std::string("setaudio ") + kMciAlias + " volume to " + IntToStr((long long)volume_ * 10), NULL);

    long long lenMs = 0;
    std::string s;
    if (Mci(std::string("status ") + kMciAlias + " length", &s)) lenMs = atoll(s.c_str());
    if (lenMs <= 0) {
        Mci(std::string("close ") + kMciAlias, NULL);
        err = "MCI reported zero length for mp3";
        return false;
    }
    mciLen_ = (double)lenMs / 1000.0;
    mciPos_ = 0.0;
    if (!Mci(std::string("play ") + kMciAlias + " from 0", &out)) {
        Mci(std::string("close ") + kMciAlias, NULL);
        err = "MCI play failed: " + out;
        return false;
    }
    mciActive_ = true;
    mciPlaying_ = true;
    return true;
}

void AudioEngine::stopMciLocked() {
    if (!mciActive_) return;
    Mci(std::string("stop ") + kMciAlias, NULL);
    Mci(std::string("close ") + kMciAlias, NULL);
    mciActive_ = false;
    mciPlaying_ = false;
    mciPos_ = 0.0;
    mciLen_ = 0.0;
}

void AudioEngine::poll() {
    if (!mciActive_) return;
    std::string s;
    if (Mci(std::string("status ") + kMciAlias + " position", &s)) {
        mciPos_ = (double)atoll(s.c_str()) / 1000.0;
    }
    if (!mciPlaying_) return;
    if (mciLen_ > 0 && mciPos_ + 0.25 >= mciLen_) {
        Mci(std::string("seek ") + kMciAlias + " to start", NULL);
        Mci(std::string("play ") + kMciAlias, NULL);
        mciPos_ = 0.0;
    }
}
