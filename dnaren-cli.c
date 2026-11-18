/* dnaren-cli is minimal DNA renderer for terminals */
/* software renderer, ANSI escapes, libc + libm only */
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "dnaren.h"

/* terminal specific settings; can be moved to dnaren.h */
#ifndef CLI_FRAME_MS
#define CLI_FRAME_MS 33         /* wanted frame interval, ~30 fps */
#endif
#ifndef CLI_FRAME_MAX_MS
#define CLI_FRAME_MAX_MS 250    /* slowest interval when terminal lags */
#endif
#ifndef CLI_MIN_RADIUS
#define CLI_MIN_RADIUS 0.75f    /* smallest bead radius in pixels */
#endif
#ifndef CLI_FIT_MARGIN
#define CLI_FIT_MARGIN 1.12f    /* free space around the helix */
#endif
#ifndef CLI_BOND_RADIUS
#define CLI_BOND_RADIUS 0.045f  /* hydrogen bond thickness */
#endif
#ifndef CLI_MOUSE_SENS
#define CLI_MOUSE_SENS 2.0f     /* degrees per terminal column */
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NUM_PALETTES 3
#define AMBIENT 0.30f
#define SPECULAR 0.50f
#define SHININESS 48.0f
#define ZINF 1e30f
#define NONE 0xFFFFFFFFu
#define GROOVE (GROOVE_ANGLE / 180.0f * (float)M_PI)
#define BASE_END 0.35f
#define PUTS(p, s) (memcpy((p), (s), sizeof(s) - 1), (p) += sizeof(s) - 1)

enum { COL_TRUE, COL_256, COL_16, COL_MONO, NUM_COL };
enum { PX_SPACE, PX_HALF };
enum { OR_AUTO, OR_VERT, OR_HORIZ };

typedef struct { float bg[3]; float s1[3]; float s2[3]; float bp[3]; float atom[3]; float base[4][3]; } Palette;
typedef struct { uint32_t a, b; } Cell;

static Palette palettes[NUM_PALETTES] = {
    { {0.00f,0.00f,0.00f}, {0.20f,0.80f,1.00f}, {1.00f,0.40f,0.70f}, {0.85f,0.85f,0.90f}, {1.00f,1.00f,0.30f},
      { {0.30f,0.85f,0.40f}, {0.95f,0.30f,0.30f}, {1.00f,0.65f,0.15f}, {0.35f,0.45f,1.00f} } },
    { {0.00f,0.00f,0.00f}, {0.00f,1.00f,0.00f}, {1.00f,0.00f,0.00f}, {0.80f,0.80f,0.80f}, {1.00f,1.00f,1.00f},
      { {1.00f,0.85f,0.10f}, {0.10f,0.80f,0.90f}, {1.00f,0.50f,0.00f}, {0.60f,0.35f,1.00f} } },
    { {0.00f,0.00f,0.00f}, {1.00f,1.00f,1.00f}, {0.70f,0.70f,0.70f}, {0.70f,0.70f,0.70f}, {0.20f,0.60f,1.00f},
      { {0.85f,0.85f,0.85f}, {0.55f,0.55f,0.55f}, {0.20f,0.60f,1.00f}, {0.10f,0.35f,0.70f} } },
};

static const char *colname[NUM_COL] = { "truecolor", "256", "16", "mono" };
static const char ramp[] = " .:-=+*#%@";
static const int pal16[16][3] = {
    {0,0,0}, {205,0,0}, {0,205,0}, {205,205,0}, {0,0,238}, {205,0,205}, {0,205,205}, {229,229,229},
    {127,127,127}, {255,0,0}, {0,255,0}, {255,255,0}, {92,92,255}, {255,0,255}, {0,255,255}, {255,255,255},
};

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t resized = 1;
static struct termios orig_tio;
static int tio_saved;

static int cols = 80, rows = 24;    /* terminal size in cells */
static int gw, gh;                  /* cell grid, without status row */
static int pw, ph;                  /* framebuffer size in pixels */
static float pa = 1.0f;             /* pixel width / pixel height */
static float *fb, *zb;
static Cell *cell, *prev;
static char *ob;
static size_t obcap;

