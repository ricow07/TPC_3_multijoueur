// server_main.c — Serveur Agar.io 3D : fenêtre GUI affichant l'IP + les joueurs
#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#pragma comment(lib, "ws2_32.lib")

#include "protocol.h"

#define CLIENT_TIMEOUT_MS  8000
#define BROADCAST_HZ       60

#define WIN_W 540
#define WIN_H 520

/* ── Palette (assortie au client) ── */
#define BG_TOP      RGB(238, 244, 252)
#define BG_BOT      RGB(214, 226, 244)
#define HUD_BG      RGB(255, 255, 255)
#define HUD_BORDER  RGB(160, 188, 222)
#define HUD_TEXT    RGB( 55,  70, 100)
#define HUD_DIM     RGB(120, 140, 175)
#define ACCENT      RGB( 80, 130, 200)
#define IP_COLOR    RGB( 35,  90, 175)

static const COLORREF PLAYER_COLORS[MAX_PLAYERS] = {
    RGB(245, 110, 110),  RGB( 95, 165, 240),  RGB(120, 215, 145),
    RGB(245, 200,  90),  RGB(190, 130, 235),  RGB( 95, 205, 215),
    RGB(245, 145, 200),  RGB(180, 220, 110),  RGB(245, 165,  95),
    RGB(110, 200, 240),  RGB(235, 220, 110),  RGB(160, 115, 230),
    RGB(110, 215, 175),  RGB(225, 130, 185),  RGB(155, 210, 110),
    RGB(110, 145, 225)
};

/* ── État jeu ── */
typedef struct {
    int                 active;
    float               x, y, z;
    float               radius;
    struct sockaddr_in  addr;
    int                 has_addr;
    DWORD               last_seen;
    DWORD               respawn_time;  /* 0 = vivant, sinon = moment de mort */
} ServerPlayer;

static ServerPlayer    players[MAX_PLAYERS];
static FoodState       food[MAX_FOOD];
static int             food_count = MAX_FOOD;
static CRITICAL_SECTION g_cs;
static SOCKET          sockfd;
static volatile int    g_running = 1;

/* IPs locales du serveur (pour affichage) */
static char  g_local_ips[8][32];
static int   g_local_ips_count = 0;

static HWND  g_hwnd;

/* ════════════════════════════════════════
   Logique de jeu
════════════════════════════════════════ */
static float frand_in(float a, float b) {
    return a + ((float)rand() / (float)RAND_MAX) * (b - a);
}
static void respawn_food(int i) {
    food[i].x     = frand_in(20.0f, MAP_SIZE - 20.0f);
    food[i].y     = frand_in(20.0f, MAP_SIZE - 20.0f);
    food[i].z     = frand_in(20.0f, MAP_SIZE - 20.0f);
    food[i].color = rand() % 8;
}
static void init_food(void) {
    for (int i = 0; i < MAX_FOOD; i++) respawn_food(i);
}
static void clamp_to_cube(float *x, float *y, float *z, float r) {
    if (*x < r)              *x = r;
    if (*x > MAP_SIZE - r)   *x = MAP_SIZE - r;
    if (*y < r)              *y = r;
    if (*y > MAP_SIZE - r)   *y = MAP_SIZE - r;
    if (*z < r)              *z = r;
    if (*z > MAP_SIZE - r)   *z = MAP_SIZE - r;
}
static int sockaddr_eq(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr &&
           a->sin_port        == b->sin_port;
}

static int find_or_assign_slot(const struct sockaddr_in *addr) {
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].has_addr && sockaddr_eq(&players[i].addr, addr)) {
            LeaveCriticalSection(&g_cs);
            return i;
        }
    }
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!players[i].has_addr) {
            memset(&players[i], 0, sizeof(players[i]));
            players[i].addr      = *addr;
            players[i].has_addr  = 1;
            players[i].last_seen = GetTickCount();
            LeaveCriticalSection(&g_cs);
            return i;
        }
    }
    LeaveCriticalSection(&g_cs);
    return -1;
}

