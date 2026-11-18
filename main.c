/*
 * scroll -- inverts mouse (non-continuous) scroll wheel direction on macOS.
 *
 * Hardened version. A CGEventTap dies in three silent ways, none of which the
 * previous revision handled:
 *
 *   1. kCGEventTapDisabledByTimeout    callback missed its deadline; the tap is
 *                                      switched off and you are told once
 *   2. kCGEventTapDisabledByUserInput  secure input (password fields) became
 *                                      active; the tap is switched off
 *   3. mach port invalidation          WindowServer went away; the tap is dead
 *                                      for good and can never be re-enabled
 *
 * The old code returned the event for cases 1 and 2 without re-enabling, so the
 * tap stayed disabled forever while the process kept running: scroll inversion
 * silently stopped, with no crash and no log line. That is the "works for a
 * few milliseconds, service still listed as running" symptom.
 *
 * This version re-enables immediately inside the callback, backs that up with a
 * run loop watchdog (in case the disable notification never arrives), and exits
 * on invalidation so launchd can relaunch a fresh tap.
 *
 * Trackpad lag. An active tap is synchronous: every scroll event it sees -- the
 * trackpad's included -- pays a round-trip to this process before the app gets
 * it. The callback itself is ~1 us, so the round-trip is the cost, and when only
 * the trackpad is being used it buys nothing (the event is passed through
 * unchanged). A second, listen-only tap watches for mouse activity; while the
 * trackpad is the only thing being used the active tap is switched off, so
 * trackpad events never enter the delivery path at all. It is switched back on
 * as soon as the mouse moves or scrolls. Listen-only taps are not in the
 * delivery path, so the watcher adds no latency.
 *
 * Tradeoff: the very first mouse scroll after a long trackpad-only stretch can
 * be missed, because the active tap is off until the watcher sees the event and
 * turns it on. Mouse movement normally precedes scrolling, so this is rare.
 */

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Watchdog cadence, seconds. Cheap: one CGEventTapIsEnabled() call per tick. */
#define WATCHDOG_INTERVAL 2.0
/* Log a stats line every N ticks, and only if something actually happened. */
#define STATS_EVERY_TICKS 150

/*
 * How long after the last mouse activity the active tap stays on. Long enough
 * that an accidental palm-brush on the trackpad does not switch it off while
 * the mouse is in use; short enough that trackpad-only scrolling goes quiet
 * quickly.
 */
#define MOUSE_IDLE_NS 1000000000ULL /* 1.0 s */

static CFMachPortRef g_tap = NULL;        /* active: inverts mouse wheels */
static CFRunLoopSourceRef g_source = NULL;
static CFMachPortRef g_watch_tap = NULL;  /* listen-only: mouse activity */
static CFRunLoopSourceRef g_watch_source = NULL;
static CFRunLoopRef g_runloop = NULL;
static CFRunLoopTimerRef g_watchdog = NULL;

static volatile sig_atomic_t g_stop = 0;
/* Set while tearing down deliberately, so our own CFMachPortInvalidate call is
 * not reported as an unexpected port death. */
static int g_tearing_down = 0;
/* Whether the active tap is supposed to be on. Cleared when only the trackpad
 * is in use, so reenable_tap() does not fight the watcher. */
static int g_want_enabled = 1;

/* Monotonic time of the last mouse movement / mouse-wheel event. */
static uint64_t g_last_mouse_ns = 0;

static unsigned long g_timeout_disables = 0;
static unsigned long g_userinput_disables = 0;
static unsigned long g_reenables = 0;
static unsigned long g_inversions = 0;
static unsigned long g_trackpad_skipped = 0;
static unsigned long g_watchdog_ticks = 0;

/* Counters at the last stats line, so idle periods stay out of the log. */
static unsigned long g_reported_timeout = 0;
static unsigned long g_reported_userinput = 0;
static unsigned long g_reported_reenables = 0;
static unsigned long g_reported_inversions = 0;
static unsigned long g_reported_trackpad = 0;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void logmsg(const char *fmt, ...) __attribute__((__format__(__printf__, 1, 2)));

static void logmsg(const char *fmt, ...) {
    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;

    localtime_r(&now, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);

    fprintf(stderr, "[%s] [scroll:%d] ", ts, (int)getpid());

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
    fflush(stderr);
}

