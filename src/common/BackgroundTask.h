#pragma once
// ============================================================================
// RomCloud BackgroundTask — worker thread chuẩn cho mọi tác vụ nặng.
// Header-only. UI poll progress() mỗi frame để vẽ progress bar, không block.
// Dùng chung cho: FileExplorer copy/paste, Download, OTA, DriveSync, thumbnail.
// ============================================================================
#include <atomic>
#include <cstdint>
#include <exception>
#include <string>
#include <thread>
#include <utility>

namespace RomCloud {

struct TaskProgress {
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> total{0};
    std::atomic<bool> cancel{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> running{false};
    std::string error;
};

class BackgroundTask {
public:
    BackgroundTask() = default;
    ~BackgroundTask() { wait(); }

    BackgroundTask(const BackgroundTask&) = delete;
    BackgroundTask& operator=(const BackgroundTask&) = delete;

    // Chạy fn(progress) trên worker thread. Gọi requestCancel() từ UI để hủy.
    template <typename Fn>
    void run(Fn&& fn) {
        wait();
        m_progress.done = 0;
        m_progress.total = 0;
        m_progress.cancel = false;
        m_progress.finished = false;
        m_progress.running = true;
        m_progress.error.clear();
        m_thread = std::thread([this, f = std::forward<Fn>(fn)]() mutable {
            try {
                f(m_progress);
            } catch (const std::exception& e) {
                m_progress.error = e.what();
            } catch (...) {
                m_progress.error = "Unknown background task error";
            }
            m_progress.finished = true;
            m_progress.running = false;
        });
    }

    TaskProgress& progress() { return m_progress; }
    const TaskProgress& progress() const { return m_progress; }

    void requestCancel() { m_progress.cancel = true; }
    bool isRunning() const { return m_progress.running.load(); }
    bool isFinished() const { return m_progress.finished.load(); }
    double fraction() const {
        uint64_t t = m_progress.total.load();
        if (t == 0) return 0.0;
        return static_cast<double>(m_progress.done.load()) / static_cast<double>(t);
    }

    void wait() {
        if (m_thread.joinable()) m_thread.join();
    }

private:
    TaskProgress m_progress;
    std::thread m_thread;
};

} // namespace RomCloud
