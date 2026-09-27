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
# Flavours: the machines this is built for, each a set of knobs.
#
#   device   the Raspberry Pi the hardware is on, fully equipped -- the
#            three daemons and the front panel on the Automation HAT
#            Mini. The default. Note what that costs: WITH_DISPLAY
#            compiles the whole of 3rd/lvgl, which is some seven hours
#            on the Pi Zero, so a bare `make build` there is an
#            overnight job. `FLAVOUR=sensors` is the same machine
#            without it.
#   sensors  the daemons alone, no panel -- the Pi before a screen was
#            wired to it, or one where the readings are all that is
#            wanted. Everything device has except the long compile.
#   viewer   anywhere else -- moses_display alone, in an SDL window,
#            watching the same broker. No daemons, so neither the GPIO
#            and I2C hardware they drive nor M-Bus need exist on that
#            machine. mosquitto still does: the broker is what it reads.
#
# The sinks go with the machine, which is why they are here and not
# standalone defaults. Where the daemons run -- device and sensors --
# all three are on: the broker for everything off the box, the system
# bus for the panel beside it, and stdout for whatever is collecting
# lines. They are not alternatives and cost nothing to have together,
# one formatter feeding each of them. The viewer produces no readings at
# all, so it has only the broker, which is where it reads them from.
#
# Every knob below takes its default from the flavour and is still
# settable on its own, because `?=` leaves a value the caller gave
# alone: `make build FLAVOUR=viewer WITH_LOG=yes` is both.
#
FLAVOURS	 = device sensors viewer
FLAVOUR		?= device

FLAVOUR_device_PROGRAMS       = yes
FLAVOUR_device_DISPLAY        = yes
FLAVOUR_device_BACKEND        = automation-hat-mini
FLAVOUR_device_MQTT           = yes
FLAVOUR_device_LINEPROTOCOL   = yes
FLAVOUR_device_DBUS           = yes

FLAVOUR_sensors_PROGRAMS      = yes
FLAVOUR_sensors_DISPLAY       = no
FLAVOUR_sensors_BACKEND       = automation-hat-mini
FLAVOUR_sensors_MQTT          = yes
FLAVOUR_sensors_LINEPROTOCOL  = yes
FLAVOUR_sensors_DBUS          = yes

FLAVOUR_viewer_PROGRAMS       = no
FLAVOUR_viewer_DISPLAY        = yes
FLAVOUR_viewer_BACKEND        = sdl
FLAVOUR_viewer_MQTT           = yes
FLAVOUR_viewer_LINEPROTOCOL   = no
FLAVOUR_viewer_DBUS           = no

# The build knobs, as CMakeLists.txt names them. Kept as yes/no here and
# turned into CMake's ON/OFF by the indirection below, because `ifeq` is
# GNU-only and BSD make rejects the line outright.
WITH_LOG	?= no
WITH_LINEPROTOCOL ?= $(FLAVOUR_$(FLAVOUR)_LINEPROTOCOL)
WITH_TESTS	?= no
WITH_WATERMETER	?= $(FLAVOUR_$(FLAVOUR)_PROGRAMS)
WITH_BREAKER	?= $(FLAVOUR_$(FLAVOUR)_PROGRAMS)
WITH_TEMPERATURE ?= $(FLAVOUR_$(FLAVOUR)_PROGRAMS)
WITH_MQTT	?= $(FLAVOUR_$(FLAVOUR)_MQTT)
WITH_DBUS	?= $(FLAVOUR_$(FLAVOUR)_DBUS)
WITH_DISPLAY	?= $(FLAVOUR_$(FLAVOUR)_DISPLAY)
WITH_DISPLAY_TESTS ?= no
WITH_WERROR	?= no
WITH_ANALYZER	?= no

DISPLAY_BACKEND	?= $(FLAVOUR_$(FLAVOUR)_BACKEND)
MQTT_TOPIC_PREFIX ?= water-breaker

