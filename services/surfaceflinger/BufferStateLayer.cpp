/*
 * Copyright (C) 2017 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#undef LOG_TAG
#define LOG_TAG "BufferStateLayer"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "BufferStateLayer.h"

#include "TimeStats/TimeStats.h"

#include <private/gui/SyncFeatures.h>
#include <renderengine/Image.h>

#include <limits>

namespace android {

// clang-format off
const std::array<float, 16> BufferStateLayer::IDENTITY_MATRIX{
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
};
// clang-format on

BufferStateLayer::BufferStateLayer(const LayerCreationArgs& args) : BufferLayer(args) {
    mOverrideScalingMode = NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW;
}
BufferStateLayer::~BufferStateLayer() = default;

// -----------------------------------------------------------------------
// Interface implementation for Layer
// -----------------------------------------------------------------------
void BufferStateLayer::onLayerDisplayed(const sp<Fence>& releaseFence) {
    // The transaction completed callback can only be sent if the release fence from the PREVIOUS
    // frame has fired. In practice, we should never actually wait on the previous release fence
    // but we should store it just in case.
    mPreviousReleaseFence = releaseFence;
}

void BufferStateLayer::setTransformHint(uint32_t /*orientation*/) const {
    // TODO(marissaw): send the transform hint to buffer owner
    return;
}

void BufferStateLayer::releasePendingBuffer(nsecs_t /*dequeueReadyTime*/) {
    return;
}

bool BufferStateLayer::shouldPresentNow(nsecs_t /*expectedPresentTime*/) const {
    if (getSidebandStreamChanged() || getAutoRefresh()) {
        return true;
    }

    return hasFrameUpdate();
}

bool BufferStateLayer::willPresentCurrentTransaction() const {
    Mutex::Autolock lock(mStateMutex);
    // Returns true if the most recent Transaction applied to CurrentState will be presented.
    return getSidebandStreamChanged() || getAutoRefresh() ||
            (mState.current.modified && mState.current.buffer != nullptr);
}

bool BufferStateLayer::getTransformToDisplayInverseLocked() const {
    return mState.current.transformToDisplayInverse;
}

void BufferStateLayer::pushPendingStateLocked() {
    if (!mState.current.modified) {
        return;
    }
    mState.pending.push_back(mState.current);
    ATRACE_INT(mTransactionName.string(), mState.pending.size());
}

bool BufferStateLayer::applyPendingStates(Layer::State* stateToCommit) {
    const bool stateUpdateAvailable = !mState.pending.empty();
    while (!mState.pending.empty()) {
        popPendingState(stateToCommit);
    }
    mCurrentStateModified = stateUpdateAvailable && mState.current.modified;
    mState.current.modified = false;
    return stateUpdateAvailable;
}

// Crop that applies to the window
Rect BufferStateLayer::getCrop(const Layer::State& /*s*/) const {
    return Rect::INVALID_RECT;
}

