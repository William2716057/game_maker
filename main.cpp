// level planner
//
// MSYS2 UCRT64:  g++ -std=c++17 -O2 level_planner.cpp -o level_planner.exe -lgdi32 -luser32
// MSVC:          cl /EHsc /std:c++17 level_planner.cpp
//
// Save writes grid_matrix.txt, prints the matrix to stdout, and rewrites the
// worldMap initializer (plus MAP_HEIGHT / MAP_WIDTH) in game.cpp.
// Keep it a console build (no -mwindows) if you want the stdout output.

#include <windows.h>
#include <windowsx.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

// ---------------------------------------------------------------- constants

// Starting grid size (hard-coded). Still adjustable with the +/- buttons.
constexpr int START_COLS = 39;   // matches MAP_WIDTH in game.cpp
constexpr int START_ROWS = 56;   // matches MAP_HEIGHT in game.cpp

// Path to the game source, relative to where you run the exe.
constexpr const char* GAME_CPP_PATH = "game.cpp";

constexpr int CELL_SIZE    = 16;   // small enough for a 56-row grid to fit on screen
constexpr int TOOLBAR_H    = 40;
constexpr int PALETTE_W    = 130;
constexpr int PALETTE_TOP  = 28;   // space for the "Color" label
constexpr int SWATCH_H     = 30;
constexpr int SWATCH_GAP   = 4;
constexpr int MIN_CLIENT_W = 480;  // keeps the toolbar visible on tiny grids

struct PaletteEntry {
    const char* name;
    COLORREF    color;
    COLORREF    textColor;
};

static const PaletteEntry PALETTE[] = {
    {"Empty",  RGB(255, 255, 255), RGB(0, 0, 0)},
    {"Wall",   RGB(0, 0, 0),       RGB(255, 255, 255)},
    {"Red",    RGB(255, 0, 0),     RGB(0, 0, 0)},
    {"Green",  RGB(50, 205, 50),   RGB(0, 0, 0)},
    {"Blue",   RGB(65, 105, 225),  RGB(255, 255, 255)},
    {"Orange", RGB(255, 165, 0),   RGB(0, 0, 0)},
    {"Purple", RGB(128, 0, 128),   RGB(255, 255, 255)},
    {"Cyan",   RGB(0, 255, 255),   RGB(0, 0, 0)},
};
constexpr int PALETTE_COUNT = sizeof(PALETTE) / sizeof(PALETTE[0]);

// Image tiles. Paths are relative to the folder you run the exe from.
// Must be .bmp files (plain Win32 can't load PNG/JPG without GDI+).
struct ImageEntry {
    const char* name;
    const char* path;
};

static const ImageEntry IMAGES[] = {
    {"Tile 1", "tiles/tile1.bmp"},
    {"Tile 2", "tiles/tile2.bmp"},
    {"Tile 3", "tiles/tile3.bmp"},
};
constexpr int IMAGE_COUNT = sizeof(IMAGES) / sizeof(IMAGES[0]);

// Cell values: 0..PALETTE_COUNT-1 = colours, PALETTE_COUNT.. = image tiles.
constexpr int TOTAL_ENTRIES = PALETTE_COUNT + IMAGE_COUNT;

enum ControlId {
    ID_COLS_DEC = 101,
    ID_COLS_INC,
    ID_ROWS_DEC,
    ID_ROWS_INC,
    ID_SAVE
};

// ---------------------------------------------------------------- app state

struct App {
    int cols = 0;
    int rows = 0;
    int selectedValue = 1;
    std::vector<std::vector<int>> filled;   // [row][col]
    HBRUSH brushes[PALETTE_COUNT] = {};
    HBRUSH missingBrush = nullptr;          // shown when an image fails to load
    HPEN   gridPen = nullptr;
    HBITMAP bitmaps[IMAGE_COUNT] = {};
    int bmpW[IMAGE_COUNT] = {};
    int bmpH[IMAGE_COUNT] = {};
};

static App g;

// ---------------------------------------------------------------- helpers

static void UpdateTitle(HWND hwnd) {
    std::string t = "Level Planner (" + std::to_string(g.cols) + " x " +
                    std::to_string(g.rows) + ")";
    SetWindowTextA(hwnd, t.c_str());
}

static int PaletteHeight() {
    return PALETTE_TOP + TOTAL_ENTRIES * (SWATCH_H + SWATCH_GAP) + 8;
}

// Resize the window so its client area fits the current grid.
static void FitWindowToGrid(HWND hwnd) {
    int clientW = PALETTE_W + g.cols * CELL_SIZE;
    if (clientW < MIN_CLIENT_W) clientW = MIN_CLIENT_W;
    int bodyH = g.rows * CELL_SIZE;
    if (bodyH < PaletteHeight()) bodyH = PaletteHeight();
    int clientH = TOOLBAR_H + bodyH;

    RECT r = {0, 0, clientW, clientH};
    DWORD style   = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
    DWORD exStyle = (DWORD)GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&r, style, FALSE, exStyle);

    SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateTitle(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
}

