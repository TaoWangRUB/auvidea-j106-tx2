## Purpose

Puts camera exposures and IMU samples on a single timebase with a known and stable offset, so that
motion estimation can fuse them — covering where each timestamp originates, which clock it is on,
what remains unknown, and how that unknown is measured.

## ADDED Requirements

### Requirement: One clock for both sensors

Camera and IMU timestamps SHALL be expressed on the same monotonic system clock, so that no
conversion between clock domains is needed to compare them.

#### Scenario: Camera timestamps are monotonic

- **WHEN** a frame is dequeued
- **THEN** its timestamp SHALL be on the system's monotonic clock, and the code SHALL verify this
  from the buffer's own timestamp-type flag rather than assuming it

#### Scenario: IMU timestamps are on the same clock

- **WHEN** an IMU sample is recorded
- **THEN** its timestamp SHALL be taken on the same monotonic clock as the camera timestamps

#### Scenario: Clocks that are not monotonic are rejected, not silently mixed

- **WHEN** a timestamp source provides its own timestamp on a different clock — for example a
  wall-clock-based kernel interface
- **THEN** that timestamp SHALL NOT be recorded as if it were monotonic; either it is converted with
  a measured offset, or it is discarded in favour of a monotonic reading taken at the same event

### Requirement: IMU samples are timed at data-ready, not at readout

An IMU sample's timestamp SHALL reflect when the sample became available, not when software happened
to read it over the bus, so that bus scheduling does not enter the measurement.

#### Scenario: Timestamp is taken on the data-ready edge

- **WHEN** the IMU asserts its data-ready interrupt line
- **THEN** the timestamp SHALL be captured at that event, and the subsequent bus read SHALL NOT
  redefine the sample's time

#### Scenario: Interrupt line is configured for the carrier

- **WHEN** the IMU's interrupt output is configured
- **THEN** it SHALL be set to a push-pull drive, because the carrier provides no pull-up on that
  line, and the carrier's inversion of the line SHALL be accounted for in the edge polarity used

#### Scenario: Missed samples are visible

- **WHEN** samples are dropped, whether through a missed interrupt or a slow reader
- **THEN** the loss SHALL be detectable from the recorded output rather than appearing as a silent
  gap in an otherwise regular series

### Requirement: Frame timing is recovered from the trigger's periodicity

Where frames are produced by a hardware-periodic trigger, frame times SHALL be estimated from the
whole sequence rather than taken from individual timestamps, because the sequence carries far more
timing information than any one sample.

#### Scenario: Period and phase are estimated over many frames

- **WHEN** a run of frames captured under a periodic trigger is analysed
- **THEN** the frame period and phase SHALL be estimated by fitting the whole series, and the
  estimate's uncertainty SHALL improve as more frames are included

#### Scenario: Relative oscillator drift is absorbed

- **WHEN** the trigger source's oscillator and the host clock differ in rate
- **THEN** the estimated period SHALL express the trigger's true rate in host clock units, so the
  difference does not accumulate as a growing timing error

#### Scenario: Exposure midpoint is derived, not guessed

- **WHEN** an exposure midpoint is needed for a frame
- **THEN** it SHALL be computed from the estimated frame time, the commanded exposure, and the known
  fixed sensor and interface delays

### Requirement: The camera-to-IMU offset is a single stated constant

The residual offset between a camera's exposure and the IMU timebase SHALL be treated as **one**
constant for the whole rig rather than one per camera, and SHALL be stated with its provenance
wherever synchronised data is recorded.

#### Scenario: One constant covers every camera

- **WHEN** the cameras are driven by a shared trigger edge
- **THEN** the residual camera-to-IMU offset SHALL be represented as a single value covering all
  cameras, because a shared edge leaves no per-camera component to estimate

#### Scenario: Offset is obtained without extra hardware

- **WHEN** no trigger echo is wired
- **THEN** the recorded output SHALL be in a form a calibration solver can consume, so the offset
  can be estimated from motion, and the documentation SHALL describe that route

#### Scenario: The offset is stated, not assumed

- **WHEN** synchronised data is recorded
- **THEN** the offset in use SHALL be recorded alongside it, and an offset that has not been
  measured SHALL be marked as unmeasured rather than recorded as a value

#### Scenario: The direct-measurement route is documented, including how it is timestamped

- **WHEN** a trigger echo is used to obtain the offset directly
- **THEN** the documentation SHALL specify that the echo is timestamped by the same path as the IMU,
  so that the wake-up latency common to both cancels rather than entering the offset, and SHALL
  include level translation into the target pin's voltage domain

### Requirement: Recorded output is usable by a motion-estimation pipeline

The recorded result SHALL be self-describing enough to be consumed without knowing how it was
produced.

#### Scenario: Both streams share one time column

- **WHEN** camera and IMU data are recorded together
- **THEN** both SHALL carry timestamps on the same clock, in one consistent unit

#### Scenario: Provenance is recorded

- **WHEN** a recording is written
- **THEN** it SHALL state the clock used, the trigger rate and exposure in force, and the offset
  applied, so a recording can be interpreted long after it was taken

#### Scenario: Unsynchronised recordings are marked as such

- **WHEN** data is recorded while the cameras are free-running rather than triggered
- **THEN** the recording SHALL be marked as unsynchronised, so it is not mistaken for triggered data
