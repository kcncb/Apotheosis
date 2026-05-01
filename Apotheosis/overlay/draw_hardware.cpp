#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <shellapi.h>

#include <cstring>
#include <string>
#include <vector>

#include "imgui/imgui.h"

#include "draw_settings.h"
#include "include/other_tools.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"
#include "Apotheosis.h"

namespace
{
const std::string& cached_ghub_version()
{
    static const std::string v = get_ghub_version();
    return v;
}

void reconnect_input_device_now()
{
    if (globalMouseThread)
    {
        globalMouseThread->setArduinoConnection(nullptr);
        globalMouseThread->setGHubMouse(nullptr);
        globalMouseThread->setKmboxAConnection(nullptr);
        globalMouseThread->setKmboxNetConnection(nullptr);
        globalMouseThread->setMakcuConnection(nullptr);
    }

    createInputDevices();
    assignInputDevices();
    input_method_changed.store(false);
}

void draw_move_test_button()
{
    if (!ImGui::Button(u8"移动测试"))
        return;

    if (config.input_method == "GHUB" && gHub)
    {
        gHub->mouse_xy(80, 0);
        gHub->mouse_xy(-80, 0);
    }
    else if (config.input_method == "ARDUINO" && arduinoSerial && arduinoSerial->isOpen())
    {
        arduinoSerial->move(80, 0);
        arduinoSerial->move(-80, 0);
    }
    else if (config.input_method == "KMBOX_NET" && kmboxNetSerial && kmboxNetSerial->isOpen())
    {
        kmboxNetSerial->move(80, 0);
        kmboxNetSerial->move(-80, 0);
    }
    else if (config.input_method == "KMBOX_A" && kmboxASerial && kmboxASerial->isOpen())
    {
        kmboxASerial->move(80, 0);
        kmboxASerial->move(-80, 0);
    }
    else if (config.input_method == "MAKCU" && makcuSerial && makcuSerial->isOpen())
    {
        makcuSerial->move(80, 0);
        makcuSerial->move(-80, 0);
    }
}

void draw_connection_state(bool connected, const char* connected_text, const char* disconnected_text)
{
    if (connected)
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "%s", connected_text);
    else
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", disconnected_text);
}
} // namespace