static RECT SwatchRect(int i) {
    RECT r;
    r.left   = 8;
    r.right  = PALETTE_W - 8;
    r.top    = TOOLBAR_H + PALETTE_TOP + i * (SWATCH_H + SWATCH_GAP);
    r.bottom = r.top + SWATCH_H;
    return r;
}

static RECT CellRect(int row, int col) {
    RECT r;
    r.left   = PALETTE_W + col * CELL_SIZE;
    r.top    = TOOLBAR_H + row * CELL_SIZE;
    r.right  = r.left + CELL_SIZE;
    r.bottom = r.top + CELL_SIZE;
    return r;
}

static void SelectColor(HWND hwnd, int value) {
    if (value == g.selectedValue) return;
    int old = g.selectedValue;
    g.selectedValue = value;
    RECT a = SwatchRect(old), b = SwatchRect(value);
    InvalidateRect(hwnd, &a, FALSE);
    InvalidateRect(hwnd, &b, FALSE);
}

// Draw image `idx` scaled into `dst`. Falls back to a magenta block if the
// bitmap didn't load.
static void DrawImageInto(HDC hdc, HDC memDc, int idx, const RECT& dst) {
    int w = dst.right - dst.left;
    int h = dst.bottom - dst.top;
    if (!g.bitmaps[idx]) {
        FillRect(hdc, &dst, g.missingBrush);
        return;
    }
    HGDIOBJ old = SelectObject(memDc, g.bitmaps[idx]);
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, nullptr);
    StretchBlt(hdc, dst.left, dst.top, w, h,
               memDc, 0, 0, g.bmpW[idx], g.bmpH[idx], SRCCOPY);
    SelectObject(memDc, old);
}

// ---------------------------------------------------------------- painting

static void PaintCell(HWND hwnd, int px, int py, int value) {
    int col = px - PALETTE_W;
    int row = py - TOOLBAR_H;
    if (col < 0 || row < 0) return;
    col /= CELL_SIZE;
    row /= CELL_SIZE;
    if (row >= g.rows || col >= g.cols) return;

    if (g.filled[row][col] != value) {
        g.filled[row][col] = value;
        RECT r = CellRect(row, col);
        InvalidateRect(hwnd, &r, FALSE);
    }
}

