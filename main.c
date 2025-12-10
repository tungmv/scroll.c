#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

// Create event tap options
static CFMachPortRef create_event_tap(void);
static CGEventRef event_callback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *refcon);

// Event callback function that intercepts scroll events
static CGEventRef event_callback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *refcon) {
    (void)proxy;
    (void)refcon;
    
    if (type == kCGEventScrollWheel) {
        // Get scroll deltas
        int64_t delta_y = CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis1);
        
        // Invert vertical scroll direction
        CGEventSetIntegerValueField(event, kCGScrollWheelEventDeltaAxis1, -delta_y);
    }
    
    return event;
}

// Create and configure the event tap
static CFMachPortRef create_event_tap(void) {
    CGEventMask event_mask = (1 << kCGEventScrollWheel);
    
    CFMachPortRef event_tap = CGEventTapCreate(
        kCGSessionEventTap,
        kCGHeadInsertEventTap,
        kCGEventTapOptionDefault,
        event_mask,
        event_callback,
        NULL
    );
    
    if (!event_tap) {
        fprintf(stderr, "Failed to create event tap. Please check Accessibility permissions.\n");
        fprintf(stderr, "System Preferences → Security & Privacy → Privacy → Accessibility\n");
        return NULL;
    }
    
    return event_tap;
}

int main(void) {
    // Create the event tap
    CFMachPortRef event_tap = create_event_tap();
    if (!event_tap) {
        return 1;
    }
    
    // Create run loop source and add to current run loop
    CFRunLoopSourceRef run_loop_source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, event_tap, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), run_loop_source, kCFRunLoopCommonModes);
    
    // Enable the event tap
    CGEventTapEnable(event_tap, true);
    
    printf("Scroll interceptor started. Press Ctrl+C to quit.\n");
    printf("Vertical scroll is now inverted.\n");
    
    // Run the event loop
    CFRunLoopRun();
    
    // Cleanup (this code will only run if the run loop exits)
    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), run_loop_source, kCFRunLoopCommonModes);
    CFRelease(run_loop_source);
    CFRelease(event_tap);
    
    return 0;
}
