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

#define LOG_TAG "InputManager-JNI"

#define ATRACE_TAG ATRACE_TAG_INPUT

//#define LOG_NDEBUG 0

// Log debug messages about InputReaderPolicy
#define DEBUG_INPUT_READER_POLICY 0

// Log debug messages about InputDispatcherPolicy
#define DEBUG_INPUT_DISPATCHER_POLICY 0

#include <android-base/parseint.h>
#include <android-base/stringprintf.h>
#include <android/os/IInputConstants.h>
#include <android_runtime/AndroidRuntime.h>
#include <android_runtime/Log.h>
#include <limits.h>
#include <atomic>
#include <cinttypes>

#include <utils/Log.h>
#include <utils/Looper.h>
#include <utils/threads.h>
#include <utils/Trace.h>

#include <binder/IServiceManager.h>

#include <input/PointerController.h>
#include <input/SpriteController.h>
#include <ui/Region.h>

#include <batteryservice/include/batteryservice/BatteryServiceConstants.h>
#include <inputflinger/InputManager.h>

#include <android_os_MessageQueue.h>
#include <android_view_InputChannel.h>
#include <android_view_InputDevice.h>
#include <android_view_KeyEvent.h>
#include <android_view_MotionEvent.h>
#include <android_view_PointerIcon.h>
#include <android_view_VerifiedKeyEvent.h>
#include <android_view_VerifiedMotionEvent.h>

#include <nativehelper/ScopedLocalFrame.h>
#include <nativehelper/ScopedLocalRef.h>
#include <nativehelper/ScopedPrimitiveArray.h>
#include <nativehelper/ScopedUtfChars.h>

#include "android_hardware_display_DisplayViewport.h"
#include "android_hardware_input_InputApplicationHandle.h"
#include "android_hardware_input_InputWindowHandle.h"
#include "android_util_Binder.h"
#include "com_android_server_power_PowerManagerService.h"

#include <vector>

#define INDENT "  "

using android::base::ParseUint;
using android::base::StringPrintf;
using android::os::BlockUntrustedTouchesMode;
using android::os::InputEventInjectionResult;
using android::os::InputEventInjectionSync;

// Maximum allowable delay value in a vibration pattern before
// which the delay will be truncated.
static constexpr std::chrono::duration MAX_VIBRATE_PATTERN_DELAY = 100s;
static constexpr std::chrono::milliseconds MAX_VIBRATE_PATTERN_DELAY_MILLIS =
        std::chrono::duration_cast<std::chrono::milliseconds>(MAX_VIBRATE_PATTERN_DELAY);

