/* dnaren is minimal DNA renderer */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glx.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "dnaren.h"

static Display *dpy;
static Window win;
static GLXContext glc;
static int running = 1;
static int width = WIDTH;
static int height = HEIGHT;

static float angle = 0.0f;
static float speed = SPEED_DEFAULT;
static int paused = 0;
static float cam_dist = CAM_DISTANCE_DEFAULT;
static float cam_rot_x = 20.0f;
static float cam_rot_y = 0.0f;

static int mouse_down = 0;
static int last_mx, last_my;

static int fullscreen = 0;

#define NUM_PALETTES 3
typedef struct { float bg[3]; float s1[3]; float s2[3]; float bp[3]; float atom[3]; float base[4][3]; } Palette;
static Palette palettes[NUM_PALETTES] = {
    { {0.00f,0.00f,0.00f}, {0.20f,0.80f,1.00f}, {1.00f,0.40f,0.70f}, {0.85f,0.85f,0.90f}, {1.00f,1.00f,0.30f},
      { {0.30f,0.85f,0.40f}, {0.95f,0.30f,0.30f}, {1.00f,0.65f,0.15f}, {0.35f,0.45f,1.00f} } },
    { {0.00f,0.00f,0.00f}, {0.00f,1.00f,0.00f}, {1.00f,0.00f,0.00f}, {0.80f,0.80f,0.80f}, {1.00f,1.00f,1.00f},
      { {1.00f,0.85f,0.10f}, {0.10f,0.80f,0.90f}, {1.00f,0.50f,0.00f}, {0.60f,0.35f,1.00f} } },
    { {0.00f,0.00f,0.00f}, {1.00f,1.00f,1.00f}, {0.70f,0.70f,0.70f}, {0.70f,0.70f,0.70f}, {0.20f,0.60f,1.00f},
      { {0.85f,0.85f,0.85f}, {0.55f,0.55f,0.55f}, {0.20f,0.60f,1.00f}, {0.10f,0.35f,0.70f} } },
};
static int palette = 0;

static const GLfloat light_pos[4] = { -0.4f, 0.6f, 1.0f, 0.0f };

static GLUquadric *quad;

static void die(const char *s) { perror(s); exit(1); }

static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static void update_title(void) {
    char buf[128];
    snprintf(buf, sizeof(buf), "dnaren | speed %.1f | %s | palette %d",
             speed, paused ? "paused" : "running", palette + 1);
    XStoreName(dpy, win, buf);
}

static void toggle_fullscreen(void) {
    Atom wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom fs = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = wm_state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = fullscreen ? 0 : 1; /* 0 remove, 1 add */
    ev.xclient.data.l[1] = (long)fs;
    ev.xclient.data.l[2] = 0;
    XSendEvent(dpy, DefaultRootWindow(dpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    fullscreen = !fullscreen;
}

static void init_gl(void) {
    Palette *p = &palettes[palette];
    GLfloat amb[4] = { 0.30f, 0.30f, 0.30f, 1.0f };
    GLfloat spec[4] = { 0.50f, 0.50f, 0.50f, 1.0f };
    glClearColor(p->bg[0], p->bg[1], p->bg[2], 1.0f);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    glLineWidth(1.5f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spec);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 48.0f);
    quad = gluNewQuadric();
    gluQuadricNormals(quad, GLU_SMOOTH);
}

static void resize(int w, int h) {
    if (h == 0) h = 1;
    width = w; height = h;
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, (double)w / (double)h, 1.0, 100.0);
    glMatrixMode(GL_MODELVIEW);
}

static void helix_pt(float *v, int i, float off) {
    float t = (float)i / STEPS_PER_TURN * 2.0f * (float)M_PI + off;
    v[0] = HELIX_RADIUS * cosf(t);
    v[1] = HELIX_PITCH * (float)i / STEPS_PER_TURN;
    v[2] = -HELIX_RADIUS * sinf(t);
}

static void rod(const float *a, const float *b, float rad) {
    float dx = b[0] - a[0];
    float dy = b[1] - a[1];
    float dz = b[2] - a[2];
    float h = sqrtf(dx * dx + dz * dz);
    glPushMatrix();
    glTranslatef(a[0], a[1], a[2]);
    glRotatef((float)(atan2(dx, dz) * 180.0 / M_PI), 0.0f, 1.0f, 0.0f);
    glRotatef(-(float)(atan2(dy, h) * 180.0 / M_PI), 1.0f, 0.0f, 0.0f);
    gluCylinder(quad, rad, rad, sqrtf(h * h + dy * dy), ROD_SLICES, 1);
    glPopMatrix();
}

