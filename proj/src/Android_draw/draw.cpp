#include "Android_draw/draw.h"
#include "verdana.h"

// Var
EGLDisplay display = EGL_NO_DISPLAY;
EGLConfig config;
EGLSurface surface = EGL_NO_SURFACE;
EGLContext context = EGL_NO_CONTEXT;

ANativeWindow *native_window;
ImFont* verdana;
//
int native_window_screen_x = 0;
int native_window_screen_y = 0;
android::ANativeWindowCreator::DisplayInfo displayInfo{0};
uint32_t orientation = 0;
bool g_Initialized = false;
ImGuiWindow *g_window = nullptr;

bool initGUI_draw(uint32_t _screen_x, uint32_t _screen_y, bool log) {
    orientation = displayInfo.orientation;

    #if defined(USE_OPENGL)
        if (!init_egl(_screen_x, _screen_y, log)) {
            return false;
        }
    #else
        InitVulkan();
        SetupVulkan();
        ::native_window = android::ANativeWindowCreator::Create("AImGui", _screen_x, _screen_y, true);
        SetupVulkanWindow(::native_window, (int) _screen_x, (int) _screen_y);
    #endif
    if (!ImGui_init()) {
        return false;
    }   

    #ifndef USE_OPENGL
        UploadFonts();
    #endif
     
    return true;
}

bool init_egl(uint32_t _screen_x, uint32_t _screen_y, bool log) {
    ::native_window = android::ANativeWindowCreator::Create("AImGui", _screen_x, _screen_y, false);
    if (!::native_window) {
        printf("Create native window failed\n");
        return false;
    }

    ANativeWindow_acquire(native_window);
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        printf("eglGetDisplay error=0x%x\n", eglGetError());
        return false;
    }
    if (log) {
        printf("eglGetDisplay ok\n");
    }
    if (eglInitialize(display, 0, 0) != EGL_TRUE) {
        printf("eglInitialize error=0x%x\n", eglGetError());
        return false;
    }
    if (log) {
        printf("eglInitialize ok\n");
    }
    EGLint num_config = 0;
    const EGLint attribList[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_BLUE_SIZE, 5,   //-->delete
            EGL_GREEN_SIZE, 6,  //-->delete
            EGL_RED_SIZE, 5,    //-->delete
            EGL_BUFFER_SIZE, 32,  //-->new field
            EGL_DEPTH_SIZE, 16,
            EGL_STENCIL_SIZE, 8,
            EGL_NONE
    };
    const EGLint attrib_list[] = {
            EGL_CONTEXT_CLIENT_VERSION,
            3,
            EGL_NONE
    };

    if (log) {
        printf("num_config = %d\n", num_config);
    }
    if (eglChooseConfig(display, attribList, &config, 1, &num_config) != EGL_TRUE || num_config < 1) {
        printf("eglChooseConfig error=0x%x configs=%d\n", eglGetError(), num_config);
        return false;
    }
    if (log) {
        printf("eglChooseConfig ok\n");
    }
    EGLint egl_format;
    eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &egl_format);
    ANativeWindow_setBuffersGeometry(native_window, 0, 0, egl_format);
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, attrib_list);
    if (context == EGL_NO_CONTEXT) {
        printf("eglCreateContext error=0x%x\n", eglGetError());
        return false;
    }
    if (log) {
        printf("eglCreateContext ok\n");
    }
    surface = eglCreateWindowSurface(display, config, native_window, nullptr);
    if (surface == EGL_NO_SURFACE) {
        printf("eglCreateWindowSurface error=0x%x\n", eglGetError());
        return false;
    }
    if (log) {
        printf("eglCreateWindowSurface ok\n");
    }
    if (!eglMakeCurrent(display, surface, surface, context)) {
        printf("eglMakeCurrent error=0x%x\n", eglGetError());
        return false;
    }
    // Prevent cloud devices without an implicit frame cap from rendering the
    // transparent overlay hundreds of times per second.
    eglSwapInterval(display, 1);
    if (log) {
        printf("eglMakeCurrent ok\n");
        printf("createNativeWindow ok\n");
    }
    return true;
}