namespace android {

// The exponent used to calculate the pointer speed scaling factor.
// The scaling factor is calculated as 2 ^ (speed * exponent),
// where the speed ranges from -7 to + 7 and is supplied by the user.
static const float POINTER_SPEED_EXPONENT = 1.0f / 4;

static struct {
    jclass clazz;
    jmethodID notifyConfigurationChanged;
    jmethodID notifyInputDevicesChanged;
    jmethodID notifySwitch;
    jmethodID notifyInputChannelBroken;
    jmethodID notifyNoFocusedWindowAnr;
    jmethodID notifyWindowUnresponsive;
    jmethodID notifyWindowResponsive;
    jmethodID notifyMonitorUnresponsive;
    jmethodID notifyMonitorResponsive;
    jmethodID notifyFocusChanged;
    jmethodID notifySensorEvent;
    jmethodID notifySensorAccuracy;
    jmethodID notifyVibratorState;
    jmethodID notifyUntrustedTouch;
    jmethodID filterInputEvent;
    jmethodID interceptKeyBeforeQueueing;
    jmethodID interceptMotionBeforeQueueingNonInteractive;
    jmethodID interceptKeyBeforeDispatching;
    jmethodID dispatchUnhandledKey;
    jmethodID checkInjectEventsPermission;
    jmethodID onPointerDownOutsideFocus;
    jmethodID getVirtualKeyQuietTimeMillis;
    jmethodID getExcludedDeviceNames;
    jmethodID getInputPortAssociations;
    jmethodID getInputUniqueIdAssociations;
    jmethodID getKeyRepeatTimeout;
    jmethodID getKeyRepeatDelay;
    jmethodID getHoverTapTimeout;
    jmethodID getHoverTapSlop;
    jmethodID getDoubleTapTimeout;
    jmethodID getLongPressTimeout;
    jmethodID getPointerLayer;
    jmethodID getPointerIcon;
    jmethodID getPointerDisplayId;
    jmethodID getKeyboardLayoutOverlay;
    jmethodID getDeviceAlias;
    jmethodID getTouchCalibrationForInputDevice;
    jmethodID getContextForDisplay;
    jmethodID notifyDropWindow;
} gServiceClassInfo;

static struct {
    jclass clazz;
} gInputDeviceClassInfo;

static struct {
    jclass clazz;
} gKeyEventClassInfo;

static struct {
    jclass clazz;
} gMotionEventClassInfo;

static struct {
    jclass clazz;
    jmethodID constructor;
} gInputDeviceIdentifierInfo;

static struct {
    jclass clazz;
    jmethodID getAffineTransform;
} gTouchCalibrationClassInfo;

static struct {
    jclass clazz;
    jmethodID constructor;
    jfieldID lightTypeSingle;
    jfieldID lightTypePlayerId;
    jfieldID lightTypeRgb;
} gLightClassInfo;

static struct {
    jclass clazz;
    jmethodID constructor;
    jmethodID add;
} gArrayListClassInfo;

static struct {
    jclass clazz;
    jmethodID constructor;
    jmethodID keyAt;
    jmethodID valueAt;
    jmethodID size;
} gSparseArrayClassInfo;

static struct InputSensorInfoOffsets {
    jclass clazz;
    // fields
    jfieldID name;
    jfieldID vendor;
    jfieldID version;
    jfieldID handle;
    jfieldID maxRange;
    jfieldID resolution;
    jfieldID power;
    jfieldID minDelay;
    jfieldID fifoReservedEventCount;
    jfieldID fifoMaxEventCount;
    jfieldID stringType;
    jfieldID requiredPermission;
    jfieldID maxDelay;
    jfieldID flags;
    jfieldID type;
    jfieldID id;
    // methods
    jmethodID init;
} gInputSensorInfo;

// --- Global functions ---

template<typename T>
inline static T min(const T& a, const T& b) {
    return a < b ? a : b;
}

template<typename T>
inline static T max(const T& a, const T& b) {
    return a > b ? a : b;
}

static inline const char* toString(bool value) {
    return value ? "true" : "false";
}

static void loadSystemIconAsSpriteWithPointerIcon(JNIEnv* env, jobject contextObj, int32_t style,
        PointerIcon* outPointerIcon, SpriteIcon* outSpriteIcon) {
    status_t status = android_view_PointerIcon_loadSystemIcon(env,
            contextObj, style, outPointerIcon);
    if (!status) {
        outSpriteIcon->bitmap = outPointerIcon->bitmap.copy(ANDROID_BITMAP_FORMAT_RGBA_8888);
        outSpriteIcon->style = outPointerIcon->style;
        outSpriteIcon->hotSpotX = outPointerIcon->hotSpotX;
        outSpriteIcon->hotSpotY = outPointerIcon->hotSpotY;
    }
}

static void loadSystemIconAsSprite(JNIEnv* env, jobject contextObj, int32_t style,
                                   SpriteIcon* outSpriteIcon) {
    PointerIcon pointerIcon;
    loadSystemIconAsSpriteWithPointerIcon(env, contextObj, style, &pointerIcon, outSpriteIcon);
}

enum {
    WM_ACTION_PASS_TO_USER = 1,
};

static std::string getStringElementFromJavaArray(JNIEnv* env, jobjectArray array, jsize index) {
    jstring item = jstring(env->GetObjectArrayElement(array, index));
    ScopedUtfChars chars(env, item);
    std::string result(chars.c_str());
    return result;
}

// --- NativeInputManager ---

class NativeInputManager : public virtual RefBase,
    public virtual InputReaderPolicyInterface,
    public virtual InputDispatcherPolicyInterface,
    public virtual PointerControllerPolicyInterface {
protected:
    virtual ~NativeInputManager();

public:
    NativeInputManager(jobject contextObj, jobject serviceObj, const sp<Looper>& looper);

    inline sp<InputManagerInterface> getInputManager() const { return mInputManager; }

    void dump(std::string& dump);

    void setDisplayViewports(JNIEnv* env, jobjectArray viewportObjArray);

    base::Result<std::unique_ptr<InputChannel>> createInputChannel(JNIEnv* env,
                                                                   const std::string& name);
    base::Result<std::unique_ptr<InputChannel>> createInputMonitor(JNIEnv* env, int32_t displayId,
                                                                   bool isGestureMonitor,
                                                                   const std::string& name,
                                                                   int32_t pid);
    status_t removeInputChannel(JNIEnv* env, const sp<IBinder>& connectionToken);
    status_t pilferPointers(const sp<IBinder>& token);

    void displayRemoved(JNIEnv* env, int32_t displayId);
    void setFocusedApplication(JNIEnv* env, int32_t displayId, jobject applicationHandleObj);
    void setFocusedDisplay(JNIEnv* env, int32_t displayId);
    void setInputDispatchMode(bool enabled, bool frozen);
    void setSystemUiLightsOut(bool lightsOut);
    void setPointerSpeed(int32_t speed);
    void setInputDeviceEnabled(uint32_t deviceId, bool enabled);
    void setShowTouches(bool enabled);
    void setInteractive(bool interactive);
    void reloadCalibration();
    void setPointerIconType(int32_t iconId);
    void reloadPointerIcons();
    void requestPointerCapture(const sp<IBinder>& windowToken, bool enabled);
    void setCustomPointerIcon(const SpriteIcon& icon);
    void setMotionClassifierEnabled(bool enabled);

    /* --- InputReaderPolicyInterface implementation --- */

    void getReaderConfiguration(InputReaderConfiguration* outConfig) override;
    std::shared_ptr<PointerControllerInterface> obtainPointerController(int32_t deviceId) override;
    void notifyInputDevicesChanged(const std::vector<InputDeviceInfo>& inputDevices) override;
    std::shared_ptr<KeyCharacterMap> getKeyboardLayoutOverlay(
            const InputDeviceIdentifier& identifier) override;
    std::string getDeviceAlias(const InputDeviceIdentifier& identifier) override;
    TouchAffineTransformation getTouchAffineTransformation(const std::string& inputDeviceDescriptor,
                                                           int32_t surfaceRotation) override;

    TouchAffineTransformation getTouchAffineTransformation(JNIEnv* env, jfloatArray matrixArr);

    /* --- InputDispatcherPolicyInterface implementation --- */

    void notifySwitch(nsecs_t when, uint32_t switchValues, uint32_t switchMask,
                      uint32_t policyFlags) override;
    void notifyConfigurationChanged(nsecs_t when) override;
    // ANR-related callbacks -- start
    void notifyNoFocusedWindowAnr(const std::shared_ptr<InputApplicationHandle>& handle) override;
    void notifyWindowUnresponsive(const sp<IBinder>& token, const std::string& reason) override;
    void notifyWindowResponsive(const sp<IBinder>& token) override;
    void notifyMonitorUnresponsive(int32_t pid, const std::string& reason) override;
    void notifyMonitorResponsive(int32_t pid) override;
    // ANR-related callbacks -- end
    void notifyInputChannelBroken(const sp<IBinder>& token) override;
    void notifyFocusChanged(const sp<IBinder>& oldToken, const sp<IBinder>& newToken) override;
    void notifySensorEvent(int32_t deviceId, InputDeviceSensorType sensorType,
                           InputDeviceSensorAccuracy accuracy, nsecs_t timestamp,
                           const std::vector<float>& values) override;
    void notifySensorAccuracy(int32_t deviceId, InputDeviceSensorType sensorType,
                              InputDeviceSensorAccuracy accuracy) override;
    void notifyVibratorState(int32_t deviceId, bool isOn) override;
    void notifyUntrustedTouch(const std::string& obscuringPackage) override;
    bool filterInputEvent(const InputEvent* inputEvent, uint32_t policyFlags) override;
    void getDispatcherConfiguration(InputDispatcherConfiguration* outConfig) override;
    void interceptKeyBeforeQueueing(const KeyEvent* keyEvent, uint32_t& policyFlags) override;
    void interceptMotionBeforeQueueing(const int32_t displayId, nsecs_t when,
                                       uint32_t& policyFlags) override;
    nsecs_t interceptKeyBeforeDispatching(const sp<IBinder>& token, const KeyEvent* keyEvent,
                                          uint32_t policyFlags) override;
    bool dispatchUnhandledKey(const sp<IBinder>& token, const KeyEvent* keyEvent,
                              uint32_t policyFlags, KeyEvent* outFallbackKeyEvent) override;
    void pokeUserActivity(nsecs_t eventTime, int32_t eventType, int32_t displayId) override;
    bool checkInjectEventsPermissionNonReentrant(int32_t injectorPid, int32_t injectorUid) override;
    void onPointerDownOutsideFocus(const sp<IBinder>& touchedToken) override;
    void setPointerCapture(bool enabled) override;
    void notifyDropWindow(const sp<IBinder>& token, float x, float y) override;

    /* --- PointerControllerPolicyInterface implementation --- */

    virtual void loadPointerIcon(SpriteIcon* icon, int32_t displayId);
    virtual void loadPointerResources(PointerResources* outResources, int32_t displayId);
    virtual void loadAdditionalMouseResources(std::map<int32_t, SpriteIcon>* outResources,
            std::map<int32_t, PointerAnimation>* outAnimationResources, int32_t displayId);
    virtual int32_t getDefaultPointerIconId();
    virtual int32_t getCustomPointerIconId();

private:
    sp<InputManagerInterface> mInputManager;

    jobject mServiceObj;
    sp<Looper> mLooper;

    Mutex mLock;
    struct Locked {
        // Display size information.
        std::vector<DisplayViewport> viewports;

        // True if System UI is less noticeable.
        bool systemUiLightsOut;

        // Pointer speed.
        int32_t pointerSpeed;

        // True if pointer gestures are enabled.
        bool pointerGesturesEnabled;

        // Show touches feature enable/disable.
        bool showTouches;

        // Pointer capture feature enable/disable.
        bool pointerCapture;

        // Sprite controller singleton, created on first use.
        sp<SpriteController> spriteController;

        // Pointer controller singleton, created and destroyed as needed.
        std::weak_ptr<PointerController> pointerController;

        // Input devices to be disabled
        std::set<int32_t> disabledInputDevices;

        // Associated Pointer controller display.
        int32_t pointerDisplayId;
    } mLocked GUARDED_BY(mLock);

    std::atomic<bool> mInteractive;

    void updateInactivityTimeoutLocked();
    void handleInterceptActions(jint wmActions, nsecs_t when, uint32_t& policyFlags);
    void ensureSpriteControllerLocked();
    int32_t getPointerDisplayId();
    void updatePointerDisplayLocked();
    static bool checkAndClearExceptionFromCallback(JNIEnv* env, const char* methodName);

    static inline JNIEnv* jniEnv() {
        return AndroidRuntime::getJNIEnv();
    }
};



NativeInputManager::NativeInputManager(jobject contextObj,
        jobject serviceObj, const sp<Looper>& looper) :
        mLooper(looper), mInteractive(true) {
    JNIEnv* env = jniEnv();

    mServiceObj = env->NewGlobalRef(serviceObj);

    {
        AutoMutex _l(mLock);
        mLocked.systemUiLightsOut = false;
        mLocked.pointerSpeed = 0;
        mLocked.pointerGesturesEnabled = true;
        mLocked.showTouches = false;
        mLocked.pointerCapture = false;
        mLocked.pointerDisplayId = ADISPLAY_ID_DEFAULT;
    }
    mInteractive = true;

    InputManager* im = new InputManager(this, this);
    mInputManager = im;
    defaultServiceManager()->addService(String16("inputflinger"), im);
}

NativeInputManager::~NativeInputManager() {
    JNIEnv* env = jniEnv();

    env->DeleteGlobalRef(mServiceObj);
}

void NativeInputManager::dump(std::string& dump) {
    dump += "Input Manager State:\n";
    {
        dump += StringPrintf(INDENT "Interactive: %s\n", toString(mInteractive.load()));
    }
    {
        AutoMutex _l(mLock);
        dump += StringPrintf(INDENT "System UI Lights Out: %s\n",
                             toString(mLocked.systemUiLightsOut));
        dump += StringPrintf(INDENT "Pointer Speed: %" PRId32 "\n", mLocked.pointerSpeed);
        dump += StringPrintf(INDENT "Pointer Gestures Enabled: %s\n",
                toString(mLocked.pointerGesturesEnabled));
        dump += StringPrintf(INDENT "Show Touches: %s\n", toString(mLocked.showTouches));
        dump += StringPrintf(INDENT "Pointer Capture Enabled: %s\n", toString(mLocked.pointerCapture));
    }
    dump += "\n";

    mInputManager->getReader()->dump(dump);
    dump += "\n";

    mInputManager->getClassifier()->dump(dump);
    dump += "\n";

    mInputManager->getDispatcher()->dump(dump);
    dump += "\n";
}

bool NativeInputManager::checkAndClearExceptionFromCallback(JNIEnv* env, const char* methodName) {
    if (env->ExceptionCheck()) {
        ALOGE("An exception was thrown by callback '%s'.", methodName);
        LOGE_EX(env);
        env->ExceptionClear();
        return true;
    }
    return false;
}

void NativeInputManager::setDisplayViewports(JNIEnv* env, jobjectArray viewportObjArray) {
    std::vector<DisplayViewport> viewports;

    if (viewportObjArray) {
        jsize length = env->GetArrayLength(viewportObjArray);
        for (jsize i = 0; i < length; i++) {
            jobject viewportObj = env->GetObjectArrayElement(viewportObjArray, i);
            if (! viewportObj) {
                break; // found null element indicating end of used portion of the array
            }

            DisplayViewport viewport;
            android_hardware_display_DisplayViewport_toNative(env, viewportObj, &viewport);
            ALOGI("Viewport [%d] to add: %s, isActive: %s", (int)i, viewport.uniqueId.c_str(),
                  toString(viewport.isActive));
            viewports.push_back(viewport);

            env->DeleteLocalRef(viewportObj);
        }
    }

    // Get the preferred pointer controller displayId.
    int32_t pointerDisplayId = getPointerDisplayId();

    { // acquire lock
        AutoMutex _l(mLock);
        mLocked.viewports = viewports;
        mLocked.pointerDisplayId = pointerDisplayId;
        std::shared_ptr<PointerController> controller = mLocked.pointerController.lock();
        if (controller != nullptr) {
            controller->onDisplayViewportsUpdated(mLocked.viewports);
        }
    } // release lock

    mInputManager->getReader()->requestRefreshConfiguration(
            InputReaderConfiguration::CHANGE_DISPLAY_INFO);
}

base::Result<std::unique_ptr<InputChannel>> NativeInputManager::createInputChannel(
        JNIEnv* /* env */, const std::string& name) {
    ATRACE_CALL();
    return mInputManager->getDispatcher()->createInputChannel(name);
}

base::Result<std::unique_ptr<InputChannel>> NativeInputManager::createInputMonitor(
        JNIEnv* /* env */, int32_t displayId, bool isGestureMonitor, const std::string& name,
        int32_t pid) {
    ATRACE_CALL();
    return mInputManager->getDispatcher()->createInputMonitor(displayId, isGestureMonitor, name,
                                                              pid);
}

status_t NativeInputManager::removeInputChannel(JNIEnv* /* env */,
                                                const sp<IBinder>& connectionToken) {
    ATRACE_CALL();
    return mInputManager->getDispatcher()->removeInputChannel(connectionToken);
}

status_t NativeInputManager::pilferPointers(const sp<IBinder>& token) {
    ATRACE_CALL();
    return mInputManager->getDispatcher()->pilferPointers(token);
}

void NativeInputManager::getReaderConfiguration(InputReaderConfiguration* outConfig) {
    ATRACE_CALL();
    JNIEnv* env = jniEnv();

    jint virtualKeyQuietTime = env->CallIntMethod(mServiceObj,
            gServiceClassInfo.getVirtualKeyQuietTimeMillis);
    if (!checkAndClearExceptionFromCallback(env, "getVirtualKeyQuietTimeMillis")) {
        outConfig->virtualKeyQuietTime = milliseconds_to_nanoseconds(virtualKeyQuietTime);
    }

    outConfig->excludedDeviceNames.clear();
    jobjectArray excludedDeviceNames = jobjectArray(env->CallStaticObjectMethod(
            gServiceClassInfo.clazz, gServiceClassInfo.getExcludedDeviceNames));
    if (!checkAndClearExceptionFromCallback(env, "getExcludedDeviceNames") && excludedDeviceNames) {
        jsize length = env->GetArrayLength(excludedDeviceNames);
        for (jsize i = 0; i < length; i++) {
            std::string deviceName = getStringElementFromJavaArray(env, excludedDeviceNames, i);
            outConfig->excludedDeviceNames.push_back(deviceName);
        }
        env->DeleteLocalRef(excludedDeviceNames);
    }

    // Associations between input ports and display ports
    // The java method packs the information in the following manner:
    // Original data: [{'inputPort1': '1'}, {'inputPort2': '2'}]
    // Received data: ['inputPort1', '1', 'inputPort2', '2']
    // So we unpack accordingly here.
    outConfig->portAssociations.clear();
    jobjectArray portAssociations = jobjectArray(env->CallObjectMethod(mServiceObj,
            gServiceClassInfo.getInputPortAssociations));
    if (!checkAndClearExceptionFromCallback(env, "getInputPortAssociations") && portAssociations) {
        jsize length = env->GetArrayLength(portAssociations);
        for (jsize i = 0; i < length / 2; i++) {
            std::string inputPort = getStringElementFromJavaArray(env, portAssociations, 2 * i);
            std::string displayPortStr =
                    getStringElementFromJavaArray(env, portAssociations, 2 * i + 1);
            uint8_t displayPort;
            // Should already have been validated earlier, but do it here for safety.
            bool success = ParseUint(displayPortStr, &displayPort);
            if (!success) {
                ALOGE("Could not parse entry in port configuration file, received: %s",
                    displayPortStr.c_str());
                continue;
            }
            outConfig->portAssociations.insert({inputPort, displayPort});
        }
        env->DeleteLocalRef(portAssociations);
    }
    outConfig->uniqueIdAssociations.clear();
    jobjectArray uniqueIdAssociations = jobjectArray(
            env->CallObjectMethod(mServiceObj, gServiceClassInfo.getInputUniqueIdAssociations));
    if (!checkAndClearExceptionFromCallback(env, "getInputUniqueIdAssociations") &&
        uniqueIdAssociations) {
        jsize length = env->GetArrayLength(uniqueIdAssociations);
        for (jsize i = 0; i < length / 2; i++) {
            std::string inputDeviceUniqueId =
                    getStringElementFromJavaArray(env, uniqueIdAssociations, 2 * i);
            std::string displayUniqueId =
                    getStringElementFromJavaArray(env, uniqueIdAssociations, 2 * i + 1);
            outConfig->uniqueIdAssociations.insert({inputDeviceUniqueId, displayUniqueId});
        }
        env->DeleteLocalRef(uniqueIdAssociations);
    }

    jint hoverTapTimeout = env->CallIntMethod(mServiceObj,
            gServiceClassInfo.getHoverTapTimeout);
    if (!checkAndClearExceptionFromCallback(env, "getHoverTapTimeout")) {
        jint doubleTapTimeout = env->CallIntMethod(mServiceObj,
                gServiceClassInfo.getDoubleTapTimeout);
        if (!checkAndClearExceptionFromCallback(env, "getDoubleTapTimeout")) {
            jint longPressTimeout = env->CallIntMethod(mServiceObj,
                    gServiceClassInfo.getLongPressTimeout);
            if (!checkAndClearExceptionFromCallback(env, "getLongPressTimeout")) {
                outConfig->pointerGestureTapInterval = milliseconds_to_nanoseconds(hoverTapTimeout);

                // We must ensure that the tap-drag interval is significantly shorter than
                // the long-press timeout because the tap is held down for the entire duration
                // of the double-tap timeout.
                jint tapDragInterval = max(min(longPressTimeout - 100,
                        doubleTapTimeout), hoverTapTimeout);
                outConfig->pointerGestureTapDragInterval =
                        milliseconds_to_nanoseconds(tapDragInterval);
            }
        }
    }

    jint hoverTapSlop = env->CallIntMethod(mServiceObj,
            gServiceClassInfo.getHoverTapSlop);
    if (!checkAndClearExceptionFromCallback(env, "getHoverTapSlop")) {
        outConfig->pointerGestureTapSlop = hoverTapSlop;
    }

    { // acquire lock
        AutoMutex _l(mLock);

        outConfig->pointerVelocityControlParameters.scale = exp2f(mLocked.pointerSpeed
                * POINTER_SPEED_EXPONENT);
        outConfig->pointerGesturesEnabled = mLocked.pointerGesturesEnabled;

        outConfig->showTouches = mLocked.showTouches;

        outConfig->pointerCapture = mLocked.pointerCapture;

        outConfig->setDisplayViewports(mLocked.viewports);

        outConfig->defaultPointerDisplayId = mLocked.pointerDisplayId;

        outConfig->disabledDevices = mLocked.disabledInputDevices;
    } // release lock
}

std::shared_ptr<PointerControllerInterface> NativeInputManager::obtainPointerController(
        int32_t /* deviceId */) {
    ATRACE_CALL();
    AutoMutex _l(mLock);

    std::shared_ptr<PointerController> controller = mLocked.pointerController.lock();
    if (controller == nullptr) {
        ensureSpriteControllerLocked();

        controller = PointerController::create(this, mLooper, mLocked.spriteController);
        mLocked.pointerController = controller;
        updateInactivityTimeoutLocked();
    }

    return controller;
}

int32_t NativeInputManager::getPointerDisplayId() {
    JNIEnv* env = jniEnv();
    jint pointerDisplayId = env->CallIntMethod(mServiceObj,
            gServiceClassInfo.getPointerDisplayId);
    if (checkAndClearExceptionFromCallback(env, "getPointerDisplayId")) {
        pointerDisplayId = ADISPLAY_ID_DEFAULT;
    }

    return pointerDisplayId;
}

void NativeInputManager::ensureSpriteControllerLocked() REQUIRES(mLock) {
    if (mLocked.spriteController == nullptr) {
        JNIEnv* env = jniEnv();
        jint layer = env->CallIntMethod(mServiceObj, gServiceClassInfo.getPointerLayer);
        if (checkAndClearExceptionFromCallback(env, "getPointerLayer")) {
            layer = -1;
        }
        mLocked.spriteController = new SpriteController(mLooper, layer);
    }
}

void NativeInputManager::notifyInputDevicesChanged(const std::vector<InputDeviceInfo>& inputDevices) {
    ATRACE_CALL();
    JNIEnv* env = jniEnv();

    size_t count = inputDevices.size();
    jobjectArray inputDevicesObjArray = env->NewObjectArray(
            count, gInputDeviceClassInfo.clazz, nullptr);
    if (inputDevicesObjArray) {
        bool error = false;
        for (size_t i = 0; i < count; i++) {
            jobject inputDeviceObj = android_view_InputDevice_create(env, inputDevices[i]);
            if (!inputDeviceObj) {
                error = true;
                break;
            }

            env->SetObjectArrayElement(inputDevicesObjArray, i, inputDeviceObj);
            env->DeleteLocalRef(inputDeviceObj);
        }

        if (!error) {
            env->CallVoidMethod(mServiceObj, gServiceClassInfo.notifyInputDevicesChanged,
                    inputDevicesObjArray);
        }

        env->DeleteLocalRef(inputDevicesObjArray);
    }

    checkAndClearExceptionFromCallback(env, "notifyInputDevicesChanged");
}

std::shared_ptr<KeyCharacterMap> NativeInputManager::getKeyboardLayoutOverlay(
        const InputDeviceIdentifier& identifier) {
    ATRACE_CALL();
    JNIEnv* env = jniEnv();

    std::shared_ptr<KeyCharacterMap> result;
    ScopedLocalRef<jstring> descriptor(env, env->NewStringUTF(identifier.descriptor.c_str()));
    ScopedLocalRef<jobject> identifierObj(env, env->NewObject(gInputDeviceIdentifierInfo.clazz,
            gInputDeviceIdentifierInfo.constructor, descriptor.get(),
            identifier.vendor, identifier.product));
    ScopedLocalRef<jobjectArray> arrayObj(env, jobjectArray(env->CallObjectMethod(mServiceObj,
                gServiceClassInfo.getKeyboardLayoutOverlay, identifierObj.get())));
    if (arrayObj.get()) {
        ScopedLocalRef<jstring> filenameObj(env,
                jstring(env->GetObjectArrayElement(arrayObj.get(), 0)));
        ScopedLocalRef<jstring> contentsObj(env,
                jstring(env->GetObjectArrayElement(arrayObj.get(), 1)));
        ScopedUtfChars filenameChars(env, filenameObj.get());
        ScopedUtfChars contentsChars(env, contentsObj.get());

        base::Result<std::shared_ptr<KeyCharacterMap>> ret =
                KeyCharacterMap::loadContents(filenameChars.c_str(), contentsChars.c_str(),
                                              KeyCharacterMap::Format::OVERLAY);
        if (ret.ok()) {
            result = *ret;
        }
    }
    checkAndClearExceptionFromCallback(env, "getKeyboardLayoutOverlay");
    return result;
}

std::string NativeInputManager::getDeviceAlias(const InputDeviceIdentifier& identifier) {
    ATRACE_CALL();
    JNIEnv* env = jniEnv();

    ScopedLocalRef<jstring> uniqueIdObj(env, env->NewStringUTF(identifier.uniqueId.c_str()));
    ScopedLocalRef<jstring> aliasObj(env, jstring(env->CallObjectMethod(mServiceObj,
            gServiceClassInfo.getDeviceAlias, uniqueIdObj.get())));
    std::string result;
    if (aliasObj.get()) {
        ScopedUtfChars aliasChars(env, aliasObj.get());
        result = aliasChars.c_str();
    }
    checkAndClearExceptionFromCallback(env, "getDeviceAlias");
    return result;
}

void NativeInputManager::notifySwitch(nsecs_t when,
        uint32_t switchValues, uint32_t switchMask, uint32_t /* policyFlags */) {
#if DEBUG_INPUT_DISPATCHER_POLICY
    ALOGD("notifySwitch - when=%lld, switchValues=0x%08x, switchMask=0x%08x, policyFlags=0x%x",
            when, switchValues, switchMask, policyFlags);
#endif
    ATRACE_CALL();

    JNIEnv* env = jniEnv();

    env->CallVoidMethod(mServiceObj, gServiceClassInfo.notifySwitch,
            when, switchValues, switchMask);
    checkAndClearExceptionFromCallback(env, "notifySwitch");
}

void NativeInputManager::notifyConfigurationChanged(nsecs_t when) {
#if DEBUG_INPUT_DISPATCHER_POLICY
    ALOGD("notifyConfigurationChanged - when=%lld", when);
#endif
    ATRACE_CALL();

    JNIEnv* env = jniEnv();

    env->CallVoidMethod(mServiceObj, gServiceClassInfo.notifyConfigurationChanged, when);
    checkAndClearExceptionFromCallback(env, "notifyConfigurationChanged");
}

static jobject getInputApplicationHandleObjLocalRef(
        JNIEnv* env, const std::shared_ptr<InputApplicationHandle>& inputApplicationHandle) {
    if (inputApplicationHandle == nullptr) {
        return nullptr;
    }
    NativeInputApplicationHandle* handle =
            static_cast<NativeInputApplicationHandle*>(inputApplicationHandle.get());

    return handle->getInputApplicationHandleObjLocalRef(env);
}

void NativeInputManager::notifyNoFocusedWindowAnr(
        const std::shared_ptr<InputApplicationHandle>& inputApplicationHandle) {
#if DEBUG_INPUT_DISPATCHER_POLICY
    ALOGD("notifyNoFocusedWindowAnr");
#endif
    ATRACE_CALL();

    JNIEnv* env = jniEnv();
    ScopedLocalFrame localFrame(env);

    jobject inputApplicationHandleObj =
            getInputApplicationHandleObjLocalRef(env, inputApplicationHandle);

    env->CallVoidMethod(mServiceObj, gServiceClassInfo.notifyNoFocusedWindowAnr,
                        inputApplicationHandleObj);
    checkAndClearExceptionFromCallback(env, "notifyNoFocusedWindowAnr");
}

void NativeInputManager::notifyWindowUnresponsive(const sp<IBinder>& token,
                                                  const std::string& reason) {
#if DEBUG_INPUT_DISPATCHER_POLICY
    ALOGD("notifyWindowUnresponsive");
#endif
    ATRACE_CALL();

    JNIEnv* env = jniEnv();
    ScopedLocalFrame localFrame(env);

    jobject tokenObj = javaObjectForIBinder(env, token);
    ScopedLocalRef<jstring> reasonObj(env, env->NewStringUTF(reason.c_str()));

    env->CallVoidMethod(mServiceObj, gServiceClassInfo.notifyWindowUnresponsive, tokenObj,
                        reasonObj.get());
    checkAndClearExceptionFromCallback(env, "notifyWindowUnresponsive");
}

void NativeInputManager::notifyWindowResponsive(const sp<IBinder>& token) {
#if DEBUG_INPUT_DISPATCHER_POLICY
    ALOGD("notifyWindowResponsive");
#endif
    ATRACE_CALL();

    JNIEnv* env = jniEnv();
    ScopedLocalFrame localFrame(env);

    jobject tokenObj = javaObjectForIBinder(env, token);

    env->CallVoidMethod(mServiceObj, gServiceClassInfo.notifyWindowResponsive, tokenObj);
    checkAndClearExceptionFromCallback(env, "notifyWindowResponsive");
}

void NativeInputManager::notifyMonitorUnresponsive(int32_t pid, const std::string& reason) {
#if DEBUG_INPUT_DISPATCHER_POLICY
    ALOGD("notifyMonitorUnresponsive");
#endif
    ATRACE_CALL();

    JNIEnv* env = jniEnv();
    ScopedLocalFrame localFrame(env);

    ScopedLocalRef<jstring> reasonObj(env, env->NewStringUTF(reason.c_str()));

    env->CallVoidMethod(mServiceObj, gServiceClassInfo.notifyMonitorUnresponsive, pid,
                        reasonObj.get());
    checkAndClearExceptionFromCallback(env, "notifyMonitorUnresponsive");
}

void NativeInputManager::notifyMonitorResponsive(int32_t pid) {
#if DEBUG_INPUT_DISPATCHER_POLICY
    ALOGD("notifyMonitorResponsive");
#endif
    ATRACE_CALL();

    JNIEnv* env = jniEnv();
    ScopedLocalFrame localFrame(env);

    env->CallVoidMethod(mServiceObj, gServiceClassInfo.notifyMonitorResponsive, pid);
    checkAndClearExceptionFromCallback(env, "notifyMonitorResponsive");
}

void NativeInputManager::notifyInputChannelBroken(const sp<IBinder>& token) {
#if DEBUG_INPUT_DISPATCHER_POLICY
    ALOGD("notifyInputChannelBroken");
#endif
    ATRACE_CALL();

    JNIEnv* env = jniEnv();
    ScopedLocalFrame localFrame(env);

    jobject tokenObj = javaObjectForIBinder(env, token);
    if (tokenObj) {
        env->CallVoidMethod(mServiceObj, gServiceClassInfo.notifyInputChannelBroken,
                tokenObj);
        checkAndClearExceptionFromCallback(env, "notifyInputChannelBroken");
    }
}

void NativeInputManager::notifyUntrustedTouch(const std::string& obscuringPackage) {
#if DEBUG_INPUT_DISPATCHER_POLICY
    ALOGD("notifyUntrustedTouch - obscuringPackage=%s", obscuringPackage.c_str());
#endif
    ATRACE_CALL();
    JNIEnv* env = jniEnv();
    jstring jPackage = env->NewStringUTF(obscuringPackage.c_str());
    env->CallVoidMethod(mServiceObj, gServiceClassInfo.notifyUntrustedTouch, jPackage);
    checkAndClearExceptionFromCallback(env, "notifyUntrustedTouch");
}

void NativeInputManager::notifyFocusChanged(const sp<IBinder>& oldToken,
        const sp<IBinder>& newToken) {
#if DEBUG_INPUT_DISPATCHER_POLICY
    ALOGD("notifyFocusChanged");
#endif
    ATRACE_CALL();

    JNIEnv* env = jniEnv();
    ScopedLocalFrame localFrame(env);

    jobject oldTokenObj = javaObjectForIBinder(env, oldToken);
    jobject newTokenObj = javaObjectForIBinder(env, newToken);
    env->CallVoidMethod(mServiceObj, gServiceClassInfo.notifyFocusChanged,
            oldTokenObj, newTokenObj);
    checkAndClearExceptionFromCallback(env, "notifyFocusChanged");
}

void NativeInputManager::notifyDropWindow(const sp<IBinder>& token, float x, float y) {
#if DEBUG_INPUT_DISPATCHER_POLICY
    ALOGD("notifyDropWindow");
#endif
    ATRACE_CALL();

    JNIEnv* env = jniEnv();
    ScopedLocalFrame localFrame(env);

    jobject tokenObj = javaObjectForIBinder(env, token);
    env->CallVoidMethod(mServiceObj, gServiceClassInfo.notifyDropWindow, tokenObj, x, y);
    checkAndClearExceptionFromCallback(env, "notifyDropWindow");
}

void NativeInputManager::notifySensorEvent(int32_t deviceId, InputDeviceSensorType sensorType,
                                           InputDeviceSensorAccuracy accuracy, nsecs_t timestamp,
                                           const std::vector<float>& values) {
#if DEBUG_INPUT_DISPATCHER_POLICY
    ALOGD("notifySensorEvent");
#endif
    ATRACE_CALL();
    JNIEnv* env = jniEnv();
    ScopedLocalFrame localFrame(env);
    jfloatArray arr = env->NewFloatArray(values.size());
    env->SetFloatArrayRegion(arr, 0, values.size(), values.data());
    env->CallVoidMethod(mServiceObj, gServiceClassInfo.notifySensorEvent, deviceId,
                        static_cast<jint>(sensorType), accuracy, timestamp, arr);
    checkAndClearExceptionFromCallback(env, "notifySensorEvent");
}

void NativeInputManager::notifySensorAccuracy(int32_t deviceId, InputDeviceSensorType sensorType,
                                              InputDeviceSensorAccuracy accuracy) {
#if DEBUG_INPUT_DISPATCHER_POLICY
    ALOGD("notifySensorAccuracy");
#endif
    ATRACE_CALL();
    JNIEnv* env = jniEnv();
    ScopedLocalFrame localFrame(env);
    env->CallVoidMethod(mServiceObj, gServiceClassInfo.notifySensorAccuracy, deviceId,
                        static_cast<jint>(sensorType), accuracy);
    checkAndClearExceptionFromCallback(env, "notifySensorAccuracy");
}

void NativeInputManager::notifyVibratorState(int32_t deviceId, bool isOn) {
#if DEBUG_INPUT_DISPATCHER_POLICY
    ALOGD("notifyVibratorState isOn:%d", isOn);
#endif
    ATRACE_CALL();
    JNIEnv* env = jniEnv();
    ScopedLocalFrame localFrame(env);
    env->CallVoidMethod(mServiceObj, gServiceClassInfo.notifyVibratorState,
                        static_cast<jint>(deviceId), static_cast<jboolean>(isOn));
    checkAndClearExceptionFromCallback(env, "notifyVibratorState");
}

void NativeInputManager::getDispatcherConfiguration(InputDispatcherConfiguration* outConfig) {
    ATRACE_CALL();
    JNIEnv* env = jniEnv();

    jint keyRepeatTimeout = env->CallIntMethod(mServiceObj,
            gServiceClassInfo.getKeyRepeatTimeout);
    if (!checkAndClearExceptionFromCallback(env, "getKeyRepeatTimeout")) {
        outConfig->keyRepeatTimeout = milliseconds_to_nanoseconds(keyRepeatTimeout);
    }

    jint keyRepeatDelay = env->CallIntMethod(mServiceObj,
            gServiceClassInfo.getKeyRepeatDelay);
    if (!checkAndClearExceptionFromCallback(env, "getKeyRepeatDelay")) {
        outConfig->keyRepeatDelay = milliseconds_to_nanoseconds(keyRepeatDelay);
    }
}

void NativeInputManager::displayRemoved(JNIEnv* env, int32_t displayId) {
    // Set an empty list to remove all handles from the specific display.
    std::vector<sp<InputWindowHandle>> windowHandles;
    mInputManager->getDispatcher()->setInputWindows({{displayId, windowHandles}});
    mInputManager->getDispatcher()->setFocusedApplication(displayId, nullptr);
}

void NativeInputManager::setFocusedApplication(JNIEnv* env, int32_t displayId,
        jobject applicationHandleObj) {
    if (!applicationHandleObj) {
        return;
    }
    std::shared_ptr<InputApplicationHandle> applicationHandle =
            android_view_InputApplicationHandle_getHandle(env, applicationHandleObj);
    applicationHandle->updateInfo();
    mInputManager->getDispatcher()->setFocusedApplication(displayId, applicationHandle);
}

void NativeInputManager::setFocusedDisplay(JNIEnv* env, int32_t displayId) {
    mInputManager->getDispatcher()->setFocusedDisplay(displayId);
}

void NativeInputManager::setInputDispatchMode(bool enabled, bool frozen) {
    mInputManager->getDispatcher()->setInputDispatchMode(enabled, frozen);
}

void NativeInputManager::setSystemUiLightsOut(bool lightsOut) {
    AutoMutex _l(mLock);

    if (mLocked.systemUiLightsOut != lightsOut) {
        mLocked.systemUiLightsOut = lightsOut;
        updateInactivityTimeoutLocked();
    }
}

void NativeInputManager::updateInactivityTimeoutLocked() REQUIRES(mLock) {
    std::shared_ptr<PointerController> controller = mLocked.pointerController.lock();
    if (controller == nullptr) {
        return;
    }

    controller->setInactivityTimeout(mLocked.systemUiLightsOut ? InactivityTimeout::SHORT
                                                               : InactivityTimeout::NORMAL);
}

void NativeInputManager::setPointerSpeed(int32_t speed) {
    { // acquire lock
        AutoMutex _l(mLock);

        if (mLocked.pointerSpeed == speed) {
            return;
        }

        ALOGI("Setting pointer speed to %d.", speed);
        mLocked.pointerSpeed = speed;
    } // release lock

    mInputManager->getReader()->requestRefreshConfiguration(
            InputReaderConfiguration::CHANGE_POINTER_SPEED);
}

void NativeInputManager::setInputDeviceEnabled(uint32_t deviceId, bool enabled) {
    { // acquire lock
        AutoMutex _l(mLock);

        auto it = mLocked.disabledInputDevices.find(deviceId);
        bool currentlyEnabled = it == mLocked.disabledInputDevices.end();
        if (!enabled && currentlyEnabled) {
            mLocked.disabledInputDevices.insert(deviceId);
        }
        if (enabled && !currentlyEnabled) {
            mLocked.disabledInputDevices.erase(deviceId);
        }
    } // release lock

    mInputManager->getReader()->requestRefreshConfiguration(
            InputReaderConfiguration::CHANGE_ENABLED_STATE);
}

void NativeInputManager::setShowTouches(bool enabled) {
    { // acquire lock
        AutoMutex _l(mLock);

        if (mLocked.showTouches == enabled) {
            return;
        }

        ALOGI("Setting show touches feature to %s.", enabled ? "enabled" : "disabled");
        mLocked.showTouches = enabled;
    } // release lock

    mInputManager->getReader()->requestRefreshConfiguration(
            InputReaderConfiguration::CHANGE_SHOW_TOUCHES);
}

void NativeInputManager::requestPointerCapture(const sp<IBinder>& windowToken, bool enabled) {
    mInputManager->getDispatcher()->requestPointerCapture(windowToken, enabled);
}

void NativeInputManager::setInteractive(bool interactive) {
    mInteractive = interactive;
}

void NativeInputManager::reloadCalibration() {
    mInputManager->getReader()->requestRefreshConfiguration(
            InputReaderConfiguration::CHANGE_TOUCH_AFFINE_TRANSFORMATION);
}

void NativeInputManager::setPointerIconType(int32_t iconId) {
    AutoMutex _l(mLock);
    std::shared_ptr<PointerController> controller = mLocked.pointerController.lock();
    if (controller != nullptr) {
        controller->updatePointerIcon(iconId);
    }
}

void NativeInputManager::reloadPointerIcons() {
    AutoMutex _l(mLock);
    std::shared_ptr<PointerController> controller = mLocked.pointerController.lock();
    if (controller != nullptr) {
        controller->reloadPointerResources();
    }
}

void NativeInputManager::setCustomPointerIcon(const SpriteIcon& icon) {
    AutoMutex _l(mLock);
    std::shared_ptr<PointerController> controller = mLocked.pointerController.lock();
    if (controller != nullptr) {
        controller->setCustomPointerIcon(icon);
    }
}

TouchAffineTransformation NativeInputManager::getTouchAffineTransformation(
        JNIEnv *env, jfloatArray matrixArr) {
    ATRACE_CALL();
    ScopedFloatArrayRO matrix(env, matrixArr);
    assert(matrix.size() == 6);

    TouchAffineTransformation transform;
    transform.x_scale  = matrix[0];
    transform.x_ymix   = matrix[1];
    transform.x_offset = matrix[2];
    transform.y_xmix   = matrix[3];
    transform.y_scale  = matrix[4];
    transform.y_offset = matrix[5];

    return transform;
}

TouchAffineTransformation NativeInputManager::getTouchAffineTransformation(
        const std::string& inputDeviceDescriptor, int32_t surfaceRotation) {
    JNIEnv* env = jniEnv();

    ScopedLocalRef<jstring> descriptorObj(env, env->NewStringUTF(inputDeviceDescriptor.c_str()));

    jobject cal = env->CallObjectMethod(mServiceObj,
            gServiceClassInfo.getTouchCalibrationForInputDevice, descriptorObj.get(),
            surfaceRotation);

    jfloatArray matrixArr = jfloatArray(env->CallObjectMethod(cal,
            gTouchCalibrationClassInfo.getAffineTransform));

    TouchAffineTransformation transform = getTouchAffineTransformation(env, matrixArr);

    env->DeleteLocalRef(matrixArr);
    env->DeleteLocalRef(cal);

    return transform;
}

bool NativeInputManager::filterInputEvent(const InputEvent* inputEvent, uint32_t policyFlags) {
    ATRACE_CALL();
    jobject inputEventObj;

    JNIEnv* env = jniEnv();
    switch (inputEvent->getType()) {
    case AINPUT_EVENT_TYPE_KEY:
        inputEventObj = android_view_KeyEvent_fromNative(env,
                static_cast<const KeyEvent*>(inputEvent));
        break;
    case AINPUT_EVENT_TYPE_MOTION:
        inputEventObj = android_view_MotionEvent_obtainAsCopy(env,
                static_cast<const MotionEvent*>(inputEvent));
        break;
    default:
        return true; // dispatch the event normally
    }

    if (!inputEventObj) {
        ALOGE("Failed to obtain input event object for filterInputEvent.");
        return true; // dispatch the event normally
    }

    // The callee is responsible for recycling the event.
    jboolean pass = env->CallBooleanMethod(mServiceObj, gServiceClassInfo.filterInputEvent,
            inputEventObj, policyFlags);
    if (checkAndClearExceptionFromCallback(env, "filterInputEvent")) {
        pass = true;
    }
    env->DeleteLocalRef(inputEventObj);
    return pass;
}

void NativeInputManager::interceptKeyBeforeQueueing(const KeyEvent* keyEvent,
        uint32_t& policyFlags) {
    ATRACE_CALL();
    // Policy:
    // - Ignore untrusted events and pass them along.
    // - Ask the window manager what to do with normal events and trusted injected events.
    // - For normal events wake and brighten the screen if currently off or dim.
    bool interactive = mInteractive.load();
    if (interactive) {
        policyFlags |= POLICY_FLAG_INTERACTIVE;
    }
    if ((policyFlags & POLICY_FLAG_TRUSTED)) {
        nsecs_t when = keyEvent->getEventTime();
        JNIEnv* env = jniEnv();
        jobject keyEventObj = android_view_KeyEvent_fromNative(env, keyEvent);
        jint wmActions;
        if (keyEventObj) {
            wmActions = env->CallIntMethod(mServiceObj,
                    gServiceClassInfo.interceptKeyBeforeQueueing,
                    keyEventObj, policyFlags);
            if (checkAndClearExceptionFromCallback(env, "interceptKeyBeforeQueueing")) {
                wmActions = 0;
            }
            android_view_KeyEvent_recycle(env, keyEventObj);
            env->DeleteLocalRef(keyEventObj);
        } else {
            ALOGE("Failed to obtain key event object for interceptKeyBeforeQueueing.");
            wmActions = 0;
        }

        handleInterceptActions(wmActions, when, /*byref*/ policyFlags);
    } else {
        if (interactive) {
            policyFlags |= POLICY_FLAG_PASS_TO_USER;
        }
    }
}

void NativeInputManager::interceptMotionBeforeQueueing(const int32_t displayId, nsecs_t when,
        uint32_t& policyFlags) {
    ATRACE_CALL();
    // Policy:
    // - Ignore untrusted events and pass them along.
    // - No special filtering for injected events required at this time.
    // - Filter normal events based on screen state.
    // - For normal events brighten (but do not wake) the screen if currently dim.
    bool interactive = mInteractive.load();
    if (interactive) {
        policyFlags |= POLICY_FLAG_INTERACTIVE;
    }
    if ((policyFlags & POLICY_FLAG_TRUSTED) && !(policyFlags & POLICY_FLAG_INJECTED)) {
        if (policyFlags & POLICY_FLAG_INTERACTIVE) {
            policyFlags |= POLICY_FLAG_PASS_TO_USER;
        } else {
            JNIEnv* env = jniEnv();
            jint wmActions = env->CallIntMethod(mServiceObj,
                        gServiceClassInfo.interceptMotionBeforeQueueingNonInteractive,
                        displayId, when, policyFlags);
            if (checkAndClearExceptionFromCallback(env,
                    "interceptMotionBeforeQueueingNonInteractive")) {
                wmActions = 0;
            }

            handleInterceptActions(wmActions, when, /*byref*/ policyFlags);
        }
    } else {
        if (interactive) {
            policyFlags |= POLICY_FLAG_PASS_TO_USER;
        }
    }
}

void NativeInputManager::handleInterceptActions(jint wmActions, nsecs_t when,
        uint32_t& policyFlags) {
    if (wmActions & WM_ACTION_PASS_TO_USER) {
        policyFlags |= POLICY_FLAG_PASS_TO_USER;
    } else {
#if DEBUG_INPUT_DISPATCHER_POLICY
        ALOGD("handleInterceptActions: Not passing key to user.");
#endif
    }
}

nsecs_t NativeInputManager::interceptKeyBeforeDispatching(
        const sp<IBinder>& token,
        const KeyEvent* keyEvent, uint32_t policyFlags) {
    ATRACE_CALL();
    // Policy:
    // - Ignore untrusted events and pass them along.
    // - Filter normal events and trusted injected events through the window manager policy to
    //   handle the HOME key and the like.
    nsecs_t result = 0;
    if (policyFlags & POLICY_FLAG_TRUSTED) {
        JNIEnv* env = jniEnv();
        ScopedLocalFrame localFrame(env);

        // Token may be null
        jobject tokenObj = javaObjectForIBinder(env, token);

        jobject keyEventObj = android_view_KeyEvent_fromNative(env, keyEvent);
        if (keyEventObj) {
            jlong delayMillis = env->CallLongMethod(mServiceObj,
                    gServiceClassInfo.interceptKeyBeforeDispatching,
                    tokenObj, keyEventObj, policyFlags);
            bool error = checkAndClearExceptionFromCallback(env, "interceptKeyBeforeDispatching");
            android_view_KeyEvent_recycle(env, keyEventObj);
            env->DeleteLocalRef(keyEventObj);
            if (!error) {
                if (delayMillis < 0) {
                    result = -1;
                } else if (delayMillis > 0) {
                    result = milliseconds_to_nanoseconds(delayMillis);
                }
            }
        } else {
            ALOGE("Failed to obtain key event object for interceptKeyBeforeDispatching.");
        }
    }
    return result;
}

bool NativeInputManager::dispatchUnhandledKey(const sp<IBinder>& token,
        const KeyEvent* keyEvent, uint32_t policyFlags, KeyEvent* outFallbackKeyEvent) {
    ATRACE_CALL();
    // Policy:
    // - Ignore untrusted events and do not perform default handling.
    bool result = false;
    if (policyFlags & POLICY_FLAG_TRUSTED) {
        JNIEnv* env = jniEnv();
        ScopedLocalFrame localFrame(env);

        // Note: tokenObj may be null.
        jobject tokenObj = javaObjectForIBinder(env, token);
        jobject keyEventObj = android_view_KeyEvent_fromNative(env, keyEvent);
        if (keyEventObj) {
            jobject fallbackKeyEventObj = env->CallObjectMethod(mServiceObj,
                    gServiceClassInfo.dispatchUnhandledKey,
                    tokenObj, keyEventObj, policyFlags);
            if (checkAndClearExceptionFromCallback(env, "dispatchUnhandledKey")) {
                fallbackKeyEventObj = nullptr;
            }
            android_view_KeyEvent_recycle(env, keyEventObj);
            env->DeleteLocalRef(keyEventObj);

            if (fallbackKeyEventObj) {
                // Note: outFallbackKeyEvent may be the same object as keyEvent.
                if (!android_view_KeyEvent_toNative(env, fallbackKeyEventObj,
                        outFallbackKeyEvent)) {
                    result = true;
                }
                android_view_KeyEvent_recycle(env, fallbackKeyEventObj);
                env->DeleteLocalRef(fallbackKeyEventObj);
            }
        } else {
            ALOGE("Failed to obtain key event object for dispatchUnhandledKey.");
        }
    }
    return result;
}

void NativeInputManager::pokeUserActivity(nsecs_t eventTime, int32_t eventType, int32_t displayId) {
    ATRACE_CALL();
    android_server_PowerManagerService_userActivity(eventTime, eventType, displayId);
}

bool NativeInputManager::checkInjectEventsPermissionNonReentrant(
        int32_t injectorPid, int32_t injectorUid) {
    ATRACE_CALL();
    JNIEnv* env = jniEnv();
    jboolean result = env->CallBooleanMethod(mServiceObj,
            gServiceClassInfo.checkInjectEventsPermission, injectorPid, injectorUid);
    if (checkAndClearExceptionFromCallback(env, "checkInjectEventsPermission")) {
        result = false;
    }
    return result;
}

void NativeInputManager::onPointerDownOutsideFocus(const sp<IBinder>& touchedToken) {
    ATRACE_CALL();
    JNIEnv* env = jniEnv();
    ScopedLocalFrame localFrame(env);

    jobject touchedTokenObj = javaObjectForIBinder(env, touchedToken);
    env->CallVoidMethod(mServiceObj, gServiceClassInfo.onPointerDownOutsideFocus, touchedTokenObj);
    checkAndClearExceptionFromCallback(env, "onPointerDownOutsideFocus");
}

void NativeInputManager::setPointerCapture(bool enabled) {
    { // acquire lock
        AutoMutex _l(mLock);

        if (mLocked.pointerCapture == enabled) {
            return;
        }

        ALOGV("%s pointer capture.", enabled ? "Enabling" : "Disabling");
        mLocked.pointerCapture = enabled;
    } // release lock

    mInputManager->getReader()->requestRefreshConfiguration(
            InputReaderConfiguration::CHANGE_POINTER_CAPTURE);
}

void NativeInputManager::loadPointerIcon(SpriteIcon* icon, int32_t displayId) {
    ATRACE_CALL();
    JNIEnv* env = jniEnv();

    ScopedLocalRef<jobject> pointerIconObj(env, env->CallObjectMethod(
            mServiceObj, gServiceClassInfo.getPointerIcon, displayId));
    if (checkAndClearExceptionFromCallback(env, "getPointerIcon")) {
        return;
    }

    ScopedLocalRef<jobject> displayContext(env, env->CallObjectMethod(
            mServiceObj, gServiceClassInfo.getContextForDisplay, displayId));

    PointerIcon pointerIcon;
    status_t status = android_view_PointerIcon_load(env, pointerIconObj.get(),
            displayContext.get(), &pointerIcon);
    if (!status && !pointerIcon.isNullIcon()) {
        *icon = SpriteIcon(
                pointerIcon.bitmap, pointerIcon.style, pointerIcon.hotSpotX, pointerIcon.hotSpotY);
    } else {
        *icon = SpriteIcon();
    }
}

void NativeInputManager::loadPointerResources(PointerResources* outResources, int32_t displayId) {
    ATRACE_CALL();
    JNIEnv* env = jniEnv();

    ScopedLocalRef<jobject> displayContext(env, env->CallObjectMethod(
            mServiceObj, gServiceClassInfo.getContextForDisplay, displayId));

    loadSystemIconAsSprite(env, displayContext.get(), POINTER_ICON_STYLE_SPOT_HOVER,
            &outResources->spotHover);
    loadSystemIconAsSprite(env, displayContext.get(), POINTER_ICON_STYLE_SPOT_TOUCH,
            &outResources->spotTouch);
    loadSystemIconAsSprite(env, displayContext.get(), POINTER_ICON_STYLE_SPOT_ANCHOR,
            &outResources->spotAnchor);
}

void NativeInputManager::loadAdditionalMouseResources(std::map<int32_t, SpriteIcon>* outResources,
        std::map<int32_t, PointerAnimation>* outAnimationResources, int32_t displayId) {
    ATRACE_CALL();
    JNIEnv* env = jniEnv();

    ScopedLocalRef<jobject> displayContext(env, env->CallObjectMethod(
            mServiceObj, gServiceClassInfo.getContextForDisplay, displayId));

    for (int iconId = POINTER_ICON_STYLE_CONTEXT_MENU; iconId <= POINTER_ICON_STYLE_GRABBING;
             ++iconId) {
        PointerIcon pointerIcon;
        loadSystemIconAsSpriteWithPointerIcon(
                env, displayContext.get(), iconId, &pointerIcon, &((*outResources)[iconId]));
        if (!pointerIcon.bitmapFrames.empty()) {
            PointerAnimation& animationData = (*outAnimationResources)[iconId];
            size_t numFrames = pointerIcon.bitmapFrames.size() + 1;
            animationData.durationPerFrame =
                    milliseconds_to_nanoseconds(pointerIcon.durationPerFrame);
            animationData.animationFrames.reserve(numFrames);
            animationData.animationFrames.push_back(SpriteIcon(
                    pointerIcon.bitmap, pointerIcon.style,
                    pointerIcon.hotSpotX, pointerIcon.hotSpotY));
            for (size_t i = 0; i < numFrames - 1; ++i) {
              animationData.animationFrames.push_back(SpriteIcon(
                      pointerIcon.bitmapFrames[i], pointerIcon.style,
                      pointerIcon.hotSpotX, pointerIcon.hotSpotY));
            }
        }
    }
    loadSystemIconAsSprite(env, displayContext.get(), POINTER_ICON_STYLE_NULL,
            &((*outResources)[POINTER_ICON_STYLE_NULL]));
}

int32_t NativeInputManager::getDefaultPointerIconId() {
    return POINTER_ICON_STYLE_ARROW;
}

int32_t NativeInputManager::getCustomPointerIconId() {
    return POINTER_ICON_STYLE_CUSTOM;
}

void NativeInputManager::setMotionClassifierEnabled(bool enabled) {
    mInputManager->getClassifier()->setMotionClassifierEnabled(enabled);
}

// ----------------------------------------------------------------------------

static jlong nativeInit(JNIEnv* env, jclass /* clazz */,
        jobject serviceObj, jobject contextObj, jobject messageQueueObj) {
    sp<MessageQueue> messageQueue = android_os_MessageQueue_getMessageQueue(env, messageQueueObj);
    if (messageQueue == nullptr) {
        jniThrowRuntimeException(env, "MessageQueue is not initialized.");
        return 0;
    }

    NativeInputManager* im = new NativeInputManager(contextObj, serviceObj,
            messageQueue->getLooper());
    im->incStrong(0);
    return reinterpret_cast<jlong>(im);
}

static void nativeStart(JNIEnv* env, jclass /* clazz */, jlong ptr) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    status_t result = im->getInputManager()->start();
    if (result) {
        jniThrowRuntimeException(env, "Input manager could not be started.");
    }
}

