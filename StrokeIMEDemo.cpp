#include <windows.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include "resource1.h"

// ===== 全域變數 =====
std::wstring strokeBuffer;
std::wstring strokeCode;
std::wstring committedText;

struct DictEntry
{
    std::wstring word;
    std::wstring code;
    int frequency; //儲存頻率
};
std::vector<DictEntry> dictionary;
std::vector<std::wstring> candidates;

int currentPage = 0;
int selectedIndex = 0;
const int pageSize = 10;

HHOOK g_hHook = NULL;
HWND  g_hwnd = NULL;
bool  g_imeActive = false;
bool  g_visible = false;   // 預設隱藏

// ===== 自訂訊息 =====
#define WM_IME_KEY  (WM_APP + 1)
#define WM_IME_CHAR (WM_APP + 2)

// ===== SendInput：把文字注入目前焦點視窗 =====
void SendTextToFocus(const std::wstring& text)
{
    std::vector<INPUT> inputs;
    inputs.reserve(text.size() * 2);

    for (wchar_t ch : text)
    {
        INPUT down = {};
        down.type = INPUT_KEYBOARD;
        down.ki.wVk = 0;
        down.ki.wScan = ch;
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(down);

        INPUT up = {};
        up.type = INPUT_KEYBOARD;
        up.ki.wVk = 0;
        up.ki.wScan = ch;
        up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs.push_back(up);
    }

    SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
}

// ===== 搜尋候選字 =====
void SearchCandidates()
{
    candidates.clear();
    currentPage = 0;
    selectedIndex = 0;

    if (strokeCode.empty())
        return;

    int inputLen = (int)strokeCode.length();
    std::vector<DictEntry> matchedEntries; // 暫存符合條件的項目

    for (auto& entry : dictionary)
    {
        if ((int)entry.code.length() < inputLen)
            continue;

        bool match = true;
        for (int i = 0; i < inputLen; i++)
        {
            if (strokeCode[i] == L'*') continue;
            if (strokeCode[i] != entry.code[i]) { match = false; break; }
        }

        if (match)
            matchedEntries.push_back(entry);
    }

    // ── 根據頻率排序 (由小到大) ──
    std::sort(matchedEntries.begin(), matchedEntries.end(),
        [](const DictEntry& a, const DictEntry& b) {
            return a.frequency < b.frequency;
        });

    // 將排序後的字存入 candidates
    for (auto& e : matchedEntries)
    {
        candidates.push_back(e.word);
    }
}

// ===== 上字 =====
void CommitText(const std::wstring& text)
{
    committedText += text;

    SendTextToFocus(text);

    strokeBuffer.clear();
    strokeCode.clear();
    candidates.clear();
    currentPage = 0;
    selectedIndex = 0;
}

// ===== 刪除已上字（本地 buffer） =====
void DeleteLastCommitted()
{
    if (!committedText.empty())
        committedText.pop_back();
}

// ===== 載入字庫 =====
void LoadDictionary()
{
    // 1. 尋找資源 (IDR_TEXTDATA1 是你在資源檔中匯入的 ID)
    HRSRC hRes = FindResource(NULL, MAKEINTRESOURCE(IDR_TXTDATA1), L"TXTDATA");
    if (!hRes) return;

    // 2. 載入並鎖定資源以獲取記憶體指標
    HGLOBAL hData = LoadResource(NULL, hRes);
    void* pData = LockResource(hData);
    DWORD size = SizeofResource(NULL, hRes);

    if (pData)
    {
        // 3. 將記憶體中的資料轉換為 stringstream 處理
        // 注意：這裡假設 txt 是 UTF-8 或 ANSI
        std::string content((char*)pData, size);
        std::istringstream file(content);
        std::string line;

        while (std::getline(file, line))
        {
            std::istringstream iss(line);
            std::string word, code, freq;
            if (iss >> word >> code >> freq)
            {
                // 編碼轉換處理 (維持你原本的邏輯)
                int ws = MultiByteToWideChar(CP_UTF8, 0, word.c_str(), -1, NULL, 0);
                std::wstring wWord(ws, 0);
                MultiByteToWideChar(CP_UTF8, 0, word.c_str(), -1, &wWord[0], ws);
                wWord.pop_back();

                int cs = MultiByteToWideChar(CP_UTF8, 0, code.c_str(), -1, NULL, 0);
                std::wstring wCode(cs, 0);
                MultiByteToWideChar(CP_UTF8, 0, code.c_str(), -1, &wCode[0], cs);
                wCode.pop_back();

                dictionary.push_back({ wWord, wCode, std::stoi(freq) });
            }
        }
    }
}

