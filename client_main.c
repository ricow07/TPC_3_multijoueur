// client_main.c — Agar.io 3D, fenêtre de connexion + jeu 3D
#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "protocol.h"

#pragma comment(lib, "ws2_32.lib")

#define WINDOW_W 1100
#define WINDOW_H 750

#define SS 2

#define PI 3.14159265358979f
#define DEG2RAD(d) ((d) * PI / 180.0f)

#define NEAR_PLANE       1.0f
#define FAR_FOG          1800.0f

#define CAM_DIST_DEFAULT 320.0f
#define CAM_DIST_MIN     120.0f
#define CAM_DIST_MAX     900.0f

/* ── état réseau ── */
static SOCKET             g_sock;
static struct sockaddr_in g_server_addr;
static int                g_player_id = 0;
static volatile int       g_connected = 0;

/* ── monde partagé ── */
static WorldStatePacket g_world;
static CRITICAL_SECTION g_cs;
static HWND             g_hwnd;

/* ── caméra ── */
static float g_yaw   = PI;
static float g_pitch = -0.20f;
static float g_cam_dist = CAM_DIST_DEFAULT;

/* ── souris ── */
static int   g_mouse_captured = 1;
static POINT g_screen_center  = {0, 0};

/* ── FPS ── */
static int   g_fps_frames = 0;
static DWORD g_fps_last_tick = 0;
static float g_fps_value = 0.0f;

/* ════════════════════════════════════════
   Palette
════════════════════════════════════════ */
static const COLORREF PLAYER_COLORS[MAX_PLAYERS] = {
    RGB(245, 110, 110),  RGB( 95, 165, 240),  RGB(120, 215, 145),
    RGB(245, 200,  90),  RGB(190, 130, 235),  RGB( 95, 205, 215),
    RGB(245, 145, 200),  RGB(180, 220, 110),  RGB(245, 165,  95),
    RGB(110, 200, 240),  RGB(235, 220, 110),  RGB(160, 115, 230),
    RGB(110, 215, 175),  RGB(225, 130, 185),  RGB(155, 210, 110),
    RGB(110, 145, 225)
};
static const COLORREF FOOD_COLORS[8] = {
    RGB(245, 130, 130),  RGB(130, 215, 240),  RGB(245, 215, 115),
    RGB(140, 230, 165),  RGB(245, 165, 215),  RGB(195, 145, 240),
    RGB(245, 180, 115),  RGB(120, 230, 220)
};

#define BG_TOP      RGB(238, 244, 252)
#define BG_BOT      RGB(214, 226, 244)
#define EDGE_COL    RGB( 80, 130, 200)
#define GRID_FLOOR  RGB(195, 210, 232)
#define GRID_CEIL   RGB(218, 226, 240)
#define HUD_BG      RGB(255, 255, 255)
#define HUD_BORDER  RGB(160, 188, 222)
#define HUD_TEXT    RGB( 55,  70, 100)
#define HUD_DIM     RGB(120, 140, 175)
#define SHADOW_COL  RGB(135, 150, 178)
#define CROSSHAIR   RGB( 70,  95, 130)
#define ACCENT      RGB( 80, 130, 200)
#define ERR_COLOR   RGB(190,  60,  60)

