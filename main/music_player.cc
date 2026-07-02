#include "music_player.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "decoder/impl/esp_mp3_dec.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "lvgl_theme.h"
#include "ui/musicplay/ui.h"
#include "ui/musicplay/screens/ui_MusicPlayScreen.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
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

constexpr uint32_t kVolumeActiveColor = 0x23DDF1;
constexpr uint32_t kVolumeInactiveColor = 0x234E7A;

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
    if (obj == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, opa, LV_PART_MAIN);
}

void SetCommand(lv_obj_t* obj, MusicPlayer* player, intptr_t command) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(obj, reinterpret_cast<void*>(command));
    lv_obj_add_event_cb(obj, MusicPlayer::EventCallback, LV_EVENT_CLICKED, player);
}

const lv_font_t* ResolveThemeTextFont(Display* display) {
    if (display == nullptr || display->GetTheme() == nullptr) {
        return nullptr;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(display->GetTheme());
    if (lvgl_theme == nullptr || lvgl_theme->text_font() == nullptr) {
        return nullptr;
    }
    return lvgl_theme->text_font()->font();
}

const lv_font_t* ApplyThemeTextFont(lv_obj_t* label, Display* display) {
    const lv_font_t* font = ResolveThemeTextFont(display);
    if (label != nullptr && font != nullptr) {
        lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    }
    return font;
}

void ConfigureSongNameLabel(lv_obj_t* label, Display* display) {
    if (label == nullptr) {
        return;
    }

    const lv_font_t* font = ApplyThemeTextFont(label, display);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(label, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(label, 0, LV_PART_MAIN);

    int line_height = font != nullptr ? font->line_height : 20;
    int min_height = line_height + 4;
    if (lv_obj_get_height(label) < min_height) {
        lv_obj_set_height(label, min_height);
    }
}

int ClampInt(int value, int low, int high) {
    return std::max(low, std::min(value, high));
}

const char* WeekdayText(int weekday) {
    static const char* kWeekdays[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    if (weekday < 0 || weekday > 6) {
        return "星期?";
    }
    return kWeekdays[weekday];
}

uint32_t ReadBe32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

uint32_t ReadSyncSafe32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0] & 0x7f) << 21) | (static_cast<uint32_t>(data[1] & 0x7f) << 14) |
           (static_cast<uint32_t>(data[2] & 0x7f) << 7) | static_cast<uint32_t>(data[3] & 0x7f);
}

void FormatTimeMs(int milliseconds, char* out, size_t out_size) {
    if (out == nullptr || out_size < 6) {
        if (out != nullptr && out_size > 0) {
            out[0] = '\0';
        }
        return;
    }
    int total_seconds = std::max(0, milliseconds) / 1000;
    int minutes = ClampInt(total_seconds / 60, 0, 99);
    int seconds = minutes >= 99 ? 59 : total_seconds % 60;
    out[0] = static_cast<char>('0' + (minutes / 10));
    out[1] = static_cast<char>('0' + (minutes % 10));
    out[2] = ':';
    out[3] = static_cast<char>('0' + (seconds / 10));
    out[4] = static_cast<char>('0' + (seconds % 10));
    out[5] = '\0';
}

long FindMp3AudioStart(FILE* file, long file_size) {
    if (file == nullptr || file_size < 10) {
        return 0;
    }

    uint8_t header[10] = {};
    if (fseek(file, 0, SEEK_SET) != 0 || fread(header, 1, sizeof(header), file) != sizeof(header)) {
        fseek(file, 0, SEEK_SET);
        return 0;
    }

    long offset = 0;
    if (memcmp(header, "ID3", 3) == 0) {
        offset = 10 + static_cast<long>(ReadSyncSafe32(header + 6));
        if ((header[5] & 0x10) != 0) {
            offset += 10;
        }
        if (offset < 0 || offset >= file_size) {
            offset = 0;
        }
    }
    fseek(file, 0, SEEK_SET);
    return offset;
}

