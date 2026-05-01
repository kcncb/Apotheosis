#include "tcp_capture.h"

#include <chrono>
#include <iostream>

TCPCapture::TCPCapture(int width, int height, const std::string& ip, int port)
    : width_(width)
    , height_(height)
    , ip_(ip)
    , port_(port)
    , listen_socket_(INVALID_SOCKET)
    , client_socket_(INVALID_SOCKET)
    , is_connected_(false)
    , should_stop_(false)
    , received_frames_(0)
    , dropped_frames_(0)
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "[TCPCapture] WSAStartup failed" << std::endl;
        return;
    }

    Initialize();
}

TCPCapture::~TCPCapture()
{
    Cleanup();
    WSACleanup();
}

bool TCPCapture::Initialize()
{
    if (listen_socket_ != INVALID_SOCKET)
    {
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
    }

    listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket_ == INVALID_SOCKET)
    {
        std::cerr << "[TCPCapture] Failed to create socket: " << WSAGetLastError() << std::endl;
        return false;
    }

    BOOL reuse = TRUE;
    setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(port_);
    if (ip_.empty() || ip_ == "0.0.0.0")
    {
        local_addr.sin_addr.s_addr = INADDR_ANY;
    }
    else if (inet_pton(AF_INET, ip_.c_str(), &local_addr.sin_addr) <= 0)
    {
        std::cerr << "[TCPCapture] Invalid IP address: " << ip_ << std::endl;
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
        return false;
    }

    if (bind(listen_socket_, (sockaddr*)&local_addr, sizeof(local_addr)) == SOCKET_ERROR)
    {
        std::cerr << "[TCPCapture] Failed to bind socket: " << WSAGetLastError() << std::endl;
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
        return false;
    }

    if (listen(listen_socket_, 1) == SOCKET_ERROR)
    {
        std::cerr << "[TCPCapture] Failed to listen: " << WSAGetLastError() << std::endl;
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
        return false;
    }

    u_long mode = 1;
    ioctlsocket(listen_socket_, FIONBIO, &mode);

    should_stop_ = false;
    is_connected_ = true;
    received_frames_ = 0;
    dropped_frames_ = 0;

    receive_thread_ = std::thread(&TCPCapture::ReceiveThread, this);

    std::cout << "[TCPCapture] Listening on TCP " << ip_ << ":" << port_ << std::endl;
    return true;
}

void TCPCapture::Cleanup()
{
    should_stop_ = true;
    is_connected_ = false;

    if (client_socket_ != INVALID_SOCKET)
    {
        closesocket(client_socket_);
        client_socket_ = INVALID_SOCKET;
    }
    if (listen_socket_ != INVALID_SOCKET)
    {
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
    }

    if (receive_thread_.joinable())
    {
        receive_thread_.join();
    }
}

void TCPCapture::SetTCPParams(const std::string& ip, int port)
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

cv::Mat TCPCapture::GetNextFrameCpu()
{
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (frame_queue_.empty())
        return cv::Mat();

    cv::Mat frame = frame_queue_.front();
    frame_queue_.pop();
    return frame;
}

void TCPCapture::ReceiveThread()
{
    try
    {
        std::vector<uint8_t> buffer(64 * 1024);
        std::vector<uint8_t> frame_data;
        frame_data.reserve(MAX_FRAME_SIZE);

        while (!should_stop_)
        {
            if (client_socket_ == INVALID_SOCKET)
            {
                sockaddr_in from_addr;
                int from_len = sizeof(from_addr);
                SOCKET s = accept(listen_socket_, (sockaddr*)&from_addr, &from_len);
                if (s == INVALID_SOCKET)
                {
                    int err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        continue;
                    }
                    if (should_stop_) break;
                    std::cerr << "[TCPCapture] Accept error: " << err << std::endl;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }

                u_long mode = 1;
                ioctlsocket(s, FIONBIO, &mode);
                client_socket_ = s;
                frame_data.clear();
                std::cout << "[TCPCapture] Client connected." << std::endl;
            }

            int bytes_received = recv(client_socket_, (char*)buffer.data(), (int)buffer.size(), 0);
            if (bytes_received == SOCKET_ERROR)
            {
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                std::cerr << "[TCPCapture] Receive error: " << err << std::endl;
                closesocket(client_socket_);
                client_socket_ = INVALID_SOCKET;
                frame_data.clear();
                continue;
            }

            if (bytes_received == 0)
            {
                std::cout << "[TCPCapture] Client disconnected." << std::endl;
                closesocket(client_socket_);
                client_socket_ = INVALID_SOCKET;
                frame_data.clear();
                continue;
            }

            frame_data.insert(frame_data.end(), buffer.begin(), buffer.begin() + bytes_received);

            while (true)
            {
                cv::Mat frame;
                if (!ParseMJPEGFrame(frame_data, frame))
                    break;

                if (frame.empty())
                    continue;

                if (frame.cols != width_ || frame.rows != height_)
                    cv::resize(frame, frame, cv::Size(width_, height_));

                std::lock_guard<std::mutex> lock(frame_mutex_);
                while (frame_queue_.size() >= MAX_QUEUE_SIZE)
                {
                    frame_queue_.pop();
                    dropped_frames_++;
                }
                frame_queue_.push(frame.clone());
                received_frames_++;
            }

            if (frame_data.size() > static_cast<size_t>(MAX_FRAME_SIZE))
            {
                frame_data.clear();
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[TCPCapture] Receive thread crashed: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[TCPCapture] Receive thread crashed: unknown exception." << std::endl;
    }
}

bool TCPCapture::ParseMJPEGFrame(std::vector<uint8_t>& data, cv::Mat& frame)
{
    if (data.size() < 4)
        return false;

    size_t start_pos = std::string::npos;
    for (size_t i = 0; i + 1 < data.size(); ++i)
    {
        if (data[i] == 0xFF && data[i + 1] == 0xD8)
        {
            start_pos = i;
            break;
        }
    }

    if (start_pos == std::string::npos)
    {
        data.clear();
        return false;
    }

    size_t end_pos = std::string::npos;
    for (size_t i = start_pos + 2; i + 1 < data.size(); ++i)
    {
        if (data[i] == 0xFF && data[i + 1] == 0xD9)
        {
            end_pos = i + 2;
            break;
        }
    }

    if (end_pos == std::string::npos)
    {
        if (start_pos > 0)
            data.erase(data.begin(), data.begin() + start_pos);
        return false;
    }

    std::vector<uint8_t> jpeg_data(data.begin() + start_pos, data.begin() + end_pos);
    data.erase(data.begin(), data.begin() + end_pos);

    try
    {
        frame = cv::imdecode(jpeg_data, cv::IMREAD_COLOR);
        return true;
    }
    catch (const cv::Exception& e)
    {
        std::cerr << "[TCPCapture] JPEG decode error: " << e.what() << std::endl;
        return true; // consumed the frame boundary; keep parsing
    }
}
