#ifndef ALARM_MANAGER_H
#define ALARM_MANAGER_H

#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include <esp_timer.h>

// AlarmManager 管理一次性闹钟：用单个 esp_timer 始终指向最早未触发的闹钟，
// 到点切回主任务播内置提示音；闹钟持久化到 NVS，开机（时间校准后）由 Load() 重排，
// 关机期间已错过的直接丢弃。只支持一次性，不做循环。
class AlarmManager {
public:
    static AlarmManager& GetInstance() {
        static AlarmManager instance;
        return instance;
    }
    AlarmManager(const AlarmManager&) = delete;
    AlarmManager& operator=(const AlarmManager&) = delete;

    // 开机、系统时间校准后调用：从 NVS 载入未过期闹钟并重排。
    void Load();
    // 设 seconds 秒后触发的一次性闹钟，返回 id(>0)；参数非法或超额返回 -1。
    int SetOnce(int seconds, const std::string& label);
    // 取消指定 id，成功返回 true。
    bool Cancel(int id);
    // 返回闹钟列表 JSON 文本：[{"id":1,"in_seconds":600,"fire_epoch":..,"label":".."}]
    std::string ListJson();

private:
    AlarmManager() = default;

    struct Alarm {
        int id;
        time_t fire_epoch;
        std::string label;
    };

    void EnsureTimer();
    void RearmLocked();
    void OnDue();  // 在主任务上执行
    void LoadLocked();
    void PersistLocked();
    static void TimerCb(void* arg);

    std::mutex mutex_;
    std::vector<Alarm> alarms_;
    int next_id_ = 1;
    esp_timer_handle_t timer_ = nullptr;
    bool loaded_ = false;
};

#endif  // ALARM_MANAGER_H
