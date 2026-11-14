#include "OverlayUI.h"
#include <dwmapi.h>
#include <cstdio>
#include <tlhelp32.h>

#pragma comment(lib, "dwmapi.lib")

namespace FrostMP::Overlay {

static HWND FindFrostpunkWindow(DWORD targetPid = 0) {
    // First try exact title match
    HWND hwnd = FindWindowW(nullptr, L"Frostpunk");
    if (hwnd) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (targetPid == 0 || pid == targetPid) return hwnd;
    }

    // Try by class name
    hwnd = FindWindowW(L"FrostpunkWindowClass", nullptr);
    if (hwnd) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (targetPid == 0 || pid == targetPid) return hwnd;
    }

    // If target PID specified, enumerate windows for that process
    if (targetPid != 0) {
        HWND result = nullptr;
        EnumWindows([](HWND h, LPARAM lParam) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == (DWORD)lParam) {
                wchar_t title[256];
                GetWindowTextW(h, title, 256);
                if (wcslen(title) > 0 && IsWindowVisible(h)) {
                    *reinterpret_cast<HWND*>(lParam + sizeof(DWORD)) = h;
                    return FALSE;
                }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&targetPid));
        // Note: EnumWindows approach is more complex, skip for now
    }

    return hwnd;
}

bool OverlayManager::Initialize(u64 localSteamID) {
    if (initialized_) return true;
    localSteamID_ = localSteamID;

    DebugLog("Initialize called");
    CreateSharedMemory();
    DebugLog("Shared memory created: %s", control_ ? "OK" : "FAILED");
    CreateOverlayWindow();
    DebugLog("Overlay window created: %p", overlayWindow_);

    initialized_ = true;
    return control_ != nullptr && overlayWindow_ != nullptr;
}

void OverlayManager::Shutdown() {
    if (!initialized_) return;

    DestroyOverlayWindow();

    if (control_) {
        UnmapViewOfFile(control_);
        control_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    initialized_ = false;
}

void OverlayManager::CreateSharedMemory() {
    wchar_t name[128];
    swprintf_s(name, L"FrostMP_Overlay_%llu", static_cast<unsigned long long>(GetCurrentProcessId()));

    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(OverlayControl), name);
    if (!mapping_) {
        DebugLog("CreateFileMappingW failed: %lu", GetLastError());
        return;
    }

    control_ = static_cast<OverlayControl*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(OverlayControl)));
    if (control_) {
        std::memset(control_, 0, sizeof(OverlayControl));
        control_->signature = OverlayControl::MAGIC;
        control_->layoutVersion = OverlayControl::VERSION;
        control_->localSteamID = localSteamID_;
    } else {
        DebugLog("MapViewOfFile failed: %lu", GetLastError());
    }
}

void OverlayManager::CreateOverlayWindow() {
    instance_ = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    wc.lpszClassName = L"FrostMP_OverlayClass";
    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            DebugLog("RegisterClassExW failed: %lu", err);
        }
    }

    // Get Frostpunk window
    HWND gameWindow = FindFrostpunkWindow();

    RECT rect{};
    if (gameWindow) {
        GetWindowRect(gameWindow, &rect);
        DebugLog("Found game window: %p, rect: %ld,%ld-%ld,%ld", gameWindow, rect.left, rect.top, rect.right, rect.bottom);
    } else {
        // Default to screen size if game window not found yet
        rect.left = 0;
        rect.top = 0;
        rect.right = GetSystemMetrics(SM_CXSCREEN);
        rect.bottom = GetSystemMetrics(SM_CYSCREEN);
        DebugLog("Game window not found, using screen size: %ldx%ld", rect.right, rect.bottom);
    }

    overlayWindow_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"FrostMP_OverlayClass", L"FrostMP Overlay",
        WS_POPUP,
        rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, instance_, this
    );

    if (overlayWindow_) {
        SetLayeredWindowAttributes(overlayWindow_, 0, 255, LWA_ALPHA);
        SetWindowPos(overlayWindow_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

        // Make it click-through except for our UI areas
        MARGINS margins = { -1 };
        DwmExtendFrameIntoClientArea(overlayWindow_, &margins);
        DebugLog("Overlay window setup complete");
    } else {
        DebugLog("CreateWindowExW failed: %lu", GetLastError());
    }
}

