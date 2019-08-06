/*
 * Based on minicap_14.cpp
 * Contains code contributed by Orange,
 * licensed under the Apache license, Version 2.0
 */

#include "Minicap.hpp"

#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <math.h>
#include <arm_neon.h>

#include <binder/ProcessState.h>

#include <binder/IServiceManager.h>
#include <binder/IMemory.h>

#include <gui/ISurfaceComposer.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>

#include <private/gui/ComposerService.h>

#include <ui/DisplayInfo.h>
#include <ui/PixelFormat.h>
#include <ui/Rect.h>

#include "mcdebug.h"

static const char*
error_name(int32_t err) {
  switch (err) {
  case android::NO_ERROR: // also android::OK
    return "NO_ERROR";
  case android::UNKNOWN_ERROR:
    return "UNKNOWN_ERROR";
  case android::NO_MEMORY:
    return "NO_MEMORY";
  case android::INVALID_OPERATION:
    return "INVALID_OPERATION";
  case android::BAD_VALUE:
    return "BAD_VALUE";
  case android::BAD_TYPE:
    return "BAD_TYPE";
  case android::NAME_NOT_FOUND:
    return "NAME_NOT_FOUND";
  case android::PERMISSION_DENIED:
    return "PERMISSION_DENIED";
  case android::NO_INIT:
    return "NO_INIT";
  case android::ALREADY_EXISTS:
    return "ALREADY_EXISTS";
  case android::DEAD_OBJECT: // also android::JPARKS_BROKE_IT
    return "DEAD_OBJECT";
  case android::FAILED_TRANSACTION:
    return "FAILED_TRANSACTION";
  case android::BAD_INDEX:
    return "BAD_INDEX";
  case android::NOT_ENOUGH_DATA:
    return "NOT_ENOUGH_DATA";
  case android::WOULD_BLOCK:
    return "WOULD_BLOCK";
  case android::TIMED_OUT:
    return "TIMED_OUT";
  case android::UNKNOWN_TRANSACTION:
    return "UNKNOWN_TRANSACTION";
  case android::FDS_NOT_ALLOWED:
    return "FDS_NOT_ALLOWED";
  default:
    return "UNMAPPED_ERROR";
  }
}

class MinicapImpl: public Minicap
{
public:
  MinicapImpl(int32_t displayId)
    : mDisplayId(displayId),
      mDesiredWidth(0),
      mDesiredHeight(0) {
  }

  virtual
  ~MinicapImpl() {
    release();
  }

  virtual int
  applyConfigChanges() {
   mDisplay = android::SurfaceComposerClient::getBuiltInDisplay(
        mDisplayId);
    if (mDisplay == NULL) {
        MCERROR("Unable to get handle for display %d\n", mDisplayId);
        return 1;
    }
    mUserFrameAvailableListener->onFrameAvailable();

    return 0;
  }

 /*
  * https://community.arm.com/developer/ip-products/processors/b/processors-ip-blog/posts/coding-for-neon---part-4-shifting-left-and-right
  * using Neon intrinsics.
  */
  void convert565to888(uint16_t *src, uint8_t* dst, int width, int height) {
    const uint16_t* end_addr = src + width * height;
    while (src < end_addr) {
        uint16x8_t in = vld1q_u16(src);
        uint16x8_t tmp = vshrq_n_u8((uint8x16_t)in, 3);
        uint8x8_t r = vshrn_n_u16(tmp, 5);
        uint8x8_t g = vshrn_n_u16(in, 5);
        g = vshl_n_u8(g, 2);
        tmp = vshlq_n_u16(in, 3);
        uint8x8_t b = vmovn_u16(tmp);
        uint8x8x3_t rgb = {r, g, b};
        vst3_u8(dst, rgb);
        src += 8;
        dst += 24;
    }
  }

  virtual int
  consumePendingFrame(Minicap::Frame* frame) {
    android::status_t result;
    static uint8_t *dst = NULL;
    void const* base = NULL;
    uint32_t width, height, stride;

    android::ScreenshotClient screenshot;

    result = screenshot.update(mDisplay, android::Rect(), 0, 0, 0, -1U, false, 0);
    if (result != android::OK) {
        MCERROR("ScreenshotClient::update() failed %s", error_name(result));
        return result;
    }
    base = screenshot.getPixels();
    width = screenshot.getWidth();
    height = screenshot.getHeight();
    stride = screenshot.getStride();

    uint16_t* src = (uint16_t*) base;
    if(dst == NULL){
        dst = (uint8_t*)malloc(stride*height*3);
    }
    /*
     * - Screenshot client reports RGBA_8888 while it actually is RGB_565,
     * - libjpeg-turbo doesn't support RGB_565 input,
     *  Thus we convert it to RGB_888 beforehand
     */
    convert565to888(src, dst,stride,height);

    frame->data = dst;
    frame->format = FORMAT_RGB_888;
    frame->width = width;
    frame->height = height;
    frame->stride = stride;
    frame->bpp = 3;
    frame->size = stride * height * frame->bpp;

    return 0;
  }

