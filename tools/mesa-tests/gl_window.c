// gl-window: OpenGL ES in a compositor window through Mesa's InstantOS EGL
// platform (step 7c-2b). EGL_DEFAULT_DISPLAY, a window config with depth, a
// GLES2 context and eglCreateWindowSurface on a struct instantos_egl_window;
// a spinning cube with a colour per face, presented with eglSwapBuffers.
// The window follows resizes; Escape or the close button ends it.
//
//   gl-window [frames]    0 or nothing: until closed
//
// Prints checks (the first frame read back before its swap), then the frame
// rate every 2 s and at the end.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <instantos/window.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;

static void check(int ok, const char* what) {
    printf("gl-window: %s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    if (!ok) {
        failures++;
    }
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static const char* kVertex =
    "attribute vec3 position;\n"
    "attribute vec3 colour;\n"
    "uniform mat4 transform;\n"
    "varying vec3 shade;\n"
    "void main() {\n"
    "    gl_Position = transform * vec4(position, 1.0);\n"
    "    shade = colour;\n"
    "}\n";

static const char* kFragment =
    "precision mediump float;\n"
    "varying vec3 shade;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(shade, 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        printf("gl-window: shader: %s\n", log);
    }
    return shader;
}

// A cube of 6 faces x 2 triangles, a colour per face.
static void cube(float* vertices, float* colours) {
    static const float corners[8][3] = {
        {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1},
    };
    static const int faces[6][4] = {
        {0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7}, {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0},
    };
    static const float faceColours[6][3] = {
        {0.22f, 0.55f, 0.82f}, {0.42f, 0.71f, 0.93f}, {0.49f, 0.88f, 0.63f},
        {0.90f, 0.28f, 0.30f}, {0.95f, 0.78f, 0.30f}, {0.67f, 0.71f, 0.75f},
    };
    static const int order[6] = {0, 1, 2, 0, 2, 3};
    int n = 0;
    for (int f = 0; f < 6; f++) {
        for (int k = 0; k < 6; k++) {
            memcpy(vertices + n * 3, corners[faces[f][order[k]]], sizeof(float) * 3);
            memcpy(colours + n * 3, faceColours[f], sizeof(float) * 3);
            n++;
        }
    }
}

static void multiply(float* out, const float* a, const float* b) {
    float r[16];
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            r[c * 4 + row] = 0;
            for (int k = 0; k < 4; k++) {
                r[c * 4 + row] += a[k * 4 + row] * b[c * 4 + k];
            }
        }
    }
    memcpy(out, r, sizeof(r));
}

// Perspective * translate(0,0,-6) * rotate(y) * rotate(x), column-major.
static void transform(float* out, float t, float aspect) {
    const float f = 1.0f / tanf(0.5f * 0.9f);
    const float nearZ = 1.0f, farZ = 20.0f;
    float projection[16] = {f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (farZ + nearZ) / (nearZ - farZ), -1,
                            0, 0, 2 * farZ * nearZ / (nearZ - farZ), 0};
    float move[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -6, 1};
    const float a = t * 0.9f, b = t * 0.6f;
    float spinY[16] = {cosf(a), 0, -sinf(a), 0, 0, 1, 0, 0, sinf(a), 0, cosf(a), 0, 0, 0, 0, 1};
    float spinX[16] = {1, 0, 0, 0, 0, cosf(b), sinf(b), 0, 0, -sinf(b), cosf(b), 0, 0, 0, 0, 1};
    float m[16];
    multiply(m, spinY, spinX);
    multiply(m, move, m);
    multiply(out, projection, m);
}