/* ════════════════════════════════════════
   Math 3D
════════════════════════════════════════ */
typedef struct { float x, y, z; } V3;
static V3 v3(float x, float y, float z) { V3 v={x,y,z}; return v; }
static V3 vsub(V3 a, V3 b) { return v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static V3 vmul(V3 a, float s) { return v3(a.x*s, a.y*s, a.z*s); }
static float vdot(V3 a, V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static V3 vcross(V3 a, V3 b) {
    return v3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
}
static V3 vnorm(V3 a) {
    float l = sqrtf(vdot(a,a));
    if (l < 1e-6f) return v3(0,0,0);
    return vmul(a, 1.0f/l);
}
static V3   g_eye, g_fwd, g_right, g_up;
static float g_focal;
static int   g_view_w, g_view_h;

static void camera_update(V3 target) {
    V3 dir;
    dir.x = sinf(g_yaw) * cosf(g_pitch);
    dir.y = sinf(g_pitch);
    dir.z = cosf(g_yaw) * cosf(g_pitch);
    g_eye = vsub(target, vmul(dir, g_cam_dist));
    g_fwd = vnorm(vsub(target, g_eye));
    V3 world_up = v3(0, 1, 0);
    V3 r = vcross(g_fwd, world_up);
    if (vdot(r,r) < 1e-4f) r = v3(1,0,0);
    g_right = vnorm(r);
    g_up    = vnorm(vcross(g_right, g_fwd));
}
static V3 world_to_cam(V3 p) {
    V3 v = vsub(p, g_eye);
    V3 c;
    c.x = vdot(v, g_right);
    c.y = vdot(v, g_up);
    c.z = vdot(v, g_fwd);
    return c;
}
static int project(V3 cam, float *sx, float *sy) {
    if (cam.z < NEAR_PLANE) return 0;
    *sx =  g_focal * cam.x / cam.z + g_view_w * 0.5f;
    *sy = -g_focal * cam.y / cam.z + g_view_h * 0.5f;
    return 1;
}
static int project_world(V3 p, float *sx, float *sy, float *cz) {
    V3 c = world_to_cam(p);
    *cz = c.z;
    return project(c, sx, sy);
}
static int clip_line_near(V3 *a, V3 *b) {
    if (a->z >= NEAR_PLANE && b->z >= NEAR_PLANE) return 1;
    if (a->z <  NEAR_PLANE && b->z <  NEAR_PLANE) return 0;
    float t = (NEAR_PLANE - a->z) / (b->z - a->z);
    V3 inter;
    inter.x = a->x + (b->x - a->x) * t;
    inter.y = a->y + (b->y - a->y) * t;
    inter.z = NEAR_PLANE;
    if (a->z < NEAR_PLANE) *a = inter; else *b = inter;
    return 1;
}
static void draw_world_line(HDC hdc, V3 wa, V3 wb, COLORREF col, int width) {
    V3 ca = world_to_cam(wa);
    V3 cb = world_to_cam(wb);
    if (!clip_line_near(&ca, &cb)) return;
    float sa_x, sa_y, sb_x, sb_y;
    if (!project(ca, &sa_x, &sa_y)) return;
    if (!project(cb, &sb_x, &sb_y)) return;
    HPEN pen = CreatePen(PS_SOLID, width, col);
    HPEN old = SelectObject(hdc, pen);
    MoveToEx(hdc, (int)sa_x, (int)sa_y, NULL);
    LineTo  (hdc, (int)sb_x, (int)sb_y);
    SelectObject(hdc, old);
    DeleteObject(pen);
}

/* ════════════════════════════════════════
   Rendu sphère
════════════════════════════════════════ */
static void DrawSphere(HDC hdc, int cx, int cy, int r, COLORREF col, float fog) {
    if (r < 2) r = 2;
    int R = GetRValue(col), G = GetGValue(col), B = GetBValue(col);
    float f = fog; if (f < 0) f = 0; if (f > 1) f = 1;
    const int BgR = 220, BgG = 232, BgB = 248;

    {
        int ro = r + 1 * SS;
        int Rc = (int)(R * 0.55f);
        int Gc = (int)(G * 0.55f);
        int Bc = (int)(B * 0.55f);
        int rr = Rc + (int)((BgR - Rc) * (1.0f - f));
        int gg = Gc + (int)((BgG - Gc) * (1.0f - f));
        int bb = Bc + (int)((BgB - Bc) * (1.0f - f));
        HBRUSH br = CreateSolidBrush(RGB(rr, gg, bb));
        HPEN   pn = CreatePen(PS_NULL, 0, 0);
        HBRUSH ob = SelectObject(hdc, br);
        HPEN   op = SelectObject(hdc, pn);
        Ellipse(hdc, cx - ro, cy - ro, cx + ro, cy + ro);
        SelectObject(hdc, ob); SelectObject(hdc, op);
        DeleteObject(br); DeleteObject(pn);
    }
    const int LAYERS = 10;
    for (int i = 0; i < LAYERS; i++) {
        float t     = (float)i / (LAYERS - 1);
        float shade = 0.62f + 0.55f * t;
        int Rb = (int)(R * shade); if (Rb > 255) Rb = 255;
        int Gb = (int)(G * shade); if (Gb > 255) Gb = 255;
        int Bb = (int)(B * shade); if (Bb > 255) Bb = 255;
        int rr = Rb + (int)((BgR - Rb) * (1.0f - f));
        int gg = Gb + (int)((BgG - Gb) * (1.0f - f));
        int bb = Bb + (int)((BgB - Bb) * (1.0f - f));
        int ri   = r - (int)(r * t * 0.88f);
        int offx = -(int)(r * 0.20f * t);
        int offy = -(int)(r * 0.20f * t);
        HBRUSH br = CreateSolidBrush(RGB(rr, gg, bb));
        HPEN   pn = CreatePen(PS_NULL, 0, 0);
        HBRUSH ob = SelectObject(hdc, br);
        HPEN   op = SelectObject(hdc, pn);
        Ellipse(hdc, cx + offx - ri, cy + offy - ri,
                     cx + offx + ri, cy + offy + ri);
        SelectObject(hdc, ob); SelectObject(hdc, op);
        DeleteObject(br); DeleteObject(pn);
    }
    if (r >= 6) {
        int hcx = cx - r / 3;
        int hcy = cy - r / 3;
        int hr  = r / 3; if (hr < 1) hr = 1;
        const int HLAY = 5;
        for (int i = 0; i < HLAY; i++) {
            float t  = (float)i / (HLAY - 1);
            int   ri = hr - (int)(hr * t * 0.90f);
            int   w  = 250 - (int)(30 * t);
            HBRUSH hbr = CreateSolidBrush(RGB(w, w, w));
            HPEN   hpn = CreatePen(PS_NULL, 0, 0);
            HBRUSH ob  = SelectObject(hdc, hbr);
            HPEN   op  = SelectObject(hdc, hpn);
            Ellipse(hdc, hcx - ri, hcy - ri, hcx + ri, hcy + ri);
            SelectObject(hdc, ob); SelectObject(hdc, op);
            DeleteObject(hbr); DeleteObject(hpn);
        }
    }
}

typedef struct {
    int      type;
    int      idx;
    float    cz;
    float    sx, sy;
    int      sr;
    COLORREF col;
    float    fog;
} RenderItem;

static int cmp_item(const void *a, const void *b) {
    const RenderItem *ra = a, *rb = b;
    if (ra->cz < rb->cz) return  1;
    if (ra->cz > rb->cz) return -1;
    return 0;
}

static void draw_ground_shadow(HDC hdc, V3 pos, float radius) {
    V3 g = v3(pos.x, 0.5f, pos.z);
    float gsx, gsy, gcz;
    if (!project_world(g, &gsx, &gsy, &gcz)) return;
    if (gcz <= NEAR_PLANE) return;
    V3 gx1 = v3(pos.x - radius, 0.5f, pos.z);
    V3 gx2 = v3(pos.x + radius, 0.5f, pos.z);
    float sx1, sy1, sx2, sy2, cz_tmp;
    if (!project_world(gx1, &sx1, &sy1, &cz_tmp)) return;
    if (!project_world(gx2, &sx2, &sy2, &cz_tmp)) return;
    float rx = fabsf(sx2 - sx1) * 0.5f;
    float ry = rx * 0.45f;
    if (rx < 2) return;
    float h_fade = 1.0f - (pos.y / MAP_SIZE);
    if (h_fade < 0.30f) h_fade = 0.30f;
    int sr = GetRValue(SHADOW_COL), sg = GetGValue(SHADOW_COL), sb = GetBValue(SHADOW_COL);
    int br = GetRValue(GRID_FLOOR), bg = GetGValue(GRID_FLOOR), bb = GetBValue(GRID_FLOOR);
    int rr = sr + (int)((br - sr) * (1.0f - h_fade));
    int gg = sg + (int)((bg - sg) * (1.0f - h_fade));
    int bbb= sb + (int)((bb - sb) * (1.0f - h_fade));
    HBRUSH brush = CreateSolidBrush(RGB(rr, gg, bbb));
    HPEN   pn    = CreatePen(PS_NULL, 0, 0);
    HBRUSH ob = SelectObject(hdc, brush);
    HPEN   op = SelectObject(hdc, pn);
    Ellipse(hdc, (int)(gsx - rx), (int)(gsy - ry),
                 (int)(gsx + rx), (int)(gsy + ry));
    SelectObject(hdc, ob); SelectObject(hdc, op);
    DeleteObject(brush); DeleteObject(pn);
}

static void draw_cube_and_grid(HDC hdc) {
    const float M = MAP_SIZE;
    V3 c[8] = {
        v3(0,0,0), v3(M,0,0), v3(M,0,M), v3(0,0,M),
        v3(0,M,0), v3(M,M,0), v3(M,M,M), v3(0,M,M)
    };
    int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7}
    };
    const int STEP = 200;
    for (int i = 0; i <= (int)(M / STEP); i++) {
        float v = (float)(i * STEP);
        draw_world_line(hdc, v3(v, 0, 0), v3(v, 0, M), GRID_FLOOR, 1 * SS);
        draw_world_line(hdc, v3(0, 0, v), v3(M, 0, v), GRID_FLOOR, 1 * SS);
    }
    for (int i = 0; i <= (int)(M / STEP); i++) {
        float v = (float)(i * STEP);
        draw_world_line(hdc, v3(v, M, 0), v3(v, M, M), GRID_CEIL, 1 * SS);
        draw_world_line(hdc, v3(0, M, v), v3(M, M, v), GRID_CEIL, 1 * SS);
    }
    for (int i = 0; i < 12; i++) {
        draw_world_line(hdc, c[edges[i][0]], c[edges[i][1]], EDGE_COL, 2 * SS);
    }
}