static void nativeSetDisplayViewports(JNIEnv* env, jclass /* clazz */, jlong ptr,
        jobjectArray viewportObjArray) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);
    im->setDisplayViewports(env, viewportObjArray);
}

static jint nativeGetScanCodeState(JNIEnv* /* env */, jclass /* clazz */,
        jlong ptr, jint deviceId, jint sourceMask, jint scanCode) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    return (jint) im->getInputManager()->getReader()->getScanCodeState(
            deviceId, uint32_t(sourceMask), scanCode);
}

static jint nativeGetKeyCodeState(JNIEnv* /* env */, jclass /* clazz */,
        jlong ptr, jint deviceId, jint sourceMask, jint keyCode) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    return (jint) im->getInputManager()->getReader()->getKeyCodeState(
            deviceId, uint32_t(sourceMask), keyCode);
}

static jint nativeGetSwitchState(JNIEnv* /* env */, jclass /* clazz */,
        jlong ptr, jint deviceId, jint sourceMask, jint sw) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    return (jint) im->getInputManager()->getReader()->getSwitchState(
            deviceId, uint32_t(sourceMask), sw);
}

static jboolean nativeHasKeys(JNIEnv* env, jclass /* clazz */,
        jlong ptr, jint deviceId, jint sourceMask, jintArray keyCodes, jbooleanArray outFlags) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    int32_t* codes = env->GetIntArrayElements(keyCodes, nullptr);
    uint8_t* flags = env->GetBooleanArrayElements(outFlags, nullptr);
    jsize numCodes = env->GetArrayLength(keyCodes);
    jboolean result;
    if (numCodes == env->GetArrayLength(keyCodes)) {
        if (im->getInputManager()->getReader()->hasKeys(
                deviceId, uint32_t(sourceMask), numCodes, codes, flags)) {
            result = JNI_TRUE;
        } else {
            result = JNI_FALSE;
        }
    } else {
        result = JNI_FALSE;
    }

    env->ReleaseBooleanArrayElements(outFlags, flags, 0);
    env->ReleaseIntArrayElements(keyCodes, codes, 0);
    return result;
}