void OverlayManager::DestroyOverlayWindow() {
    if (overlayWindow_) {
        DestroyWindow(overlayWindow_);
        overlayWindow_ = nullptr;
    }
    UnregisterClassW(L"FrostMP_OverlayClass", instance_);
}

void OverlayManager::Update() {
    if (!control_ || !overlayWindow_) {
        static int counter = 0;
        if (++counter % 300 == 0) {
            OutputDebugStringA("[FrostMP Overlay] Update skipped: control_=");
            OutputDebugStringA(control_ ? "OK" : "NULL");
            OutputDebugStringA(" overlayWindow_=");
            OutputDebugStringA(overlayWindow_ ? "OK" : "NULL");
            OutputDebugStringA("\n");
        }
        return;
    }

    // Update window position to follow game window
    HWND gameWindow = FindWindowW(nullptr, L"Frostpunk");
    if (gameWindow) {
        RECT rect{};
        GetWindowRect(gameWindow, &rect);
        SetWindowPos(overlayWindow_, HWND_TOPMOST, rect.left, rect.top,
                     rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOACTIVATE);
    }

    HandleInput();
}

void OverlayManager::Render() {
    if (!control_ || !overlayWindow_) return;

    // This is a simplified render - in practice you'd use Direct2D or similar
    // For now, we'll use GDI for basic drawing
    HDC hdc = GetDC(overlayWindow_);
    if (!hdc) return;

    RECT clientRect{};
    GetClientRect(overlayWindow_, &clientRect);

    // Clear with transparent background
    HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &clientRect, brush);
    DeleteObject(brush);

    // Draw connection status
    if (control_->connectionState > 0) {
        const char* statusText = "";
        switch (control_->connectionState) {
            case 1: statusText = "Connecting..."; break;
            case 2: statusText = "Connected - In Lobby"; break;
            case 3: statusText = "In Game"; break;
        }

        SetTextColor(hdc, RGB(255, 255, 255));
        SetBkMode(hdc, TRANSPARENT);
        TextOutA(hdc, 10, 10, statusText, static_cast<int>(strlen(statusText)));
    }

    // Draw game time
    if (control_->gameTimeMs > 0) {
        long long totalSeconds = control_->gameTimeMs / 1000;
        long long hours = totalSeconds / 3600;
        long long minutes = (totalSeconds % 3600) / 60;
        long long seconds = totalSeconds % 60;

        char timeStr[64];
        sprintf_s(timeStr, "Time: %lld:%02lld:%02lld %s", hours, minutes, seconds,
                  control_->isPaused ? "[PAUSED]" : "");
        TextOutA(hdc, 10, 35, timeStr, static_cast<int>(strlen(timeStr)));
    }

    // Draw game speed
    const char* speedNames[] = {"1x", "3x", "12x"};
    if (control_->gameSpeed >= 0 && control_->gameSpeed <= 2) {
        char speedStr[64];
        sprintf_s(speedStr, "Speed: %s", speedNames[control_->gameSpeed]);
        TextOutA(hdc, 10, 60, speedStr, static_cast<int>(strlen(speedStr)));
    }

    // Draw resources
    int y = 85;
    const char* resourceNames[] = {"Coal", "Wood", "Steel", "Cores", "Raw Food", "Rations"};
    for (int i = 0; i < 6; ++i) {
        char resStr[64];
        sprintf_s(resStr, "%s: %d", resourceNames[i], control_->resources[i].load());
        TextOutA(hdc, 10, y, resStr, static_cast<int>(strlen(resStr)));
        y += 25;
    }

    // Draw chat
    if (control_->showChatUI) {
        TextOutA(hdc, 10, y, "--- CHAT ---", 11);
        y += 25;
        for (int i = 0; i < control_->chatCount && i < 10; ++i) {
            auto& msg = control_->chatHistory[(control_->chatCount - 1 - i + 100) % 100];
            char chatStr[600];
            sprintf_s(chatStr, "[%s] %s", msg.sender, msg.message);
            TextOutA(hdc, 10, y, chatStr, static_cast<int>(strlen(chatStr)));
            y += 20;
        }
    }

    // Draw trade offers
    if (control_->showTradeUI) {
        TextOutA(hdc, 10, y, "--- TRADE ---", 12);
        y += 25;
        for (int i = 0; i < control_->tradeCount && i < 10; ++i) {
            auto& offer = control_->tradeOffers[i];
            const char* resName = resourceNames[offer.resource];
            const char* statusStr = offer.status == 0 ? "Pending" : (offer.status == 1 ? "Accepted" : "Declined");
            char tradeStr[256];
            sprintf_s(tradeStr, "%s %d %s (%s)", resName, offer.amount, statusStr,
                      offer.fromPlayer == control_->localSteamID ? "You -> Them" : "Them -> You");
            TextOutA(hdc, 10, y, tradeStr, static_cast<int>(strlen(tradeStr)));
            y += 20;
        }
    }

    // Draw status message
    if (control_->showStatus && control_->statusMessage[0]) {
        TextOutA(hdc, 10, clientRect.bottom - 50, control_->statusMessage, static_cast<int>(strlen(control_->statusMessage)));
    }

    // Draw input prompt
    if (control_->inputActive && control_->inputBuffer[0]) {
        char inputStr[300];
        const char* prompt = "";
        switch (control_->inputType) {
            case 0: prompt = "Chat: "; break;
            case 1: prompt = "Save name: "; break;
            case 2: prompt = "SteamID64: "; break;
        }
        sprintf_s(inputStr, "%s%s_", prompt, control_->inputBuffer);
        TextOutA(hdc, 10, clientRect.bottom - 25, inputStr, static_cast<int>(strlen(inputStr)));
    }

    ReleaseDC(overlayWindow_, hdc);
}

