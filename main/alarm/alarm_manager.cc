#include "alarm/alarm_manager.h"

#include <algorithm>
#include <cstdlib>

#include <esp_log.h>

#include "application.h"
#include "settings.h"
#include "assets/lang_config.h"

#define TAG "Alarm"

namespace {
constexpr size_t kMaxAlarms = 8;
constexpr char kNs[] = "alarm";
constexpr char kKeyList[] = "list";
constexpr char kKeyNextId[] = "next_id";

// EscapeJson 转义 label 里的 " 和 \ 与换行，保证拼出的 JSON 合法。
std::string EscapeJson(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += c;
        } else if (c == '\n') {
            o += "\\n";
        } else {
            o += c;
        }
    }
    return o;
}
}  // namespace

void AlarmManager::EnsureTimer() {
    if (timer_ != nullptr) {
        return;
    }
    esp_timer_create_args_t args = {};
    args.callback = &AlarmManager::TimerCb;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "alarm";
    esp_timer_create(&args, &timer_);
}

void AlarmManager::TimerCb(void* /*arg*/) {
    // 定时回调在 esp_timer 任务里，切回主任务再改状态/播音。
    Application::GetInstance().Schedule([]() { AlarmManager::GetInstance().OnDue(); });
}

int AlarmManager::SetOnce(int seconds, const std::string& label) {
    if (seconds <= 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureTimer();
    if (alarms_.size() >= kMaxAlarms) {
        ESP_LOGW(TAG, "too many alarms (%d)", (int)alarms_.size());
        return -1;
    }
    std::string clean = label;
    std::replace(clean.begin(), clean.end(), '\n', ' ');  // 保证单行持久化格式
    Alarm a;
    a.id = next_id_++;
    a.fire_epoch = time(nullptr) + seconds;
    a.label = clean;
    alarms_.push_back(a);
    PersistLocked();
    RearmLocked();
    ESP_LOGI(TAG, "set id=%d in %ds label=%s", a.id, seconds, clean.c_str());
    return a.id;
}

bool AlarmManager::Cancel(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t before = alarms_.size();
    alarms_.erase(std::remove_if(alarms_.begin(), alarms_.end(),
                                 [id](const Alarm& a) { return a.id == id; }),
                  alarms_.end());
    if (alarms_.size() == before) {
        return false;
    }
    PersistLocked();
    RearmLocked();
    ESP_LOGI(TAG, "cancel id=%d", id);
    return true;
}

std::string AlarmManager::ListJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    time_t now = time(nullptr);
    std::string out = "[";
    bool first = true;
    for (const auto& a : alarms_) {
        if (!first) {
            out += ",";
        }
        first = false;
        long in_sec = (long)(a.fire_epoch - now);
        if (in_sec < 0) {
            in_sec = 0;
        }
        out += "{\"id\":" + std::to_string(a.id) +
               ",\"in_seconds\":" + std::to_string(in_sec) +
               ",\"fire_epoch\":" + std::to_string((long long)a.fire_epoch) +
               ",\"label\":\"" + EscapeJson(a.label) + "\"}";
    }
    out += "]";
    return out;
}

void AlarmManager::OnDue() {
    int fired = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        time_t now = time(nullptr);
        for (auto it = alarms_.begin(); it != alarms_.end();) {
            if (it->fire_epoch <= now + 1) {  // 到点（留 1s 容差）
                ESP_LOGI(TAG, "fired id=%d label=%s", it->id, it->label.c_str());
                it = alarms_.erase(it);
                fired++;
            } else {
                ++it;
            }
        }
        if (fired > 0) {
            PersistLocked();
        }
        RearmLocked();
    }
    // 锁外播音（已在主任务）。响几声更像闹钟。
    for (int i = 0; i < fired; i++) {
        for (int k = 0; k < 3; k++) {
            Application::GetInstance().PlaySound(Lang::Sounds::OGG_EXCLAMATION);
        }
    }
}

void AlarmManager::RearmLocked() {
    if (timer_ == nullptr) {
        return;
    }
    esp_timer_stop(timer_);  // 幂等：未启动也无妨
    if (alarms_.empty()) {
        return;
    }
    time_t now = time(nullptr);
    time_t earliest = alarms_.front().fire_epoch;
    for (const auto& a : alarms_) {
        if (a.fire_epoch < earliest) {
            earliest = a.fire_epoch;
        }
    }
    int64_t delay = (int64_t)earliest - (int64_t)now;
    if (delay < 0) {
        delay = 0;
    }
    esp_timer_start_once(timer_, (uint64_t)delay * 1000000ULL);
}

void AlarmManager::PersistLocked() {
    std::string data;
    for (const auto& a : alarms_) {
        data += std::to_string(a.id) + "," + std::to_string((long long)a.fire_epoch) + "," +
                a.label + "\n";
    }
    Settings settings(kNs, true);
    settings.SetString(kKeyList, data);
    settings.SetInt(kKeyNextId, next_id_);
}

void AlarmManager::LoadLocked() {
    alarms_.clear();
    Settings settings(kNs, false);
    next_id_ = settings.GetInt(kKeyNextId, 1);
    std::string data = settings.GetString(kKeyList, "");
    time_t now = time(nullptr);
    size_t pos = 0;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        std::string line =
            (nl == std::string::npos) ? data.substr(pos) : data.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? data.size() : nl + 1;
        if (line.empty()) {
            continue;
        }
        size_t c1 = line.find(',');
        if (c1 == std::string::npos) {
            continue;
        }
        size_t c2 = line.find(',', c1 + 1);
        if (c2 == std::string::npos) {
            continue;
        }
        Alarm a;
        a.id = atoi(line.substr(0, c1).c_str());
        a.fire_epoch = (time_t)atoll(line.substr(c1 + 1, c2 - c1 - 1).c_str());
        a.label = line.substr(c2 + 1);
        if (a.fire_epoch > now) {  // 关机期间已错过的丢弃
            alarms_.push_back(a);
            if (a.id >= next_id_) {
                next_id_ = a.id + 1;
            }
        }
    }
}

void AlarmManager::Load() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (loaded_) {
        return;
    }
    loaded_ = true;
    EnsureTimer();
    LoadLocked();
    RearmLocked();
    if (!alarms_.empty()) {
        ESP_LOGI(TAG, "loaded %d alarm(s)", (int)alarms_.size());
    }
}