void draw_hardware()
{
    if (OverlayUI::BeginSection(u8"输入方式", "hardware_section_input_method"))
    {
        const std::vector<std::string> methods = {
            "WIN32", "GHUB", "ARDUINO", "KMBOX_NET", "KMBOX_A", "MAKCU"
        };
        std::vector<const char*> method_items;
        method_items.reserve(methods.size());
        for (const auto& m : methods) method_items.push_back(m.c_str());

        int idx = 0;
        for (size_t i = 0; i < methods.size(); ++i)
            if (methods[i] == config.input_method) { idx = static_cast<int>(i); break; }

        if (ImGui::Combo(u8"输入设备", &idx, method_items.data(), static_cast<int>(method_items.size())))
        {
            const std::string next = methods[idx];
            if (next != config.input_method)
            {
                config.input_method = next;
                OverlayConfig_MarkDirty();
                reconnect_input_device_now();
            }
        }

        OverlayUI::EndSection();
    }

    if (config.input_method == "WIN32")
    {
        if (OverlayUI::BeginSection(u8"Win32", "hardware_section_win32"))
        {
            ImGui::TextWrapped(u8"标准 SendInput 接口；多数反作弊游戏会检测到。建议仅在测试时使用。");
            OverlayUI::EndSection();
        }
    }
    else if (config.input_method == "GHUB")
    {
        if (OverlayUI::BeginSection(u8"Logitech G HUB", "hardware_section_ghub"))
        {
            const auto& ver = cached_ghub_version();
            if (ver == "13.1.4")
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), u8"G HUB 版本正确：%s", ver.c_str());
            else
            {
                ImGui::TextWrapped(u8"未检测到预期版本的 G HUB（需要 13.1.4，默认安装路径：C:\\Program Files\\LGHUB）。");
                if (ImGui::Button(u8"查看文档"))
                {
                    ShellExecute(nullptr, nullptr,
                        L"https://github.com/Apotheosis/Apotheosis/blob/main/docs/guides.md#g-hub-input-method",
                        nullptr, nullptr, SW_SHOW);
                }
            }
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), u8"注意：部分游戏会检测此方式。");
            draw_move_test_button();
            OverlayUI::EndSection();
        }
    }
    else if (config.input_method == "ARDUINO")
    {
        if (OverlayUI::BeginSection(u8"Arduino", "hardware_section_arduino"))
        {
            draw_connection_state(arduinoSerial && arduinoSerial->isOpen(), u8"Arduino 已连接", u8"Arduino 未连接");

            std::vector<std::string> ports;
            for (int i = 1; i <= 30; ++i) ports.push_back("COM" + std::to_string(i));
            std::vector<const char*> port_items;
            port_items.reserve(ports.size());
            for (const auto& p : ports) port_items.push_back(p.c_str());

            int port_idx = 0;
            for (size_t i = 0; i < ports.size(); ++i)
                if (ports[i] == config.arduino_port) { port_idx = static_cast<int>(i); break; }
            if (ImGui::Combo(u8"端口", &port_idx, port_items.data(), static_cast<int>(port_items.size())))
            {
                config.arduino_port = ports[port_idx];
                OverlayConfig_MarkDirty();
                reconnect_input_device_now();
            }

            const std::vector<int> bauds = { 9600, 19200, 38400, 57600, 115200 };
            std::vector<std::string> baud_s;
            for (int b : bauds) baud_s.push_back(std::to_string(b));
            std::vector<const char*> baud_items;
            baud_items.reserve(baud_s.size());
            for (const auto& s : baud_s) baud_items.push_back(s.c_str());
            int baud_idx = 4;
            for (size_t i = 0; i < bauds.size(); ++i)
                if (bauds[i] == config.arduino_baudrate) { baud_idx = static_cast<int>(i); break; }
            if (ImGui::Combo(u8"波特率", &baud_idx, baud_items.data(), static_cast<int>(baud_items.size())))
            {
                config.arduino_baudrate = bauds[baud_idx];
                OverlayConfig_MarkDirty();
                reconnect_input_device_now();
            }

            if (ImGui::Checkbox(u8"16 位鼠标坐标", &config.arduino_16_bit_mouse))
                OverlayConfig_MarkDirty();
            if (ImGui::Checkbox(u8"允许 Arduino 报告按键", &config.arduino_enable_keys))
                OverlayConfig_MarkDirty();
            draw_move_test_button();
            OverlayUI::EndSection();
        }
    }
    else if (config.input_method == "KMBOX_NET")
    {
        if (OverlayUI::BeginSection(u8"KmboxNet", "hardware_section_kmbox_net"))
        {
            static char ip[32]{}, port[8]{}, uuid[16]{};
            static std::string last_ip, last_port, last_uuid;
            if (last_ip != config.kmbox_net_ip || last_port != config.kmbox_net_port || last_uuid != config.kmbox_net_uuid)
            {
                std::strncpy(ip, config.kmbox_net_ip.c_str(), sizeof(ip) - 1);
                std::strncpy(port, config.kmbox_net_port.c_str(), sizeof(port) - 1);
                std::strncpy(uuid, config.kmbox_net_uuid.c_str(), sizeof(uuid) - 1);
                ip[sizeof(ip) - 1] = port[sizeof(port) - 1] = uuid[sizeof(uuid) - 1] = '\0';
                last_ip = config.kmbox_net_ip;
                last_port = config.kmbox_net_port;
                last_uuid = config.kmbox_net_uuid;
            }

            ImGui::InputText("IP", ip, sizeof(ip));
            ImGui::InputText(u8"端口", port, sizeof(port));
            ImGui::InputText("UUID", uuid, sizeof(uuid));

            if (ImGui::Button(u8"保存并重连"))
            {
                config.kmbox_net_ip = ip;
                config.kmbox_net_port = port;
                config.kmbox_net_uuid = uuid;
                last_ip = config.kmbox_net_ip;
                last_port = config.kmbox_net_port;
                last_uuid = config.kmbox_net_uuid;
                OverlayConfig_MarkDirty();
                reconnect_input_device_now();
            }
            ImGui::SameLine();
            draw_connection_state(kmboxNetSerial && kmboxNetSerial->isOpen(), u8"● 已连接", u8"● 未连接");

            draw_move_test_button();
            ImGui::SameLine();
            if (ImGui::Button(u8"重启盒子") && kmboxNetSerial)
            {
                kmboxNetSerial->reboot();
                if (globalMouseThread)
                    globalMouseThread->setKmboxNetConnection(nullptr);
                delete kmboxNetSerial;
                kmboxNetSerial = nullptr;
                assignInputDevices();
            }
            OverlayUI::EndSection();
        }
    }
    else if (config.input_method == "KMBOX_A")
    {
        if (OverlayUI::BeginSection(u8"KmboxA", "hardware_section_kmbox_a"))
        {
            static char pidvid[32]{};
            static std::string last;
            if (last != config.kmbox_a_pidvid)
            {
                std::strncpy(pidvid, config.kmbox_a_pidvid.c_str(), sizeof(pidvid) - 1);
                pidvid[sizeof(pidvid) - 1] = '\0';
                last = config.kmbox_a_pidvid;
            }
            ImGui::InputText("PIDVID", pidvid, sizeof(pidvid));
            ImGui::TextDisabled(u8"格式：PPPPVVVV（单字段 8 位）");

            if (ImGui::Button(u8"保存并重连"))
            {
                config.kmbox_a_pidvid = pidvid;
                last = config.kmbox_a_pidvid;
                OverlayConfig_MarkDirty();
                reconnect_input_device_now();
            }
            ImGui::SameLine();
            draw_connection_state(kmboxASerial && kmboxASerial->isOpen(), u8"● 已连接", u8"● 未连接");
            draw_move_test_button();
            OverlayUI::EndSection();
        }
    }
    else if (config.input_method == "MAKCU")
    {
        if (OverlayUI::BeginSection(u8"MAKCU", "hardware_section_makcu"))
        {
            std::vector<std::string> ports;
            for (int i = 1; i <= 30; ++i) ports.push_back("COM" + std::to_string(i));
            std::vector<const char*> port_items;
            port_items.reserve(ports.size());
            for (const auto& p : ports) port_items.push_back(p.c_str());
            int port_idx = 0;
            for (size_t i = 0; i < ports.size(); ++i)
                if (ports[i] == config.makcu_port) { port_idx = static_cast<int>(i); break; }
            if (ImGui::Combo(u8"端口", &port_idx, port_items.data(), static_cast<int>(port_items.size())))
            {
                config.makcu_port = ports[port_idx];
                OverlayConfig_MarkDirty();
                reconnect_input_device_now();
            }

            const std::vector<int> bauds = { 9600, 19200, 38400, 57600, 115200 };
            std::vector<std::string> baud_s;
            for (int b : bauds) baud_s.push_back(std::to_string(b));
            std::vector<const char*> baud_items;
            baud_items.reserve(baud_s.size());
            for (const auto& s : baud_s) baud_items.push_back(s.c_str());
            int baud_idx = 4;
            for (size_t i = 0; i < bauds.size(); ++i)
                if (bauds[i] == config.makcu_baudrate) { baud_idx = static_cast<int>(i); break; }
            if (ImGui::Combo(u8"波特率", &baud_idx, baud_items.data(), static_cast<int>(baud_items.size())))
            {
                config.makcu_baudrate = bauds[baud_idx];
                OverlayConfig_MarkDirty();
                reconnect_input_device_now();
            }

            draw_connection_state(makcuSerial && makcuSerial->isOpen(), u8"● 已连接", u8"● 未连接");
            draw_move_test_button();
            OverlayUI::EndSection();
        }
    }

    if (OverlayUI::BeginSection(u8"警告", "hardware_section_warning"))
    {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.55f),
            u8"调试鼠标参数时请最小化覆盖窗口，以免误触发射击。");
        OverlayUI::EndSection();
    }
}
