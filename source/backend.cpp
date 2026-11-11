#include "backend.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace {
constexpr u64 timeoutNs = 2000000000ULL;
constexpr u64 disconnectTimeoutNs = 5000000000ULL;
// joycon-turnoff observed Pro Controllers dropping back-to-back subcommands; pause after each ACK.
constexpr u64 proCommandGapNs = 100000000ULL;

u64 Now() { return armTicksToNs(armGetSystemTick()); }
bool SameAddress(const BtdrvAddress& a, const BtdrvAddress& b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}
u8 Expected(int phase) {
    return phase == 1 ? 0x02 : phase == 2 ? 0x07 : phase == 3 ? 0x08 : 0;
}

bool ReadTransport(HidsysUniquePadId padId, u8 side, bool& connected, bool& wired) {
    connected = wired = false;
    bool queryFailed = false;
    for (unsigned slot = 0; slot < 9; ++slot) {
        const auto id = slot == 8 ? HidNpadIdType_Handheld : static_cast<HidNpadIdType>(slot);
        const u32 styles = hidGetNpadStyleSet(id);
        if (!styles) continue;
        HidsysUniquePadId pads[8]{};
        s32 count = 0;
        const Result rc = hidsysGetUniquePadsFromNpad(id, pads, 8, &count);
        if (R_FAILED(rc)) { queryFailed = true; continue; }
        if (!std::any_of(pads, pads + std::clamp(count, 0, 8),
            [&](const HidsysUniquePadId& value) { return value.id == padId.id; })) continue;

        const auto read = [&](const HidNpadCommonState& state, bool split) {
            const u32 attributes = state.attributes;
            const u32 connectedBit = side == SideLeft ? HidNpadAttribute_IsLeftConnected : HidNpadAttribute_IsRightConnected;
            const u32 wiredBit = side == SidePro ? HidNpadAttribute_IsWired
                : side == SideLeft ? HidNpadAttribute_IsLeftWired : HidNpadAttribute_IsRightWired;
            if (split ? (attributes & connectedBit) != 0 : (attributes & HidNpadAttribute_IsConnected) != 0) {
                connected = true;
                wired = wired || (attributes & (wiredBit | HidNpadAttribute_IsWired)) != 0;
            }
        };

        HidNpadCommonState state{};
        if (side == SidePro) {
            // Only the FullKey style is a Pro Controller; NSO/GameCube styles never become candidates.
            if ((styles & HidNpadStyleTag_NpadFullKey) && hidGetNpadStatesFullKey(id, &state, 1)) read(state, false);
            continue;
        }
        if ((styles & HidNpadStyleTag_NpadHandheld) && hidGetNpadStatesHandheld(id, &state, 1)) read(state, true);
        if ((styles & HidNpadStyleTag_NpadJoyDual) && hidGetNpadStatesJoyDual(id, &state, 1)) read(state, true);
        if (side == SideLeft && (styles & HidNpadStyleTag_NpadJoyLeft) && hidGetNpadStatesJoyLeft(id, &state, 1)) read(state, false);
        if (side == SideRight && (styles & HidNpadStyleTag_NpadJoyRight) && hidGetNpadStatesJoyRight(id, &state, 1)) read(state, false);
    }
    return connected || !queryFailed;
}
}

JoyBackend::~JoyBackend() {
    if (mapped) shmemClose(&memory);
    if (driverReady) btdrvExit();
    if (btmReady) btmExit();
    if (hidReady) hidsysExit();
}

std::string JoyBackend::GetSlotName(HidsysUniquePadId padId) {
    for (unsigned slot = 0; slot < 9; ++slot) {
        const auto id = slot == 8 ? HidNpadIdType_Handheld : static_cast<HidNpadIdType>(slot);
        HidsysUniquePadId pads[8]{};
        s32 count = 0;
        if (R_SUCCEEDED(hidsysGetUniquePadsFromNpad(id, pads, 8, &count))) {
            for (s32 i = 0; i < std::clamp(count, 0, 8); ++i) {
                if (pads[i].id == padId.id) {
                    if (id == HidNpadIdType_Handheld) return "Handheld";
                    return "Player " + std::to_string(static_cast<int>(id) + 1);
                }
            }
        }
    }
    return "Unassigned";
}