int main(int argc, char** argv) {
    long frames = argc > 1 ? strtol(argv[1], NULL, 10) : 0;
    // Diagnostics: "readback" reads two pixels back from frames 1-6 and every
    // 50th; "finish" calls glFinish before every swap.
    const int readback = argc > 2 && strcmp(argv[2], "readback") == 0;
    const int finish = argc > 2 && strcmp(argv[2], "finish") == 0;
    int width = 640, height = 480;

    struct instantos_window window;
    check(instantos_window_create(&window, width, height, "GL cube") == 0, "a compositor window");
    if (failures) {
        return 1;
    }
    struct instantos_egl_window native = {window.window, width, height};

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    check(display != EGL_NO_DISPLAY && eglInitialize(display, &major, &minor), "eglInitialize (the InstantOS platform)");
    if (failures) {
        printf("gl-window: EGL error 0x%x\n", eglGetError());
        return 1;
    }
    printf("gl-window: EGL %d.%d, %s\n", major, minor, eglQueryString(display, EGL_VENDOR));
    const EGLint configAttribs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 16,
                                    EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    check(eglChooseConfig(display, configAttribs, &config, 1, &count) && count == 1, "a window config with depth");
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    check(context != EGL_NO_CONTEXT, "a GLES2 context");
    EGLSurface surface = eglCreateWindowSurface(display, config, (EGLNativeWindowType)(uintptr_t)&native, NULL);
    check(surface != EGL_NO_SURFACE, "eglCreateWindowSurface on the window");
    check(eglMakeCurrent(display, surface, surface, context), "eglMakeCurrent");
    if (failures) {
        printf("gl-window: EGL error 0x%x\n", eglGetError());
        return 1;
    }
    printf("gl-window: GL_RENDERER %s\n", (const char*)glGetString(GL_RENDERER));

    GLuint program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER, kVertex));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER, kFragment));
    glBindAttribLocation(program, 0, "position");
    glBindAttribLocation(program, 1, "colour");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    check(linked, "the shaders link");
    glUseProgram(program);
    const GLint transformAt = glGetUniformLocation(program, "transform");
    static float vertices[36 * 3], colours[36 * 3];
    cube(vertices, colours);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, vertices);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, colours);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnable(GL_DEPTH_TEST);

    const double start = now_s();
    double lastReport = start;
    long drawn = 0, sinceReport = 0;
    int running = 1;
    while (running && (frames <= 0 || drawn < frames)) {
        struct instantos_event event;
        while (instantos_window_next_event(&window, &event, 0)) {
            if (event.type == INSTANTOS_EVENT_WINDOW && event.window.action == INSTANTOS_WINDOW_CLOSE_REQUESTED) {
                running = 0;
            } else if (event.type == INSTANTOS_EVENT_WINDOW && event.window.action == INSTANTOS_WINDOW_RESIZED &&
                       event.window.width > 0 && event.window.height > 0) {
                native.width = event.window.width;   // the next frame is drawn at this size
                native.height = event.window.height;
            } else if (event.type == INSTANTOS_EVENT_KEY && event.key.action == INSTANTOS_KEY_PRESS &&
                       event.key.keycode == INSTANTOS_KEY_ESCAPE) {
                running = 0;
            }
        }
        if (!running) {
            break;
        }
        EGLint w = native.width, h = native.height;
        glViewport(0, 0, w, h);
        glClearColor(0.086f, 0.114f, 0.157f, 1.0f);   // the window colour, #161d28
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        float m[16];
        transform(m, (float)(now_s() - start), (float)w / (float)h);
        glUniformMatrix4fv(transformAt, 1, GL_FALSE, m);
        glDrawArrays(GL_TRIANGLES, 0, 36);
        if (drawn == 0) {
            unsigned char corner[4] = {0}, middle[4] = {0};
            glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
            glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, middle);
            printf("gl-window: first frame: corner %d,%d,%d middle %d,%d,%d\n", corner[0], corner[1], corner[2],
                   middle[0], middle[1], middle[2]);
            check(abs(corner[0] - 22) <= 2 && abs(corner[1] - 29) <= 2 && abs(corner[2] - 40) <= 2,
                  "the first frame's corner is the clear colour");
            check(!(abs(middle[0] - 22) <= 2 && abs(middle[1] - 29) <= 2 && abs(middle[2] - 40) <= 2),
                  "and the cube covers its middle");
            check(glGetError() == GL_NO_ERROR, "no GL error");
        }
        if (readback && drawn > 0 && (drawn <= 6 || drawn % 50 == 0)) {
            unsigned char corner[4] = {0}, middle[4] = {0}, edge[4] = {0};
            glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
            glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, middle);
            glReadPixels(w / 2 + 60, h / 2 + 40, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, edge);
            printf("gl-window: frame %ld: corner %d,%d,%d middle %d,%d,%d off-centre %d,%d,%d\n", drawn, corner[0],
                   corner[1], corner[2], middle[0], middle[1], middle[2], edge[0], edge[1], edge[2]);
        }
        if (finish) {
            glFinish();
        }
        if (!eglSwapBuffers(display, surface)) {
            printf("gl-window: eglSwapBuffers failed, EGL error 0x%x\n", eglGetError());
            failures++;
            break;
        }
        if (drawn == 0) {
            check(1, "eglSwapBuffers shows the first frame");
        }
        drawn++;
        sinceReport++;
        const double t = now_s();
        if (t - lastReport >= 2.0) {
            printf("gl-window: %.1f frames per second at %dx%d\n", sinceReport / (t - lastReport), w, h);
            fflush(stdout);
            lastReport = t;
            sinceReport = 0;
        }
    }
    const double elapsed = now_s() - start;
    printf("gl-window: %ld frames in %.2f s (%.1f per second)\n", drawn, elapsed, elapsed > 0 ? drawn / elapsed : 0.0);
    EGLint age = -1;
    eglQuerySurface(display, surface, EGL_BUFFER_AGE_EXT, &age);
    printf("gl-window: buffer age %d\n", age);

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    instantos_window_destroy(&window);
    printf(failures == 0 ? "gl-window: all checks passed\n" : "gl-window: some checks FAILED\n");
    return failures == 0 ? 0 : 1;
}