static void OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    HDC memDc = CreateCompatibleDC(hdc);

    SetBkMode(hdc, TRANSPARENT);

    // Palette label
    SetTextColor(hdc, RGB(0, 0, 0));
    RECT lbl = {0, TOOLBAR_H + 4, PALETTE_W, TOOLBAR_H + PALETTE_TOP};
    DrawTextA(hdc, "Color", -1, &lbl, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // Palette swatches: colours first, then image tiles
    for (int i = 0; i < TOTAL_ENTRIES; ++i) {
        RECT r = SwatchRect(i);
        UINT edge = (i == g.selectedValue) ? EDGE_SUNKEN : EDGE_RAISED;

        if (i < PALETTE_COUNT) {
            FillRect(hdc, &r, g.brushes[i]);
            DrawEdge(hdc, &r, edge, BF_RECT);
            SetTextColor(hdc, PALETTE[i].textColor);
            DrawTextA(hdc, PALETTE[i].name, -1, &r,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            int k = i - PALETTE_COUNT;
            FillRect(hdc, &r, (HBRUSH)(COLOR_BTNFACE + 1));
            RECT thumb = {r.left + 2, r.top + 2, r.left + SWATCH_H - 2, r.bottom - 2};
            DrawImageInto(hdc, memDc, k, thumb);
            DrawEdge(hdc, &r, edge, BF_RECT);
            SetTextColor(hdc, RGB(0, 0, 0));
            RECT txt = {r.left + SWATCH_H + 2, r.top, r.right - 2, r.bottom};
            DrawTextA(hdc, IMAGES[k].name, -1, &txt,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // Grid (only cells that intersect the dirty rectangle)
    HPEN oldPen = (HPEN)SelectObject(hdc, g.gridPen);
    for (int row = 0; row < g.rows; ++row) {
        for (int col = 0; col < g.cols; ++col) {
            RECT cell = CellRect(row, col);
            RECT tmp;
            if (!IntersectRect(&tmp, &cell, &ps.rcPaint)) continue;

            int v = g.filled[row][col];
            HBRUSH base = g.brushes[v < PALETTE_COUNT ? v : 0];
            HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, base);
            Rectangle(hdc, cell.left, cell.top, cell.right + 1, cell.bottom + 1);
            SelectObject(hdc, oldBrush);

            if (v >= PALETTE_COUNT) {
                RECT inner = {cell.left + 1, cell.top + 1, cell.right, cell.bottom};
                DrawImageInto(hdc, memDc, v - PALETTE_COUNT, inner);
            }
        }
    }
    SelectObject(hdc, oldPen);

    DeleteDC(memDc);
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------- resizing

static void IncreaseCols(HWND hwnd) {
    for (auto& row : g.filled) row.push_back(0);
    ++g.cols;
    FitWindowToGrid(hwnd);
}

static void DecreaseCols(HWND hwnd) {
    if (g.cols <= 1) return;
    for (int r = 0; r < g.rows; ++r) {
        if (g.filled[r][g.cols - 1] != 0) {
            MessageBoxA(hwnd, "Can't decrease columns: the last column has filled squares.",
                        "Cannot resize", MB_OK | MB_ICONWARNING);
            return;
        }
    }
    for (auto& row : g.filled) row.pop_back();
    --g.cols;
    FitWindowToGrid(hwnd);
}

static void IncreaseRows(HWND hwnd) {
    g.filled.emplace_back(g.cols, 0);
    ++g.rows;
    FitWindowToGrid(hwnd);
}

static void DecreaseRows(HWND hwnd) {
    if (g.rows <= 1) return;
    for (int v : g.filled.back()) {
        if (v != 0) {
            MessageBoxA(hwnd, "Can't decrease rows: the last row has filled squares.",
                        "Cannot resize", MB_OK | MB_ICONWARNING);
            return;
        }
    }
    g.filled.pop_back();
    --g.rows;
    FitWindowToGrid(hwnd);
}

// ---------------------------------------------------------------- save

// Replace the number in:  const int NAME = <number>;
static bool SetConstInt(std::string& src, const char* name, int value) {
    std::regex re(std::string(R"(const\s+int\s+)") + name + R"(\s*=\s*(\d+))");
    std::smatch m;
    if (!std::regex_search(src, m, re)) return false;
    src.replace(m.position(1), m.length(1), std::to_string(value));
    return true;
}

// Updates MAP_HEIGHT / MAP_WIDTH and the worldMap initializer in game.cpp.
// A backup is written to game.cpp.bak first.
static bool UpdateGameCpp(std::string& err) {
    std::ifstream in(GAME_CPP_PATH, std::ios::binary);
    if (!in) { err = "Could not open game.cpp"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    in.close();

    {
        std::ofstream bak(std::string(GAME_CPP_PATH) + ".bak", std::ios::binary);
        bak << src;
    }

    if (!SetConstInt(src, "MAP_HEIGHT", g.rows) ||
        !SetConstInt(src, "MAP_WIDTH",  g.cols)) {
        err = "Could not find MAP_HEIGHT / MAP_WIDTH in game.cpp";
        return false;
    }

    // Find:  worldMap[...][...] =   then the matching { ... }
    std::regex decl(R"(worldMap\s*\[[^\]]*\]\s*\[[^\]]*\]\s*=)");
    std::smatch m;
    if (!std::regex_search(src, m, decl)) {
        err = "Could not find the worldMap declaration in game.cpp";
        return false;
    }
    size_t open = src.find('{', m.position() + m.length());
    if (open == std::string::npos) { err = "No '{' after worldMap"; return false; }

    size_t close = std::string::npos;
    int depth = 0;
    for (size_t i = open; i < src.size(); ++i) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}' && --depth == 0) { close = i; break; }
    }
    if (close == std::string::npos) { err = "No matching '}' for worldMap"; return false; }

    std::string body = "\n";
    for (const auto& row : g.filled) {
        body += "    {";
        for (size_t i = 0; i < row.size(); ++i) {
            if (i) body += ", ";
            body += std::to_string(row[i]);
        }
        body += "},\n";
    }
    src.replace(open + 1, close - open - 1, body);

    std::ofstream out(GAME_CPP_PATH, std::ios::binary);
    if (!out) { err = "Could not write game.cpp"; return false; }
    out << src;
    return true;
}

static void SaveMatrix(HWND hwnd) {
    std::string text = "[\n";
    for (const auto& row : g.filled) {
        text += "  [";
        for (size_t i = 0; i < row.size(); ++i) {
            if (i) text += ", ";
            text += std::to_string(row[i]);
        }
        text += "],\n";
    }
    text += "]";

    std::ofstream out("grid_matrix.txt");
    if (!out) {
        MessageBoxA(hwnd, "Could not open grid_matrix.txt for writing.",
                    "Error", MB_OK | MB_ICONERROR);
        return;
    }
    out << text << "\n";
    out.close();

    std::cout << text << std::endl;

    std::string err;
    if (UpdateGameCpp(err)) {
        MessageBoxA(hwnd,
                    "Saved grid_matrix.txt and updated worldMap in game.cpp\n(backup: game.cpp.bak).",
                    "Saved", MB_OK | MB_ICONINFORMATION);
    } else {
        std::string msg = "Saved grid_matrix.txt, but game.cpp was not updated:\n" + err;
        MessageBoxA(hwnd, msg.c_str(), "Saved (with warning)", MB_OK | MB_ICONWARNING);
    }
}

// ---------------------------------------------------------------- window proc

static void CreateToolbar(HWND hwnd) {
    HINSTANCE inst = GetModuleHandle(nullptr);
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    auto make = [&](const char* cls, const char* text, int px, int w, int id) {
        HWND h = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE,
                                 px, 8, w, 24, hwnd, (HMENU)(INT_PTR)id, inst, nullptr);
        SendMessage(h, WM_SETFONT, (WPARAM)font, TRUE);
    };

    int x = 8;
    make("STATIC", "Columns:", x, 56, 0);          x += 58;
    make("BUTTON", "-", x, 24, ID_COLS_DEC);       x += 26;
    make("BUTTON", "+", x, 24, ID_COLS_INC);       x += 40;
    make("STATIC", "Rows:", x, 40, 0);             x += 42;
    make("BUTTON", "-", x, 24, ID_ROWS_DEC);       x += 26;
    make("BUTTON", "+", x, 24, ID_ROWS_INC);       x += 40;
    make("BUTTON", "Save Matrix", x, 90, ID_SAVE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        CreateToolbar(hwnd);
        return 0;

    case WM_PAINT:
        OnPaint(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_COLS_DEC: DecreaseCols(hwnd); break;
        case ID_COLS_INC: IncreaseCols(hwnd); break;
        case ID_ROWS_DEC: DecreaseRows(hwnd); break;
        case ID_ROWS_INC: IncreaseRows(hwnd); break;
        case ID_SAVE:     SaveMatrix(hwnd);   break;
        }
        SetFocus(hwnd);
        return 0;

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        // Palette click?
        if (x < PALETTE_W && y >= TOOLBAR_H) {
            for (int i = 0; i < TOTAL_ENTRIES; ++i) {
                RECT r = SwatchRect(i);
                POINT p = {x, y};
                if (PtInRect(&r, p)) { SelectColor(hwnd, i); break; }
            }
            return 0;
        }
        SetCapture(hwnd);
        PaintCell(hwnd, x, y, g.selectedValue);
        return 0;
    }

    case WM_RBUTTONDOWN:
        SetCapture(hwnd);
        PaintCell(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 0);
        return 0;

    case WM_MOUSEMOVE:
        // Drag-paint / drag-erase
        if (wp & MK_LBUTTON)
            PaintCell(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), g.selectedValue);
        else if (wp & MK_RBUTTON)
            PaintCell(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 0);
        return 0;

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if (!(wp & (MK_LBUTTON | MK_RBUTTON))) ReleaseCapture();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- startup

static void LoadImages() {
    for (int i = 0; i < IMAGE_COUNT; ++i) {
        g.bitmaps[i] = (HBITMAP)LoadImageA(nullptr, IMAGES[i].path, IMAGE_BITMAP,
                                           0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION);
        if (g.bitmaps[i]) {
            BITMAP bm;
            GetObject(g.bitmaps[i], sizeof(bm), &bm);
            g.bmpW[i] = bm.bmWidth;
            g.bmpH[i] = bm.bmHeight;
        } else {
            std::cerr << "Could not load image: " << IMAGES[i].path << "\n";
        }
    }
}

int main() {
    g.cols = START_COLS;
    g.rows = START_ROWS;
    g.filled.assign(g.rows, std::vector<int>(g.cols, 0));

    for (int i = 0; i < PALETTE_COUNT; ++i)
        g.brushes[i] = CreateSolidBrush(PALETTE[i].color);
    g.missingBrush = CreateSolidBrush(RGB(255, 0, 255));
    g.gridPen = CreatePen(PS_SOLID, 1, RGB(128, 128, 128));
    LoadImages();

    HINSTANCE inst = GetModuleHandle(nullptr);

    WNDCLASSA wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "GridToggleWnd";
    RegisterClassA(&wc);

    // Fixed-size window (no resize border / maximize) - size follows the grid.
    DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    HWND hwnd = CreateWindowExA(0, "GridToggleWnd", "Level Planner", style,
                                CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                                nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    FitWindowToGrid(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessage(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }

    for (HBRUSH b : g.brushes) DeleteObject(b);
    DeleteObject(g.missingBrush);
    DeleteObject(g.gridPen);
    for (HBITMAP b : g.bitmaps) if (b) DeleteObject(b);
    return 0;
}
