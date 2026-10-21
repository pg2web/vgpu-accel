/*
 * gltest - OpenGL smoke test for the virgl path (vgpu.sys + Mesa virgl).
 *
 * Run it next to Mesa's opengl32.dll + vio_ogl_x64.dll (app-local) so Windows'
 * own opengl32.dll is bypassed. Renders into a hidden window and reads the
 * pixels back, so it works the same from the console or over RDP.
 *
 *   1. renderer identification (must be virgl, not llvmpipe/GDI Generic)
 *   2. glClear + fixed-function triangle, verified with glReadPixels
 *   3. GLSL program (vertex + fragment shader, uniform), verified with glReadPixels
 *   4. SwapBuffers (exercises the GDI present path)
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -static -o gltest.exe gltest.c -lopengl32 -lgdi32
 */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>

#define W 64
#define H 64

typedef char GLchar;
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER   0x8B31
#define GL_COMPILE_STATUS  0x8B81
#define GL_LINK_STATUS     0x8B82

typedef GLuint (APIENTRY *PFN_CreateShader)(GLenum);
typedef void (APIENTRY *PFN_ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void (APIENTRY *PFN_CompileShader)(GLuint);
typedef void (APIENTRY *PFN_GetShaderiv)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *PFN_GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef GLuint (APIENTRY *PFN_CreateProgram)(void);
typedef void (APIENTRY *PFN_AttachShader)(GLuint, GLuint);
typedef void (APIENTRY *PFN_LinkProgram)(GLuint);
typedef void (APIENTRY *PFN_GetProgramiv)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *PFN_UseProgram)(GLuint);
typedef GLint (APIENTRY *PFN_GetUniformLocation)(GLuint, const GLchar *);
typedef void (APIENTRY *PFN_Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);

static int failures;

#define CHECK(cond, ...) do { \
        int ok_ = (cond); \
        printf(ok_ ? "  PASS  " : "  FAIL  "); printf(__VA_ARGS__); printf("\n"); \
        if (!ok_) failures++; \
    } while (0)

static int pixel_is(int x, int y, int r, int g, int b)
{
    unsigned char p[4] = { 0 };
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    int ok = abs(p[0] - r) <= 2 && abs(p[1] - g) <= 2 && abs(p[2] - b) <= 2;
    if (!ok)
        printf("        pixel(%d,%d) = %d,%d,%d expected %d,%d,%d\n", x, y, p[0], p[1], p[2], r, g, b);
    return ok;
}

static void *get_proc(const char *name)
{
    void *p = (void *)wglGetProcAddress(name);
    if (!p)
        printf("  info  missing %s\n", name);
    return p;
}

static void test_shaders(void)
{
    PFN_CreateShader CreateShader = get_proc("glCreateShader");
    PFN_ShaderSource ShaderSource = get_proc("glShaderSource");
    PFN_CompileShader CompileShader = get_proc("glCompileShader");
    PFN_GetShaderiv GetShaderiv = get_proc("glGetShaderiv");
    PFN_GetShaderInfoLog GetShaderInfoLog = get_proc("glGetShaderInfoLog");
    PFN_CreateProgram CreateProgram = get_proc("glCreateProgram");
    PFN_AttachShader AttachShader = get_proc("glAttachShader");
    PFN_LinkProgram LinkProgram = get_proc("glLinkProgram");
    PFN_GetProgramiv GetProgramiv = get_proc("glGetProgramiv");
    PFN_UseProgram UseProgram = get_proc("glUseProgram");
    PFN_GetUniformLocation GetUniformLocation = get_proc("glGetUniformLocation");
    PFN_Uniform4f Uniform4f = get_proc("glUniform4f");

    if (!CreateShader || !ShaderSource || !CompileShader || !GetShaderiv || !GetShaderInfoLog || !CreateProgram ||
        !AttachShader || !LinkProgram || !GetProgramiv || !UseProgram || !GetUniformLocation || !Uniform4f) {
        CHECK(0, "GL 2.0 shader entry points available");
        return;
    }

    static const char *vs_src =
        "#version 110\n"
        "void main() { gl_Position = gl_Vertex; }\n";
    static const char *fs_src =
        "#version 110\n"
        "uniform vec4 color;\n"
        "void main() { gl_FragColor = color; }\n";

    GLint ok = 0;
    char log[1024];
    GLuint vs = CreateShader(GL_VERTEX_SHADER), fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(vs, 1, &vs_src, NULL);
    CompileShader(vs);
    GetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) { GetShaderInfoLog(vs, sizeof(log), NULL, log); printf("        %s\n", log); }
    CHECK(ok, "vertex shader compiles");
    ShaderSource(fs, 1, &fs_src, NULL);
    CompileShader(fs);
    GetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { GetShaderInfoLog(fs, sizeof(log), NULL, log); printf("        %s\n", log); }
    CHECK(ok, "fragment shader compiles");

    GLuint prog = CreateProgram();
    AttachShader(prog, vs);
    AttachShader(prog, fs);
    LinkProgram(prog);
    GetProgramiv(prog, GL_LINK_STATUS, &ok);
    CHECK(ok, "program links");
    if (!ok)
        return;

    UseProgram(prog);
    Uniform4f(GetUniformLocation(prog, "color"), 1.0f, 0.5f, 0.0f, 1.0f);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLE_STRIP);
    glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(-1, 1); glVertex2f(1, 1);
    glEnd();
    glFinish();
    CHECK(pixel_is(W / 2, H / 2, 255, 128, 0), "shader output read back from the host (orange)");
    UseProgram(0);
}

