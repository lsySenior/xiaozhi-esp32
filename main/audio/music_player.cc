#include "music_player.h"

#include <array>
#include <cstring>
#include <vector>

#include <esp_log.h>

#include "application.h"
#include "audio_service.h"
#include "board.h"
#include "http.h"

#include "esp_audio_types.h"
#include "esp_ae_rate_cvt.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"
#include "decoder/esp_audio_dec_default.h"

#define TAG "MusicPlayer"

namespace {
constexpr uint32_t kMusicTaskStack = 8192;
constexpr UBaseType_t kMusicTaskPriority = 3;
constexpr int kHttpTimeoutMs = 10000;

// SniffType 依据流头几个字节判断编码：m4a/mp4 容器含 "ftyp"，其余按 mp3 处理
// （kugou 直链基本是 mp3；ID3/MPEG sync 也归 mp3）。
esp_audio_simple_dec_type_t SniffType(const std::vector<uint8_t>& b) {
    if (b.size() >= 12 && memcmp(&b[4], "ftyp", 4) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
}

std::atomic<bool> g_dec_registered{false};

// EmitPcm 把解码出的交织 PCM 下混单声道、重采样到 codec 采样率后送入播放队列。
void EmitPcm(AudioService& audio, esp_ae_rate_cvt_handle_t resampler, int src_ch,
             const uint8_t* pcm_bytes, uint32_t decoded_size, std::atomic<bool>& running) {
    const int16_t* s = reinterpret_cast<const int16_t*>(pcm_bytes);
    size_t total = decoded_size / sizeof(int16_t);
    size_t frames = (src_ch == 2) ? total / 2 : total;
    if (frames == 0) {
        return;
    }
    std::vector<int16_t> mono(frames);
    if (src_ch == 2) {
        for (size_t i = 0; i < frames; i++) {
            mono[i] = (int16_t)(((int)s[2 * i] + (int)s[2 * i + 1]) / 2);
        }
    } else {
        memcpy(mono.data(), s, frames * sizeof(int16_t));
    }
    std::vector<int16_t> outpcm;
    if (resampler != nullptr) {
        uint32_t maxout = 0;
        esp_ae_rate_cvt_get_max_out_sample_num(resampler, frames, &maxout);
        outpcm.resize(maxout);
        uint32_t actual = maxout;
        esp_ae_rate_cvt_process(resampler, (esp_ae_sample_t)mono.data(), frames,
                                (esp_ae_sample_t)outpcm.data(), &actual);
        outpcm.resize(actual);
    } else {
        outpcm = std::move(mono);
    }
    if (!audio.PlayPcm(std::move(outpcm))) {
        running.store(false);
    }
}
}  // namespace

bool MusicPlayer::Play(const std::string& url) {
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) {
        return false;
    }
    Stop();  // 停掉当前（置停 + StopMusic 打断 PlayPcm 阻塞；旧任务自行退出）
    std::lock_guard<std::mutex> lock(mutex_);
    url_ = url;
    running_.store(true);
    user_paused_.store(false);
    ducked_.store(false);
    if (xTaskCreate(WorkerEntry, "music", kMusicTaskStack, this, kMusicTaskPriority, &task_) !=
        pdPASS) {
        ESP_LOGE(TAG, "create music task failed");
        running_.store(false);
        task_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "play %s", url.c_str());
    return true;
}

void MusicPlayer::Pause() { user_paused_.store(true); }

void MusicPlayer::Resume() { user_paused_.store(false); }

void MusicPlayer::Duck() { ducked_.store(true); }

void MusicPlayer::Unduck() { ducked_.store(false); }

void MusicPlayer::Stop() {
    running_.store(false);
    user_paused_.store(false);
    ducked_.store(false);
    // 打断可能阻塞在 PlayPcm 的 worker，并清空播放队列；不 join，避免阻塞主任务。
    Application::GetInstance().GetAudioService().StopMusic();
}

void MusicPlayer::WorkerEntry(void* arg) {
    static_cast<MusicPlayer*>(arg)->WorkerTask();
    vTaskDelete(nullptr);
}

