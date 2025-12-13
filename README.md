# Scroll Interceptor - C Port

C port of UnnaturalScrollWheels scroll interceptor that modifies macOS scroll behavior by intercepting scroll wheel events. (https://github.com/ther0n/UnnaturalScrollWheels/blob/master/UnnaturalScrollWheels/ScrollInterceptor.swift)

## Quick Start
``` sh
clang -std=c17 -O3 -flto -march=native -DNDEBUG -framework ApplicationServices -framework CoreFoundation -o scroll_c main.c
```

**Requirements**: Accessibility permissions (System Preferences → Security & Privacy → Privacy → Accessibility)

## Configuration
Default: Inverts vertical scroll, keeps horizontal normal. Modify `create_options()` to customize.

Automatically inverts mouse scroll direction. Use with tmux for background operation.