static void handleInputChannelDisposed(JNIEnv* env, jobject /* inputChannelObj */,
                                       const std::shared_ptr<InputChannel>& inputChannel,
                                       void* data) {
    NativeInputManager* im = static_cast<NativeInputManager*>(data);

    ALOGW("Input channel object '%s' was disposed without first being removed with "
          "the input manager!",
          inputChannel->getName().c_str());
    im->removeInputChannel(env, inputChannel->getConnectionToken());
}

static jobject nativeCreateInputChannel(JNIEnv* env, jclass /* clazz */, jlong ptr,
                                        jstring nameObj) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    ScopedUtfChars nameChars(env, nameObj);
    std::string name = nameChars.c_str();

    base::Result<std::unique_ptr<InputChannel>> inputChannel = im->createInputChannel(env, name);

    if (!inputChannel.ok()) {
        std::string message = inputChannel.error().message();
        message += StringPrintf(" Status=%d", inputChannel.error().code());
        jniThrowRuntimeException(env, message.c_str());
        return nullptr;
    }

    jobject inputChannelObj =
            android_view_InputChannel_createJavaObject(env, std::move(*inputChannel));
    if (!inputChannelObj) {
        return nullptr;
    }

    android_view_InputChannel_setDisposeCallback(env, inputChannelObj,
            handleInputChannelDisposed, im);
    return inputChannelObj;
}