static HFONT make_font(int size, int bold, const char *face) {
    return CreateFont(size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                      0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                      DEFAULT_PITCH | FF_SWISS, face);
}

/* ════════════════════════════════════════
   Rendu frame de jeu
════════════════════════════════════════ */
static void Render(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    g_fps_frames++;
    DWORD now_ms = GetTickCount();
    if (g_fps_last_tick == 0) g_fps_last_tick = now_ms;
    DWORD dt = now_ms - g_fps_last_tick;
    if (dt >= 500) {
        g_fps_value = (g_fps_frames * 1000.0f) / (float)dt;
        g_fps_frames = 0;
        g_fps_last_tick = now_ms;
    }

    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int W_ss = W * SS, H_ss = H * SS;
    g_view_w = W_ss; g_view_h = H_ss;
    g_focal  = (H_ss * 0.5f) / tanf(DEG2RAD(35.0f));

    HDC     memDC  = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, W_ss, H_ss);
    HBITMAP oldBmp = SelectObject(memDC, memBmp);

    {
        const int bands = 60;
        int tr = GetRValue(BG_TOP), tg = GetGValue(BG_TOP), tb = GetBValue(BG_TOP);
        int br = GetRValue(BG_BOT), bg = GetGValue(BG_BOT), bb = GetBValue(BG_BOT);
        for (int i = 0; i < bands; i++) {
            float t = (float)i / (bands - 1);
            int r = tr + (int)((br - tr) * t);
            int g = tg + (int)((bg - tg) * t);
            int b = tb + (int)((bb - tb) * t);
            RECT bRc = {0, (H_ss * i) / bands, W_ss, (H_ss * (i+1)) / bands};
            HBRUSH bbr = CreateSolidBrush(RGB(r,g,b));
            FillRect(memDC, &bRc, bbr);
            DeleteObject(bbr);
        }
    }

    EnterCriticalSection(&g_cs);
    WorldStatePacket world = g_world;
    LeaveCriticalSection(&g_cs);

    PlayerState *me = &world.players[g_player_id];
    V3 target;
    if (me->radius > 0.5f) {
        target = v3(me->x, me->y + me->radius * 0.5f, me->z);
    } else {
        target = v3(MAP_SIZE/2, MAP_SIZE/2, MAP_SIZE/2);
    }
    camera_update(target);

    draw_cube_and_grid(memDC);

    static RenderItem items[MAX_PLAYERS + MAX_FOOD];
    int n = 0;
    for (int i = 0; i < world.food_count && n < MAX_PLAYERS+MAX_FOOD; i++) {
        V3 p = v3(world.food[i].x, world.food[i].y, world.food[i].z);
        V3 c = world_to_cam(p);
        if (c.z < NEAR_PLANE) continue;
        float sx =  g_focal * c.x / c.z + W_ss * 0.5f;
        float sy = -g_focal * c.y / c.z + H_ss * 0.5f;
        int   sr = (int)(g_focal * 14.0f / c.z);
        if (sr < 1) continue;
        if (sx < -sr-10 || sx > W_ss+sr+10 || sy < -sr-10 || sy > H_ss+sr+10) continue;
        float fog = 1.0f - (c.z / FAR_FOG); if (fog < 0) fog = 0;
        items[n].type=0; items[n].idx=i; items[n].cz=c.z;
        items[n].sx=sx; items[n].sy=sy; items[n].sr=sr;
        items[n].col=FOOD_COLORS[world.food[i].color & 7];
        items[n].fog=fog;
        n++;
    }
    for (int i = 0; i < world.player_count && n < MAX_PLAYERS+MAX_FOOD; i++) {
        PlayerState *p = &world.players[i];
        if (p->radius < 1.0f) continue;
        V3 wp = v3(p->x, p->y, p->z);
        V3 c  = world_to_cam(wp);
        if (c.z < NEAR_PLANE) continue;
        float sx =  g_focal * c.x / c.z + W_ss * 0.5f;
        float sy = -g_focal * c.y / c.z + H_ss * 0.5f;
        int   sr = (int)(g_focal * p->radius / c.z);
        if (sr < 2) continue;
        if (sx < -sr-40 || sx > W_ss+sr+40 || sy < -sr-40 || sy > H_ss+sr+40) continue;
        float fog = 1.0f - (c.z / FAR_FOG); if (fog < 0) fog = 0;
        items[n].type=1; items[n].idx=i; items[n].cz=c.z;
        items[n].sx=sx; items[n].sy=sy; items[n].sr=sr;
        items[n].col=PLAYER_COLORS[p->id % MAX_PLAYERS];
        items[n].fog=fog;
        n++;
    }
    qsort(items, n, sizeof(items[0]), cmp_item);

    for (int i = 0; i < n; i++) {
        if (items[i].type == 0) continue;
        PlayerState *p = &world.players[items[i].idx];
        draw_ground_shadow(memDC, v3(p->x, p->y, p->z), p->radius);
    }

    HFONT labelFont = make_font(15 * SS, 1, "Segoe UI");
    for (int i = 0; i < n; i++) {
        DrawSphere(memDC, (int)items[i].sx, (int)items[i].sy,
                   items[i].sr, items[i].col, items[i].fog);
        if (items[i].type == 1 && items[i].sr >= 10) {
            PlayerState *p = &world.players[items[i].idx];
            char lbl[24];
            if (p->id == g_player_id) snprintf(lbl, sizeof(lbl), "Vous");
            else                      snprintf(lbl, sizeof(lbl), "P%d", p->id);
            HFONT oldF = SelectObject(memDC, labelFont);
            SetBkMode(memDC, TRANSPARENT);
            SIZE sz; GetTextExtentPoint32(memDC, lbl, (int)strlen(lbl), &sz);
            SetTextColor(memDC, RGB(255,255,255));
            for (int dx = -1; dx <= 1; dx++)
                for (int dy = -1; dy <= 1; dy++)
                    TextOut(memDC, (int)items[i].sx - sz.cx/2 + dx,
                                   (int)items[i].sy + dy, lbl, (int)strlen(lbl));
            SetTextColor(memDC, HUD_TEXT);
            TextOut(memDC, (int)items[i].sx - sz.cx/2,
                           (int)items[i].sy, lbl, (int)strlen(lbl));
            SelectObject(memDC, oldF);
        }
    }
    DeleteObject(labelFont);

    /* HUD */
    {
        RECT topRc = {0, 0, W_ss, 42 * SS};
        HBRUSH topBg = CreateSolidBrush(HUD_BG);
        FillRect(memDC, &topRc, topBg);
        DeleteObject(topBg);
        HPEN sepPen = CreatePen(PS_SOLID, 1 * SS, HUD_BORDER);
        HPEN op = SelectObject(memDC, sepPen);
        MoveToEx(memDC, 0, 42 * SS, NULL); LineTo(memDC, W_ss, 42 * SS);
        SelectObject(memDC, op);
        DeleteObject(sepPen);

        HFONT hudFont = make_font(18 * SS, 1, "Segoe UI");
        HFONT oldF = SelectObject(memDC, hudFont);
        SetBkMode(memDC, TRANSPARENT);

        char title[64];
        snprintf(title, sizeof(title), "AGAR  3D   —   Joueur %d", g_player_id);
        SetTextColor(memDC, PLAYER_COLORS[g_player_id % MAX_PLAYERS]);
        TextOut(memDC, 16 * SS, 11 * SS, title, (int)strlen(title));

        char hud[200];
        snprintf(hud, sizeof(hud), "Taille : %.0f     Pos : (%.0f, %.0f, %.0f)     FPS : %.0f",
                 me->radius, me->x, me->y, me->z, g_fps_value);
        SetTextColor(memDC, HUD_TEXT);
        TextOut(memDC, 270 * SS, 12 * SS, hud, (int)strlen(hud));

        int sorted_ids[MAX_PLAYERS];
        for (int i = 0; i < MAX_PLAYERS; i++) sorted_ids[i] = i;
        for (int i = 0; i < MAX_PLAYERS; i++)
            for (int j = i+1; j < MAX_PLAYERS; j++)
                if (world.players[sorted_ids[j]].radius >
                    world.players[sorted_ids[i]].radius) {
                    int t = sorted_ids[i]; sorted_ids[i] = sorted_ids[j]; sorted_ids[j] = t;
                }

        RECT lbRc = {(W - 240) * SS, 56 * SS, (W - 12) * SS, (56 + 24*6) * SS};
        HBRUSH lbBg = CreateSolidBrush(HUD_BG);
        FillRect(memDC, &lbRc, lbBg);
        DeleteObject(lbBg);
        HPEN lbPen = CreatePen(PS_SOLID, 1 * SS, HUD_BORDER);
        op = SelectObject(memDC, lbPen);
        HBRUSH nb = GetStockObject(NULL_BRUSH);
        HBRUSH ob = SelectObject(memDC, nb);
        Rectangle(memDC, lbRc.left, lbRc.top, lbRc.right, lbRc.bottom);
        SelectObject(memDC, ob); SelectObject(memDC, op);
        DeleteObject(lbPen);

        HFONT lbFont = make_font(15 * SS, 1, "Segoe UI");
        SelectObject(memDC, lbFont);
        SetTextColor(memDC, HUD_DIM);
        TextOut(memDC, (W - 230) * SS, 60 * SS, "CLASSEMENT", 10);

        int rank = 0;
        for (int i = 0; i < MAX_PLAYERS && rank < 5; i++) {
            PlayerState *p = &world.players[sorted_ids[i]];
            if (p->radius < 1.0f) continue;
            char row[48];
            snprintf(row, sizeof(row), "%d. P%d  —  %.0f", rank+1, p->id, p->radius);
            SetTextColor(memDC, PLAYER_COLORS[p->id % MAX_PLAYERS]);
            TextOut(memDC, (W - 230) * SS, (80 + rank * 22) * SS, row, (int)strlen(row));
            rank++;
        }
        DeleteObject(lbFont);

        HFONT helpFont = make_font(13 * SS, 0, "Segoe UI");
        SelectObject(memDC, helpFont);
        SetTextColor(memDC, HUD_DIM);
        const char *help =
            "[WASD/ZQSD/Fleches] avance dans la direction de la camera   "
            "[Souris] vise   [Molette] zoom   [Tab] souris on/off   [Echap] quitter";
        TextOut(memDC, 16 * SS, (H - 22) * SS, help, (int)strlen(help));
        DeleteObject(helpFont);

        SelectObject(memDC, oldF);
        DeleteObject(hudFont);

        HPEN rp = CreatePen(PS_SOLID, 1 * SS, CROSSHAIR);
        op = SelectObject(memDC, rp);
        MoveToEx(memDC, W_ss/2 - 6 * SS, H_ss/2, NULL); LineTo(memDC, W_ss/2 + 6 * SS, H_ss/2);
        MoveToEx(memDC, W_ss/2, H_ss/2 - 6 * SS, NULL); LineTo(memDC, W_ss/2, H_ss/2 + 6 * SS);
        SelectObject(memDC, op);
        DeleteObject(rp);
    }

    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);
    StretchBlt(hdc, 0, 0, W, H, memDC, 0, 0, W_ss, H_ss, SRCCOPY);

    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
    EndPaint(hwnd, &ps);
}

