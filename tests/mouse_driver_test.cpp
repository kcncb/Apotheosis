#include <cassert>
#include <iostream>
#include <string>

#include "mouse/mouse_driver.h"

// ─────────────────────────────────────────────────────────────────────────────
// mouse_driver 单元测试: 验证统一驱动抽象的契约 (形状仿 AimMagic 驱动工厂)
// ─────────────────────────────────────────────────────────────────────────────

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::cerr << "FAIL: " #expr " at " __FILE__ ":" << __LINE__ << "\n"; \
        return 1; \
    } \
} while(0)

int main()
{
    using namespace mouse_driver;

    std::cout << "[1] 后端名称列表必须包含三家...\n";
    {
        const auto names = backendNames();
        CHECK(names.size() == 3);
        CHECK(names[0] == kBackendMakcu);
        CHECK(names[1] == kBackendMakcuNew);
        CHECK(names[2] == kBackendKmboxNet);
    }

    std::cout << "[2] 能力位中文描述...\n";
    {
        const std::string desc = describeCapabilities(kCapMove | kCapButtonLeft | kCapKeyboard);
        CHECK(desc.find(u8"位移") != std::string::npos);
        CHECK(desc.find(u8"左键") != std::string::npos);
        CHECK(desc.find(u8"键盘") != std::string::npos);
        CHECK(desc.find(u8"滚轮") == std::string::npos); // 未包含的不许出现
        CHECK(describeCapabilities(0) == u8"无");
    }

    std::cout << "[3] 状态字符串渲染 (连不上必须带得出证据)...\n";
    {
        const std::string s_fail = describeStatus("KMBOXNET", false, "192.168.2.88:6234 超时");
        CHECK(s_fail.find(u8"不可用") != std::string::npos);
        CHECK(s_fail.find("192.168.2.88") != std::string::npos);
        CHECK(s_fail.find(u8"超时") != std::string::npos);

        const std::string s_ok = describeStatus("MAKCUNEW", true, "COM3@6000000");
        CHECK(s_ok.find(u8"已连接") != std::string::npos);
        CHECK(s_ok.find("COM3") != std::string::npos);
    }

    std::cout << "[4] 工厂对不认识的名字拒绝并给出理由 (不静默回落)...\n";
    {
        const auto res = open("KMBOX_BPLUS", "", 0, "", 0, "", "", "");
        CHECK(res.driver == nullptr);
        CHECK(!res.error.empty());
        CHECK(res.error.find(u8"不认识的名字") != std::string::npos);
        CHECK(res.error.find("KMBOX_BPLUS") != std::string::npos);
        // 理由里必须把合法候选列出来
        CHECK(res.error.find("MAKCU") != std::string::npos);
        CHECK(res.error.find("MAKCUNEW") != std::string::npos);
        CHECK(res.error.find("KMBOXNET") != std::string::npos);
    }

    std::cout << "[5] KMBOXNET 未填 IP 时拒绝并给出理由...\n";
    {
        const auto res = open("KMBOXNET", "", 0, "", 0, "", "6234", "12345");
        CHECK(res.driver == nullptr);
        CHECK(!res.error.empty());
        CHECK(res.error.find(u8"未填写盒子 IP") != std::string::npos);
    }

    std::cout << "[6] 假连接对象的包装器契约...\n";
    // 用一个 nullptr 传入包装器, 断言其安全降级(不崩 + 明确返回 false + 状态正确)
    {
        WrappedMakcuDriver d_makcu(nullptr);
        CHECK(std::string(d_makcu.name()) == "MAKCU");
        CHECK(!d_makcu.isOpen());
        CHECK(!d_makcu.move(10, 20));
        CHECK(!d_makcu.leftDown());
        CHECK(!d_makcu.tapKey(0x1A, 50, 0)); // MAKCU 永远不支持键盘
        CHECK(d_makcu.physicalButtonPressed(1) == -1);
        CHECK((d_makcu.capabilities() & kCapKeyboard) == 0); // 无键盘能力
        CHECK((d_makcu.capabilities() & kCapMove) != 0);

        WrappedMakcuNewDriver d_new(nullptr);
        CHECK(std::string(d_new.name()) == "MAKCUNEW");
        CHECK(!d_new.isOpen());
        CHECK(!d_new.move(10, 20));
        CHECK(!d_new.tapKey(0x1A, 50, 0));
        CHECK((d_new.capabilities() & kCapKeyboard) != 0);   // MAKCUNEW 有键盘能力
        CHECK(d_new.directSend());                            // 直发

        WrappedKmboxNetDriver d_km(nullptr);
        CHECK(std::string(d_km.name()) == "KMBOXNET");
        CHECK(!d_km.isOpen());
        CHECK(!d_km.move(10, 20));
        CHECK((d_km.capabilities() & kCapKeyboard) != 0);    // KMBOXNET 有键盘能力
        CHECK(d_km.directSend());                             // 直发
    }

    std::cout << "All mouse_driver unit tests passed.\n";
    return 0;
}