// ===== 更新游標位置 =====
void UpdateCaretPosition(HWND hwnd, HDC hdc)
{
    SIZE size;
    GetTextExtentPoint32W(
        hdc,
        strokeBuffer.c_str(),
        (int)strokeBuffer.size(),
        &size);

    SetCaretPos(100 + size.cx, 60);
}

// ===== 全域鍵盤 Hook =====
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && g_hwnd)
    {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
        {
            DWORD vk = p->vkCode;
            bool  ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool  shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

            // ── Ctrl+Shift+Space：顯示 / 隱藏視窗 ──
            if (ctrl && shift && vk == VK_SPACE)
            {
                g_visible = !g_visible;

                if (g_visible)
                {
                    ShowWindow(g_hwnd, SW_SHOW);
                    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE);
                }
                else
                {
                    ShowWindow(g_hwnd, SW_HIDE);
                }

                return 1;
            }

            // ── Ctrl+Space：開關輸入法（視窗顯示時才有效） ──
            if (ctrl && !shift && vk == VK_SPACE)
            {
                if (g_visible)
                {
                    g_imeActive = !g_imeActive;
                    PostMessage(g_hwnd, WM_IME_KEY, 0xFFFF, 0);
                }
                return 1;
            }

            // 輸入法未啟動，不攔截
            if (!g_imeActive)
                return CallNextHookEx(g_hHook, nCode, wParam, lParam);

            // ── 功能鍵 ──
            if (vk == VK_BACK || vk == VK_RETURN ||
                vk == VK_NEXT || vk == VK_PRIOR ||
                vk == VK_UP || vk == VK_DOWN || vk == VK_SPACE)
            {
                bool hasInput = !strokeCode.empty() || !candidates.empty();

                if (hasInput || vk == VK_BACK)
                {
                    PostMessage(g_hwnd, WM_IME_KEY, vk, 0);

                    if (hasInput)
                        return 1;
                }

                return CallNextHookEx(g_hHook, nCode, wParam, lParam);
            }

            // ── 數字選字 ──
            if (!candidates.empty() && vk >= '0' && vk <= '9')
            {
                PostMessage(g_hwnd, WM_IME_KEY, vk, 0);
                return 1;
            }

            // ── 筆劃按鍵 ──
            if (vk == 'U' || vk == 'I' || vk == 'O' ||
                vk == 'J' || vk == 'K' || vk == 'L')
            {
                wchar_t ch = (wchar_t)towlower((wchar_t)vk);
                PostMessage(g_hwnd, WM_IME_CHAR, ch, 0);
                return 1;
            }
        }
    }

    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
}

// ===== 處理字元輸入 =====
void HandleChar(wchar_t ch, HWND hwnd)
{
    switch (ch)
    {
    case 'u': strokeCode += L"u"; strokeBuffer += L"一 "; break;
    case 'i': strokeCode += L"i"; strokeBuffer += L"丨 "; break;
    case 'o': strokeCode += L"o"; strokeBuffer += L"丿 "; break;
    case 'j': strokeCode += L"j"; strokeBuffer += L"丶 "; break;
    case 'k': strokeCode += L"k"; strokeBuffer += L"乛 "; break;
    case 'l': strokeCode += L"*"; strokeBuffer += L"* "; break;
    }

    SearchCandidates();
    InvalidateRect(hwnd, NULL, TRUE);
}

