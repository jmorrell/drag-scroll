#include <ApplicationServices/ApplicationServices.h>
#include <dispatch/dispatch.h>
#include <math.h>

#define DEFAULT_BUTTON 5
#define DEFAULT_KEYS kCGEventFlagMaskShift
#define DEFAULT_SPEED 3
#define MAX_KEY_COUNT 5

#define DEFAULT_SMOOTHING_ENABLED true
#define DEFAULT_SMOOTHING_ALPHA 0.35
#define DEFAULT_INERTIA_ENABLED true
#define DEFAULT_INERTIA_DECAY 0.95
#define DEFAULT_INERTIA_THRESHOLD_PIXELS 10.0
#define DEFAULT_INERTIA_THRESHOLD_TIME 0.5
#define DEFAULT_MOVEMENT_STOP_DELAY 0.008
#define EQ(x, y) (CFStringCompare(x, y, kCFCompareCaseInsensitive) == kCFCompareEqualTo)

static const CFStringRef AX_NOTIFICATION = CFSTR("com.apple.accessibility.api");
static bool TRUSTED;

static int BUTTON;
static int KEYS;
static int SPEED;

static bool BUTTON_ENABLED;
static bool KEY_ENABLED;
static CGPoint POINT;

typedef struct {
    bool smoothingEnabled;
    double smoothingAlpha;
    double smoothedDeltaX;
    double smoothedDeltaY;

    bool inertiaEnabled;
    double inertiaDecay;
    double inertiaThresholdPixels;
    double inertiaThresholdTime;
    double movementStopDelay;

    double momentumX;
    double momentumY;
    CFAbsoluteTime lastInputTime;
    dispatch_source_t inertiaTimer;
    bool inertiaActive;

    double accumulatedX;
    double accumulatedY;
    CFAbsoluteTime movementStartTime;
    bool trackingMovement;

    CGEventTapProxy currentProxy;
} ScrollState;

static ScrollState scrollState = {0};
static dispatch_source_t movementCheckTimer = NULL;

static void cancelInertia(void)
{
    if (scrollState.inertiaActive && scrollState.inertiaTimer) {
        dispatch_source_cancel(scrollState.inertiaTimer);
        dispatch_release(scrollState.inertiaTimer);
        scrollState.inertiaTimer = NULL;
        scrollState.inertiaActive = false;
    }
}

static void applySmoothingToDeltas(int rawDeltaX, int rawDeltaY, double *smoothedX, double *smoothedY)
{
    if (!scrollState.smoothingEnabled) {
        *smoothedX = rawDeltaX;
        *smoothedY = rawDeltaY;
        return;
    }

    double alpha = scrollState.smoothingAlpha;
    scrollState.smoothedDeltaX = alpha * rawDeltaX + (1.0 - alpha) * scrollState.smoothedDeltaX;
    scrollState.smoothedDeltaY = alpha * rawDeltaY + (1.0 - alpha) * scrollState.smoothedDeltaY;

    *smoothedX = scrollState.smoothedDeltaX;
    *smoothedY = scrollState.smoothedDeltaY;
}

static void sendScrollEvent(CGEventTapProxy proxy, double deltaX, double deltaY)
{
    CGEventRef scrollWheelEvent = CGEventCreateScrollWheelEvent(
        NULL, kCGScrollEventUnitPixel, 2, -deltaY, -deltaX
    );
    CGEventTapPostEvent(proxy, scrollWheelEvent);
    CFRelease(scrollWheelEvent);
}

static void inertiaTimerCallback(void *context)
{
    scrollState.momentumX *= scrollState.inertiaDecay;
    scrollState.momentumY *= scrollState.inertiaDecay;

    double magnitude = sqrt(scrollState.momentumX * scrollState.momentumX +
                           scrollState.momentumY * scrollState.momentumY);

    if (magnitude < 0.1) {
        cancelInertia();
        return;
    }

    sendScrollEvent(scrollState.currentProxy,
                   SPEED * scrollState.momentumX,
                   SPEED * scrollState.momentumY);
}