Result JoyBackend::Open() {
    if (!hosversionAtLeast(13, 0, 0)) return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    if (!hidReady) {
        Result rc = hidsysInitialize();
        if (R_FAILED(rc)) return rc;
        hidReady = true;
    }
    if (!btmReady) {
        Result rc = btmInitialize();
        btmReady = R_SUCCEEDED(rc);
    }
    if (!driverReady) {
        Result rc = btdrvInitialize();
        if (R_FAILED(rc)) return rc;
        driverReady = true;
    }
    if (!mapped) {
        Handle handle = INVALID_HANDLE;
        // Get the report memory only. Do not register/clear HID events or consume its queue.
        Result rc = serviceDispatch(btdrvGetServiceSession(), 38,
            .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
            .out_handles = &handle);
        if (R_FAILED(rc)) return rc;
        // The kernel requires the service's exact remote permission; access stays const below.
        shmemLoadRemote(&memory, handle, 0x3000, Perm_Rw);
        rc = shmemMap(&memory);
        if (R_FAILED(rc)) { shmemClose(&memory); return rc; }
        mapped = true;
        ring = static_cast<const BtdrvCircularBuffer*>(shmemGetAddr(&memory));
    }
    return 0;
}

void JoyBackend::Refresh() {
    if (running) return;
    targets.clear();
    identifying = false;
    Result rc = Open();
    if (R_FAILED(rc)) {
        message = "BT/HID service unavailable (Requires CFW & 13.0+ FW).";
        return;
    }

    snapshotValid = ReadConnections(connectionSnapshot);
    nextConnectionCheck = Now() + 500000000ULL;

    HidsysUniquePadId pads[32]{};
    s32 padCount = 0;
    const Result padsResult = hidsysGetUniquePadIds(pads, 32, &padCount);
    if (R_FAILED(padsResult)) {
        message = "Failed to enumerate controllers.";
        return;
    }

    for (s32 i = 0; i < std::clamp(padCount, 0, 32); ++i) {
        HidsysUniquePadType type{};
        if (R_FAILED(hidsysGetUniquePadType(pads[i], &type))) continue;

        const u8 side = type == HidsysUniquePadType_LeftController ? SideLeft
            : type == HidsysUniquePadType_RightController ? SideRight
            : type == HidsysUniquePadType_FullKeyController ? SidePro : 0;
        if (!side) continue;

        bool connectedNow = false, wiredNow = false;
        const bool transportKnown = ReadTransport(pads[i], side, connectedNow, wiredNow);
        if (side == SidePro && !connectedNow) continue;

        JoyTarget target;
        target.padId = pads[i];
        target.side = side;
        target.wired = wiredNow;
        target.name = ControllerName(side);
        target.slotName = GetSlotName(pads[i]);

        if (!transportKnown || !connectedNow || wiredNow) {
            target.done = true;
            target.status = target.wired ? "Attached to Switch (Detach first)" : "Waiting for Bluetooth...";
            targets.push_back(target);
            continue;
        }

        BtdrvAddress address{};
        if (R_FAILED(hidsysGetUniquePadBluetoothAddress(pads[i], &address))) continue;
        const BtdrvAddress empty{};
        if (SameAddress(address, empty)) continue;

        target.address = address;
        target.status = "Identifying...";
        targets.push_back(target);
    }

    if (targets.empty()) {
        message = "No BT controllers found. Connect them wirelessly.";
        return;
    }

    if (!Observe()) return;
    identifying = running = true;
    message = "Identifying controllers...";
    for (auto& target : targets) {
        if (target.done) continue;
        target.counter = static_cast<u8>(armGetSystemTick() & 0x0f);
        target.phase = 1;
        Send(target, 0x02, 0);
    }
}

