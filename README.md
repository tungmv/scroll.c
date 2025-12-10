# Scroll Interceptor - C Port

C port of UnnaturalScrollWheels scroll interceptor that modifies macOS scroll behavior by intercepting scroll wheel events. (https://github.com/ther0n/UnnaturalScrollWheels/blob/master/UnnaturalScrollWheels/ScrollInterceptor.swift)

## Quick Start
``` sh
clang -std=c17 -Wall -Wextra -Werror -pedantic -O3 -march=native -mtune=native -flto -ffast-math -funroll-loops -fomit-frame-pointer -fno-stack-protector -DNDEBUG -fstrict-aliasing -fno-math-errno -D_FORTIFY_SOURCE=1 -Wl,-dead_strip -Wl,-x -flto -framework ApplicationServices -framework CoreFoundation -o scroll_c main.c
```

**Requirements**: Accessibility permissions (System Preferences → Security & Privacy → Privacy → Accessibility)

## Configuration
Default: Inverts vertical scroll, keeps horizontal normal. Modify `create_event_tap()` to customize.

Automatically inverts mouse scroll direction. Use with tmux for background operation.
