// audio.h - waveOut streaming playback with transport controls, plus an MCI
// fallback used only for loose .mp3 tracks (no mp3 decoder is bundled).
#pragma once

#include "common.h"
#include "decoder.h"

#include <mmsystem.h>

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    bool init(std::string& err);   // opens the waveOut device (44.1k/16/2)
    void shutdown();

    // Takes ownership of the decoded samples (swapped in) and starts playback.
    bool playPcm(PcmBuffer& pcm, std::string& err);
    // Best-effort playback for loose .mp3 fallback tracks.
    bool playMci(const std::string& path, std::string& err);

    void stop();
    bool isPlaying() const;
    bool isMci() const { return mciActive_; }

    void setVolume(int v); // 0..100
    int volume() const { return volume_; }

    double positionSeconds() const;
    double durationSeconds();

    void togglePause();
    void seek(double seconds);
    bool isPaused();
    void setLoop(bool loop);
    bool finished();

    // Pump for the MCI loop; call from the UI timer.
    void poll();

private:
    static void CALLBACK WaveProc(HWAVEOUT hwo, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2);
    static DWORD WINAPI FeedProc(LPVOID param);
    void feedLoop();
    size_t fillBuffer(int idx);
    bool startPcmLocked(size_t frame, bool keepPaused);
    void resetWaveOutLocked();
    void finishPcmLocked();
    void unprepareAll();
    void freeBuffers();
    void stopMciLocked();

    HWAVEOUT hwo_;
    WAVEFORMATEX fmt_;
    WAVEHDR hdrs_[4];
    short* bufs_[4];
    std::vector<short>* pcm_;   // interleaved stereo
    size_t totalFrames_;
    size_t cursor_;
    size_t positionBaseFrames_;
    bool hdrQueued_[4];
    int queuedBuffers_;
    volatile LONG playing_;
    bool paused_;
    bool finished_;
    bool loop_;
    CRITICAL_SECTION cs_;

    // The waveOut callback only signals this event; a dedicated feeder thread
    // refills the buffers. Calling waveOutWrite from inside the callback can
    // deadlock against waveOutGetPosition on some drivers, so we never do it.
    HANDLE wakeEvent_;
    HANDLE feedThread_;
    volatile LONG threadRun_;

    int volume_;
    bool hwVolume_;
    bool inited_;

    bool mciActive_;
    bool mciPlaying_;
    bool mciPaused_;
    bool mciFinished_;
    double mciPos_;
    double mciLen_;

    AudioEngine(const AudioEngine&);
    AudioEngine& operator=(const AudioEngine&);
};