static jobject nativeCreateInputMonitor(JNIEnv* env, jclass /* clazz */, jlong ptr, jint displayId,
                                        jboolean isGestureMonitor, jstring nameObj, jint pid) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    if (displayId == ADISPLAY_ID_NONE) {
        std::string message = "InputChannel used as a monitor must be associated with a display";
        jniThrowRuntimeException(env, message.c_str());
        return nullptr;
    }

    ScopedUtfChars nameChars(env, nameObj);
    std::string name = nameChars.c_str();

    base::Result<std::unique_ptr<InputChannel>> inputChannel =
            im->createInputMonitor(env, displayId, isGestureMonitor, name, pid);

    if (!inputChannel.ok()) {
        std::string message = inputChannel.error().message();
        message += StringPrintf(" Status=%d", inputChannel.error().code());
        jniThrowRuntimeException(env, message.c_str());
        return nullptr;
    }

    jobject inputChannelObj =
            android_view_InputChannel_createJavaObject(env, std::move(*inputChannel));
    if (!inputChannelObj) {
        return nullptr;
    }
    return inputChannelObj;
}

static void nativeRemoveInputChannel(JNIEnv* env, jclass /* clazz */, jlong ptr, jobject tokenObj) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);
    sp<IBinder> token = ibinderForJavaObject(env, tokenObj);

    status_t status = im->removeInputChannel(env, token);
    if (status && status != BAD_VALUE) { // ignore already removed channel
        std::string message;
        message += StringPrintf("Failed to remove input channel.  status=%d", status);
        jniThrowRuntimeException(env, message.c_str());
    }
}

static void nativePilferPointers(JNIEnv* env, jclass /* clazz */, jlong ptr, jobject tokenObj) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);
    sp<IBinder> token = ibinderForJavaObject(env, tokenObj);
    im->pilferPointers(token);
}


static void nativeSetInputFilterEnabled(JNIEnv* /* env */, jclass /* clazz */,
        jlong ptr, jboolean enabled) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->getInputManager()->getDispatcher()->setInputFilterEnabled(enabled);
}

static void nativeSetInTouchMode(JNIEnv* /* env */, jclass /* clazz */,
        jlong ptr, jboolean inTouchMode) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->getInputManager()->getDispatcher()->setInTouchMode(inTouchMode);
}

static void nativeSetMaximumObscuringOpacityForTouch(JNIEnv* /* env */, jclass /* clazz */,
                                                     jlong ptr, jfloat opacity) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->getInputManager()->getDispatcher()->setMaximumObscuringOpacityForTouch(opacity);
}

static void nativeSetBlockUntrustedTouchesMode(JNIEnv* env, jclass /* clazz */, jlong ptr,
                                               jint mode) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->getInputManager()->getDispatcher()->setBlockUntrustedTouchesMode(
            static_cast<BlockUntrustedTouchesMode>(mode));
}

static jint nativeInjectInputEvent(JNIEnv* env, jclass /* clazz */,
        jlong ptr, jobject inputEventObj, jint injectorPid, jint injectorUid,
        jint syncMode, jint timeoutMillis, jint policyFlags) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    // static_cast is safe because the value was already checked at the Java layer
    InputEventInjectionSync mode = static_cast<InputEventInjectionSync>(syncMode);

    if (env->IsInstanceOf(inputEventObj, gKeyEventClassInfo.clazz)) {
        KeyEvent keyEvent;
        status_t status = android_view_KeyEvent_toNative(env, inputEventObj, & keyEvent);
        if (status) {
            jniThrowRuntimeException(env, "Could not read contents of KeyEvent object.");
            return static_cast<jint>(InputEventInjectionResult::FAILED);
        }

        const InputEventInjectionResult result =
                im->getInputManager()->getDispatcher()->injectInputEvent(&keyEvent, injectorPid,
                                                                         injectorUid, mode,
                                                                         std::chrono::milliseconds(
                                                                                 timeoutMillis),
                                                                         uint32_t(policyFlags));
        return static_cast<jint>(result);
    } else if (env->IsInstanceOf(inputEventObj, gMotionEventClassInfo.clazz)) {
        const MotionEvent* motionEvent = android_view_MotionEvent_getNativePtr(env, inputEventObj);
        if (!motionEvent) {
            jniThrowRuntimeException(env, "Could not read contents of MotionEvent object.");
            return static_cast<jint>(InputEventInjectionResult::FAILED);
        }

        const InputEventInjectionResult result =
                im->getInputManager()->getDispatcher()->injectInputEvent(motionEvent, injectorPid,
                                                                         injectorUid, mode,
                                                                         std::chrono::milliseconds(
                                                                                 timeoutMillis),
                                                                         uint32_t(policyFlags));
        return static_cast<jint>(result);
    } else {
        jniThrowRuntimeException(env, "Invalid input event type.");
        return static_cast<jint>(InputEventInjectionResult::FAILED);
    }
}

static jobject nativeVerifyInputEvent(JNIEnv* env, jclass /* clazz */, jlong ptr,
                                      jobject inputEventObj) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    if (env->IsInstanceOf(inputEventObj, gKeyEventClassInfo.clazz)) {
        KeyEvent keyEvent;
        status_t status = android_view_KeyEvent_toNative(env, inputEventObj, &keyEvent);
        if (status) {
            jniThrowRuntimeException(env, "Could not read contents of KeyEvent object.");
            return nullptr;
        }

        std::unique_ptr<VerifiedInputEvent> verifiedEvent =
                im->getInputManager()->getDispatcher()->verifyInputEvent(keyEvent);
        if (verifiedEvent == nullptr) {
            return nullptr;
        }

        return android_view_VerifiedKeyEvent(env,
                                             static_cast<const VerifiedKeyEvent&>(*verifiedEvent));
    } else if (env->IsInstanceOf(inputEventObj, gMotionEventClassInfo.clazz)) {
        const MotionEvent* motionEvent = android_view_MotionEvent_getNativePtr(env, inputEventObj);
        if (!motionEvent) {
            jniThrowRuntimeException(env, "Could not read contents of MotionEvent object.");
            return nullptr;
        }

        std::unique_ptr<VerifiedInputEvent> verifiedEvent =
                im->getInputManager()->getDispatcher()->verifyInputEvent(*motionEvent);

        if (verifiedEvent == nullptr) {
            return nullptr;
        }

        return android_view_VerifiedMotionEvent(env,
                                                static_cast<const VerifiedMotionEvent&>(
                                                        *verifiedEvent));
    } else {
        jniThrowRuntimeException(env, "Invalid input event type.");
        return nullptr;
    }
}