int EstimateMp3DurationMs(FILE* file, long file_size) {
    if (file == nullptr || file_size <= 0) {
        return 0;
    }

    const long audio_start = FindMp3AudioStart(file, file_size);
    const long scan_end = std::min(file_size - 4, audio_start + 64 * 1024);
    static const int kBitrateMpeg1Layer3[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int kBitrateMpeg2Layer3[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const int kSampleRateMpeg1[4] = {44100, 48000, 32000, 0};
    static const int kSampleRateMpeg2[4] = {22050, 24000, 16000, 0};
    static const int kSampleRateMpeg25[4] = {11025, 12000, 8000, 0};

    for (long pos = audio_start; pos <= scan_end; ++pos) {
        uint8_t header[4] = {};
        if (fseek(file, pos, SEEK_SET) != 0 || fread(header, 1, sizeof(header), file) != sizeof(header)) {
            break;
        }
        if (header[0] != 0xff || (header[1] & 0xe0) != 0xe0) {
            continue;
        }

        int version = (header[1] >> 3) & 0x03;
        int layer = (header[1] >> 1) & 0x03;
        int bitrate_index = (header[2] >> 4) & 0x0f;
        int sample_rate_index = (header[2] >> 2) & 0x03;
        int channel_mode = (header[3] >> 6) & 0x03;
        if (version == 1 || layer != 1 || bitrate_index == 0 || bitrate_index == 15 || sample_rate_index == 3) {
            continue;
        }

        int bitrate_kbps = version == 3 ? kBitrateMpeg1Layer3[bitrate_index] : kBitrateMpeg2Layer3[bitrate_index];
        const int* sample_rate_table = version == 3 ? kSampleRateMpeg1 : (version == 2 ? kSampleRateMpeg2 : kSampleRateMpeg25);
        int sample_rate = sample_rate_table[sample_rate_index];
        if (bitrate_kbps <= 0 || sample_rate <= 0) {
            continue;
        }

        int side_info_size = version == 3 ? (channel_mode == 3 ? 17 : 32) : (channel_mode == 3 ? 9 : 17);
        uint8_t xing[16] = {};
        long xing_pos = pos + 4 + side_info_size;
        if (xing_pos + static_cast<long>(sizeof(xing)) <= file_size && fseek(file, xing_pos, SEEK_SET) == 0 &&
            fread(xing, 1, sizeof(xing), file) == sizeof(xing) &&
            (memcmp(xing, "Xing", 4) == 0 || memcmp(xing, "Info", 4) == 0)) {
            uint32_t flags = ReadBe32(xing + 4);
            if ((flags & 0x01) != 0) {
                uint32_t frames = ReadBe32(xing + 8);
                if (frames > 0) {
                    int samples_per_frame = version == 3 ? 1152 : 576;
                    fseek(file, 0, SEEK_SET);
                    return static_cast<int>((static_cast<uint64_t>(frames) * samples_per_frame * 1000ULL) / sample_rate);
                }
            }
        }

        long audio_bytes = file_size - audio_start;
        if (audio_bytes > 128) {
            uint8_t tag[3] = {};
            if (fseek(file, file_size - 128, SEEK_SET) == 0 && fread(tag, 1, sizeof(tag), file) == sizeof(tag) &&
                memcmp(tag, "TAG", 3) == 0) {
                audio_bytes -= 128;
            }
        }
        fseek(file, 0, SEEK_SET);
        return static_cast<int>((static_cast<uint64_t>(audio_bytes) * 8ULL) / static_cast<uint64_t>(bitrate_kbps));
    }

    fseek(file, 0, SEEK_SET);
    return 0;
}

}  // namespace

MusicPlayer::MusicPlayer(AudioCodec* codec, Display* display) : codec_(codec), display_(display) {
    esp_audio_err_t dec_ret = esp_mp3_dec_register();
    ESP_LOGI(TAG, "Register MP3 decoder ret=%d", dec_ret);
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

    ui_MusicPlayScreen_screen_init();
    screen_ = ui_MusicPlayScreen;
    SetCommand(ui_btnprev, this, 2);
    SetCommand(ui_iconprev, this, 2);
    SetCommand(ui_btn_play_pause, this, 3);
    SetCommand(ui_iconplaypause, this, 3);
    SetCommand(ui_btn_next, this, 4);
    SetCommand(ui_iconnext, this, 4);

    lv_obj_add_flag(ui_volumegroup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui_volumegroup, MusicPlayer::VolumeEventCallback, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(ui_volumegroup, MusicPlayer::VolumeEventCallback, LV_EVENT_PRESSING, this);
    lv_obj_add_event_cb(ui_iconvolume, MusicPlayer::VolumeEventCallback, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(ui_iconvolume, MusicPlayer::VolumeEventCallback, LV_EVENT_PRESSING, this);

    UpdateDateLabel();
    UpdateVolumeUi();
    UpdatePlayPauseIcon();
    UpdateProgress(0);
    if (ui_labelsongname != nullptr) {
        ConfigureSongNameLabel(ui_labelsongname, display_);
    }
    lv_label_set_text(ui_labelsource, "TF卡音乐");
    lv_label_set_text(ui_labeltimetotal, "--:--");
    return screen_;
}

void MusicPlayer::Load(lv_obj_t* previous_screen) {
    ESP_LOGI(TAG, "Load music player screen");
    if (InUiTask()) {
        CreateScreen(previous_screen);
        lv_screen_load(screen_);
        visible_ = true;
        UpdateUi("TF卡扫描中...");
    } else {
        DisplayLockGuard lock(display_);
        CreateScreen(previous_screen);
        lv_screen_load(screen_);
        visible_ = true;
        UpdateUi("TF卡扫描中...");
    }
    Refresh();
    if (!tracks_.empty()) {
        ESP_LOGI(TAG, "Auto start first track after entering music screen");
        StartCurrent();
    }
}

void MusicPlayer::BackToMenu() {
    Stop();
    if (InUiTask()) {
        if (previous_screen_ != nullptr) {
            lv_screen_load(previous_screen_);
        }
        visible_ = false;
    } else {
        DisplayLockGuard lock(display_);
        if (previous_screen_ != nullptr) {
            lv_screen_load(previous_screen_);
        }
        visible_ = false;
    }
}

void MusicPlayer::TogglePlayPause() {
    State state = state_.load();
    ESP_LOGI(TAG, "TogglePlayPause state=%d visible=%d tracks=%u", static_cast<int>(state), visible_.load(),
             static_cast<unsigned>(tracks_.size()));
    if (state == State::kPlaying) {
        paused_ = true;
        ESP_LOGI(TAG, "Music paused");
        SetState(State::kPaused, "已暂停");
        return;
    }
    if (state == State::kPaused) {
        paused_ = false;
        ESP_LOGI(TAG, "Music resumed");
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
        SetState(State::kReady, "点击播放");
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

    switch (command) {
    case 2:
        player->Previous();
        break;
    case 3:
        player->TogglePlayPause();
        break;
    case 4:
        player->Next();
        break;
    default:
        break;
    }
}

void MusicPlayer::VolumeEventCallback(lv_event_t* e) {
    auto* player = static_cast<MusicPlayer*>(lv_event_get_user_data(e));
    if (player == nullptr) {
        return;
    }
    player->SetVolumeFromTouch(e);
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

    ESP_LOGI(TAG, "Mounting TF card with SDMMC CLK=%d CMD=%d D0=%d", MUSIC_SD_CLK_GPIO, MUSIC_SD_CMD_GPIO,
             MUSIC_SD_D0_GPIO);
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
    ESP_LOGI(TAG, "TF card mounted at %s", MUSIC_SD_MOUNT_POINT);
    return true;
}

void MusicPlayer::ScanTracks() {
    tracks_.clear();
    if (!EnsureSdMounted()) {
        RebuildList();
        return;
    }

    ScanDirectory(MUSIC_SD_MOUNT_POINT, 0);
    std::sort(tracks_.begin(), tracks_.end(), [](const Track& a, const Track& b) {
        return ToLower(a.name) < ToLower(b.name);
    });
    ESP_LOGI(TAG, "TF scan finished, tracks=%u", static_cast<unsigned>(tracks_.size()));
    RebuildList();
}

void MusicPlayer::ScanDirectory(const std::string& directory, int depth) {
    if (depth > 3) {
        return;
    }

    DIR* dir = opendir(directory.c_str());
    if (dir == nullptr) {
        ESP_LOGW(TAG, "Failed to open TF directory: %s", directory.c_str());
        return;
    }

    while (auto* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        std::string path = directory + "/" + entry->d_name;
        struct stat st = {};
        if (stat(path.c_str(), &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            ScanDirectory(path, depth + 1);
            continue;
        }

        if (!IsSupportedFile(entry->d_name)) {
            continue;
        }

        ESP_LOGI(TAG, "Found music file: %s name=%s", path.c_str(), entry->d_name);
        tracks_.push_back({path, entry->d_name});
    }
    closedir(dir);
}

void MusicPlayer::StartCurrent() {
    ESP_LOGI(TAG, "StartCurrent tracks=%u current_index=%d task=%p", static_cast<unsigned>(tracks_.size()),
             current_index_, playback_task_);
    if (tracks_.empty()) {
        Refresh();
        if (tracks_.empty()) {
            ESP_LOGW(TAG, "No playable music file after refresh");
            return;
        }
    }
    RequestStopAndWait();
    stop_requested_ = false;
    paused_ = false;
    track_duration_ms_ = 0;
    UpdateProgress(0);
    SetState(State::kPlaying, "播放中");
    BaseType_t ret = xTaskCreate([](void* arg) {
        MusicPlayer::PlaybackTaskEntry(arg);
    }, "music_player", 8192, this, 3, &playback_task_);
    if (ret != pdPASS) {
        playback_task_ = nullptr;
        SetState(State::kError, "播放任务失败");
        ESP_LOGE(TAG, "Failed to create playback task");
    }
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
    UpdateProgress(100);
    SetState(State::kStopped, "播放完成");
}

bool MusicPlayer::DecodeAndPlay(const Track& track) {
    ESP_LOGI(TAG, "DecodeAndPlay path=%s", track.path.c_str());
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

    long file_size = 0;
    if (fseek(file, 0, SEEK_END) == 0) {
        file_size = ftell(file);
        fseek(file, 0, SEEK_SET);
    }
    ESP_LOGI(TAG, "Music file size=%ld decoder_type=%d", file_size, decoder_type);
    int duration_ms = decoder_type == ESP_AUDIO_SIMPLE_DEC_TYPE_MP3 ? EstimateMp3DurationMs(file, file_size) : 0;
    track_duration_ms_ = duration_ms;
    ESP_LOGI(TAG, "Estimated duration=%d ms", duration_ms);
    UpdateProgress(0, 0);
    long bytes_read_total = 0;
    int last_progress = -1;
    bool pcm_logged = false;

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
        bytes_read_total += static_cast<long>(read);
        if (file_size > 0) {
            int progress = ClampInt(static_cast<int>(bytes_read_total * 100 / file_size), 0, 100);
            if (progress != last_progress) {
                last_progress = progress;
                UpdateProgress(progress);
            }
        }
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
                    if (!pcm_logged) {
                        pcm_logged = true;
                        ESP_LOGI(TAG, "First PCM frame decoded: sample_rate=%d channels=%d bytes=%u",
                                 info.sample_rate, info.channel, frame.decoded_size);
                    }
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
    ESP_LOGI(TAG, "DecodeAndPlay finished success=%d", success);
    return success;
}

void MusicPlayer::RebuildList() {
    UpdateUi();
}

void MusicPlayer::UpdateUi(const char* status) {
    if (screen_ == nullptr) {
        return;
    }

    UpdateDateLabel();
    UpdateVolumeUi();
    UpdatePlayPauseIcon();

    if (status != nullptr && ui_labelsource != nullptr) {
        lv_label_set_text(ui_labelsource, status);
    }
    if (ui_labelsongname != nullptr) {
        ConfigureSongNameLabel(ui_labelsongname, display_);
        if (!tracks_.empty() && current_index_ >= 0 && current_index_ < static_cast<int>(tracks_.size())) {
            lv_label_set_text(ui_labelsongname, tracks_[current_index_].name.c_str());
            ESP_LOGI(TAG, "UI song name: %s", tracks_[current_index_].name.c_str());
        } else {
            lv_label_set_text(ui_labelsongname, "TF卡暂无音乐");
        }
        ConfigureSongNameLabel(ui_labelsongname, display_);
    }
}

void MusicPlayer::UpdateDateLabel() {
    if (ui_labeldateweek == nullptr) {
        return;
    }
    time_t now = time(nullptr);
    struct tm timeinfo = {};
    localtime_r(&now, &timeinfo);
    char text[32];
    snprintf(text, sizeof(text), "%02d/%02d %s", timeinfo.tm_mon + 1, timeinfo.tm_mday, WeekdayText(timeinfo.tm_wday));
    lv_label_set_text(ui_labeldateweek, text);
}

void MusicPlayer::UpdateProgress(int percent, int elapsed_ms) {
    auto update = [this, percent, elapsed_ms]() {
        int safe_percent = ClampInt(percent, 0, 100);
        int duration_ms = track_duration_ms_.load();
        int safe_elapsed_ms = elapsed_ms;
        if (safe_elapsed_ms < 0 && duration_ms > 0) {
            safe_elapsed_ms = static_cast<int>((static_cast<int64_t>(duration_ms) * safe_percent) / 100);
        }
        if (ui_barprogress != nullptr) {
            lv_bar_set_value(ui_barprogress, safe_percent, LV_ANIM_OFF);
        }
        if (ui_progress_knob != nullptr && ui_barprogress != nullptr) {
            int bar_x = lv_obj_get_x(ui_barprogress);
            int bar_w = lv_obj_get_width(ui_barprogress);
            int knob_w = lv_obj_get_width(ui_progress_knob);
            lv_obj_set_x(ui_progress_knob, bar_x + (bar_w * safe_percent / 100) - (knob_w / 2));
        }
        if (ui_labeltimecurrent != nullptr) {
            char text[8];
            FormatTimeMs(std::max(0, safe_elapsed_ms), text, sizeof(text));
            lv_label_set_text(ui_labeltimecurrent, text);
        }
        if (ui_labeltimetotal != nullptr) {
            if (duration_ms > 0) {
                char text[8];
                FormatTimeMs(duration_ms, text, sizeof(text));
                lv_label_set_text(ui_labeltimetotal, text);
            } else {
                lv_label_set_text(ui_labeltimetotal, "--:--");
            }
        }
    };

    if (InUiTask()) {
        update();
    } else {
        DisplayLockGuard lock(display_);
        update();
    }
}

void MusicPlayer::UpdateVolumeUi() {
    lv_obj_t* bars[] = {
        ui_volumebar01, ui_volumebar02, ui_volume_bar_03, ui_volume_bar_04, ui_volumebar05,
        ui_volumebar06, ui_volumebar07, ui_volumebar08, ui_volumebar09,
    };
    int volume = codec_ != nullptr ? codec_->output_volume() : 0;
    int active_count = ClampInt((volume + 10) / 11, 0, 9);
    for (int i = 0; i < 9; ++i) {
        SetObjBg(bars[i], i < active_count ? kVolumeActiveColor : kVolumeInactiveColor);
    }
}

void MusicPlayer::UpdatePlayPauseIcon() {
    if (ui_iconplaypause == nullptr) {
        return;
    }

    if (state_.load() == State::kPlaying) {
        lv_image_set_src(ui_iconplaypause, &ui_img_images_musicplay_icon_pause_png);
    } else {
        lv_image_set_src(ui_iconplaypause, &ui_img_images_musicplay_icon_play_png);
    }
}

void MusicPlayer::SetVolumeFromTouch(lv_event_t* e) {
    if (codec_ == nullptr || ui_volumegroup == nullptr) {
        return;
    }

    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) {
        return;
    }

    lv_point_t point = {};
    lv_indev_get_point(indev, &point);

    lv_area_t area = {};
    lv_obj_get_coords(ui_volumegroup, &area);
    int width = lv_area_get_width(&area);
    if (width <= 0) {
        return;
    }

    int volume = ClampInt((point.x - area.x1) * 100 / width, 0, 100);
    codec_->SetOutputVolume(volume);
    UpdateVolumeUi();
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
    return EndsWith(lower, ".mp3");
}

int MusicPlayer::GetDecoderTypeForPath(const std::string& path) const {
    std::string lower = ToLower(path);
    if (EndsWith(lower, ".mp3")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
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