static void ball(const float *v, float rad) {
    glPushMatrix();
    glTranslatef(v[0], v[1], v[2]);
    gluSphere(quad, rad, SPHERE_SLICES, SPHERE_STACKS);
    glPopMatrix();
}

static void tube_vtx(int i, float off, float phi) {
    float t = (float)i / STEPS_PER_TURN * 2.0f * (float)M_PI + off;
    float c = HELIX_PITCH / (2.0f * (float)M_PI);
    float l = sqrtf(HELIX_RADIUS * HELIX_RADIUS + c * c);
    float nx = -cosf(t), nz = sinf(t);
    float bx = c * sinf(t) / l, by = HELIX_RADIUS / l, bz = c * cosf(t) / l;
    float cp = cosf(phi), sp = sinf(phi);
    float x = cp * nx + sp * bx;
    float y = sp * by;
    float z = cp * nz + sp * bz;
    glNormal3f(x, y, z);
    glVertex3f(HELIX_RADIUS * cosf(t) + BACKBONE_RADIUS * x,
               HELIX_PITCH * (float)i / STEPS_PER_TURN + BACKBONE_RADIUS * y,
               -HELIX_RADIUS * sinf(t) + BACKBONE_RADIUS * z);
}

static void tube(float off) {
    int steps = (int)(HELIX_TURNS * STEPS_PER_TURN);
    int i, j;
    for (i = 0; i < steps; i++) {
        glBegin(GL_QUAD_STRIP);
        for (j = 0; j <= ROD_SLICES; j++) {
            float phi = (float)j / ROD_SLICES * 2.0f * (float)M_PI;
            tube_vtx(i + 1, off, phi);
            tube_vtx(i, off, phi);
        }
        glEnd();
    }
}

#define GROOVE (GROOVE_ANGLE / 180.0f * (float)M_PI)
#define BASE_END 0.35f
#define DEG(x) ((x) * 180.0f / (float)M_PI)

static void draw_helix(void) {
    Palette *p = &palettes[palette];
    int steps = (int)(HELIX_TURNS * STEPS_PER_TURN);
    unsigned seed = SEED;
    float a[3], b[3], c[3], d[3];
    int i, k, n, bonds;

    glColor3fv(p->s1);
    tube(0.0f);
    glColor3fv(p->s2);
    tube(GROOVE);

    for (i = 0; i <= steps; i += BASEPAIR_EVERY) {
        helix_pt(a, i, 0.0f);
        helix_pt(b, i, GROOVE);
        for (k = 0; k < 3; k++) {
            c[k] = a[k] + (b[k] - a[k]) * BASE_END;
            d[k] = b[k] + (a[k] - b[k]) * BASE_END;
        }
        seed = seed * 1103515245u + 12345u;
        n = (int)((seed >> 16) & 3);

        glColor3fv(p->base[n]);
        rod(a, c, BASE_RADIUS);
        glColor3fv(p->base[n ^ 1]);
        rod(b, d, BASE_RADIUS);

        glColor3fv(p->atom);
        ball(a, SPHERE_RADIUS);
        ball(b, SPHERE_RADIUS);

        bonds = 2 + (n >> 1);
        glDisable(GL_LIGHTING);
        glColor3fv(p->bp);
        glBegin(GL_LINES);
        for (k = 0; k < bonds; k++) {
            float dy = ((float)k - (float)(bonds - 1) / 2.0f) * BOND_SPACING;
            glVertex3f(c[0], c[1] + dy, c[2]);
            glVertex3f(d[0], d[1] + dy, d[2]);
        }
        glEnd();
        glEnable(GL_LIGHTING);
    }
}

