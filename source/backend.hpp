#pragma once
#include <switch.h>
#include <string>
#include <vector>

// Controller kinds use the Device Info (subcommand 02) type byte.
constexpr u8 SideLeft = 1;
constexpr u8 SideRight = 2;
constexpr u8 SidePro = 3;

inline const char* ControllerName(u8 side) {
    return side == SideLeft ? "Joy-Con (L)" : side == SideRight ? "Joy-Con (R)"
        : side == SidePro ? "Pro Controller" : "Controller";
}

struct JoyTarget {
    HidsysUniquePadId padId{};
    BtdrvAddress address{};
    std::string name;
    std::string slotName = "Unknown";
    u8 side = 0; // SideLeft, SideRight, or SidePro.
    bool wired = false;
    std::string status = "Waiting...";
    bool failed = false;
    bool done = false;
    int phase = 0;
    u8 counter = 0;
    u8 identityRetries = 0;
    bool attempted = false;
    u64 sentTick = 0;
    u64 deadline = 0;
    // Pro Controllers get a short pause between acknowledged commands.
    u8 queuedCommand = 0;
    u8 queuedArgument = 0;
    u64 queuedAt = 0;
};

class JoyBackend {
    bool hidReady = false;
    bool btmReady = false;
    bool driverReady = false;
    bool running = false;
    bool identifying = false;
    SharedMemory memory{};
    bool mapped = false;
    const BtdrvCircularBuffer* ring = nullptr;
    size_t cursor = 0;
    std::vector<JoyTarget> targets;
    std::string message;
    std::string connectionSnapshot;
    bool snapshotValid = false;
    u64 nextConnectionCheck = 0;

    Result Open();
    bool ReadConnections(std::string& snapshot);
    bool Observe();
    bool Send(JoyTarget& target, u8 command, u8 argument);
    void SendNext(JoyTarget& target, u8 command, u8 argument);
    void Receive();
    void Response(const BtdrvHidReportEventInfo& event, size_t size, u64 tick);
    void Fail(JoyTarget& target, const char* text, Result rc = 0);
    std::string GetSlotName(HidsysUniquePadId padId);

public:
    ~JoyBackend();
    void Refresh();
    void Start();
    void StartSingle(size_t index);
    void Update();
    void AutoRefresh(bool paused);
    void ExplainUnavailable();
    bool Running() const { return running; }
    bool Ready() const;
    const std::vector<JoyTarget>& Targets() const { return targets; }
    const std::string& Message() const { return message; }
};
