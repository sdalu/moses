#
# moses -- the interface to the build.
#
# The build itself is CMake (CMakeLists.txt is the authority on what the
# options mean); this is the front door: `make help` says what can be
# typed here and what every knob is currently set to, so that driving
# the tree does not depend on remembering a cmake command line.
#
# Runs under GNU make and BSD make alike -- the daemons are built on a
# Raspberry Pi and the display's tests on whatever machine is to hand.
#

BUILD		?= build
CMAKE		?= cmake
CTEST		?= ctest
INSTALL		?= install

# As many jobs as the machine has cores. `!=` is the portable shell
# assignment -- $(shell ...) is GNU-only and BSD make would hand back an
# empty string in silence. On the single-core Pi this resolves to 1,
# which is the answer wanted there anyway.
JOBS		!= (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)

PREFIX		?= /usr/local
BINDIR		?= $(PREFIX)/bin
DESTDIR		?=

#
# Flavours: the two machines this is built for, each a set of knobs.
#
#   device  the Raspberry Pi the hardware is on -- the daemons, and the
#           display wired for the Automation HAT Mini's panel should it
#           be asked for. These are the plain defaults, so a bare
#           `make build` is this and nothing changes for typing nothing.
#   viewer  anywhere else -- moses_display alone, in an SDL window,
#           watching the same broker. No daemons, so neither mosquitto's
#           hardware nor M-Bus need exist on that machine.
#
# Every knob below takes its default from the flavour and is still
# settable on its own, because `?=` leaves a value the caller gave
# alone: `make build FLAVOUR=viewer WITH_LOG=yes` is both.
#
FLAVOURS	 = device viewer
FLAVOUR		?= device

FLAVOUR_device_PROGRAMS	= yes
FLAVOUR_device_DISPLAY	= no
FLAVOUR_device_BACKEND	= automation-hat-mini
FLAVOUR_viewer_PROGRAMS	= no
FLAVOUR_viewer_DISPLAY	= yes
FLAVOUR_viewer_BACKEND	= sdl

# The build knobs, as CMakeLists.txt names them. Kept as yes/no here and
# turned into CMake's ON/OFF by the indirection below, because `ifeq` is
# GNU-only and BSD make rejects the line outright.
WITH_LOG	?= no
WITH_LINEPROTOCOL ?= no
WITH_TESTS	?= no
WITH_WATERMETER	?= $(FLAVOUR_$(FLAVOUR)_PROGRAMS)
WITH_BREAKER	?= $(FLAVOUR_$(FLAVOUR)_PROGRAMS)
WITH_TEMPERATURE ?= $(FLAVOUR_$(FLAVOUR)_PROGRAMS)
WITH_MQTT	?= yes
WITH_DGRAM	?= no
WITH_DISPLAY	?= $(FLAVOUR_$(FLAVOUR)_DISPLAY)
WITH_DISPLAY_TESTS ?= no
WITH_WERROR	?= no
WITH_ANALYZER	?= no

DISPLAY_BACKEND	?= $(FLAVOUR_$(FLAVOUR)_BACKEND)
MQTT_TOPIC_PREFIX ?= water-breaker
MOSES_DGRAM_PATH ?= /run/moses.sock

ON_yes		= ON
ON_no		= OFF