void JoyBackend::Update() {
    if (!running) return;
    Receive();

    bool checkConnections = false;
    for (auto& target : targets) {
        if (target.done || target.phase == 0) continue;
        if (target.queuedCommand) {
            if (Now() < target.queuedAt) continue;
            const u8 command = target.queuedCommand, argument = target.queuedArgument;
            target.queuedCommand = 0;
            Send(target, command, argument);
            continue;
        }
        if (target.phase == 4) checkConnections = true;
        else if (Now() >= target.deadline) {
            Fail(target, "Timeout.");
        }
    }

    if (checkConnections) {
        HidsysUniquePadId connected[32]{};
        s32 count = 0;
        const Result rc = hidsysGetUniquePadIds(connected, 32, &count);
        for (auto& target : targets) {
            if (target.done || target.phase != 4 || target.queuedCommand) continue;
            bool present = std::any_of(connected, connected + std::clamp(count, 0, 32),
                [&](const HidsysUniquePadId& id) { return id.id == target.padId.id; });
            if (!present) {
                target.done = true;
                target.status = "OFF (Low Power Mode)";
            } else if (Now() >= target.deadline) {
                Fail(target, "Disconnect unconfirmed.");
            }
        }
    }

    if (!targets.empty()) {
        std::sort(targets.begin(), targets.end(), [](const JoyTarget& a, const JoyTarget& b) {
            auto getPriority = [](const std::string& slot) -> int {
                if (slot == "Player 1") return 1;
                if (slot == "Player 2") return 2;
                if (slot == "Player 3") return 3;
                if (slot == "Player 4") return 4;
                if (slot == "Player 5") return 5;
                if (slot == "Player 6") return 6;
                if (slot == "Player 7") return 7;
                if (slot == "Player 8") return 8;
                if (slot == "Handheld") return 90;
                return 99; // Unassigned
            };
            return getPriority(a.slotName) < getPriority(b.slotName);
        });
    }

    running = std::any_of(targets.begin(), targets.end(), [](const JoyTarget& t) { return !t.done && t.phase != 0; });
    if (!running) {
        if (identifying) {
            identifying = false;
            message = Ready() ? "Ready. Use [A] to turn off selected or [-] for all." : "No ready controller found.";
        } else {
            message = "Operation completed!";
        }
    }
}
bool JoyBackend::Ready() const {
    return mapped && !running && std::any_of(targets.begin(), targets.end(), [](const JoyTarget& t) {
        return !t.failed && !t.done && t.side != 0 && t.phase == 0;
    });
}

bool JoyBackend::ReadConnections(std::string& snapshot) {
    HidsysUniquePadId pads[32]{};
    s32 count = 0;
    if (R_FAILED(hidsysGetUniquePadIds(pads, 32, &count))) return false;
    count = std::clamp(count, 0, 32);
    std::string current;
    for (s32 i = 0; i < count; ++i) {
        HidsysUniquePadType type{};
        if (R_FAILED(hidsysGetUniquePadType(pads[i], &type))) return false;
        current += std::to_string(pads[i].id) + ":" + std::to_string(static_cast<unsigned>(type)) + ";";
    }
    snapshot = current;
    return true;
}

void JoyBackend::AutoRefresh(bool paused) {
    if (paused || running || !hidReady || Now() < nextConnectionCheck) return;
    nextConnectionCheck = Now() + 500000000ULL;
    std::string current;
    if (!ReadConnections(current)) return;
    if (!snapshotValid || current != connectionSnapshot) Refresh();
}

void JoyBackend::ExplainUnavailable() {
    if (running) message = "Operation in progress. Please wait!";
    else if (targets.empty()) message = "No wireless controller connected.";
    else message = "Detach Joy-Cons from console or unplug USB cable first!";
}

bool JoyBackend::Observe() {
    const s32 offset = __atomic_load_n(&ring->write_offset, __ATOMIC_ACQUIRE);
    if (offset < 0 || static_cast<size_t>(offset) >= sizeof(ring->data)) {
        message = "BT Ring Buffer Error.";
        return false;
    }
    cursor = static_cast<size_t>(offset);
    return true;
}

void JoyBackend::Fail(JoyTarget& target, const char* text, Result rc) {
    target.failed = true;
    target.done = true;
    target.status = text;
}

bool JoyBackend::Send(JoyTarget& target, u8 command, u8 argument) {
    BtdrvHidReport report{};
    report.size = 49;
    report.data[0] = 0x01;
    report.data[1] = target.counter++ & 0x0f;
    const u8 neutral[] = {0x00, 0x01, 0x40, 0x40};
    std::memcpy(report.data + 2, neutral, sizeof(neutral));
    std::memcpy(report.data + 6, neutral, sizeof(neutral));
    report.data[10] = command;
    report.data[11] = argument;
    target.sentTick = armGetSystemTick();
    target.deadline = Now() + (command == 0x06 ? disconnectTimeoutNs : timeoutNs);
    
    const Result rc = btdrvWriteHidData(target.address, &report);
    if (R_FAILED(rc)) {
        Fail(target, "Send Failed!", rc);
        return false;
    }
    return true;
}

