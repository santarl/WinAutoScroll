#define _USE_MATH_DEFINES
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OEMRESOURCE
#include <windows.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>

#pragma comment (lib, "Gdiplus.lib")
#pragma comment (lib, "User32.lib")
#pragma comment (lib, "Gdi32.lib")
#pragma comment (lib, "Shell32.lib")
#pragma comment (lib, "Kernel32.lib")

using namespace Gdiplus;

// --- CONFIGURATION ---
const char* REPO_SCRIPT_URL = "https://raw.githubusercontent.com/santarl/WinAutoScroll/refs/heads/main/upload_stats.ps1";

// --- Constants & Messages ---
#define WM_TRAYICON         (WM_APP + 1)
#define WM_APP_MBUTTON_DOWN (WM_APP + 10)
#define WM_APP_MBUTTON_UP   (WM_APP + 11)
#define WM_APP_MOUSE_MOVE   (WM_APP + 12)
#define WM_APP_KEY_DOWN     (WM_APP + 13)
#define WM_APP_KEY_UP       (WM_APP + 14)
#define WM_APP_CANCEL       (WM_APP + 15)

#define ID_MENU_EDIT_CONFIG 1000
#define ID_MENU_RELOAD      1001
#define ID_MENU_EXIT        1002
#define ID_MENU_PAUSE       1003
#define ID_MENU_STATS       1004
#define ID_MENU_UPLOAD      1005

// --- Enums ---
typedef enum { STATE_IDLE, STATE_PRIMED, STATE_SCROLLING, STATE_STOPPING } ScrollState;
typedef enum { SHAPE_CIRCLE, SHAPE_SQUARE, SHAPE_CROSS } Shape;
typedef enum { MODE_TOGGLE, MODE_HOLD } TriggerMode;
typedef enum { CURSOR_NONE, CURSOR_ALL, CURSOR_NS, CURSOR_WE, CURSOR_NWSE, CURSOR_NESW } ScrollCursorType;

// --- Config & Stats ---
typedef struct
{
    int min_scroll, max_scroll;
    float sensitivity, ramp_exponent;
    int update_frequency;
    int trigger_vk_code, trigger_middle_mouse, emulate_touchpad_scrolling;
    TriggerMode trigger_mode;
    int middle_mouse_passthrough, keyboard_passthrough, drag_threshold;
    Shape dead_zone_shape;
    int dead_zone, cross_dead_zone_thickness;
    int show_indicator;
    Shape indicator_shape;
    int indicator_size, indicator_cross_thickness;
    int indicator_color_r, indicator_color_g, indicator_color_b, indicator_color_a;
    float indicator_thickness;
    int indicator_filled;
    int fun_stats;
    int natural_scrolling;
} AppConfig;

typedef struct
{
    unsigned long long total_pixels;
    unsigned long long dir_up, dir_down, dir_left, dir_right;
    unsigned long long session_pixels;
} Stats;

AppConfig g_config = { 1, 1000, 0.01f, 4.0f, 60, 0, 1, 0, MODE_HOLD, 1, 1, 40, SHAPE_CROSS, 1, 10, 1, SHAPE_CIRCLE, 25, 10, 100, 100, 100, 180, 1.5f, 0, 1, 0 };
Stats g_stats = { 0 };


// --- Global State ---
HHOOK g_hMouseHook, g_hKeyboardHook;
volatile ScrollState g_scrollState = STATE_IDLE;
volatile BOOL g_isPaused = FALSE;
POINT g_startScrollPos, g_primeStartPos;
ULONG_PTR g_gdiplusToken;
HWND g_hMainWnd, g_hOverlayWnd;
HINSTANCE g_hInstance;
ScrollCursorType g_currentCursorType = CURSOR_NONE;
char g_statsPath[MAX_PATH];

// --- Cached Cursors ---
HCURSOR g_hCursorAll = NULL, g_hCursorNS = NULL, g_hCursorWE = NULL, g_hCursorNWSE = NULL, g_hCursorNESW = NULL;