static void timeout_inactive(void) {
    DWORD now = GetTickCount();
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].has_addr && (now - players[i].last_seen) > CLIENT_TIMEOUT_MS) {
            memset(&players[i], 0, sizeof(players[i]));
        }
    }
    LeaveCriticalSection(&g_cs);
}

static void check_food_collision(ServerPlayer *p) {
    /* Skip collision check for dead players (waiting to respawn) */
    if (p->respawn_time != 0) return;
    float r2 = (p->radius + 6.0f) * (p->radius + 6.0f);
    for (int i = 0; i < food_count; i++) {
        float dx = p->x - food[i].x;
        float dy = p->y - food[i].y;
        float dz = p->z - food[i].z;
        if (dx*dx + dy*dy + dz*dz < r2) {
            float vol = (4.0f/3.0f) * 3.14159f * p->radius * p->radius * p->radius;
            vol += 350.0f;
            p->radius = cbrtf(vol / ((4.0f/3.0f) * 3.14159f));
            respawn_food(i);
        }
    }
}

static void check_player_collision(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!players[i].active || players[i].respawn_time != 0) continue;
        for (int j = 0; j < MAX_PLAYERS; j++) {
            if (!players[j].active || players[j].respawn_time != 0 || i == j) continue;
            float dx = players[i].x - players[j].x;
            float dy = players[i].y - players[j].y;
            float dz = players[i].z - players[j].z;
            float d  = sqrtf(dx*dx + dy*dy + dz*dz);
            if (players[i].radius > players[j].radius * 1.15f &&
                d < players[i].radius * 0.85f) {
                float ve = (4.0f/3.0f)*3.14159f*players[i].radius*players[i].radius*players[i].radius;
                float vp = (4.0f/3.0f)*3.14159f*players[j].radius*players[j].radius*players[j].radius;
                ve += vp;
                players[i].radius = cbrtf(ve / ((4.0f/3.0f)*3.14159f));
                /* Marquer le joueur mangé comme mort, il respawnera dans 3 secondes */
                players[j].respawn_time = GetTickCount();
            }
        }
    }
}

static void check_respawn_timers(void) {
    DWORD now = GetTickCount();
    const DWORD respawn_delay_ms = 3000;  /* 3 secondes */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!players[i].active || players[i].respawn_time == 0) continue;
        if (now - players[i].respawn_time >= respawn_delay_ms) {
            /* Respawner le joueur */
            players[i].radius = 25.0f;
            players[i].x = frand_in(100.0f, MAP_SIZE - 100.0f);
            players[i].y = frand_in(100.0f, MAP_SIZE - 100.0f);
            players[i].z = frand_in(100.0f, MAP_SIZE - 100.0f);
            players[i].respawn_time = 0;  /* Marqué comme vivant */
        }
    }
}

static void build_world_packet(WorldStatePacket *pkt) {
    pkt->player_count = MAX_PLAYERS;
    pkt->food_count   = food_count;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        pkt->players[i].id     = i;
        pkt->players[i].x      = players[i].x;
        pkt->players[i].y      = players[i].y;
        pkt->players[i].z      = players[i].z;
        pkt->players[i].radius = players[i].active ? players[i].radius : 0.0f;
    }
    for (int i = 0; i < food_count; i++) pkt->food[i] = food[i];
}

static void broadcast_world_state(void) {
    static WorldStatePacket pkt;
    EnterCriticalSection(&g_cs);
    build_world_packet(&pkt);
    struct sockaddr_in addrs[MAX_PLAYERS];
    int    flags[MAX_PLAYERS];
    for (int i = 0; i < MAX_PLAYERS; i++) {
        flags[i] = players[i].has_addr;
        addrs[i] = players[i].addr;
    }
    LeaveCriticalSection(&g_cs);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!flags[i]) continue;
        sendto(sockfd, (char*)&pkt, sizeof(pkt), 0,
               (struct sockaddr*)&addrs[i], sizeof(addrs[i]));
    }
}