void JoyBackend::SendNext(JoyTarget& target, u8 command, u8 argument) {
    if (target.side != SidePro) { Send(target, command, argument); return; }
    target.queuedCommand = command;
    target.queuedArgument = argument;
    target.queuedAt = Now() + proCommandGapNs;
}

void JoyBackend::Start() {
    if (!Ready()) { ExplainUnavailable(); return; }
    if (!Observe()) return;
    identifying = false;
    running = true;
    message = "Turning off ALL controllers...";
    for (auto& target : targets) {
        if (target.failed || target.done || !target.side || target.phase != 0) continue;
        target.attempted = true;
        target.counter = static_cast<u8>(armGetSystemTick() & 0x0f);
        target.phase = 1;
        target.status = "Identifying...";
        Send(target, 0x02, 0);
    }
}

void JoyBackend::StartSingle(size_t index) {
    if (index >= targets.size()) return;
    auto& target = targets[index];
    if (target.failed || target.done || target.wired || target.phase != 0) {
        message = "Selected controller is not ready or already off.";
        return;
    }
    if (!Observe()) return;
    identifying = false;
    running = true;
    message = "Turning off " + target.name + " (" + target.slotName + ")...";
    
    target.attempted = true;
    target.counter = static_cast<u8>(armGetSystemTick() & 0x0f);
    target.phase = 1;
    target.status = "Identifying...";
    Send(target, 0x02, 0);
}

void JoyBackend::Response(const BtdrvHidReportEventInfo& event, size_t size, u64 tick) {
    const auto& data = event.data_report.v9;
    const size_t reportOffset = offsetof(BtdrvHidReportEventInfo, data_report.v9.report.data);
    if (size < reportOffset + 15 || data.res != 0 || data.report.size < 15) return;
    
    const auto* reply = data.report.data;
    if (reply[0] != 0x21) return;

    for (auto& target : targets) {
        if (target.done || target.queuedCommand || !SameAddress(target.address, data.addr) || tick < target.sentTick ||
            reply[14] != Expected(target.phase)) continue;

        if (!(reply[13] & 0x80)) {
            Fail(target, "Command Rejected.");
            continue;
        }

        if (target.phase == 1) {
            target.side = reply[17];
            target.name = ControllerName(target.side);
            if (identifying) {
                target.phase = 0;
                target.status = "Ready to turn off";
                continue;
            }
            target.phase = 2;
            target.status = "Clearing pairing...";
            SendNext(target, 0x07, 0);
        } else if (target.phase == 2) {
            target.phase = 3;
            target.status = "Setting Low Power mode...";
            SendNext(target, 0x08, 1);
        } else if (target.phase == 3) {
            target.phase = 4;
            target.status = "Disconnecting...";
            SendNext(target, 0x06, 0);
        }
    }
}

void JoyBackend::Receive() {
    struct Header { u8 type; u8 pad[7]; u64 tick; u64 size; };
    const size_t capacity = sizeof(ring->data);
    const s32 write = __atomic_load_n(&ring->write_offset, __ATOMIC_ACQUIRE);
    if (write < 0 || static_cast<size_t>(write) >= capacity) return;

    size_t budget = capacity;
    while (cursor != static_cast<size_t>(write) && budget >= sizeof(Header)) {
        if (capacity - cursor < sizeof(Header)) { cursor = 0; budget -= sizeof(Header); continue; }
        Header header{};
        std::memcpy(&header, ring->data + cursor, sizeof(header));
        if (header.type == 0xff) { cursor = 0; budget -= sizeof(Header); continue; }
        if (header.size > sizeof(BtdrvHidReportEventInfo) || header.size > capacity - cursor - sizeof(Header)) {
            cursor = static_cast<size_t>(write);
            return;
        }
        BtdrvHidReportEventInfo event{};
        std::memcpy(&event, ring->data + cursor + sizeof(Header), header.size);
        const size_t advance = sizeof(Header) + header.size;
        cursor += advance;
        if (cursor >= capacity) cursor = 0;
        budget = advance > budget ? 0 : budget - advance;

        if (header.type == BtdrvHidEventType_Data) {
            Response(event, header.size, header.tick);
        }
    }
}