bool BufferStateLayer::setTransform(uint32_t transform) {
    Mutex::Autolock lock(mStateMutex);
    if (mState.current.transform == transform) return false;
    mState.current.sequence++;
    mState.current.transform = transform;
    mState.current.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool BufferStateLayer::setTransformToDisplayInverse(bool transformToDisplayInverse) {
    Mutex::Autolock lock(mStateMutex);
    if (mState.current.transformToDisplayInverse == transformToDisplayInverse) return false;
    mState.current.sequence++;
    mState.current.transformToDisplayInverse = transformToDisplayInverse;
    mState.current.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool BufferStateLayer::setCrop(const Rect& crop) {
    Mutex::Autolock lock(mStateMutex);
    if (mState.current.crop == crop) return false;
    mState.current.sequence++;
    mState.current.crop = crop;
    mState.current.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool BufferStateLayer::setFrame(const Rect& frame) {
    int x = frame.left;
    int y = frame.top;
    int w = frame.getWidth();
    int h = frame.getHeight();

    Mutex::Autolock lock(mStateMutex);
    if (mState.current.active.transform.tx() == x && mState.current.active.transform.ty() == y &&
        mState.current.active.w == w && mState.current.active.h == h) {
        return false;
    }

    if (!frame.isValid()) {
        x = y = w = h = 0;
    }
    mState.current.active.transform.set(x, y);
    mState.current.active.w = w;
    mState.current.active.h = h;

    mState.current.sequence++;
    mState.current.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool BufferStateLayer::setBuffer(const sp<GraphicBuffer>& buffer) {
    Mutex::Autolock lock(mStateMutex);
    if (mState.current.buffer) {
        mReleasePreviousBuffer = true;
    }

    mState.current.sequence++;
    mState.current.buffer = buffer;
    mState.current.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool BufferStateLayer::setAcquireFence(const sp<Fence>& fence) {
    Mutex::Autolock lock(mStateMutex);
    // The acquire fences of BufferStateLayers have already signaled before they are set
    mCallbackHandleAcquireTime = fence->getSignalTime();

    mState.current.acquireFence = fence;
    mState.current.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool BufferStateLayer::setDataspace(ui::Dataspace dataspace) {
    Mutex::Autolock lock(mStateMutex);
    if (mState.current.dataspace == dataspace) return false;
    mState.current.sequence++;
    mState.current.dataspace = dataspace;
    mState.current.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool BufferStateLayer::setHdrMetadata(const HdrMetadata& hdrMetadata) {
    Mutex::Autolock lock(mStateMutex);
    if (mState.current.hdrMetadata == hdrMetadata) return false;
    mState.current.sequence++;
    mState.current.hdrMetadata = hdrMetadata;
    mState.current.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool BufferStateLayer::setSurfaceDamageRegion(const Region& surfaceDamage) {
    Mutex::Autolock lock(mStateMutex);
    mState.current.sequence++;
    mState.current.surfaceDamageRegion = surfaceDamage;
    mState.current.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool BufferStateLayer::setApi(int32_t api) {
    Mutex::Autolock lock(mStateMutex);
    if (mState.current.api == api) return false;
    mState.current.sequence++;
    mState.current.api = api;
    mState.current.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool BufferStateLayer::setSidebandStream(const sp<NativeHandle>& sidebandStream) {
    Mutex::Autolock lock(mStateMutex);
    if (mState.current.sidebandStream == sidebandStream) return false;
    mState.current.sequence++;
    mState.current.sidebandStream = sidebandStream;
    mState.current.modified = true;
    setTransactionFlags(eTransactionNeeded);

    if (!mSidebandStreamChanged.exchange(true)) {
        // mSidebandStreamChanged was false
        mFlinger->signalLayerUpdate();
    }
    return true;
}

bool BufferStateLayer::setTransactionCompletedListeners(
        const std::vector<sp<CallbackHandle>>& handles) {
    // If there is no handle, we will not send a callback so reset mReleasePreviousBuffer and return
    if (handles.empty()) {
        mReleasePreviousBuffer = false;
        return false;
    }

    const bool willPresent = willPresentCurrentTransaction();

    for (const auto& handle : handles) {
        // If this transaction set a buffer on this layer, release its previous buffer
        handle->releasePreviousBuffer = mReleasePreviousBuffer;

        // If this layer will be presented in this frame
        if (willPresent) {
            // If this transaction set an acquire fence on this layer, set its acquire time
            handle->acquireTime = mCallbackHandleAcquireTime;

            // Notify the transaction completed thread that there is a pending latched callback
            // handle
            mFlinger->getTransactionCompletedThread().registerPendingLatchedCallbackHandle(handle);

            // Store so latched time and release fence can be set
            {
                Mutex::Autolock lock(mStateMutex);
                mState.current.callbackHandles.push_back(handle);
            }

        } else { // If this layer will NOT need to be relatched and presented this frame
            // Notify the transaction completed thread this handle is done
            mFlinger->getTransactionCompletedThread().addUnlatchedCallbackHandle(handle);
        }
    }

    mReleasePreviousBuffer = false;
    mCallbackHandleAcquireTime = -1;

    return willPresent;
}

bool BufferStateLayer::setTransparentRegionHint(const Region& transparent) {
    Mutex::Autolock lock(mStateMutex);
    mState.current.transparentRegionHint = transparent;
    mState.current.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

Rect BufferStateLayer::getBufferSize(const State& s) const {
    // for buffer state layers we use the display frame size as the buffer size.
    if (getActiveWidth(s) < UINT32_MAX && getActiveHeight(s) < UINT32_MAX) {
        return Rect(getActiveWidth(s), getActiveHeight(s));
    }

    // if the display frame is not defined, use the parent bounds as the buffer size.
    const auto& p = mDrawingParent.promote();
    if (p != nullptr) {
        Rect parentBounds = Rect(p->computeBounds(Region()));
        if (!parentBounds.isEmpty()) {
            return parentBounds;
        }
    }

    // if there is no parent layer, use the buffer's bounds as the buffer size
    if (s.buffer) {
        return s.buffer->getBounds();
    }
    return Rect::INVALID_RECT;
}
// -----------------------------------------------------------------------

// -----------------------------------------------------------------------
// Interface implementation for BufferLayer
// -----------------------------------------------------------------------
bool BufferStateLayer::fenceHasSignaled() const {
    if (latchUnsignaledBuffers()) {
        return true;
    }

    Mutex::Autolock lock(mStateMutex);
    return getDrawingState().acquireFence->getStatus() == Fence::Status::Signaled;
}

nsecs_t BufferStateLayer::getDesiredPresentTime() {
    // TODO(marissaw): support an equivalent to desiredPresentTime for timestats metrics
    return 0;
}

std::shared_ptr<FenceTime> BufferStateLayer::getCurrentFenceTimeLocked() const {
    return std::make_shared<FenceTime>(getDrawingState().acquireFence);
}

void BufferStateLayer::getDrawingTransformMatrix(float *matrix) {
    std::copy(std::begin(mTransformMatrix), std::end(mTransformMatrix), matrix);
}

uint32_t BufferStateLayer::getDrawingTransform() const {
    return getDrawingState().transform;
}

ui::Dataspace BufferStateLayer::getDrawingDataSpace() const {
    return getDrawingState().dataspace;
}

// Crop that applies to the buffer
Rect BufferStateLayer::getDrawingCrop() const {
    const State& s(getDrawingState());

    if (s.crop.isEmpty() && s.buffer) {
        return s.buffer->getBounds();
    } else if (s.buffer) {
        Rect crop = s.crop;
        crop.left = std::max(crop.left, 0);
        crop.top = std::max(crop.top, 0);
        uint32_t bufferWidth = s.buffer->getWidth();
        uint32_t bufferHeight = s.buffer->getHeight();
        if (bufferHeight <= std::numeric_limits<int32_t>::max() &&
            bufferWidth <= std::numeric_limits<int32_t>::max()) {
            crop.right = std::min(crop.right, static_cast<int32_t>(bufferWidth));
            crop.bottom = std::min(crop.bottom, static_cast<int32_t>(bufferHeight));
        }
        if (!crop.isValid()) {
            // Crop rect is out of bounds, return whole buffer
            return s.buffer->getBounds();
        }
        return crop;
    }
    return s.crop;
}

uint32_t BufferStateLayer::getDrawingScalingMode() const {
    return NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW;
}

Region BufferStateLayer::getDrawingSurfaceDamage() const {
    Mutex::Autolock lock(mStateMutex);
    return getDrawingState().surfaceDamageRegion;
}

const HdrMetadata& BufferStateLayer::getDrawingHdrMetadata() const {
    Mutex::Autolock lock(mStateMutex);
    return getDrawingState().hdrMetadata;
}

int BufferStateLayer::getDrawingApi() const {
    Mutex::Autolock lock(mStateMutex);
    return getDrawingState().api;
}

PixelFormat BufferStateLayer::getPixelFormat() const {
    if (!mActiveBuffer) {
        return PIXEL_FORMAT_NONE;
    }
    return mActiveBuffer->format;
}

uint64_t BufferStateLayer::getFrameNumber() const {
    return mFrameNumber;
}

bool BufferStateLayer::getAutoRefresh() const {
    // TODO(marissaw): support shared buffer mode
    return false;
}

bool BufferStateLayer::getSidebandStreamChanged() const {
    return mSidebandStreamChanged.load();
}

std::optional<Region> BufferStateLayer::latchSidebandStream(bool& recomputeVisibleRegions) {
    Mutex::Autolock lock(mStateMutex);
    if (mSidebandStreamChanged.exchange(false)) {
        const State& s(getDrawingState());
        // mSidebandStreamChanged was true
        // replicated in LayerBE until FE/BE is ready to be synchronized
        getBE().compositionInfo.hwc.sidebandStream = s.sidebandStream;
        if (getBE().compositionInfo.hwc.sidebandStream != nullptr) {
            setTransactionFlags(eTransactionNeeded);
            mFlinger->setTransactionFlags(eTraversalNeeded);
        }
        recomputeVisibleRegions = true;

        return getTransformLocked().transform(Region(Rect(s.active.w, s.active.h)));
    }
    return {};
}

bool BufferStateLayer::hasFrameUpdateLocked() const {
    return mCurrentStateModified && getCurrentState().buffer != nullptr;
}

void BufferStateLayer::setFilteringEnabled(bool enabled) {
    GLConsumer::computeTransformMatrix(mTransformMatrix.data(), mActiveBuffer, mCurrentCrop,
                                       mCurrentTransform, enabled);
}

status_t BufferStateLayer::bindTextureImage() {
    Mutex::Autolock lock(mStateMutex);
    return bindTextureImageLocked();
}
status_t BufferStateLayer::bindTextureImageLocked() {
    const State& s(getDrawingState());
    auto& engine(mFlinger->getRenderEngine());

    engine.checkErrors();

    // TODO(marissaw): once buffers are cached, don't create a new image everytime
    mTextureImage = engine.createImage();

    bool created =
            mTextureImage->setNativeWindowBuffer(s.buffer->getNativeBuffer(),
                                                 s.buffer->getUsage() & GRALLOC_USAGE_PROTECTED);
    if (!created) {
        ALOGE("Failed to create image. size=%ux%u st=%u usage=%#" PRIx64 " fmt=%d",
              s.buffer->getWidth(), s.buffer->getHeight(), s.buffer->getStride(),
              s.buffer->getUsage(), s.buffer->getPixelFormat());
        engine.bindExternalTextureImage(mTextureName, *engine.createImage());
        return NO_INIT;
    }

    engine.bindExternalTextureImage(mTextureName, *mTextureImage);

    // Wait for the new buffer to be ready.
    if (s.acquireFence->isValid()) {
        if (SyncFeatures::getInstance().useWaitSync()) {
            base::unique_fd fenceFd(s.acquireFence->dup());
            if (fenceFd == -1) {
                ALOGE("error dup'ing fence fd: %d", errno);
                return -errno;
            }
            if (!engine.waitFence(std::move(fenceFd))) {
                ALOGE("failed to wait on fence fd");
                return UNKNOWN_ERROR;
            }
        } else {
            status_t err = s.acquireFence->waitForever("BufferStateLayer::bindTextureImage");
            if (err != NO_ERROR) {
                ALOGE("error waiting for fence: %d", err);
                return err;
            }
        }
    }

    return NO_ERROR;
}

status_t BufferStateLayer::updateTexImage(bool& /*recomputeVisibleRegions*/, nsecs_t latchTime,
                                          const sp<Fence>& releaseFence) {
    const State& s(getDrawingState());

    if (!s.buffer) {
        return NO_ERROR;
    }

    const int32_t layerID = getSequence();

    // Reject if the layer is invalid
    uint32_t bufferWidth = s.buffer->width;
    uint32_t bufferHeight = s.buffer->height;

    if (s.transform & ui::Transform::ROT_90) {
        std::swap(bufferWidth, bufferHeight);
    }

    if (s.transformToDisplayInverse) {
        uint32_t invTransform = DisplayDevice::getPrimaryDisplayOrientationTransform();
        if (invTransform & ui::Transform::ROT_90) {
            std::swap(bufferWidth, bufferHeight);
        }
    }

    if (getEffectiveScalingMode() == NATIVE_WINDOW_SCALING_MODE_FREEZE &&
        (s.active.w != bufferWidth || s.active.h != bufferHeight)) {
        ALOGE("[%s] rejecting buffer: "
              "bufferWidth=%d, bufferHeight=%d, front.active.{w=%d, h=%d}",
              mName.string(), bufferWidth, bufferHeight, s.active.w, s.active.h);
        mFlinger->mTimeStats->removeTimeRecord(layerID, getFrameNumber());
        return BAD_VALUE;
    }

    mFlinger->getTransactionCompletedThread()
            .addLatchedCallbackHandles(getDrawingState().callbackHandles, latchTime,
                                       mPreviousReleaseFence);

    // Handle sync fences
    if (SyncFeatures::getInstance().useNativeFenceSync() && releaseFence != Fence::NO_FENCE) {
        // TODO(alecmouri): Fail somewhere upstream if the fence is invalid.
        if (!releaseFence->isValid()) {
            mFlinger->mTimeStats->onDestroy(layerID);
            return UNKNOWN_ERROR;
        }

        // Check status of fences first because merging is expensive.
        // Merging an invalid fence with any other fence results in an
        // invalid fence.
        auto currentStatus = s.acquireFence->getStatus();
        if (currentStatus == Fence::Status::Invalid) {
            ALOGE("Existing fence has invalid state");
            mFlinger->mTimeStats->onDestroy(layerID);
            return BAD_VALUE;
        }

        auto incomingStatus = releaseFence->getStatus();
        if (incomingStatus == Fence::Status::Invalid) {
            ALOGE("New fence has invalid state");
            mState.drawing.acquireFence = releaseFence;
            mFlinger->mTimeStats->onDestroy(layerID);
            return BAD_VALUE;
        }

        // If both fences are signaled or both are unsignaled, we need to merge
        // them to get an accurate timestamp.
        if (currentStatus == incomingStatus) {
            char fenceName[32] = {};
            snprintf(fenceName, 32, "%.28s:%d", mName.string(), mFrameNumber);
            sp<Fence> mergedFence =
                    Fence::merge(fenceName, mState.drawing.acquireFence, releaseFence);
            if (!mergedFence.get()) {
                ALOGE("failed to merge release fences");
                // synchronization is broken, the best we can do is hope fences
                // signal in order so the new fence will act like a union
                mState.drawing.acquireFence = releaseFence;
                mFlinger->mTimeStats->onDestroy(layerID);
                return BAD_VALUE;
            }
            mState.drawing.acquireFence = mergedFence;
        } else if (incomingStatus == Fence::Status::Unsignaled) {
            // If one fence has signaled and the other hasn't, the unsignaled
            // fence will approximately correspond with the correct timestamp.
            // There's a small race if both fences signal at about the same time
            // and their statuses are retrieved with unfortunate timing. However,
            // by this point, they will have both signaled and only the timestamp
            // will be slightly off; any dependencies after this point will
            // already have been met.
            mState.drawing.acquireFence = releaseFence;
        }
    } else {
        // Bind the new buffer to the GL texture.
        //
        // Older devices require the "implicit" synchronization provided
        // by glEGLImageTargetTexture2DOES, which this method calls.  Newer
        // devices will either call this in Layer::onDraw, or (if it's not
        // a GL-composited layer) not at all.
        status_t err = bindTextureImageLocked();
        if (err != NO_ERROR) {
            mFlinger->mTimeStats->onDestroy(layerID);
            return BAD_VALUE;
        }
    }

    // TODO(marissaw): properly support mTimeStats
    mFlinger->mTimeStats->setPostTime(layerID, getFrameNumber(), getName().c_str(), latchTime);
    mFlinger->mTimeStats->setAcquireFence(layerID, getFrameNumber(), getCurrentFenceTimeLocked());
    mFlinger->mTimeStats->setLatchTime(layerID, getFrameNumber(), latchTime);

    return NO_ERROR;
}

status_t BufferStateLayer::updateActiveBuffer() {
    const State& s(getDrawingState());

    if (s.buffer == nullptr) {
        return BAD_VALUE;
    }

    mActiveBuffer = s.buffer;
    getBE().compositionInfo.mBuffer = mActiveBuffer;
    getBE().compositionInfo.mBufferSlot = 0;

    return NO_ERROR;
}

status_t BufferStateLayer::updateFrameNumber(nsecs_t /*latchTime*/) {
    // TODO(marissaw): support frame history events
    mCurrentFrameNumber = mFrameNumber;
    return NO_ERROR;
}

void BufferStateLayer::setHwcLayerBuffer(DisplayId displayId) {
    Mutex::Autolock lock(mStateMutex);
    auto& hwcInfo = getBE().mHwcLayers[displayId];
    auto& hwcLayer = hwcInfo.layer;

    const State& s(getDrawingState());

    // TODO(marissaw): support more than one slot
    uint32_t hwcSlot = 0;

    auto error = hwcLayer->setBuffer(hwcSlot, s.buffer, s.acquireFence);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set buffer %p: %s (%d)", mName.string(),
              s.buffer->handle, to_string(error).c_str(), static_cast<int32_t>(error));
    }

    mCurrentStateModified = false;
    mFrameNumber++;
}

void BufferStateLayer::onFirstRef() {
    BufferLayer::onFirstRef();

    if (const auto display = mFlinger->getDefaultDisplayDevice()) {
        updateTransformHint(display);
    }
}

} // namespace android