# Appended a line at a time rather than continued with backslashes: the
# two makes disagree about what the whitespace around a continuation
# becomes, and BSD make leaves the tabs in. That is invisible where the
# value is only passed to cmake, and visible the moment `features`
# prints it for someone to eval.
# Grouped as the help groups them: where a reading goes, what gets
# built, diagnostics.
CMAKEFLAGS	 = -DWITH_MQTT=$(ON_$(WITH_MQTT))
CMAKEFLAGS	+= -DWITH_LINEPROTOCOL=$(ON_$(WITH_LINEPROTOCOL))
CMAKEFLAGS	+= -DWITH_DGRAM=$(ON_$(WITH_DGRAM))
CMAKEFLAGS	+= -DMQTT_TOPIC_PREFIX=$(MQTT_TOPIC_PREFIX)
CMAKEFLAGS	+= -DMOSES_DGRAM_PATH=$(MOSES_DGRAM_PATH)
CMAKEFLAGS	+= -DWITH_WATERMETER=$(ON_$(WITH_WATERMETER))
CMAKEFLAGS	+= -DWITH_BREAKER=$(ON_$(WITH_BREAKER))
CMAKEFLAGS	+= -DWITH_TEMPERATURE=$(ON_$(WITH_TEMPERATURE))
CMAKEFLAGS	+= -DWITH_DISPLAY=$(ON_$(WITH_DISPLAY))
CMAKEFLAGS	+= -DWITH_DISPLAY_TESTS=$(ON_$(WITH_DISPLAY_TESTS))
CMAKEFLAGS	+= -DWITH_TESTS=$(ON_$(WITH_TESTS))
CMAKEFLAGS	+= -DDISPLAY_BACKEND=$(DISPLAY_BACKEND)
CMAKEFLAGS	+= -DWITH_LOG=$(ON_$(WITH_LOG))
CMAKEFLAGS	+= -DWITH_WERROR=$(ON_$(WITH_WERROR))
CMAKEFLAGS	+= -DWITH_ANALYZER=$(ON_$(WITH_ANALYZER))

HELPERS		= loop-runner nut-notify

# What `install` puts down is what this configuration actually built,
# not whatever happens to be lying in bin/ from an earlier run. Keyed by
# the same indirection as everything else.
METER_yes	= bin/moses_watermeter
METER_no	=
VALVE_yes	= bin/moses_breaker
VALVE_no	=
TEMP_yes	= bin/moses_sensors
TEMP_no		=
SCREEN_yes	= bin/moses_display
SCREEN_no	=
# loop-runner supervises a daemon and nut-notify feeds the UPS state, so
# they go down with the first daemon that is built and not otherwise.
HELP_yes	= scripts/loop-runner scripts/nut-notify
HELP_no		=
ANY		= $(WITH_WATERMETER)$(WITH_BREAKER)$(WITH_TEMPERATURE)
ANY_yesyesyes	= yes
ANY_yesyesno	= yes
ANY_yesnoyes	= yes
ANY_yesnono	= yes
ANY_noyesyes	= yes
ANY_noyesno	= yes
ANY_nonoyes	= yes
ANY_nonono	= no

INSTALL_FILES	= $(METER_$(WITH_WATERMETER)) $(VALVE_$(WITH_BREAKER))	\
		  $(TEMP_$(WITH_TEMPERATURE)) $(SCREEN_$(WITH_DISPLAY))	\
		  $(HELP_$(ANY_$(ANY)))

# Everything install could ever have put down, for uninstall to take
# back. rm -f, so removing a configuration's worth that was never
# installed is not an error.
INSTALLED	= moses_watermeter moses_breaker moses_sensors moses_display
INSTALLED	+= loop-runner nut-notify

.PHONY: help all check check-flavour check-submodules check-shell build \
	tests tests-display tests-nohw \
	flavours options features clean distclean install uninstall


