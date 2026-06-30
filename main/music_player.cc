#include "music_player.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#define TAG "MusicPlayer"

#define MUSIC_SD_MOUNT_POINT "/sdcard"
#define MUSIC_SD_CLK_GPIO 47
#define MUSIC_SD_CMD_GPIO 48
#define MUSIC_SD_D0_GPIO 21
#define MUSIC_READ_BUFFER_SIZE 4096
#define MUSIC_DECODE_BUFFER_SIZE (32 * 1024)

namespace {

std::string ToLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

bool EndsWith(const std::string& text, const char* suffix) {
    size_t suffix_len = std::strlen(suffix);
    return text.size() >= suffix_len && text.compare(text.size() - suffix_len, suffix_len, suffix) == 0;
}

void SetObjBg(lv_obj_t* obj, uint32_t color, uint8_t opa = 255) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
}

lv_obj_t* MakeButton(lv_obj_t* parent, const char* text, int x, int y, int w, int h, MusicPlayer* player,
                     intptr_t command) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x00A8FF), 0);
    SetObjBg(btn, 0x00448C);
    lv_obj_set_user_data(btn, reinterpret_cast<void*>(command));
    lv_obj_add_event_cb(btn, MusicPlayer::EventCallback, LV_EVENT_CLICKED, player);

    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label);
    return label;
}

}  // namespace

MusicPlayer::MusicPlayer(AudioCodec* codec, Display* display) : codec_(codec), display_(display) {
    esp_audio_simple_dec_register_default();
}

MusicPlayer::~MusicPlayer() {
    RequestStopAndWait();
}