static void draw(void) {
    Palette *p = &palettes[palette];
    float total_height;
    glClearColor(p->bg[0], p->bg[1], p->bg[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
    glTranslatef(0.0f, 0.0f, -cam_dist);
    glRotatef(cam_rot_x, 1.0f, 0.0f, 0.0f);
    glRotatef(cam_rot_y, 0.0f, 1.0f, 0.0f);
    glRotatef(angle, 0.0f, 1.0f, 0.0f);
    total_height = HELIX_TURNS * HELIX_PITCH;
    glTranslatef(0.0f, -total_height / 2.0f, 0.0f);
    draw_helix();
    glXSwapBuffers(dpy, win);
    update_title();
}

static void zoom(float d) {
    cam_dist += d;
    if (cam_dist < CAM_DISTANCE_MIN) cam_dist = CAM_DISTANCE_MIN;
    if (cam_dist > CAM_DISTANCE_MAX) cam_dist = CAM_DISTANCE_MAX;
}

static void handle_key(XKeyEvent *ev) {
    KeySym ks = XLookupKeysym(ev, 0);
    switch (ks) {
        case XK_Escape: case XK_q: running = 0; break;
        case XK_plus: case XK_equal: case XK_KP_Add:
            speed += SPEED_STEP; if (speed > SPEED_MAX) speed = SPEED_MAX; break;
        case XK_minus: case XK_KP_Subtract:
            speed -= SPEED_STEP; if (speed < SPEED_MIN) speed = SPEED_MIN; break;
        case XK_0: case XK_KP_0: speed = SPEED_DEFAULT; break;
        case XK_space: paused = !paused; break;
        case XK_r: cam_rot_x = 20.0f; cam_rot_y = 0.0f; cam_dist = CAM_DISTANCE_DEFAULT; break;
        case XK_c: palette = (palette + 1) % NUM_PALETTES; break;
        case XK_Left:  cam_rot_y -= CAM_ROTATE_STEP; break;
        case XK_Right: cam_rot_y += CAM_ROTATE_STEP; break;
        case XK_Up:    cam_rot_x -= CAM_ROTATE_STEP; break;
        case XK_Down:  cam_rot_x += CAM_ROTATE_STEP; break;
        case XK_z: zoom(-CAM_DISTANCE_STEP); break;
        case XK_x: zoom(CAM_DISTANCE_STEP); break;
        case XK_a: cam_rot_y -= CAM_ROTATE_STEP; break;
        case XK_d: cam_rot_y += CAM_ROTATE_STEP; break;
        case XK_w: cam_rot_x -= CAM_ROTATE_STEP; break;
        case XK_s: cam_rot_x += CAM_ROTATE_STEP; break;
        case XK_f: case XK_F11: toggle_fullscreen(); break;
    }
}

static void handle_button(XButtonEvent *ev) {
    if (ev->button == Button1) { mouse_down = 1; last_mx = ev->x; last_my = ev->y; }
    else if (ev->button == Button4) zoom(-CAM_DISTANCE_STEP);
    else if (ev->button == Button5) zoom(CAM_DISTANCE_STEP);
}

static void handle_motion(XMotionEvent *ev) {
    if (mouse_down) {
        cam_rot_y += (ev->x - last_mx) * MOUSE_SENS;
        cam_rot_x += (ev->y - last_my) * MOUSE_SENS;
        last_mx = ev->x; last_my = ev->y;
    }
}

static void print_version(void) {
    printf("dnaren %s\n", VERSION);
}

int main(int argc, char **argv) {
    XVisualInfo *vi;
    Colormap cmap;
    XSetWindowAttributes swa;
    XEvent ev;
    int screen;
    Atom wm_delete_window;
    int msaa_attr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 24,
                        GLX_SAMPLE_BUFFERS, 1, GLX_SAMPLES, MSAA_SAMPLES, None };
    int plain_attr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 24, None };
    double last = now_sec();

    if (argc > 1 && (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0)) {
        print_version();
        return 0;
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) die("XOpenDisplay");

    screen = DefaultScreen(dpy);
    vi = glXChooseVisual(dpy, screen, msaa_attr);
    if (!vi) vi = glXChooseVisual(dpy, screen, plain_attr);
    if (!vi) die("glXChooseVisual");

    cmap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
    swa.colormap = cmap;
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | StructureNotifyMask;
    win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0, 0, width, height, 0,
                        vi->depth, InputOutput, vi->visual,
                        CWColormap | CWEventMask, &swa);
    XStoreName(dpy, win, TITLE);
    wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete_window, 1);
    XMapWindow(dpy, win);

    glc = glXCreateContext(dpy, vi, NULL, GL_TRUE);
    glXMakeCurrent(dpy, win, glc);

    init_gl();
    resize(width, height);
    update_title();

    while (running) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            switch (ev.type) {
                case Expose: draw(); break;
                case ConfigureNotify: resize(ev.xconfigure.width, ev.xconfigure.height); break;
                case KeyPress: handle_key(&ev.xkey); break;
                case ButtonPress: handle_button(&ev.xbutton); break;
                case ButtonRelease: mouse_down = 0; break;
                case MotionNotify: handle_motion(&ev.xmotion); break;
                case ClientMessage:
                    if ((Atom)ev.xclient.data.l[0] == wm_delete_window) running = 0;
                    break;
            }
        }
        double t = now_sec();
        double dt = t - last;
        last = t;
        if (!paused && !mouse_down) angle += speed * dt * 60.0f;
        draw();
        {
            struct timespec ts = { 0, FRAME_DELAY_MS * 1000000 };
            nanosleep(&ts, NULL);
        }
    }

    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, glc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}