# Empty lets bitters find the Pi's GPIO controller by asking each chip
# for its label, which is what makes one binary work on a Zero and a Pi
# 5 alike. Naming one -- pinctrl-bcm2835 for a Zero, pinctrl-bcm2711 for
# a Pi 4, pinctrl-rp1 for a Pi 5 -- skips that search.
RPI_GPIO_CHIP	?=

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
CMAKEFLAGS	+= -DWITH_DBUS=$(ON_$(WITH_DBUS))
CMAKEFLAGS	+= -DMQTT_TOPIC_PREFIX=$(MQTT_TOPIC_PREFIX)
CMAKEFLAGS	+= -DWITH_WATERMETER=$(ON_$(WITH_WATERMETER))
CMAKEFLAGS	+= -DWITH_BREAKER=$(ON_$(WITH_BREAKER))
CMAKEFLAGS	+= -DWITH_TEMPERATURE=$(ON_$(WITH_TEMPERATURE))
CMAKEFLAGS	+= -DWITH_DISPLAY=$(ON_$(WITH_DISPLAY))
CMAKEFLAGS	+= -DWITH_DISPLAY_TESTS=$(ON_$(WITH_DISPLAY_TESTS))
CMAKEFLAGS	+= -DWITH_TESTS=$(ON_$(WITH_TESTS))
CMAKEFLAGS	+= -DDISPLAY_BACKEND=$(DISPLAY_BACKEND)
CMAKEFLAGS	+= -DRPI_GPIO_CHIP=$(RPI_GPIO_CHIP)
CMAKEFLAGS	+= -DWITH_LOG=$(ON_$(WITH_LOG))
CMAKEFLAGS	+= -DWITH_WERROR=$(ON_$(WITH_WERROR))
CMAKEFLAGS	+= -DWITH_ANALYZER=$(ON_$(WITH_ANALYZER))

HELPERS		= loop-runner nut-notify check-spi

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

.PHONY: help all check check-flavour check-submodules check-shell \
	check-spi build \
	tests tests-display tests-refs tests-nohw \
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
	@printf '  %-20s %s\n'						      \
	    FLAVOUR		'$(FLAVOUR)  (one of: $(FLAVOURS); see `make flavours`)' \
	    BUILD		'$(BUILD)  (build directory)'		      \
	    CMAKE		'$(CMAKE)'				      \
	    JOBS		'$(JOBS)  (parallel build jobs; cores by default)' \
	    PREFIX		'$(PREFIX)  (install goes to $(BINDIR))'      \
	    DESTDIR		'$(DESTDIR)  (staging prefix for packaging)'  \
	    DISPLAY_BACKEND	'$(DISPLAY_BACKEND)  (one of: automation-hat-mini sdl)' \
	    MQTT_TOPIC_PREFIX	'$(MQTT_TOPIC_PREFIX)  (compiled-in default topic prefix)' \
	    RPI_GPIO_CHIP	'$(RPI_GPIO_CHIP)  (empty: bitters finds it by label)'
	@echo ''
	@echo 'Where a reading goes (yes/no, and none of them exclusive):'
	@printf '  %-20s %-4s %s\n'					      \
	    WITH_MQTT		'$(WITH_MQTT)'				      \
		'(the broker, and the only way moses_breaker is commanded)'   \
	    WITH_LINEPROTOCOL	'$(WITH_LINEPROTOCOL)'			      \
		'(stdout, as InfluxDB line protocol)'			      \
	    WITH_DBUS		'$(WITH_DBUS)'				      \
		'(the same line on the system bus; needs libdbus)'
	@echo ''
	@echo 'What gets built (yes/no; see `make options`):'
	@printf '  %-20s %-4s %s\n'					      \
	    WITH_WATERMETER	'$(WITH_WATERMETER)'			      \
		'(moses_watermeter; needs M-Bus)'			      \
	    WITH_BREAKER	'$(WITH_BREAKER)'			      \
		'(moses_breaker; uncommandable without WITH_MQTT)'	      \
	    WITH_TEMPERATURE	'$(WITH_TEMPERATURE)'			      \
		'(moses_sensors, the BME280)'				      \
	    WITH_DISPLAY	'$(WITH_DISPLAY)'			      \
		'(moses_display; pulls in LVGL, a long compile)'	      \
	    WITH_DISPLAY_TESTS	'$(WITH_DISPLAY_TESTS)'			      \
		'(the screenshot tests; needs LVGL, not a panel)'	      \
	    WITH_TESTS		'$(WITH_TESTS)'				      \
		'(build the unit tests; `make tests` sets it)'
	@echo ''
	@echo 'Diagnostics:'
	@printf '  %-20s %-4s %s\n'					      \
	    WITH_LOG		'$(WITH_LOG)'	 '(log messages on stderr)'   \
	    WITH_WERROR		'$(WITH_WERROR)' '(warnings are errors, for CI)' \
	    WITH_ANALYZER	'$(WITH_ANALYZER)' '(run the GCC static analyzer)'
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