void screen_config() {
    displayInfo = android::ANativeWindowCreator::GetDisplayInfo();
}
bool ImGui_init() {
    if (g_Initialized) {
        return true;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplAndroid_Init(native_window);
    #if defined(USE_OPENGL)
        ImGui_ImplOpenGL3_Init("#version 300 es");
    #endif
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    static ImFontConfig font_cfg;
    font_cfg.SizePixels = 22.0f;
    font_cfg.FontDataOwnedByAtlas = false;
   io.Fonts->AddFontFromMemoryTTF((void *) OPPOSans_H, OPPOSans_H_size, 32.0f, &font_cfg, io.Fonts->GetGlyphRangesChineseFull());
	//ImFontConfig font_cfg22;
	static ImFontConfig font_cfg2;
    font_cfg2.SizePixels = 22.0f;
	font_cfg2.FontDataOwnedByAtlas = false;
	//font_cfg2.GlyphRanges = io.Fonts->GetGlyphRangesCyrillic();
	verdana = io.Fonts->AddFontFromMemoryTTF((void *) Verdana, sizeof Verdana, 17.0f, &font_cfg2, io.Fonts->GetGlyphRangesCyrillic());
    io.Fonts->AddFontDefault(&font_cfg);
    ImGui::GetStyle().ScaleAllSizes(3.0f);
    ::g_Initialized = true;
    return true;
}

void drawBegin() {
    screen_config();
    if (::orientation != displayInfo.orientation) {
        ::orientation = displayInfo.orientation;
        UpdateScreenData(displayInfo.width, displayInfo.height, displayInfo.orientation);
        if (g_window) {
            g_window->Pos.x = 100;
            g_window->Pos.y = 125;
        }
    }

    #ifdef USE_OPENGL
        EGLint surfaceWidth = 0;
        EGLint surfaceHeight = 0;
        if (display != EGL_NO_DISPLAY && surface != EGL_NO_SURFACE &&
            eglQuerySurface(display, surface, EGL_WIDTH, &surfaceWidth) == EGL_TRUE &&
            eglQuerySurface(display, surface, EGL_HEIGHT, &surfaceHeight) == EGL_TRUE &&
            surfaceWidth > 0 && surfaceHeight > 0) {
            native_window_screen_x = surfaceWidth;
            native_window_screen_y = surfaceHeight;
        }
    #endif

    #ifdef USE_OPENGL
        ImGui_ImplOpenGL3_NewFrame();
    #else
        ImGui_ImplVulkan_NewFrame();
    #endif        
    ImGui_ImplAndroid_NewFrame(native_window_screen_x, native_window_screen_y);
    ImGui::NewFrame();
}

void drawEnd() {
    ImGui::Render();
    
    #ifdef USE_OPENGL
        glViewport(0, 0, native_window_screen_x, native_window_screen_y);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (eglSwapBuffers(display, surface) != EGL_TRUE) {
            printf("eglSwapBuffers error=0x%x\n", eglGetError());
        }
    #else
        FrameRender(ImGui::GetDrawData());
        FramePresent();
    #endif
}



void shutdown() {
    if (!g_Initialized) {
        return;
    }
    // Cleanup
    #ifdef USE_OPENGL
        ImGui_ImplOpenGL3_Shutdown();
    #else
        DeviceWait();
        ImGui_ImplVulkan_Shutdown();
    #endif
    ImGui_ImplAndroid_Shutdown();
    ImGui::DestroyContext();
    
    
    #ifdef USE_OPENGL
        if (display != EGL_NO_DISPLAY) {
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (context != EGL_NO_CONTEXT) {
                eglDestroyContext(display, context);
            }
            if (surface != EGL_NO_SURFACE) {
                eglDestroySurface(display, surface);
            }
            eglTerminate(display);
        }
        display = EGL_NO_DISPLAY;
        context = EGL_NO_CONTEXT;
        surface = EGL_NO_SURFACE;
    #else    
        CleanupVulkanWindow();
        CleanupVulkan();
    #endif
    
    ANativeWindow_release(native_window);
    android::ANativeWindowCreator::Destroy(native_window);
    ::g_Initialized = false;
}