/* ════════════════════════════════════════
   Thread serveur (boucle réseau)
════════════════════════════════════════ */
static DWORD WINAPI ServerThread(LPVOID p) {
    (void)p;
    DWORD last_broadcast     = GetTickCount();
    DWORD last_timeout_check = GetTickCount();
    const DWORD broadcast_period = 1000 / BROADCAST_HZ;

    while (g_running) {
        ClientInputPacket input;
        struct sockaddr_in from;
        int from_len = sizeof(from);
        int n = recvfrom(sockfd, (char*)&input, sizeof(input), 0,
                         (struct sockaddr*)&from, &from_len);

        if (n == sizeof(input)) {
            if (input.id == DISCOVERY_ID) {
                int slot = find_or_assign_slot(&from);
                DiscoveryReplyPacket rep;
                rep.magic       = SERVER_MAGIC;
                rep.assigned_id = slot;
                sendto(sockfd, (char*)&rep, sizeof(rep), 0,
                       (struct sockaddr*)&from, sizeof(from));
            } else if (input.id >= 0 && input.id < MAX_PLAYERS) {
                int id = input.id;
                EnterCriticalSection(&g_cs);
                players[id].addr      = from;
                players[id].has_addr  = 1;
                players[id].last_seen = GetTickCount();

                if (!players[id].active) {
                    players[id].active = 1;
                    players[id].x = frand_in(200.0f, MAP_SIZE - 200.0f);
                    players[id].y = frand_in(200.0f, MAP_SIZE - 200.0f);
                    players[id].z = frand_in(200.0f, MAP_SIZE - 200.0f);
                    players[id].radius = 25.0f;
                    players[id].respawn_time = 0;
                }

                float base_speed = 4.0f;
                float speed = base_speed * (25.0f / (players[id].radius + 5.0f) + 0.4f);
                players[id].x += input.dir_x * speed;
                players[id].y += input.dir_y * speed;
                players[id].z += input.dir_z * speed;
                clamp_to_cube(&players[id].x, &players[id].y, &players[id].z, players[id].radius);
                check_food_collision(&players[id]);
                check_player_collision();
                check_respawn_timers();
                LeaveCriticalSection(&g_cs);
            }
        }

        DWORD now = GetTickCount();
        if (now - last_broadcast >= broadcast_period) {
            broadcast_world_state();
            last_broadcast = now;
        }
        if (now - last_timeout_check >= 1000) {
            timeout_inactive();
            last_timeout_check = now;
        }
    }
    return 0;
}

/* ════════════════════════════════════════
   Détection des IPs locales
════════════════════════════════════════ */
static void detect_local_ips(void) {
    g_local_ips_count = 0;
    char host[256];
    if (gethostname(host, sizeof(host)) != 0) return;
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) return;
    for (struct addrinfo *p = res; p && g_local_ips_count < 8; p = p->ai_next) {
        struct sockaddr_in *sa = (struct sockaddr_in*)p->ai_addr;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
        if (strcmp(ip, "127.0.0.1") == 0) continue;
        /* dédoublonne */
        int dup = 0;
        for (int k = 0; k < g_local_ips_count; k++)
            if (strcmp(g_local_ips[k], ip) == 0) { dup = 1; break; }
        if (dup) continue;
        strncpy(g_local_ips[g_local_ips_count], ip, 31);
        g_local_ips[g_local_ips_count][31] = 0;
        g_local_ips_count++;
    }
    freeaddrinfo(res);
}

/* ════════════════════════════════════════
   UI : fenêtre serveur
════════════════════════════════════════ */
static HFONT make_font(int size, int bold, const char *face) {
    return CreateFont(size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                      0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                      DEFAULT_PITCH | FF_SWISS, face);
}