static void checkForMovementStop(void)
{
    if (!scrollState.trackingMovement || !scrollState.inertiaEnabled) {
        return;
    }

    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    if (now - scrollState.lastInputTime >= scrollState.movementStopDelay) {
        scrollState.trackingMovement = false;

        CFAbsoluteTime movementDuration = scrollState.lastInputTime - scrollState.movementStartTime;
        double totalDistance = sqrt(scrollState.accumulatedX * scrollState.accumulatedX +
                                  scrollState.accumulatedY * scrollState.accumulatedY);

        if (totalDistance >= scrollState.inertiaThresholdPixels &&
            movementDuration >= scrollState.inertiaThresholdTime) {

            scrollState.momentumX = scrollState.accumulatedX / movementDuration;
            scrollState.momentumY = scrollState.accumulatedY / movementDuration;

            dispatch_queue_t queue = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0);
            scrollState.inertiaTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);

            if (scrollState.inertiaTimer) {
                dispatch_source_set_timer(scrollState.inertiaTimer,
                                        dispatch_time(DISPATCH_TIME_NOW, 16670000),
                                        16670000, 1000000);

                dispatch_source_set_event_handler(scrollState.inertiaTimer, ^{
                    inertiaTimerCallback(NULL);
                });

                scrollState.inertiaActive = true;
                dispatch_resume(scrollState.inertiaTimer);
            }
        }
    }
}

static void maybeSetPointAndWarpMouse(bool thisEnabled, bool otherEnabled, CGEventRef event)
{
    if (!otherEnabled) {
        POINT = CGEventGetLocation(event);
        CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateCombinedSessionState);
        if (thisEnabled) {
            CGEventSourceSetLocalEventsSuppressionInterval(source, 10.0);
            CGWarpMouseCursorPosition(POINT);
        } else {
            CGEventSourceSetLocalEventsSuppressionInterval(source, 0.0);
            CGWarpMouseCursorPosition(POINT);
            CGEventSourceSetLocalEventsSuppressionInterval(source, 0.25);
        }
        CFRelease(source);
    }
}

static CGEventRef tapCallback(CGEventTapProxy proxy,
                              CGEventType type, CGEventRef event, void *userInfo)
{
    if (type == kCGEventMouseMoved && (BUTTON_ENABLED || KEY_ENABLED)) {
        cancelInertia();

        int rawDeltaX = (int)CGEventGetIntegerValueField(event, kCGMouseEventDeltaX);
        int rawDeltaY = (int)CGEventGetIntegerValueField(event, kCGMouseEventDeltaY);

        double smoothedDeltaX, smoothedDeltaY;
        applySmoothingToDeltas(rawDeltaX, rawDeltaY, &smoothedDeltaX, &smoothedDeltaY);

        scrollState.lastInputTime = CFAbsoluteTimeGetCurrent();

        if (!scrollState.trackingMovement) {
            scrollState.trackingMovement = true;
            scrollState.movementStartTime = scrollState.lastInputTime;
            scrollState.accumulatedX = 0.0;
            scrollState.accumulatedY = 0.0;
        }
        scrollState.accumulatedX += smoothedDeltaX;
        scrollState.accumulatedY += smoothedDeltaY;

        scrollState.currentProxy = proxy;

        CGEventRef scrollWheelEvent = CGEventCreateScrollWheelEvent(
            NULL, kCGScrollEventUnitPixel, 2,
            -SPEED * smoothedDeltaY, -SPEED * smoothedDeltaX
        );
        if (KEY_ENABLED)
            CGEventSetFlags(scrollWheelEvent, CGEventGetFlags(event) & ~KEYS);
        CGEventTapPostEvent(proxy, scrollWheelEvent);
        CFRelease(scrollWheelEvent);
        CGWarpMouseCursorPosition(POINT);
        event = NULL;
    } else if (type == kCGEventOtherMouseDown
               && CGEventGetFlags(event) == 0
               && CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber) == BUTTON) {
        BUTTON_ENABLED = !BUTTON_ENABLED;
        maybeSetPointAndWarpMouse(BUTTON_ENABLED, KEY_ENABLED, event);
        event = NULL;
    } else if (type == kCGEventFlagsChanged) {
        KEY_ENABLED = (CGEventGetFlags(event) & KEYS) == KEYS;
        maybeSetPointAndWarpMouse(KEY_ENABLED, BUTTON_ENABLED, event);
    }

    return event;
}