// --- Prototypes ---
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK LowLevelMouseProc(int, WPARAM, LPARAM);
LRESULT CALLBACK LowLevelKeyboardProc(int, WPARAM, LPARAM);
DWORD WINAPI ScrollingThread(LPVOID);
void StartScrolling();
void StopScrolling();
void LoadConfig(const char*);
void LoadStats();
void SaveStats();
void CopyToClipboard(const char* text);
void ShowLocalStats();
void ShowUploadDialog();
void AddTrayIcon();
void RemoveTrayIcon();
void ShowContextMenu();
void UpdateTrayIconState();
void SetScrollCursor(ScrollCursorType);
void RestoreSystemCursors();
void CreateOverlayWindow();
void RenderAndShowOverlay(POINT center);
void HideOverlay();
int CalculateScrollAmount(int delta, BOOL isTouchpad);
void SendMouseInput(DWORD flags, DWORD mouseData);
void LoadCursors();
char* Trim(char*);

// --- Entry Point ---
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    SetProcessDPIAware();
    g_hInstance = hInstance;

    GetModuleFileName(NULL, g_statsPath, MAX_PATH);
    char* lastSlash = strrchr(g_statsPath, '\\');
    if (lastSlash) *(lastSlash + 1) = 0;
    strcat_s(g_statsPath, MAX_PATH, "stats.ini");

    WNDCLASSEX wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "ScrollAppHidden";
    RegisterClassEx(&wc);
    g_hMainWnd = CreateWindowEx(0, "ScrollAppHidden", "WinAutoScroll", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);

    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);

    LoadConfig("config.ini");
    LoadStats();
    LoadCursors();
    CreateOverlayWindow();
    AddTrayIcon();

    g_hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, hInstance, 0);
    g_hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(g_hMouseHook);
    UnhookWindowsHookEx(g_hKeyboardHook);
    RemoveTrayIcon();
    GdiplusShutdown(g_gdiplusToken);
    return (int)msg.wParam;
}

// --- Logic ---
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) ShowContextMenu();
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case ID_MENU_EDIT_CONFIG: ShellExecute(NULL, "edit", "config.ini", NULL, NULL, SW_SHOWNORMAL); break;
        case ID_MENU_RELOAD: LoadConfig("config.ini"); LoadCursors(); break;
        case ID_MENU_STATS: ShowLocalStats(); break;
        case ID_MENU_UPLOAD: ShowUploadDialog(); break;
        case ID_MENU_EXIT: DestroyWindow(hWnd); break;
        case ID_MENU_PAUSE:
            g_isPaused = !g_isPaused;
            if (g_isPaused) StopScrolling();
            UpdateTrayIconState();
            break;
        }
        break;
    case WM_APP_MBUTTON_DOWN:
        if (g_scrollState == STATE_IDLE)
        {
            g_scrollState = STATE_PRIMED;
            GetCursorPos(&g_primeStartPos);
        }
        else if (g_scrollState == STATE_SCROLLING && g_config.trigger_mode == MODE_TOGGLE)
        {
            StopScrolling();
        }
        break;
    case WM_APP_MBUTTON_UP:
        if (g_scrollState == STATE_PRIMED)
        {
            g_scrollState = STATE_IDLE;
            if (g_config.middle_mouse_passthrough)
            {
                SendMouseInput(MOUSEEVENTF_MIDDLEDOWN, 0);
                SendMouseInput(MOUSEEVENTF_MIDDLEUP, 0);
            }
        }
        else if (g_scrollState == STATE_SCROLLING && g_config.trigger_mode == MODE_HOLD)
        {
            StopScrolling();
        }
        break;
    case WM_APP_MOUSE_MOVE:
        if (g_scrollState == STATE_PRIMED)
        {
            POINT c;
            GetCursorPos(&c);
            if (abs(c.x - g_primeStartPos.x) > g_config.drag_threshold || abs(c.y - g_primeStartPos.y) > g_config.drag_threshold)
            {
                StartScrolling();
            }
        }
        break;
    case WM_APP_KEY_DOWN:
        if (g_config.trigger_mode == MODE_HOLD)
        {
            StartScrolling();
        }
        else
        {
            if (g_scrollState == STATE_IDLE) StartScrolling();
            else if (g_scrollState == STATE_SCROLLING) StopScrolling();
        }
        break;
    case WM_APP_KEY_UP:
        if (g_config.trigger_mode == MODE_HOLD) StopScrolling();
        break;
    case WM_APP_CANCEL:
        if (g_scrollState == STATE_PRIMED) g_scrollState = STATE_IDLE;
        else StopScrolling();
        break;
    case WM_DESTROY:
        SaveStats();
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

