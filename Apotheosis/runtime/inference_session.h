#ifndef RUNTIME_INFERENCE_SESSION_H
#define RUNTIME_INFERENCE_SESSION_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class IDetector;
class MouseThread;

namespace runtime
{

bool preload_model_metadata(const std::string& model_path, bool persist_config, std::string* error = nullptr);

// Encapsulates the end-to-end inference pipeline: detector creation, capture
// thread, detector thread and mouse thread. Constructed once (owned by the
// Launcher UI); start() / stop() can be called multiple times so the user can
// swap backends between runs without restarting ai.exe.
class InferenceSession
{
public:
    explicit InferenceSession(MouseThread& mouse_driver);
    ~InferenceSession();

    InferenceSession(const InferenceSession&) = delete;
    InferenceSession& operator=(const InferenceSession&) = delete;

    bool start(const std::string& backend, const std::string& model_path);
    void stop();

    bool running() const noexcept { return running_.load(std::memory_order_acquire); }
    IDetector* detector() const noexcept { return detector_raw_; }
    const std::string& current_backend() const noexcept { return current_backend_; }
    const std::string& current_model_path() const noexcept { return current_model_path_; }
    const std::string& last_error() const noexcept { return last_error_; }

private:
    void join_all_locked();

    std::mutex mutex_;
    std::atomic<bool> running_{false};

    std::unique_ptr<IDetector> detector_owned_;
    IDetector* detector_raw_ = nullptr;

    MouseThread& mouse_driver_;

    std::thread capture_thread_;
    std::thread detector_thread_;
    std::thread mouse_thread_;
    std::thread heartbeat_thread_;

    std::string current_backend_;
    std::string current_model_path_;
    std::string last_error_;
};

} // namespace runtime

#endif // RUNTIME_INFERENCE_SESSION_H