static void nativeToggleCapsLock(JNIEnv* env, jclass /* clazz */,
         jlong ptr, jint deviceId) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->getInputManager()->getReader()->toggleCapsLockState(deviceId);
}

static void nativeDisplayRemoved(JNIEnv* env, jclass /* clazz */, jlong ptr, jint displayId) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->displayRemoved(env, displayId);
}

static void nativeSetFocusedApplication(JNIEnv* env, jclass /* clazz */,
        jlong ptr, jint displayId, jobject applicationHandleObj) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->setFocusedApplication(env, displayId, applicationHandleObj);
}

static void nativeSetFocusedDisplay(JNIEnv* env, jclass /* clazz */,
        jlong ptr, jint displayId) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->setFocusedDisplay(env, displayId);
}

static void nativeRequestPointerCapture(JNIEnv* env, jclass /* clazz */, jlong ptr,
                                        jobject tokenObj, jboolean enabled) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);
    sp<IBinder> windowToken = ibinderForJavaObject(env, tokenObj);

    im->requestPointerCapture(windowToken, enabled);
}

static void nativeSetInputDispatchMode(JNIEnv* /* env */,
        jclass /* clazz */, jlong ptr, jboolean enabled, jboolean frozen) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->setInputDispatchMode(enabled, frozen);
}

static void nativeSetSystemUiLightsOut(JNIEnv* /* env */, jclass /* clazz */, jlong ptr,
                                       jboolean lightsOut) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->setSystemUiLightsOut(lightsOut);
}

static jboolean nativeTransferTouchFocus(JNIEnv* env, jclass /* clazz */, jlong ptr,
                                         jobject fromChannelTokenObj, jobject toChannelTokenObj,
                                         jboolean isDragDrop) {
    if (fromChannelTokenObj == nullptr || toChannelTokenObj == nullptr) {
        return JNI_FALSE;
    }

    sp<IBinder> fromChannelToken = ibinderForJavaObject(env, fromChannelTokenObj);
    sp<IBinder> toChannelToken = ibinderForJavaObject(env, toChannelTokenObj);

    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);
    if (im->getInputManager()->getDispatcher()->transferTouchFocus(fromChannelToken, toChannelToken,
                                                                   isDragDrop)) {
        return JNI_TRUE;
    } else {
        return JNI_FALSE;
    }
}

static jboolean nativeTransferTouch(JNIEnv* env, jclass /* clazz */, jlong ptr,
                                    jobject destChannelTokenObj) {
    sp<IBinder> destChannelToken = ibinderForJavaObject(env, destChannelTokenObj);

    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);
    if (im->getInputManager()->getDispatcher()->transferTouch(destChannelToken)) {
        return JNI_TRUE;
    } else {
        return JNI_FALSE;
    }
}

static void nativeSetPointerSpeed(JNIEnv* /* env */, jclass /* clazz */, jlong ptr, jint speed) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->setPointerSpeed(speed);
}

static void nativeSetShowTouches(JNIEnv* /* env */,
        jclass /* clazz */, jlong ptr, jboolean enabled) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->setShowTouches(enabled);
}

static void nativeSetInteractive(JNIEnv* env,
        jclass clazz, jlong ptr, jboolean interactive) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->setInteractive(interactive);
}

static void nativeReloadCalibration(JNIEnv* env, jclass clazz, jlong ptr) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->reloadCalibration();
}

static void nativeVibrate(JNIEnv* env, jclass /* clazz */, jlong ptr, jint deviceId,
                          jlongArray patternObj, jintArray amplitudesObj, jint repeat, jint token) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    size_t patternSize = env->GetArrayLength(patternObj);
    if (patternSize > MAX_VIBRATE_PATTERN_SIZE) {
        ALOGI("Skipped requested vibration because the pattern size is %zu "
                "which is more than the maximum supported size of %d.",
                patternSize, MAX_VIBRATE_PATTERN_SIZE);
        return; // limit to reasonable size
    }

    jlong* patternMillis = static_cast<jlong*>(env->GetPrimitiveArrayCritical(
            patternObj, nullptr));
    jint* amplitudes = static_cast<jint*>(env->GetPrimitiveArrayCritical(amplitudesObj, nullptr));

    VibrationSequence sequence(patternSize);
    std::vector<int32_t> vibrators = im->getInputManager()->getReader()->getVibratorIds(deviceId);
    for (size_t i = 0; i < patternSize; i++) {
        // VibrationEffect.validate guarantees duration > 0.
        std::chrono::milliseconds duration(patternMillis[i]);
        VibrationElement element(CHANNEL_SIZE);
        element.duration = std::min(duration, MAX_VIBRATE_PATTERN_DELAY_MILLIS);
        // Vibrate on both channels
        for (int32_t channel = 0; channel < vibrators.size(); channel++) {
            element.addChannel(vibrators[channel], static_cast<uint8_t>(amplitudes[i]));
        }
        sequence.addElement(element);
    }
    env->ReleasePrimitiveArrayCritical(patternObj, patternMillis, JNI_ABORT);
    env->ReleasePrimitiveArrayCritical(amplitudesObj, amplitudes, JNI_ABORT);

    im->getInputManager()->getReader()->vibrate(deviceId, sequence, repeat, token);
}

static void nativeVibrateCombined(JNIEnv* env, jclass /* clazz */, jlong ptr, jint deviceId,
                                  jlongArray patternObj, jobject amplitudesObj, jint repeat,
                                  jint token) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    size_t patternSize = env->GetArrayLength(patternObj);

    if (patternSize > MAX_VIBRATE_PATTERN_SIZE) {
        ALOGI("Skipped requested vibration because the pattern size is %zu "
              "which is more than the maximum supported size of %d.",
              patternSize, MAX_VIBRATE_PATTERN_SIZE);
        return; // limit to reasonable size
    }
    const jlong* patternMillis = env->GetLongArrayElements(patternObj, nullptr);

    std::array<jint*, CHANNEL_SIZE> amplitudesArray;
    std::array<jint, CHANNEL_SIZE> vibratorIdArray;
    jint amplSize = env->CallIntMethod(amplitudesObj, gSparseArrayClassInfo.size);
    if (amplSize > CHANNEL_SIZE) {
        ALOGE("Can not fit into input device vibration element.");
        return;
    }

    for (int i = 0; i < amplSize; i++) {
        vibratorIdArray[i] = env->CallIntMethod(amplitudesObj, gSparseArrayClassInfo.keyAt, i);
        jintArray arr = static_cast<jintArray>(
                env->CallObjectMethod(amplitudesObj, gSparseArrayClassInfo.valueAt, i));
        amplitudesArray[i] = env->GetIntArrayElements(arr, nullptr);
        if (env->GetArrayLength(arr) != patternSize) {
            ALOGE("Amplitude length not equal to pattern length!");
            return;
        }
    }

    VibrationSequence sequence(patternSize);
    for (size_t i = 0; i < patternSize; i++) {
        VibrationElement element(CHANNEL_SIZE);
        // VibrationEffect.validate guarantees duration > 0.
        std::chrono::milliseconds duration(patternMillis[i]);
        element.duration = std::min(duration, MAX_VIBRATE_PATTERN_DELAY_MILLIS);
        for (int32_t channel = 0; channel < amplSize; channel++) {
            element.addChannel(vibratorIdArray[channel],
                               static_cast<uint8_t>(amplitudesArray[channel][i]));
        }
        sequence.addElement(element);
    }

    im->getInputManager()->getReader()->vibrate(deviceId, sequence, repeat, token);
}

static void nativeCancelVibrate(JNIEnv* /* env */,
        jclass /* clazz */, jlong ptr, jint deviceId, jint token) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->getInputManager()->getReader()->cancelVibrate(deviceId, token);
}

static bool nativeIsVibrating(JNIEnv* /* env */, jclass /* clazz */, jlong ptr, jint deviceId) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    return im->getInputManager()->getReader()->isVibrating(deviceId);
}

static jintArray nativeGetVibratorIds(JNIEnv* env, jclass clazz, jlong ptr, jint deviceId) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);
    std::vector<int32_t> vibrators = im->getInputManager()->getReader()->getVibratorIds(deviceId);

    jintArray vibIdArray = env->NewIntArray(vibrators.size());
    if (vibIdArray != nullptr) {
        env->SetIntArrayRegion(vibIdArray, 0, vibrators.size(), vibrators.data());
    }
    return vibIdArray;
}

static jobject nativeGetLights(JNIEnv* env, jclass clazz, jlong ptr, jint deviceId) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);
    jobject jLights = env->NewObject(gArrayListClassInfo.clazz, gArrayListClassInfo.constructor);

    std::vector<int> lightIds = im->getInputManager()->getReader()->getLightIds(deviceId);

    for (size_t i = 0; i < lightIds.size(); i++) {
        const InputDeviceLightInfo* lightInfo =
                im->getInputManager()->getReader()->getLightInfo(deviceId, lightIds[i]);
        if (lightInfo == nullptr) {
            ALOGW("Failed to get input device %d light info for id %d", deviceId, lightIds[i]);
            continue;
        }

        jint jTypeId = 0;
        if (lightInfo->type == InputDeviceLightType::SINGLE) {
            jTypeId =
                    env->GetStaticIntField(gLightClassInfo.clazz, gLightClassInfo.lightTypeSingle);
        } else if (lightInfo->type == InputDeviceLightType::PLAYER_ID) {
            jTypeId = env->GetStaticIntField(gLightClassInfo.clazz,
                                             gLightClassInfo.lightTypePlayerId);
        } else if (lightInfo->type == InputDeviceLightType::RGB ||
                   lightInfo->type == InputDeviceLightType::MULTI_COLOR) {
            jTypeId = env->GetStaticIntField(gLightClassInfo.clazz, gLightClassInfo.lightTypeRgb);
        } else {
            ALOGW("Unknown light type %d", lightInfo->type);
            continue;
        }
        ScopedLocalRef<jobject>
                lightObj(env,
                         env->NewObject(gLightClassInfo.clazz, gLightClassInfo.constructor,
                                        (jint)lightInfo->id, (jint)lightInfo->ordinal, jTypeId,
                                        env->NewStringUTF(lightInfo->name.c_str())));
        // Add light object to list
        env->CallBooleanMethod(jLights, gArrayListClassInfo.add, lightObj.get());
    }

    return jLights;
}

static jint nativeGetLightPlayerId(JNIEnv* env, jclass /* clazz */, jlong ptr, jint deviceId,
                                   jint lightId) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    std::optional<int32_t> ret =
            im->getInputManager()->getReader()->getLightPlayerId(deviceId, lightId);

    return static_cast<jint>(ret.value_or(0));
}

static jint nativeGetLightColor(JNIEnv* env, jclass /* clazz */, jlong ptr, jint deviceId,
                                jint lightId) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    std::optional<int32_t> ret =
            im->getInputManager()->getReader()->getLightColor(deviceId, lightId);
    return static_cast<jint>(ret.value_or(0));
}

static void nativeSetLightPlayerId(JNIEnv* env, jclass /* clazz */, jlong ptr, jint deviceId,
                                   jint lightId, jint playerId) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->getInputManager()->getReader()->setLightPlayerId(deviceId, lightId, playerId);
}

static void nativeSetLightColor(JNIEnv* env, jclass /* clazz */, jlong ptr, jint deviceId,
                                jint lightId, jint color) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->getInputManager()->getReader()->setLightColor(deviceId, lightId, color);
}

static jint nativeGetBatteryCapacity(JNIEnv* env, jclass /* clazz */, jlong ptr, jint deviceId) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    std::optional<int32_t> ret = im->getInputManager()->getReader()->getBatteryCapacity(deviceId);
    return static_cast<jint>(ret.value_or(android::os::IInputConstants::INVALID_BATTERY_CAPACITY));
}

static jint nativeGetBatteryStatus(JNIEnv* env, jclass /* clazz */, jlong ptr, jint deviceId) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    std::optional<int32_t> ret = im->getInputManager()->getReader()->getBatteryStatus(deviceId);
    return static_cast<jint>(ret.value_or(BATTERY_STATUS_UNKNOWN));
}

static void nativeReloadKeyboardLayouts(JNIEnv* /* env */,
        jclass /* clazz */, jlong ptr) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->getInputManager()->getReader()->requestRefreshConfiguration(
            InputReaderConfiguration::CHANGE_KEYBOARD_LAYOUTS);
}

static void nativeReloadDeviceAliases(JNIEnv* /* env */,
        jclass /* clazz */, jlong ptr) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->getInputManager()->getReader()->requestRefreshConfiguration(
            InputReaderConfiguration::CHANGE_DEVICE_ALIAS);
}

static jstring nativeDump(JNIEnv* env, jclass /* clazz */, jlong ptr) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    std::string dump;
    im->dump(dump);
    return env->NewStringUTF(dump.c_str());
}

static void nativeMonitor(JNIEnv* /* env */, jclass /* clazz */, jlong ptr) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->getInputManager()->getReader()->monitor();
    im->getInputManager()->getDispatcher()->monitor();
}

static jboolean nativeIsInputDeviceEnabled(JNIEnv* env /* env */,
        jclass /* clazz */, jlong ptr, jint deviceId) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    return im->getInputManager()->getReader()->isInputDeviceEnabled(deviceId);
}

static void nativeEnableInputDevice(JNIEnv* /* env */,
        jclass /* clazz */, jlong ptr, jint deviceId) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->setInputDeviceEnabled(deviceId, true);
}

static void nativeDisableInputDevice(JNIEnv* /* env */,
        jclass /* clazz */, jlong ptr, jint deviceId) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->setInputDeviceEnabled(deviceId, false);
}

static void nativeSetPointerIconType(JNIEnv* /* env */, jclass /* clazz */, jlong ptr, jint iconId) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->setPointerIconType(iconId);
}

static void nativeReloadPointerIcons(JNIEnv* /* env */, jclass /* clazz */, jlong ptr) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->reloadPointerIcons();
}