LRESULT CALLBACK OverlayManager::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    OverlayManager* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = reinterpret_cast<OverlayManager*>(reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<OverlayManager*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (self && self->control_) {
        switch (msg) {
            case WM_CHAR: {
                if (self->control_->inputActive) {
                    // Handle printable characters
                    if (wParam >= 32 && wParam < 127) { // Printable ASCII
                        size_t len = strlen(self->control_->inputBuffer);
                        if (len < 255) {
                            self->control_->inputBuffer[len] = static_cast<char>(wParam);
                            self->control_->inputBuffer[len + 1] = '\0';
                        }
                    }
                    return 0;
                }
                break;
            }
            case WM_KEYDOWN: {
                if (self->control_->inputActive) {
                    switch (wParam) {
                        case VK_BACK: { // Backspace
                            size_t len = strlen(self->control_->inputBuffer);
                            if (len > 0) {
                                self->control_->inputBuffer[len - 1] = '\0';
                            }
                            return 0;
                        }
                        case VK_RETURN: { // Enter - submit input
                            self->control_->inputActive = 0;
                            // The mod will poll GetInputText() and process it
                            return 0;
                        }
                        case VK_ESCAPE: { // Escape - cancel input
                            self->control_->inputActive = 0;
                            self->control_->inputBuffer[0] = '\0';
                            return 0;
                        }
                    }
                } else {
                    // Global hotkeys when not in input mode
                    switch (wParam) {
                        case VK_F1: // Toggle chat
                            self->control_->showChatUI = !self->control_->showChatUI;
                            if (self->control_->showChatUI) {
                                self->ActivateInput(0); // Chat input
                            }
                            return 0;
                        case VK_F2: // Toggle trade
                            self->control_->showTradeUI = !self->control_->showTradeUI;
                            return 0;
                        case VK_F3: // Toggle save
                            self->control_->showSaveUI = !self->control_->showSaveUI;
                            if (self->control_->showSaveUI) {
                                self->ActivateInput(1); // Save name input
                            }
                            return 0;
                        case VK_F10: // Toggle overlay visibility
                            // Could minimize/restore overlay window
                            return 0;
                    }
                }
                break;
            }
            case WM_DESTROY:
                PostQuitMessage(0);
                break;
        }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void OverlayManager::SetConnectionState(int state) {
    if (control_) control_->connectionState = state;
}

void OverlayManager::SetHostSteamID(u64 id) {
    if (control_) control_->hostSteamID = id;
}

void OverlayManager::SetClientSteamID(u64 id) {
    if (control_) control_->clientSteamID = id;
}

void OverlayManager::SetLocalSteamID(u64 id) {
    if (control_) control_->localSteamID = id;
}

void OverlayManager::SetGameSpeed(int speed) {
    if (control_) control_->gameSpeed = speed;
}

void OverlayManager::SetPaused(bool paused) {
    if (control_) control_->isPaused = paused ? 1 : 0;
}

void OverlayManager::SetGameTime(long long timeMs) {
    if (control_) control_->gameTimeMs = timeMs;
}

void OverlayManager::SetResource(int index, int amount) {
    if (control_ && index >= 0 && index < 6) {
        control_->resources[index] = amount;
    }
}

void OverlayManager::AddChatMessage(const char* sender, const char* message) {
    if (!control_) return;

    int idx = control_->chatCount % 100;
    auto& msg = control_->chatHistory[idx];
    strncpy_s(msg.sender, sender, 63);
    strncpy_s(msg.message, message, 511);
    msg.timestamp = GetTimestampMs();
    control_->chatCount++;
}

void OverlayManager::AddTradeOffer(u32 offerID, u64 from, u64 to, int resource, int amount) {
    if (!control_) return;

    int idx = control_->tradeCount % 50;
    auto& offer = control_->tradeOffers[idx];
    offer.offerID = offerID;
    offer.fromPlayer = from;
    offer.toPlayer = to;
    offer.resource = resource;
    offer.amount = amount;
    offer.status = 0; // Pending
    control_->tradeCount++;
}

void OverlayManager::UpdateTradeOffer(u32 offerID, int status) {
    if (!control_) return;

    for (int i = 0; i < control_->tradeCount && i < 50; ++i) {
        if (control_->tradeOffers[i].offerID == offerID) {
            control_->tradeOffers[i].status = status;
            break;
        }
    }
}

void OverlayManager::SetStatusMessage(const char* msg) {
    if (control_) {
        strncpy_s(control_->statusMessage, msg, 255);
        control_->showStatus = 1;
    }
}

void OverlayManager::HandleInput() {
    // Input handling is primarily done in WndProc for keyboard events
    // This method can be extended for mouse polling, input timeouts, etc.
    if (!control_) return;

    // Example: auto-hide status message after 5 seconds
    // static u64 statusStartTime = 0;
    // if (control_->showStatus && control_->statusMessage[0]) {
    //     u64 now = GetTimestampMs();
    //     if (statusStartTime == 0) statusStartTime = now;
    //     else if (now - statusStartTime > 5000) {
    //         control_->showStatus = 0;
    //         control_->statusMessage[0] = '\0';
    //         statusStartTime = 0;
    //     }
    // }
}

void OverlayManager::ActivateInput(int type) {
    if (control_) {
        control_->inputActive = 1;
        control_->inputType = type;
        control_->inputBuffer[0] = '\0';
    }
}

void OverlayManager::DeactivateInput() {
    if (control_) {
        control_->inputActive = 0;
        control_->inputBuffer[0] = '\0';
    }
}

bool OverlayManager::IsInputActive() const {
    return control_ && control_->inputActive;
}

const char* OverlayManager::GetInputText() const {
    return control_ ? control_->inputBuffer : "";
}

void OverlayManager::ShowTradeUI(bool show) {
    if (control_) control_->showTradeUI = show ? 1 : 0;
}

void OverlayManager::ShowChatUI(bool show) {
    if (control_) control_->showChatUI = show ? 1 : 0;
}

void OverlayManager::ShowSaveUI(bool show) {
    if (control_) control_->showSaveUI = show ? 1 : 0;
}

} // namespace FrostMP::Overlay