static void draw_server_ui(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    HDC     memDC  = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP oldBmp = SelectObject(memDC, memBmp);

    /* fond dégradé clair */
    {
        const int bands = 50;
        int tr = GetRValue(BG_TOP), tg = GetGValue(BG_TOP), tb = GetBValue(BG_TOP);
        int br = GetRValue(BG_BOT), bg = GetGValue(BG_BOT), bb = GetBValue(BG_BOT);
        for (int i = 0; i < bands; i++) {
            float t = (float)i / (bands - 1);
            int r = tr + (int)((br - tr) * t);
            int g = tg + (int)((bg - tg) * t);
            int b = tb + (int)((bb - tb) * t);
            RECT bRc = {0, (H * i) / bands, W, (H * (i+1)) / bands};
            HBRUSH bbr = CreateSolidBrush(RGB(r,g,b));
            FillRect(memDC, &bRc, bbr);
            DeleteObject(bbr);
        }
    }

    SetBkMode(memDC, TRANSPARENT);

    int y = 24;

    /* titre */
    {
        HFONT f = make_font(28, 1, "Segoe UI");
        HFONT of = SelectObject(memDC, f);
        SetTextColor(memDC, ACCENT);
        RECT t = {0, y, W, y + 38};
        DrawText(memDC, "AGAR 3D — SERVEUR", -1, &t, DT_CENTER | DT_SINGLELINE);
        SelectObject(memDC, of);
        DeleteObject(f);
        y += 50;
    }

    /* sous-titre */
    {
        HFONT f = make_font(15, 0, "Segoe UI");
        HFONT of = SelectObject(memDC, f);
        SetTextColor(memDC, HUD_DIM);
        RECT t = {0, y, W, y + 22};
        DrawText(memDC, "Communique cette adresse aux autres joueurs :", -1, &t,
                 DT_CENTER | DT_SINGLELINE);
        SelectObject(memDC, of);
        DeleteObject(f);
        y += 24;
    }

    /* IPs locales (cadre blanc) */
    int box_top = y + 8;
    int n_ips = g_local_ips_count > 0 ? g_local_ips_count : 1;
    int box_h = 20 + n_ips * 32 + 8;
    {
        RECT box = {30, box_top, W - 30, box_top + box_h};
        HBRUSH wb = CreateSolidBrush(HUD_BG);
        FillRect(memDC, &box, wb);
        DeleteObject(wb);
        HPEN pen = CreatePen(PS_SOLID, 1, HUD_BORDER);
        HPEN op = SelectObject(memDC, pen);
        HBRUSH nb = GetStockObject(NULL_BRUSH);
        HBRUSH ob = SelectObject(memDC, nb);
        Rectangle(memDC, box.left, box.top, box.right, box.bottom);
        SelectObject(memDC, ob); SelectObject(memDC, op);
        DeleteObject(pen);

        HFONT f = make_font(28, 1, "Consolas");
        HFONT of = SelectObject(memDC, f);
        SetTextColor(memDC, IP_COLOR);
        if (g_local_ips_count == 0) {
            RECT t = {box.left, box.top + 14, box.right, box.top + 14 + 32};
            DrawText(memDC, "(aucune adresse detectee)", -1, &t,
                     DT_CENTER | DT_SINGLELINE);
        } else {
            for (int i = 0; i < g_local_ips_count; i++) {
                RECT t = {box.left, box.top + 14 + i*32, box.right, box.top + 14 + (i+1)*32};
                DrawText(memDC, g_local_ips[i], -1, &t, DT_CENTER | DT_SINGLELINE);
            }
        }
        SelectObject(memDC, of);
        DeleteObject(f);
    }
    y = box_top + box_h + 8;

    /* port */
    {
        HFONT f = make_font(14, 0, "Segoe UI");
        HFONT of = SelectObject(memDC, f);
        SetTextColor(memDC, HUD_TEXT);
        char port_line[64];
        snprintf(port_line, sizeof(port_line), "Port UDP : %d", SERVER_PORT);
        RECT t = {0, y, W, y + 22};
        DrawText(memDC, port_line, -1, &t, DT_CENTER | DT_SINGLELINE);
        SelectObject(memDC, of);
        DeleteObject(f);
        y += 28;
    }

    /* séparateur */
    {
        HPEN pen = CreatePen(PS_SOLID, 1, HUD_BORDER);
        HPEN op = SelectObject(memDC, pen);
        MoveToEx(memDC, 30, y, NULL); LineTo(memDC, W - 30, y);
        SelectObject(memDC, op);
        DeleteObject(pen);
        y += 12;
    }

    /* joueurs connectés */
    EnterCriticalSection(&g_cs);
    int n_connected = 0;
    int order[MAX_PLAYERS];
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].has_addr) order[n_connected++] = i;
    }

    char header[64];
    snprintf(header, sizeof(header), "Joueurs connectes : %d / %d", n_connected, MAX_PLAYERS);

    HFONT f = make_font(15, 1, "Segoe UI");
    HFONT of = SelectObject(memDC, f);
    SetTextColor(memDC, HUD_TEXT);
    TextOut(memDC, 30, y, header, (int)strlen(header));
    SelectObject(memDC, of);
    DeleteObject(f);
    y += 26;

    if (n_connected == 0) {
        HFONT f2 = make_font(13, 0, "Segoe UI");
        HFONT of2 = SelectObject(memDC, f2);
        SetTextColor(memDC, HUD_DIM);
        TextOut(memDC, 40, y, "En attente...", 13);
        SelectObject(memDC, of2);
        DeleteObject(f2);
    } else {
        HFONT mono = make_font(13, 0, "Consolas");
        HFONT of2 = SelectObject(memDC, mono);
        for (int k = 0; k < n_connected && k < 10; k++) {
            int i = order[k];
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &players[i].addr.sin_addr, ip, sizeof(ip));
            char line[128];
            snprintf(line, sizeof(line), "P%-2d   taille %5.0f   %s:%u",
                     i, players[i].radius, ip, ntohs(players[i].addr.sin_port));
            SetTextColor(memDC, PLAYER_COLORS[i % MAX_PLAYERS]);
            /* pastille couleur */
            int dot_r = 6;
            int dot_cx = 40, dot_cy = y + 9;
            HBRUSH db = CreateSolidBrush(PLAYER_COLORS[i % MAX_PLAYERS]);
            HPEN   dp = CreatePen(PS_NULL, 0, 0);
            HBRUSH oldB = SelectObject(memDC, db);
            HPEN   oldP = SelectObject(memDC, dp);
            Ellipse(memDC, dot_cx - dot_r, dot_cy - dot_r, dot_cx + dot_r, dot_cy + dot_r);
            SelectObject(memDC, oldB); SelectObject(memDC, oldP);
            DeleteObject(db); DeleteObject(dp);

            SetTextColor(memDC, HUD_TEXT);
            TextOut(memDC, 56, y, line, (int)strlen(line));
            y += 22;
        }
        SelectObject(memDC, of2);
        DeleteObject(mono);
    }
    LeaveCriticalSection(&g_cs);

    BitBlt(hdc, 0, 0, W, H, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK ServerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, 1, 500, NULL);
        return 0;
    case WM_TIMER:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        draw_server_ui(hwnd);
        return 0;
    case WM_CLOSE:
    case WM_DESTROY:
        g_running = 0;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int nShow) {
    (void)hPrev; (void)cmd;

    srand((unsigned)time(NULL));
    memset(players, 0, sizeof(players));
    init_food();
    InitializeCriticalSection(&g_cs);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd == INVALID_SOCKET) {
        MessageBox(NULL, "Impossible de creer le socket.", "Erreur", MB_ICONERROR);
        return 1;
    }
    DWORD to = 5;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(SERVER_PORT);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (bind(sockfd, (struct sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) {
        char m[160];
        snprintf(m, sizeof(m),
                 "Impossible d'ouvrir le port UDP %d.\n"
                 "Un autre serveur tourne deja ?", SERVER_PORT);
        MessageBox(NULL, m, "Erreur", MB_ICONERROR);
        closesocket(sockfd); WSACleanup();
        return 1;
    }

    detect_local_ips();

    WNDCLASS wc = {0};
    wc.lpfnWndProc   = ServerWndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = "Agar3DServer";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    RegisterClass(&wc);

    /* fenêtre centrée */
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    g_hwnd = CreateWindow("Agar3DServer", "Agar 3D — Serveur",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (sw - WIN_W) / 2, (sh - WIN_H) / 2, WIN_W, WIN_H,
        NULL, NULL, hInst, NULL);

    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    HANDLE th = CreateThread(NULL, 0, ServerThread, NULL, 0, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running = 0;
    closesocket(sockfd);
    WaitForSingleObject(th, 1000);
    CloseHandle(th);
    WSACleanup();
    DeleteCriticalSection(&g_cs);
    return 0;
}
