#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// 硬件协议抽象层 —— 移植自 AimMagic 的"鼠标驱动"形状
// ─────────────────────────────────────────────────────────────────────────────
//
// ★ 先说清楚抄的是什么、没抄什么:
//
// AimMagic 的 `FUN_140040ff0` 是一个**驱动工厂** —— 它按配置里的名字字符串
// (长度 + 内容双重比较, 例如 5 字 "makcu"、11 字 "kmbox_bplus"、6 字 "ferrum")
// 构造九个后端之一, 每个后端实现同一套 vtable:
//
//     +0x08  连接/探活      -> 返回 bool, 失败后走 +0x40 取理由
//     +0x20  名称           -> 用于 "runtime initialized: capture=..., mouse=..." 日志
//     +0x30  执行一步动作
//     +0x40  最后一次错误   -> 中文/英文理由串
//
// 上层 `FUN_140077f40` 只走虚接口, 连不上就打印
// `"mouse driver unavailable: " + <理由>` 然后停 —— **不会**静默退化成别的后端。
//
// 我们抄的是这层**契约**, 不是那九个后端的协议实现 —— 那些是 KMBox / Ferrum /
// ProBox 各家自己的固件协议, 本项目没有那些硬件, 抄过来是死代码。
//
// 本项目实际支持的后端:
//   MAKCU     —— modules/makcu 官方库 (串口)
//   MAKCUNEW  —— 自研直通透传固件 (串口, 有键盘通道)
//   KMBOXNET  —— KMBox Net (以太网 UDP, 从 9236942^ 恢复)
//
// ★ 三条设计约束 (都是本项目踩过的坑, 不是 AM 的):
//   ①**能力查询而不是运行时试探** —— 旧代码在 `MouseThread` 里
//     `if (makcu_new_) ... else if (makcu_)` 分叉, 想知道"能不能发键盘"
//     只能去问具体类型。加一个后端就要改所有调用点。改成能力位之后,
//     调用方问的是"这个后端有没有键盘通道", 而不是"你是哪个类"。
//   ②**连不上必须给得出理由** —— AM 行 732 那条
//     `"mouse driver unavailable: "` 串是这个设计的全部价值: 用户配错 IP/COM
//     时看到的是"连不上 192.168.2.88:6234 (超时)", 而不是"功能没反应"。
//     本项目旧代码只 `std::cerr << "[KmboxNet] Error connecting."`, 不够。
//   ③**绝不静默降级** —— 后端连不上就是没有后端, 不自动换一家。静默换家
//     会让"为什么手感变了"变成无法归因的问题。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// ★ 这三个连接类是**全局命名空间**里的 (Makcu.h / MakcuNew.h /
//   kmboxNetConnection.h)。它们的前置声明放在下面、**命名空间之外** ——
//   在里面写 `class MakcuConnection;` 会声明出 `mouse_driver::MakcuConnection`,
//   一个和真正类型毫无关系的**新类型**(第一次编译的 C2027 就是这么来的)。
class MakcuConnection;
class MakcuNewConnection;
class KmboxNetConnection;