/*
 * Turn the active tap on or off. Only touches the tap when the state actually
 * changes, so the watcher can call this on every mouse-move event cheaply.
 */
static void set_tap_enabled(int want) {
    if (want == g_want_enabled) {
        return;
    }
    g_want_enabled = want;
    if (g_tap != NULL) {
        CGEventTapEnable(g_tap, want);
        logmsg("active tap %s (mouse %s)", want ? "enabled" : "disabled",
               want ? "active" : "idle");
    }
}

/* Re-enable the tap if it is currently off and we want it on.
 * Returns 1 if a re-enable happened. */
static int reenable_tap(const char *why) {
    if (g_tap == NULL) {
        return 0;
    }
    if (!g_want_enabled) {
        return 0; /* deliberately off: trackpad-only mode */
    }
    if (CGEventTapIsEnabled(g_tap)) {
        return 0;
    }

    CGEventTapEnable(g_tap, true);
    g_reenables++;
    logmsg("event tap re-enabled (%s), reenables=%lu", why, g_reenables);
    return 1;
}

/*
 * Is this scroll event a real mouse wheel, i.e. something we should invert?
 *
 * kCGScrollWheelEventIsContinuous alone is not enough: the internal trackpad
 * also emits events with that flag clear (especially during momentum), which is
 * why an earlier revision inverted the trackpad too.
 *
 * A real wheel tick belongs to no gesture sequence: scroll phase and momentum
 * phase are both absent. A trackpad event always belongs to one, so requiring
 * both phases to be zero reliably selects wheel events.
 *
 * Kept free of CoreGraphics calls so it can be unit tested.
 */
static int is_mouse_wheel(int is_continuous, int scroll_phase,
                          int momentum_phase) {
    return is_continuous == 0 && scroll_phase == 0 && momentum_phase == 0;
}

static CGEventRef event_callback(CGEventTapProxy proxy, CGEventType type,
                                 CGEventRef event, void *refcon) {
    (void)proxy;
    (void)refcon;

    switch (type) {
    case kCGEventTapDisabledByTimeout:
        /*
         * Our callback ran past the deadline (system under load, or another tap
         * on the same stream stalled). Re-enable straight away: this is the
         * notification, not a death sentence.
         *
         * A deliberate CGEventTapEnable(false) from the watcher also arrives
         * here, so ignore it instead of counting it as a real timeout.
         */
        if (!g_want_enabled) {
            return event;
        }
        g_timeout_disables++;
        logmsg("tap disabled by TIMEOUT (count=%lu) -> re-enabling",
               g_timeout_disables);
        if (g_tap != NULL) {
            CGEventTapEnable(g_tap, true);
            g_reenables++;
        }
        return event;

    case kCGEventTapDisabledByUserInput:
        /*
         * Secure input (password field, sudo prompt) disabled the tap. Safe to
         * re-enable; the system re-disables while secure input remains active.
         *
         * A deliberate CGEventTapEnable(false) from the watcher arrives here
         * too, so ignore it instead of counting it as secure input.
         */
        if (!g_want_enabled) {
            return event;
        }
        g_userinput_disables++;
        logmsg("tap disabled by USER INPUT (count=%lu) -> re-enabling",
               g_userinput_disables);
        if (g_tap != NULL) {
            CGEventTapEnable(g_tap, true);
            g_reenables++;
        }
        return event;

    case kCGEventScrollWheel: {
        int64_t is_continuous =
            CGEventGetIntegerValueField(event, kCGScrollWheelEventIsContinuous);
        int64_t scroll_phase =
            CGEventGetIntegerValueField(event, kCGScrollWheelEventScrollPhase);
        int64_t momentum_phase =
            CGEventGetIntegerValueField(event, kCGScrollWheelEventMomentumPhase);

        if (!is_mouse_wheel((int)is_continuous, (int)scroll_phase,
                            (int)momentum_phase)) {
            /* Trackpad gesture or synthetic event: leave it completely alone. */
            g_trackpad_skipped++;
            return event;
        }

        int64_t delta_y =
            CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis1);
        CGEventSetIntegerValueField(event, kCGScrollWheelEventDeltaAxis1,
                                    -delta_y);
        g_inversions++;
        return event;
    }

    default:
        return event;
    }
}

