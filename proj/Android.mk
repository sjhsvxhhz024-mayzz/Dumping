LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE    := eclipsoxide
LOCAL_CPP_FEATURES := exceptions

# --- Sources ---
# Amalgamated Lua (all .c compiled as C++ via lua_all.cpp)
LOCAL_SRC_FILES := \
    src/main.cpp \
    src/eclips_oxide_menu.cpp \
    src/memory.cpp \
    src/utils.cpp \
    src/oxlog.cpp \
    src/menu_bg.cpp \
    src/LuaIntegration.cpp \
    src/lua_all.cpp \
    src/oxorany/oxorany.cpp \
    src/Android_draw/draw.cpp \
    src/Android_touch/TouchHelperA.cpp \
    src/ImGui/imgui.cpp \
    src/ImGui/imgui_demo.cpp \
    src/ImGui/imgui_draw.cpp \
    src/ImGui/imgui_tables.cpp \
    src/ImGui/imgui_widgets.cpp \
    src/ImGui/backends/imgui_impl_android.cpp \
    src/ImGui/backends/imgui_impl_opengl3.cpp

# --- Include paths ---
LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/include \
    $(LOCAL_PATH)/src \
    $(LOCAL_PATH)/lua/lua-5.4.7/src \
    $(LOCAL_PATH)/include/ImGui \
    $(LOCAL_PATH)/include/ImGui/backends \
    $(LOCAL_PATH)/include/ImGui/font \
    $(LOCAL_PATH)/include/native_surface \
    $(LOCAL_PATH)/include/Android_draw \
    $(LOCAL_PATH)/include/Android_touch \
    $(LOCAL_PATH)/include/oxorany \
    $(LOCAL_PATH)/src/oxorany

# --- Compile flags ---
# -DUSE_OPENGL: forces the EGL/GLES3 path in draw.cpp (not Vulkan).
# -std=c++17: required by ANativeWindowCreator.h (structured bindings) and oxorany.
# -fvisibility=hidden: keep symbol table small (matches existing binary).
LOCAL_CFLAGS := \
    -DUSE_OPENGL \
    -Wno-unused-result \
    -Wno-deprecated-declarations \
    -Wno-format-security \
    -Wno-format \
    -Wno-format-pedantic

# Lua is C compiled as C++ (amalgamated); suppress C++ warnings on it.
LOCAL_CPPFLAGS := -std=c++17 -fno-rtti -fexceptions

# --- Link flags ---
# Standalone PIE executable. Links against public Android libs only:
#   -llog     android/log.h (__android_log_print)
#   -landroid android/native_window.h (ANativeWindow_*)
#   -lEGL     EGL
#   -lGLESv3  OpenGL ES 3 (USE_OPENGL path)
#   -ldl      dlopen/dlsym (ANativeWindowCreator runtime symbol resolution)
#   -lm       math
# Private SurfaceComposer symbols (libgui.so/libutils.so) are resolved at
# runtime via dlsym — no link-time dependency.
LOCAL_LDLIBS := \
    -llog \
    -landroid \
    -lEGL \
    -lGLESv3 \
    -ldl \
    -lm \
    C:/ndk/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_static.a \
    C:/ndk/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++abi.a

# Build as a standalone executable (not a .so). Runs under `su` on device.
include $(BUILD_EXECUTABLE)