help:						## show this help (the default)
	@echo 'moses -- a do-it-yourself water-leak breaker'
	@echo ''
	@echo 'Targets:'
	@awk -F':.*## ' '/^[a-z][a-z0-9-]*:.*## /{ \
	    pre = sprintf("  %-16s ", $$1); n = split($$2, w, / /); line = ""; \
	    for (i = 1; i <= n; i++) { \
	        cand = (line == "" ? w[i] : line " " w[i]); \
	        if (length(pre) + length(cand) > 80 && line != "") { \
	            print pre line; pre = "                   "; line = w[i]; \
	        } else line = cand; \
	    } \
	    if (line != "") print pre line; \
	}' Makefile
	@echo ''
	@echo 'Variables (current value):'
	@printf '  %-16s %s\n'						      \
	    FLAVOUR		'$(FLAVOUR)  (one of: $(FLAVOURS); see `make flavours`)' \
	    BUILD		'$(BUILD)  (build directory)'		      \
	    CMAKE		'$(CMAKE)'				      \
	    JOBS		'$(JOBS)  (parallel build jobs; cores by default)' \
	    PREFIX		'$(PREFIX)  (install goes to $(BINDIR))'      \
	    DESTDIR		'$(DESTDIR)  (staging prefix for packaging)'  \
	    DISPLAY_BACKEND	'$(DISPLAY_BACKEND)  (one of: automation-hat-mini sdl)' \
	    MQTT_TOPIC_PREFIX	'$(MQTT_TOPIC_PREFIX)  (compiled-in default topic prefix)'
	@echo ''
	@echo 'Where a reading goes (yes/no, and none of them exclusive):'
	@printf '  %-18s %s\n'						      \
	    WITH_MQTT		'$(WITH_MQTT)  (the broker; no also drops moses_breaker)' \
	    WITH_LINEPROTOCOL	'$(WITH_LINEPROTOCOL)  (stdout, as InfluxDB line protocol)' \
	    WITH_DGRAM		'$(WITH_DGRAM)  (the same line to $(MOSES_DGRAM_PATH))'
	@echo ''
	@echo 'What gets built (yes/no; see `make options`):'
	@printf '  %-18s %s\n'						      \
	    WITH_WATERMETER	'$(WITH_WATERMETER)  (moses_watermeter; needs M-Bus)' \
	    WITH_BREAKER	'$(WITH_BREAKER)  (moses_breaker; needs WITH_MQTT)' \
	    WITH_TEMPERATURE	'$(WITH_TEMPERATURE)  (moses_sensors, the BME280)' \
	    WITH_DISPLAY	'$(WITH_DISPLAY)  (moses_display; pulls in LVGL, a long compile)' \
	    WITH_DISPLAY_TESTS	'$(WITH_DISPLAY_TESTS)  (the screenshot tests; needs LVGL, not a panel)' \
	    WITH_TESTS		'$(WITH_TESTS)  (build the unit tests; `make tests` sets it)'
	@echo ''
	@echo 'Diagnostics:'
	@printf '  %-18s %s\n'						      \
	    WITH_LOG		'$(WITH_LOG)  (log messages on stderr)'	      \
	    WITH_WERROR		'$(WITH_WERROR)  (warnings are errors, for CI)' \
	    WITH_ANALYZER	'$(WITH_ANALYZER)  (run the GCC static analyzer)'
	@echo ''
	@echo 'There is no version or tag target: moses carries no release'
	@echo 'number. There is no deploy target either -- the machine this'
	@echo 'runs on holds the valve open, and pushing to it is a decision'
	@echo 'to take deliberately, not a target to type. See CLAUDE.local.md.'


all: check build				## preflight, then build what this configuration selects


#
# Preflight. Runs none of moses's own code, needs nothing built, and is
# the first thing to run when something looks wrong.
#
check: check-flavour check-submodules check-shell	## preflight: this tree is fit to build

check-flavour:					## FLAVOUR names a set of knobs that exists
	@case ' $(FLAVOURS) ' in						\
	    *' $(FLAVOUR) '*)	;;					\
	    *) echo 'make: unknown FLAVOUR=$(FLAVOUR); one of: $(FLAVOURS)' >&2; \
	       exit 1 ;;							\
	esac