namespace mouse_driver
{

// 后端能力位。调用方按位查询, 不要按具体类型分叉。
enum Capability : uint32_t
{
    kCapNone         = 0,
    kCapMove         = 1u << 0,   // 发送相对位移 (所有后端都必须有)
    kCapButtonLeft   = 1u << 1,   // 左键按下/抬起 (所有后端都必须有)
    kCapButtonRight  = 1u << 2,   // 右键 (自动开镜用)
    kCapButtonMiddle = 1u << 3,
    kCapButtonSide   = 1u << 4,   // 侧键 X1/X2
    kCapWheel        = 1u << 5,
    kCapKeyboard     = 1u << 6,   // 键盘注入 (自动急停用) —— 只有 MAKCUNEW / KMBOXNET 有
    kCapPhysicalRead = 1u << 7,   // 能读回玩家物理按键 (自动急停的自激防护用)
};

inline const char* capabilityName(uint32_t cap)
{
    switch (cap)
    {
    case kCapMove:         return u8"位移";
    case kCapButtonLeft:   return u8"左键";
    case kCapButtonRight:  return u8"右键";
    case kCapButtonMiddle: return u8"中键";
    case kCapButtonSide:   return u8"侧键";
    case kCapWheel:        return u8"滚轮";
    case kCapKeyboard:     return u8"键盘";
    case kCapPhysicalRead: return u8"物理按键回读";
    default:               return u8"未知";
    }
}

// 把能力位渲染成中文串, 给 UI / 日志用。例: "位移 左键 右键 键盘"。
std::string describeCapabilities(uint32_t caps);

// 统一驱动接口。所有后端实现它。
//
// ★ 线程约定: `MouseThread::sendMovementToDriver` 在**移动工作线程**上调用,
// 而按键走 `input_method_mutex` 保护的调用点。两个后端内部各自有锁, 所以
// 接口本身不再加锁 —— 但实现**必须**自己是线程安全的。
class IDriver
{
public:
    virtual ~IDriver() = default;

    // 用于日志/UI 的后端名 ("MAKCU" / "MAKCUNEW" / "KMBOXNET")。
    virtual const char* name() const = 0;

    // 能力位。
    virtual uint32_t capabilities() const = 0;

    // 会话是否已建立(探活通过)。**不是**"串口已打开" —— 打开但没探活
    // 成功对上层来说等于不可用 (MAKCUNEW 的 openSerial 与 open_ 是两件事)。
    virtual bool isOpen() const = 0;

    // 连不上/掉线时的理由, 中文。**必须给得出证据** —— 带上 IP/COM/超时值,
    // 而不是只写"连接失败"。参考本项目坑④: 错误信息要描述
    // 【对方/环境说了什么】, 而不是只描述我们这边的失败状态。
    virtual std::string lastError() const = 0;

    // ─── 动作。返回 false = 这一步没发出去, 调用方可据此累计 failedMoves_ ───
    virtual bool move(int dx, int dy) = 0;

    virtual bool leftDown() = 0;
    virtual bool leftUp() = 0;
    virtual bool rightDown() = 0;
    virtual bool rightUp() = 0;
    virtual bool middleDown() = 0;
    virtual bool middleUp() = 0;

    // 滚轮 (后端不支持时返回 false, 不静默成功)。
    virtual bool wheel(int /*delta*/) { return false; }

    // 键盘单键短按。`hidKey` = HID usage id, `holdMs` 由固件定时弹起(自清)。
    // ★ 没有键盘通道的后端**必须**返回 false, 不能假装成功 ——
    // 自动急停靠这个返回值判断"功能自动失效"。
    virtual bool tapKey(int /*hidKey*/, int /*holdMs*/, int /*mod*/ = 0) { return false; }

    // 读回玩家物理按键状态 (用于自动急停的自激防护)。
    // 返回 -1 = 该后端读不到(未知), 0 = 没按, 1 = 按着。
    virtual int physicalButtonPressed(int /*button*/) const { return -1; }

    // 打断/丢弃固件里还没执行的平滑移动 (MAKCU 系的 cancelMove)。
    virtual void cancelMove() {}