lv_obj_t* MusicPlayer::CreateScreen(lv_obj_t* previous_screen) {
    if (ui_task_handle_ == nullptr) {
        ui_task_handle_ = xTaskGetCurrentTaskHandle();
    }
    previous_screen_ = previous_screen;
    if (screen_ != nullptr) {
        return screen_;
    }

    screen_ = lv_obj_create(nullptr);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
    SetObjBg(screen_, 0x001334);

    lv_obj_t* header = lv_obj_create(screen_);
    lv_obj_set_size(header, 320, 36);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    SetObjBg(header, 0x003476);

    MakeButton(header, "<", 6, 4, 38, 28, this, 1);

    title_label_ = lv_label_create(header);
    lv_label_set_text(title_label_, "音乐播放");
    lv_obj_set_style_text_color(title_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title_label_, LV_ALIGN_CENTER, 0, 0);

    status_label_ = lv_label_create(screen_);
    lv_obj_set_size(status_label_, 270, 20);
    lv_obj_set_pos(status_label_, 25, 43);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0x74E8FF), 0);
    lv_label_set_text(status_label_, "准备扫描TF卡");

    track_label_ = lv_label_create(screen_);
    lv_obj_set_size(track_label_, 286, 26);
    lv_obj_set_pos(track_label_, 17, 67);
    lv_label_set_long_mode(track_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(track_label_, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(track_label_, "未选择歌曲");

    list_obj_ = lv_obj_create(screen_);
    lv_obj_set_size(list_obj_, 286, 82);
    lv_obj_set_pos(list_obj_, 17, 98);
    lv_obj_set_style_radius(list_obj_, 10, 0);
    lv_obj_set_style_border_width(list_obj_, 1, 0);
    lv_obj_set_style_border_color(list_obj_, lv_color_hex(0x0074CD), 0);
    lv_obj_set_style_pad_all(list_obj_, 4, 0);
    lv_obj_set_flex_flow(list_obj_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_obj_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(list_obj_, LV_SCROLLBAR_MODE_AUTO);
    SetObjBg(list_obj_, 0x00245A);

    counter_label_ = lv_label_create(screen_);
    lv_obj_set_size(counter_label_, 80, 18);
    lv_obj_set_pos(counter_label_, 120, 183);
    lv_obj_set_style_text_align(counter_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(counter_label_, lv_color_hex(0xAEEFFF), 0);
    lv_label_set_text(counter_label_, "0/0");

    MakeButton(screen_, "|<", 22, 203, 50, 30, this, 2);
    play_button_label_ = MakeButton(screen_, "播放", 86, 197, 72, 38, this, 3);
    MakeButton(screen_, ">|", 172, 203, 50, 30, this, 4);
    MakeButton(screen_, "停止", 238, 203, 60, 30, this, 5);

    return screen_;
}

void MusicPlayer::Load(lv_obj_t* previous_screen) {
    if (InUiTask()) {
        CreateScreen(previous_screen);
        lv_screen_load(screen_);
        UpdateUi("TF卡扫描中...");
    } else {
        DisplayLockGuard lock(display_);
        CreateScreen(previous_screen);
        lv_screen_load(screen_);
        UpdateUi("TF卡扫描中...");
    }
    Refresh();
}

void MusicPlayer::BackToMenu() {
    Stop();
    if (InUiTask()) {
        if (previous_screen_ != nullptr) {
            lv_screen_load(previous_screen_);
        }
    } else {
        DisplayLockGuard lock(display_);
        if (previous_screen_ != nullptr) {
            lv_screen_load(previous_screen_);
        }
    }
}

void MusicPlayer::TogglePlayPause() {
    State state = state_.load();
    if (state == State::kPlaying) {
        paused_ = true;
        SetState(State::kPaused, "已暂停");
        return;
    }
    if (state == State::kPaused) {
        paused_ = false;
        SetState(State::kPlaying, "播放中");
        return;
    }
    StartCurrent();
}

void MusicPlayer::Stop() {
    RequestStopAndWait();
    SetState(State::kStopped, "已停止");
}

void MusicPlayer::Next() {
    if (tracks_.empty()) {
        SetState(State::kError, "TF卡没有可播放文件");
        return;
    }
    RequestStopAndWait();
    current_index_ = (current_index_ + 1) % tracks_.size();
    StartCurrent();
}

void MusicPlayer::Previous() {
    if (tracks_.empty()) {
        SetState(State::kError, "TF卡没有可播放文件");
        return;
    }
    RequestStopAndWait();
    current_index_ = (current_index_ + tracks_.size() - 1) % tracks_.size();
    StartCurrent();
}

void MusicPlayer::Refresh() {
    SetState(State::kScanning, "TF卡扫描中...");
    ScanTracks();
    if (tracks_.empty()) {
        SetState(State::kError, "未找到音乐文件");
    } else {
        current_index_ = std::min(current_index_, static_cast<int>(tracks_.size() - 1));
        SetState(State::kReady, "选择歌曲后点播放");
    }
}

void MusicPlayer::EventCallback(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto* player = static_cast<MusicPlayer*>(lv_event_get_user_data(e));
    if (player == nullptr) {
        return;
    }
    if (player->ui_task_handle_ == nullptr) {
        player->ui_task_handle_ = xTaskGetCurrentTaskHandle();
    }
    intptr_t command = reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_current_target_obj(e)));

    if (command < 0) {
        auto index = -command - 1;
        player->RequestStopAndWait();
        player->current_index_ = static_cast<int>(index);
        player->StartCurrent();
        return;
    }

    switch (command) {
    case 1:
        player->BackToMenu();
        break;
    case 2:
        player->Previous();
        break;
    case 3:
        player->TogglePlayPause();
        break;
    case 4:
        player->Next();
        break;
    case 5:
        player->Stop();
        break;
    default:
        break;
    }
}

void MusicPlayer::PlaybackTaskEntry(void* arg) {
    auto* player = static_cast<MusicPlayer*>(arg);
    player->PlaybackTask();
    player->playback_task_ = nullptr;
    vTaskDelete(nullptr);
}

bool MusicPlayer::EnsureSdMounted() {
    if (sd_mounted_) {
        return true;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = (gpio_num_t)MUSIC_SD_CLK_GPIO;
    slot_config.cmd = (gpio_num_t)MUSIC_SD_CMD_GPIO;
    slot_config.d0 = (gpio_num_t)MUSIC_SD_D0_GPIO;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sdmmc_card_t* card = nullptr;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MUSIC_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to mount TF card: %s", esp_err_to_name(ret));
        sd_card_ = nullptr;
        sd_mounted_ = false;
        return false;
    }

    sd_card_ = card;
    sd_mounted_ = true;
    sdmmc_card_print_info(stdout, card);
    return true;
}

void MusicPlayer::ScanTracks() {
    tracks_.clear();
    if (!EnsureSdMounted()) {
        RebuildList();
        return;
    }

    DIR* dir = opendir(MUSIC_SD_MOUNT_POINT);
    if (dir == nullptr) {
        ESP_LOGW(TAG, "Failed to open TF root");
        RebuildList();
        return;
    }

    while (auto* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (!IsSupportedFile(entry->d_name)) {
            continue;
        }

        std::string path = std::string(MUSIC_SD_MOUNT_POINT) + "/" + entry->d_name;
        struct stat st = {};
        if (stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
            continue;
        }
        tracks_.push_back({path, entry->d_name});
    }
    closedir(dir);

    std::sort(tracks_.begin(), tracks_.end(), [](const Track& a, const Track& b) {
        return ToLower(a.name) < ToLower(b.name);
    });
    RebuildList();
}

void MusicPlayer::StartCurrent() {
    if (tracks_.empty()) {
        Refresh();
        if (tracks_.empty()) {
            return;
        }
    }
    RequestStopAndWait();
    stop_requested_ = false;
    paused_ = false;
    SetState(State::kPlaying, "播放中");
    xTaskCreate([](void* arg) {
        MusicPlayer::PlaybackTaskEntry(arg);
    }, "music_player", 8192, this, 3, &playback_task_);
}

void MusicPlayer::PlaybackTask() {
    if (current_index_ < 0 || current_index_ >= static_cast<int>(tracks_.size())) {
        SetState(State::kError, "歌曲索引错误");
        return;
    }

    const Track track = tracks_[current_index_];
    bool ok = DecodeAndPlay(track);
    if (stop_requested_) {
        return;
    }
    if (!ok) {
        SetState(State::kError, "播放失败");
        return;
    }
    SetState(State::kStopped, "播放完成");
}

bool MusicPlayer::DecodeAndPlay(const Track& track) {
    int decoder_type = GetDecoderTypeForPath(track.path);
    if (decoder_type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        ESP_LOGW(TAG, "Unsupported music file: %s", track.path.c_str());
        return false;
    }

    FILE* file = fopen(track.path.c_str(), "rb");
    if (file == nullptr) {
        ESP_LOGW(TAG, "Failed to open music file: %s", track.path.c_str());
        return false;
    }

    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = static_cast<esp_audio_simple_dec_type_t>(decoder_type),
        .dec_cfg = nullptr,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    esp_audio_simple_dec_handle_t decoder = nullptr;
    esp_audio_err_t ret = esp_audio_simple_dec_open(&cfg, &decoder);
    if (ret != ESP_AUDIO_ERR_OK || decoder == nullptr) {
        ESP_LOGW(TAG, "Failed to open decoder for %s: %d", track.path.c_str(), ret);
        fclose(file);
        return false;
    }

    std::vector<uint8_t> input(MUSIC_READ_BUFFER_SIZE);
    std::vector<uint8_t> output(MUSIC_DECODE_BUFFER_SIZE);
    bool success = true;
    bool eof = false;

    while (!stop_requested_) {
        while (paused_ && !stop_requested_) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (stop_requested_) {
            break;
        }

        size_t read = fread(input.data(), 1, input.size(), file);
        eof = read < input.size();
        if (read == 0 && eof) {
            break;
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer = input.data(),
            .len = static_cast<uint32_t>(read),
            .eos = eof,
            .consumed = 0,
            .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
        };

        while ((raw.len > 0 || raw.eos) && !stop_requested_) {
            esp_audio_simple_dec_out_t frame = {
                .buffer = output.data(),
                .len = static_cast<uint32_t>(output.size()),
                .needed_size = 0,
                .decoded_size = 0,
            };
            ret = esp_audio_simple_dec_process(decoder, &raw, &frame);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && frame.needed_size > output.size()) {
                output.resize(frame.needed_size);
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                ESP_LOGW(TAG, "Decode failed for %s: %d", track.path.c_str(), ret);
                success = false;
                break;
            }
            if (frame.decoded_size > 0) {
                esp_audio_simple_dec_info_t info = {};
                if (esp_audio_simple_dec_get_info(decoder, &info) == ESP_AUDIO_ERR_OK &&
                    info.bits_per_sample == 16 && info.sample_rate > 0 && info.channel > 0) {
                    OutputPcm(reinterpret_cast<int16_t*>(frame.buffer), frame.decoded_size / sizeof(int16_t),
                              info.sample_rate, info.channel);
                }
            }
            if (raw.consumed == 0) {
                break;
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            raw.consumed = 0;
            if (raw.len == 0) {
                raw.eos = false;
            }
        }
        if (!success || eof) {
            break;
        }
    }

    esp_audio_simple_dec_close(decoder);
    fclose(file);
    return success;
}

void MusicPlayer::RebuildList() {
    if (list_obj_ == nullptr) {
        return;
    }

    auto rebuild = [this]() {
        lv_obj_clean(list_obj_);
        for (size_t i = 0; i < tracks_.size(); ++i) {
            const Track& track = tracks_[i];
            lv_obj_t* btn = lv_button_create(list_obj_);
            lv_obj_set_size(btn, 268, 28);
            lv_obj_set_style_radius(btn, 7, 0);
            lv_obj_set_style_border_width(btn, 1, 0);
            lv_obj_set_style_border_color(btn, lv_color_hex(0x006EC5), 0);
            SetObjBg(btn, i == static_cast<size_t>(current_index_) ? 0x0060B8 : 0x003476);
            lv_obj_set_user_data(btn, reinterpret_cast<void*>(-(static_cast<intptr_t>(i) + 1)));
            lv_obj_add_event_cb(btn, MusicPlayer::EventCallback, LV_EVENT_CLICKED, this);

            lv_obj_t* label = lv_label_create(btn);
            lv_obj_set_width(label, 250);
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            lv_label_set_text(label, track.name.c_str());
            lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
            lv_obj_center(label);
        }
        UpdateUi();
    };

    if (InUiTask()) {
        rebuild();
    } else {
        DisplayLockGuard lock(display_);
        rebuild();
    }
}

void MusicPlayer::UpdateUi(const char* status) {
    if (screen_ == nullptr) {
        return;
    }

    if (status != nullptr && status_label_ != nullptr) {
        lv_label_set_text(status_label_, status);
    }
    if (track_label_ != nullptr) {
        if (!tracks_.empty() && current_index_ >= 0 && current_index_ < static_cast<int>(tracks_.size())) {
            lv_label_set_text(track_label_, tracks_[current_index_].name.c_str());
        } else {
            lv_label_set_text(track_label_, "TF卡根目录暂无音乐");
        }
    }
    if (counter_label_ != nullptr) {
        lv_label_set_text_fmt(counter_label_, "%d/%d", tracks_.empty() ? 0 : current_index_ + 1,
                              static_cast<int>(tracks_.size()));
    }
    if (play_button_label_ != nullptr) {
        State state = state_.load();
        lv_label_set_text(play_button_label_, state == State::kPlaying ? "暂停" : "播放");
    }
}

void MusicPlayer::SetState(State state, const char* status) {
    state_ = state;
    if (InUiTask()) {
        UpdateUi(status);
    } else {
        DisplayLockGuard lock(display_);
        UpdateUi(status);
    }
}

bool MusicPlayer::InUiTask() const {
    return ui_task_handle_ != nullptr && ui_task_handle_ == xTaskGetCurrentTaskHandle();
}

void MusicPlayer::RequestStopAndWait() {
    if (playback_task_ == nullptr) {
        stop_requested_ = false;
        paused_ = false;
        return;
    }

    stop_requested_ = true;
    paused_ = false;
    for (int i = 0; i < 60 && playback_task_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    stop_requested_ = false;
}

bool MusicPlayer::IsSupportedFile(const char* name) const {
    std::string lower = ToLower(name);
    return EndsWith(lower, ".mp3") || EndsWith(lower, ".wav") || EndsWith(lower, ".aac") ||
           EndsWith(lower, ".m4a") || EndsWith(lower, ".flac");
}

int MusicPlayer::GetDecoderTypeForPath(const std::string& path) const {
    std::string lower = ToLower(path);
    if (EndsWith(lower, ".mp3")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    if (EndsWith(lower, ".wav")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    }
    if (EndsWith(lower, ".aac")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    }
    if (EndsWith(lower, ".m4a")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    }
    if (EndsWith(lower, ".flac")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

void MusicPlayer::OutputPcm(const int16_t* pcm, size_t samples, int sample_rate, int channels) {
    if (pcm == nullptr || samples == 0 || sample_rate <= 0 || channels <= 0) {
        return;
    }

    if (!codec_->output_enabled()) {
        codec_->EnableOutput(true);
    }

    const int output_rate = codec_->output_sample_rate();
    const size_t frames = samples / channels;
    if (frames == 0) {
        return;
    }

    std::vector<int16_t> mono;
    mono.reserve(frames);
    for (size_t frame = 0; frame < frames; ++frame) {
        int32_t mixed = 0;
        for (int ch = 0; ch < channels; ++ch) {
            mixed += pcm[frame * channels + ch];
        }
        mono.push_back(static_cast<int16_t>(mixed / channels));
    }

    if (sample_rate == output_rate) {
        codec_->OutputData(mono);
        return;
    }

    size_t out_frames = mono.size() * output_rate / sample_rate;
    if (out_frames == 0) {
        out_frames = 1;
    }
    std::vector<int16_t> resampled(out_frames);
    for (size_t i = 0; i < out_frames; ++i) {
        size_t src_index = i * sample_rate / output_rate;
        if (src_index >= mono.size()) {
            src_index = mono.size() - 1;
        }
        resampled[i] = mono[src_index];
    }
    codec_->OutputData(resampled);
}