  virtual Minicap::CaptureMethod
  getCaptureMethod() {
    return METHOD_SCREENSHOT;
  }

  virtual int32_t
  getDisplayId() {
    return mDisplayId;
  }

  virtual void
  release() {
  }

  virtual void
  releaseConsumedFrame(Minicap::Frame* /* frame */) {
    return mUserFrameAvailableListener->onFrameAvailable();
  }

  virtual int
  setDesiredInfo(const Minicap::DisplayInfo& info) {
    mDesiredWidth = info.width;
    mDesiredHeight = info.height;
    return 0;
  }

  virtual void
  setFrameAvailableListener(Minicap::FrameAvailableListener* listener) {
    mUserFrameAvailableListener = listener;
  }

  virtual int
  setRealInfo(const Minicap::DisplayInfo& info) {
    return 0;
  }

private:
  int32_t mDisplayId;
  android::sp<android::IBinder> mDisplay;
  uint32_t mDesiredWidth;
  uint32_t mDesiredHeight;

  Minicap::FrameAvailableListener* mUserFrameAvailableListener;

  static Minicap::Format
  convertFormat(android::PixelFormat format) {
    switch (format) {
    case android::PIXEL_FORMAT_NONE:
      return FORMAT_NONE;
    case android::PIXEL_FORMAT_CUSTOM:
      return FORMAT_CUSTOM;
    case android::PIXEL_FORMAT_TRANSLUCENT:
      return FORMAT_TRANSLUCENT;
    case android::PIXEL_FORMAT_TRANSPARENT:
      return FORMAT_TRANSPARENT;
    case android::PIXEL_FORMAT_OPAQUE:
      return FORMAT_OPAQUE;
    case android::PIXEL_FORMAT_RGBA_8888:
      return FORMAT_RGBA_8888;
    case android::PIXEL_FORMAT_RGBX_8888:
      return FORMAT_RGBX_8888;
    case android::PIXEL_FORMAT_RGB_888:
      return FORMAT_RGB_888;
    case android::PIXEL_FORMAT_RGB_565:
      return FORMAT_RGB_565;
    case android::PIXEL_FORMAT_BGRA_8888:
      return FORMAT_BGRA_8888;
    case android::PIXEL_FORMAT_RGBA_5551:
      return FORMAT_RGBA_5551;
    case android::PIXEL_FORMAT_RGBA_4444:
      return FORMAT_RGBA_4444;
    default:
      return FORMAT_UNKNOWN;
    }
  }
};

int
minicap_try_get_display_info(int32_t displayId, Minicap::DisplayInfo* info) {
  android::sp<android::IBinder> dpy = android::SurfaceComposerClient::getBuiltInDisplay(displayId);

  android::Vector<android::DisplayInfo> configs;
  android::status_t err = android::SurfaceComposerClient::getDisplayConfigs(dpy, &configs);

  if (err != android::NO_ERROR) {
    MCERROR("SurfaceComposerClient::getDisplayInfo() failed: %s (%d)\n", error_name(err), err);
    return err;
  }

  int activeConfig = android::SurfaceComposerClient::getActiveConfig(dpy);
  if(static_cast<size_t>(activeConfig) >= configs.size()) {
      MCERROR("Active config %d not inside configs (size %zu)\n", activeConfig, configs.size());
      return android::BAD_VALUE;
  }
  android::DisplayInfo dinfo = configs[activeConfig];

  info->width = dinfo.w;
  info->height = dinfo.h;
  info->orientation = dinfo.orientation;
  info->fps = dinfo.fps;
  info->density = dinfo.density;
  info->xdpi = dinfo.xdpi;
  info->ydpi = dinfo.ydpi;
  info->secure = dinfo.secure;
  info->size = sqrt(pow(dinfo.w / dinfo.xdpi, 2) + pow(dinfo.h / dinfo.ydpi, 2));

  return 0;
}

Minicap*
minicap_create(int32_t displayId) {
  return new MinicapImpl(displayId);
}

void
minicap_free(Minicap* mc) {
  delete mc;
}

void
minicap_start_thread_pool() {
  android::ProcessState::self()->startThreadPool();
}