    // 位移是否走【直发】(在调用线程上直接写出, 不进 moveWorkerLoop 队列)。
    //
    // ★ 这是**传输语义**而不是能力, 但它同样不该让调用方按类型分叉:
    //   MAKCUNEW / KMBOXNET 的写出本身就足够快且要贴合每一拍推理结果,
    //   排队反而引入一个额外的调度延迟; MAKCU 官方库的写出会阻塞较长,
    //   排到工作线程上才不会拖住瞄准循环。
    virtual bool directSend() const { return false; }
};

// ─────────────────────────────────────────────────────────────────────────────
// OwningDriver<T> —— 让包装器**拥有**连接对象
// ─────────────────────────────────────────────────────────────────────────────
//
// 工厂 `open()` 自己构造连接, 所以它必须负责关闭。把"拥有"做成一个模板
// 而不是在包装器里加 `bool owned`: 所有权写在类型上, 就不存在
// "析构时该不该 delete"这种到处要判断的问题。
//
// ★ 成员声明顺序很重要: `conn_` 必须在 `wrapper_` **之前**析构 ——
//   包装器析构时会调 `cancelMove()` 之类的转发, 连接对象得还活着。
//   但 C++ 保证成员按声明逆序析构, 所以 conn_ 写在前、wrapper_ 写在后,
//   于是 wrapper_ 先析构、conn_ 后析构 —— 正是我们要的顺序。
//
// ★ 连接类型在一个 TU 里只有三种, 所以直接用一个小基类存指针 + 虚析构,
//   不需要 type erasure 的花活。
class DriverConnectionHolder
{
public:
    virtual ~DriverConnectionHolder() = default;
};

template <typename ConnT>
class DriverConnection final : public DriverConnectionHolder
{
public:
    explicit DriverConnection(std::unique_ptr<ConnT> conn) : conn_(std::move(conn)) {}
    ConnT* get() const { return conn_.get(); }

private:
    std::unique_ptr<ConnT> conn_;
};

template <typename WrapperT>
class OwningDriver final : public IDriver
{
public:
    template <typename ConnT>
    explicit OwningDriver(std::unique_ptr<ConnT> conn)
        : holder_(std::make_unique<DriverConnection<ConnT>>(std::move(conn))),
          wrapper_(static_cast<DriverConnection<ConnT>*>(holder_.get())->get())
    {
    }

