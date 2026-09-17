#ifndef RUNTIME_INFERENCE_SESSION_H
#define RUNTIME_INFERENCE_SESSION_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class IDetector;

namespace runtime
{

bool preload_model_metadata(const std::string& model_path, bool persist_config, std::string* error = nullptr);

// Encapsulates the end-to-end inference pipeline: detector creation, capture
// thread and detector thread. Constructed once (owned by the Launcher UI);
// start() / stop() can be called multiple times so the user can swap backends
// between runs without restarting Apotheosis.exe.
//
// ★ 瞄准控制链已整条移除(2026-09-17): 本会话现在【只做 采集 → 推理】,
//   检测结果落在 detectionBuffer 里供预览窗显示。原来这里还起一条 MouseThread,
//   它承担锁靶/瞄点/PID/扳机/下发整条链 —— 那部分已删除, 所以会话不再需要
//   MouseThread 引用, 也不再 join 鼠标线程。
class InferenceSession
{
public:
    InferenceSession();
    ~InferenceSession();

    InferenceSession(const InferenceSession&) = delete;
    InferenceSession& operator=(const InferenceSession&) = delete;

    bool start(const std::string& backend, const std::string& model_path);
    void stop();

    bool running() const noexcept { return running_.load(std::memory_order_acquire); }
    std::string last_error() const;

private:
    void join_all_locked();

    void stop_locked();
    mutable std::mutex mutex_;
    std::atomic<bool> running_{false};

    std::unique_ptr<IDetector> detector_owned_;
    IDetector* detector_raw_ = nullptr;

    std::thread capture_thread_;
    std::thread detector_thread_;
    std::thread heartbeat_thread_;

    std::string current_backend_;
    std::string current_model_path_;
    std::string last_error_;
};

} // namespace runtime

#endif // RUNTIME_INFERENCE_SESSION_H
