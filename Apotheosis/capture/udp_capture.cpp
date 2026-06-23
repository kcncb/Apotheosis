#include "udp_capture.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>

#include "gpu_color_ops.h"

UDPCapture::UDPCapture(int width, int height, const std::string& ip, int port)
    : width_(width)
    , height_(height)
    , ip_(ip)
    , port_(port)
    , socket_(INVALID_SOCKET)
    , is_connected_(false)
    , should_stop_(false)
    , received_frames_(0)
    , dropped_frames_(0)
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "[UDPCapture] WSAStartup failed" << std::endl;
        return;
    }

    Initialize();
}

UDPCapture::~UDPCapture()
{
    Cleanup();
    if (pinned_jpeg_buffer_)
    {
        cudaFreeHost(pinned_jpeg_buffer_);
        pinned_jpeg_buffer_ = nullptr;
        pinned_jpeg_capacity_ = 0;
    }
    for (auto& evt : decode_events_)
    {
        if (evt)
        {
            cudaEventDestroy(evt);
            evt = nullptr;
        }
    }
    if (decode_stream_)
    {
        cudaStreamDestroy(decode_stream_);
        decode_stream_ = nullptr;
    }
    WSACleanup();
}

bool UDPCapture::Initialize()
{
    if (socket_ != INVALID_SOCKET)
        closesocket(socket_);

    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET)
    {
        std::cerr << "[UDPCapture] Failed to create socket: " << WSAGetLastError() << std::endl;
        return false;
    }

    int buffer_size = MAX_FRAME_SIZE;
    if (setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, (char*)&buffer_size, sizeof(buffer_size)) == SOCKET_ERROR)
    {
        std::cerr << "[UDPCapture] Failed to set receive buffer size: " << WSAGetLastError() << std::endl;
    }

    u_long mode = 1;
    if (ioctlsocket(socket_, FIONBIO, &mode) == SOCKET_ERROR)
    {
        std::cerr << "[UDPCapture] Failed to set non-blocking mode: " << WSAGetLastError() << std::endl;
    }

    memset(&server_addr_, 0, sizeof(server_addr_));
    server_addr_.sin_family = AF_INET;
    server_addr_.sin_port = htons(port_);
    if (inet_pton(AF_INET, ip_.c_str(), &server_addr_.sin_addr) <= 0)
    {
        std::cerr << "[UDPCapture] Invalid IP address: " << ip_ << std::endl;
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return false;
    }

    sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(port_);
    if (bind(socket_, (sockaddr*)&local_addr, sizeof(local_addr)) == SOCKET_ERROR)
    {
        std::cerr << "[UDPCapture] Failed to bind socket: " << WSAGetLastError() << std::endl;
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return false;
    }

    should_stop_ = false;
    is_connected_ = true;
    received_frames_ = 0;
    dropped_frames_ = 0;

    receive_thread_ = std::thread(&UDPCapture::ReceiveThread, this);

    std::cout << "[UDPCapture] Listening on UDP " << ip_ << ":" << port_ << std::endl;
    return true;
}

