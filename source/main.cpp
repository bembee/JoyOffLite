#include <switch.h>
#include <cstdio>
#include <vector>
#include <algorithm>
#include "backend.hpp"

enum ActionType {
    ACTION_TARGET_MIN = 0,
    ACTION_TARGET_MAX = 7,
    ACTION_A = 100,
    ACTION_MINUS = 101,
    ACTION_REFRESH = 102,
    ACTION_EXIT = 103
};

bool IsSelectable(const JoyTarget& t) {
    return !t.done && !t.wired && t.phase == 0 && !t.failed;
}

struct TouchZone {
    u32 startRow, endRow;
    u32 startCol, endCol;
    int action;
};

std::vector<TouchZone> touchZones;

void RenderUI(const JoyBackend& backend, size_t selectedIndex) {
    consoleClear();
    touchZones.clear();

    std::printf("\x1b[1;1H================================================================================\n");
    std::printf("  JoyOff Lite - Controller Manager by Bembee\n");
    std::printf("================================================================================\n");

    u32 currentRow = 3;
    const auto& targets = backend.Targets();

    if (targets.empty()) {
        std::printf("\n  +--------------------------------------------------------------------------+\n");
        std::printf("  |  No wireless controllers detected.                                       |\n");
        std::printf("  |  Connect them over Bluetooth to manage.                                  |\n");
        std::printf("  +--------------------------------------------------------------------------+\n");
    } else {
        size_t maxDisplay = std::min(targets.size(), size_t(8));
        for (size_t i = 0; i < maxDisplay; ++i) {
            const auto& t = targets[i];
            const auto* addr = reinterpret_cast<const u8*>(&t.address);
            const bool ready = IsSelectable(t);
            const bool isSelected = (i == selectedIndex && ready);

            const char* fmtColor = !ready ? "\x1b[2;37m" : (isSelected ? "\x1b[32m" : "");
            const char* fmtHeader = !ready ? "---" : (isSelected ? "===>" : "---");

            std::printf("%s%s [ %-9s ] ---------------------------------------------------------\x1b[0m\n", fmtColor, fmtHeader, t.slotName.c_str());
            std::printf("%s| %-16s | MAC: %02X:%02X:%02X:%02X:%02X:%02X                               |\x1b[0m\n",
                        fmtColor, t.name.c_str(), addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

            if (!ready) {
                if (t.failed) {
                    std::printf("\x1b[2;37m| %-59s \x1b[1;33m[ REFRESH ]\x1b[0m\x1b[2;37m |\x1b[0m\n", t.status.c_str());
                } else {
                    std::printf("\x1b[2;37m| %-57s \x1b[2;37m[ TIMED OUT ]\x1b[0m\x1b[2;37m |\x1b[0m\n", t.status.c_str());
                }
            } else {
                std::printf("%s| %-58s \x1b[1;31m[ TURN OFF ]\x1b[0m%s |\x1b[0m\n", fmtColor, t.status.c_str(), fmtColor);
            }

            touchZones.push_back({currentRow, currentRow + 2, 0, 80, static_cast<int>(i)});
            currentRow += 3;
        }
    }

    std::printf("\x1b[34;1H================================================================================\n");
    std::printf(" STATUS: %-71s\n", backend.Message().c_str());
    std::printf("================================================================================\n");

    std::printf("\x1b[37;1H+-------------------------------------+ +-------------------------------------+\n");
    std::printf("|        [A] TURN OFF SELECTED        | |           [-] TURN OFF ALL          |\n");
    std::printf("+-------------------------------------+ +-------------------------------------+\n");
    touchZones.push_back({37, 39, 0, 39, ACTION_A});
    touchZones.push_back({37, 39, 40, 80, ACTION_MINUS});

    std::printf("\x1b[40;1H+-------------------------------------+ +-------------------------------------+\n");
    std::printf("|         [X] REFRESH LIST            | |               \x1b[1;32m[+] EXIT\x1b[0m              |\n");
    std::printf("+-------------------------------------+ +-------------------------------------+\n");
    touchZones.push_back({40, 42, 0, 39, ACTION_REFRESH});
    touchZones.push_back({40, 42, 40, 80, ACTION_EXIT});

    consoleUpdate(NULL);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    consoleInit(NULL);
    hidInitializeTouchScreen();

    PadState pad;
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    padInitializeAny(&pad);

    JoyBackend backend;
    backend.Refresh();

    size_t selectedIndex = 0;
    bool touching = false;
    bool appRunning = true;

    auto executeSelect = [&]() {
        const auto& targets = backend.Targets();
        if (!targets.empty() && selectedIndex < targets.size()) {
            if (targets[selectedIndex].failed) backend.Refresh();
            else if (IsSelectable(targets[selectedIndex])) backend.StartSingle(selectedIndex);
        }
    };

    auto executeAll = [&]() {
        if (backend.Ready()) backend.Start();
        else backend.ExplainUnavailable();
    };

    while (appletMainLoop() && appRunning) {
        backend.Update();
        backend.AutoRefresh(false);

        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        const auto& targets = backend.Targets();

        if (!targets.empty() && (selectedIndex >= targets.size() || !IsSelectable(targets[selectedIndex]))) {
            auto it = std::find_if(targets.begin(), targets.end(), IsSelectable);
            selectedIndex = (it != targets.end()) ? std::distance(targets.begin(), it) : 0;
        }

        HidTouchScreenState touchState{};
        hidGetTouchScreenStates(&touchState, 1);
        bool touchPressed = (touchState.count > 0) && !touching;
        touching = (touchState.count > 0);

        if (touchPressed) {
            u32 debugCol = touchState.touches[0].x / 16;
            u32 debugRow = touchState.touches[0].y / 16;

            for (const auto& zone : touchZones) {
                if (debugRow >= zone.startRow && debugRow <= zone.endRow &&
                    debugCol >= zone.startCol && debugCol <= zone.endCol) {
                    
                    if (zone.action <= ACTION_TARGET_MAX) {
                        size_t clickedIndex = static_cast<size_t>(zone.action);
                        if (clickedIndex < targets.size()) {
                            if (targets[clickedIndex].failed) backend.Refresh();
                            else if (selectedIndex == clickedIndex && !backend.Running()) backend.StartSingle(selectedIndex);
                            else selectedIndex = clickedIndex;
                        }
                    } else if (!backend.Running()) {
                        switch (zone.action) {
                            case ACTION_A:       executeSelect(); break;
                            case ACTION_MINUS:   executeAll(); break;
                            case ACTION_REFRESH: backend.Refresh(); break;
                            case ACTION_EXIT:    appRunning = false; break;
                        }
                    }
                    break;
                }
            }
        }

        if (!targets.empty() && !backend.Running()) {
            if (down & HidNpadButton_Down) {
                for (size_t i = selectedIndex + 1; i < targets.size(); ++i) {
                    if (IsSelectable(targets[i])) { selectedIndex = i; break; }
                }
            }
            if (down & HidNpadButton_Up && selectedIndex > 0) {
                for (size_t i = selectedIndex - 1; i < targets.size(); --i) {
                    if (IsSelectable(targets[i])) { selectedIndex = i; break; }
                    if (i == 0) break;
                }
            }
        }

        if (!backend.Running()) {
            if (down & HidNpadButton_Plus)  appRunning = false;
            if (down & HidNpadButton_X)     backend.Refresh();
            if (down & HidNpadButton_A)     executeSelect();
            if (down & HidNpadButton_Minus) executeAll();
        }

        RenderUI(backend, selectedIndex);
        svcSleepThread(16666666ULL);
    }

    u64 exitDeadline = armTicksToNs(armGetSystemTick()) + 3000000000ULL;
    while (backend.Running() && armTicksToNs(armGetSystemTick()) < exitDeadline) {
        backend.Update();
        svcSleepThread(10000000ULL);
    }

    consoleExit(NULL);
    return 0;
}