# Not a prerequisite of `check`, and that is the point: check asks
# whether this tree is fit to build and has to answer on a workstation,
# while this asks whether the machine in front of you is wired and booted
# for the panel. Run it on the Pi, where it is the difference between a
# display that comes up and one that stops at "cannot claim the LCD
# data/command pin". It skips where there is no SPI0 to look at, so it is
# harmless to run anywhere.
check-spi:					## the Pi's SPI leaves the panel its pins; check does not run it
	@sh scripts/check-spi


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
tests: WITH_DISPLAY = no
tests: check-flavour check-submodules				## build and run the unit tests, and report
	$(CMAKE) -B $(BUILD) $(CMAKEFLAGS)
	$(CMAKE) --build $(BUILD) --parallel $(JOBS)
	$(CTEST) --test-dir $(BUILD) --output-on-failure

tests-display: WITH_DISPLAY_TESTS = yes
tests-display: WITH_DISPLAY = no
tests-display: WITH_TESTS = no
tests-display: WITH_WATERMETER = no
tests-display: WITH_BREAKER = no
tests-display: WITH_TEMPERATURE = no
tests-display: check-flavour check-submodules			## render the display's screen and compare with test/ref-imgs
	$(CMAKE) -B $(BUILD) $(CMAKEFLAGS)
	$(CMAKE) --build $(BUILD) --parallel $(JOBS)
	$(CTEST) --test-dir $(BUILD) --output-on-failure -R dashboard

#
# Writing the references is deliberate and separate, because a reference
# nobody looked at proves nothing: LVGL can fill a missing one in from
# whatever was just rendered and pass, which is why that is switched off
# (src/lv_conf.h, LV_TEST_SCREENSHOT_CREATE_REFERENCE_IMAGE) and why the
# writing is the test's own -w. The images land in the source tree, so
# `git diff` is what says whether the change was the intended one.
#
tests-refs: WITH_DISPLAY_TESTS = yes
tests-refs: WITH_DISPLAY = no
tests-refs: WITH_TESTS = no
tests-refs: WITH_WATERMETER = no
tests-refs: WITH_BREAKER = no
tests-refs: WITH_TEMPERATURE = no
tests-refs: check-flavour check-submodules		## rewrite test/ref-imgs/ from what the screen renders now
	$(CMAKE) -B $(BUILD) $(CMAKEFLAGS)
	$(CMAKE) --build $(BUILD) --parallel $(JOBS)
	./bin/test_dashboard -w
	@echo ''
	@echo 'Review them before committing: `git diff --stat test/ref-imgs`'
	@echo 'for what moved, the images themselves for whether it is right,'
	@echo 'then `make tests-display` to confirm they compare equal.'

tests-nohw: WITH_TESTS = yes
tests-nohw: WITH_DISPLAY = no
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
	@echo 'FLAVOUR picks a machine by giving these knobs their defaults.'
	@echo 'Each one is still settable on its own.'
	@echo ''
	@printf '  %-18s %-21s %-21s %s\n'				\
	    'knob'		'device'	'sensors'    'viewer'	\
	    'WITH_MQTT'		'$(FLAVOUR_device_MQTT)'		\
				'$(FLAVOUR_sensors_MQTT)'		\
				'$(FLAVOUR_viewer_MQTT)'		\
	    'WITH_LINEPROTOCOL'	'$(FLAVOUR_device_LINEPROTOCOL)'	\
				'$(FLAVOUR_sensors_LINEPROTOCOL)'	\
				'$(FLAVOUR_viewer_LINEPROTOCOL)'	\
	    'WITH_DBUS'		'$(FLAVOUR_device_DBUS)'		\
				'$(FLAVOUR_sensors_DBUS)'		\
				'$(FLAVOUR_viewer_DBUS)'		\
	    'WITH_WATERMETER'	'$(FLAVOUR_device_PROGRAMS)'		\
				'$(FLAVOUR_sensors_PROGRAMS)'		\
				'$(FLAVOUR_viewer_PROGRAMS)'		\
	    'WITH_BREAKER'	'$(FLAVOUR_device_PROGRAMS)'		\
				'$(FLAVOUR_sensors_PROGRAMS)'		\
				'$(FLAVOUR_viewer_PROGRAMS)'		\
	    'WITH_TEMPERATURE'	'$(FLAVOUR_device_PROGRAMS)'		\
				'$(FLAVOUR_sensors_PROGRAMS)'		\
				'$(FLAVOUR_viewer_PROGRAMS)'		\
	    'WITH_DISPLAY'	'$(FLAVOUR_device_DISPLAY)'		\
				'$(FLAVOUR_sensors_DISPLAY)'		\
				'$(FLAVOUR_viewer_DISPLAY)'		\
	    'DISPLAY_BACKEND'	'$(FLAVOUR_device_BACKEND)'		\
				'$(FLAVOUR_sensors_BACKEND)'		\
				'$(FLAVOUR_viewer_BACKEND)'
	@echo ''
	@echo 'device:  the Pi the hardware is on, daemons and panel both.'
	@echo 'sensors: the same machine without the panel.'
	@echo 'viewer:  anywhere else -- moses_display alone in a window.'