static void recompute_screen_center(void) {
    RECT rc; GetClientRect(g_hwnd, &rc);
    POINT c = {(rc.right - rc.left)/2, (rc.bottom - rc.top)/2};
    ClientToScreen(g_hwnd, &c);
    g_screen_center = c;
}
static void update_mouse_look(void) {
    if (!g_mouse_captured) return;
    if (GetForegroundWindow() != g_hwnd) return;
    recompute_screen_center();
    POINT p; GetCursorPos(&p);
    int dx = p.x - g_screen_center.x;
    int dy = p.y - g_screen_center.y;
    if (dx == 0 && dy == 0) return;
    const float sens = 0.0028f;
    g_yaw   -= dx * sens;
    g_pitch -= dy * sens;
    if (g_pitch >  1.45f) g_pitch =  1.45f;
    if (g_pitch < -1.45f) g_pitch = -1.45f;
    while (g_yaw >  PI) g_yaw -= 2*PI;
    while (g_yaw < -PI) g_yaw += 2*PI;
    SetCursorPos(g_screen_center.x, g_screen_center.y);
}
static void set_mouse_capture(int on) {
    g_mouse_captured = on;
    if (on) {
        ShowCursor(FALSE);
        recompute_screen_center();
        SetCursorPos(g_screen_center.x, g_screen_center.y);
        SetCapture(g_hwnd);
    } else {
        ShowCursor(TRUE);
        ReleaseCapture();
    }
}