static float angle = 0.0f;
static float speed = SPEED_DEFAULT;
static int paused = 0;
static float cam_dist = CAM_DISTANCE_DEFAULT;
static float cam_rot_x = 20.0f;
static float cam_rot_y = 0.0f;
static int palette = 0;
static int colmode = COL_TRUE;
static int pxmode = PX_SPACE;
static int orient = OR_AUTO;
static int show_hud = 1;
static int redraw = 1;
static int dirty = 1;
static int mouse_down = 0;
static int last_mx, last_my;
static double frame_s = CLI_FRAME_MS / 1000.0;

static unsigned char ib[256];
static int in_n;
static char hudprev[512];

/* view state, set once per frame */
static float rot[9], fcl, ccx, ccy, model_h;
static int horiz;
static float lx, ly, lz, hx, hy, hz;

static void term_done(void);

static void die(const char *s) {
    term_done();
    perror(s);
    exit(1);
}

static void *xrealloc(void *p, size_t n) {
    p = realloc(p, n);
    if (!p) die("realloc");
    return p;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void write_all(const char *s, size_t n) {
    while (n > 0) {
        ssize_t w = write(STDOUT_FILENO, s, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                struct pollfd pf;
                pf.fd = STDOUT_FILENO; pf.events = POLLOUT; pf.revents = 0;
                poll(&pf, 1, 100);
                continue;
            }
            running = 0;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

static void on_term(int s) { (void)s; running = 0; }
static void on_winch(int s) { (void)s; resized = 1; }

static void term_init(void) {
    struct termios t;
    struct sigaction sa;
    static const char s[] = "\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[?1002h\x1b[?1006h\x1b[0m\x1b[2J";
    if (tcgetattr(STDIN_FILENO, &orig_tio) < 0) die("tcgetattr");
    t = orig_tio;
    t.c_iflag &= ~(tcflag_t)(IXON | ICRNL | INLCR | BRKINT | ISTRIP);
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) < 0) die("tcsetattr");
    tio_saved = 1;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_term;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);
    write_all(s, sizeof(s) - 1);
}

static void term_done(void) {
    static const char s[] = "\x1b[0m\x1b[?2026l\x1b[?7h\x1b[?1006l\x1b[?1002l\x1b[?25h\x1b[?1049l";
    if (!tio_saved) return;
    tio_saved = 0;
    write_all(s, sizeof(s) - 1);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_tio);
}

static int detect_color(void) {
    const char *ct = getenv("COLORTERM");
    const char *t = getenv("TERM");
    const char *nc = getenv("NO_COLOR");
    if (nc && *nc) return COL_MONO;
    if (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"))) return COL_TRUE;
    if (!t || !*t || strcmp(t, "dumb") == 0) return COL_MONO;
    if (strstr(t, "direct") || strstr(t, "kitty") || strstr(t, "alacritty") ||
        strstr(t, "wezterm") || strstr(t, "foot") || strstr(t, "ghostty"))
        return COL_TRUE;
    if (strstr(t, "256")) return COL_256;
    return COL_16;
}

static int half(void) {
    return pxmode == PX_HALF && colmode != COL_MONO;
}

/* terminal size in cells; pixel aspect from window pixels when known */
static void term_size(void) {
    struct winsize ws;
    const char *e;
    float cw = 0.0f, ch = 0.0f;
    int c = 0, r = 0;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        c = ws.ws_col;
        r = ws.ws_row;
        if (ws.ws_xpixel > 0 && ws.ws_ypixel > 0) {
            cw = (float)ws.ws_xpixel / (float)c;
            ch = (float)ws.ws_ypixel / (float)r;
        }
    }
    if (c <= 0 && (e = getenv("COLUMNS"))) c = atoi(e);
    if (r <= 0 && (e = getenv("LINES"))) r = atoi(e);
    cols = c > 0 ? c : 80;
    rows = r > 0 ? r : 24;
    pa = (cw > 0.0f && ch > 0.0f) ? 2.0f * cw / ch : 1.0f;
    if (pa < 0.5f) pa = 0.5f;
    if (pa > 2.0f) pa = 2.0f;
}

/* (re)allocate buffers for current size and modes */
static void layout(void) {
    int hud = (show_hud && rows > 1) ? 1 : 0;
    size_t np, nc;
    gh = rows - hud;
    if (half()) { gw = cols; pw = cols; ph = gh * 2; }
    else { gw = (cols + 1) / 2; pw = gw; ph = gh; }
    np = (size_t)pw * (size_t)ph;
    nc = (size_t)gw * (size_t)gh;
    fb = xrealloc(fb, np * 3 * sizeof(float));
    zb = xrealloc(zb, np * sizeof(float));
    cell = xrealloc(cell, nc * sizeof(Cell));
    prev = xrealloc(prev, nc * sizeof(Cell));
    obcap = nc * 64 + (size_t)cols + 256;
    ob = xrealloc(ob, obcap);
    redraw = 1;
    dirty = 1;
}

