#pragma once

#include <android-base/unique_fd.h>
#include <cutils/native_handle.h>

#include <cstdint>
#include <vector>

namespace exynos4::composer3 {

struct DisplayMode {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t refreshRateMilliHz = 0;
    uint32_t xdpiMilli = 0;
    uint32_t ydpiMilli = 0;
};

class DisplayBackend {
  public:
    struct Window {
        const native_handle_t* buffer = nullptr;
        int acquireFence = -1;
        int32_t x = 0;
        int32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t sourceX = 0;
        uint32_t sourceY = 0;
        int32_t blending = 0;
        uint8_t planeAlpha = 255;
    };

    bool openDisplay();
    bool hasFimc() const { return mFimc.get() >= 0; }
    void disableFimc() { mFimc.reset(); }
    int convertVideo(const native_handle_t* source, int acquireFence,
                     uint32_t cropX, uint32_t cropY, uint32_t cropWidth, uint32_t cropHeight,
                     const native_handle_t* destination, uint32_t width, uint32_t height);
    const DisplayMode& mode() const { return mMode; }
    int presentWindows(const std::vector<Window>& windows,
                       android::base::unique_fd* outPresentFence);
    // The caller retains ownership of both the buffer and acquireFence.
    // On success, outPresentFence receives ownership of the kernel fence.
    int presentClientTarget(const native_handle_t* buffer, int acquireFence,
                            android::base::unique_fd* outPresentFence);
    int setPower(bool on);
    int setBrightness(float brightness);
    int setVsyncEnabled(bool enabled);

  private:
    android::base::unique_fd mFramebuffer;
    android::base::unique_fd mFimc;
    // Backlight node and its range never change at runtime; open and read
    // them once instead of on every brightness update.
    android::base::unique_fd mBacklight;
    unsigned int mBacklightMax = 0;
    DisplayMode mMode;
};

}  // namespace exynos4::composer3
