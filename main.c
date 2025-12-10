#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <signal.h>

// MARK: - Configuration Options
typedef struct {
    bool invertVerticalScroll;
    bool disableScrollAccel;
    int64_t scrollLines;
    bool alternateDetectionMethod;
} Options;

static Options* create_options(
    bool invertVerticalScroll,
    bool disableScrollAccel,
    int scrollLines,
    bool alternateDetectionMethod
) {
    Options *options = malloc(sizeof(Options));
    if (!options) {
        fprintf(stderr, "Failed to allocate memory for options\n");
        exit(1);
    }

    options->invertVerticalScroll = invertVerticalScroll;
    options->disableScrollAccel = disableScrollAccel;
    options->scrollLines = (int64_t)scrollLines;
    options->alternateDetectionMethod = alternateDetectionMethod;
    return options;
}

static Options* get_shared_options(void) {
    static Options *sharedOptions = NULL;
    if (sharedOptions == NULL) {
        sharedOptions = create_options(true, false, 3, false);
    }
    return sharedOptions;
}

// MARK: - Scroll Interceptor
// Cache field constants to avoid repeated lookups (equivalent to Swift's static lets)
static const CGEventField kIsContinuousField = kCGScrollWheelEventIsContinuous;
static const CGEventField kMomentumPhaseField = kCGScrollWheelEventMomentumPhase;
static const CGEventField kScrollCountField = kCGScrollWheelEventScrollCount;
static const CGEventField kScrollPhaseField = kCGScrollWheelEventScrollPhase;
static const CGEventField kDeltaAxis1Field = kCGScrollWheelEventDeltaAxis1;
// static const CGEventField kDeltaAxis2Field = kCGScrollWheelEventDeltaAxis2;

// Global reference for cleanup
static CFMachPortRef g_eventTap = NULL;
static CFRunLoopSourceRef g_runLoopSource = NULL;

// Inline function equivalent to Swift's @inline(__always)
static inline bool is_wheel_event(CGEventRef event, const Options *options) {
    if (!options->alternateDetectionMethod) {
        return CGEventGetIntegerValueField(event, kIsContinuousField) == 0;
    } else {
        return (CGEventGetIntegerValueField(event, kMomentumPhaseField) == 0 &&
                CGEventGetDoubleValueField(event, kScrollCountField) == 0.0 &&
                CGEventGetDoubleValueField(event, kScrollPhaseField) == 0.0);
    }
}

// Inline function equivalent to Swift's @inline(__always)
static inline void process_scroll_event(CGEventRef event, const Options *options) {
    if (options->invertVerticalScroll) {
        int64_t value = CGEventGetIntegerValueField(event, kDeltaAxis1Field);
        CGEventSetIntegerValueField(event, kDeltaAxis1Field, -value);
    }

    if (options->disableScrollAccel) {
        int64_t currentValue = CGEventGetIntegerValueField(event, kDeltaAxis1Field);
        int64_t sign = (currentValue > 0) ? 1 : (currentValue < 0) ? -1 : 0;
        CGEventSetIntegerValueField(event, kDeltaAxis1Field, sign * options->scrollLines);
    }
}

static CGEventRef scroll_event_callback(CGEventTapProxy proxy __attribute__((unused)), CGEventType type __attribute__((unused)), CGEventRef event, void *userInfo __attribute__((unused))) {
    const Options *options = get_shared_options();
    bool isWheel = is_wheel_event(event, options);

    if (isWheel) {
        process_scroll_event(event, options);
    }

    // Return the event to continue processing (equivalent to Swift's Unmanaged.passUnretained(event))
    return event;
}

static void cleanup_resources(void) {
    if (g_eventTap) {
        CGEventTapEnable(g_eventTap, false);
        CFRelease(g_eventTap);
        g_eventTap = NULL;
    }

    if (g_runLoopSource) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), g_runLoopSource, kCFRunLoopCommonModes);
        CFRelease(g_runLoopSource);
        g_runLoopSource = NULL;
    }
}

static void signal_handler(int sig) {
    printf("\nReceived signal %d, cleaning up...\n", sig);
    cleanup_resources();
    exit(0);
}

static void setup_signal_handlers(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
}

void intercept_scroll(void) {
    setup_signal_handlers();

    // Create event tap with same parameters as Swift version
    CGEventMask eventMask = CGEventMaskBit(kCGEventScrollWheel);
    g_eventTap = CGEventTapCreate(
        kCGHIDEventTap,                // .cghidEventTap
        kCGTailAppendEventTap,         // .tailAppendEventTap
        kCGEventTapOptionDefault,       // .defaultTap
        eventMask,
        scroll_event_callback,
        NULL
    );

    if (!g_eventTap) {
        fprintf(stderr, "Failed to create event tap. Ensure the app has Accessibility permissions.\n");
        exit(1);
    }

    g_runLoopSource = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, g_eventTap, 0);
    if (!g_runLoopSource) {
        fprintf(stderr, "Failed to create run loop source.\n");
        CFRelease(g_eventTap);
        exit(1);
    }

    CFRunLoopAddSource(CFRunLoopGetCurrent(), g_runLoopSource, kCFRunLoopCommonModes);
    CGEventTapEnable(g_eventTap, true);

    printf("Scroll interceptor running. Press Ctrl+C to stop.\n");
    CFRunLoopRun();
}

// MARK: - Main Execution
int main(int argc __attribute__((unused)), const char * argv[] __attribute__((unused))) {
    // Start the scroll interceptor (equivalent to Swift's ScrollInterceptor.shared.interceptScroll())
    intercept_scroll();

    // Clean up on exit
    cleanup_resources();
    return 0;
}