void StartScrolling()
{
    if (g_scrollState == STATE_IDLE || g_scrollState == STATE_PRIMED)
    {
        if (g_scrollState == STATE_IDLE) GetCursorPos(&g_startScrollPos);
        else g_startScrollPos = g_primeStartPos;

        g_scrollState = STATE_SCROLLING;
        SetScrollCursor(CURSOR_ALL);

        if (g_config.show_indicator) RenderAndShowOverlay(g_startScrollPos);

        HANDLE hThread = CreateThread(NULL, 0, ScrollingThread, NULL, 0, NULL);
        if (hThread) CloseHandle(hThread);
    }
}

void StopScrolling()
{
    if (g_scrollState == STATE_SCROLLING)
    {
        HideOverlay();
        g_scrollState = STATE_STOPPING;
        if (g_config.fun_stats) SaveStats();
    }
}

// --- Hooks & Thread ---
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        MSLLHOOKSTRUCT* pMouse = (MSLLHOOKSTRUCT*)lParam;
        if (pMouse->flags & LLMHF_INJECTED) return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);

        if (!g_isPaused && g_config.trigger_middle_mouse)
        {
            if (wParam == WM_MBUTTONDOWN)
            {
                PostMessage(g_hMainWnd, WM_APP_MBUTTON_DOWN, 0, 0);
                return 1;
            }
            if (wParam == WM_MOUSEMOVE && g_scrollState == STATE_PRIMED)
            {
                PostMessage(g_hMainWnd, WM_APP_MOUSE_MOVE, 0, 0);
            }
            if (wParam == WM_MBUTTONUP)
            {
                if (g_scrollState == STATE_PRIMED || g_scrollState == STATE_SCROLLING)
                {
                    PostMessage(g_hMainWnd, WM_APP_MBUTTON_UP, 0, 0);
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && !g_isPaused)
    {
        PKBDLLHOOKSTRUCT p = (PKBDLLHOOKSTRUCT)lParam;
        if (wParam == WM_KEYDOWN && p->vkCode == VK_ESCAPE && (g_scrollState == STATE_SCROLLING || g_scrollState == STATE_PRIMED))
        {
            PostMessage(g_hMainWnd, WM_APP_CANCEL, 0, 0);
            return 1;
        }
        if (g_config.trigger_vk_code != 0 && p->vkCode == g_config.trigger_vk_code)
        {
            if (wParam == WM_KEYDOWN) PostMessage(g_hMainWnd, WM_APP_KEY_DOWN, 0, 0);
            else if (wParam == WM_KEYUP) PostMessage(g_hMainWnd, WM_APP_KEY_UP, 0, 0);
            if (!g_config.keyboard_passthrough) return 1;
        }
    }
    return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

DWORD WINAPI ScrollingThread(LPVOID lpParameter)
{
    while (g_scrollState == STATE_SCROLLING)
    {
        POINT currentPos;
        GetCursorPos(&currentPos);
        int dx = currentPos.x - g_startScrollPos.x;
        int dy = currentPos.y - g_startScrollPos.y;
        int vS = 0, hS = 0;
        bool act = false;

        if (g_config.dead_zone_shape == SHAPE_SQUARE)
        {
            act = (abs(dx) > g_config.dead_zone || abs(dy) > g_config.dead_zone);
        }
        else if (g_config.dead_zone_shape == SHAPE_CROSS)
        {
            act = (abs(dx) > g_config.cross_dead_zone_thickness || abs(dy) > g_config.cross_dead_zone_thickness);
        }
        else
        {
            act = (sqrt((double)dx * dx + (double)dy * dy) > g_config.dead_zone);
        }

        if (act)
        {
            if (g_config.dead_zone_shape == SHAPE_CROSS)
            {
                if (abs(dx) <= g_config.cross_dead_zone_thickness)
                {
                    vS = -CalculateScrollAmount(dy, g_config.emulate_touchpad_scrolling);
                }
                else if (abs(dy) <= g_config.cross_dead_zone_thickness)
                {
                    hS = CalculateScrollAmount(dx, g_config.emulate_touchpad_scrolling);
                }
                else
                {
                    vS = -CalculateScrollAmount(dy, g_config.emulate_touchpad_scrolling);
                    hS = CalculateScrollAmount(dx, g_config.emulate_touchpad_scrolling);
                }
            }
            else
            {
                vS = -CalculateScrollAmount(dy, g_config.emulate_touchpad_scrolling);
                hS = CalculateScrollAmount(dx, g_config.emulate_touchpad_scrolling);
            }

            // --- NEW: Natural Scrolling Logic ---
            if (g_config.natural_scrolling) {
                vS = -vS;
                hS = -hS;
            }

            if (vS != 0) SendMouseInput(MOUSEEVENTF_WHEEL, (DWORD)vS);
            if (hS != 0) SendMouseInput(MOUSEEVENTF_HWHEEL, (DWORD)hS);

            if (g_config.fun_stats)
            {
                unsigned long long moved = abs(vS) + abs(hS);
                g_stats.total_pixels += moved;
                g_stats.session_pixels += moved;
                
                int logicalVs = g_config.natural_scrolling ? -vS : vS;
                int logicalHs = g_config.natural_scrolling ? -hS : hS;

                if (logicalVs > 0) g_stats.dir_up += logicalVs;
                if (logicalVs < 0) g_stats.dir_down += abs(logicalVs);
                if (logicalHs > 0) g_stats.dir_right += logicalHs;
                if (logicalHs < 0) g_stats.dir_left += abs(logicalHs);
            }
        }

        ScrollCursorType target = CURSOR_ALL;
        if (act)
        {
            double angle = atan2((double)dy, (double)dx) * 180.0 / M_PI;
            if (fabs(angle) <= 22.5 || fabs(angle) >= 157.5) target = CURSOR_WE;
            else if (fabs(angle) >= 67.5 && fabs(angle) <= 112.5) target = CURSOR_NS;
            else
            {
                if ((dx > 0 && dy > 0) || (dx < 0 && dy < 0)) target = CURSOR_NWSE;
                else target = CURSOR_NESW;
            }
        }
        if (g_currentCursorType != target) SetScrollCursor(target);

        int freq = g_config.update_frequency;
        if (freq <= 0) freq = 60;
        Sleep(1000 / freq);
    }
    RestoreSystemCursors();
    g_scrollState = STATE_IDLE;
    return 0;
}

// --- Helper Funcs ---
int CalculateScrollAmount(int delta, BOOL isTouchpad)
{
    if (delta == 0) return 0;
    double val = pow(abs(delta) * g_config.sensitivity, g_config.ramp_exponent);
    int res = (int)val;
    if (res < g_config.min_scroll) res = g_config.min_scroll;
    if (res > g_config.max_scroll) res = g_config.max_scroll;
    return (delta < 0 ? -res : res);
}

void SendMouseInput(DWORD flags, DWORD mouseData)
{
    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    input.mi.mouseData = mouseData;
    SendInput(1, &input, sizeof(INPUT));
}

char* Trim(char* str)
{
    char* end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    *(end + 1) = 0;
    return str;
}

// --- Stats & Clipboard ---
void LoadStats()
{
    char buf[32];
    GetPrivateProfileString("Stats", "TotalPixels", "0", buf, 32, g_statsPath); g_stats.total_pixels = strtoull(buf, NULL, 10);
    GetPrivateProfileString("Stats", "Up", "0", buf, 32, g_statsPath); g_stats.dir_up = strtoull(buf, NULL, 10);
    GetPrivateProfileString("Stats", "Down", "0", buf, 32, g_statsPath); g_stats.dir_down = strtoull(buf, NULL, 10);
    GetPrivateProfileString("Stats", "Left", "0", buf, 32, g_statsPath); g_stats.dir_left = strtoull(buf, NULL, 10);
    GetPrivateProfileString("Stats", "Right", "0", buf, 32, g_statsPath); g_stats.dir_right = strtoull(buf, NULL, 10);
    GetPrivateProfileString("Stats", "Unuploaded", "0", buf, 32, g_statsPath); g_stats.session_pixels = strtoull(buf, NULL, 10);
}

void SaveStats()
{
    char buf[32];
    sprintf_s(buf, "%llu", g_stats.total_pixels); WritePrivateProfileString("Stats", "TotalPixels", buf, g_statsPath);
    sprintf_s(buf, "%llu", g_stats.dir_up); WritePrivateProfileString("Stats", "Up", buf, g_statsPath);
    sprintf_s(buf, "%llu", g_stats.dir_down); WritePrivateProfileString("Stats", "Down", buf, g_statsPath);
    sprintf_s(buf, "%llu", g_stats.dir_left); WritePrivateProfileString("Stats", "Left", buf, g_statsPath);
    sprintf_s(buf, "%llu", g_stats.dir_right); WritePrivateProfileString("Stats", "Right", buf, g_statsPath);
    sprintf_s(buf, "%llu", g_stats.session_pixels); WritePrivateProfileString("Stats", "Unuploaded", buf, g_statsPath);
}

void CopyToClipboard(const char* text)
{
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, strlen(text) + 1);
    if (hGlob)
    {
        memcpy(GlobalLock(hGlob), text, strlen(text) + 1);
        GlobalUnlock(hGlob);
        SetClipboardData(CF_TEXT, hGlob);
    }
    CloseClipboard();
}

void ShowLocalStats()
{
    double meters = (double)g_stats.total_pixels * 0.0254 / 96.0;

    char msg[1024];
    sprintf_s(msg,
        "WinAutoScroll Statistics\n\n"
        "Total Scrolled: %.2f virtual metres\n"
        "Total Pixels: %llu\n\n"
        "Session Pixels (Unuploaded): %llu\n\n"
        "Direction Breakdown:\n"
        "  Up: %llu\n  Down: %llu\n  Left: %llu\n  Right: %llu",
        meters, g_stats.total_pixels, g_stats.session_pixels,
        g_stats.dir_up, g_stats.dir_down, g_stats.dir_left, g_stats.dir_right);

    // Silent MessageBox
    MessageBox(g_hMainWnd, msg, "Local Stats", MB_OK);
}

void ShowUploadDialog()
{
    SaveStats();

    char psCommand[2048];
    sprintf_s(psCommand,
        "powershell -NoProfile -Command \"& { `$WASPath='%s'; irm %s | iex }\"",
        g_statsPath, REPO_SCRIPT_URL);

    char msg[2048];
    sprintf_s(msg,
        "To contribute to the Global Counter:\n\n"
        "1. Click 'Yes' to copy the upload command.\n"
        "2. Paste it into a PowerShell window.\n\n"
        "Pending Upload: %llu pixels\n\n"
        "NOTE: The application will automatically restart upon successful upload.",
        g_stats.session_pixels);

    if (MessageBox(g_hMainWnd, msg, "Upload Stats", MB_YESNO | MB_ICONINFORMATION) == IDYES)
    {
        CopyToClipboard(psCommand);
    }
}

// --- Config Loading & Misc ---
void LoadConfig(const char* filename)
{
    FILE* file;
    if (fopen_s(&file, filename, "r") != 0 || !file) return;

    char line[256];
    while (fgets(line, sizeof(line), file))
    {
        if (line[0] == '#' || line[0] == '\n') continue;
        char* delim = strchr(line, '=');
        if (!delim) continue;

        *delim = 0;
        char* key = Trim(line);
        char* val = Trim(delim + 1);

        if (!strcmp(key, "min_scroll")) g_config.min_scroll = atoi(val);
        else if (!strcmp(key, "max_scroll")) g_config.max_scroll = atoi(val);
        else if (!strcmp(key, "sensitivity")) g_config.sensitivity = (float)atof(val);
        else if (!strcmp(key, "ramp_exponent")) g_config.ramp_exponent = (float)atof(val);
        else if (!strcmp(key, "update_frequency")) g_config.update_frequency = atoi(val);
        else if (!strcmp(key, "trigger_middle_mouse")) g_config.trigger_middle_mouse = atoi(val);
        else if (!strcmp(key, "trigger_vk_code")) g_config.trigger_vk_code = strtol(val, NULL, 0);
        else if (!strcmp(key, "emulate_touchpad_scrolling")) g_config.emulate_touchpad_scrolling = atoi(val);
        else if (!strcmp(key, "middle_mouse_passthrough")) g_config.middle_mouse_passthrough = atoi(val);
        else if (!strcmp(key, "keyboard_passthrough")) g_config.keyboard_passthrough = atoi(val);
        else if (!strcmp(key, "drag_threshold")) g_config.drag_threshold = atoi(val);
        else if (!strcmp(key, "dead_zone")) g_config.dead_zone = atoi(val);
        else if (!strcmp(key, "cross_dead_zone_thickness")) g_config.cross_dead_zone_thickness = atoi(val);
        else if (!strcmp(key, "show_indicator")) g_config.show_indicator = atoi(val);
        else if (!strcmp(key, "indicator_size")) g_config.indicator_size = atoi(val);
        else if (!strcmp(key, "indicator_cross_thickness")) g_config.indicator_cross_thickness = atoi(val);
        else if (!strcmp(key, "indicator_color_r")) g_config.indicator_color_r = atoi(val);
        else if (!strcmp(key, "indicator_color_g")) g_config.indicator_color_g = atoi(val);
        else if (!strcmp(key, "indicator_color_b")) g_config.indicator_color_b = atoi(val);
        else if (!strcmp(key, "indicator_color_a")) g_config.indicator_color_a = atoi(val);
        else if (!strcmp(key, "indicator_thickness")) g_config.indicator_thickness = (float)atof(val);
        else if (!strcmp(key, "indicator_filled")) g_config.indicator_filled = atoi(val);
        else if (!strcmp(key, "fun_stats")) g_config.fun_stats = atoi(val);
        else if (!strcmp(key, "natural_scrolling")) g_config.natural_scrolling = atoi(val);
        else if (!strcmp(key, "trigger_mode"))
        {
            g_config.trigger_mode = (_stricmp(val, "hold") == 0) ? MODE_HOLD : MODE_TOGGLE;
        }
        else if (!strcmp(key, "dead_zone_shape"))
        {
            if (_stricmp(val, "square") == 0) g_config.dead_zone_shape = SHAPE_SQUARE;
            else if (_stricmp(val, "cross") == 0) g_config.dead_zone_shape = SHAPE_CROSS;
            else g_config.dead_zone_shape = SHAPE_CIRCLE;
        }
        else if (!strcmp(key, "indicator_shape"))
        {
            if (_stricmp(val, "square") == 0) g_config.indicator_shape = SHAPE_SQUARE;
            else if (_stricmp(val, "cross") == 0) g_config.indicator_shape = SHAPE_CROSS;
            else g_config.indicator_shape = SHAPE_CIRCLE;
        }
    }
    fclose(file);
}

void UpdateTrayIconState()
{
    NOTIFYICONDATA nid = { 0 };
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = g_hMainWnd;
    nid.uID = 1;
    nid.uFlags = NIF_TIP | NIF_ICON;

    HMODULE hShell32 = LoadLibraryEx("shell32.dll", NULL, LOAD_LIBRARY_AS_DATAFILE);
    HICON hBaseIcon = NULL;
    if (hShell32)
    {
        hBaseIcon = (HICON)LoadImage(hShell32, MAKEINTRESOURCE(250), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
        FreeLibrary(hShell32);
    }
    if (!hBaseIcon) hBaseIcon = LoadIcon(NULL, IDI_APPLICATION);

    if (g_isPaused)
    {
        Bitmap* bmp = Bitmap::FromHICON(hBaseIcon);
        if (bmp)
        {
            Graphics g(bmp);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            Pen redPen(Color(220, 200, 15, 30), 8);
            int w = bmp->GetWidth();
            int h = bmp->GetHeight();
            g.DrawLine(&redPen, 0, 0, w, h);
            g.DrawLine(&redPen, 0, h, w, 0);

            HICON hPausedIcon = NULL;
            bmp->GetHICON(&hPausedIcon);
            delete bmp;
            DestroyIcon(hBaseIcon);
            nid.hIcon = hPausedIcon;
        }
        else
        {
            nid.hIcon = hBaseIcon;
        }
        strcpy_s(nid.szTip, "WinAutoScroll - Paused");
    }
    else
    {
        nid.hIcon = hBaseIcon;
        strcpy_s(nid.szTip, "WinAutoScroll - Active");
    }
    Shell_NotifyIcon(NIM_MODIFY, &nid);
    DestroyIcon(nid.hIcon);
}

void AddTrayIcon()
{
    NOTIFYICONDATA nid = { 0 };
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = g_hMainWnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAYICON;
    Shell_NotifyIcon(NIM_ADD, &nid);
    UpdateTrayIconState();
}

void RemoveTrayIcon()
{
    NOTIFYICONDATA nid = { 0 };
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = g_hMainWnd;
    nid.uID = 1;
    Shell_NotifyIcon(NIM_DELETE, &nid);
}

void ShowContextMenu()
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    AppendMenu(hMenu, MF_STRING, ID_MENU_PAUSE, g_isPaused ? "Resume" : "Pause");
    
    if (g_config.fun_stats) {
        AppendMenu(hMenu, MF_STRING, ID_MENU_STATS, "View Stats");
        AppendMenu(hMenu, MF_STRING, ID_MENU_UPLOAD, "Upload Stats");
    }
    
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, ID_MENU_EDIT_CONFIG, "Edit Config");
    AppendMenu(hMenu, MF_STRING, ID_MENU_RELOAD, "Reload Config");
    AppendMenu(hMenu, MF_STRING, ID_MENU_EXIT, "Exit");
    SetForegroundWindow(g_hMainWnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, g_hMainWnd, NULL);
    DestroyMenu(hMenu);
}