# Guarded twice, and both arms were earned rather than imagined. The
# gate runs from an rsync'd copy of this tree with .git left behind
# (CLAUDE.local.md), where git lists no submodules at all -- so without
# the second arm this check passes by having nothing to look at, in
# exactly the place it is most relied on. And `git rev-parse
# --is-inside-work-tree` will not do as the test: on the deploy host,
# / is itself a git repository, so every directory on the machine
# answers yes to that.
check-submodules:				## every submodule is present and at its committed commit
	@st=$$(git submodule status 2>/dev/null);			\
	if [ ! -f .gitmodules ]; then					\
	    :;								\
	elif [ -z "$$st" ]; then					\
	    echo 'make: submodules not verifiable here (not this tree'"'"'s checkout)' >&2; \
	elif echo "$$st" | grep -q '^[+-]'; then				\
	    echo 'make: submodules are not at their committed commits:' >&2; \
	    echo "$$st" | grep '^[+-]' >&2;				\
	    echo 'run: git submodule update --init --recursive' >&2;	\
	    exit 1;							\
	fi

check-shell:					## the helper scripts pass shellcheck
	@if command -v shellcheck > /dev/null 2>&1; then		\
	    cd scripts && shellcheck $(HELPERS);			\
	else								\
	    echo 'make: shellcheck is not installed, skipped' >&2;	\
	fi


#
# CMake is re-run on every build rather than only when the cache is
# missing. It is a few seconds, and it is what makes a changed variable
# on the command line take effect: a build against a cache configured
# with different options is the failure that looks like a code error and
# is not.
#
build: check-flavour check-submodules				## build what this configuration selects
	$(CMAKE) -B $(BUILD) $(CMAKEFLAGS)
	$(CMAKE) --build $(BUILD) --parallel $(JOBS)


#
# The suite. Separate from the build because "does it compile" and "does
# it work" are two questions, and a single answer would not say which
# half was red.
#
tests: WITH_TESTS = yes
tests: check-flavour check-submodules				## build and run the unit tests, and report
	$(CMAKE) -B $(BUILD) $(CMAKEFLAGS)
	$(CMAKE) --build $(BUILD) --parallel $(JOBS)
	$(CTEST) --test-dir $(BUILD) --output-on-failure

tests-display: WITH_DISPLAY_TESTS = yes
tests-display: WITH_TESTS = no
tests-display: WITH_WATERMETER = no
tests-display: WITH_BREAKER = no
tests-display: WITH_TEMPERATURE = no
tests-display: check-flavour check-submodules			## render the display's screen and compare with test/ref-imgs
	$(CMAKE) -B $(BUILD) $(CMAKEFLAGS)
	$(CMAKE) --build $(BUILD) --parallel $(JOBS)
	$(CTEST) --test-dir $(BUILD) --output-on-failure -R dashboard

tests-nohw: WITH_TESTS = yes
tests-nohw: WITH_DISPLAY_TESTS = yes
tests-nohw: WITH_WATERMETER = no
tests-nohw: WITH_BREAKER = no
tests-nohw: WITH_TEMPERATURE = no
tests-nohw: check-flavour check-submodules			## the tests that need neither mosquitto, M-Bus nor Linux
	$(CMAKE) -B $(BUILD) $(CMAKEFLAGS)
	$(CMAKE) --build $(BUILD) --parallel $(JOBS)
	$(CTEST) --test-dir $(BUILD) --output-on-failure


#
# What can be set, and what this invocation made of it. `options` is for
# reading; `features` is meant to be consumed:
#
#     eval "$$(make -s features)"
#     echo "$$MOSES_CMAKE_FLAGS"
#
flavours:					## the flavours, and the knobs each one stands for
	@echo 'FLAVOUR picks a machine. Any knob can still be set on its own.'
	@echo ''
	@printf '  %-8s %-14s %-14s %s\n'				\
	    ''       'the daemons' 'WITH_DISPLAY' 'DISPLAY_BACKEND'	\
	    'device' '$(FLAVOUR_device_PROGRAMS)' '$(FLAVOUR_device_DISPLAY)' '$(FLAVOUR_device_BACKEND)' \
	    'viewer' '$(FLAVOUR_viewer_PROGRAMS)' '$(FLAVOUR_viewer_DISPLAY)' '$(FLAVOUR_viewer_BACKEND)'
	@echo ''
	@echo 'device: the Raspberry Pi the hardware is on. viewer: anywhere'
	@echo 'else, moses_display alone in a window, watching the same broker.'