static void nativeSetCustomPointerIcon(JNIEnv* env, jclass /* clazz */,
                                       jlong ptr, jobject iconObj) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    PointerIcon pointerIcon;
    status_t result = android_view_PointerIcon_getLoadedIcon(env, iconObj, &pointerIcon);
    if (result) {
        jniThrowRuntimeException(env, "Failed to load custom pointer icon.");
        return;
    }

    SpriteIcon spriteIcon(pointerIcon.bitmap.copy(ANDROID_BITMAP_FORMAT_RGBA_8888),
                          pointerIcon.style, pointerIcon.hotSpotX, pointerIcon.hotSpotY);
    im->setCustomPointerIcon(spriteIcon);
}

static jboolean nativeCanDispatchToDisplay(JNIEnv* env, jclass /* clazz */, jlong ptr,
        jint deviceId, jint displayId) {

    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);
    return im->getInputManager()->getReader()->canDispatchToDisplay(deviceId, displayId);
}

static void nativeNotifyPortAssociationsChanged(JNIEnv* env, jclass /* clazz */, jlong ptr) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);
    im->getInputManager()->getReader()->requestRefreshConfiguration(
            InputReaderConfiguration::CHANGE_DISPLAY_INFO);
}

static void nativeChangeUniqueIdAssociation(JNIEnv* env, jclass /* clazz */, jlong ptr) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);
    im->getInputManager()->getReader()->requestRefreshConfiguration(
            InputReaderConfiguration::CHANGE_DISPLAY_INFO);
}

static void nativeSetMotionClassifierEnabled(JNIEnv* /* env */, jclass /* clazz */, jlong ptr,
                                             jboolean enabled) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->setMotionClassifierEnabled(enabled);
}

static jobject createInputSensorInfo(JNIEnv* env, jstring name, jstring vendor, jint version,
                                     jint handle, jint type, jfloat maxRange, jfloat resolution,
                                     jfloat power, jfloat minDelay, jint fifoReservedEventCount,
                                     jint fifoMaxEventCount, jstring stringType,
                                     jstring requiredPermission, jint maxDelay, jint flags,
                                     jint id) {
    // SensorInfo sensorInfo = new Sensor();
    jobject sensorInfo = env->NewObject(gInputSensorInfo.clazz, gInputSensorInfo.init, "");

    if (sensorInfo != NULL) {
        env->SetObjectField(sensorInfo, gInputSensorInfo.name, name);
        env->SetObjectField(sensorInfo, gInputSensorInfo.vendor, vendor);
        env->SetIntField(sensorInfo, gInputSensorInfo.version, version);
        env->SetIntField(sensorInfo, gInputSensorInfo.handle, handle);
        env->SetFloatField(sensorInfo, gInputSensorInfo.maxRange, maxRange);
        env->SetFloatField(sensorInfo, gInputSensorInfo.resolution, resolution);
        env->SetFloatField(sensorInfo, gInputSensorInfo.power, power);
        env->SetIntField(sensorInfo, gInputSensorInfo.minDelay, minDelay);
        env->SetIntField(sensorInfo, gInputSensorInfo.fifoReservedEventCount,
                         fifoReservedEventCount);
        env->SetIntField(sensorInfo, gInputSensorInfo.fifoMaxEventCount, fifoMaxEventCount);
        env->SetObjectField(sensorInfo, gInputSensorInfo.requiredPermission, requiredPermission);
        env->SetIntField(sensorInfo, gInputSensorInfo.maxDelay, maxDelay);
        env->SetIntField(sensorInfo, gInputSensorInfo.flags, flags);
        env->SetObjectField(sensorInfo, gInputSensorInfo.stringType, stringType);
        env->SetIntField(sensorInfo, gInputSensorInfo.type, type);
        env->SetIntField(sensorInfo, gInputSensorInfo.id, id);
    }
    return sensorInfo;
}

static jobjectArray nativeGetSensorList(JNIEnv* env, jclass /* clazz */, jlong ptr, jint deviceId) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);
    std::vector<InputDeviceInfo> devices = im->getInputManager()->getReader()->getInputDevices();
    // Find the input device by deviceId
    auto it = std::find_if(devices.begin(), devices.end(),
                           [deviceId](InputDeviceInfo& info) { return info.getId() == deviceId; });

    if (it == devices.end()) {
        // Return an array of size 0
        return env->NewObjectArray(0, gInputSensorInfo.clazz, nullptr);
    }

    std::vector<InputDeviceSensorType> types = it->getSensorTypes();
    jobjectArray arr = env->NewObjectArray(types.size(), gInputSensorInfo.clazz, nullptr);
    for (int i = 0; i < types.size(); i++) {
        const InputDeviceSensorInfo* sensorInfo = it->getSensorInfo(types[i]);
        if (sensorInfo == nullptr) {
            ALOGW("Failed to get input device %d sensor info for type %s", deviceId,
                  NamedEnum::string(types[i]).c_str());
            continue;
        }

        jobject info =
                createInputSensorInfo(env, env->NewStringUTF(sensorInfo->name.c_str()),
                                      env->NewStringUTF(sensorInfo->vendor.c_str()),
                                      (jint)sensorInfo->version, 0 /* handle */,
                                      (jint)sensorInfo->type, (jfloat)sensorInfo->maxRange,
                                      (jfloat)sensorInfo->resolution, (jfloat)sensorInfo->power,
                                      (jfloat)sensorInfo->minDelay,
                                      (jint)sensorInfo->fifoReservedEventCount,
                                      (jint)sensorInfo->fifoMaxEventCount,
                                      env->NewStringUTF(sensorInfo->stringType.c_str()),
                                      env->NewStringUTF("") /* requiredPermission */,
                                      (jint)sensorInfo->maxDelay, (jint)sensorInfo->flags,
                                      (jint)sensorInfo->id);
        env->SetObjectArrayElement(arr, i, info);
        env->DeleteLocalRef(info);
    }
    return arr;
}

static jboolean nativeEnableSensor(JNIEnv* env, jclass /* clazz */, jlong ptr, jint deviceId,
                                   jint sensorType, jint samplingPeriodUs,
                                   jint maxBatchReportLatencyUs) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    return im->getInputManager()
            ->getReader()
            ->enableSensor(deviceId, static_cast<InputDeviceSensorType>(sensorType),
                           std::chrono::microseconds(samplingPeriodUs),
                           std::chrono::microseconds(maxBatchReportLatencyUs));
}

static void nativeDisableSensor(JNIEnv* env, jclass /* clazz */, jlong ptr, jint deviceId,
                                jint sensorType) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->getInputManager()->getReader()->disableSensor(deviceId,
                                                      static_cast<InputDeviceSensorType>(
                                                              sensorType));
}

static jboolean nativeFlushSensor(JNIEnv* env, jclass /* clazz */, jlong ptr, jint deviceId,
                                  jint sensorType) {
    NativeInputManager* im = reinterpret_cast<NativeInputManager*>(ptr);

    im->getInputManager()->getReader()->flushSensor(deviceId,
                                                    static_cast<InputDeviceSensorType>(sensorType));
    return im->getInputManager()->getDispatcher()->flushSensor(deviceId,
                                                               static_cast<InputDeviceSensorType>(
                                                                       sensorType));
}

// ----------------------------------------------------------------------------

static const JNINativeMethod gInputManagerMethods[] = {
        /* name, signature, funcPtr */
        {"nativeInit",
         "(Lcom/android/server/input/InputManagerService;Landroid/content/Context;Landroid/os/"
         "MessageQueue;)J",
         (void*)nativeInit},
        {"nativeStart", "(J)V", (void*)nativeStart},
        {"nativeSetDisplayViewports", "(J[Landroid/hardware/display/DisplayViewport;)V",
         (void*)nativeSetDisplayViewports},
        {"nativeGetScanCodeState", "(JIII)I", (void*)nativeGetScanCodeState},
        {"nativeGetKeyCodeState", "(JIII)I", (void*)nativeGetKeyCodeState},
        {"nativeGetSwitchState", "(JIII)I", (void*)nativeGetSwitchState},
        {"nativeHasKeys", "(JII[I[Z)Z", (void*)nativeHasKeys},
        {"nativeCreateInputChannel", "(JLjava/lang/String;)Landroid/view/InputChannel;",
         (void*)nativeCreateInputChannel},
        {"nativeCreateInputMonitor", "(JIZLjava/lang/String;I)Landroid/view/InputChannel;",
         (void*)nativeCreateInputMonitor},
        {"nativeRemoveInputChannel", "(JLandroid/os/IBinder;)V", (void*)nativeRemoveInputChannel},
        {"nativePilferPointers", "(JLandroid/os/IBinder;)V", (void*)nativePilferPointers},
        {"nativeSetInputFilterEnabled", "(JZ)V", (void*)nativeSetInputFilterEnabled},
        {"nativeSetInTouchMode", "(JZ)V", (void*)nativeSetInTouchMode},
        {"nativeSetMaximumObscuringOpacityForTouch", "(JF)V",
         (void*)nativeSetMaximumObscuringOpacityForTouch},
        {"nativeSetBlockUntrustedTouchesMode", "(JI)V", (void*)nativeSetBlockUntrustedTouchesMode},
        {"nativeInjectInputEvent", "(JLandroid/view/InputEvent;IIIII)I",
         (void*)nativeInjectInputEvent},
        {"nativeVerifyInputEvent", "(JLandroid/view/InputEvent;)Landroid/view/VerifiedInputEvent;",
         (void*)nativeVerifyInputEvent},
        {"nativeToggleCapsLock", "(JI)V", (void*)nativeToggleCapsLock},
        {"nativeDisplayRemoved", "(JI)V", (void*)nativeDisplayRemoved},
        {"nativeSetFocusedApplication", "(JILandroid/view/InputApplicationHandle;)V",
         (void*)nativeSetFocusedApplication},
        {"nativeSetFocusedDisplay", "(JI)V", (void*)nativeSetFocusedDisplay},
        {"nativeRequestPointerCapture", "(JLandroid/os/IBinder;Z)V",
         (void*)nativeRequestPointerCapture},
        {"nativeSetInputDispatchMode", "(JZZ)V", (void*)nativeSetInputDispatchMode},
        {"nativeSetSystemUiLightsOut", "(JZ)V", (void*)nativeSetSystemUiLightsOut},
        {"nativeTransferTouchFocus", "(JLandroid/os/IBinder;Landroid/os/IBinder;Z)Z",
         (void*)nativeTransferTouchFocus},
        {"nativeTransferTouch", "(JLandroid/os/IBinder;)Z", (void*)nativeTransferTouch},
        {"nativeSetPointerSpeed", "(JI)V", (void*)nativeSetPointerSpeed},
        {"nativeSetShowTouches", "(JZ)V", (void*)nativeSetShowTouches},
        {"nativeSetInteractive", "(JZ)V", (void*)nativeSetInteractive},
        {"nativeReloadCalibration", "(J)V", (void*)nativeReloadCalibration},
        {"nativeVibrate", "(JI[J[III)V", (void*)nativeVibrate},
        {"nativeVibrateCombined", "(JI[JLandroid/util/SparseArray;II)V",
         (void*)nativeVibrateCombined},
        {"nativeCancelVibrate", "(JII)V", (void*)nativeCancelVibrate},
        {"nativeIsVibrating", "(JI)Z", (void*)nativeIsVibrating},
        {"nativeGetVibratorIds", "(JI)[I", (void*)nativeGetVibratorIds},
        {"nativeGetLights", "(JI)Ljava/util/List;", (void*)nativeGetLights},
        {"nativeGetLightPlayerId", "(JII)I", (void*)nativeGetLightPlayerId},
        {"nativeGetLightColor", "(JII)I", (void*)nativeGetLightColor},
        {"nativeSetLightPlayerId", "(JIII)V", (void*)nativeSetLightPlayerId},
        {"nativeSetLightColor", "(JIII)V", (void*)nativeSetLightColor},
        {"nativeGetBatteryCapacity", "(JI)I", (void*)nativeGetBatteryCapacity},
        {"nativeGetBatteryStatus", "(JI)I", (void*)nativeGetBatteryStatus},
        {"nativeReloadKeyboardLayouts", "(J)V", (void*)nativeReloadKeyboardLayouts},
        {"nativeReloadDeviceAliases", "(J)V", (void*)nativeReloadDeviceAliases},
        {"nativeDump", "(J)Ljava/lang/String;", (void*)nativeDump},
        {"nativeMonitor", "(J)V", (void*)nativeMonitor},
        {"nativeIsInputDeviceEnabled", "(JI)Z", (void*)nativeIsInputDeviceEnabled},
        {"nativeEnableInputDevice", "(JI)V", (void*)nativeEnableInputDevice},
        {"nativeDisableInputDevice", "(JI)V", (void*)nativeDisableInputDevice},
        {"nativeSetPointerIconType", "(JI)V", (void*)nativeSetPointerIconType},
        {"nativeReloadPointerIcons", "(J)V", (void*)nativeReloadPointerIcons},
        {"nativeSetCustomPointerIcon", "(JLandroid/view/PointerIcon;)V",
         (void*)nativeSetCustomPointerIcon},
        {"nativeCanDispatchToDisplay", "(JII)Z", (void*)nativeCanDispatchToDisplay},
        {"nativeNotifyPortAssociationsChanged", "(J)V", (void*)nativeNotifyPortAssociationsChanged},
        {"nativeChangeUniqueIdAssociation", "(J)V", (void*)nativeChangeUniqueIdAssociation},
        {"nativeSetMotionClassifierEnabled", "(JZ)V", (void*)nativeSetMotionClassifierEnabled},
        {"nativeGetSensorList", "(JI)[Landroid/hardware/input/InputSensorInfo;",
         (void*)nativeGetSensorList},
        {"nativeEnableSensor", "(JIIII)Z", (void*)nativeEnableSensor},
        {"nativeDisableSensor", "(JII)V", (void*)nativeDisableSensor},
        {"nativeFlushSensor", "(JII)Z", (void*)nativeFlushSensor},
};

#define FIND_CLASS(var, className) \
        var = env->FindClass(className); \
        LOG_FATAL_IF(! (var), "Unable to find class " className);

#define GET_METHOD_ID(var, clazz, methodName, methodDescriptor) \
        var = env->GetMethodID(clazz, methodName, methodDescriptor); \
        LOG_FATAL_IF(! (var), "Unable to find method " methodName);