static void displayNoticeAndExit(CFStringRef alertHeader)
{
    CFUserNotificationDisplayNotice(
        0, kCFUserNotificationCautionAlertLevel,
        NULL, NULL, NULL,
        alertHeader, NULL, NULL
    );

    exit(EXIT_FAILURE);
}

static void notificationCallback(CFNotificationCenterRef center, void *observer,
                                 CFNotificationName name, const void *object,
                                 CFDictionaryRef userInfo)
{
    CFRunLoopRef runLoop = CFRunLoopGetCurrent();
    CFRunLoopPerformBlock(
        runLoop, kCFRunLoopDefaultMode, ^{
            bool previouslyTrusted = TRUSTED;
            if ((TRUSTED = AXIsProcessTrusted()) && !previouslyTrusted)
                CFRunLoopStop(runLoop);
        }
    );
}

static bool getIntPreference(CFStringRef key, int *valuePtr)
{
    CFNumberRef number = (CFNumberRef)CFPreferencesCopyAppValue(
        key, kCFPreferencesCurrentApplication
    );
    bool got = false;
    if (number) {
        if (CFGetTypeID(number) == CFNumberGetTypeID())
            got = CFNumberGetValue(number, kCFNumberIntType, valuePtr);
        CFRelease(number);
    }

    return got;
}

static bool getArrayPreference(CFStringRef key, CFStringRef *values, int *count, int maxCount)
{
    CFArrayRef array = (CFArrayRef)CFPreferencesCopyAppValue(
        key, kCFPreferencesCurrentApplication
    );
    bool got = false;
    if (array) {
        if (CFGetTypeID(array) == CFArrayGetTypeID()) {
            CFIndex c = CFArrayGetCount(array);
            if (c <= maxCount) {
                CFArrayGetValues(array, CFRangeMake(0, c), (const void **)values);
                *count = (int)c;
                got = true;
            }
        }
        CFRelease(array);
    }

    return got;
}

static bool getBoolPreference(CFStringRef key, bool *valuePtr)
{
    CFBooleanRef boolean = (CFBooleanRef)CFPreferencesCopyAppValue(
        key, kCFPreferencesCurrentApplication
    );
    bool got = false;
    if (boolean) {
        if (CFGetTypeID(boolean) == CFBooleanGetTypeID()) {
            *valuePtr = CFBooleanGetValue(boolean);
            got = true;
        }
        CFRelease(boolean);
    }
    return got;
}

static bool getDoublePreference(CFStringRef key, double *valuePtr)
{
    CFNumberRef number = (CFNumberRef)CFPreferencesCopyAppValue(
        key, kCFPreferencesCurrentApplication
    );
    bool got = false;
    if (number) {
        if (CFGetTypeID(number) == CFNumberGetTypeID())
            got = CFNumberGetValue(number, kCFNumberDoubleType, valuePtr);
        CFRelease(number);
    }
    return got;
}

static void initializeScrollState(void)
{
    if (!getBoolPreference(CFSTR("smoothingEnabled"), &scrollState.smoothingEnabled))
        scrollState.smoothingEnabled = DEFAULT_SMOOTHING_ENABLED;

    if (!getDoublePreference(CFSTR("smoothingAlpha"), &scrollState.smoothingAlpha))
        scrollState.smoothingAlpha = DEFAULT_SMOOTHING_ALPHA;

    if (!getBoolPreference(CFSTR("inertiaEnabled"), &scrollState.inertiaEnabled))
        scrollState.inertiaEnabled = DEFAULT_INERTIA_ENABLED;

    if (!getDoublePreference(CFSTR("inertiaDecay"), &scrollState.inertiaDecay))
        scrollState.inertiaDecay = DEFAULT_INERTIA_DECAY;

    if (!getDoublePreference(CFSTR("inertiaThresholdPixels"), &scrollState.inertiaThresholdPixels))
        scrollState.inertiaThresholdPixels = DEFAULT_INERTIA_THRESHOLD_PIXELS;

    if (!getDoublePreference(CFSTR("inertiaThresholdTime"), &scrollState.inertiaThresholdTime))
        scrollState.inertiaThresholdTime = DEFAULT_INERTIA_THRESHOLD_TIME;

    if (!getDoublePreference(CFSTR("movementStopDelay"), &scrollState.movementStopDelay))
        scrollState.movementStopDelay = DEFAULT_MOVEMENT_STOP_DELAY;

    dispatch_queue_t queue = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);
    movementCheckTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);

    if (movementCheckTimer) {
        dispatch_source_set_timer(movementCheckTimer,
                                dispatch_time(DISPATCH_TIME_NOW, 5000000),
                                5000000, 500000);

        dispatch_source_set_event_handler(movementCheckTimer, ^{
            checkForMovementStop();
        });

        dispatch_resume(movementCheckTimer);
    }
}

