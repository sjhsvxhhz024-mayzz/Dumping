# Target: ARM64 Android, standalone executable (runs under root on device).
APP_ABI      := arm64-v8a
# android-29 gives us EGL, GLES3, ANativeWindow, dlfcn, system_properties.
APP_PLATFORM := android-29
# Static-link the C++ STL so the binary has no external libstdc++ dependency.
APP_STL      := c++_static
# -Wno-error: downgrade all -Werror back to warnings (oxorany + ImGui::TextColored
#   with non-literal format strings trigger -Wformat-security; the original build
#   was compiled with warnings, not errors).
# -Wno-format-security: silence the non-literal-format-string warning entirely.
APP_CPPFLAGS := -std=c++17 -fno-rtti -fexceptions -Wno-error -Wno-format-security -Wno-format
APP_CFLAGS   := -ffunction-sections -fdata-sections -Wno-error -Wno-format-security -Wno-format
# Release build (matches the existing stripped binary).
APP_OPTIM    := release
APP_STRIP_MODE := --strip-all
