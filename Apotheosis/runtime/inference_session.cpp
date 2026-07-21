#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <atomic>
#include <exception>
#include <filesystem>
#include <iostream>
#include <cstring>

#include "inference_session.h"
#include "thread_loops.h"
#include "cuda_availability.h"
#include "active_hotkey.h"

#include "capture.h"
#include "mouse.h"
#include "Apotheosis.h"
#include "dml_detector.h"
#include "trt_detector.h"
#include "detector/model_inspector.h"
#include "model_crypto/model_crypto.h"
#include "auth/auth_state.h"
#include "runtime/config_snapshot.h"

extern std::atomic<bool> shouldExit;
extern std::atomic<bool> detection_resolution_changed;
extern std::atomic<bool> detector_model_changed;
extern std::atomic<bool> session_stop_requested;

namespace runtime
{
namespace
{
template <typename Func>
std::thread start_guarded(const char* name, Func func, std::atomic<bool>* running)
{
    return std::thread([name, func, running]() mutable {
        try
        {
            func();
        }
        catch (const std::exception& e)
        {
            std::cerr << "[Session] Thread '" << name << "' crashed: " << e.what() << std::endl;
            if (running) running->store(false, std::memory_order_release);
            shouldExit = true;
        }
        catch (...)
        {
            std::cerr << "[Session] Thread '" << name << "' crashed with unknown exception." << std::endl;
            if (running) running->store(false, std::memory_order_release);
            shouldExit = true;
        }
    });
}

void publish_model_metadata(IDetector* detector)
{
    if (!detector)
    {
        std::lock_guard<std::mutex> lock(g_model_metadata_mutex);
        g_model_metadata = detector::ModelMetadata{};
        return;
    }

    detector::ModelMetadata md;
    md.class_count = detector->numberOfClasses();
    md.class_names = detector->classNames();
    detector::pad_class_names(md, md.class_count);
    if (md.source == detector::ClassNamesSource::None && !md.class_names.empty())
        md.source = detector::ClassNamesSource::OnnxCustomMetadata;

    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        config.sync_class_filters_from_model(md.class_count, md.class_names);
    }
    runtime_config::publish();

    {
        std::lock_guard<std::mutex> lock(g_model_metadata_mutex);
        g_model_metadata = std::move(md);
    }
}

void publish_model_metadata(detector::ModelMetadata md)
{
    detector::pad_class_names(md, md.class_count);

    bool config_changed = false;
    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        if (md.fixed_input_size_known && config.fixed_input_size != md.fixed_input_size)
        {
            config.fixed_input_size = md.fixed_input_size;
            config_changed = true;
            std::cout << "[ModelInspector] Automatically set fixed_input_size = "
                      << (md.fixed_input_size ? "true" : "false") << std::endl;
        }
        config.sync_class_filters_from_model(md.class_count, md.class_names);
    }
    runtime_config::publish();

    {
        std::lock_guard<std::mutex> lock(g_model_metadata_mutex);
        g_model_metadata = std::move(md);
    }

    if (config_changed)
        detector_model_changed.store(true);
}

detector::ModelMetadata inspect_model_metadata_for_ui(const std::string& model_path)
{
    std::filesystem::path path(std::filesystem::u8path(model_path));
    const std::string ext = path.extension().u8string();
    if (_stricmp(ext.c_str(), ".onnx") == 0 || oliver::is_oliver_path(model_path))
        return detector::inspect_onnx_model(model_path, runtime_config::read()->verbose);

    detector::ModelMetadata md;
    if (_stricmp(ext.c_str(), ".engine") == 0)
    {
        const std::string header = detector::read_ultralytics_engine_header(model_path);
        if (!header.empty())
        {
            md.class_names = detector::parse_json_names(header);
            if (!md.class_names.empty())
                md.source = detector::ClassNamesSource::TrtEngineHeader;
        }
    }

    if (md.class_names.empty())
    {
        detector::ClassNamesSource source = detector::ClassNamesSource::None;
        auto sidecar = detector::read_sidecar_class_names(model_path, &source);
        if (!sidecar.empty())
        {
            md.class_names = std::move(sidecar);
            md.source = source;
        }
    }

    md.class_count = static_cast<int>(md.class_names.size());
    detector::pad_class_names(md, md.class_count);
    return md;
}

} // namespace