    const char* name() const override { return wrapper_.name(); }
    uint32_t capabilities() const override { return wrapper_.capabilities(); }
    bool isOpen() const override { return wrapper_.isOpen(); }
    std::string lastError() const override { return wrapper_.lastError(); }
    bool move(int dx, int dy) override { return wrapper_.move(dx, dy); }
    bool leftDown() override { return wrapper_.leftDown(); }
    bool leftUp() override { return wrapper_.leftUp(); }
    bool rightDown() override { return wrapper_.rightDown(); }
    bool rightUp() override { return wrapper_.rightUp(); }
    bool middleDown() override { return wrapper_.middleDown(); }
    bool middleUp() override { return wrapper_.middleUp(); }
    bool wheel(int delta) override { return wrapper_.wheel(delta); }
    bool tapKey(int hidKey, int holdMs, int mod) override { return wrapper_.tapKey(hidKey, holdMs, mod); }
    int physicalButtonPressed(int button) const override { return wrapper_.physicalButtonPressed(button); }
    void cancelMove() override { wrapper_.cancelMove(); }
    bool directSend() const override { return wrapper_.directSend(); }

private:
    std::unique_ptr<DriverConnectionHolder> holder_;   // 先声明 ⇒ 后析构
    WrapperT wrapper_;                                 // 后声明 ⇒ 先析构
};

// ─────────────────────────────────────────────────────────────────────────────
// 包装适配器 —— 把**已经构造好的**连接对象包成 IDriver (不拥有它)
// ─────────────────────────────────────────────────────────────────────────────
//
// `open()` 自己 new 连接对象; 但 Apotheosis 的设备热插拔流程是
// 「Apotheosis.cpp 持有 `makcuSerial` / `makcuNewSerial` / `kmboxNetSerial`,
// 再 setXxxConnection 把裸指针交给 MouseThread」, 生命周期由前者管。
// 这两种所有权模型都要支持, 所以这里提供**不拥有**的包装版本:
// 它只转发调用, 析构时不 delete 连接对象。
//
// ★ 为什么不用一个 `bool owned` 的成员省掉这层: IDriver 的契约是纯转发,
//   把所有权混进去会让"谁负责关串口"变成一个到处都要判断的问题。
//   分开两类实现, 所有权在类型上就写死了。

// ★ 三个连接类型见文件顶部的全局前置声明。

class WrappedMakcuDriver final : public IDriver
{
public:
    explicit WrappedMakcuDriver(MakcuConnection* conn);
    const char* name() const override;
    uint32_t capabilities() const override;
    bool isOpen() const override;
    std::string lastError() const override;
    bool move(int dx, int dy) override;
    bool leftDown() override;
    bool leftUp() override;
    bool rightDown() override;
    bool rightUp() override;
    bool middleDown() override;
    bool middleUp() override;
    bool wheel(int delta) override;
    bool tapKey(int hidKey, int holdMs, int mod) override;
    int physicalButtonPressed(int button) const override;
private:
    MakcuConnection* conn_;
};

class WrappedMakcuNewDriver final : public IDriver
{
public:
    explicit WrappedMakcuNewDriver(MakcuNewConnection* conn);
    const char* name() const override;
    uint32_t capabilities() const override;
    bool isOpen() const override;
    std::string lastError() const override;
    bool move(int dx, int dy) override;
    bool leftDown() override;
    bool leftUp() override;
    bool rightDown() override;
    bool rightUp() override;
    bool middleDown() override;
    bool middleUp() override;
    bool wheel(int delta) override;
    bool tapKey(int hidKey, int holdMs, int mod) override;
    int physicalButtonPressed(int button) const override;
    void cancelMove() override;
    bool directSend() const override;
private:
    MakcuNewConnection* conn_;
};

class WrappedKmboxNetDriver final : public IDriver
{
public:
    explicit WrappedKmboxNetDriver(KmboxNetConnection* conn);
    const char* name() const override;
    uint32_t capabilities() const override;
    bool isOpen() const override;
    std::string lastError() const override;
    bool move(int dx, int dy) override;
    bool leftDown() override;
    bool leftUp() override;
    bool rightDown() override;
    bool rightUp() override;
    bool middleDown() override;
    bool middleUp() override;
    bool wheel(int delta) override;
    bool tapKey(int hidKey, int holdMs, int mod) override;
    int physicalButtonPressed(int button) const override;
    bool directSend() const override;
private:
    KmboxNetConnection* conn_;
};

// ─────────────────────────────────────────────────────────────────────────────
// 工厂 —— 对应 AimMagic 的 `FUN_140040ff0`
// ─────────────────────────────────────────────────────────────────────────────
// 一个后端的选择结果。`driver == nullptr` 时 `error` 一定有内容。
struct OpenResult
{
    IDriver* driver = nullptr;
    std::string error;   // 中文, 带证据
};

// 后端名 (配置里 `input_method` 的合法值)。
extern const char* const kBackendMakcu;
extern const char* const kBackendMakcuNew;
extern const char* const kBackendKmboxNet;

// 全部可选后端名, 给 UI 下拉用。
std::vector<std::string> backendNames();

// 按名字建后端。
//
// `makcuPort`/`makcuBaud`、`makcuNewPort`/`makcuNewBaud`、
// `kmboxNetIp`/`kmboxNetPort`/`kmboxNetUuid` 三组参数各自独立 ——
// 不用的那两组传空即可。
//
// ★ 失败时返回 {nullptr, 理由}, **不抛异常**: 设备不在是正常情况, 不是编程错误。
// ★ 不认识的 `backend` 名同样返回理由, 不默默选一个默认的。
OpenResult open(const std::string& backend,
                const std::string& makcuPort, unsigned int makcuBaud,
                const std::string& makcuNewPort, unsigned int makcuNewBaud,
                const std::string& kmboxNetIp, const std::string& kmboxNetPort,
                const std::string& kmboxNetUuid);

// 渲染成一行给日志/UI 的状态串, 形如:
//   "鼠标后端 MAKCUNEW (COM3@6000000) 已连接, 能力: 位移 左键 右键 中键 侧键 滚轮 键盘"
//   "鼠标后端 KMBOXNET (192.168.2.88:6234) 不可用: ...理由..."
std::string describeStatus(const std::string& backend, bool open, const std::string& detail);

} // namespace mouse_driver