/*
 * Listen-only watcher. Never blocks the event stream; it only decides whether
 * the active tap should be on. Mouse movement or a mouse-wheel event keeps the
 * active tap on; once the trackpad has been the only input for MOUSE_IDLE_NS,
 * the first trackpad scroll switches the active tap off.
 */
static CGEventRef observe_callback(CGEventTapProxy proxy, CGEventType type,
                                   CGEventRef event, void *refcon) {
    (void)proxy;
    (void)refcon;

    switch (type) {
    case kCGEventMouseMoved:
    case kCGEventLeftMouseDragged:
    case kCGEventRightMouseDragged:
    case kCGEventOtherMouseDragged:
        g_last_mouse_ns = now_ns();
        set_tap_enabled(1);
        return event;

    case kCGEventScrollWheel: {
        int64_t is_continuous =
            CGEventGetIntegerValueField(event, kCGScrollWheelEventIsContinuous);
        int64_t scroll_phase =
            CGEventGetIntegerValueField(event, kCGScrollWheelEventScrollPhase);
        int64_t momentum_phase =
            CGEventGetIntegerValueField(event, kCGScrollWheelEventMomentumPhase);

        if (is_mouse_wheel((int)is_continuous, (int)scroll_phase,
                           (int)momentum_phase)) {
            g_last_mouse_ns = now_ns();
            set_tap_enabled(1);
        } else if (now_ns() - g_last_mouse_ns > MOUSE_IDLE_NS) {
            /* Trackpad is the only thing in use: stop paying the tap cost. */
            set_tap_enabled(0);
        }
        return event;
    }

    default:
        return event;
    }
}

/*
 * Belt and braces: if a disable notification is ever dropped, this notices the
 * tap is off and turns it back on. It also emits a stats line when counters
 * have moved since the last one.
 */
static void watchdog_cb(CFRunLoopTimerRef timer, void *info) {
    (void)timer;
    (void)info;

    g_watchdog_ticks++;
    (void)reenable_tap("watchdog");

    if (g_watchdog_ticks % STATS_EVERY_TICKS != 0) {
        return;
    }

    if (g_timeout_disables == g_reported_timeout &&
        g_userinput_disables == g_reported_userinput &&
        g_reenables == g_reported_reenables &&
        g_inversions == g_reported_inversions &&
        g_trackpad_skipped == g_reported_trackpad) {
        return; /* nothing happened; stay quiet */
    }

    logmsg("stats: wheel_inverted=%lu trackpad_passed=%lu timeouts=%lu "
           "userinput=%lu reenables=%lu enabled=%d",
           g_inversions, g_trackpad_skipped, g_timeout_disables,
           g_userinput_disables, g_reenables,
           g_tap != NULL && CGEventTapIsEnabled(g_tap) ? 1 : 0);

    g_reported_timeout = g_timeout_disables;
    g_reported_userinput = g_userinput_disables;
    g_reported_reenables = g_reenables;
    g_reported_inversions = g_inversions;
    g_reported_trackpad = g_trackpad_skipped;
}

/* The port died (WindowServer restart, stream torn down). Unrecoverable. */
static void tap_invalidated(CFMachPortRef port, void *info) {
    (void)port;
    (void)info;

    if (g_tearing_down) {
        return; /* triggered by our own teardown, not a real failure */
    }

    logmsg("event tap mach port invalidated -> exiting so launchd relaunches");
    g_stop = 1;
    if (g_runloop != NULL) {
        CFRunLoopStop(g_runloop);
    }
}

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
    if (g_runloop != NULL) {
        CFRunLoopStop(g_runloop);
    }
}

static CFMachPortRef create_tap(CGEventTapPlacement placement,
                                CGEventTapOptions opts, CGEventMask event_mask,
                                CGEventTapCallBack callback) {
    CFMachPortRef tap = CGEventTapCreate(kCGSessionEventTap, placement, opts,
                                         event_mask, callback, NULL);

    if (tap == NULL) {
        logmsg("failed to create event tap: Accessibility permission missing or "
               "revoked");
        logmsg("grant it in System Settings -> Privacy & Security -> "
               "Accessibility");
        return NULL;
    }

    CFMachPortSetInvalidationCallBack(tap, tap_invalidated);
    return tap;
}

