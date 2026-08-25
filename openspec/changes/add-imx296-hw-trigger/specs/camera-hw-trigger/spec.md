## Purpose

Gives the J106/TX2 IMX296 rig a single externally generated, hardware-timed frame trigger so every
camera exposes at the same instant instead of free-running on its own oscillator, and defines the
electrical and control contract that makes connecting that trigger safe and reversible.

## ADDED Requirements

### Requirement: Externally generated trigger waveform

A trigger source outside the Tegra SHALL generate the frame trigger from a hardware timer, so that
edge placement does not depend on operating-system scheduling. The waveform SHALL match what the
sensor's Fast Trigger mode consumes: idle high, one active-low pulse per frame, where the low
duration sets the exposure.

#### Scenario: Waveform shape

- **WHEN** the trigger source is running
- **THEN** the trigger net SHALL idle **high**, SHALL be driven **low** exactly once per frame
  period, and the low duration SHALL be the commanded exposure

#### Scenario: Timing comes from hardware, not software

- **WHEN** the trigger source is under any host or command load
- **THEN** the period and pulse width SHALL continue to be produced by a hardware timer, and SHALL
  NOT be reconstructed by software timing loops or operating-system timers

#### Scenario: Trigger stops cleanly

- **WHEN** the trigger source is commanded to stop
- **THEN** the trigger net SHALL be left in its idle-high state, not mid-pulse, so no sensor is
  left waiting inside an exposure

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
- **THEN** the emitted low pulse SHALL be shortened by the sensor's fixed exposure offset so that
  the exposure the sensor actually integrates equals the value requested

#### Scenario: Exposure cannot exceed the frame period

- **WHEN** a requested exposure would not leave the sensor time to read the frame out before the
  next trigger
- **THEN** the request SHALL be rejected with an explanatory error rather than silently clamped into
  a waveform that stalls capture

### Requirement: Electrical safety gate before connection

The sensor's trigger input is a 1.8 V-domain input with an absolute maximum rating of 3.3 V. The
documented procedure SHALL require the installer to establish which voltage domain the camera
module's trigger pads present **before** any wire is connected, and SHALL give a level-translation
path for the case where they are the raw sensor domain.

#### Scenario: Domain is measured first

- **WHEN** an installer follows the wiring procedure
- **THEN** the procedure SHALL require a measurement identifying the trigger pad's voltage domain
  and ground reference, and SHALL state that a 3.3 V drive into a raw 1.8 V input is outside the
  sensor's absolute maximum rating

#### Scenario: Level translation is specified for the 1.8 V case

- **WHEN** the measurement shows the trigger pad is a raw 1.8 V sensor input
- **THEN** the procedure SHALL specify a level-translation network, with values, that lands inside
  the sensor's input-high and input-low thresholds

#### Scenario: Idle state is safe when the trigger source is absent

- **WHEN** the trigger source is unpowered, in reset, or disconnected
- **THEN** the documented wiring SHALL leave the sensors' trigger input in a state that does not
  hold them inside an exposure, or SHALL state explicitly what recovery the host must perform

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
