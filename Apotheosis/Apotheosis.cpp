#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

#include "capture.h"
#include "mouse.h"
#include "Apotheosis.h"
#include "keyboard_listener.h"
#include "overlay.h"
#include "overlay/app_log.h"
#include "ghub.h"
#include "other_tools.h"
#include "mem/gpu_resource_manager.h"
#include "mem/cpu_affinity_manager.h"
#include "runtime/cuda_availability.h"
#include "runtime/inference_session.h"
#include "runtime/thread_loops.h"
#include "detector/dml_detector.h"
#include "auth/auth_state.h"

#include "depth/depth_anything_trt.h"
#include "depth/depth_mask.h"
#include "tensorrt/nvinf.h"

std::condition_variable frameCV;
std::atomic<bool> shouldExit(false);
std::atomic<bool> aiming(false);
std::atomic<bool> session_stop_requested(true); // starts true: no session running yet
std::recursive_mutex configMutex;

TrtDetector trt_detector;

DirectMLDetector* dml_detector = nullptr;
IDetector* g_detector = nullptr;
runtime::InferenceSession* g_inference_session = nullptr;
MouseThread* globalMouseThread = nullptr;
Config config;


GhubMouse* gHub = nullptr;
Arduino* arduinoSerial = nullptr;
KmboxNetConnection* kmboxNetSerial = nullptr;
KmboxAConnection* kmboxASerial = nullptr;
MakcuConnection* makcuSerial = nullptr;

std::atomic<bool> detection_resolution_changed(false);
std::atomic<bool> capture_method_changed(false);
std::atomic<bool> capture_fps_changed(false);
std::atomic<bool> detector_model_changed(false);
std::atomic<bool> input_method_changed(false);


std::string g_iconLastError;

static int FatalExit(const std::string& message)
{
    std::cerr << message << std::endl;
    MessageBoxA(nullptr, message.c_str(), "Apotheosis", MB_ICONERROR | MB_OK);
    return -1;
}

static void HandleThreadCrash(const char* name, const std::exception* ex)
{
    std::cerr << "[Thread] " << name << " crashed: "
              << (ex ? ex->what() : "unknown exception") << std::endl;
    shouldExit = true;
    detectionBuffer.cv.notify_all();
}

template <typename Func>
static std::thread StartThreadGuarded(const char* name, Func func)
{
    return std::thread([name, func]() mutable {
        try
        {
            func();
        }
        catch (const std::exception& e)
        {
            HandleThreadCrash(name, &e);
        }
        catch (...)
        {
            HandleThreadCrash(name, nullptr);
        }
    });
}

void createInputDevices()
{
    if (arduinoSerial)
    {
        delete arduinoSerial;
        arduinoSerial = nullptr;
    }

    if (gHub)
    {
        gHub->mouse_close();
        delete gHub;
        gHub = nullptr;
    }

    if (kmboxNetSerial)
    {
        delete kmboxNetSerial;
        kmboxNetSerial = nullptr;
    }

    if (kmboxASerial)
    {
        delete kmboxASerial;
        kmboxASerial = nullptr;
    }

    if (makcuSerial)
    {
        delete makcuSerial;
        makcuSerial = nullptr;
    }

    if (config.input_method == "ARDUINO")
    {
        std::cout << "[Mouse] Using Arduino method input." << std::endl;
        arduinoSerial = new Arduino(config.arduino_port, config.arduino_baudrate);
    }
    else if (config.input_method == "GHUB")
    {
        std::cout << "[Mouse] Using Ghub method input." << std::endl;
        gHub = new GhubMouse();
        if (!gHub->mouse_xy(0, 0))
        {
            std::cerr << "[Ghub] Error with opening mouse." << std::endl;
            delete gHub;
            gHub = nullptr;
        }
    }
    else if (config.input_method == "KMBOX_NET")
    {
        std::cout << "[Mouse] Using KMBOX_NET input." << std::endl;
        kmboxNetSerial = new KmboxNetConnection(config.kmbox_net_ip, config.kmbox_net_port, config.kmbox_net_uuid);
        if (!kmboxNetSerial->isOpen())
        {
            std::cerr << "[KmboxNet] Error connecting." << std::endl;
            delete kmboxNetSerial;
            kmboxNetSerial = nullptr;
        }
    }
    else if (config.input_method == "KMBOX_A")
    {
        std::cout << "[Mouse] Using KMBOX_A input." << std::endl;
        if (config.kmbox_a_pidvid.empty())
        {
            std::cerr << "[KmboxA] PIDVID is empty." << std::endl;
            return;
        }
        kmboxASerial = new KmboxAConnection(config.kmbox_a_pidvid);
        if (!kmboxASerial->isOpen())
        {
            std::cerr << "[KmboxA] Error connecting." << std::endl;
            delete kmboxASerial;
            kmboxASerial = nullptr;
        }
    }
    else if (config.input_method == "MAKCU")
    {
        std::cout << "[Mouse] Using MAKCU input." << std::endl;
        makcuSerial = new MakcuConnection(config.makcu_port, config.makcu_baudrate);
        if (!makcuSerial->isOpen())
        {
            std::cerr << "[Makcu] Error connecting." << std::endl;
            delete makcuSerial;
            makcuSerial = nullptr;
        }
    }
    else
    {
        std::cout << "[Mouse] Using default Win32 method input." << std::endl;
    }
}