options:					## every build knob, and what it defaults to
	@echo 'Build options, as CMakeLists.txt defines them.'
	@echo 'Set any of them on the command line: make tests WITH_WERROR=yes'
	@echo ''
	@echo 'Where a reading goes -- three sinks, none of them exclusive:'
	@printf '  %-20s %-4s %s\n'					\
	    'WITH_MQTT'		 'yes'	'publish to the broker'		\
	    'WITH_LINEPROTOCOL'	 'no'	'one line per reading on stdout' \
	    'WITH_DBUS'		 'no'	'the same line on the system bus (needs libdbus)'
	@echo ''
	@echo 'Where the broker one points:'
	@printf '  %-20s %-20s %s\n'					\
	    'MQTT_TOPIC_PREFIX'	 'water-breaker'	'compiled-in topic prefix'
	@echo ''
	@echo 'What gets built:'
	@printf '  %-20s %-4s %s\n'					\
	    'WITH_WATERMETER'	 'yes'	'moses_watermeter; the only one wanting M-Bus' \
	    'WITH_BREAKER'	 'yes'	'moses_breaker; MQTT is how it is commanded' \
	    'WITH_TEMPERATURE'	 'yes'	'moses_sensors, the BME280 reader' \
	    'WITH_TESTS'	 'no'	'build the unit tests'
	@echo ''
	@echo 'The front panel:'
	@printf '  %-20s %-20s %s\n'					\
	    'WITH_DISPLAY'	 'no'	'moses_display, the LVGL front panel' \
	    'WITH_DISPLAY_TESTS' 'no'	'the screenshot tests'		\
	    'DISPLAY_BACKEND'	 'automation-hat-mini' 'or sdl, for a window' \
	    'RPI_GPIO_CHIP'	 '(empty)'	'pin the GPIO controller, e.g. pinctrl-bcm2835'
	@echo ''
	@echo 'Diagnostics:'
	@printf '  %-20s %-4s %s\n'					\
	    'WITH_LOG'		 'no'	'log messages on stderr'	\
	    'WITH_WERROR'	 'no'	'treat warnings as errors'	\
	    'WITH_ANALYZER'	 'no'	'run the GCC static analyzer'

features:					## what this invocation selected, as shell variables
	@echo '# FLAVOUR=$(FLAVOUR) WITH_LOG=$(WITH_LOG) WITH_LINEPROTOCOL=$(WITH_LINEPROTOCOL) WITH_TESTS=$(WITH_TESTS) WITH_WATERMETER=$(WITH_WATERMETER) WITH_BREAKER=$(WITH_BREAKER) WITH_TEMPERATURE=$(WITH_TEMPERATURE) WITH_MQTT=$(WITH_MQTT) WITH_DBUS=$(WITH_DBUS) WITH_DISPLAY=$(WITH_DISPLAY) DISPLAY_BACKEND=$(DISPLAY_BACKEND)'
	@echo "MOSES_CMAKE_FLAGS='$(CMAKEFLAGS)'"
	@echo "MOSES_BUILD='$(BUILD)'"


clean:						## remove the build directory and bin/
	rm -rf $(BUILD) bin

distclean: clean				## clean, plus the other build directories
	rm -rf build-analyze build-nohw build-display


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