static void cleanupScrollState(void)
{
    cancelInertia();

    if (movementCheckTimer) {
        dispatch_source_cancel(movementCheckTimer);
        dispatch_release(movementCheckTimer);
        movementCheckTimer = NULL;
    }
}

int main(void)
{
    CFNotificationCenterRef center = CFNotificationCenterGetDistributedCenter();
    char observer;
    CFNotificationCenterAddObserver(
        center, &observer, notificationCallback, AX_NOTIFICATION, NULL,
        CFNotificationSuspensionBehaviorDeliverImmediately
    );
    CFDictionaryRef options = CFDictionaryCreate(
        kCFAllocatorDefault,
        (const void **)&kAXTrustedCheckOptionPrompt, (const void **)&kCFBooleanTrue, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
    );
    TRUSTED = AXIsProcessTrustedWithOptions(options);
    CFRelease(options);
    if (!TRUSTED)
        CFRunLoopRun();
    CFNotificationCenterRemoveObserver(center, &observer, AX_NOTIFICATION, NULL);

    if (!(getIntPreference(CFSTR("button"), &BUTTON)
          && (BUTTON == 0 || (BUTTON >= 3 && BUTTON <= 32))))
        BUTTON = DEFAULT_BUTTON;

    CFStringRef keyNames[MAX_KEY_COUNT];
    int keyCount;
    if (getArrayPreference(CFSTR("keys"), keyNames, &keyCount, MAX_KEY_COUNT)) {
        KEYS = 0;
        for (int i = 0; i < keyCount; i++) {
            if (EQ(keyNames[i], CFSTR("capslock"))) {
                KEYS |= kCGEventFlagMaskAlphaShift;
            } else if (EQ(keyNames[i], CFSTR("shift"))) {
                KEYS |= kCGEventFlagMaskShift;
            } else if (EQ(keyNames[i], CFSTR("control"))) {
                KEYS |= kCGEventFlagMaskControl;
            } else if (EQ(keyNames[i], CFSTR("option"))) {
                KEYS |= kCGEventFlagMaskAlternate;
            } else if (EQ(keyNames[i], CFSTR("command"))) {
                KEYS |= kCGEventFlagMaskCommand;
            } else {
                KEYS = DEFAULT_KEYS;
                break;
            }
        }
    } else {
        KEYS = DEFAULT_KEYS;
    }

    if (!getIntPreference(CFSTR("speed"), &SPEED))
        SPEED = DEFAULT_SPEED;

    initializeScrollState();

    CGEventMask events = CGEventMaskBit(kCGEventMouseMoved);
    if (BUTTON != 0) {
        events |= CGEventMaskBit(kCGEventOtherMouseDown);
        BUTTON--;
    }
    if (KEYS != 0)
        events |= CGEventMaskBit(kCGEventFlagsChanged);
    CFMachPortRef tap = CGEventTapCreate(
        kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
        events, tapCallback, NULL
    );
    if (!tap)
        displayNoticeAndExit(CFSTR("DragScroll could not create an event tap."));
    CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
    if (!source)
        displayNoticeAndExit(CFSTR("DragScroll could not create a run loop source."));
    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
    CFRelease(tap);
    CFRelease(source);
    CFRunLoopRun();

    return EXIT_SUCCESS;
}
