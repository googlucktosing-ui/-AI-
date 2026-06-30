#ifndef MUSIC_PLAYER_H_
#define MUSIC_PLAYER_H_

#include "audio/audio_codec.h"
#include "display/display.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <atomic>
#include <string>
#include <vector>

class MusicPlayer {
public:
    explicit MusicPlayer(AudioCodec* codec, Display* display);
    ~MusicPlayer();

    lv_obj_t* CreateScreen(lv_obj_t* previous_screen);
    void Load(lv_obj_t* previous_screen);
    void BackToMenu();

    void TogglePlayPause();
    void Stop();
    void Next();
    void Previous();
    void Refresh();
    static void EventCallback(lv_event_t* e);

private:
    enum class State {
        kIdle,
        kScanning,
        kReady,
        kPlaying,
        kPaused,
        kStopped,
        kError,
    };

    struct Track {
        std::string path;
        std::string name;
    };

    AudioCodec* codec_;
    Display* display_;
    lv_obj_t* screen_ = nullptr;
    lv_obj_t* previous_screen_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* track_label_ = nullptr;
    lv_obj_t* counter_label_ = nullptr;
    lv_obj_t* play_button_label_ = nullptr;
    lv_obj_t* list_obj_ = nullptr;
    TaskHandle_t ui_task_handle_ = nullptr;

    std::vector<Track> tracks_;
    int current_index_ = 0;
    std::atomic<State> state_{State::kIdle};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> paused_{false};
    TaskHandle_t playback_task_ = nullptr;

    bool sd_mounted_ = false;
    void* sd_card_ = nullptr;

    static void PlaybackTaskEntry(void* arg);

    bool EnsureSdMounted();
    void ScanTracks();
    void StartCurrent();
    void PlaybackTask();
    bool DecodeAndPlay(const Track& track);
    void RebuildList();
    void UpdateUi(const char* status = nullptr);
    void SetState(State state, const char* status = nullptr);
    bool InUiTask() const;
    void RequestStopAndWait();
    bool IsSupportedFile(const char* name) const;
    int GetDecoderTypeForPath(const std::string& path) const;
    void OutputPcm(const int16_t* pcm, size_t samples, int sample_rate, int channels);
};

#endif  // MUSIC_PLAYER_H_
