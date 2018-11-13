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

#include "../InputDispatcher.h"

#include <gtest/gtest.h>
#include <linux/input.h>

namespace android {

// An arbitrary time value.
static const nsecs_t ARBITRARY_TIME = 1234;

// An arbitrary device id.
static const int32_t DEVICE_ID = 1;

// An arbitrary display id.
static const int32_t DISPLAY_ID = ADISPLAY_ID_DEFAULT;

// An arbitrary injector pid / uid pair that has permission to inject events.
static const int32_t INJECTOR_PID = 999;
static const int32_t INJECTOR_UID = 1001;


// --- FakeInputDispatcherPolicy ---

class FakeInputDispatcherPolicy : public InputDispatcherPolicyInterface {
    InputDispatcherConfiguration mConfig;

protected:
    virtual ~FakeInputDispatcherPolicy() {
    }

public:
    FakeInputDispatcherPolicy() {
    }

private:
    virtual void notifyConfigurationChanged(nsecs_t) {
    }

    virtual nsecs_t notifyANR(const sp<InputApplicationHandle>&,
            const sp<InputWindowHandle>&,
            const std::string&) {
        return 0;
    }

    virtual void notifyInputChannelBroken(const sp<InputWindowHandle>&) {
    }

    virtual void getDispatcherConfiguration(InputDispatcherConfiguration* outConfig) {
        *outConfig = mConfig;
    }

    virtual bool filterInputEvent(const InputEvent*, uint32_t) {
        return true;
    }

    virtual void interceptKeyBeforeQueueing(const KeyEvent*, uint32_t&) {
    }

    virtual void interceptMotionBeforeQueueing(nsecs_t, uint32_t&) {
    }

    virtual nsecs_t interceptKeyBeforeDispatching(const sp<InputWindowHandle>&,
            const KeyEvent*, uint32_t) {
        return 0;
    }

    virtual bool dispatchUnhandledKey(const sp<InputWindowHandle>&,
            const KeyEvent*, uint32_t, KeyEvent*) {
        return false;
    }

    virtual void notifySwitch(nsecs_t, uint32_t, uint32_t, uint32_t) {
    }

    virtual void pokeUserActivity(nsecs_t, int32_t) {
    }

    virtual bool checkInjectEventsPermissionNonReentrant(int32_t, int32_t) {
        return false;
    }
};


// --- InputDispatcherTest ---

class InputDispatcherTest : public testing::Test {
protected:
    sp<FakeInputDispatcherPolicy> mFakePolicy;
    sp<InputDispatcher> mDispatcher;
    sp<InputDispatcherThread> mDispatcherThread;

    virtual void SetUp() {
        mFakePolicy = new FakeInputDispatcherPolicy();
        mDispatcher = new InputDispatcher(mFakePolicy);
        mDispatcher->setInputDispatchMode(/*enabled*/ true, /*frozen*/ false);
        //Start InputDispatcher thread
        mDispatcherThread = new InputDispatcherThread(mDispatcher);
        mDispatcherThread->run("InputDispatcherTest", PRIORITY_URGENT_DISPLAY);
    }

