# camera-hw-trigger Specification

## Purpose
Gives the J106/TX2 IMX296 rig a single externally generated, hardware-timed frame trigger so every
camera exposes at the same instant instead of free-running on its own oscillator, and defines the
electrical and control contract that makes connecting that trigger safe and reversible.

## Requirements

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

### Requirement: Trigger rate and exposure stay inside the sensor's limits

The trigger source SHALL refuse to emit a waveform the sensor cannot honour. The limits SHALL be
derived from the sensor's own timing model rather than assumed.

#### Scenario: Frame period respects the trigger prohibition window

- **WHEN** a frame rate is requested whose period is shorter than the sensor's next-trigger
  prohibited period for the active readout mode
- **THEN** the request SHALL be rejected with an explanatory error and the running waveform SHALL be
  left unchanged

#### Scenario: Exposure accounts for the sensor's fixed offset

- **WHEN** an exposure time is requested
- **THEN** the emitted pulse SHALL be shortened by the sensor's fixed exposure offset so that the
  exposure the sensor actually integrates equals the value requested

#### Scenario: Exposure cannot exceed the frame period

- **WHEN** a requested exposure would not leave the sensor time to read the frame out before the
  next trigger
- **THEN** the request SHALL be rejected with an explanatory error rather than silently clamped into
  a waveform that stalls capture

### Requirement: Drive matched to the module's trigger input

The camera module's trigger input SHALL be characterised by measurement before any connection is
made, and the drive circuit SHALL match what that measurement shows rather than what the pad
labelling implies.

#### Scenario: Input type is established by measurement

- **WHEN** an installer follows the wiring procedure
- **THEN** the procedure SHALL require measurements that establish whether the trigger pads are a
  voltage input referenced to module ground or a galvanically isolated current input, and SHALL
  record the result

#### Scenario: Isolated current input is driven with current

- **WHEN** the trigger pads are an optocoupler LED isolated from module ground
- **THEN** the drive SHALL supply a bounded forward current through a series resistor, SHALL NOT
  connect to the camera's ground, and the per-pin current SHALL stay within the driving device's
  rated limit

#### Scenario: Ground-referenced input stays inside the sensor's ratings

- **WHEN** the trigger pad is instead a voltage input in the sensor's own 1.8 V domain
- **THEN** the procedure SHALL specify level translation landing inside the sensor's input-high and
  input-low thresholds, because that pin's absolute maximum rating is below a 3.3 V drive

#### Scenario: Reversed connection is recoverable, not destructive

- **WHEN** the trigger connection is made with the wrong polarity
- **THEN** the result SHALL be an absence of triggering rather than damage, and the procedure SHALL
  say so, so an installer can resolve polarity empirically

#### Scenario: Idle state is safe when the trigger source is absent

- **WHEN** the trigger source is unpowered, in reset, or disconnected
- **THEN** the trigger SHALL come to rest in its unasserted state, so no sensor is left held inside
  an exposure

### Requirement: Pulse sense and transport delay are correctable at runtime

Where the trigger passes through an isolating or inverting stage, neither the sense of the pulse nor
its transport delay can be determined by inspection from the driving side. Both SHALL therefore be
adjustable at runtime, without rebuilding or reflashing the trigger source.

#### Scenario: Pulse sense can be inverted

- **WHEN** driving the interface in one sense produces no frames
- **THEN** the opposite sense SHALL be selectable at runtime, and the active setting SHALL be
  reported

#### Scenario: Transport delay is compensated

- **WHEN** the interface's asserting and releasing delays differ, so the exposure the sensor
  integrates differs from the exposure requested
- **THEN** that difference SHALL be settable at runtime and SHALL be removed from the emitted pulse
  alongside the sensor's own fixed exposure offset

#### Scenario: Transport delay does not disturb synchronisation

- **WHEN** every camera is driven through the same kind of interface from one timer
- **THEN** the delay SHALL affect only the absolute exposure, not the relative timing between
  cameras, and the documentation SHALL state this so the two are not confused

### Requirement: Sensor enters and leaves trigger mode safely

The camera driver SHALL be able to place every IMX296 into external trigger mode and return it to
free-running mode, following the sensor's mandated transition sequence, without disturbing the
carrier's shared reset line or the other sensors.

#### Scenario: Mode transition goes through a standby cycle

- **WHEN** the driver switches a sensor between free-running and external-trigger mode
- **THEN** the transition SHALL be made as part of a standby cycle — the sensor's fast-trigger mode
  may not be changed during operation — and the trigger registers SHALL be programmed **before**
  master operation is started, not while the sensor is already running

#### Scenario: Sync outputs are not driven onto the shared wiring

- **WHEN** a sensor is placed into external trigger mode
- **THEN** its vertical and horizontal sync pins SHALL be placed in a high-impedance state, so that
  modules wired in parallel cannot contend on those pads

#### Scenario: Shared reset line is untouched

- **WHEN** any sensor enters or leaves trigger mode
- **THEN** the carrier's shared camera reset GPIO SHALL NOT be requested, driven, or released, so
  sensors on other ports are unaffected

### Requirement: Trigger mode is selectable at runtime without reflashing

Switching the rig between free-running and externally triggered operation SHALL NOT require
rebuilding or redeploying the device tree, so the two configurations can be compared back to back on
one boot.

#### Scenario: Mode is switched without a reboot

- **WHEN** an operator selects external-trigger mode on a running system
- **THEN** the selection SHALL take effect on the next stream start, with no device-tree change,
  no reflash and no reboot

#### Scenario: Free-running remains the default

- **WHEN** the system boots with no explicit selection
- **THEN** the cameras SHALL free-run exactly as they did before this capability existed, so a rig
  with no trigger wiring attached is unaffected

#### Scenario: Active mode is discoverable

- **WHEN** an operator inspects a running system
- **THEN** the mode each sensor is streaming in SHALL be reported, so a silent fallback to
  free-running cannot be mistaken for successful triggering

### Requirement: Synchronisation is measurable

The change SHALL provide a means of demonstrating, from the host, that the cameras are actually
synchronised, rather than relying on the trigger being wired correctly.

#### Scenario: Inter-camera skew is reported

- **WHEN** an operator runs the synchronisation check against all populated camera nodes
- **THEN** it SHALL capture frames concurrently from each and report the per-frame spread of
  capture timestamps across cameras, including a worst-case figure

#### Scenario: Triggered and free-running are distinguishable

- **WHEN** the same check is run once free-running and once externally triggered
- **THEN** the reported spread SHALL be small and bounded in the triggered case and SHALL drift
  without bound in the free-running case, making the difference evident from the output alone

#### Scenario: Frame count matches trigger count

- **WHEN** the trigger source has emitted a known number of pulses
- **THEN** the check SHALL make it possible to confirm each camera delivered the same number of
  frames, so dropped or duplicated triggers are detected
