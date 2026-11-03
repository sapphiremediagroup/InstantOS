// gl-offscreen: OpenGL ES 2 through Mesa in InstantOS, without a window
// system: EGL's surfaceless platform, a context with no surface, a 64x64
// framebuffer object, a clear and a shaded triangle read back with
// glReadPixels. On the HD 530 that is iris; `gl-offscreen software` asks for
// softpipe (Mesa's CPU renderer), which is what QEMU can run.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

static int failures;

static void check(int ok, const char *what) {
    printf("gl-offscreen: %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static GLuint compile(GLenum kind, const char *source) {
    GLuint shader = glCreateShader(kind);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        printf("gl-offscreen: shader log: %s\n", log);
    }
    return ok ? shader : 0;
}

static int pixel_is(const unsigned char *rgba, int r, int g, int b) {
    return abs(rgba[0] - r) <= 2 && abs(rgba[1] - g) <= 2 && abs(rgba[2] - b) <= 2 && rgba[3] == 255;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "software") == 0) {
        setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
        setenv("GALLIUM_DRIVER", "softpipe", 1);
    }
    double t0 = now_ms();
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    check(getPlatformDisplay != NULL, "eglGetPlatformDisplayEXT");
    if (!getPlatformDisplay) {
        return 1;
    }
    EGLDisplay display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint major = 0, minor = 0;
    const int initialized = display != EGL_NO_DISPLAY && eglInitialize(display, &major, &minor);
    printf("gl-offscreen: eglInitialize %s after %.1f ms (EGL %d.%d), error 0x%x\n", initialized ? "ok" : "failed",
           now_ms() - t0, major, minor, eglGetError());
    check(initialized, "eglInitialize on the surfaceless platform");
    if (!initialized) {
        printf("gl-offscreen: some checks FAILED\n");
        return 1;
    }
    printf("gl-offscreen: EGL vendor %s, client APIs %s\n", eglQueryString(display, EGL_VENDOR),
           eglQueryString(display, EGL_CLIENT_APIS));

    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, contextAttribs);
    check(context != EGL_NO_CONTEXT, "eglCreateContext without a config (GLES 2)");
    check(context != EGL_NO_CONTEXT && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context),
          "eglMakeCurrent with no surface");
    if (context == EGL_NO_CONTEXT) {
        printf("gl-offscreen: some checks FAILED\n");
        return 1;
    }
    printf("gl-offscreen: GL_RENDERER %s\n", glGetString(GL_RENDERER));
    printf("gl-offscreen: GL_VERSION %s\n", glGetString(GL_VERSION));

    // A 64x64 RGBA8 framebuffer object.
    GLuint fbo = 0, rb = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, 64, 64);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "a complete 64x64 RGBA8 framebuffer");
    glViewport(0, 0, 64, 64);

    unsigned char pixels[64 * 64 * 4];
    double t1 = now_ms();
    glClearColor(0.2f, 0.4f, 0.8f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    printf("gl-offscreen: clear and read back in %.2f ms\n", now_ms() - t1);
    check(pixel_is(pixels, 51, 102, 204) && pixel_is(pixels + (63 * 64 + 63) * 4, 51, 102, 204),
          "glClear: every corner is the clear colour (51,102,204)");

    // A triangle through a vertex and a fragment shader.
    double t2 = now_ms();
    GLuint vs = compile(GL_VERTEX_SHADER, "attribute vec2 pos;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); }\n");
    GLuint fs = compile(GL_FRAGMENT_SHADER,
                        "precision mediump float;\nuniform vec4 color;\nvoid main() { gl_FragColor = color; }\n");
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "pos");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    printf("gl-offscreen: shaders compiled and linked in %.1f ms\n", now_ms() - t2);
    check(vs && fs && linked, "a vertex and a fragment shader compile and link");
    glUseProgram(program);
    glUniform4f(glGetUniformLocation(program, "color"), 1.0f, 0.5f, 0.0f, 1.0f);
    const GLfloat triangle[] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, triangle);
    glEnableVertexAttribArray(0);
    double t3 = now_ms();
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    printf("gl-offscreen: triangle drawn and read back in %.2f ms\n", now_ms() - t3);
    check(pixel_is(pixels + (32 * 64 + 32) * 4, 255, 128, 0), "the triangle's middle is its colour (255,128,0)");
    check(pixel_is(pixels + (2 * 64 + 2) * 4, 51, 102, 204) && pixel_is(pixels + (60 * 64 + 2) * 4, 51, 102, 204),
          "outside it the clear colour stays");
    check(glGetError() == GL_NO_ERROR, "no GL error");

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglTerminate(display);
    printf(failures == 0 ? "gl-offscreen: all checks passed\n" : "gl-offscreen: some checks FAILED\n");
    return failures == 0 ? 0 : 1;
}
