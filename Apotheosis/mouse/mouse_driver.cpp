#include "mouse_driver.h"

#include <algorithm>
#include <memory>
#include <sstream>

#include "Makcu.h"
#include "MakcuNew.h"
#include "kmboxNetConnection.h"

namespace mouse_driver
{

const char* const kBackendMakcu     = "MAKCU";
const char* const kBackendMakcuNew  = "MAKCUNEW";
const char* const kBackendKmboxNet  = "KMBOXNET";

std::string describeCapabilities(uint32_t caps)
{
    static const uint32_t order[] = {
        kCapMove, kCapButtonLeft, kCapButtonRight, kCapButtonMiddle,
        kCapButtonSide, kCapWheel, kCapKeyboard, kCapPhysicalRead
    };
    std::string out;
    for (uint32_t c : order)
    {
        if ((caps & c) == 0) continue;
        if (!out.empty()) out += ' ';
        out += capabilityName(c);
    }
    if (out.empty()) out = u8"无";
    return out;
}

std::vector<std::string> backendNames()
{
    return { kBackendMakcu, kBackendMakcuNew, kBackendKmboxNet };
}

std::string describeStatus(const std::string& backend, bool open, const std::string& detail)
{
    std::string out = u8"鼠标后端 ";
    out += backend.empty() ? std::string(u8"(未选择)") : backend;
    if (!detail.empty())
    {
        out += " (";
        out += detail;
        out += ")";
    }
    out += open ? u8" 已连接" : u8" 不可用";
    if (!open && !detail.empty())
    {
        out += u8": ";
        out += detail;
    }
    return out;
}

namespace
{

std::string errWith(const char* what, const std::string& detail)
{
    std::string s = what;
    if (!detail.empty())
    {
        s += u8" (";
        s += detail;
        s += ')';
    }
    return s;
}

// HID usage id (Keyboard/Keypad page 0x07) -> Windows Virtual-Key。
// 只覆盖自动急停用到的四个方向键 —— 覆盖全部 256 个键没有意义,
// 而**漏转**比不支持更危险(会发出别的键), 所以不认识的键一律返回 0 = 拒绝。
int hidUsageToVk(int hid)
{
    switch (hid)
    {
    case 0x1A: return 0x57;   // W
    case 0x04: return 0x41;   // A
    case 0x16: return 0x53;   // S
    case 0x07: return 0x44;   // D
    default:   return 0;      // 明确拒绝, 不瞎猜
    }
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 包装适配器实现 —— 不拥有连接对象, 只转发
// ─────────────────────────────────────────────────────────────────────────────

// ── MAKCU (官方库) ──
//
// 能力位取自 Makcu.cpp 的实际实现: toButton(1..5) 覆盖左/右/中/侧1/侧2,
// 有 click/press/release/move/wheel, 而且 `device_.setMouseButtonCallback`
// 提供**物理按键回读**(缓存进 shooting/zooming/middle/side1/side2_active)。
//
// ★ 但**没有** tapKey —— 所以自动急停在 MAKCU 档自动失效(这是既有的本意行为,
//   不是遗漏: 官方库没有键盘注入通道)。
WrappedMakcuDriver::WrappedMakcuDriver(MakcuConnection* conn) : conn_(conn) {}

const char* WrappedMakcuDriver::name() const { return kBackendMakcu; }

uint32_t WrappedMakcuDriver::capabilities() const
{
    return kCapMove | kCapButtonLeft | kCapButtonRight | kCapButtonMiddle |
           kCapButtonSide | kCapWheel | kCapPhysicalRead;
}

bool WrappedMakcuDriver::isOpen() const { return conn_ != nullptr && conn_->isOpen(); }

std::string WrappedMakcuDriver::lastError() const
{
    return u8"[MAKCU] 串口未打开或设备未响应";
}

bool WrappedMakcuDriver::move(int dx, int dy)
{
    if (!isOpen()) return false;
    conn_->move(dx, dy);
    // 官方库的 move 无返回值, 用"发完之后口还是开的"作为成功判据 ——
    // 这是旧代码的原判据, 保留以免改变既有行为。
    return conn_->isOpen();
}

bool WrappedMakcuDriver::leftDown()   { if (!isOpen()) return false; conn_->press(1);   return true; }
bool WrappedMakcuDriver::leftUp()     { if (!isOpen()) return false; conn_->release(1); return true; }
bool WrappedMakcuDriver::rightDown()  { if (!isOpen()) return false; conn_->press(2);   return true; }
bool WrappedMakcuDriver::rightUp()    { if (!isOpen()) return false; conn_->release(2); return true; }
bool WrappedMakcuDriver::middleDown() { if (!isOpen()) return false; conn_->press(3);   return true; }
bool WrappedMakcuDriver::middleUp()   { if (!isOpen()) return false; conn_->release(3); return true; }

bool WrappedMakcuDriver::wheel(int delta)
{
    if (!isOpen()) return false;
    conn_->wheel(delta);
    return true;
}

// 官方库没有键盘注入通道 —— 明确失败, 让自动急停判定为不可用。
bool WrappedMakcuDriver::tapKey(int, int, int) { return false; }

int WrappedMakcuDriver::physicalButtonPressed(int button) const
{
    if (!isOpen()) return -1;
    switch (button)
    {
    case 1: return conn_->shooting_active ? 1 : 0;
    case 2: return conn_->zooming_active  ? 1 : 0;
    case 3: return conn_->middle_active   ? 1 : 0;
    case 4: return conn_->side1_active    ? 1 : 0;
    case 5: return conn_->side2_active    ? 1 : 0;
    default: return -1;
    }
}

// ── MAKCUNEW (直通透传固件) ──
//
// 能力最全: 有 0x22 KEY_TAP 键盘通道(自动急停靠它), 也有物理按键回读。
WrappedMakcuNewDriver::WrappedMakcuNewDriver(MakcuNewConnection* conn) : conn_(conn) {}

const char* WrappedMakcuNewDriver::name() const { return kBackendMakcuNew; }

uint32_t WrappedMakcuNewDriver::capabilities() const
{
    return kCapMove | kCapButtonLeft | kCapButtonRight | kCapButtonMiddle |
           kCapButtonSide | kCapWheel | kCapKeyboard | kCapPhysicalRead;
}

bool WrappedMakcuNewDriver::isOpen() const { return conn_ != nullptr && conn_->isOpen(); }

std::string WrappedMakcuNewDriver::lastError() const
{
    // 固件波特率不持久化, 每次上电一定在 115200; 协商失败会自动退回。
    // 这条理由带上"协商"这层, 因为连不上最常见的原因就是速率没谈成。
    return u8"[MAKCUNEW] 串口未打开或会话探活失败(固件上电必为 115200, 协商失败会自动退回)";
}

bool WrappedMakcuNewDriver::move(int dx, int dy)
{
    if (!isOpen()) return false;
    return conn_->move(dx, dy);
}

bool WrappedMakcuNewDriver::leftDown()   { if (!isOpen()) return false; conn_->press(1);   return true; }
bool WrappedMakcuNewDriver::leftUp()     { if (!isOpen()) return false; conn_->release(1); return true; }
bool WrappedMakcuNewDriver::rightDown()  { if (!isOpen()) return false; conn_->press(2);   return true; }
bool WrappedMakcuNewDriver::rightUp()    { if (!isOpen()) return false; conn_->release(2); return true; }
bool WrappedMakcuNewDriver::middleDown() { if (!isOpen()) return false; conn_->press(3);   return true; }
bool WrappedMakcuNewDriver::middleUp()   { if (!isOpen()) return false; conn_->release(3); return true; }

bool WrappedMakcuNewDriver::wheel(int delta)
{
    if (!isOpen()) return false;
    conn_->wheel(delta);
    return true;
}

bool WrappedMakcuNewDriver::tapKey(int hidKey, int holdMs, int mod)
{
    if (!isOpen()) return false;
    return conn_->tapKey(hidKey, holdMs, mod);
}

int WrappedMakcuNewDriver::physicalButtonPressed(int button) const
{
    if (!isOpen()) return -1;
    const bool down = conn_->physicalButtonPressed(button);
    return down ? 1 : 0;
}

void WrappedMakcuNewDriver::cancelMove()
{
    if (isOpen()) conn_->cancelMove();
}

// 直发: 每一拍推理结果要贴合, 排队会多一层调度延迟(既有行为)。
bool WrappedMakcuNewDriver::directSend() const { return true; }

// ── KMBOXNET (以太网 UDP) ──
//
// 能力: 位移 / 左右中侧键 / 滚轮 / 键盘 / 物理回读。
//
// ★ 两个必须说清的差异 (恢复这段代码时不能假装没这回事):
//   ①**键盘用 vkey, 不是 HID usage id。** 自动急停那套是 HID usage
//     (W=0x1A/A=0x04/S=0x16/D=0x07), 而 kmNet_keydown 吃的是 Windows
//     Virtual-Key (W=0x57/A=0x41/S=0x53/D=0x44)。必须转换, 直接填 HID 值
//     会发出完全无关的键。
//   ②**固件没有"定时弹起"**, 必须在 keyUp 之前自己 hold —— 而且必须补抬,
//     否则漏一次就把玩家的键永久卡住(与本项目"用 KEY_TAP 而不是 KEY_MASK"
//     那条教训是同一个理由, 只是这里固件不提供自清, 只能我们自己保证)。
WrappedKmboxNetDriver::WrappedKmboxNetDriver(KmboxNetConnection* conn) : conn_(conn) {}

const char* WrappedKmboxNetDriver::name() const { return kBackendKmboxNet; }

uint32_t WrappedKmboxNetDriver::capabilities() const
{
    return kCapMove | kCapButtonLeft | kCapButtonRight | kCapButtonMiddle |
           kCapButtonSide | kCapWheel | kCapKeyboard | kCapPhysicalRead;
}

bool WrappedKmboxNetDriver::isOpen() const { return conn_ != nullptr && conn_->isOpen(); }

std::string WrappedKmboxNetDriver::lastError() const
{
    return u8"[KMBOXNET] UDP 连接失败(核对盒子屏幕上的 IP/端口/UUID)";
}

bool WrappedKmboxNetDriver::move(int dx, int dy)
{
    if (!isOpen()) return false;
    conn_->move(dx, dy);
    return true;
}

bool WrappedKmboxNetDriver::leftDown()   { if (!isOpen()) return false; conn_->leftDown();   return true; }
bool WrappedKmboxNetDriver::leftUp()     { if (!isOpen()) return false; conn_->leftUp();     return true; }
bool WrappedKmboxNetDriver::rightDown()  { if (!isOpen()) return false; conn_->rightDown();  return true; }
bool WrappedKmboxNetDriver::rightUp()    { if (!isOpen()) return false; conn_->rightUp();    return true; }
bool WrappedKmboxNetDriver::middleDown() { if (!isOpen()) return false; conn_->middleDown(); return true; }
bool WrappedKmboxNetDriver::middleUp()   { if (!isOpen()) return false; conn_->middleUp();   return true; }

bool WrappedKmboxNetDriver::wheel(int delta)
{
    if (!isOpen()) return false;
    conn_->wheel(delta);
    return true;
}

bool WrappedKmboxNetDriver::tapKey(int hidKey, int holdMs, int)
{
    if (!isOpen()) return false;
    const int vk = hidUsageToVk(hidKey);
    if (vk == 0) return false;   // 这个键我们不会转, 明确失败, 不瞎猜

    conn_->keyDown(vk);
    if (holdMs > 0) Sleep(static_cast<DWORD>(holdMs));
    conn_->keyUp(vk);
    return true;
}

int WrappedKmboxNetDriver::physicalButtonPressed(int button) const
{
    if (!isOpen()) return -1;
    switch (button)
    {
    case 1: return conn_->monitorMouseLeft();
    case 2: return conn_->monitorMouseRight();
    case 3: return conn_->monitorMouseMiddle();
    case 4: return conn_->monitorMouseSide1();
    case 5: return conn_->monitorMouseSide2();
    default: return -1;
    }
}

// 直发: UDP sendto 是微秒级且无阻塞握手, 不需要排队。
bool WrappedKmboxNetDriver::directSend() const { return true; }

// ─────────────────────────────────────────────────────────────────────────────
// 工厂 —— 对应 AimMagic 的 `FUN_140040ff0`
// ─────────────────────────────────────────────────────────────────────────────

OpenResult open(const std::string& backend,
                const std::string& makcuPort, unsigned int makcuBaud,
                const std::string& makcuNewPort, unsigned int makcuNewBaud,
                const std::string& kmboxNetIp, const std::string& kmboxNetPort,
                const std::string& kmboxNetUuid)
{
    OpenResult result;

    if (backend == kBackendMakcu)
    {
        auto conn = std::make_unique<MakcuConnection>(makcuPort, makcuBaud);
        if (!conn->isOpen())
        {
            result.error = errWith(u8"[MAKCU] 串口打不开或设备无响应",
                                   makcuPort + "@" + std::to_string(makcuBaud));
            return result;
        }
        result.driver = new OwningDriver<WrappedMakcuDriver>(std::move(conn));
        return result;
    }

    if (backend == kBackendMakcuNew)
    {
        auto conn = std::make_unique<MakcuNewConnection>(makcuNewPort, makcuNewBaud);
        if (!conn->isOpen())
        {
            result.error = errWith(u8"[MAKCUNEW] 会话未建立(固件上电必为 115200, 协商失败自动退回)",
                                   makcuNewPort + "@" + std::to_string(makcuNewBaud));
            return result;
        }
        result.driver = new OwningDriver<WrappedMakcuNewDriver>(std::move(conn));
        return result;
    }

    if (backend == kBackendKmboxNet)
    {
        if (kmboxNetIp.empty())
        {
            result.error = u8"[KMBOXNET] 未填写盒子 IP (显示在盒子屏幕上, 例如 192.168.2.88)";
            return result;
        }
        auto conn = std::make_unique<KmboxNetConnection>(kmboxNetIp, kmboxNetPort, kmboxNetUuid);
        if (!conn->isOpen())
        {
            result.error = errWith(u8"[KMBOXNET] UDP 连接失败(核对盒子屏幕上的 IP/端口/UUID)",
                                   kmboxNetIp + ":" + kmboxNetPort + " uuid=" + kmboxNetUuid);
            return result;
        }
        result.driver = new OwningDriver<WrappedKmboxNetDriver>(std::move(conn));
        return result;
    }

    // 不认识的档位: 给得出理由, 不默默选一个默认后端。
    result.error = u8"[输入后端] 不认识的名字 \"";
    result.error += backend.empty() ? std::string(u8"(空)") : backend;
    result.error += u8"\", 可选: ";
    const auto names = backendNames();
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (i) result.error += " / ";
        result.error += names[i];
    }
    return result;
}

} // namespace mouse_driver
