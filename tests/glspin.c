/*
 * glspin - visible OpenGL animation with an FPS counter, to check that virgl
 * frames reach the screen (locally or over RDP) and to measure presentation
 * throughput.
 *
 * Usage: glspin.exe [seconds] [width height]     (defaults: 15 800 600)
 *
 * Draws a spinning, lit cube plus a grid of quads every frame, prints the FPS
 * once per second (also in the title bar) and an average at the end. The
 * window is resizable; resizing changes the per-frame readback size.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -static -o glspin.exe glspin.c -lopengl32 -lgdi32
 */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int width = 800, height = 600;
static volatile int running = 1;

static LRESULT CALLBACK wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        width = LOWORD(lp) ? LOWORD(lp) : 1;
        height = HIWORD(lp) ? HIWORD(lp) : 1;
        return 0;
    case WM_CLOSE:
        running = 0;
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            running = 0;
        return 0;
    }
    return DefWindowProcA(wnd, msg, wp, lp);
}

static void perspective(double fovy, double aspect, double znear, double zfar)
{
    double f = znear * tan(fovy * 3.14159265358979 / 360.0);
    glFrustum(-f * aspect, f * aspect, -f, f, znear, zfar);
}

static void cube(void)
{
    static const float n[6][3] = { {0,0,1}, {0,0,-1}, {0,1,0}, {0,-1,0}, {1,0,0}, {-1,0,0} };
    static const float c[6][3] = { {1,.2f,.2f}, {.2f,1,.2f}, {.2f,.2f,1}, {1,1,.2f}, {1,.2f,1}, {.2f,1,1} };
    static const float v[6][4][3] = {
        {{-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1}}, {{ 1,-1,-1},{-1,-1,-1},{-1, 1,-1},{ 1, 1,-1}},
        {{-1, 1, 1},{ 1, 1, 1},{ 1, 1,-1},{-1, 1,-1}}, {{-1,-1,-1},{ 1,-1,-1},{ 1,-1, 1},{-1,-1, 1}},
        {{ 1,-1, 1},{ 1,-1,-1},{ 1, 1,-1},{ 1, 1, 1}}, {{-1,-1,-1},{-1,-1, 1},{-1, 1, 1},{-1, 1,-1}},
    };
    glBegin(GL_QUADS);
    for (int f = 0; f < 6; f++) {
        glNormal3fv(n[f]);
        glColor3fv(c[f]);
        for (int i = 0; i < 4; i++)
            glVertex3fv(v[f][i]);
    }
    glEnd();
}

int main(int argc, char **argv)
{
    double seconds = argc > 1 ? atof(argv[1]) : 15.0;
    if (argc > 3) {
        width = atoi(argv[2]);
        height = atoi(argv[3]);
    }

    WNDCLASSA wc = { CS_OWNDC, wndproc, 0, 0, GetModuleHandleA(NULL), 0, LoadCursor(NULL, IDC_ARROW), 0, 0, "glspin" };
    RegisterClassA(&wc);
    RECT r = { 0, 0, width, height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND wnd = CreateWindowA("glspin", "glspin", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, 0, 0, wc.hInstance, 0);
    HDC dc = GetDC(wnd);

    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                  PFD_TYPE_RGBA, 32 };
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) {
        printf("glspin: no OpenGL context (err=%lu)\n", GetLastError());
        return 1;
    }
    printf("glspin: %s | %s\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    printf("running %.0f s, Esc or close to stop early\n", seconds);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glEnable(GL_NORMALIZE);
    float light[4] = { 2, 3, 4, 0 };

    LARGE_INTEGER freq, t0, tlast, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    tlast = t0;
    long frames = 0, frames_window = 0;
    double min_fps = 1e9, max_fps = 0;

    while (running) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        QueryPerformanceCounter(&now);
        double t = (double)(now.QuadPart - t0.QuadPart) / freq.QuadPart;
        if (t >= seconds)
            break;

        glViewport(0, 0, width, height);
        glClearColor(0.1f, 0.1f, 0.15f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        perspective(45, (double)width / height, 1, 100);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glLightfv(GL_LIGHT0, GL_POSITION, light);
        glTranslatef(0, 0, -12);

        /* background grid of small spinning cubes: some geometry load per frame */
        for (int y = -3; y <= 3; y++)
            for (int x = -5; x <= 5; x++) {
                glPushMatrix();
                glTranslatef(x * 1.6f, y * 1.6f, -6);
                glRotatef((float)(t * 90 + x * 20 + y * 30), 1, 1, 0);
                glScalef(0.4f, 0.4f, 0.4f);
                cube();
                glPopMatrix();
            }

        glRotatef((float)(t * 60), 0, 1, 0);
        glRotatef((float)(t * 40), 1, 0, 0);
        glScalef(2, 2, 2);
        cube();

        SwapBuffers(dc);
        frames++;
        frames_window++;

        double dt = (double)(now.QuadPart - tlast.QuadPart) / freq.QuadPart;
        if (dt >= 1.0) {
            double fps = frames_window / dt;
            if (fps < min_fps) min_fps = fps;
            if (fps > max_fps) max_fps = fps;
            char title[128];
            snprintf(title, sizeof(title), "glspin - %.1f FPS - %dx%d", fps, width, height);
            SetWindowTextA(wnd, title);
            printf("  %5.1f FPS  (%dx%d)\n", fps, width, height);
            frames_window = 0;
            tlast = now;
        }
    }

    QueryPerformanceCounter(&now);
    double total = (double)(now.QuadPart - t0.QuadPart) / freq.QuadPart;
    GLenum err = glGetError();
    printf("\naverage %.1f FPS over %.1f s (%ld frames), min %.1f, max %.1f, glGetError=0x%x\n",
           frames / total, total, frames, min_fps < 1e9 ? min_fps : 0, max_fps, err);

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    ReleaseDC(wnd, dc);
    DestroyWindow(wnd);
    return 0;
}