static void light_init(void) {
    float l = sqrtf(0.4f * 0.4f + 0.6f * 0.6f + 1.0f);
    float h;
    lx = -0.4f / l; ly = 0.6f / l; lz = 1.0f / l;
    h = sqrtf(lx * lx + ly * ly + (lz + 1.0f) * (lz + 1.0f));
    hx = lx / h; hy = ly / h; hz = (lz + 1.0f) / h;
}

/* ---- math ---- */

static void rotm(float *m, int axis, float deg) {
    float r = deg * (float)M_PI / 180.0f;
    float c = cosf(r), s = sinf(r);
    memset(m, 0, 9 * sizeof(float));
    if (axis == 0) { m[0] = 1; m[4] = c; m[5] = -s; m[7] = s; m[8] = c; }
    else if (axis == 1) { m[0] = c; m[2] = s; m[4] = 1; m[6] = -s; m[8] = c; }
    else { m[0] = c; m[1] = -s; m[3] = s; m[4] = c; m[8] = 1; }
}

static void mmul(float *o, const float *a, const float *b) {
    int i, j, k;
    float t[9];
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) {
            t[i * 3 + j] = 0.0f;
            for (k = 0; k < 3; k++) t[i * 3 + j] += a[i * 3 + k] * b[k * 3 + j];
        }
    memcpy(o, t, sizeof(t));
}

/* fit helix into pixel grid, pick orientation, build camera matrix */
static void setup_view(void) {
    float len = HELIX_TURNS * HELIX_PITCH + 2.0f * SPHERE_RADIUS;
    float th = 2.0f * (HELIX_RADIUS + SPHERE_RADIUS);
    float w = (float)pw * pa, h = (float)ph;
    float sv = fminf(h / len, w / th) / CLI_FIT_MARGIN;
    float sh = fminf(w / len, h / th) / CLI_FIT_MARGIN;
    float a[9], b[9], c[9];
    horiz = orient == OR_HORIZ || (orient == OR_AUTO && sh > sv);
    fcl = (horiz ? sh : sv) * CAM_DISTANCE_DEFAULT;
    ccx = (float)pw / 2.0f;
    ccy = (float)ph / 2.0f;
    model_h = HELIX_TURNS * HELIX_PITCH;
    /* rot = Rx * Ry * Rz(roll) * Ry(spin) */
    rotm(a, 1, angle);
    rotm(b, 2, horiz ? 90.0f : 0.0f);
    mmul(c, b, a);
    rotm(a, 1, cam_rot_y);
    mmul(b, a, c);
    rotm(a, 0, cam_rot_x);
    mmul(rot, a, b);
}

/* ---- rasterizer: shaded sphere impostors with z-buffer ---- */

static void splat(const float *p, float r, const float *col, int lit) {
    float x = p[0], y = p[1] - model_h * 0.5f, z = p[2];
    float ex = rot[0] * x + rot[1] * y + rot[2] * z;
    float ey = rot[3] * x + rot[4] * y + rot[5] * z;
    float ez = rot[6] * x + rot[7] * y + rot[8] * z - cam_dist;
    float d = -ez, k, rr, sx, sy, xr;
    int x0, x1, y0, y1, px, py;
    if (d < 1.0f) return;
    k = fcl / d;
    rr = k * r;
    if (rr < CLI_MIN_RADIUS) rr = CLI_MIN_RADIUS;
    sx = ccx + k * ex / pa;
    sy = ccy - k * ey;
    xr = rr / pa;
    if (sx + xr < 0.0f || sx - xr > (float)pw || sy + rr < 0.0f || sy - rr > (float)ph) return;
    x0 = (int)floorf(sx - xr); x1 = (int)ceilf(sx + xr);
    y0 = (int)floorf(sy - rr); y1 = (int)ceilf(sy + rr);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > pw - 1) x1 = pw - 1;
    if (y1 > ph - 1) y1 = ph - 1;
    for (py = y0; py <= y1; py++)
        for (px = x0; px <= x1; px++) {
            float u = ((float)px + 0.5f - sx) * pa;
            float v = sy - ((float)py + 0.5f);
            float q = (u * u + v * v) / (rr * rr);
            float nz, depth, nx, ny, o[3];
            size_t idx = (size_t)py * (size_t)pw + (size_t)px;
            int i;
            if (q >= 1.0f) continue;
            nz = sqrtf(1.0f - q);
            depth = d - r * nz;
            if (depth >= zb[idx]) continue;
            zb[idx] = depth;
            if (lit) {
                float dif, sp = 0.0f, hd;
                nx = u / rr; ny = v / rr;
                dif = nx * lx + ny * ly + nz * lz;
                if (dif > 0.0f) {
                    hd = nx * hx + ny * hy + nz * hz;
                    if (hd > 0.0f) sp = SPECULAR * powf(hd, SHININESS);
                } else dif = 0.0f;
                for (i = 0; i < 3; i++) {
                    o[i] = col[i] * (AMBIENT + dif) + sp;
                    if (o[i] > 1.0f) o[i] = 1.0f;
                }
            } else {
                for (i = 0; i < 3; i++) o[i] = col[i];
            }
            fb[idx * 3] = o[0];
            fb[idx * 3 + 1] = o[1];
            fb[idx * 3 + 2] = o[2];
        }
}