int main(void)
{
    printf("gltest: OpenGL over virgl smoke test\n\n");

    if (!GetModuleHandleA("opengl32.dll") && !LoadLibraryA("opengl32.dll"))
        printf("  info  could not load opengl32.dll\n");
    char path[MAX_PATH] = "";
    GetModuleFileNameA(GetModuleHandleA("opengl32.dll"), path, sizeof(path));
    printf("  info  opengl32.dll loaded from: %s\n", path);

    WNDCLASSA wc = { CS_OWNDC, DefWindowProcA, 0, 0, GetModuleHandleA(NULL), 0, 0, 0, 0, "gltest" };
    RegisterClassA(&wc);
    /* borderless popup: the client area (= framebuffer) is exactly W x H */
    HWND wnd = CreateWindowA("gltest", "gltest", WS_POPUP, 0, 0, W, H, 0, 0, wc.hInstance, 0);
    RECT cr;
    GetClientRect(wnd, &cr);
    printf("  info  framebuffer %ldx%ld\n", cr.right - cr.left, cr.bottom - cr.top);
    HDC dc = GetDC(wnd);

    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                  PFD_TYPE_RGBA, 32 };
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int fmt = ChoosePixelFormat(dc, &pfd);
    CHECK(fmt > 0 && SetPixelFormat(dc, fmt, &pfd), "pixel format %d", fmt);

    printf("\n[1] context\n");
    HGLRC rc = wglCreateContext(dc);
    CHECK(rc != NULL, "wglCreateContext (err=%lu)", rc ? 0 : GetLastError());
    if (!rc || !wglMakeCurrent(dc, rc)) {
        CHECK(0, "wglMakeCurrent");
        printf("\nFAILED: no context. Check DebugView for [vgpu] and 'vgpu:' messages.\n");
        return 1;
    }
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    printf("  info  GL_VENDOR   = %s\n  info  GL_RENDERER = %s\n  info  GL_VERSION  = %s\n",
           vendor, renderer, version);
    CHECK(renderer && strstr(renderer, "virgl") != NULL, "renderer is virgl (host GPU)");

    printf("\n[2] fixed function\n");
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    CHECK(pixel_is(1, 1, 0, 0, 255), "glClear blue read back");
    glBegin(GL_TRIANGLES);
    glColor3f(0, 1, 0);
    glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(0, 1);
    glEnd();
    glFinish();
    CHECK(pixel_is(W / 2, H / 4, 0, 255, 0), "triangle green at center");
    CHECK(pixel_is(1, H - 2, 0, 0, 255), "background still blue at corner");
    CHECK(glGetError() == GL_NO_ERROR, "no GL error");

    printf("\n[3] GLSL\n");
    test_shaders();
    CHECK(glGetError() == GL_NO_ERROR, "no GL error");

    printf("\n[4] present\n");
    CHECK(SwapBuffers(dc), "SwapBuffers");

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    ReleaseDC(wnd, dc);
    DestroyWindow(wnd);

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