#define GET_STATIC_METHOD_ID(var, clazz, methodName, methodDescriptor) \
        var = env->GetStaticMethodID(clazz, methodName, methodDescriptor); \
        LOG_FATAL_IF(! (var), "Unable to find static method " methodName);

#define GET_FIELD_ID(var, clazz, fieldName, fieldDescriptor) \
        var = env->GetFieldID(clazz, fieldName, fieldDescriptor); \
        LOG_FATAL_IF(! (var), "Unable to find field " fieldName);

int register_android_server_InputManager(JNIEnv* env) {
    int res = jniRegisterNativeMethods(env, "com/android/server/input/InputManagerService",
            gInputManagerMethods, NELEM(gInputManagerMethods));
    (void) res;  // Faked use when LOG_NDEBUG.
    LOG_FATAL_IF(res < 0, "Unable to register native methods.");

    // Callbacks

    jclass clazz;
    FIND_CLASS(clazz, "com/android/server/input/InputManagerService");
    gServiceClassInfo.clazz = reinterpret_cast<jclass>(env->NewGlobalRef(clazz));

    GET_METHOD_ID(gServiceClassInfo.notifyConfigurationChanged, clazz,
            "notifyConfigurationChanged", "(J)V");

    GET_METHOD_ID(gServiceClassInfo.notifyInputDevicesChanged, clazz,
            "notifyInputDevicesChanged", "([Landroid/view/InputDevice;)V");

    GET_METHOD_ID(gServiceClassInfo.notifySwitch, clazz,
            "notifySwitch", "(JII)V");

    GET_METHOD_ID(gServiceClassInfo.notifyInputChannelBroken, clazz,
            "notifyInputChannelBroken", "(Landroid/os/IBinder;)V");

    GET_METHOD_ID(gServiceClassInfo.notifyFocusChanged, clazz,
            "notifyFocusChanged", "(Landroid/os/IBinder;Landroid/os/IBinder;)V");
    GET_METHOD_ID(gServiceClassInfo.notifyDropWindow, clazz, "notifyDropWindow",
                  "(Landroid/os/IBinder;FF)V");

    GET_METHOD_ID(gServiceClassInfo.notifySensorEvent, clazz, "notifySensorEvent", "(IIIJ[F)V");

    GET_METHOD_ID(gServiceClassInfo.notifySensorAccuracy, clazz, "notifySensorAccuracy", "(III)V");

    GET_METHOD_ID(gServiceClassInfo.notifyVibratorState, clazz, "notifyVibratorState", "(IZ)V");

    GET_METHOD_ID(gServiceClassInfo.notifyUntrustedTouch, clazz, "notifyUntrustedTouch",
                  "(Ljava/lang/String;)V");

    GET_METHOD_ID(gServiceClassInfo.notifyNoFocusedWindowAnr, clazz, "notifyNoFocusedWindowAnr",
                  "(Landroid/view/InputApplicationHandle;)V");

    GET_METHOD_ID(gServiceClassInfo.notifyWindowUnresponsive, clazz, "notifyWindowUnresponsive",
                  "(Landroid/os/IBinder;Ljava/lang/String;)V");

    GET_METHOD_ID(gServiceClassInfo.notifyMonitorUnresponsive, clazz, "notifyMonitorUnresponsive",
                  "(ILjava/lang/String;)V");

    GET_METHOD_ID(gServiceClassInfo.notifyWindowResponsive, clazz, "notifyWindowResponsive",
                  "(Landroid/os/IBinder;)V");

    GET_METHOD_ID(gServiceClassInfo.notifyMonitorResponsive, clazz, "notifyMonitorResponsive",
                  "(I)V");

    GET_METHOD_ID(gServiceClassInfo.filterInputEvent, clazz,
            "filterInputEvent", "(Landroid/view/InputEvent;I)Z");

    GET_METHOD_ID(gServiceClassInfo.interceptKeyBeforeQueueing, clazz,
            "interceptKeyBeforeQueueing", "(Landroid/view/KeyEvent;I)I");

    GET_METHOD_ID(gServiceClassInfo.interceptMotionBeforeQueueingNonInteractive, clazz,
            "interceptMotionBeforeQueueingNonInteractive", "(IJI)I");

    GET_METHOD_ID(gServiceClassInfo.interceptKeyBeforeDispatching, clazz,
            "interceptKeyBeforeDispatching",
            "(Landroid/os/IBinder;Landroid/view/KeyEvent;I)J");

    GET_METHOD_ID(gServiceClassInfo.dispatchUnhandledKey, clazz,
            "dispatchUnhandledKey",
            "(Landroid/os/IBinder;Landroid/view/KeyEvent;I)Landroid/view/KeyEvent;");

    GET_METHOD_ID(gServiceClassInfo.checkInjectEventsPermission, clazz,
            "checkInjectEventsPermission", "(II)Z");

    GET_METHOD_ID(gServiceClassInfo.onPointerDownOutsideFocus, clazz,
            "onPointerDownOutsideFocus", "(Landroid/os/IBinder;)V");

    GET_METHOD_ID(gServiceClassInfo.getVirtualKeyQuietTimeMillis, clazz,
            "getVirtualKeyQuietTimeMillis", "()I");

    GET_STATIC_METHOD_ID(gServiceClassInfo.getExcludedDeviceNames, clazz,
            "getExcludedDeviceNames", "()[Ljava/lang/String;");

    GET_METHOD_ID(gServiceClassInfo.getInputPortAssociations, clazz,
            "getInputPortAssociations", "()[Ljava/lang/String;");

    GET_METHOD_ID(gServiceClassInfo.getInputUniqueIdAssociations, clazz,
                  "getInputUniqueIdAssociations", "()[Ljava/lang/String;");

    GET_METHOD_ID(gServiceClassInfo.getKeyRepeatTimeout, clazz,
            "getKeyRepeatTimeout", "()I");

    GET_METHOD_ID(gServiceClassInfo.getKeyRepeatDelay, clazz,
            "getKeyRepeatDelay", "()I");

    GET_METHOD_ID(gServiceClassInfo.getHoverTapTimeout, clazz,
            "getHoverTapTimeout", "()I");

    GET_METHOD_ID(gServiceClassInfo.getHoverTapSlop, clazz,
            "getHoverTapSlop", "()I");

    GET_METHOD_ID(gServiceClassInfo.getDoubleTapTimeout, clazz,
            "getDoubleTapTimeout", "()I");

    GET_METHOD_ID(gServiceClassInfo.getLongPressTimeout, clazz,
            "getLongPressTimeout", "()I");

    GET_METHOD_ID(gServiceClassInfo.getPointerLayer, clazz,
            "getPointerLayer", "()I");

    GET_METHOD_ID(gServiceClassInfo.getPointerIcon, clazz,
            "getPointerIcon", "(I)Landroid/view/PointerIcon;");

    GET_METHOD_ID(gServiceClassInfo.getPointerDisplayId, clazz,
            "getPointerDisplayId", "()I");

    GET_METHOD_ID(gServiceClassInfo.getKeyboardLayoutOverlay, clazz,
            "getKeyboardLayoutOverlay",
            "(Landroid/hardware/input/InputDeviceIdentifier;)[Ljava/lang/String;");

    GET_METHOD_ID(gServiceClassInfo.getDeviceAlias, clazz,
            "getDeviceAlias", "(Ljava/lang/String;)Ljava/lang/String;");

    GET_METHOD_ID(gServiceClassInfo.getTouchCalibrationForInputDevice, clazz,
            "getTouchCalibrationForInputDevice",
            "(Ljava/lang/String;I)Landroid/hardware/input/TouchCalibration;");

    GET_METHOD_ID(gServiceClassInfo.getContextForDisplay, clazz,
            "getContextForDisplay",
            "(I)Landroid/content/Context;")

    // InputDevice

    FIND_CLASS(gInputDeviceClassInfo.clazz, "android/view/InputDevice");
    gInputDeviceClassInfo.clazz = jclass(env->NewGlobalRef(gInputDeviceClassInfo.clazz));

    // KeyEvent

    FIND_CLASS(gKeyEventClassInfo.clazz, "android/view/KeyEvent");
    gKeyEventClassInfo.clazz = jclass(env->NewGlobalRef(gKeyEventClassInfo.clazz));

    // MotionEvent

    FIND_CLASS(gMotionEventClassInfo.clazz, "android/view/MotionEvent");
    gMotionEventClassInfo.clazz = jclass(env->NewGlobalRef(gMotionEventClassInfo.clazz));

    // InputDeviceIdentifier

    FIND_CLASS(gInputDeviceIdentifierInfo.clazz, "android/hardware/input/InputDeviceIdentifier");
    gInputDeviceIdentifierInfo.clazz = jclass(env->NewGlobalRef(gInputDeviceIdentifierInfo.clazz));
    GET_METHOD_ID(gInputDeviceIdentifierInfo.constructor, gInputDeviceIdentifierInfo.clazz,
            "<init>", "(Ljava/lang/String;II)V");

    // TouchCalibration

    FIND_CLASS(gTouchCalibrationClassInfo.clazz, "android/hardware/input/TouchCalibration");
    gTouchCalibrationClassInfo.clazz = jclass(env->NewGlobalRef(gTouchCalibrationClassInfo.clazz));

    GET_METHOD_ID(gTouchCalibrationClassInfo.getAffineTransform, gTouchCalibrationClassInfo.clazz,
            "getAffineTransform", "()[F");

    // Light
    FIND_CLASS(gLightClassInfo.clazz, "android/hardware/lights/Light");
    gLightClassInfo.clazz = jclass(env->NewGlobalRef(gLightClassInfo.clazz));
    GET_METHOD_ID(gLightClassInfo.constructor, gLightClassInfo.clazz, "<init>",
                  "(IIILjava/lang/String;)V");

    gLightClassInfo.clazz = jclass(env->NewGlobalRef(gLightClassInfo.clazz));
    gLightClassInfo.lightTypeSingle =
            env->GetStaticFieldID(gLightClassInfo.clazz, "LIGHT_TYPE_INPUT_SINGLE", "I");
    gLightClassInfo.lightTypePlayerId =
            env->GetStaticFieldID(gLightClassInfo.clazz, "LIGHT_TYPE_INPUT_PLAYER_ID", "I");
    gLightClassInfo.lightTypeRgb =
            env->GetStaticFieldID(gLightClassInfo.clazz, "LIGHT_TYPE_INPUT_RGB", "I");

    // ArrayList
    FIND_CLASS(gArrayListClassInfo.clazz, "java/util/ArrayList");
    gArrayListClassInfo.clazz = jclass(env->NewGlobalRef(gArrayListClassInfo.clazz));
    GET_METHOD_ID(gArrayListClassInfo.constructor, gArrayListClassInfo.clazz, "<init>", "()V");
    GET_METHOD_ID(gArrayListClassInfo.add, gArrayListClassInfo.clazz, "add",
                  "(Ljava/lang/Object;)Z");

    // SparseArray
    FIND_CLASS(gSparseArrayClassInfo.clazz, "android/util/SparseArray");
    gSparseArrayClassInfo.clazz = jclass(env->NewGlobalRef(gSparseArrayClassInfo.clazz));
    GET_METHOD_ID(gSparseArrayClassInfo.constructor, gSparseArrayClassInfo.clazz, "<init>", "()V");
    GET_METHOD_ID(gSparseArrayClassInfo.keyAt, gSparseArrayClassInfo.clazz, "keyAt", "(I)I");
    GET_METHOD_ID(gSparseArrayClassInfo.valueAt, gSparseArrayClassInfo.clazz, "valueAt",
                  "(I)Ljava/lang/Object;");
    GET_METHOD_ID(gSparseArrayClassInfo.size, gSparseArrayClassInfo.clazz, "size", "()I");
    // InputSensorInfo
    // android.hardware.input.InputDeviceSensorInfo
    FIND_CLASS(clazz, "android/hardware/input/InputSensorInfo");
    gInputSensorInfo.clazz = reinterpret_cast<jclass>(env->NewGlobalRef(clazz));

    GET_FIELD_ID(gInputSensorInfo.name, gInputSensorInfo.clazz, "mName", "Ljava/lang/String;");
    GET_FIELD_ID(gInputSensorInfo.vendor, gInputSensorInfo.clazz, "mVendor", "Ljava/lang/String;");
    GET_FIELD_ID(gInputSensorInfo.version, gInputSensorInfo.clazz, "mVersion", "I");
    GET_FIELD_ID(gInputSensorInfo.handle, gInputSensorInfo.clazz, "mHandle", "I");
    GET_FIELD_ID(gInputSensorInfo.maxRange, gInputSensorInfo.clazz, "mMaxRange", "F");
    GET_FIELD_ID(gInputSensorInfo.resolution, gInputSensorInfo.clazz, "mResolution", "F");
    GET_FIELD_ID(gInputSensorInfo.power, gInputSensorInfo.clazz, "mPower", "F");
    GET_FIELD_ID(gInputSensorInfo.minDelay, gInputSensorInfo.clazz, "mMinDelay", "I");
    GET_FIELD_ID(gInputSensorInfo.fifoReservedEventCount, gInputSensorInfo.clazz,
                 "mFifoReservedEventCount", "I");
    GET_FIELD_ID(gInputSensorInfo.fifoMaxEventCount, gInputSensorInfo.clazz, "mFifoMaxEventCount",
                 "I");
    GET_FIELD_ID(gInputSensorInfo.stringType, gInputSensorInfo.clazz, "mStringType",
                 "Ljava/lang/String;");
    GET_FIELD_ID(gInputSensorInfo.requiredPermission, gInputSensorInfo.clazz, "mRequiredPermission",
                 "Ljava/lang/String;");
    GET_FIELD_ID(gInputSensorInfo.maxDelay, gInputSensorInfo.clazz, "mMaxDelay", "I");
    GET_FIELD_ID(gInputSensorInfo.flags, gInputSensorInfo.clazz, "mFlags", "I");
    GET_FIELD_ID(gInputSensorInfo.type, gInputSensorInfo.clazz, "mType", "I");
    GET_FIELD_ID(gInputSensorInfo.id, gInputSensorInfo.clazz, "mId", "I");

    GET_METHOD_ID(gInputSensorInfo.init, gInputSensorInfo.clazz, "<init>", "()V");

    return 0;
}

} /* namespace android */