void MusicPlayer::WorkerTask() {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = url_;
    }
    auto& audio = Application::GetInstance().GetAudioService();
    audio.SetMusicActive(true);
    if (!g_dec_registered.exchange(true)) {
        // simple_dec 只注册容器/简单解码器(m4a/ogg/wav...)，MP3/AAC 等裸流解码器
        // 由底层 esp_audio_dec 注册。缺了它会报 "Decoder MP3 not registered"，
        // 故先注册底层默认解码器，再显式补一遍 MP3(不依赖 menuconfig 开关)。
        esp_audio_dec_register_default();
        esp_mp3_dec_register();
        esp_audio_simple_dec_register_default();
    }

    const int codec_rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
    esp_audio_simple_dec_handle_t dec = nullptr;
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    int src_rate = 0, src_ch = 0;
    std::vector<uint8_t> inbuf;
    std::vector<uint8_t> outbuf(16384);

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (http) {
        http->SetTimeout(kHttpTimeoutMs);
        http->SetHeader("Accept", "*/*");
        http->SetHeader("Accept-Encoding", "identity");
        if (http->Open("GET", url)) {
            auto status = http->GetStatusCode();
            if (status && *status >= 200 && *status < 300) {
                std::array<char, 2048> buf;
                bool eof = false;
                while (running_.load() && !eof) {
                    if (user_paused_.load()) {
                        // 用户主动暂停才停读 socket；播报 Duck 不停读(见下方丢帧)，
                        // 否则暂停几秒会把直链连接拖死、播报结束无法续播。
                        vTaskDelay(pdMS_TO_TICKS(50));
                        continue;
                    }
                    auto size = http->Read(buf.data(), buf.size());
                    if (!size) break;
                    if (*size == 0) {
                        eof = true;
                    } else {
                        inbuf.insert(inbuf.end(), buf.data(), buf.data() + *size);
                    }
                    if (dec == nullptr && inbuf.size() >= 16) {
                        esp_audio_simple_dec_cfg_t cfg = {};
                        cfg.dec_type = SniffType(inbuf);
                        if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK) {
                            ESP_LOGE(TAG, "open decoder failed");
                            dec = nullptr;
                            break;
                        }
                    }
                    if (dec == nullptr) {
                        continue;
                    }
                    // 解码 inbuf 里已凑齐的帧，逐帧下混+重采样后送播放队列。
                    size_t offset = 0;
                    while (running_.load() && !user_paused_.load() && offset < inbuf.size()) {
                        esp_audio_simple_dec_raw_t raw = {};
                        raw.buffer = inbuf.data() + offset;
                        raw.len = (uint32_t)(inbuf.size() - offset);
                        raw.eos = eof;
                        esp_audio_simple_dec_out_t out = {};
                        out.buffer = outbuf.data();
                        out.len = (uint32_t)outbuf.size();
                        auto ret = esp_audio_simple_dec_process(dec, &raw, &out);
                        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                            outbuf.resize(out.needed_size);
                            continue;
                        }
                        if (ret != ESP_AUDIO_ERR_OK) {
                            offset = inbuf.size();
                            break;
                        }
                        offset += raw.consumed;
                        if (out.decoded_size > 0 && !ducked_.load()) {
                            // Duck 期间(TTS 播报)照常解码推进，但丢弃这几秒音频不送喇叭，
                            // 让出播放通道给 TTS；播报结束 Unduck 后无缝续播。
                            if (src_rate == 0) {
                                esp_audio_simple_dec_info_t info = {};
                                esp_audio_simple_dec_get_info(dec, &info);
                                src_rate = (int)info.sample_rate;
                                src_ch = info.channel > 0 ? info.channel : 1;
                                if (src_rate != 0 && src_rate != codec_rate) {
                                    esp_ae_rate_cvt_cfg_t rc = {};
                                    rc.src_rate = (uint32_t)src_rate;
                                    rc.dest_rate = (uint32_t)codec_rate;
                                    rc.channel = 1;
                                    rc.bits_per_sample = ESP_AUDIO_BIT16;
                                    rc.complexity = 2;
                                    rc.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
                                    esp_ae_rate_cvt_open(&rc, &resampler);
                                }
                            }
                            EmitPcm(audio, resampler, src_ch, out.buffer, out.decoded_size,
                                    running_);
                        }
                        if (raw.consumed == 0) {
                            break;  // 需要更多输入
                        }
                    }
                    if (offset > 0) {
                        inbuf.erase(inbuf.begin(), inbuf.begin() + offset);
                    }
                }
            } else {
                ESP_LOGE(TAG, "http status not ok");
            }
        } else {
            ESP_LOGE(TAG, "http open failed");
        }
        http->Close();
    }
    if (dec) esp_audio_simple_dec_close(dec);
    if (resampler) esp_ae_rate_cvt_close(resampler);
    audio.SetMusicActive(false);
    running_.store(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        task_ = nullptr;
    }
    ESP_LOGI(TAG, "music worker exit");
}