void assignInputDevices()
{
    if (globalMouseThread)
    {
        globalMouseThread->setArduinoConnection(arduinoSerial);
        globalMouseThread->setGHubMouse(gHub);
        globalMouseThread->setKmboxAConnection(kmboxASerial);
        globalMouseThread->setKmboxNetConnection(kmboxNetSerial);
        globalMouseThread->setMakcuConnection(makcuSerial);
    }
}


int main()
{
    AppLog::InstallStdStreamCapture();

    SetConsoleOutputCP(CP_UTF8);
    SetRandomConsoleTitle();
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_FATAL);

    {
        wchar_t exePath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0)
        {
            std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
            std::error_code ec;
            std::filesystem::current_path(exeDir, ec);
            if (ec && config.verbose)
            {
                std::cout << "[Config] Failed to set working dir: " << exeDir.u8string()
                          << " (" << ec.message() << ")" << std::endl;
            }
        }
    }

    if (!config.loadConfig())
    {
        std::cerr << "[Config] Error with loading config!" << std::endl;
        return FatalExit("[Config] Error with loading config!");
    }

    auth::state().initialize(config.auth_server_url);
    auth::state().try_restore_session();

    CPUAffinityManager cpuManager;

    if (config.cpuCoreReserveCount > 0)
    {
        if (!cpuManager.reserveCPUCores(config.cpuCoreReserveCount))
            return FatalExit("[MAIN] Failed to reserve CPU cores.");
    }

    if (config.systemMemoryReserveMB > 0)
    {
        if (!cpuManager.reserveSystemMemory(config.systemMemoryReserveMB))
            return FatalExit("[MAIN] Failed to reserve system memory.");
    }

    try
    {
        const auto& cudaStatus = runtime::probe_cuda_runtime();
        if (config.verbose)
        {
            std::cout << "[CUDA] Probe: cudart=" << cudaStatus.cudart_loadable
                      << " nvinfer=" << cudaStatus.nvinfer_loadable
                      << " nvonnxparser=" << cudaStatus.nvonnxparser_loadable
                      << " devices=" << cudaStatus.device_count
                      << " version=" << cudaStatus.cuda_runtime_version << std::endl;
        }

        if (config.backend == "TRT" && !cudaStatus.trt_ready())
        {
            std::cerr << "[MAIN] TRT backend requested but unavailable: "
                      << cudaStatus.failure_reason << ". Falling back to DML." << std::endl;
            config.backend = "DML";
            config.saveConfig();
        }

        if (cudaStatus.trt_ready())
        {
            const int required_cuda_version = 12090;
            const int max_supported_cuda_version = 12099;
            if (cudaStatus.cuda_runtime_version < required_cuda_version ||
                cudaStatus.cuda_runtime_version > max_supported_cuda_version)
            {
                const int runtime_major = cudaStatus.cuda_runtime_version / 1000;
                const int runtime_minor = (cudaStatus.cuda_runtime_version % 1000) / 10;
                std::cerr << "[MAIN] CUDA 12.9 targeted. Detected "
                          << runtime_major << "." << runtime_minor
                          << ". TRT backend may misbehave." << std::endl;
            }

            GPUResourceManager gpuManager;
            if (config.backend == "TRT")
            {
                if (config.gpuMemoryReserveMB > 0)
                {
                    if (!gpuManager.reserveGPUMemory(config.gpuMemoryReserveMB))
                        return FatalExit("[MAIN] Failed to reserve GPU memory.");
                }

                if (config.enableGpuExclusiveMode)
                {
                    if (!gpuManager.setGPUExclusiveMode())
                        return FatalExit("[MAIN] Failed to set GPU exclusive mode.");
                }
            }
        }
        if (!CreateDirectory(L"screenshots", NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            std::cout << "[MAIN] Error with screenshot folder" << std::endl;
            return FatalExit("[MAIN] Error with screenshot folder");
        }

        if (!CreateDirectory(L"models", NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            std::cout << "[MAIN] Error with models folder" << std::endl;
            return FatalExit("[MAIN] Error with models folder");
        }
        if (!CreateDirectory(L"models\\depth", NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            std::cout << "[MAIN] Error with models\\depth folder" << std::endl;
            return FatalExit("[MAIN] Error with models\\depth folder");
        }
        if (!CreateDirectory(L"models\\engines", NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            std::cout << "[MAIN] Error with models\\engines folder" << std::endl;
            return FatalExit("[MAIN] Error with models\\engines folder");
        }

        std::string modelPath = "models/" + config.ai_model;

        if (!std::filesystem::exists(std::filesystem::u8path(modelPath)))
        {
            std::cerr << "[MAIN] Specified model does not exist: " << modelPath << std::endl;

            std::vector<std::string> modelFiles = getModelFiles();

            if (!modelFiles.empty())
            {
                config.ai_model = modelFiles[0];
                config.saveConfig();
                std::cout << "[MAIN] Loaded first available model: " << config.ai_model << std::endl;
            }
            else
            {
                std::cerr << "[MAIN] No models found in 'models' directory." << std::endl;
                return FatalExit("[MAIN] No models found in 'models' directory.");
            }
        }

        // MouseThread is built with generic defaults; the mouse loop will
        // overwrite params as soon as the first aim hotkey becomes active.
        MouseRuntimeParams mouse_params{};
        mouse_params.detection_resolution = config.detection_resolution;

        MouseThread mouseThread(mouse_params);

        globalMouseThread = &mouseThread;

        std::vector<std::string> availableModels = getAvailableModels();

        if (!config.ai_model.empty())
        {
            std::string candidate = "models/" + config.ai_model;
            if (!std::filesystem::exists(std::filesystem::u8path(candidate)))
            {
                std::cerr << "[MAIN] Specified model does not exist: " << candidate << std::endl;

                if (!availableModels.empty())
                {
                    config.ai_model = availableModels[0];
                    config.saveConfig("config.ini");
                    std::cout << "[MAIN] Loaded first available model: " << config.ai_model << std::endl;
                }
                else
                {
                    std::cerr << "[MAIN] No models found in 'models' directory." << std::endl;
                    return FatalExit("[MAIN] No models found in 'models' directory.");
                }
            }
        }
        else
        {
            if (!availableModels.empty())
            {
                config.ai_model = availableModels[0];
                config.saveConfig();
                std::cout << "[MAIN] No AI model specified in config. Loaded first available model: " << config.ai_model << std::endl;
            }
            else
            {
                std::cerr << "[MAIN] No AI models found in 'models' directory." << std::endl;
                return FatalExit("[MAIN] No AI models found in 'models' directory.");
            }
        }

        {
            const auto dmlAdapters = EnumerateDMLAdapters();
            if (!dmlAdapters.empty())
            {
                auto selected = std::find_if(dmlAdapters.begin(), dmlAdapters.end(), [](const DmlAdapterInfo& adapter) {
                    return adapter.device_id == config.dml_device_id;
                });
                if (selected == dmlAdapters.end())
                {
                    config.dml_device_id = dmlAdapters.front().device_id;
                    config.saveConfig("config.ini");
                    selected = dmlAdapters.begin();
                }

                std::cout << "[MAIN] DirectML adapters detected:" << std::endl;
                for (const auto& adapter : dmlAdapters)
                    std::cout << "  [" << adapter.device_id << "] " << adapter.name
                              << (adapter.device_id == config.dml_device_id ? " (selected)" : "") << std::endl;
            }
            else
            {
                std::cerr << "[MAIN] No DirectML/DXGI adapters detected." << std::endl;
            }
        }

        {
            std::string preloadError;
            runtime::preload_model_metadata(std::string("models/") + config.ai_model, true, &preloadError);
            if (!preloadError.empty())
                std::cerr << "[MAIN] Model metadata preload failed: " << preloadError << std::endl;
        }

        // Launcher workflow: construct the session but do NOT start it. The
        // overlay UI (see draw_session.cpp) owns start/stop via its buttons so
        // the user can pick backend/model before any capture or inference runs.
        runtime::InferenceSession session(mouseThread);
        g_inference_session = &session;

        if (GetEnvironmentVariableA("APOTHEOSIS_AUTOSTART_TRT", nullptr, 0) > 0)
        {
            config.backend = "TRT";
            std::cout << "[MAIN] APOTHEOSIS_AUTOSTART_TRT=1, starting TensorRT session." << std::endl;
            session.start(config.backend, std::string("models/") + config.ai_model);
        }

        std::thread keyThread = StartThreadGuarded("KeyboardListener", [] {
            keyboardListener();
        });
        std::thread overlayThread = StartThreadGuarded("OverlayThread", [] {
            OverlayThread();
        });

        welcome_message();

        keyThread.join();
        overlayThread.join();

        session.stop();
        g_inference_session = nullptr;

        if (arduinoSerial)
        {
            delete arduinoSerial;
            arduinoSerial = nullptr;
        }

        if (gHub)
        {
            gHub->mouse_close();
            delete gHub;
            gHub = nullptr;
        }

        if (kmboxASerial)
        {
            delete kmboxASerial;
            kmboxASerial = nullptr;
        }

        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[MAIN] An error has occurred in the main stream: " << e.what() << std::endl;
        return FatalExit(std::string("[MAIN] An error has occurred in the main stream: ") + e.what());
    }
}