void UDPCapture::Cleanup()
{
    should_stop_ = true;
    is_connected_ = false;

    if (receive_thread_.joinable())
    {
        receive_thread_.join();
    }

    if (socket_ != INVALID_SOCKET)
    {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

void UDPCapture::SetUDPParams(const std::string& ip, int port)
{
    if (ip_ != ip || port_ != port)
    {
        ip_ = ip;
        port_ = port;

        if (is_connected_)
        {
            Cleanup();
            Initialize();
        }
    }
}

cv::Mat UDPCapture::GetNextFrameCpu()
{
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (frame_queue_.empty())
        return cv::Mat();

    cv::Mat frame = frame_queue_.front();
    frame_queue_.pop();
    return frame;
}

GpuImage UDPCapture::GetNextFrameGpu()
{
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (gpu_frame_queue_.empty())
        return GpuImage();

    GpuImage frame = std::move(gpu_frame_queue_.front());
    gpu_frame_queue_.pop();
    return frame;
}

void UDPCapture::ReceiveThread()
{
    try
    {
        // Initialize nvJPEG + its dedicated stream lazily on this thread so
        // the handle/state sits on the thread that uses them. If init fails
        // (older driver, OOM, etc.) we transparently fall back to the CPU
        // cv::imdecode + CPU queue path.
        if (!gpu_decoder_)
        {
            gpu_decoder_ = std::make_unique<capture::GpuJpegDecoder>();
            if (!gpu_decoder_->init())
            {
                std::cerr << "[UDPCapture] nvJPEG unavailable; using CPU cv::imdecode" << std::endl;
                gpu_decoder_.reset();
            }
            else if (!decode_stream_)
            {
                cudaStreamCreateWithFlags(&decode_stream_, cudaStreamNonBlocking);
                for (auto& evt : decode_events_)
                    cudaEventCreateWithFlags(&evt, cudaEventDisableTiming);
            }
        }

        std::vector<uint8_t> buffer(MAX_FRAME_SIZE);
        std::vector<uint8_t> frame_data;

        while (!should_stop_)
        {
            sockaddr_in from_addr;
            int from_len = sizeof(from_addr);

            int bytes_received = recvfrom(
                socket_,
                (char*)buffer.data(),
                (int)buffer.size(),
                0,
                (sockaddr*)&from_addr,
                &from_len
            );

            if (bytes_received == SOCKET_ERROR)
            {
                int error = WSAGetLastError();
                if (error == WSAEWOULDBLOCK)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }

                std::cerr << "[UDPCapture] Receive error: " << error << std::endl;
                break;
            }

            if (bytes_received <= 0)
                continue;

            if (server_addr_.sin_addr.s_addr != 0 &&
                from_addr.sin_addr.s_addr != server_addr_.sin_addr.s_addr)
            {
                continue;
            }

            frame_data.insert(frame_data.end(), buffer.begin(), buffer.begin() + bytes_received);
            if (frame_data.size() > MAX_FRAME_SIZE * 2)
            {
                frame_data.clear();
                continue;
            }

            // Locate JPEG SOI/EOI markers in the accumulated UDP byte stream.
            size_t start_pos = 0, end_pos = 0;
            if (!FindJpegBounds(frame_data, start_pos, end_pos))
                continue;

            const uint8_t* jpeg = frame_data.data() + start_pos;
            const size_t jpeg_size = end_pos - start_pos;

            bool gpu_ok = false;
            if (gpu_decoder_ && decode_stream_)
            {
                // Stage the JPEG bytes into a pinned host buffer so nvjpeg's
                // internal host->device copy is async/pageable-free. Grow
                // lazily up to MAX_FRAME_SIZE.
                if (pinned_jpeg_capacity_ < jpeg_size)
                {
                    if (pinned_jpeg_buffer_) cudaFreeHost(pinned_jpeg_buffer_);
                    pinned_jpeg_buffer_ = nullptr;
                    pinned_jpeg_capacity_ = 0;
                    const size_t want = std::max<size_t>(jpeg_size, 64 * 1024);
                    if (cudaHostAlloc(reinterpret_cast<void**>(&pinned_jpeg_buffer_),
                                      want, cudaHostAllocDefault) == cudaSuccess)
                    {
                        pinned_jpeg_capacity_ = want;
                    }
                }

                const uint8_t* decode_src = jpeg;
                if (pinned_jpeg_buffer_ && pinned_jpeg_capacity_ >= jpeg_size)
                {
                    std::memcpy(pinned_jpeg_buffer_, jpeg, jpeg_size);
                    decode_src = pinned_jpeg_buffer_;
                }

                // Decode into the next ring slot rather than a fresh GpuImage:
                // GpuImage::create() reuses the slot's device buffer in steady
                // state, so the hot path no longer pays a device-synchronizing
                // cudaMalloc (decode) + cudaFree (consume) every frame.
                GpuImage& decoded = decode_pool_[decode_pool_index_];
                if (gpu_decoder_->decode(decode_src, jpeg_size, decoded, decode_stream_))
                {
                    // Sender is expected to emit frames already at width_/height_
                    // (configured detection_resolution). If it doesn't, resize
                    // on GPU so downstream sees a consistent square frame.
                    GpuImage finalFrame = decoded;
                    if (!decoded.empty() && (decoded.cols() != width_ || decoded.rows() != height_))
                    {
                        GpuImage& resized = resize_pool_[decode_pool_index_];
                        if (resized.create(height_, width_, 3))
                        {
                            launch_resize_bgr_u8_bilinear(
                                decoded.data(), decoded.step(), decoded.cols(), decoded.rows(),
                                resized.data(), resized.step(), width_, height_,
                                decode_stream_);
                            // Share (not move) so the ring slot keeps owning the
                            // buffer for reuse next cycle.
                            finalFrame = resized;
                        }
                    }

                    // Record (don't wait for) decode+resize completion on this
                    // slot's event. The detector waits on it from its own stream
                    // via cudaStreamWaitEvent, so decode and inference overlap
                    // instead of being serialized by a CPU sync here. The event
                    // tracks decode_stream_'s progress, so it covers both the
                    // decode-only and the resized cases.
                    cudaEvent_t slot_event = decode_events_[decode_pool_index_];
                    if (slot_event)
                    {
                        cudaEventRecord(slot_event, decode_stream_);
                        finalFrame.setReadyEvent(slot_event);
                    }
                    else
                    {
                        // Event creation failed: fall back to the safe sync so
                        // the detector never reads an incomplete decode.
                        cudaStreamSynchronize(decode_stream_);
                    }

                    decode_pool_index_ = (decode_pool_index_ + 1) % DECODE_POOL_SIZE;

                    std::lock_guard<std::mutex> lock(frame_mutex_);
                    while (gpu_frame_queue_.size() >= MAX_QUEUE_SIZE)
                    {
                        gpu_frame_queue_.pop();
                        dropped_frames_++;
                    }
                    gpu_frame_queue_.push(std::move(finalFrame));
                    received_frames_++;
                    gpu_ok = true;
                }
            }

            if (!gpu_ok)
            {
                // CPU fallback: cv::imdecode + CPU resize + CPU queue.
                cv::Mat frame;
                std::vector<uint8_t> jpeg_data(jpeg, jpeg + jpeg_size);
                try
                {
                    frame = cv::imdecode(jpeg_data, cv::IMREAD_COLOR);
                }
                catch (const cv::Exception& e)
                {
                    std::cerr << "[UDPCapture] JPEG decode error: " << e.what() << std::endl;
                }

                if (!frame.empty())
                {
                    if (frame.cols != width_ || frame.rows != height_)
                        cv::resize(frame, frame, cv::Size(width_, height_));

                    std::lock_guard<std::mutex> lock(frame_mutex_);
                    while (frame_queue_.size() >= MAX_QUEUE_SIZE)
                    {
                        frame_queue_.pop();
                        dropped_frames_++;
                    }
                    frame_queue_.push(std::move(frame));
                    received_frames_++;
                }
            }

            frame_data.clear();
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[UDPCapture] Receive thread crashed: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[UDPCapture] Receive thread crashed: unknown exception." << std::endl;
    }
}

bool UDPCapture::FindJpegBounds(const std::vector<uint8_t>& data, size_t& start_pos, size_t& end_pos)
{
    if (data.size() < 4) return false;

    bool found_start = false;
    for (size_t i = 0; i + 1 < data.size(); ++i)
    {
        if (data[i] == 0xFF && data[i + 1] == 0xD8)
        {
            start_pos = i;
            found_start = true;
            break;
        }
    }
    if (!found_start) return false;

    for (size_t i = start_pos + 2; i + 1 < data.size(); ++i)
    {
        if (data[i] == 0xFF && data[i + 1] == 0xD9)
        {
            end_pos = i + 2;
            return true;
        }
    }
    return false;
}
