# Optimized compilation
CFLAGS = -O3 -flto -march=native -DNDEBUG
LDFLAGS = -framework ApplicationServices -framework CoreFoundation
PLIST = $(HOME)/Library/LaunchAgents/com.scroll.interceptor.plist

scroll: main.c
	clang $(CFLAGS) -o scroll main.c $(LDFLAGS)

# For debugging
debug: CFLAGS = -O0 -g -Wall -Wextra
debug: scroll

check:
	clang -std=c17 -O2 -Wall -Wextra -Werror -pedantic -o /dev/null main.c $(LDFLAGS)

# Unit test for the wheel-vs-trackpad decision (see test_filter.c)
test: test_filter.c main.c
	clang -std=c17 -O2 -Wall -Wextra -Werror -pedantic -o /tmp/scroll_test_filter test_filter.c $(LDFLAGS)
	/tmp/scroll_test_filter

# Install, reload the agent, and report the new cdhash.
#
# IMPORTANT: rebuilding changes the code signature hash, which invalidates the
# Accessibility / PostEvent grants that macOS pinned to the OLD hash. The tap
# then fails at startup and launchd restarts it every ThrottleInterval seconds,
# which looks exactly like a crash loop. After this target either:
#   - toggle the entry in System Settings > Privacy & Security > Accessibility, or
#   - rewrite the csreq column in TCC.db as documented in README.md
deploy: scroll
	install -m 0755 scroll $(HOME)/.local/bin/scroll
	launchctl kickstart -k gui/$$(id -u)/com.scroll.interceptor
	@echo
	@echo "deployed. new cdhash:"
	@codesign -dvvv $(HOME)/.local/bin/scroll 2>&1 | grep '^CDHash='
	@echo "if the log shows 'failed to create event tap', re-grant Accessibility."

log:
	@tail -f $(HOME)/Library/Logs/scroll.log

.PHONY: scroll debug check test deploy log
