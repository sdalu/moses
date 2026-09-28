# Closing a round on moses

What has to be true before a unit of work here is done. Every line is a
command or a question with a yes or a no at the end of it.

moses is built for **Linux** and worked on somewhere else. `make tests`
wants `linux/gpio.h`; `make tests-nohw` does not, and is seven of the
eight tests. A round done off the Pi is not closed until
`make tests` has run on a Linux host — CLAUDE.local.md names one.


## Gates

Run in order. A red gate is answered before anything else, and the answer
is sometimes that the checker is wrong rather than the file.

- [ ] `make check` -- preflight: the flavour names knobs that exist, the
      submodules are at their committed commits, the helper scripts pass
      shellcheck. Runs none of the project's code, so it runs anywhere
      and it runs first.
- [ ] `make tests-nohw` -- the seven that need neither Linux, M-Bus nor
      mosquitto. This is the gate on a development machine.
- [ ] `make tests` -- all eight, on a Linux host. The only
      thing that compiles the three daemons at all, and the only thing
      that runs `parsers`.
- [ ] `make tests-display` -- **if this round touched the screen**:
      `src/display/dashboard.c`, `model.c`, a font, `src/lv_conf.h`, or
      anything the panel lays out.
- [ ] `make tests-refs` then **look at `git diff test/ref-imgs`** -- only
      if the screen was *meant* to change. A reference nobody looked at
      proves nothing.
- [ ] `make build FLAVOUR=viewer` -- **if this round touched
      `src/display/` outside `dashboard.c` and `model.c`**: a source,
      `display.c`, `backend/sdl.c`. `make tests` builds none of that and
      `tests-display` links none of it, so a file left out of a CMake
      target compiles clean everywhere above and fails only here.
- [ ] **A backend under `src/display/backend/` is not covered by the
      line above.** `viewer` compiles `sdl.c` and `device` compiles
      `rpi-automation-hat-mini.c`, so only `FLAVOUR=device` on a Linux
      host builds the hat backend -- an overnight LVGL build. Short of
      that, compile the one translation unit with the flags a real build
      used, which is a genuine compile and not a syntax check:

          flags=$(sed -n -e 's/^C_DEFINES = //p' -e 's/^C_INCLUDES = //p' \
                         -e 's/^C_FLAGS = //p' \
                         build/CMakeFiles/moses_display.dir/flags.make |
                  tr '\n' ' ')
          eval "cc $flags -Werror -c src/display/backend/<file>.c -o /tmp/check.o"

      One `-e` per variable, because BSD sed has no `\|`; `eval`,
      because `flags.make` escapes its quotes (`\"water-breaker\"`).

      Compile the unmodified file the same way first; a warning the
      baseline already had is not this round's.
- [ ] `make check-spi` **on the Pi** -- **if this round touched the
      panel's pins, `backend/rpi-automation-hat-mini.c`, or what the boot
      configuration has to provide.** It reads the pin out of the backend
      and the buffer size out of docs/hardware.md, so it fails when the
      code and the machine disagree. `check` does not run it: that one has
      to answer on a workstation.
- [ ] `make tests WITH_WERROR=yes` -- CI builds this way, and the Pi's
      GCC is stricter than a clang workstation. A new warning is a
      failure here, not a note.
- [ ] **If this round touched a daemon**, CI's static-analysis build on
      a Linux host with GCC -- the same flags as its step in
      `.github/workflows/build.yml`:

          cmake -B build-analyze -DWITH_LOG=1 -DWITH_LINEPROTOCOL=1 \
                -DWITH_WERROR=1 -DWITH_ANALYZER=1 -DWITH_DBUS=ON
          cmake --build build-analyze

      `make tests` does not run `-fanalyzer`, so a finding there reaches
      CI first. It is how an array filled in one function and read in
      another got past a green gate: the analyzer cannot follow a loop
      bound across the call, and called the read uninitialised.
- [ ] `make check` under **both** makes -- **if this round touched the
      Makefile.** GNU make and BSD make, and the claim is that they
      agree.
- [ ] Every knob the round added or renamed appears in `make options`,
      in `make flavours` if it belongs to a machine, and in the CI
      workflow if CI should build it. A `-D` CMake does not recognise is
      accepted in silence, so a renamed knob leaves CI green and testing
      nothing.


## Documents

Re-read each against what this round changed: a document that was true
this morning is a claim, not a fact.

- [ ] `README.md`, `docs/*.md`, `test/README.md` -- does every flag,
      path, topic and command they name still exist, and does a reader
      following their recipes get the tests they say they get? A
      section that moves between them takes its citations with it: the
      code comments cite a file and a section, `docs/hardware.md` and
      *LCD*.
- [ ] `DESIGN.md` -- did this round decide something, or invalidate a
      reason written there? A decision worked out through iteration is
      recorded in the same unit of work or not at all.
- [ ] `CLAUDE.md` -- does every pointer resolve, does the gate command
      run, and did this round cost time in a way the next session would
      too? Anything the README or DESIGN would answer is cut, not added.
- [ ] `CLAUDE.local.md` -- is what it says about this machine and about
      `ssh://moses` still true, including the test counts?
- [ ] `TODO.md` -- did this round finish, change or add an item?
- [ ] `dbus/moses.conf` -- **if a bus name, interface, method or property
      changed.** The policy file is what lets a daemon own its name; the
      gate passes without it and a deployed daemon goes silent.


## Release

Nothing here is versioned, and that is deliberate — see DESIGN.md,
*Nothing here is versioned*. There is no number to move and no tag to
make. What identifies a build is the commit, and the submodule commits
`make check` holds in place.


Then capture: whatever this round learned goes to the artifact that owns
that kind of fact, and anything this list failed to ask becomes a line
here.