/* ════════════════════════════════════════
   Thread réseau
════════════════════════════════════════ */
static DWORD WINAPI NetworkThread(LPVOID param) {
    (void)param;
    while (1) {
        float cp = cosf(g_pitch), sp = sinf(g_pitch);
        float fx = sinf(g_yaw) * cp;
        float fy = sp;
        float fz = cosf(g_yaw) * cp;
        float rx =  cosf(g_yaw);
        float rz = -sinf(g_yaw);

        float dx = 0, dy = 0, dz = 0;
        if ((GetAsyncKeyState('W') & 0x8000) || (GetAsyncKeyState(VK_UP)    & 0x8000)) { dx += fx; dy += fy; dz += fz; }
        if ((GetAsyncKeyState('S') & 0x8000) || (GetAsyncKeyState(VK_DOWN)  & 0x8000)) { dx -= fx; dy -= fy; dz -= fz; }
        if ((GetAsyncKeyState('D') & 0x8000) || (GetAsyncKeyState(VK_RIGHT) & 0x8000)) { dx += rx; dz += rz; }
        if ((GetAsyncKeyState('A') & 0x8000) || (GetAsyncKeyState(VK_LEFT)  & 0x8000)) { dx -= rx; dz -= rz; }

        float len = sqrtf(dx*dx + dy*dy + dz*dz);
        if (len > 0.01f) { dx/=len; dy/=len; dz/=len; }
        else             { dx = dy = dz = 0; }

        ClientInputPacket inp;
        inp.id    = g_player_id;
        inp.dir_x = dx; inp.dir_y = dy; inp.dir_z = dz;
        sendto(g_sock, (char*)&inp, sizeof(inp), 0,
               (struct sockaddr*)&g_server_addr, sizeof(g_server_addr));

        WorldStatePacket world;
        struct sockaddr_in from;
        int from_len = sizeof(from);
        int n = recvfrom(g_sock, (char*)&world, sizeof(world), 0,
                         (struct sockaddr*)&from, &from_len);
        if (n == sizeof(world)) {
            EnterCriticalSection(&g_cs);
            g_world = world;
            LeaveCriticalSection(&g_cs);
        }
        Sleep(16);
    }
    return 0;
}