bool preload_model_metadata(const std::string& model_path, bool persist_config, std::string* error)
{
    if (error)
        error->clear();

    if (!std::filesystem::exists(std::filesystem::u8path(model_path)))
    {
        if (error)
            *error = "model does not exist: " + model_path;
        return false;
    }

    // 单机自用：跳过 oliver 密钥/心跳检查
    detector::ModelMetadata md = inspect_model_metadata_for_ui(model_path);
    publish_model_metadata(std::move(md));

    if (persist_config)
        config.saveConfig("config.ini");

    std::cout << "[ModelInspector] Preloaded metadata for " << model_path << std::endl;
    return true;
}

InferenceSession::InferenceSession(MouseThread& mouse_driver)
    : mouse_driver_(mouse_driver)
{
}

InferenceSession::~InferenceSession()
{
    stop();
}

bool InferenceSession::start(const std::string& backend, const std::string& model_path)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (running_.load(std::memory_order_acquire))
    {
        last_error_ = "session already running";
        return false;
    }

    current_backend_ = backend;
    current_model_path_ = model_path;
    last_error_.clear();

    // 单机自用：跳过 oliver 密钥/心跳检查
    if (backend == "DML")
    {
        auto dml = std::make_unique<DirectMLDetector>();
        if (!dml->initialize(model_path))
        {
            last_error_ = "DirectML detector initialization failed";
            detector_owned_.reset();
            detector_raw_ = nullptr;
            return false;
        }
        detector_raw_ = dml.get();
        detector_owned_ = std::move(dml);
        dml_detector = static_cast<DirectMLDetector*>(detector_raw_);
        g_detector = detector_raw_;
    }
    else if (backend == "TRT")
    {
        if (!is_tensorrt_available())
        {
            last_error_ = "TensorRT runtime not available: " + probe_cuda_runtime().failure_reason;
            return false;
        }
        if (!trt_detector.initialize(model_path))
        {
            last_error_ = "TensorRT detector initialization failed";
            return false;
        }
        detector_raw_ = &trt_detector;
        g_detector = detector_raw_;
    }
    else
    {
        last_error_ = "unknown backend: " + backend;
        return false;
    }

    publish_model_metadata(detector_raw_);

    createInputDevices();
    assignInputDevices();
    input_method_changed.store(false);

    detection_resolution_changed.store(true);
    detector_model_changed.store(false);

    session_stop_requested.store(false);

    int capture_resolution = 320;
    {
        std::lock_guard<std::recursive_mutex> config_lock(configMutex);
        capture_resolution = runtime_config::read()->detection_resolution;
    }
    running_.store(true, std::memory_order_release);

    capture_thread_ = start_guarded("CaptureThread", [capture_resolution] {
        captureThread(capture_resolution, capture_resolution);
    }, &running_);

    detector_thread_ = start_guarded("DetectorThread", [] {
        if (g_detector)
            g_detector->inferenceThread();
    }, &running_);

    mouse_thread_ = start_guarded("MouseThread", [this] {
        mouseThreadFunction(mouse_driver_);
    }, &running_);

    // 单机自用：不再启动鉴权心跳线程
    std::cout << "[Session] Started with backend=" << backend
              << " model=" << model_path << std::endl;
    return true;
}

void InferenceSession::stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.load(std::memory_order_acquire) &&
        !capture_thread_.joinable() &&
        !detector_thread_.joinable() &&
        !mouse_thread_.joinable())
    {
        return;
    }

    session_stop_requested.store(true);
    if (detector_raw_)
        detector_raw_->requestExit();
    detectionBuffer.cv.notify_all();
    frameCV.notify_all();

    join_all_locked();

    g_detector = nullptr;
    dml_detector = nullptr;
    detector_raw_ = nullptr;
    detector_owned_.reset();
    publish_model_metadata(nullptr);

    running_.store(false, std::memory_order_release);
    std::cout << "[Session] Stopped." << std::endl;
}

void InferenceSession::join_all_locked()
{
    auto safe_join = [](const char* name, std::thread& t) {
        if (t.joinable() && t.get_id() != std::this_thread::get_id())
        {
            std::cout << "[Session] Joining " << name << "..." << std::endl;
            t.join();
            std::cout << "[Session] Joined " << name << "." << std::endl;
        }
        else if (t.joinable())
        {
            std::cout << "[Session] Detaching " << name << " from its own thread." << std::endl;
            t.detach();
        }
    };

    safe_join("capture", capture_thread_);
    safe_join("detector", detector_thread_);
    safe_join("mouse", mouse_thread_);
    safe_join("heartbeat", heartbeat_thread_);
}

} // namespace runtime