/* rod as a chain of overlapping beads */
static void rod(const float *a, const float *b, float rad, const float *col, int lit) {
    float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    float len = sqrtf(dx * dx + dy * dy + dz * dz);
    int n = (int)(len / (rad * 0.8f)) + 1, i;
    for (i = 0; i <= n; i++) {
        float t = (float)i / (float)n;
        float q[3];
        q[0] = a[0] + dx * t;
        q[1] = a[1] + dy * t;
        q[2] = a[2] + dz * t;
        splat(q, rad, col, lit);
    }
}

static void helix_pt(float *v, int i, float off) {
    float t = (float)i / STEPS_PER_TURN * 2.0f * (float)M_PI + off;
    v[0] = HELIX_RADIUS * cosf(t);
    v[1] = HELIX_PITCH * (float)i / STEPS_PER_TURN;
    v[2] = -HELIX_RADIUS * sinf(t);
}

/* same geometry and base sequence as dnaren.c */
static void draw_helix(const Palette *p) {
    int steps = (int)(HELIX_TURNS * STEPS_PER_TURN);
    unsigned seed = SEED;
    float a[3], b[3], c[3], d[3], e[3], f[3];
    int i, k, n, bonds, s;

    for (s = 0; s < 2; s++)
        for (i = 0; i <= steps; i++) {
            helix_pt(a, i, s ? GROOVE : 0.0f);
            splat(a, BACKBONE_RADIUS, s ? p->s2 : p->s1, 1);
        }

    for (i = 0; i <= steps; i += BASEPAIR_EVERY) {
        helix_pt(a, i, 0.0f);
        helix_pt(b, i, GROOVE);
        for (k = 0; k < 3; k++) {
            c[k] = a[k] + (b[k] - a[k]) * BASE_END;
            d[k] = b[k] + (a[k] - b[k]) * BASE_END;
        }
        seed = seed * 1103515245u + 12345u;
        n = (int)((seed >> 16) & 3);

        rod(a, c, BASE_RADIUS, p->base[n], 1);
        rod(b, d, BASE_RADIUS, p->base[n ^ 1], 1);
        splat(a, SPHERE_RADIUS, p->atom, 1);
        splat(b, SPHERE_RADIUS, p->atom, 1);

        bonds = 2 + (n >> 1);
        for (k = 0; k < bonds; k++) {
            float dy = ((float)k - (float)(bonds - 1) / 2.0f) * BOND_SPACING;
            memcpy(e, c, sizeof(e));
            memcpy(f, d, sizeof(f));
            e[1] += dy;
            f[1] += dy;
            rod(e, f, CLI_BOND_RADIUS, p->bp, 0);
        }
    }
}

static void render(void) {
    const Palette *p = &palettes[palette];
    size_t i, n = (size_t)pw * (size_t)ph;
    setup_view();
    for (i = 0; i < n; i++) {
        zb[i] = ZINF;
        fb[i * 3] = p->bg[0];
        fb[i * 3 + 1] = p->bg[1];
        fb[i * 3 + 2] = p->bg[2];
    }
    draw_helix(p);
}

/* ---- color quantization ---- */