// ===== 處理虛擬鍵 =====
void HandleVKey(DWORD vk, HWND hwnd)
{
    bool changed = false;

    if (vk == 0xFFFF) // 重繪（切換輸入法開關）
    {
        InvalidateRect(hwnd, NULL, TRUE);
        return;
    }

    if (vk == VK_BACK)
    {
        if (!strokeCode.empty())
        {
            strokeCode.pop_back();

            if (strokeBuffer.length() >= 2)
                strokeBuffer.erase(strokeBuffer.length() - 2, 2);

            SearchCandidates();
            changed = true;
        }
        else
        {
            DeleteLastCommitted();
            changed = true;
        }
    }
    else if (vk == VK_RETURN || vk == VK_SPACE)
    {
        if (!candidates.empty())
        {
            CommitText(candidates[selectedIndex]);
            changed = true;
        }
    }
    else if (vk >= '0' && vk <= '9')
    {
        wchar_t ch = (wchar_t)vk;

        if (ch == '0')
        {
            int index = currentPage * pageSize;
            if (index < (int)candidates.size())
            {
                CommitText(candidates[index]);
                changed = true;
            }
        }
        else
        {
            int offset = 10 - (ch - '0');
            int index = currentPage * pageSize + offset;

            if (index < (int)candidates.size())
            {
                CommitText(candidates[index]);
                changed = true;
            }
        }
    }
    else if (vk == VK_NEXT)
    {
        if ((currentPage + 1) * pageSize < (int)candidates.size())
        {
            currentPage++;
            selectedIndex = currentPage * pageSize;
            changed = true;
        }
    }
    else if (vk == VK_PRIOR)
    {
        if (currentPage > 0)
        {
            currentPage--;
            selectedIndex = currentPage * pageSize;
            changed = true;
        }
    }
    else if (vk == VK_DOWN && !candidates.empty())
    {
        selectedIndex++;
        if (selectedIndex >= (int)candidates.size())
            selectedIndex = 0;
        currentPage = selectedIndex / pageSize;
        changed = true;
    }
    else if (vk == VK_UP && !candidates.empty())
    {
        selectedIndex--;
        if (selectedIndex < 0)
            selectedIndex = (int)candidates.size() - 1;
        currentPage = selectedIndex / pageSize;
        changed = true;
    }

    if (changed)
        InvalidateRect(hwnd, NULL, TRUE);
}