void CreateOverlayWindow()
{
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), 0, DefWindowProc, 0, 0, g_hInstance, NULL, NULL, NULL, NULL, "ScrollOverlay", NULL };
    RegisterClassEx(&wc);
    g_hOverlayWnd = CreateWindowEx(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, "ScrollOverlay", NULL, WS_POPUP, 0, 0, 0, 0, NULL, NULL, g_hInstance, NULL);
}

void RenderAndShowOverlay(POINT center)
{
    int s = g_config.indicator_size;
    int w = s * 2 + 4, h = s * 2 + 4;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, w, h);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

    Graphics g(hdcMem);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));

    Color c(g_config.indicator_color_a, g_config.indicator_color_r, g_config.indicator_color_g, g_config.indicator_color_b);
    SolidBrush brush(c);
    Pen pen(c, g_config.indicator_thickness);
    int mid = w / 2;

    switch (g_config.indicator_shape)
    {
    case SHAPE_SQUARE:
        if (g_config.indicator_filled) g.FillRectangle(&brush, mid - s, mid - s, s * 2, s * 2);
        else g.DrawRectangle(&pen, mid - s, mid - s, s * 2, s * 2);
        break;
    case SHAPE_CROSS:
        g.FillRectangle(&brush, mid - s, mid - g_config.indicator_cross_thickness, s * 2, g_config.indicator_cross_thickness * 2);
        g.FillRectangle(&brush, mid - g_config.indicator_cross_thickness, mid - s, g_config.indicator_cross_thickness * 2, s * 2);
        break;
    case SHAPE_CIRCLE:
    default:
        if (g_config.indicator_filled) g.FillEllipse(&brush, mid - s, mid - s, s * 2, s * 2);
        else g.DrawEllipse(&pen, mid - s, mid - s, s * 2, s * 2);
        break;
    }

    POINT ptSrc = { 0, 0 };
    POINT ptDst = { center.x - mid, center.y - mid };
    SIZE size = { w, h };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_hOverlayWnd, hdcScreen, &ptDst, &size, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
    ShowWindow(g_hOverlayWnd, SW_SHOWNOACTIVATE);

    SelectObject(hdcMem, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

void HideOverlay()
{
    ShowWindow(g_hOverlayWnd, SW_HIDE);
}

HCURSOR LoadDynamicCursor(const char* file)
{
    char path[MAX_PATH];
    ExpandEnvironmentStrings(file, path, MAX_PATH);
    return LoadCursorFromFile(path);
}

void LoadCursors()
{
    g_hCursorAll = LoadCursor(NULL, IDC_SIZEALL);
    g_hCursorNS = LoadDynamicCursor("%SystemRoot%\\Cursors\\lns.cur");
    g_hCursorWE = LoadDynamicCursor("%SystemRoot%\\Cursors\\lwe.cur");
    g_hCursorNWSE = LoadDynamicCursor("%SystemRoot%\\Cursors\\lnwse.cur");
    g_hCursorNESW = LoadDynamicCursor("%SystemRoot%\\Cursors\\lnesw.cur");
    if (!g_hCursorNS) g_hCursorNS = LoadCursor(NULL, IDC_SIZENS);
    if (!g_hCursorWE) g_hCursorWE = LoadCursor(NULL, IDC_SIZEWE);
    if (!g_hCursorNWSE) g_hCursorNWSE = LoadCursor(NULL, IDC_SIZENWSE);
    if (!g_hCursorNESW) g_hCursorNESW = LoadCursor(NULL, IDC_SIZENESW);
}

void SetScrollCursor(ScrollCursorType t)
{
    if (t == g_currentCursorType) return;
    HCURSOR target = g_hCursorAll;
    switch (t)
    {
    case CURSOR_NS: target = g_hCursorNS; break;
    case CURSOR_WE: target = g_hCursorWE; break;
    case CURSOR_NWSE: target = g_hCursorNWSE; break;
    case CURSOR_NESW: target = g_hCursorNESW; break;
    default: target = g_hCursorAll; break;
    }
    SetSystemCursor(CopyCursor(target), OCR_NORMAL);
    g_currentCursorType = t;
}

void RestoreSystemCursors()
{
    if (g_currentCursorType != CURSOR_NONE)
    {
        g_currentCursorType = CURSOR_NONE;
        SystemParametersInfo(SPI_SETCURSORS, 0, NULL, SPIF_SENDCHANGE);
    }
}