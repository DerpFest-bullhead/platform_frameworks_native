/*
 * Copyright (C) 2010 The Android Open Source Project
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
#define LOG_TAG "HWComposer"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <utils/Errors.h>
#include <utils/misc.h>
#include <utils/NativeHandle.h>
#include <utils/String8.h>
#include <utils/Thread.h>
#include <utils/Trace.h>
#include <utils/Vector.h>

#include <ui/DebugUtils.h>
#include <ui/GraphicBuffer.h>

#include <hardware/hardware.h>
#include <hardware/hwcomposer.h>

#include <android/configuration.h>

#include <cutils/properties.h>
#include <log/log.h>

#include "HWComposer.h"
#include "HWC2.h"
#include "ComposerHal.h"

#include "../Layer.h"           // needed only for debugging
#include "../SurfaceFlinger.h"

#define LOG_HWC_DISPLAY_ERROR(hwcDisplayId, msg) \
    ALOGE("%s failed for HWC display %" PRIu64 ": %s", __FUNCTION__, hwcDisplayId, msg)

#define LOG_DISPLAY_ERROR(displayId, msg) \
    ALOGE("%s failed for display %d: %s", __FUNCTION__, displayId, msg)

#define LOG_HWC_ERROR(what, error, displayId)                                     \
    ALOGE("%s: %s failed for display %d: %s (%d)", __FUNCTION__, what, displayId, \
          to_string(error).c_str(), static_cast<int32_t>(error))

#define RETURN_IF_INVALID_DISPLAY(displayId, ...)            \
    do {                                                     \
        if (!isValidDisplay(displayId)) {                    \
            LOG_DISPLAY_ERROR(displayId, "Invalid display"); \
            return __VA_ARGS__;                              \
        }                                                    \
    } while (false)

#define RETURN_IF_HWC_ERROR_FOR(what, error, displayId, ...) \
    do {                                                     \
        if (error != HWC2::Error::None) {                    \
            LOG_HWC_ERROR(what, error, displayId);           \
            return __VA_ARGS__;                              \
        }                                                    \
    } while (false)

#define RETURN_IF_HWC_ERROR(error, displayId, ...) \
    RETURN_IF_HWC_ERROR_FOR(__FUNCTION__, error, displayId, __VA_ARGS__)

namespace android {

#define MIN_HWC_HEADER_VERSION HWC_HEADER_VERSION

// ---------------------------------------------------------------------------

HWComposer::HWComposer(std::unique_ptr<android::Hwc2::Composer> composer)
      : mHwcDevice(std::make_unique<HWC2::Device>(std::move(composer))) {}

HWComposer::~HWComposer() = default;

void HWComposer::registerCallback(HWC2::ComposerCallback* callback,
                                  int32_t sequenceId) {
    mHwcDevice->registerCallback(callback, sequenceId);
}

bool HWComposer::getDisplayIdentificationData(hwc2_display_t hwcDisplayId, uint8_t* outPort,
                                              DisplayIdentificationData* outData) const {
    const auto error = mHwcDevice->getDisplayIdentificationData(hwcDisplayId, outPort, outData);
    if (error != HWC2::Error::None) {
        if (error != HWC2::Error::Unsupported) {
            LOG_HWC_DISPLAY_ERROR(hwcDisplayId, to_string(error).c_str());
        }
        return false;
    }
    return true;
}

bool HWComposer::hasCapability(HWC2::Capability capability) const
{
    return mHwcDevice->getCapabilities().count(capability) > 0;
}

bool HWComposer::isValidDisplay(int32_t displayId) const {
    return static_cast<size_t>(displayId) < mDisplayData.size() &&
            mDisplayData[displayId].hwcDisplay;
}

void HWComposer::validateChange(HWC2::Composition from, HWC2::Composition to) {
    bool valid = true;
    switch (from) {
        case HWC2::Composition::Client:
            valid = false;
            break;
        case HWC2::Composition::Device:
        case HWC2::Composition::SolidColor:
            valid = (to == HWC2::Composition::Client);
            break;
        case HWC2::Composition::Cursor:
        case HWC2::Composition::Sideband:
            valid = (to == HWC2::Composition::Client ||
                    to == HWC2::Composition::Device);
            break;
        default:
            break;
    }

    if (!valid) {
        ALOGE("Invalid layer type change: %s --> %s", to_string(from).c_str(),
                to_string(to).c_str());
    }
}

std::optional<DisplayId> HWComposer::onHotplug(hwc2_display_t hwcDisplayId, int32_t displayType,
                                               HWC2::Connection connection) {
    if (displayType >= HWC_NUM_PHYSICAL_DISPLAY_TYPES) {
        ALOGE("Invalid display type of %d", displayType);
        return {};
    }

    ALOGV("hotplug: %" PRIu64 ", %s %s", hwcDisplayId,
          displayType == DisplayDevice::DISPLAY_PRIMARY ? "primary" : "external",
          to_string(connection).c_str());
    auto error = mHwcDevice->onHotplug(hwcDisplayId, connection);
        if (error != HWC2::Error::None) {
        return {};
    }

    std::optional<DisplayId> displayId;

    if (connection == HWC2::Connection::Connected) {
        uint8_t port;
        DisplayIdentificationData data;
        if (getDisplayIdentificationData(hwcDisplayId, &port, &data)) {
            displayId = generateDisplayId(port, data);
            ALOGE_IF(!displayId, "Failed to generate stable ID for display %" PRIu64, hwcDisplayId);
        }
    }

    // Disconnect is handled through HWComposer::disconnectDisplay via
    // SurfaceFlinger's onHotplugReceived callback handling
    if (connection == HWC2::Connection::Connected) {
        mDisplayData[displayType].hwcDisplay = mHwcDevice->getDisplayById(hwcDisplayId);
        mHwcDisplaySlots[hwcDisplayId] = displayType;
    }

    return displayId;
}

bool HWComposer::onVsync(hwc2_display_t hwcDisplayId, int64_t timestamp, int32_t* outDisplayId) {
    const auto it = mHwcDisplaySlots.find(hwcDisplayId);
    if (it == mHwcDisplaySlots.end()) {
        LOG_HWC_DISPLAY_ERROR(hwcDisplayId, "Invalid display");
        return false;
    }

    const int32_t displayId = it->second;
    RETURN_IF_INVALID_DISPLAY(displayId, false);

    const auto& displayData = mDisplayData[displayId];
    if (displayData.isVirtual) {
        LOG_DISPLAY_ERROR(displayId, "Invalid operation on virtual display");
        return false;
    }

    {
        Mutex::Autolock _l(mLock);

        // There have been reports of HWCs that signal several vsync events
        // with the same timestamp when turning the display off and on. This
        // is a bug in the HWC implementation, but filter the extra events
        // out here so they don't cause havoc downstream.
        if (timestamp == mLastHwVSync[displayId]) {
            ALOGW("Ignoring duplicate VSYNC event from HWC (t=%" PRId64 ")",
                    timestamp);
            return false;
        }

        mLastHwVSync[displayId] = timestamp;
    }

    if (outDisplayId) {
        *outDisplayId = displayId;
    }

    char tag[16];
    snprintf(tag, sizeof(tag), "HW_VSYNC_%1u", displayId);
    ATRACE_INT(tag, ++mVSyncCounts[displayId] & 1);

    return true;
}

status_t HWComposer::allocateVirtualDisplay(uint32_t width, uint32_t height,
        ui::PixelFormat* format, int32_t *outId) {
    if (mRemainingHwcVirtualDisplays == 0) {
        ALOGE("%s: No remaining virtual displays", __FUNCTION__);
        return NO_MEMORY;
    }

    if (SurfaceFlinger::maxVirtualDisplaySize != 0 &&
        (width > SurfaceFlinger::maxVirtualDisplaySize ||
         height > SurfaceFlinger::maxVirtualDisplaySize)) {
        ALOGE("%s: Display size %ux%u exceeds maximum dimension of %" PRIu64, __FUNCTION__, width,
              height, SurfaceFlinger::maxVirtualDisplaySize);
        return INVALID_OPERATION;
    }

    HWC2::Display* display;
    auto error = mHwcDevice->createVirtualDisplay(width, height, format,
            &display);
    if (error != HWC2::Error::None) {
        ALOGE("%s: Failed to create HWC virtual display", __FUNCTION__);
        return NO_MEMORY;
    }

    size_t displaySlot = 0;
    if (!mFreeDisplaySlots.empty()) {
        displaySlot = *mFreeDisplaySlots.begin();
        mFreeDisplaySlots.erase(displaySlot);
    } else if (mDisplayData.size() < INT32_MAX) {
        // Don't bother allocating a slot larger than we can return
        displaySlot = mDisplayData.size();
        mDisplayData.resize(displaySlot + 1);
    } else {
        ALOGE("%s: Unable to allocate a display slot", __FUNCTION__);
        return NO_MEMORY;
    }

    auto& displayData = mDisplayData[displaySlot];
    displayData.hwcDisplay = display;
    displayData.isVirtual = true;

    --mRemainingHwcVirtualDisplays;
    *outId = static_cast<int32_t>(displaySlot);

    return NO_ERROR;
}

HWC2::Layer* HWComposer::createLayer(int32_t displayId) {
    RETURN_IF_INVALID_DISPLAY(displayId, nullptr);

    auto display = mDisplayData[displayId].hwcDisplay;
    HWC2::Layer* layer;
    auto error = display->createLayer(&layer);
    RETURN_IF_HWC_ERROR(error, displayId, nullptr);
    return layer;
}

void HWComposer::destroyLayer(int32_t displayId, HWC2::Layer* layer) {
    RETURN_IF_INVALID_DISPLAY(displayId);

    auto display = mDisplayData[displayId].hwcDisplay;
    auto error = display->destroyLayer(layer);
    RETURN_IF_HWC_ERROR(error, displayId);
}

nsecs_t HWComposer::getRefreshTimestamp(int32_t displayId) const {
    // this returns the last refresh timestamp.
    // if the last one is not available, we estimate it based on
    // the refresh period and whatever closest timestamp we have.
    Mutex::Autolock _l(mLock);
    nsecs_t now = systemTime(CLOCK_MONOTONIC);
    auto vsyncPeriod = getActiveConfig(displayId)->getVsyncPeriod();
    return now - ((now - mLastHwVSync[displayId]) % vsyncPeriod);
}

bool HWComposer::isConnected(int32_t displayId) const {
    RETURN_IF_INVALID_DISPLAY(displayId, false);
    return mDisplayData[displayId].hwcDisplay->isConnected();
}

std::vector<std::shared_ptr<const HWC2::Display::Config>>
        HWComposer::getConfigs(int32_t displayId) const {
    RETURN_IF_INVALID_DISPLAY(displayId, {});

    auto& displayData = mDisplayData[displayId];
    auto configs = mDisplayData[displayId].hwcDisplay->getConfigs();
    if (displayData.configMap.empty()) {
        for (size_t i = 0; i < configs.size(); ++i) {
            displayData.configMap[i] = configs[i];
        }
    }
    return configs;
}

std::shared_ptr<const HWC2::Display::Config>
        HWComposer::getActiveConfig(int32_t displayId) const {
    RETURN_IF_INVALID_DISPLAY(displayId, nullptr);

    std::shared_ptr<const HWC2::Display::Config> config;
    auto error = mDisplayData[displayId].hwcDisplay->getActiveConfig(&config);
    if (error == HWC2::Error::BadConfig) {
        LOG_DISPLAY_ERROR(displayId, "No active config");
        return nullptr;
    }

    RETURN_IF_HWC_ERROR(error, displayId, nullptr);

    if (!config) {
        LOG_DISPLAY_ERROR(displayId, "Unknown config");
        return nullptr;
    }

    return config;
}

int HWComposer::getActiveConfigIndex(int32_t displayId) const {
    RETURN_IF_INVALID_DISPLAY(displayId, -1);

    int index;
    auto error = mDisplayData[displayId].hwcDisplay->getActiveConfigIndex(&index);
    if (error == HWC2::Error::BadConfig) {
        LOG_DISPLAY_ERROR(displayId, "No active config");
        return -1;
    }

    RETURN_IF_HWC_ERROR(error, displayId, -1);

    if (index < 0) {
        LOG_DISPLAY_ERROR(displayId, "Unknown config");
        return -1;
    }

    return index;
}

std::vector<ui::ColorMode> HWComposer::getColorModes(int32_t displayId) const {
    RETURN_IF_INVALID_DISPLAY(displayId, {});

    std::vector<ui::ColorMode> modes;
    auto error = mDisplayData[displayId].hwcDisplay->getColorModes(&modes);
    RETURN_IF_HWC_ERROR(error, displayId, {});
    return modes;
}

status_t HWComposer::setActiveColorMode(int32_t displayId, ui::ColorMode mode,
        ui::RenderIntent renderIntent) {
    RETURN_IF_INVALID_DISPLAY(displayId, BAD_INDEX);

    auto& displayData = mDisplayData[displayId];
    auto error = displayData.hwcDisplay->setColorMode(mode, renderIntent);
    RETURN_IF_HWC_ERROR_FOR(("setColorMode(" + decodeColorMode(mode) + ", " +
                             decodeRenderIntent(renderIntent) + ")")
                                    .c_str(),
                            error, displayId, UNKNOWN_ERROR);

    return NO_ERROR;
}


void HWComposer::setVsyncEnabled(int32_t displayId, HWC2::Vsync enabled) {
    RETURN_IF_INVALID_DISPLAY(displayId);
    auto& displayData = mDisplayData[displayId];

    if (displayData.isVirtual) {
        LOG_DISPLAY_ERROR(displayId, "Invalid operation on virtual display");
        return;
    }

    // NOTE: we use our own internal lock here because we have to call
    // into the HWC with the lock held, and we want to make sure
    // that even if HWC blocks (which it shouldn't), it won't
    // affect other threads.
    Mutex::Autolock _l(mVsyncLock);
    if (enabled != displayData.vsyncEnabled) {
        ATRACE_CALL();
        auto error = displayData.hwcDisplay->setVsyncEnabled(enabled);
        RETURN_IF_HWC_ERROR(error, displayId);

        displayData.vsyncEnabled = enabled;

        char tag[16];
        snprintf(tag, sizeof(tag), "HW_VSYNC_ON_%1u", displayId);
        ATRACE_INT(tag, enabled == HWC2::Vsync::Enable ? 1 : 0);
    }
}

status_t HWComposer::setClientTarget(int32_t displayId, uint32_t slot,
        const sp<Fence>& acquireFence, const sp<GraphicBuffer>& target,
        ui::Dataspace dataspace) {
    RETURN_IF_INVALID_DISPLAY(displayId, BAD_INDEX);

    ALOGV("setClientTarget for display %d", displayId);
    auto& hwcDisplay = mDisplayData[displayId].hwcDisplay;
    auto error = hwcDisplay->setClientTarget(slot, target, acquireFence, dataspace);
    RETURN_IF_HWC_ERROR(error, displayId, BAD_VALUE);
    return NO_ERROR;
}

status_t HWComposer::prepare(DisplayDevice& display,
        std::vector<CompositionInfo>& compositionData) {
    ATRACE_CALL();

    Mutex::Autolock _l(mDisplayLock);
    const auto displayId = display.getId();
    if (displayId == DisplayDevice::DISPLAY_ID_INVALID) {
        ALOGV("Skipping HWComposer prepare for non-HWC display");
        return NO_ERROR;
    }

    RETURN_IF_INVALID_DISPLAY(displayId, BAD_INDEX);

    auto& displayData = mDisplayData[displayId];
    auto& hwcDisplay = displayData.hwcDisplay;
    if (!hwcDisplay->isConnected()) {
        return NO_ERROR;
    }

    uint32_t numTypes = 0;
    uint32_t numRequests = 0;

    HWC2::Error error = HWC2::Error::None;

    // First try to skip validate altogether when there is no client
    // composition.  When there is client composition, since we haven't
    // rendered to the client target yet, we should not attempt to skip
    // validate.
    //
    // displayData.hasClientComposition hasn't been updated for this frame.
    // The check below is incorrect.  We actually rely on HWC here to fall
    // back to validate when there is any client layer.
    displayData.validateWasSkipped = false;
    if (!displayData.hasClientComposition) {
        sp<android::Fence> outPresentFence;
        uint32_t state = UINT32_MAX;
        error = hwcDisplay->presentOrValidate(&numTypes, &numRequests, &outPresentFence , &state);
        if (error != HWC2::Error::HasChanges) {
            RETURN_IF_HWC_ERROR_FOR("presentOrValidate", error, displayId, UNKNOWN_ERROR);
        }
        if (state == 1) { //Present Succeeded.
            std::unordered_map<HWC2::Layer*, sp<Fence>> releaseFences;
            error = hwcDisplay->getReleaseFences(&releaseFences);
            displayData.releaseFences = std::move(releaseFences);
            displayData.lastPresentFence = outPresentFence;
            displayData.validateWasSkipped = true;
            displayData.presentError = error;
            return NO_ERROR;
        }
        // Present failed but Validate ran.
    } else {
        error = hwcDisplay->validate(&numTypes, &numRequests);
    }
    ALOGV("SkipValidate failed, Falling back to SLOW validate/present");
    if (error != HWC2::Error::HasChanges) {
        RETURN_IF_HWC_ERROR_FOR("validate", error, displayId, BAD_INDEX);
    }

    std::unordered_map<HWC2::Layer*, HWC2::Composition> changedTypes;
    changedTypes.reserve(numTypes);
    error = hwcDisplay->getChangedCompositionTypes(&changedTypes);
    RETURN_IF_HWC_ERROR_FOR("getChangedCompositionTypes", error, displayId, BAD_INDEX);

    displayData.displayRequests = static_cast<HWC2::DisplayRequest>(0);
    std::unordered_map<HWC2::Layer*, HWC2::LayerRequest> layerRequests;
    layerRequests.reserve(numRequests);
    error = hwcDisplay->getRequests(&displayData.displayRequests,
            &layerRequests);
    RETURN_IF_HWC_ERROR_FOR("getRequests", error, displayId, BAD_INDEX);

    displayData.hasClientComposition = false;
    displayData.hasDeviceComposition = false;
    for (auto& compositionInfo : compositionData) {
        auto hwcLayer = compositionInfo.hwc.hwcLayer;

        if (changedTypes.count(&*hwcLayer) != 0) {
            // We pass false so we only update our state and don't call back
            // into the HWC device
            validateChange(compositionInfo.compositionType,
                    changedTypes[&*hwcLayer]);
            compositionInfo.compositionType = changedTypes[&*hwcLayer];
            compositionInfo.layer->mLayer->setCompositionType(displayId,
                    compositionInfo.compositionType, false);
        }

        switch (compositionInfo.compositionType) {
            case HWC2::Composition::Client:
                displayData.hasClientComposition = true;
                break;
            case HWC2::Composition::Device:
            case HWC2::Composition::SolidColor:
            case HWC2::Composition::Cursor:
            case HWC2::Composition::Sideband:
                displayData.hasDeviceComposition = true;
                break;
            default:
                break;
        }

        if (layerRequests.count(&*hwcLayer) != 0 &&
                layerRequests[&*hwcLayer] ==
                        HWC2::LayerRequest::ClearClientTarget) {
            compositionInfo.hwc.clearClientTarget = true;
            compositionInfo.layer->mLayer->setClearClientTarget(displayId, true);
        } else {
            if (layerRequests.count(&*hwcLayer) != 0) {
                LOG_DISPLAY_ERROR(displayId,
                                  ("Unknown layer request " + to_string(layerRequests[&*hwcLayer]))
                                          .c_str());
            }
            compositionInfo.hwc.clearClientTarget = false;
            compositionInfo.layer->mLayer->setClearClientTarget(displayId, false);
        }
    }

    error = hwcDisplay->acceptChanges();
    RETURN_IF_HWC_ERROR_FOR("acceptChanges", error, displayId, BAD_INDEX);

    return NO_ERROR;
}

bool HWComposer::hasDeviceComposition(int32_t displayId) const {
    if (displayId == DisplayDevice::DISPLAY_ID_INVALID) {
        // Displays without a corresponding HWC display are never composed by
        // the device
        return false;
    }

    RETURN_IF_INVALID_DISPLAY(displayId, false);
    return mDisplayData[displayId].hasDeviceComposition;
}

bool HWComposer::hasFlipClientTargetRequest(int32_t displayId) const {
    if (displayId == DisplayDevice::DISPLAY_ID_INVALID) {
        // Displays without a corresponding HWC display are never composed by
        // the device
        return false;
    }

    RETURN_IF_INVALID_DISPLAY(displayId, false);
    return ((static_cast<uint32_t>(mDisplayData[displayId].displayRequests) &
             static_cast<uint32_t>(HWC2::DisplayRequest::FlipClientTarget)) != 0);
}

bool HWComposer::hasClientComposition(int32_t displayId) const {
    if (displayId == DisplayDevice::DISPLAY_ID_INVALID) {
        // Displays without a corresponding HWC display are always composed by
        // the client
        return true;
    }

    RETURN_IF_INVALID_DISPLAY(displayId, true);
    return mDisplayData[displayId].hasClientComposition;
}

sp<Fence> HWComposer::getPresentFence(int32_t displayId) const {
    RETURN_IF_INVALID_DISPLAY(displayId, Fence::NO_FENCE);
    return mDisplayData[displayId].lastPresentFence;
}

sp<Fence> HWComposer::getLayerReleaseFence(int32_t displayId,
        HWC2::Layer* layer) const {
    RETURN_IF_INVALID_DISPLAY(displayId, Fence::NO_FENCE);
    auto displayFences = mDisplayData[displayId].releaseFences;
    if (displayFences.count(layer) == 0) {
        ALOGV("getLayerReleaseFence: Release fence not found");
        return Fence::NO_FENCE;
    }
    return displayFences[layer];
}

status_t HWComposer::presentAndGetReleaseFences(int32_t displayId) {
    ATRACE_CALL();

    RETURN_IF_INVALID_DISPLAY(displayId, BAD_INDEX);

    auto& displayData = mDisplayData[displayId];
    auto& hwcDisplay = displayData.hwcDisplay;

    if (displayData.validateWasSkipped) {
        // explicitly flush all pending commands
        auto error = mHwcDevice->flushCommands();
        RETURN_IF_HWC_ERROR_FOR("flushCommands", error, displayId, UNKNOWN_ERROR);
        RETURN_IF_HWC_ERROR_FOR("present", displayData.presentError, displayId, UNKNOWN_ERROR);
        return NO_ERROR;
    }

    displayData.lastPresentFence = Fence::NO_FENCE;
    auto error = hwcDisplay->present(&displayData.lastPresentFence);
    RETURN_IF_HWC_ERROR_FOR("present", error, displayId, UNKNOWN_ERROR);

    std::unordered_map<HWC2::Layer*, sp<Fence>> releaseFences;
    error = hwcDisplay->getReleaseFences(&releaseFences);
    RETURN_IF_HWC_ERROR_FOR("getReleaseFences", error, displayId, UNKNOWN_ERROR);

    displayData.releaseFences = std::move(releaseFences);

    return NO_ERROR;
}

status_t HWComposer::setPowerMode(int32_t displayId, int32_t intMode) {
    ALOGV("setPowerMode(%d, %d)", displayId, intMode);
    RETURN_IF_INVALID_DISPLAY(displayId, BAD_INDEX);

    const auto& displayData = mDisplayData[displayId];
    if (displayData.isVirtual) {
        LOG_DISPLAY_ERROR(displayId, "Invalid operation on virtual display");
        return INVALID_OPERATION;
    }

    auto mode = static_cast<HWC2::PowerMode>(intMode);
    if (mode == HWC2::PowerMode::Off) {
        setVsyncEnabled(displayId, HWC2::Vsync::Disable);
    }

    auto& hwcDisplay = displayData.hwcDisplay;
    switch (mode) {
        case HWC2::PowerMode::Off:
        case HWC2::PowerMode::On:
            ALOGV("setPowerMode: Calling HWC %s", to_string(mode).c_str());
            {
                auto error = hwcDisplay->setPowerMode(mode);
                if (error != HWC2::Error::None) {
                    LOG_HWC_ERROR(("setPowerMode(" + to_string(mode) + ")").c_str(),
                                  error, displayId);
                }
            }
            break;
        case HWC2::PowerMode::Doze:
        case HWC2::PowerMode::DozeSuspend:
            ALOGV("setPowerMode: Calling HWC %s", to_string(mode).c_str());
            {
                bool supportsDoze = false;
                auto error = hwcDisplay->supportsDoze(&supportsDoze);
                if (error != HWC2::Error::None) {
                    LOG_HWC_ERROR("supportsDoze", error, displayId);
                }

                if (!supportsDoze) {
                    mode = HWC2::PowerMode::On;
                }

                error = hwcDisplay->setPowerMode(mode);
                if (error != HWC2::Error::None) {
                    LOG_HWC_ERROR(("setPowerMode(" + to_string(mode) + ")").c_str(),
                                  error, displayId);
                }
            }
            break;
        default:
            ALOGV("setPowerMode: Not calling HWC");
            break;
    }

    return NO_ERROR;
}

status_t HWComposer::setActiveConfig(int32_t displayId, size_t configId) {
    RETURN_IF_INVALID_DISPLAY(displayId, BAD_INDEX);

    auto& displayData = mDisplayData[displayId];
    if (displayData.configMap.count(configId) == 0) {
        LOG_DISPLAY_ERROR(displayId, ("Invalid config " + std::to_string(configId)).c_str());
        return BAD_INDEX;
    }

    auto error = displayData.hwcDisplay->setActiveConfig(displayData.configMap[configId]);
    RETURN_IF_HWC_ERROR(error, displayId, UNKNOWN_ERROR);
    return NO_ERROR;
}

status_t HWComposer::setColorTransform(int32_t displayId,
        const mat4& transform) {
    RETURN_IF_INVALID_DISPLAY(displayId, BAD_INDEX);

    auto& displayData = mDisplayData[displayId];
    bool isIdentity = transform == mat4();
    auto error = displayData.hwcDisplay->setColorTransform(transform,
            isIdentity ? HAL_COLOR_TRANSFORM_IDENTITY :
            HAL_COLOR_TRANSFORM_ARBITRARY_MATRIX);
    RETURN_IF_HWC_ERROR(error, displayId, UNKNOWN_ERROR);
    return NO_ERROR;
}

void HWComposer::disconnectDisplay(int32_t displayId) {
    RETURN_IF_INVALID_DISPLAY(displayId);
    auto& displayData = mDisplayData[displayId];

    // If this was a virtual display, add its slot back for reuse by future
    // virtual displays
    if (displayData.isVirtual) {
        mFreeDisplaySlots.insert(displayId);
        ++mRemainingHwcVirtualDisplays;
    }

    const auto hwcDisplayId = displayData.hwcDisplay->getId();
    mHwcDisplaySlots.erase(hwcDisplayId);
    displayData = DisplayData();

    mHwcDevice->destroyDisplay(hwcDisplayId);
}

status_t HWComposer::setOutputBuffer(int32_t displayId,
        const sp<Fence>& acquireFence, const sp<GraphicBuffer>& buffer) {
    RETURN_IF_INVALID_DISPLAY(displayId, BAD_INDEX);
    const auto& displayData = mDisplayData[displayId];

    if (!displayData.isVirtual) {
        LOG_DISPLAY_ERROR(displayId, "Invalid operation on physical display");
        return INVALID_OPERATION;
    }

    auto error = displayData.hwcDisplay->setOutputBuffer(buffer, acquireFence);
    RETURN_IF_HWC_ERROR(error, displayId, UNKNOWN_ERROR);
    return NO_ERROR;
}

void HWComposer::clearReleaseFences(int32_t displayId) {
    RETURN_IF_INVALID_DISPLAY(displayId);
    mDisplayData[displayId].releaseFences.clear();
}

status_t HWComposer::getHdrCapabilities(
        int32_t displayId, HdrCapabilities* outCapabilities) {
    RETURN_IF_INVALID_DISPLAY(displayId, BAD_INDEX);

    auto& hwcDisplay = mDisplayData[displayId].hwcDisplay;
    auto error = hwcDisplay->getHdrCapabilities(outCapabilities);
    RETURN_IF_HWC_ERROR(error, displayId, UNKNOWN_ERROR);
    return NO_ERROR;
}

int32_t HWComposer::getSupportedPerFrameMetadata(int32_t displayId) const {
    RETURN_IF_INVALID_DISPLAY(displayId, 0);
    return mDisplayData[displayId].hwcDisplay->getSupportedPerFrameMetadata();
}

std::vector<ui::RenderIntent> HWComposer::getRenderIntents(int32_t displayId,
        ui::ColorMode colorMode) const {
    RETURN_IF_INVALID_DISPLAY(displayId, {});

    std::vector<ui::RenderIntent> renderIntents;
    auto error = mDisplayData[displayId].hwcDisplay->getRenderIntents(colorMode, &renderIntents);
    RETURN_IF_HWC_ERROR(error, displayId, {});
    return renderIntents;
}

mat4 HWComposer::getDataspaceSaturationMatrix(int32_t displayId, ui::Dataspace dataspace) {
    RETURN_IF_INVALID_DISPLAY(displayId, {});

    mat4 matrix;
    auto error = mDisplayData[displayId].hwcDisplay->getDataspaceSaturationMatrix(dataspace,
            &matrix);
    RETURN_IF_HWC_ERROR(error, displayId, {});
    return matrix;
}

// Converts a PixelFormat to a human-readable string.  Max 11 chars.
// (Could use a table of prefab String8 objects.)
/*
static String8 getFormatStr(PixelFormat format) {
    switch (format) {
    case PIXEL_FORMAT_RGBA_8888:    return String8("RGBA_8888");
    case PIXEL_FORMAT_RGBX_8888:    return String8("RGBx_8888");
    case PIXEL_FORMAT_RGB_888:      return String8("RGB_888");
    case PIXEL_FORMAT_RGB_565:      return String8("RGB_565");
    case PIXEL_FORMAT_BGRA_8888:    return String8("BGRA_8888");
    case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED:
                                    return String8("ImplDef");
    default:
        String8 result;
        result.appendFormat("? %08x", format);
        return result;
    }
}
*/

bool HWComposer::isUsingVrComposer() const {
    return getComposer()->isUsingVrComposer();
}

void HWComposer::dump(String8& result) const {
    // TODO: In order to provide a dump equivalent to HWC1, we need to shadow
    // all the state going into the layers. This is probably better done in
    // Layer itself, but it's going to take a bit of work to get there.
    result.append(mHwcDevice->dump().c_str());
}

std::optional<hwc2_display_t>
HWComposer::getHwcDisplayId(int32_t displayId) const {
    if (!isValidDisplay(displayId)) {
        return {};
    }
    return mDisplayData[displayId].hwcDisplay->getId();
}

void HWComposer::setDisplayFrequencyScaleParameters(
        HWC2::Device::FrequencyScaler frequencyScaler)
{
    mHwcDevice->setDisplayFrequencyScaleParameters(frequencyScaler);
}

HWC2::Device::FrequencyScaler HWComposer::getDisplayFrequencyScaleParameters()
{
    return mHwcDevice->getDisplayFrequencyScaleParameters();
}

} // namespace android