static int to8(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return (int)(v * 255.0f + 0.5f);
}

static uint32_t q256(int r, int g, int b) {
    int ri, gi, bi, avg;
    if (abs(r - g) < 10 && abs(g - b) < 10) {
        avg = (r + g + b) / 3;
        if (avg < 4) return 16;
        if (avg > 246) return 231;
        return (uint32_t)(232 + avg * 24 / 256);
    }
    ri = r < 48 ? 0 : r < 115 ? 1 : (r - 35) / 40;
    gi = g < 48 ? 0 : g < 115 ? 1 : (g - 35) / 40;
    bi = b < 48 ? 0 : b < 115 ? 1 : (b - 35) / 40;
    return (uint32_t)(16 + 36 * ri + 6 * gi + bi);
}

static uint32_t q16(int r, int g, int b) {
    int i, best = 0, bd = 1 << 30;
    for (i = 0; i < 16; i++) {
        int dr = r - pal16[i][0], dg = g - pal16[i][1], db = b - pal16[i][2];
        int dd = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
        if (dd < bd) { bd = dd; best = i; }
    }
    return (uint32_t)best;
}

static uint32_t pixel_code(size_t i) {
    const float *c = fb + i * 3;
    int r = to8(c[0]), g = to8(c[1]), b = to8(c[2]);
    switch (colmode) {
    case COL_TRUE: return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    case COL_256: return q256(r, g, b);
    case COL_16: return q16(r, g, b);
    default: {
        float l;
        if (zb[i] >= ZINF) return 0;
        l = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
        if (l > 1.0f) l = 1.0f;
        return (uint32_t)(1 + (int)(l * 8.99f));
    }
    }
}

static void to_cells(void) {
    int x, y;
    for (y = 0; y < gh; y++)
        for (x = 0; x < gw; x++) {
            Cell *c = &cell[(size_t)y * (size_t)gw + (size_t)x];
            if (half()) {
                c->a = pixel_code((size_t)(2 * y) * (size_t)pw + (size_t)x);
                c->b = pixel_code((size_t)(2 * y + 1) * (size_t)pw + (size_t)x);
            } else {
                c->a = pixel_code((size_t)y * (size_t)pw + (size_t)x);
                c->b = 0;
            }
        }
}

/* ---- output: only changed cells are sent ---- */

