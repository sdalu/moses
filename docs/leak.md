Leak detection
==============

How `moses_watermeter --leak` decides that water is leaking, what it
publishes when it does, and what it cannot see. The options are listed
in [programs.md](programs.md#moses_watermeter), the payload in
[interfaces.md](interfaces.md#leaks); why it is built this way is in
[DESIGN.md](../DESIGN.md), *Leaks are reported, not acted on*.


The principle
-------------

A meter cannot tell a leak from a use: a litre is a litre. What it can
see is the *shape* of consumption over time, and a leak has a shape
that people do not:

- **It does not stop.** People open a tap and close it again; a shower,
  even a long one, is paused; a bath fills in ten minutes. A toilet
  whose fill valve does not close, a burst pipe or a tap left open runs
  on, at a steady rate, for as long as nobody notices.
- **It is regular when it is small.** A drip passes the meter a litre
  at a time, at even intervals, at three in the morning as at noon.
  People use water in clusters -- a flush, a hand wash -- at uneven
  times.
- **It never lets the house go quiet.** Every household has hours
  without a drop, most nights. A leak, however small, fills them.

Each of these becomes a rule. None of them needs to know the house's
habits, which is why they can run on the device, beside the meter, and
keep working when the broker, the network or the database is down.


The three rules
---------------

Every litre counted goes in with the time it was counted. The rules are
also re-evaluated every 30 seconds, because some things -- a flow that
stopped, a litre that stayed alone, a quiet night -- only show by
waiting. Each rule is off when its setting is `0`.

### flow -- water that does not stop

A *flow* is a sequence of litres with no two more than **90 s** apart.
When one has lasted `--leak-flow` (default **20 min**), it is an
**alert**, with its start, the litres since then and its rate in L/min.
It ends -- back to `ok` -- as soon as 90 s pass without a litre.

90 s is one missed reading of an index polled every minute, so a steady
flow read from the index is not broken by the polling itself. 20 min is
longer than a bath fills or than this house's showers run without a
pause, and short enough that a stuck toilet (5.6 L/min) is reported
after about 110 L rather than the 3 m³ it once cost.

### slow -- a regular drip

A litre is **lone** when no other is counted within **3 minutes** either
side of it; an index reading that grew by two litres or more is a
cluster, not two lone litres. So a lone litre is only known to be lone
3 minutes after it arrives, which is when it joins the *run*.

The run is the lone litres in a row. Anything that is not a lone litre
-- a flush, a tap, any cluster -- starts it again, and so does a gap of
more than **3 hours** between two of them. When the latest
`--leak-slow` litres of the run (default **8**) are evenly spaced -- the
standard deviation of their intervals at most **25 %** of the mean -- it
is an **alert**. It carries the leak's rate, 3600 divided by the median
interval, in L/h, and the run's start and length; those stay fixed while
the drip goes on, so a report is published when the alert begins, not
at every litre. It ends when a cluster breaks the run; the next quiet
stretch raises it again.

Because ordinary use restarts the run, in practice it completes when the
house is asleep or empty, without the rule knowing what time it is.
Eight litres at even intervals rarely happen by chance: in the thirteen
months of this house's meter before July 2025, on four nights.

### quiet -- a house that never rests

A **quiet stretch** is `--leak-quiet` (default **2 h**) with no litre at
all. If the last **24 hours** held none, it is a **warning** -- never an
alert, since a busy day can do it too -- with the time the last quiet
stretch was seen. It is never raised in the first 24 hours after a
start, when there is no past to judge.

### Which report wins

One report at a time: the highest level, and at the same level flow,
then slow, then quiet. It is the current evidence, not a latch -- a
flow alert clears when the water stops, a slow alert when the run is
broken -- so a leak that goes on is reported again, and a consumer that
wants to remember has to.


Where the litres come from
--------------------------

`moses_watermeter` has two readings of the same water: the meter's
index over M-Bus, polled every `--interval`, and the pulses of the
HRI on a GPIO line, one per litre, each with the kernel's own
timestamp. The rules read one of them at a time, so no litre is ever
counted twice; `--leak-source` chooses:

| `--leak-source` | Reads                                                            |
| --------------- | ---------------------------------------------------------------- |
| `index`         | the index; refused at start without an M-Bus device              |
| `pulse`         | the pulses; refused at start without `-P`                        |
| `auto` (default) | the only one configured; with both, see below                   |

An index reading hands over the litres it grew by, whole litres only --
the fraction is carried to the next reading. A reading lower than the
last, or more than 1000 L above it (a meter swapped, a bad frame),
counts nothing and becomes the new reference.

### auto with both: checking the pulses

A `-P` pin says nothing about whether the line behind it is wired as
[Pulse counting](hardware.md#pulse-counting) wants, so `auto` starts on
the index and treats the pulses as unproven. At each index reading, the
pulses counted since the reading before are set against what the index
grew by. They are judged in **windows** of at least **3** readings that
grew and **10 L** -- as long as that takes, and a window closes at the
first reading where both hold, so it can be larger:

- they **match** if they differ by at most **2 + 10 %** of the window's
  litres -- the slack is for a pulse that lands on the other side of a
  reading;
- they are **broken** if they do not match -- no pulse at all, half or
  double the count -- or if **10** pulses arrive while the index does
  not move at all, which is a line that bounces without water; that
  verdict does not wait for a window.

| From      | On                          | To        | Rules read |
| --------- | --------------------------- | --------- | ---------- |
| unknown   | one matching window         | ok        | pulses     |
| unknown   | a broken verdict            | broken    | index      |
| ok        | a broken verdict            | broken    | index      |
| broken    | two matching windows in a row | ok      | pulses     |

So the pulses are believed after at least 10 L, and again after a
failure only after at least 20 L over six growing readings with no
failure in between: a line that is only sometimes wrong stays on the
index. Going broken is logged, sent on `error` and written as a
`failure="pulse-check"` line; coming back is logged. Either way the
`leak` report is republished, since its `source` changed.

The switch happens at an index reading, after that reading's litres have
gone to the source that was in use, so nothing is dropped or doubled.
The pulses are preferred once proven because they time every litre to
the nanosecond, where the index lumps a minute's litres into one reading.


What is published
-----------------

On `leak`, retained, at start -- so a report from before a restart is
replaced -- and whenever the level, the kind or the start changes:

~~~json
{ "level": "alert", "kind": "slow", "since": 1790483260, "volume": 8, "rate": 3.30, "source": "index" }
~~~

and the same as a `watermeter leak=` line on stdout and on the system
bus. The fields are in [interfaces.md](interfaces.md#leaks).
`moses_display` puts it on the panel as a banner in the header's place
-- an icon, `Flow 5.6 L/min`, and `42 min` at the right -- red for an
alert, amber for the quiet warning; every screen is in
[alert.md](alert.md).

**Nothing closes the valve.** Whether a report is worth shutting the
water for is for whoever reads it: slow and quiet are too slow to do
damage in the time a person takes to look, and a flow cannot be told
from a hose or a bath without knowing the house.


Where the numbers come from
---------------------------

The defaults were chosen by replaying this household's meter -- the
index at one reading a minute, 22 May 2024 to 27 Sep 2026 -- through
`src/leak.c` itself, against what actually happened there.

| Event                                      | What the meter saw                  | What the rules did                         |
| ------------------------------------------ | ----------------------------------- | ------------------------------------------ |
| Pipe leak, 29 Sep 2025 - 11 Jan 2026       | a litre every 10-38 min, 2.6-6 L/h  | slow alert on 86 of its 105 nights, the first at 04:53 on its first night (3.3 L/h) |
| Toilet fill valve stuck, 27 Sep 2026       | 5.6 L/min for 9 h 12 min, 3.1 m³    | flow alert at 20 min, after 117 L          |
| The same valve, 13 episodes before it      | 5.3-5.9 L/min, 11-179 min           | flow alert on the 8 that lasted 20 min or more; the 5 shorter ones pass |

and what they did the rest of the time:

- **flow**: 43 alerts in the 2.3 years. 14 are at a toilet's refill
  rate, 5-6.4 L/min, the episodes above among them. The other 29 are
  long legitimate flows at 1.9-9.2 L/min, most of them summer evenings
  around 18:00 at 7-9 L/min -- a hose, by the look of it. That is the
  price of a rule that does not know the house, and the reason the
  report carries the rate: a consumer that knows 5.6 L/min is the toilet
  can act on that and let a hose be.
- **slow**: 9 nights outside the pipe leak. Five of them fall in July
  to September 2025, when night-time use was already rising, and may be
  the leak starting; the other four -- one every few months -- are
  false. The settings tried, counted on the pipe leak's 105 nights and
  on the thirteen months before July 2025:

  | `--leak-slow` | spread | leak nights caught | false nights |
  | ------------- | ------ | ------------------ | ------------ |
  | 6             | 35 %   | 87                 | 19           |
  | 6             | 25 %   | 87                 | 10           |
  | 8             | 35 %   | 86                 | 6            |
  | **8**         | **25 %** | **86**           | **4**        |
  | 10            | 25 %   | 84                 | 2            |

  Longer runs are also later each night: ten litres at 3 L/h take 40
  minutes more than eight.
- **quiet**: a warning on 68 days of the pipe leak, and on 10 days
  outside it, busy summer days mostly, which is why it only warns.

`test/test_leak.c` replays three of those stretches -- the pipe leak's
first night, the stuck toilet, an ordinary day -- so a change that stops
catching them, or starts catching the ordinary day, fails the suite.


What it cannot see
------------------

- **A leak under about 1.5 L/h**, as an alert. At one litre per tick,
  eight even litres at 1 L/h need eight undisturbed hours, more than
  most nights give. The quiet warning still sees any leak fast enough to
  fill a 2-hour gap, i.e. about 0.5 L/h and up.
- **A stuck toilet that stops within 20 minutes.** It is not a flow yet;
  if it keeps doing it, the report never says so.
- **A steady leak of about 20 to 40 L/h.** Its litres come every 90 to
  180 s: too far apart to be a flow, too close together to be lone. The
  quiet warning is what reports it.
- **A leak that looks like use.** A tap left dripping into a flow of
  more than 90 s between litres, a long legitimate flow (garden hose,
  pool filling) reported as one: the rules report shapes, and a shape
  that belongs to the house is for the consumer to recognise.
- **Anything upstream of the meter.** Water that does not pass through
  it is not counted.