// ===== 視窗程序 =====
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_SETFOCUS:
        CreateCaret(hwnd, NULL, 2, 24);
        ShowCaret(hwnd);
        break;

    case WM_KILLFOCUS:
        DestroyCaret();
        break;

    case WM_IME_KEY:
        HandleVKey((DWORD)wParam, hwnd);
        break;

    case WM_IME_CHAR:
        HandleChar((wchar_t)wParam, hwnd);
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        HFONT hFont = CreateFontW(
            18, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"微軟正黑體");

        HFONT oldFont = (HFONT)SelectObject(hdc, hFont);
        SetBkMode(hdc, TRANSPARENT);

        RECT rect; 
        GetClientRect(hwnd, &rect);

        // ── 右上角：狀態 ──
        std::wstring status =
            (g_imeActive ? L"[輸入法：開]  " : L"[輸入法：關]  ") +
            std::wstring(L"字庫：") +
            std::to_wstring(dictionary.size()) +
            L"  |  Ctrl+Space=開關  Ctrl+Shift+Space=顯示/隱藏";

        SetTextColor(hdc, g_imeActive ? RGB(0, 128, 0) : RGB(160, 160, 160));
        RECT statusRect = rect;
        statusRect.top = 8;
        statusRect.right -= 10;
        DrawTextW(hdc, status.c_str(), -1, &statusRect, DT_RIGHT | DT_SINGLELINE);

        SetTextColor(hdc, RGB(0, 0, 0));

        // ── 已上字 ──
        TextOutW(hdc, 20, 20,
            committedText.c_str(),
            (int)committedText.size());

        // ── 筆劃輸入列 ──
        TextOutW(hdc, 20, 60, L"筆劃：", 3);
        TextOutW(hdc, 100, 60,
            strokeBuffer.c_str(),
            (int)strokeBuffer.size());

        UpdateCaretPosition(hwnd, hdc);

        // ── 候選字 ──
        int y = 100;
        TextOutW(hdc, 20, y, L"候選字：", 4);
        y += 40;

        int startX = 350;  // 表格起始 X 座標 (避開左側的筆劃列)
        int startY = 60;   // 表格起始 Y 座標
        int cellW = 70;   // 每個格子的寬度
        int cellH = 40;    // 每個格子的高度
        int cols = 3;      // 設定為 2 欄

        int start = currentPage * pageSize;
        int end = min(start + pageSize, (int)candidates.size());

        for (int i = start; i < end; i++)
        {
            int localIdx = i - start; // 在當前頁面的編號 (0~9)
            int r = localIdx / cols;  // 計算列
            int c = localIdx % cols;  // 計算欄

            // 計算格子的位置
            int x = startX + (c * cellW);
            int y = startY + (r * cellH);

            // 處理數字鍵顯示 (0, 9, 8... 邏輯)
            int num = (localIdx == 0) ? 0 : (10 - localIdx);
            std::wstring item = std::to_wstring(num) + L"." + candidates[i];

            RECT cellRect = { x, y, x + cellW - 5, y + cellH - 5 };

            if (i == selectedIndex)
            {
                // 被選中的藍色背景
                HBRUSH brush = CreateSolidBrush(RGB(0, 120, 215));
                FillRect(hdc, &cellRect, brush);
                DeleteObject(brush);
                SetTextColor(hdc, RGB(255, 255, 255));
            }
            else
            {
                // 未選中的灰色邊框背景 (選配，增加表格感)
                SetTextColor(hdc, RGB(0, 0, 0));
            }

            // 繪製文字 (置中於格子)
            DrawTextW(hdc, item.c_str(), -1, &cellRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        // ── 底部說明 ──
        HFONT hSmall = CreateFontW(
            16, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"微軟正黑體");
        SelectObject(hdc, hSmall);
        SetTextColor(hdc, RGB(120, 120, 120));

        std::wstring hint =
            L"u=一  i=丨  o=丿  j=丶  k=乛  l=*(萬用)  "
            L"Enter=選中字  1~9/0=選字  PgDn/PgUp=翻頁  ↑↓=移動";

        RECT hintRect = rect;
        hintRect.top = rect.bottom - 30;
        hintRect.left = 10;
        hintRect.right -= 10;
        DrawTextW(hdc, hint.c_str(), -1, &hintRect, DT_LEFT | DT_SINGLELINE);

        SelectObject(hdc, oldFont);
        DeleteObject(hFont);
        DeleteObject(hSmall);

        EndPaint(hwnd, &ps);
        break;
    }

    case WM_DESTROY:
        if (g_hHook)
        {
            UnhookWindowsHookEx(g_hHook);
            g_hHook = NULL;
        }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    return 0;
}

// ===== 主程式 =====
int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int nCmdShow)
{
    LoadDictionary();

    const wchar_t CLASS_NAME[] = L"StrokeIME";

    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        CLASS_NAME,
        L"簡易筆劃輸入法",
        WS_OVERLAPPEDWINDOW,
        690, 0,
        600, 190, 
        NULL, NULL,
        hInstance,
        NULL);

    g_hwnd = hwnd;

    // 安裝全域低階鍵盤 Hook
    g_hHook = SetWindowsHookEx(
        WH_KEYBOARD_LL,
        LowLevelKeyboardProc,
        hInstance,
        0);

    if (!g_hHook)
        MessageBoxW(NULL, L"Hook 安裝失敗，請以系統管理員身份執行", L"錯誤", MB_OK);

    // 預設隱藏，按 Ctrl+Shift+Space 才顯示
    ShowWindow(hwnd, SW_HIDE);

    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}