/* ════════════════════════════════════════
   Connexion : envoie un paquet de découverte
   à l'IP donnée et attend la réponse magique
════════════════════════════════════════ */
static int try_connect(const char *server_ip) {
    struct sockaddr_in target;
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port   = htons(SERVER_PORT);
    if (inet_pton(AF_INET, server_ip, &target.sin_addr) != 1) return 0;

    DWORD to = 600;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));

    ClientInputPacket disco;
    memset(&disco, 0, sizeof(disco));
    disco.id = DISCOVERY_ID;

    for (int attempt = 0; attempt < 5; attempt++) {
        sendto(g_sock, (char*)&disco, sizeof(disco), 0,
               (struct sockaddr*)&target, sizeof(target));
        DWORD deadline = GetTickCount() + 600;
        while (GetTickCount() < deadline) {
            DiscoveryReplyPacket rep;
            struct sockaddr_in from;
            int from_len = sizeof(from);
            int n = recvfrom(g_sock, (char*)&rep, sizeof(rep), 0,
                             (struct sockaddr*)&from, &from_len);
            if (n == sizeof(rep) && rep.magic == SERVER_MAGIC && rep.assigned_id >= 0) {
                g_server_addr = from;
                g_player_id   = rep.assigned_id;
                DWORD t = 50;
                setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&t, sizeof(t));
                return 1;
            }
        }
    }
    return 0;
}

