## MODIFIED Requirements

### Requirement: Externally generated trigger waveform

A trigger source outside the Tegra SHALL generate the frame trigger from a hardware timer, so that
edge placement does not depend on operating-system scheduling. The waveform SHALL match what the
sensor's Fast Trigger mode consumes: one asserted pulse per frame, whose duration sets the exposure.
The requirement is expressed in terms of assertion rather than voltage level, because the transport
between source and sensor may invert or isolate the signal.

Independence from scheduling SHALL hold for **any** scheduler, including one running on the trigger
source itself. Where the trigger source runs a multitasking kernel or interrupt-driven middleware,
the waveform SHALL be produced by timer hardware that continues to run without software service, so
that scheduler tick, task priority, preemption latency, interrupt latency, interrupt masking, and
control-path faults cannot move an edge.

#### Scenario: Waveform shape

- **WHEN** the trigger source is running
- **THEN** the trigger SHALL be asserted exactly once per frame period, SHALL be unasserted for the
  remainder, and the asserted duration SHALL be the commanded exposure

#### Scenario: Timing comes from hardware, not software

- **WHEN** the trigger source is under any host or command load
- **THEN** the period and pulse width SHALL continue to be produced by a hardware timer, and SHALL
  NOT be reconstructed by software timing loops or operating-system timers

#### Scenario: A scheduler on the trigger source cannot move an edge

- **WHEN** the trigger source runs a multitasking kernel, and its tasks are preempted, delayed,
  starved, or blocked for any duration
- **THEN** the emitted period and pulse width SHALL be unchanged, because the timer generates the
  waveform without per-edge software service

#### Scenario: Interrupt activity cannot move an edge

- **WHEN** interrupt handlers on the trigger source run at any rate, for any duration, or interrupts
  are masked for an extended period
- **THEN** the emitted period and pulse width SHALL be unchanged, and no edge SHALL be deferred,
  because no interrupt participates in producing the waveform

#### Scenario: Waveform survives a control-path fault

- **WHEN** any task or handler outside the waveform's own hardware path stops running, faults, or
  hangs
- **THEN** the trigger SHALL continue at its last commanded parameters rather than stopping, stalling
  mid-pulse, or reverting

#### Scenario: Trigger stops cleanly

- **WHEN** the trigger source is commanded to stop
- **THEN** the trigger SHALL be left **unasserted**, not mid-pulse, so no sensor is left waiting
  inside an exposure
