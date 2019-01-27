/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// #define LOG_NDEBUG 0
#undef LOG_TAG
#define LOG_TAG "DisplayDevice"

#include "DisplayDevice.h"

#include <array>
#include <unordered_set>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <android-base/stringprintf.h>
#include <android/hardware/configstore/1.0/ISurfaceFlingerConfigs.h>
#include <configstore/Utils.h>
#include <cutils/properties.h>
#include <gui/Surface.h>
#include <hardware/gralloc.h>
#include <renderengine/RenderEngine.h>
#include <sync/sync.h>
#include <system/window.h>
#include <ui/DebugUtils.h>
#include <ui/DisplayInfo.h>
#include <ui/PixelFormat.h>
#include <utils/Log.h>
#include <utils/RefBase.h>

#include "DisplayHardware/DisplaySurface.h"
#include "DisplayHardware/HWComposer.h"
#include "DisplayHardware/HWC2.h"
#include "SurfaceFlinger.h"
#include "Layer.h"

namespace android {

// retrieve triple buffer setting from configstore
using namespace android::hardware::configstore;
using namespace android::hardware::configstore::V1_0;
using android::base::StringAppendF;
using android::ui::ColorMode;
using android::ui::Dataspace;
using android::ui::Hdr;
using android::ui::RenderIntent;

/*
 * Initialize the display to the specified values.
 *
 */

uint32_t DisplayDevice::sPrimaryDisplayOrientation = 0;

namespace {

// ordered list of known SDR color modes
const std::array<ColorMode, 3> sSdrColorModes = {
        ColorMode::DISPLAY_BT2020,
        ColorMode::DISPLAY_P3,
        ColorMode::SRGB,
};

// ordered list of known HDR color modes
const std::array<ColorMode, 2> sHdrColorModes = {
        ColorMode::BT2100_PQ,
        ColorMode::BT2100_HLG,
};

// ordered list of known SDR render intents
const std::array<RenderIntent, 2> sSdrRenderIntents = {
        RenderIntent::ENHANCE,
        RenderIntent::COLORIMETRIC,
};

// ordered list of known HDR render intents
const std::array<RenderIntent, 2> sHdrRenderIntents = {
        RenderIntent::TONE_MAP_ENHANCE,
        RenderIntent::TONE_MAP_COLORIMETRIC,
};

// map known color mode to dataspace
Dataspace colorModeToDataspace(ColorMode mode) {
    switch (mode) {
        case ColorMode::SRGB:
            return Dataspace::V0_SRGB;
        case ColorMode::DISPLAY_P3:
            return Dataspace::DISPLAY_P3;
        case ColorMode::DISPLAY_BT2020:
            return Dataspace::DISPLAY_BT2020;
        case ColorMode::BT2100_HLG:
            return Dataspace::BT2020_HLG;
        case ColorMode::BT2100_PQ:
            return Dataspace::BT2020_PQ;
        default:
            return Dataspace::UNKNOWN;
    }
}

// Return a list of candidate color modes.
std::vector<ColorMode> getColorModeCandidates(ColorMode mode) {
    std::vector<ColorMode> candidates;

    // add mode itself
    candidates.push_back(mode);

    // check if mode is HDR
    bool isHdr = false;
    for (auto hdrMode : sHdrColorModes) {
        if (hdrMode == mode) {
            isHdr = true;
            break;
        }
    }

    // add other HDR candidates when mode is HDR
    if (isHdr) {
        for (auto hdrMode : sHdrColorModes) {
            if (hdrMode != mode) {
                candidates.push_back(hdrMode);
            }
        }
    }

    // add other SDR candidates
    for (auto sdrMode : sSdrColorModes) {
        if (sdrMode != mode) {
            candidates.push_back(sdrMode);
        }
    }

    return candidates;
}

// Return a list of candidate render intents.
std::vector<RenderIntent> getRenderIntentCandidates(RenderIntent intent) {
    std::vector<RenderIntent> candidates;

    // add intent itself
    candidates.push_back(intent);

    // check if intent is HDR
    bool isHdr = false;
    for (auto hdrIntent : sHdrRenderIntents) {
        if (hdrIntent == intent) {
            isHdr = true;
            break;
        }
    }

    if (isHdr) {
        // add other HDR candidates when intent is HDR
        for (auto hdrIntent : sHdrRenderIntents) {
            if (hdrIntent != intent) {
                candidates.push_back(hdrIntent);
            }
        }
    } else {
        // add other SDR candidates when intent is SDR
        for (auto sdrIntent : sSdrRenderIntents) {
            if (sdrIntent != intent) {
                candidates.push_back(sdrIntent);
            }
        }
    }

    return candidates;
}

// Return the best color mode supported by HWC.
ColorMode getHwcColorMode(
        const std::unordered_map<ColorMode, std::vector<RenderIntent>>& hwcColorModes,
        ColorMode mode) {
    std::vector<ColorMode> candidates = getColorModeCandidates(mode);
    for (auto candidate : candidates) {
        auto iter = hwcColorModes.find(candidate);
        if (iter != hwcColorModes.end()) {
            return candidate;
        }
    }

    return ColorMode::NATIVE;
}

// Return the best render intent supported by HWC.
RenderIntent getHwcRenderIntent(const std::vector<RenderIntent>& hwcIntents, RenderIntent intent) {
    std::vector<RenderIntent> candidates = getRenderIntentCandidates(intent);
    for (auto candidate : candidates) {
        for (auto hwcIntent : hwcIntents) {
            if (candidate == hwcIntent) {
                return candidate;
            }
        }
    }

    return RenderIntent::COLORIMETRIC;
}

} // anonymous namespace

DisplayDeviceCreationArgs::DisplayDeviceCreationArgs(const sp<SurfaceFlinger>& flinger,
                                                     const wp<IBinder>& displayToken,
                                                     const std::optional<DisplayId>& displayId)
      : flinger(flinger), displayToken(displayToken), displayId(displayId) {}

DisplayDevice::DisplayDevice(DisplayDeviceCreationArgs&& args)
      : lastCompositionHadVisibleLayers(false),
        mFlinger(args.flinger),
        mDisplayToken(args.displayToken),
        mSequenceId(args.sequenceId),
        mId(args.displayId),
        mNativeWindow(args.nativeWindow),
        mGraphicBuffer(nullptr),
        mDisplaySurface(args.displaySurface),
        mDisplayInstallOrientation(args.displayInstallOrientation),
        mPageFlipCount(0),
        mIsVirtual(args.isVirtual),
        mIsSecure(args.isSecure),
        mLayerStack(NO_LAYER_STACK),
        mOrientation(),
        mViewport(Rect::INVALID_RECT),
        mFrame(Rect::INVALID_RECT),
        mPowerMode(args.initialPowerMode),
        mActiveConfig(0),
        mColorTransform(HAL_COLOR_TRANSFORM_IDENTITY),
        mHasWideColorGamut(args.hasWideColorGamut),
        mHasHdr10Plus(false),
        mHasHdr10(false),
        mHasHLG(false),
        mHasDolbyVision(false),
        mSupportedPerFrameMetadata(args.supportedPerFrameMetadata),
        mIsPrimary(args.isPrimary) {
    populateColorModes(args.hwcColorModes);

    ALOGE_IF(!mNativeWindow, "No native window was set for display");
    ALOGE_IF(!mDisplaySurface, "No display surface was set for display");

    std::vector<Hdr> types = args.hdrCapabilities.getSupportedHdrTypes();
    for (Hdr hdrType : types) {
        switch (hdrType) {
            case Hdr::HDR10_PLUS:
                mHasHdr10Plus = true;
                break;
            case Hdr::HDR10:
                mHasHdr10 = true;
                break;
            case Hdr::HLG:
                mHasHLG = true;
                break;
            case Hdr::DOLBY_VISION:
                mHasDolbyVision = true;
                break;
            default:
                ALOGE("UNKNOWN HDR capability: %d", static_cast<int32_t>(hdrType));
        }
    }

    float minLuminance = args.hdrCapabilities.getDesiredMinLuminance();
    float maxLuminance = args.hdrCapabilities.getDesiredMaxLuminance();
    float maxAverageLuminance = args.hdrCapabilities.getDesiredMaxAverageLuminance();

    minLuminance = minLuminance <= 0.0 ? sDefaultMinLumiance : minLuminance;
    maxLuminance = maxLuminance <= 0.0 ? sDefaultMaxLumiance : maxLuminance;
    maxAverageLuminance = maxAverageLuminance <= 0.0 ? sDefaultMaxLumiance : maxAverageLuminance;
    if (this->hasWideColorGamut()) {
        // insert HDR10/HLG as we will force client composition for HDR10/HLG
        // layers
        if (!hasHDR10Support()) {
            types.push_back(Hdr::HDR10);
        }

        if (!hasHLGSupport()) {
            types.push_back(Hdr::HLG);
        }
    }
    mHdrCapabilities = HdrCapabilities(types, maxLuminance, maxAverageLuminance, minLuminance);

    ANativeWindow* const window = mNativeWindow.get();

    int status = native_window_api_connect(window, NATIVE_WINDOW_API_EGL);
    ALOGE_IF(status != NO_ERROR, "Unable to connect BQ producer: %d", status);
    status = native_window_set_buffers_format(window, HAL_PIXEL_FORMAT_RGBA_8888);
    ALOGE_IF(status != NO_ERROR, "Unable to set BQ format to RGBA888: %d", status);
    status = native_window_set_usage(window, GRALLOC_USAGE_HW_RENDER);
    ALOGE_IF(status != NO_ERROR, "Unable to set BQ usage bits for GPU rendering: %d", status);

    mDisplayWidth = ANativeWindow_getWidth(window);
    mDisplayHeight = ANativeWindow_getHeight(window);

    // initialize the display orientation transform.
    setProjection(DisplayState::eOrientationDefault, mViewport, mFrame);
}

DisplayDevice::~DisplayDevice() = default;

void DisplayDevice::disconnect(HWComposer& hwc) {
    if (mId) {
        hwc.disconnectDisplay(*mId);
        mId.reset();
    }
}

int DisplayDevice::getWidth() const {
    return mDisplayWidth;
}

int DisplayDevice::getHeight() const {
    return mDisplayHeight;
}

void DisplayDevice::setDisplayName(const std::string& displayName) {
    if (!displayName.empty()) {
        // never override the name with an empty name
        mDisplayName = displayName;
    }
}

uint32_t DisplayDevice::getPageFlipCount() const {
    return mPageFlipCount;
}

void DisplayDevice::flip() const
{
    mPageFlipCount++;
}

status_t DisplayDevice::beginFrame(bool mustRecompose) const {
    return mDisplaySurface->beginFrame(mustRecompose);
}

status_t DisplayDevice::prepareFrame(HWComposer& hwc,
        std::vector<CompositionInfo>& compositionData) {
    if (mId) {
        status_t error = hwc.prepare(*mId, compositionData);
        if (error != NO_ERROR) {
            return error;
        }
    }

    DisplaySurface::CompositionType compositionType;
    bool hasClient = hwc.hasClientComposition(mId);
    bool hasDevice = hwc.hasDeviceComposition(mId);
    if (hasClient && hasDevice) {
        compositionType = DisplaySurface::COMPOSITION_MIXED;
    } else if (hasClient) {
        compositionType = DisplaySurface::COMPOSITION_GLES;
    } else if (hasDevice) {
        compositionType = DisplaySurface::COMPOSITION_HWC;
    } else {
        // Nothing to do -- when turning the screen off we get a frame like
        // this. Call it a HWC frame since we won't be doing any GLES work but
        // will do a prepare/set cycle.
        compositionType = DisplaySurface::COMPOSITION_HWC;
    }
    return mDisplaySurface->prepareFrame(compositionType);
}

void DisplayDevice::setProtected(bool useProtected) {
    uint64_t usageFlags = GRALLOC_USAGE_HW_RENDER;
    if (useProtected) {
        usageFlags |= GRALLOC_USAGE_PROTECTED;
    }
    const int status = native_window_set_usage(mNativeWindow.get(), usageFlags);
    ALOGE_IF(status != NO_ERROR, "Unable to set BQ usage bits for protected content: %d", status);
}

sp<GraphicBuffer> DisplayDevice::dequeueBuffer() {
    int fd;
    ANativeWindowBuffer* buffer;

    status_t res = mNativeWindow->dequeueBuffer(mNativeWindow.get(), &buffer, &fd);

    if (res != NO_ERROR) {
        ALOGE("ANativeWindow::dequeueBuffer failed for display [%s] with error: %d",
              getDisplayName().c_str(), res);
        // Return fast here as we can't do much more - any rendering we do
        // now will just be wrong.
        return mGraphicBuffer;
    }

    ALOGW_IF(mGraphicBuffer != nullptr, "Clobbering a non-null pointer to a buffer [%p].",
             mGraphicBuffer->getNativeBuffer()->handle);
    mGraphicBuffer = GraphicBuffer::from(buffer);

    // Block until the buffer is ready
    // TODO(alecmouri): it's perhaps more appropriate to block renderengine so
    // that the gl driver can block instead.
    if (fd >= 0) {
        sync_wait(fd, -1);
        close(fd);
    }

    return mGraphicBuffer;
}

void DisplayDevice::queueBuffer(HWComposer& hwc) {
    if (hwc.hasClientComposition(mId) || hwc.hasFlipClientTargetRequest(mId)) {
        // hasFlipClientTargetRequest could return true even if we haven't
        // dequeued a buffer before. Try dequeueing one if we don't have a
        // buffer ready.
        if (mGraphicBuffer == nullptr) {
            ALOGI("Attempting to queue a client composited buffer without one "
                  "previously dequeued for display [%s]. Attempting to dequeue "
                  "a scratch buffer now",
                  mDisplayName.c_str());
            // We shouldn't deadlock here, since mGraphicBuffer == nullptr only
            // after a successful call to queueBuffer, or if dequeueBuffer has
            // never been called.
            dequeueBuffer();
        }

        if (mGraphicBuffer == nullptr) {
            ALOGE("No buffer is ready for display [%s]", mDisplayName.c_str());
        } else {
            status_t res = mNativeWindow->queueBuffer(mNativeWindow.get(),
                                                      mGraphicBuffer->getNativeBuffer(),
                                                      dup(mBufferReady));
            if (res != NO_ERROR) {
                ALOGE("Error when queueing buffer for display [%s]: %d", mDisplayName.c_str(), res);
                // We risk blocking on dequeueBuffer if the primary display failed
                // to queue up its buffer, so crash here.
                if (isPrimary()) {
                    LOG_ALWAYS_FATAL("ANativeWindow::queueBuffer failed with error: %d", res);
                } else {
                    mNativeWindow->cancelBuffer(mNativeWindow.get(),
                                                mGraphicBuffer->getNativeBuffer(),
                                                dup(mBufferReady));
                }
            }

            mBufferReady.reset();
            mGraphicBuffer = nullptr;
        }
    }

    status_t result = mDisplaySurface->advanceFrame();
    if (result != NO_ERROR) {
        ALOGE("[%s] failed pushing new frame to HWC: %d", mDisplayName.c_str(), result);
    }
}

void DisplayDevice::onPresentDisplayCompleted() {
    mDisplaySurface->onFrameCommitted();
}

void DisplayDevice::setViewportAndProjection() const {
    size_t w = mDisplayWidth;
    size_t h = mDisplayHeight;
    Rect sourceCrop(0, 0, w, h);
    mFlinger->getRenderEngine().setViewportAndProjection(w, h, sourceCrop, ui::Transform::ROT_0);
}

void DisplayDevice::finishBuffer() {
    mBufferReady = mFlinger->getRenderEngine().flush();
    if (mBufferReady.get() < 0) {
        mFlinger->getRenderEngine().finish();
    }
}

const sp<Fence>& DisplayDevice::getClientTargetAcquireFence() const {
    return mDisplaySurface->getClientTargetAcquireFence();
}

// ----------------------------------------------------------------------------

void DisplayDevice::setVisibleLayersSortedByZ(const Vector< sp<Layer> >& layers) {
    mVisibleLayersSortedByZ = layers;
}

const Vector< sp<Layer> >& DisplayDevice::getVisibleLayersSortedByZ() const {
    return mVisibleLayersSortedByZ;
}

void DisplayDevice::setLayersNeedingFences(const Vector< sp<Layer> >& layers) {
    mLayersNeedingFences = layers;
}

const Vector< sp<Layer> >& DisplayDevice::getLayersNeedingFences() const {
    return mLayersNeedingFences;
}

Region DisplayDevice::getDirtyRegion(bool repaintEverything) const {
    Region dirty;
    if (repaintEverything) {
        dirty.set(getBounds());
    } else {
        const ui::Transform& planeTransform(mGlobalTransform);
        dirty = planeTransform.transform(this->dirtyRegion);
        dirty.andSelf(getBounds());
    }
    return dirty;
}

// ----------------------------------------------------------------------------
void DisplayDevice::setPowerMode(int mode) {
    mPowerMode = mode;
}

int DisplayDevice::getPowerMode()  const {
    return mPowerMode;
}

bool DisplayDevice::isPoweredOn() const {
    return mPowerMode != HWC_POWER_MODE_OFF;
}

// ----------------------------------------------------------------------------
void DisplayDevice::setActiveConfig(int mode) {
    mActiveConfig = mode;
}

int DisplayDevice::getActiveConfig()  const {
    return mActiveConfig;
}

// ----------------------------------------------------------------------------
void DisplayDevice::setActiveColorMode(ColorMode mode) {
    mActiveColorMode = mode;
}

ColorMode DisplayDevice::getActiveColorMode() const {
    return mActiveColorMode;
}

RenderIntent DisplayDevice::getActiveRenderIntent() const {
    return mActiveRenderIntent;
}

void DisplayDevice::setActiveRenderIntent(RenderIntent renderIntent) {
    mActiveRenderIntent = renderIntent;
}

void DisplayDevice::setColorTransform(const mat4& transform) {
    const bool isIdentity = (transform == mat4());
    mColorTransform =
            isIdentity ? HAL_COLOR_TRANSFORM_IDENTITY : HAL_COLOR_TRANSFORM_ARBITRARY_MATRIX;
}

android_color_transform_t DisplayDevice::getColorTransform() const {
    return mColorTransform;
}

void DisplayDevice::setCompositionDataSpace(ui::Dataspace dataspace) {
    mCompositionDataSpace = dataspace;
    ANativeWindow* const window = mNativeWindow.get();
    native_window_set_buffers_data_space(window, static_cast<android_dataspace>(dataspace));
}

ui::Dataspace DisplayDevice::getCompositionDataSpace() const {
    return mCompositionDataSpace;
}

// ----------------------------------------------------------------------------

void DisplayDevice::setLayerStack(uint32_t stack) {
    mLayerStack = stack;
    dirtyRegion.set(bounds());
}

// ----------------------------------------------------------------------------

uint32_t DisplayDevice::getOrientationTransform() const {
    uint32_t transform = 0;
    switch (mOrientation) {
        case DisplayState::eOrientationDefault:
            transform = ui::Transform::ROT_0;
            break;
        case DisplayState::eOrientation90:
            transform = ui::Transform::ROT_90;
            break;
        case DisplayState::eOrientation180:
            transform = ui::Transform::ROT_180;
            break;
        case DisplayState::eOrientation270:
            transform = ui::Transform::ROT_270;
            break;
    }
    return transform;
}

status_t DisplayDevice::orientationToTransfrom(
        int orientation, int w, int h, ui::Transform* tr)
{
    uint32_t flags = 0;
    switch (orientation) {
    case DisplayState::eOrientationDefault:
        flags = ui::Transform::ROT_0;
        break;
    case DisplayState::eOrientation90:
        flags = ui::Transform::ROT_90;
        break;
    case DisplayState::eOrientation180:
        flags = ui::Transform::ROT_180;
        break;
    case DisplayState::eOrientation270:
        flags = ui::Transform::ROT_270;
        break;
    default:
        return BAD_VALUE;
    }
    tr->set(flags, w, h);
    return NO_ERROR;
}

void DisplayDevice::setDisplaySize(const int newWidth, const int newHeight) {
    dirtyRegion.set(getBounds());

    mDisplaySurface->resizeBuffers(newWidth, newHeight);

    mDisplayWidth = newWidth;
    mDisplayHeight = newHeight;
}

void DisplayDevice::setProjection(int orientation,
        const Rect& newViewport, const Rect& newFrame) {
    Rect viewport(newViewport);
    Rect frame(newFrame);

    const int w = mDisplayWidth;
    const int h = mDisplayHeight;

    ui::Transform R;
    DisplayDevice::orientationToTransfrom(orientation, w, h, &R);

    if (!frame.isValid()) {
        // the destination frame can be invalid if it has never been set,
        // in that case we assume the whole display frame.
        frame = Rect(w, h);
    }

    if (viewport.isEmpty()) {
        // viewport can be invalid if it has never been set, in that case
        // we assume the whole display size.
        // it's also invalid to have an empty viewport, so we handle that
        // case in the same way.
        viewport = Rect(w, h);
        if (R.getOrientation() & ui::Transform::ROT_90) {
            // viewport is always specified in the logical orientation
            // of the display (ie: post-rotation).
            std::swap(viewport.right, viewport.bottom);
        }
    }

    dirtyRegion.set(getBounds());

    ui::Transform TL, TP, S;
    float src_width  = viewport.width();
    float src_height = viewport.height();
    float dst_width  = frame.width();
    float dst_height = frame.height();
    if (src_width != dst_width || src_height != dst_height) {
        float sx = dst_width  / src_width;
        float sy = dst_height / src_height;
        S.set(sx, 0, 0, sy);
    }

    float src_x = viewport.left;
    float src_y = viewport.top;
    float dst_x = frame.left;
    float dst_y = frame.top;
    TL.set(-src_x, -src_y);
    TP.set(dst_x, dst_y);

    // need to take care of primary display rotation for mGlobalTransform
    // for case if the panel is not installed aligned with device orientation
    if (isPrimary()) {
        DisplayDevice::orientationToTransfrom(
                (orientation + mDisplayInstallOrientation) % (DisplayState::eOrientation270 + 1),
                w, h, &R);
    }

    // The viewport and frame are both in the logical orientation.
    // Apply the logical translation, scale to physical size, apply the
    // physical translation and finally rotate to the physical orientation.
    mGlobalTransform = R * TP * S * TL;

    const uint8_t type = mGlobalTransform.getType();
    mNeedsFiltering = (!mGlobalTransform.preserveRects() ||
            (type >= ui::Transform::SCALE));

    mScissor = mGlobalTransform.transform(viewport);
    if (mScissor.isEmpty()) {
        mScissor = getBounds();
    }

    mOrientation = orientation;
    if (isPrimary()) {
        uint32_t transform = 0;
        switch (mOrientation) {
            case DisplayState::eOrientationDefault:
                transform = ui::Transform::ROT_0;
                break;
            case DisplayState::eOrientation90:
                transform = ui::Transform::ROT_90;
                break;
            case DisplayState::eOrientation180:
                transform = ui::Transform::ROT_180;
                break;
            case DisplayState::eOrientation270:
                transform = ui::Transform::ROT_270;
                break;
        }
        sPrimaryDisplayOrientation = transform;
    }
    mViewport = viewport;
    mFrame = frame;
}

uint32_t DisplayDevice::getPrimaryDisplayOrientationTransform() {
    return sPrimaryDisplayOrientation;
}

std::string DisplayDevice::getDebugName() const {
    const auto id = mId ? to_string(*mId) + ", " : std::string();
    return base::StringPrintf("DisplayDevice{%s%s%s\"%s\"}", id.c_str(),
                              isPrimary() ? "primary, " : "", isVirtual() ? "virtual, " : "",
                              mDisplayName.c_str());
}

void DisplayDevice::dump(std::string& result) const {
    const ui::Transform& tr(mGlobalTransform);
    ANativeWindow* const window = mNativeWindow.get();
    StringAppendF(&result, "+ %s\n", getDebugName().c_str());
    StringAppendF(&result,
                  "  layerStack=%u, (%4dx%4d), ANativeWindow=%p "
                  "format=%d, orient=%2d (type=%08x), flips=%u, isSecure=%d, "
                  "powerMode=%d, activeConfig=%d, numLayers=%zu\n",
                  mLayerStack, mDisplayWidth, mDisplayHeight, window,
                  ANativeWindow_getFormat(window), mOrientation, tr.getType(), getPageFlipCount(),
                  mIsSecure, mPowerMode, mActiveConfig, mVisibleLayersSortedByZ.size());
    StringAppendF(&result,
                  "   v:[%d,%d,%d,%d], f:[%d,%d,%d,%d], s:[%d,%d,%d,%d],"
                  "transform:[[%0.3f,%0.3f,%0.3f][%0.3f,%0.3f,%0.3f][%0.3f,%0.3f,%0.3f]]\n",
                  mViewport.left, mViewport.top, mViewport.right, mViewport.bottom, mFrame.left,
                  mFrame.top, mFrame.right, mFrame.bottom, mScissor.left, mScissor.top,
                  mScissor.right, mScissor.bottom, tr[0][0], tr[1][0], tr[2][0], tr[0][1], tr[1][1],
                  tr[2][1], tr[0][2], tr[1][2], tr[2][2]);
    auto const surface = static_cast<Surface*>(window);
    ui::Dataspace dataspace = surface->getBuffersDataSpace();
    StringAppendF(&result,
                  "   wideColorGamut=%d, hdr10plus =%d, hdr10=%d, colorMode=%s, dataspace: %s "
                  "(%d)\n",
                  mHasWideColorGamut, mHasHdr10Plus, mHasHdr10,
                  decodeColorMode(mActiveColorMode).c_str(),
                  dataspaceDetails(static_cast<android_dataspace>(dataspace)).c_str(), dataspace);

    String8 surfaceDump;
    mDisplaySurface->dumpAsString(surfaceDump);
    result.append(surfaceDump.string(), surfaceDump.size());
}

// Map dataspace/intent to the best matched dataspace/colorMode/renderIntent
// supported by HWC.
void DisplayDevice::addColorMode(
        const std::unordered_map<ColorMode, std::vector<RenderIntent>>& hwcColorModes,
        const ColorMode mode, const RenderIntent intent) {
    // find the best color mode
    const ColorMode hwcColorMode = getHwcColorMode(hwcColorModes, mode);

    // find the best render intent
    auto iter = hwcColorModes.find(hwcColorMode);
    const auto& hwcIntents =
            iter != hwcColorModes.end() ? iter->second : std::vector<RenderIntent>();
    const RenderIntent hwcIntent = getHwcRenderIntent(hwcIntents, intent);

    const Dataspace dataspace = colorModeToDataspace(mode);
    const Dataspace hwcDataspace = colorModeToDataspace(hwcColorMode);

    ALOGV("%s: map (%s, %s) to (%s, %s, %s)", getDebugName().c_str(),
          dataspaceDetails(static_cast<android_dataspace_t>(dataspace)).c_str(),
          decodeRenderIntent(intent).c_str(),
          dataspaceDetails(static_cast<android_dataspace_t>(hwcDataspace)).c_str(),
          decodeColorMode(hwcColorMode).c_str(), decodeRenderIntent(hwcIntent).c_str());

    mColorModes[getColorModeKey(dataspace, intent)] = {hwcDataspace, hwcColorMode, hwcIntent};
}

void DisplayDevice::populateColorModes(
        const std::unordered_map<ColorMode, std::vector<RenderIntent>>& hwcColorModes) {
    if (!hasWideColorGamut()) {
        return;
    }

    // collect all known SDR render intents
    std::unordered_set<RenderIntent> sdrRenderIntents(sSdrRenderIntents.begin(),
                                                      sSdrRenderIntents.end());
    auto iter = hwcColorModes.find(ColorMode::SRGB);
    if (iter != hwcColorModes.end()) {
        for (auto intent : iter->second) {
            sdrRenderIntents.insert(intent);
        }
    }

    // add all known SDR combinations
    for (auto intent : sdrRenderIntents) {
        for (auto mode : sSdrColorModes) {
            addColorMode(hwcColorModes, mode, intent);
        }
    }

    // collect all known HDR render intents
    std::unordered_set<RenderIntent> hdrRenderIntents(sHdrRenderIntents.begin(),
                                                      sHdrRenderIntents.end());
    iter = hwcColorModes.find(ColorMode::BT2100_PQ);
    if (iter != hwcColorModes.end()) {
        for (auto intent : iter->second) {
            hdrRenderIntents.insert(intent);
        }
    }

    // add all known HDR combinations
    for (auto intent : sHdrRenderIntents) {
        for (auto mode : sHdrColorModes) {
            addColorMode(hwcColorModes, mode, intent);
        }
    }
}

bool DisplayDevice::hasRenderIntent(RenderIntent intent) const {
    // assume a render intent is supported when SRGB supports it; we should
    // get rid of that assumption.
    auto iter = mColorModes.find(getColorModeKey(Dataspace::V0_SRGB, intent));
    return iter != mColorModes.end() && iter->second.renderIntent == intent;
}

bool DisplayDevice::hasLegacyHdrSupport(Dataspace dataspace) const {
    if ((dataspace == Dataspace::BT2020_PQ && hasHDR10Support()) ||
        (dataspace == Dataspace::BT2020_HLG && hasHLGSupport())) {
        auto iter =
                mColorModes.find(getColorModeKey(dataspace, RenderIntent::TONE_MAP_COLORIMETRIC));
        return iter == mColorModes.end() || iter->second.dataspace != dataspace;
    }

    return false;
}

void DisplayDevice::getBestColorMode(Dataspace dataspace, RenderIntent intent,
                                     Dataspace* outDataspace, ColorMode* outMode,
                                     RenderIntent* outIntent) const {
    auto iter = mColorModes.find(getColorModeKey(dataspace, intent));
    if (iter != mColorModes.end()) {
        *outDataspace = iter->second.dataspace;
        *outMode = iter->second.colorMode;
        *outIntent = iter->second.renderIntent;
    } else {
        // this is unexpected on a WCG display
        if (hasWideColorGamut()) {
            ALOGE("map unknown (%s)/(%s) to default color mode",
                  dataspaceDetails(static_cast<android_dataspace_t>(dataspace)).c_str(),
                  decodeRenderIntent(intent).c_str());
        }

        *outDataspace = Dataspace::UNKNOWN;
        *outMode = ColorMode::NATIVE;
        *outIntent = RenderIntent::COLORIMETRIC;
    }
}

std::atomic<int32_t> DisplayDeviceState::sNextSequenceId(1);

}  // namespace android