/* ════════════════════════════════════════
   Fenêtre de connexion (dialog perso)
════════════════════════════════════════ */
#define DLG_W 480
#define DLG_H 340
#define IDC_EDIT_IP  1001
#define IDC_BTN_OK   1002

static HWND  g_dlg = NULL;
static HWND  g_edit_ip = NULL;
static HWND  g_btn_ok = NULL;
static char  g_status_msg[160] = "";
static COLORREF g_status_color = HUD_DIM;
static char  g_input_ip[64] = "";
static HBRUSH g_edit_brush = NULL;

static DWORD WINAPI ConnectThread(LPVOID p) {
    (void)p;
    int ok = try_connect(g_input_ip);
    PostMessage(g_dlg, WM_USER + 1, ok ? 1 : 0, 0);
    return 0;
}

static void draw_dialog(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    HDC     memDC  = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP oldBmp = SelectObject(memDC, memBmp);

    /* fond dégradé */
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

    /* titre */
    {
        HFONT f = make_font(40, 1, "Segoe UI");
        HFONT of = SelectObject(memDC, f);
        SetTextColor(memDC, ACCENT);
        RECT t = {0, 22, W, 80};
        DrawText(memDC, "AGAR 3D", -1, &t, DT_CENTER | DT_SINGLELINE);
        SelectObject(memDC, of);
        DeleteObject(f);
    }
    /* sous-titre */
    {
        HFONT f = make_font(15, 0, "Segoe UI");
        HFONT of = SelectObject(memDC, f);
        SetTextColor(memDC, HUD_DIM);
        RECT t = {0, 78, W, 102};
        DrawText(memDC, "Connexion au serveur", -1, &t, DT_CENTER | DT_SINGLELINE);
        SelectObject(memDC, of);
        DeleteObject(f);
    }
    /* label */
    {
        HFONT f = make_font(14, 1, "Segoe UI");
        HFONT of = SelectObject(memDC, f);
        SetTextColor(memDC, HUD_TEXT);
        TextOut(memDC, 55, 130, "Adresse IP du serveur :", 24);
        SelectObject(memDC, of);
        DeleteObject(f);
    }
    /* status */
    if (g_status_msg[0]) {
        HFONT f = make_font(14, 0, "Segoe UI");
        HFONT of = SelectObject(memDC, f);
        SetTextColor(memDC, g_status_color);
        RECT t = {0, 218, W, 240};
        DrawText(memDC, g_status_msg, -1, &t, DT_CENTER | DT_SINGLELINE);
        SelectObject(memDC, of);
        DeleteObject(f);
    }

    BitBlt(hdc, 0, 0, W, H, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK DlgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_edit_ip = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "127.0.0.1",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL,
            55, 158, 370, 38, hwnd, (HMENU)(intptr_t)IDC_EDIT_IP, NULL, NULL);
        HFONT ef = make_font(22, 0, "Consolas");
        SendMessage(g_edit_ip, WM_SETFONT, (WPARAM)ef, TRUE);
        SendMessage(g_edit_ip, EM_SETSEL, 0, -1);

        g_btn_ok = CreateWindow("BUTTON", "Se connecter",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
            165, 252, 150, 44, hwnd, (HMENU)(intptr_t)IDC_BTN_OK, NULL, NULL);
        HFONT bf = make_font(16, 1, "Segoe UI");
        SendMessage(g_btn_ok, WM_SETFONT, (WPARAM)bf, TRUE);

        g_edit_brush = CreateSolidBrush(RGB(255, 255, 255));
        SetFocus(g_edit_ip);
        return 0;
    }
    case WM_PAINT:
        draw_dialog(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, RGB(255, 255, 255));
        SetTextColor(dc, HUD_TEXT);
        return (LRESULT)g_edit_brush;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_BTN_OK && HIWORD(wp) == BN_CLICKED) {
            char ip[64];
            GetWindowText(g_edit_ip, ip, sizeof(ip));
            /* trim */
            int s = 0, e = (int)strlen(ip);
            while (s < e && (ip[s] == ' ' || ip[s] == '\t')) s++;
            while (e > s && (ip[e-1] == ' ' || ip[e-1] == '\t')) e--;
            ip[e] = 0;
            if (ip[s] == 0) {
                strncpy(g_status_msg, "Saisis une adresse IP.", sizeof(g_status_msg));
                g_status_color = ERR_COLOR;
                InvalidateRect(hwnd, NULL, TRUE);
                return 0;
            }
            strncpy(g_input_ip, ip + s, sizeof(g_input_ip) - 1);
            g_input_ip[sizeof(g_input_ip) - 1] = 0;

            EnableWindow(g_btn_ok, FALSE);
            EnableWindow(g_edit_ip, FALSE);
            strncpy(g_status_msg, "Connexion en cours...", sizeof(g_status_msg));
            g_status_color = HUD_DIM;
            InvalidateRect(hwnd, NULL, TRUE);

            CreateThread(NULL, 0, ConnectThread, NULL, 0, NULL);
        }
        return 0;
    case WM_USER + 1:
        if (wp == 1) {
            g_connected = 1;
            DestroyWindow(hwnd);
        } else {
            strncpy(g_status_msg,
                    "Pas de reponse a cette adresse. Verifie l'IP / pare-feu.",
                    sizeof(g_status_msg));
            g_status_color = ERR_COLOR;
            EnableWindow(g_btn_ok, TRUE);
            EnableWindow(g_edit_ip, TRUE);
            SetFocus(g_edit_ip);
            SendMessage(g_edit_ip, EM_SETSEL, 0, -1);
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_edit_brush) { DeleteObject(g_edit_brush); g_edit_brush = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* ════════════════════════════════════════
   WndProc fenêtre de jeu
════════════════════════════════════════ */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, 1, 16, NULL);
        return 0;
    case WM_TIMER:
        update_mouse_look();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_SIZE:
        recompute_screen_center();
        return 0;
    case WM_PAINT:
        Render(hwnd);
        return 0;
    case WM_MOUSEWHEEL: {
        short delta = GET_WHEEL_DELTA_WPARAM(wp);
        g_cam_dist -= delta * 0.4f;
        if (g_cam_dist < CAM_DIST_MIN) g_cam_dist = CAM_DIST_MIN;
        if (g_cam_dist > CAM_DIST_MAX) g_cam_dist = CAM_DIST_MAX;
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (!g_mouse_captured) set_mouse_capture(1);
        return 0;
    case WM_SETFOCUS:
        if (g_mouse_captured) {
            recompute_screen_center();
            SetCursorPos(g_screen_center.x, g_screen_center.y);
            ShowCursor(FALSE);
        }
        return 0;
    case WM_KILLFOCUS:
        ShowCursor(TRUE);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { PostQuitMessage(0); return 0; }
        if (wp == VK_TAB)    { set_mouse_capture(!g_mouse_captured); return 0; }
        return 0;
    case WM_DESTROY:
        if (g_mouse_captured) ShowCursor(TRUE);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* ════════════════════════════════════════
   WinMain : dialog connexion → fenêtre jeu
════════════════════════════════════════ */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hPrev;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);

    InitializeCriticalSection(&g_cs);
    memset(&g_world, 0, sizeof(g_world));

    /* enregistre les 2 classes */
    WNDCLASS wcd = {0};
    wcd.lpfnWndProc   = DlgWndProc;
    wcd.hInstance     = hInst;
    wcd.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wcd.lpszClassName = "Agar3DConnect";
    wcd.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wcd.style         = CS_HREDRAW | CS_VREDRAW;
    RegisterClass(&wcd);

    WNDCLASS wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = "Agar3DClient";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    RegisterClass(&wc);

    /* si une IP est fournie en argument, on saute la dialog */
    if (lpCmd && lpCmd[0] != '\0' && strchr(lpCmd, '.')) {
        char ip[64];
        strncpy(ip, lpCmd, 63); ip[63] = 0;
        /* trim */
        int s = 0, e = (int)strlen(ip);
        while (s < e && (ip[s] == ' ' || ip[s] == '\t')) s++;
        while (e > s && (ip[e-1] == ' ' || ip[e-1] == '\t')) e--;
        ip[e] = 0;
        if (try_connect(ip + s)) {
            g_connected = 1;
        } else {
            MessageBox(NULL,
                "Pas de reponse a cette adresse.\nVerifie l'IP et le pare-feu.",
                "Connexion impossible", MB_ICONERROR);
        }
    }

    /* si pas connecté, on ouvre la fenêtre de dialog */
    if (!g_connected) {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        g_dlg = CreateWindow("Agar3DConnect", "Agar 3D — Connexion",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            (sw - DLG_W) / 2, (sh - DLG_H) / 2, DLG_W, DLG_H,
            NULL, NULL, hInst, NULL);
        ShowWindow(g_dlg, nShow);
        UpdateWindow(g_dlg);

        MSG msg;
        while (GetMessage(&msg, NULL, 0, 0)) {
            if (!IsDialogMessage(g_dlg, &msg)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
    }

    if (!g_connected) {
        closesocket(g_sock);
        WSACleanup();
        DeleteCriticalSection(&g_cs);
        return 0;
    }

    /* fenêtre de jeu */
    g_hwnd = CreateWindow("Agar3DClient", "Agar.io 3D",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, WINDOW_W, WINDOW_H,
        NULL, NULL, hInst, NULL);
    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);
    recompute_screen_center();
    set_mouse_capture(1);

    CreateThread(NULL, 0, NetworkThread, NULL, 0, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_mouse_captured) ShowCursor(TRUE);
    closesocket(g_sock);
    WSACleanup();
    DeleteCriticalSection(&g_cs);
    return 0;
}