    virtual void TearDown() {
        mDispatcherThread->requestExit();
        mDispatcherThread.clear();
        mFakePolicy.clear();
        mDispatcher.clear();
    }
};


TEST_F(InputDispatcherTest, InjectInputEvent_ValidatesKeyEvents) {
    KeyEvent event;

    // Rejects undefined key actions.
    event.initialize(DEVICE_ID, AINPUT_SOURCE_KEYBOARD, ADISPLAY_ID_NONE,
            /*action*/ -1, 0,
            AKEYCODE_A, KEY_A, AMETA_NONE, 0, ARBITRARY_TIME, ARBITRARY_TIME);
    ASSERT_EQ(INPUT_EVENT_INJECTION_FAILED, mDispatcher->injectInputEvent(
            &event,
            INJECTOR_PID, INJECTOR_UID, INPUT_EVENT_INJECTION_SYNC_NONE, 0, 0))
            << "Should reject key events with undefined action.";

    // Rejects ACTION_MULTIPLE since it is not supported despite being defined in the API.
    event.initialize(DEVICE_ID, AINPUT_SOURCE_KEYBOARD, ADISPLAY_ID_NONE,
            AKEY_EVENT_ACTION_MULTIPLE, 0,
            AKEYCODE_A, KEY_A, AMETA_NONE, 0, ARBITRARY_TIME, ARBITRARY_TIME);
    ASSERT_EQ(INPUT_EVENT_INJECTION_FAILED, mDispatcher->injectInputEvent(
            &event,
            INJECTOR_PID, INJECTOR_UID, INPUT_EVENT_INJECTION_SYNC_NONE, 0, 0))
            << "Should reject key events with ACTION_MULTIPLE.";
}

TEST_F(InputDispatcherTest, InjectInputEvent_ValidatesMotionEvents) {
    MotionEvent event;
    PointerProperties pointerProperties[MAX_POINTERS + 1];
    PointerCoords pointerCoords[MAX_POINTERS + 1];
    for (int i = 0; i <= MAX_POINTERS; i++) {
        pointerProperties[i].clear();
        pointerProperties[i].id = i;
        pointerCoords[i].clear();
    }

    // Rejects undefined motion actions.
    event.initialize(DEVICE_ID, AINPUT_SOURCE_TOUCHSCREEN, DISPLAY_ID,
            /*action*/ -1, 0, 0, 0, AMETA_NONE, 0, 0, 0, 0, 0,
            ARBITRARY_TIME, ARBITRARY_TIME,
            /*pointerCount*/ 1, pointerProperties, pointerCoords);
    ASSERT_EQ(INPUT_EVENT_INJECTION_FAILED, mDispatcher->injectInputEvent(
            &event,
            INJECTOR_PID, INJECTOR_UID, INPUT_EVENT_INJECTION_SYNC_NONE, 0, 0))
            << "Should reject motion events with undefined action.";

    // Rejects pointer down with invalid index.
    event.initialize(DEVICE_ID, AINPUT_SOURCE_TOUCHSCREEN, DISPLAY_ID,
            AMOTION_EVENT_ACTION_POINTER_DOWN | (1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
            0, 0, 0, AMETA_NONE, 0, 0, 0, 0, 0,
            ARBITRARY_TIME, ARBITRARY_TIME,
            /*pointerCount*/ 1, pointerProperties, pointerCoords);
    ASSERT_EQ(INPUT_EVENT_INJECTION_FAILED, mDispatcher->injectInputEvent(
            &event,
            INJECTOR_PID, INJECTOR_UID, INPUT_EVENT_INJECTION_SYNC_NONE, 0, 0))
            << "Should reject motion events with pointer down index too large.";

    event.initialize(DEVICE_ID, AINPUT_SOURCE_TOUCHSCREEN, DISPLAY_ID,
            AMOTION_EVENT_ACTION_POINTER_DOWN | (~0U << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
            0, 0, 0, AMETA_NONE, 0, 0, 0, 0, 0,
            ARBITRARY_TIME, ARBITRARY_TIME,
            /*pointerCount*/ 1, pointerProperties, pointerCoords);
    ASSERT_EQ(INPUT_EVENT_INJECTION_FAILED, mDispatcher->injectInputEvent(
            &event,
            INJECTOR_PID, INJECTOR_UID, INPUT_EVENT_INJECTION_SYNC_NONE, 0, 0))
            << "Should reject motion events with pointer down index too small.";

    // Rejects pointer up with invalid index.
    event.initialize(DEVICE_ID, AINPUT_SOURCE_TOUCHSCREEN, DISPLAY_ID,
            AMOTION_EVENT_ACTION_POINTER_UP | (1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
            0, 0, 0, AMETA_NONE, 0, 0, 0, 0, 0,
            ARBITRARY_TIME, ARBITRARY_TIME,
            /*pointerCount*/ 1, pointerProperties, pointerCoords);
    ASSERT_EQ(INPUT_EVENT_INJECTION_FAILED, mDispatcher->injectInputEvent(
            &event,
            INJECTOR_PID, INJECTOR_UID, INPUT_EVENT_INJECTION_SYNC_NONE, 0, 0))
            << "Should reject motion events with pointer up index too large.";

    event.initialize(DEVICE_ID, AINPUT_SOURCE_TOUCHSCREEN, DISPLAY_ID,
            AMOTION_EVENT_ACTION_POINTER_UP | (~0U << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
            0, 0, 0, AMETA_NONE, 0, 0, 0, 0, 0,
            ARBITRARY_TIME, ARBITRARY_TIME,
            /*pointerCount*/ 1, pointerProperties, pointerCoords);
    ASSERT_EQ(INPUT_EVENT_INJECTION_FAILED, mDispatcher->injectInputEvent(
            &event,
            INJECTOR_PID, INJECTOR_UID, INPUT_EVENT_INJECTION_SYNC_NONE, 0, 0))
            << "Should reject motion events with pointer up index too small.";

    // Rejects motion events with invalid number of pointers.
    event.initialize(DEVICE_ID, AINPUT_SOURCE_TOUCHSCREEN, DISPLAY_ID,
            AMOTION_EVENT_ACTION_DOWN, 0, 0, 0, AMETA_NONE, 0, 0, 0, 0, 0,
            ARBITRARY_TIME, ARBITRARY_TIME,
            /*pointerCount*/ 0, pointerProperties, pointerCoords);
    ASSERT_EQ(INPUT_EVENT_INJECTION_FAILED, mDispatcher->injectInputEvent(
            &event,
            INJECTOR_PID, INJECTOR_UID, INPUT_EVENT_INJECTION_SYNC_NONE, 0, 0))
            << "Should reject motion events with 0 pointers.";

    event.initialize(DEVICE_ID, AINPUT_SOURCE_TOUCHSCREEN, DISPLAY_ID,
            AMOTION_EVENT_ACTION_DOWN, 0, 0, 0, AMETA_NONE, 0, 0, 0, 0, 0,
            ARBITRARY_TIME, ARBITRARY_TIME,
            /*pointerCount*/ MAX_POINTERS + 1, pointerProperties, pointerCoords);
    ASSERT_EQ(INPUT_EVENT_INJECTION_FAILED, mDispatcher->injectInputEvent(
            &event,
            INJECTOR_PID, INJECTOR_UID, INPUT_EVENT_INJECTION_SYNC_NONE, 0, 0))
            << "Should reject motion events with more than MAX_POINTERS pointers.";

    // Rejects motion events with invalid pointer ids.
    pointerProperties[0].id = -1;
    event.initialize(DEVICE_ID, AINPUT_SOURCE_TOUCHSCREEN, DISPLAY_ID,
            AMOTION_EVENT_ACTION_DOWN, 0, 0, 0, AMETA_NONE, 0, 0, 0, 0, 0,
            ARBITRARY_TIME, ARBITRARY_TIME,
            /*pointerCount*/ 1, pointerProperties, pointerCoords);
    ASSERT_EQ(INPUT_EVENT_INJECTION_FAILED, mDispatcher->injectInputEvent(
            &event,
            INJECTOR_PID, INJECTOR_UID, INPUT_EVENT_INJECTION_SYNC_NONE, 0, 0))
            << "Should reject motion events with pointer ids less than 0.";

    pointerProperties[0].id = MAX_POINTER_ID + 1;
    event.initialize(DEVICE_ID, AINPUT_SOURCE_TOUCHSCREEN, DISPLAY_ID,
            AMOTION_EVENT_ACTION_DOWN, 0, 0, 0, AMETA_NONE, 0, 0, 0, 0, 0,
            ARBITRARY_TIME, ARBITRARY_TIME,
            /*pointerCount*/ 1, pointerProperties, pointerCoords);
    ASSERT_EQ(INPUT_EVENT_INJECTION_FAILED, mDispatcher->injectInputEvent(
            &event,
            INJECTOR_PID, INJECTOR_UID, INPUT_EVENT_INJECTION_SYNC_NONE, 0, 0))
            << "Should reject motion events with pointer ids greater than MAX_POINTER_ID.";

    // Rejects motion events with duplicate pointer ids.
    pointerProperties[0].id = 1;
    pointerProperties[1].id = 1;
    event.initialize(DEVICE_ID, AINPUT_SOURCE_TOUCHSCREEN, DISPLAY_ID,
            AMOTION_EVENT_ACTION_DOWN, 0, 0, 0, AMETA_NONE, 0, 0, 0, 0, 0,
            ARBITRARY_TIME, ARBITRARY_TIME,
            /*pointerCount*/ 2, pointerProperties, pointerCoords);
    ASSERT_EQ(INPUT_EVENT_INJECTION_FAILED, mDispatcher->injectInputEvent(
            &event,
            INJECTOR_PID, INJECTOR_UID, INPUT_EVENT_INJECTION_SYNC_NONE, 0, 0))
            << "Should reject motion events with duplicate pointer ids.";
}

// --- InputDispatcherTest SetInputWindowTest ---
static const int32_t INJECT_EVENT_TIMEOUT = 500;
static const int32_t DISPATCHING_TIMEOUT = 100;

class FakeApplicationHandle : public InputApplicationHandle {
public:
    FakeApplicationHandle() {}
    virtual ~FakeApplicationHandle() {}

    virtual bool updateInfo() {
        if (!mInfo) {
            mInfo = new InputApplicationInfo();
        }
        mInfo->dispatchingTimeout = DISPATCHING_TIMEOUT;
        return true;
    }
};

class FakeWindowHandle : public InputWindowHandle {
public:
    static const int32_t WIDTH = 600;
    static const int32_t HEIGHT = 800;

    FakeWindowHandle(const sp<InputApplicationHandle>& inputApplicationHandle,
        const sp<InputDispatcher>& dispatcher, const std::string name) :
            InputWindowHandle(inputApplicationHandle), mDispatcher(dispatcher),
            mName(name), mFocused(false), mDisplayId(ADISPLAY_ID_DEFAULT) {
        InputChannel::openInputChannelPair(name, mServerChannel, mClientChannel);
        mConsumer = new InputConsumer(mClientChannel);
        mDispatcher->registerInputChannel(mServerChannel, this, false);
    }

    virtual ~FakeWindowHandle() {
        mDispatcher->unregisterInputChannel(mServerChannel);
        mServerChannel.clear();
        mClientChannel.clear();
        mDispatcher.clear();

        if (mConsumer != nullptr) {
            delete mConsumer;
        }
    }

    virtual bool updateInfo() {
        if (!mInfo) {
            mInfo = new InputWindowInfo();
        }
        mInfo->inputChannel = mServerChannel;
        mInfo->name = mName;
        mInfo->layoutParamsFlags = 0;
        mInfo->layoutParamsType = InputWindowInfo::TYPE_APPLICATION;
        mInfo->dispatchingTimeout = DISPATCHING_TIMEOUT;
        mInfo->frameLeft = 0;
        mInfo->frameTop = 0;
        mInfo->frameRight = WIDTH;
        mInfo->frameBottom = HEIGHT;
        mInfo->scaleFactor = 1.0;
        mInfo->addTouchableRegion(Rect(0, 0, WIDTH, HEIGHT));
        mInfo->visible = true;
        mInfo->canReceiveKeys = true;
        mInfo->hasFocus = mFocused;
        mInfo->hasWallpaper = false;
        mInfo->paused = false;
        mInfo->layer = 0;
        mInfo->ownerPid = INJECTOR_PID;
        mInfo->ownerUid = INJECTOR_UID;
        mInfo->inputFeatures = 0;
        mInfo->displayId = mDisplayId;

        return true;
    }

    void setFocus() {
        mFocused = true;
    }

    void setDisplayId(int32_t displayId) {
        mDisplayId = displayId;
    }

    void consumeEvent(int32_t expectedEventType, int32_t expectedDisplayId) {
        uint32_t consumeSeq;
        InputEvent* event;
        status_t status = mConsumer->consume(&mEventFactory, false /*consumeBatches*/, -1,
            &consumeSeq, &event);

        ASSERT_EQ(OK, status)
                << mName.c_str() << ": consumer consume should return OK.";
        ASSERT_TRUE(event != nullptr)
                << mName.c_str() << ": consumer should have returned non-NULL event.";
        ASSERT_EQ(expectedEventType, event->getType())
                << mName.c_str() << ": consumer type should same as expected one.";

        ASSERT_EQ(expectedDisplayId, event->getDisplayId())
                << mName.c_str() << ": consumer displayId should same as expected one.";

        status = mConsumer->sendFinishedSignal(consumeSeq, true /*handled*/);
        ASSERT_EQ(OK, status)
                << mName.c_str() << ": consumer sendFinishedSignal should return OK.";
    }

    void assertNoEvents() {
        uint32_t consumeSeq;
        InputEvent* event;
        status_t status = mConsumer->consume(&mEventFactory, false /*consumeBatches*/, -1,
            &consumeSeq, &event);
        ASSERT_NE(OK, status)
                << mName.c_str()
                << ": should not have received any events, so consume(..) should not return OK.";
    }

    private:
        sp<InputDispatcher> mDispatcher;
        sp<InputChannel> mServerChannel, mClientChannel;
        InputConsumer *mConsumer;
        PreallocatedInputEventFactory mEventFactory;

        std::string mName;
        bool mFocused;
        int32_t mDisplayId;
};

static int32_t injectKeyDown(const sp<InputDispatcher>& dispatcher,
        int32_t displayId = ADISPLAY_ID_NONE) {
    KeyEvent event;
    nsecs_t currentTime = systemTime(SYSTEM_TIME_MONOTONIC);

    // Define a valid key down event.
    event.initialize(DEVICE_ID, AINPUT_SOURCE_KEYBOARD, displayId,
            AKEY_EVENT_ACTION_DOWN, /* flags */ 0,
            AKEYCODE_A, KEY_A, AMETA_NONE, /* repeatCount */ 0, currentTime, currentTime);

    // Inject event until dispatch out.
    return dispatcher->injectInputEvent(
            &event,
            INJECTOR_PID, INJECTOR_UID, INPUT_EVENT_INJECTION_SYNC_WAIT_FOR_RESULT,
            INJECT_EVENT_TIMEOUT, POLICY_FLAG_FILTERED | POLICY_FLAG_PASS_TO_USER);
}

static int32_t injectMotionDown(const sp<InputDispatcher>& dispatcher, int32_t displayId) {
    MotionEvent event;
    PointerProperties pointerProperties[1];
    PointerCoords pointerCoords[1];

    pointerProperties[0].clear();
    pointerProperties[0].id = 0;
    pointerProperties[0].toolType = AMOTION_EVENT_TOOL_TYPE_FINGER;

    pointerCoords[0].clear();
    pointerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_X, 100);
    pointerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_Y, 200);

    nsecs_t currentTime = systemTime(SYSTEM_TIME_MONOTONIC);
    // Define a valid motion down event.
    event.initialize(DEVICE_ID, AINPUT_SOURCE_TOUCHSCREEN, displayId,
            AMOTION_EVENT_ACTION_DOWN, /* actionButton */0, /* flags */ 0, /* edgeFlags */ 0,
            AMETA_NONE, /* buttonState */ 0, /* xOffset */ 0, /* yOffset */ 0, /* xPrecision */ 0,
            /* yPrecision */ 0, currentTime, currentTime, /*pointerCount*/ 1, pointerProperties,
            pointerCoords);

    // Inject event until dispatch out.
    return dispatcher->injectInputEvent(
            &event,
            INJECTOR_PID, INJECTOR_UID, INPUT_EVENT_INJECTION_SYNC_WAIT_FOR_RESULT,
            INJECT_EVENT_TIMEOUT, POLICY_FLAG_FILTERED | POLICY_FLAG_PASS_TO_USER);
}

TEST_F(InputDispatcherTest, SetInputWindow_SingleWindowTouch) {
    sp<FakeApplicationHandle> application = new FakeApplicationHandle();
    sp<FakeWindowHandle> window = new FakeWindowHandle(application, mDispatcher, "Fake Window");

    Vector<sp<InputWindowHandle>> inputWindowHandles;
    inputWindowHandles.add(window);

    mDispatcher->setInputWindows(inputWindowHandles, ADISPLAY_ID_DEFAULT);
    ASSERT_EQ(INPUT_EVENT_INJECTION_SUCCEEDED, injectMotionDown(mDispatcher, ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return INPUT_EVENT_INJECTION_SUCCEEDED";

    // Window should receive motion event.
    window->consumeEvent(AINPUT_EVENT_TYPE_MOTION, ADISPLAY_ID_DEFAULT);
}

// The foreground window should receive the first touch down event.
TEST_F(InputDispatcherTest, SetInputWindow_MultiWindowsTouch) {
    sp<FakeApplicationHandle> application = new FakeApplicationHandle();
    sp<FakeWindowHandle> windowTop = new FakeWindowHandle(application, mDispatcher, "Top");
    sp<FakeWindowHandle> windowSecond = new FakeWindowHandle(application, mDispatcher, "Second");

    Vector<sp<InputWindowHandle>> inputWindowHandles;
    inputWindowHandles.add(windowTop);
    inputWindowHandles.add(windowSecond);

    mDispatcher->setInputWindows(inputWindowHandles, ADISPLAY_ID_DEFAULT);
    ASSERT_EQ(INPUT_EVENT_INJECTION_SUCCEEDED, injectMotionDown(mDispatcher, ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return INPUT_EVENT_INJECTION_SUCCEEDED";

    // Top window should receive the touch down event. Second window should not receive anything.
    windowTop->consumeEvent(AINPUT_EVENT_TYPE_MOTION, ADISPLAY_ID_DEFAULT);
    windowSecond->assertNoEvents();
}

TEST_F(InputDispatcherTest, SetInputWindow_FocusedWindow) {
    sp<FakeApplicationHandle> application = new FakeApplicationHandle();
    sp<FakeWindowHandle> windowTop = new FakeWindowHandle(application, mDispatcher, "Top");
    sp<FakeWindowHandle> windowSecond = new FakeWindowHandle(application, mDispatcher, "Second");

    // Set focus application.
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);

    // Expect one focus window exist in display.
    windowSecond->setFocus();
    Vector<sp<InputWindowHandle>> inputWindowHandles;
    inputWindowHandles.add(windowTop);
    inputWindowHandles.add(windowSecond);

    mDispatcher->setInputWindows(inputWindowHandles, ADISPLAY_ID_DEFAULT);
    ASSERT_EQ(INPUT_EVENT_INJECTION_SUCCEEDED, injectKeyDown(mDispatcher))
            << "Inject key event should return INPUT_EVENT_INJECTION_SUCCEEDED";

    // Focused window should receive event.
    windowTop->assertNoEvents();
    windowSecond->consumeEvent(AINPUT_EVENT_TYPE_KEY, ADISPLAY_ID_NONE);
}

TEST_F(InputDispatcherTest, SetInputWindow_MultiDisplayTouch) {
    sp<FakeApplicationHandle> application = new FakeApplicationHandle();
    sp<FakeWindowHandle> windowInPrimary = new FakeWindowHandle(application, mDispatcher, "D_1");
    sp<FakeWindowHandle> windowInSecondary = new FakeWindowHandle(application, mDispatcher, "D_2");

    // Test the primary display touch down.
    Vector<sp<InputWindowHandle>> inputWindowHandles;
    inputWindowHandles.push(windowInPrimary);

    mDispatcher->setInputWindows(inputWindowHandles, ADISPLAY_ID_DEFAULT);
    ASSERT_EQ(INPUT_EVENT_INJECTION_SUCCEEDED, injectMotionDown(mDispatcher, ADISPLAY_ID_DEFAULT))
            << "Inject motion event should return INPUT_EVENT_INJECTION_SUCCEEDED";
    windowInPrimary->consumeEvent(AINPUT_EVENT_TYPE_MOTION, ADISPLAY_ID_DEFAULT);
    windowInSecondary->assertNoEvents();

    // Test the second display touch down.
    constexpr int32_t SECOND_DISPLAY_ID = 1;
    windowInSecondary->setDisplayId(SECOND_DISPLAY_ID);
    Vector<sp<InputWindowHandle>> inputWindowHandles_Second;
    inputWindowHandles_Second.push(windowInSecondary);

    mDispatcher->setInputWindows(inputWindowHandles_Second, SECOND_DISPLAY_ID);
    ASSERT_EQ(INPUT_EVENT_INJECTION_SUCCEEDED, injectMotionDown(mDispatcher, SECOND_DISPLAY_ID))
            << "Inject motion event should return INPUT_EVENT_INJECTION_SUCCEEDED";
    windowInPrimary->assertNoEvents();
    windowInSecondary->consumeEvent(AINPUT_EVENT_TYPE_MOTION, SECOND_DISPLAY_ID);
}

TEST_F(InputDispatcherTest, SetInputWindow_FocusedInMultiDisplay) {
    sp<FakeApplicationHandle> application = new FakeApplicationHandle();
    sp<FakeWindowHandle> windowInPrimary = new FakeWindowHandle(application, mDispatcher, "D_1");
    sp<FakeApplicationHandle> application2 = new FakeApplicationHandle();
    sp<FakeWindowHandle> windowInSecondary = new FakeWindowHandle(application2, mDispatcher, "D_2");

    constexpr int32_t SECOND_DISPLAY_ID = 1;

    // Set focus to primary display window.
    mDispatcher->setFocusedApplication(ADISPLAY_ID_DEFAULT, application);
    windowInPrimary->setFocus();

    // Set focus to second display window.
    mDispatcher->setFocusedDisplay(SECOND_DISPLAY_ID);
    mDispatcher->setFocusedApplication(SECOND_DISPLAY_ID, application2);
    windowInSecondary->setFocus();

    // Update all windows per displays.
    Vector<sp<InputWindowHandle>> inputWindowHandles;
    inputWindowHandles.push(windowInPrimary);
    mDispatcher->setInputWindows(inputWindowHandles, ADISPLAY_ID_DEFAULT);

    windowInSecondary->setDisplayId(SECOND_DISPLAY_ID);
    Vector<sp<InputWindowHandle>> inputWindowHandles_Second;
    inputWindowHandles_Second.push(windowInSecondary);
    mDispatcher->setInputWindows(inputWindowHandles_Second, SECOND_DISPLAY_ID);

    // Test inject a key down with display id specified.
    ASSERT_EQ(INPUT_EVENT_INJECTION_SUCCEEDED, injectKeyDown(mDispatcher, ADISPLAY_ID_DEFAULT))
            << "Inject key event should return INPUT_EVENT_INJECTION_SUCCEEDED";
    windowInPrimary->consumeEvent(AINPUT_EVENT_TYPE_KEY, ADISPLAY_ID_DEFAULT);
    windowInSecondary->assertNoEvents();

    // Test inject a key down without display id specified.
    ASSERT_EQ(INPUT_EVENT_INJECTION_SUCCEEDED, injectKeyDown(mDispatcher))
            << "Inject key event should return INPUT_EVENT_INJECTION_SUCCEEDED";
    windowInPrimary->assertNoEvents();
    windowInSecondary->consumeEvent(AINPUT_EVENT_TYPE_KEY, ADISPLAY_ID_NONE);

    // Remove secondary display.
    inputWindowHandles_Second.clear();
    mDispatcher->setInputWindows(inputWindowHandles_Second, SECOND_DISPLAY_ID);

    // Expect old focus should receive a cancel event.
    windowInSecondary->consumeEvent(AINPUT_EVENT_TYPE_KEY, ADISPLAY_ID_NONE);
    // TODO(b/111361570): Validate that the event here was marked as canceled.

    // Test inject a key down, should timeout because of no target window.
    ASSERT_EQ(INPUT_EVENT_INJECTION_TIMED_OUT, injectKeyDown(mDispatcher))
            << "Inject key event should return INPUT_EVENT_INJECTION_TIMED_OUT";
    windowInPrimary->assertNoEvents();
    windowInSecondary->assertNoEvents();
}

} // namespace android