options:					## every build knob, and what it defaults to
	@echo 'Build options, as CMakeLists.txt defines them.'
	@echo 'Set any of them on the command line: make tests WITH_WERROR=yes'
	@echo ''
	@echo 'Where a reading goes -- three sinks, none of them exclusive:'
	@printf '  %-20s %-21s %s\n'					\
	    'WITH_MQTT'		 'yes'	'publish to the broker'		\
	    'WITH_LINEPROTOCOL'	 'no'	'one line per reading on stdout' \
	    'WITH_DGRAM'	 'no'	'the same line to a unix datagram socket' \
	    'MQTT_TOPIC_PREFIX'	 'water-breaker'	'compiled-in topic prefix' \
	    'MOSES_DGRAM_PATH'	 '/run/moses.sock' 'the socket it goes to'
	@echo ''
	@echo 'What gets built:'
	@printf '  %-20s %-21s %s\n'					\
	    'WITH_WATERMETER'	 'yes'	'moses_watermeter; the only one wanting M-Bus' \
	    'WITH_BREAKER'	 'yes'	'moses_breaker; needs WITH_MQTT to be commanded' \
	    'WITH_TEMPERATURE'	 'yes'	'moses_sensors, the BME280 reader' \
	    'WITH_DISPLAY'	 'no'	'moses_display, the LVGL front panel' \
	    'WITH_DISPLAY_TESTS' 'no'	'the screenshot tests'		\
	    'WITH_TESTS'	 'no'	'build the unit tests'		\
	    'DISPLAY_BACKEND'	 'automation-hat-mini' 'or sdl, for a window'
	@echo ''
	@echo 'Diagnostics:'
	@printf '  %-20s %-21s %s\n'					\
	    'WITH_LOG'		 'no'	'log messages on stderr'	\
	    'WITH_WERROR'	 'no'	'treat warnings as errors'	\
	    'WITH_ANALYZER'	 'no'	'run the GCC static analyzer'

features:					## what this invocation selected, as shell variables
	@echo '# FLAVOUR=$(FLAVOUR) WITH_LOG=$(WITH_LOG) WITH_LINEPROTOCOL=$(WITH_LINEPROTOCOL) WITH_TESTS=$(WITH_TESTS) WITH_WATERMETER=$(WITH_WATERMETER) WITH_BREAKER=$(WITH_BREAKER) WITH_TEMPERATURE=$(WITH_TEMPERATURE) WITH_MQTT=$(WITH_MQTT) WITH_DGRAM=$(WITH_DGRAM) WITH_DISPLAY=$(WITH_DISPLAY) DISPLAY_BACKEND=$(DISPLAY_BACKEND)'
	@echo "MOSES_CMAKE_FLAGS='$(CMAKEFLAGS)'"
	@echo "MOSES_BUILD='$(BUILD)'"


clean:						## remove the build directory and bin/
	rm -rf $(BUILD) bin

distclean: clean				## clean, plus the other build directories
	rm -rf build-analyze


#
# Local only. The machine moses runs on is a live water breaker: see
# CLAUDE.local.md before installing anything there.
#
install: build					## install what this configuration built, under PREFIX
	@if [ -z '$(INSTALL_FILES)' ]; then				\
	    echo 'make: this configuration builds nothing to install' >&2;	\
	    exit 1;							\
	fi
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 755 $(INSTALL_FILES) $(DESTDIR)$(BINDIR)

uninstall:					## remove what install put down
	cd $(DESTDIR)$(BINDIR) && rm -f $(INSTALLED)
