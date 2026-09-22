#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <atomic>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// MusicPlayer：HTTP 拉流 → esp_audio_simple_dec 解码(mp3/aac/m4a) → 重采样到 codec
// 采样率并下混单声道 → AudioService::PlayPcm，复用现有 I2S/codec/输出任务。
// 与语音链路时分复用：唤醒/对话开始时由上层调 Stop()。真正的暂停靠停读 socket 实现。
class MusicPlayer {
public:
    static MusicPlayer& GetInstance() {
        static MusicPlayer instance;
        return instance;
    }
    MusicPlayer(const MusicPlayer&) = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;

    // 播放 url（会先停掉当前播放）。url 非法返回 false。
    bool Play(const std::string& url);
    void Pause();   // 用户主动暂停
    void Resume();  // 用户主动继续
    void Duck();    // 播报期间临时让路（不改变用户暂停意图）
    void Unduck();
    void Stop();
    bool IsPlaying() const { return running_.load(); }

private:
    MusicPlayer() = default;
    static void WorkerEntry(void* arg);
    void WorkerTask();
    bool IsPaused() const { return user_paused_.load() || ducked_.load(); }

    std::mutex mutex_;
    std::string url_;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> user_paused_{false};
    std::atomic<bool> ducked_{false};
};

#endif  // MUSIC_PLAYER_H