static void teardown(void) {
    g_tearing_down = 1;

    if (g_watchdog != NULL) {
        CFRunLoopTimerInvalidate(g_watchdog);
        CFRelease(g_watchdog);
        g_watchdog = NULL;
    }
    if (g_watch_source != NULL) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), g_watch_source,
                              kCFRunLoopCommonModes);
        CFRelease(g_watch_source);
        g_watch_source = NULL;
    }
    if (g_source != NULL) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), g_source,
                              kCFRunLoopCommonModes);
        CFRelease(g_source);
        g_source = NULL;
    }
    if (g_watch_tap != NULL) {
        CFMachPortInvalidate(g_watch_tap);
        CFRelease(g_watch_tap);
        g_watch_tap = NULL;
    }
    if (g_tap != NULL) {
        CFMachPortInvalidate(g_tap);
        CFRelease(g_tap);
        g_tap = NULL;
    }
}

int main(void) {
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGHUP, &sa, NULL);

    g_runloop = CFRunLoopGetCurrent();
    CFRetain(g_runloop);

    /*
     * Active tap: only scroll wheel events, at the head so it sees them before
     * the app does. Starts enabled (old behaviour); the watcher turns it off
     * once only the trackpad is in use.
     */
    g_tap = create_tap(kCGHeadInsertEventTap, kCGEventTapOptionDefault,
                       CGEventMaskBit(kCGEventScrollWheel), event_callback);
    if (g_tap == NULL) {
        CFRelease(g_runloop);
        return 1;
    }

    g_source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, g_tap, 0);
    if (g_source == NULL) {
        logmsg("failed to create run loop source");
        teardown();
        CFRelease(g_runloop);
        return 1;
    }
    CFRunLoopAddSource(g_runloop, g_source, kCFRunLoopCommonModes);

    /*
     * Watcher: passive, so it never adds latency. It sees mouse movement and
     * scroll wheel events and flips the active tap on/off. Optional: if it
     * cannot be created we simply keep the active tap on (old behaviour).
     */
    g_watch_tap = create_tap(kCGTailAppendEventTap, kCGEventTapOptionListenOnly,
                             CGEventMaskBit(kCGEventScrollWheel) |
                                 CGEventMaskBit(kCGEventMouseMoved) |
                                 CGEventMaskBit(kCGEventLeftMouseDragged) |
                                 CGEventMaskBit(kCGEventRightMouseDragged) |
                                 CGEventMaskBit(kCGEventOtherMouseDragged),
                             observe_callback);
    if (g_watch_tap != NULL) {
        g_watch_source =
            CFMachPortCreateRunLoopSource(kCFAllocatorDefault, g_watch_tap, 0);
        if (g_watch_source != NULL) {
            CFRunLoopAddSource(g_runloop, g_watch_source, kCFRunLoopCommonModes);
        } else {
            logmsg("failed to create watcher run loop source; staying always-on");
            CFRelease(g_watch_tap);
            g_watch_tap = NULL;
        }
    }

    g_watchdog = CFRunLoopTimerCreate(kCFAllocatorDefault,
                                      CFAbsoluteTimeGetCurrent() +
                                          WATCHDOG_INTERVAL,
                                      WATCHDOG_INTERVAL, 0, 0, watchdog_cb, NULL);
    if (g_watchdog == NULL) {
        logmsg("failed to create watchdog timer");
        teardown();
        CFRelease(g_runloop);
        return 1;
    }
    CFRunLoopAddTimer(g_runloop, g_watchdog, kCFRunLoopCommonModes);

    CGEventTapEnable(g_tap, true);
    if (g_watch_tap != NULL) {
        CGEventTapEnable(g_watch_tap, true);
    }
    logmsg("scroll interceptor started: session event tap, inverting mouse "
           "wheel only (trackpad passed through untouched; active tap idles "
           "when only the trackpad is in use)");

    CFRunLoopRun();

    if (g_stop) {
        logmsg("shutting down: wheel_inverted=%lu trackpad_passed=%lu "
               "timeouts=%lu userinput=%lu reenables=%lu",
               g_inversions, g_trackpad_skipped, g_timeout_disables,
               g_userinput_disables, g_reenables);
    }

    teardown();
    CFRelease(g_runloop);
    return 0;
}