static char *pu(char *p, unsigned n) {
    char t[12];
    int i = 0;
    do { t[i++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (i) *p++ = t[--i];
    return p;
}

static char *pcol(char *p, int fg, uint32_t c) {
    *p++ = '\x1b';
    *p++ = '[';
    if (colmode == COL_TRUE) {
        p = pu(p, fg ? 38 : 48);
        PUTS(p, ";2;");
        p = pu(p, (c >> 16) & 255);
        *p++ = ';';
        p = pu(p, (c >> 8) & 255);
        *p++ = ';';
        p = pu(p, c & 255);
    } else if (colmode == COL_256) {
        p = pu(p, fg ? 38 : 48);
        PUTS(p, ";5;");
        p = pu(p, c);
    } else if (c < 8) {
        p = pu(p, (fg ? 30u : 40u) + c);
    } else {
        p = pu(p, (fg ? 90u : 100u) + c - 8u);
    }
    *p++ = 'm';
    return p;
}

static void status(char *buf, size_t n) {
    snprintf(buf, n, " dnaren | speed %.1f | %s | palette %d | %s | %s | %s | %.0f fps | %dx%d ",
             speed, paused ? "paused" : "running", palette + 1, colname[colmode],
             half() ? "half" : "2sp", horiz ? "horiz" : "vert",
             1.0 / frame_s, pw, ph);
}

static void present(void) {
    char *p = ob;
    char hud[512];
    int x, y, crow = -1, ccol = -1, full = redraw;
    int hf = half(), mono = colmode == COL_MONO;
    uint32_t cfg = NONE, cbg = NONE;
    size_t nc = (size_t)gw * (size_t)gh, i;

    PUTS(p, "\x1b[?2026h");
    if (redraw) {
        PUTS(p, "\x1b[0m\x1b[H\x1b[2J");
        for (i = 0; i < nc; i++) { prev[i].a = NONE; prev[i].b = NONE; }
        hudprev[0] = '\0';
        redraw = 0;
    }
    for (y = 0; y < gh; y++)
        for (x = 0; x < gw; x++) {
            Cell *c = &cell[(size_t)y * (size_t)gw + (size_t)x];
            Cell *o = &prev[(size_t)y * (size_t)gw + (size_t)x];
            int sx = hf ? x : 2 * x;
            if (c->a == o->a && c->b == o->b) continue;
            *o = *c;
            if (crow != y || ccol != sx) {
                *p++ = '\x1b';
                *p++ = '[';
                p = pu(p, (unsigned)(y + 1));
                *p++ = ';';
                p = pu(p, (unsigned)(sx + 1));
                *p++ = 'H';
            }
            if (hf) {
                if (c->a != cfg) { p = pcol(p, 1, c->a); cfg = c->a; }
                if (c->b != cbg) { p = pcol(p, 0, c->b); cbg = c->b; }
                PUTS(p, "\xe2\x96\x80");   /* upper half block */
                ccol = sx + 1;
            } else if (mono) {
                *p++ = ramp[c->a];
                if (sx + 1 < cols) *p++ = ramp[c->a];
                ccol = sx + 2;
            } else {
                if (c->a != cbg) { p = pcol(p, 0, c->a); cbg = c->a; }
                *p++ = ' ';
                if (sx + 1 < cols) *p++ = ' ';
                ccol = sx + 2;
            }
            crow = y;
            if (ccol >= cols) ccol = -1;
        }
    if (gh < rows) {
        status(hud, sizeof(hud));
        if (full || strcmp(hud, hudprev) != 0) {
            size_t len = strlen(hud);
            int j;
            strcpy(hudprev, hud);
            if (len > (size_t)cols) len = (size_t)cols;
            *p++ = '\x1b';
            *p++ = '[';
            p = pu(p, (unsigned)rows);
            PUTS(p, ";1H\x1b[0;7m");
            memcpy(p, hud, len);
            p += len;
            for (j = (int)len; j < cols; j++) *p++ = ' ';
            PUTS(p, "\x1b[0m");
        }
    }
    PUTS(p, "\x1b[?2026l");
    write_all(ob, (size_t)(p - ob));
}

/* ---- input ---- */

enum { K_UP = 256, K_DOWN, K_RIGHT, K_LEFT };

static void zoom(float d) {
    cam_dist += d;
    if (cam_dist < CAM_DISTANCE_MIN) cam_dist = CAM_DISTANCE_MIN;
    if (cam_dist > CAM_DISTANCE_MAX) cam_dist = CAM_DISTANCE_MAX;
}

static void key(int k) {
    switch (k) {
    case 27: case 'q': case 3: running = 0; break;
    case '+': case '=':
        speed += SPEED_STEP; if (speed > SPEED_MAX) speed = SPEED_MAX; break;
    case '-':
        speed -= SPEED_STEP; if (speed < SPEED_MIN) speed = SPEED_MIN; break;
    case '0': speed = SPEED_DEFAULT; break;
    case ' ': paused = !paused; break;
    case 'r': cam_rot_x = 20.0f; cam_rot_y = 0.0f; cam_dist = CAM_DISTANCE_DEFAULT; break;
    case 'c': palette = (palette + 1) % NUM_PALETTES; break;
    case K_LEFT: case 'a': cam_rot_y -= CAM_ROTATE_STEP; break;
    case K_RIGHT: case 'd': cam_rot_y += CAM_ROTATE_STEP; break;
    case K_UP: case 'w': cam_rot_x -= CAM_ROTATE_STEP; break;
    case K_DOWN: case 's': cam_rot_x += CAM_ROTATE_STEP; break;
    case 'z': zoom(-CAM_DISTANCE_STEP); break;
    case 'x': zoom(CAM_DISTANCE_STEP); break;
    case 'm': colmode = (colmode + 1) % NUM_COL; layout(); break;
    case 'p': pxmode = !pxmode; layout(); break;
    case 'o': orient = (orient + 1) % 3; break;
    case 'h': show_hud = !show_hud; layout(); break;
    default: return;
    }
    dirty = 1;
}

/* SGR mouse: b = button/flags, x y = 1-based cell */
static void mouse(int b, int x, int y, int press) {
    int wheel = b & 64, motion = b & 32, btn = b & 3;
    if (wheel) {
        if (press && btn == 0) { zoom(-CAM_DISTANCE_STEP); dirty = 1; }
        else if (press && btn == 1) { zoom(CAM_DISTANCE_STEP); dirty = 1; }
        return;
    }
    if (btn != 0) return;
    if (!press) { mouse_down = 0; dirty = 1; return; }
    if (motion && mouse_down) {
        cam_rot_y += (float)(x - last_mx) * CLI_MOUSE_SENS;
        cam_rot_x += (float)(y - last_my) * CLI_MOUSE_SENS * 2.0f;
        dirty = 1;
    } else {
        mouse_down = 1;
    }
    last_mx = x;
    last_my = y;
}

static void csi(int intro, const unsigned char *par, int n, int fin) {
    if (intro == '[' && n > 0 && par[0] == '<' && n < 32) {
        char tmp[32];
        int b, x, y;
        memcpy(tmp, par + 1, (size_t)n - 1);
        tmp[n - 1] = '\0';
        if (sscanf(tmp, "%d;%d;%d", &b, &x, &y) == 3 && (fin == 'M' || fin == 'm'))
            mouse(b, x, y, fin == 'M');
        return;
    }
    switch (fin) {
    case 'A': key(K_UP); break;
    case 'B': key(K_DOWN); break;
    case 'C': key(K_RIGHT); break;
    case 'D': key(K_LEFT); break;
    }
}

static void read_input(void) {
    ssize_t r;
    int i = 0;
    if (in_n >= (int)sizeof(ib)) in_n = 0;
    r = read(STDIN_FILENO, ib + in_n, sizeof(ib) - (size_t)in_n);
    if (r == 0) { running = 0; return; }
    if (r < 0) {
        if (errno != EINTR && errno != EAGAIN) running = 0;
        return;
    }
    in_n += (int)r;
    while (i < in_n) {
        int c = ib[i];
        if (c != 0x1b) { key(c); i++; continue; }
        if (i + 1 >= in_n) { key(27); i++; continue; }
        c = ib[i + 1];
        if (c == '[' || c == 'O') {
            int j = i + 2;
            while (j < in_n && ib[j] >= 0x20 && ib[j] <= 0x3f) j++;
            if (j >= in_n) break;       /* incomplete, wait for the rest */
            csi(c, ib + i + 2, j - (i + 2), ib[j]);
            i = j + 1;
            continue;
        }
        i += 2;
    }
    memmove(ib, ib + i, (size_t)(in_n - i));
    in_n -= i;
}

/* sleep until deadline, handling input meanwhile */
static void wait_input(double deadline) {
    struct pollfd pf;
    pf.fd = STDIN_FILENO;
    pf.events = POLLIN;
    while (running && !resized) {
        double rem = deadline - now_sec();
        if (rem <= 0.0) break;
        pf.revents = 0;
        if (poll(&pf, 1, (int)(rem * 1000.0) + 1) > 0)
            read_input();
    }
}

int main(int argc, char **argv) {
    double last, ema = 0.0;
    const double base = CLI_FRAME_MS / 1000.0;
    const double slow = CLI_FRAME_MAX_MS / 1000.0;

    if (argc > 1 && (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0)) {
        printf("dnaren-cli %s\n", VERSION);
        return 0;
    }
    if (argc > 1) {
        fputs("usage: dnaren-cli [-v]\n", stderr);
        return 1;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("dnaren-cli: stdin and stdout must be a terminal\n", stderr);
        return 1;
    }

    light_init();
    colmode = detect_color();
    term_init();
    last = now_sec();

    while (running) {
        double t0, t1, dt;
        int rendered = 0, full;
        if (resized) {
            resized = 0;
            term_size();
            layout();
        }
        t0 = now_sec();
        dt = t0 - last;
        last = t0;
        if (dt > 0.25) dt = 0.25;
        if (!paused && !mouse_down) {
            angle = fmodf(angle + speed * (float)dt * 60.0f, 360.0f);
            dirty = 1;
        }
        full = redraw;
        if (dirty) {
            dirty = 0;
            render();
            to_cells();
            present();
            rendered = 1;
        }
        t1 = now_sec();
        /* adaptive pace: keep frame cost under ~70% of the interval */
        if (rendered && !full) {
            double want;
            ema = ema > 0.0 ? ema * 0.9 + (t1 - t0) * 0.1 : (t1 - t0);
            want = ema / 0.7;
            if (want < base) want = base;
            if (want > slow) want = slow;
            frame_s += (want - frame_s) * 0.1;
        }
        wait_input(t0 + frame_s);
    }

    term_done();
    return 0;
}
