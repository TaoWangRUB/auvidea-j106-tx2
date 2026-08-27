## MODIFIED Requirements

### Requirement: Sensor is reachable at the carrier-shifted address

Each IMX296 SHALL be addressed at the I²C address the carrier presents on the bus, not the sensor's
native address, because the J106 address-shifter remaps the south sensor of each CSI pair. Where two
IMX296 share one bus, the north sensor SHALL be declared at its native address and the south sensor
at the shifted address.

#### Scenario: Two IMX296 on one bus are declared at distinct addresses

- **WHEN** a CSI pair on one I²C bus is populated with two IMX296 modules
- **THEN** the north sensor SHALL be declared at `0x1a` and the south sensor at `0x18`, and both
  SHALL bind as separate camera devices

#### Scenario: Address alias does not create a second camera

- **WHEN** a sensor also answers at an aliased address on the same bus, because the IMX296 exposes
  both a `SLAMODE`-strapped address and a common address
- **THEN** only the common address SHALL be declared, so exactly one camera is registered per
  physical module and the declaration survives a differently-strapped module

### Requirement: Single-lane CSI routing

Every port carrying an IMX296 SHALL be configured for single-lane MIPI CSI-2, independently of any
two-lane ports on the same board.

#### Scenario: Each IMX296 port is configured for one lane

- **WHEN** the device tree describes an IMX296's sensor endpoint and the matching NVCSI and VI
  endpoints for that port
- **THEN** all three SHALL declare a lane width of 1

#### Scenario: Lane budget reflects the populated configuration

- **WHEN** the platform's total CSI lane count is declared
- **THEN** it SHALL equal the sum of the lanes declared by all ports, counting each IMX296 port as
  one lane and each two-lane sensor port as two — recomputed whenever the population changes

### Requirement: Coexistence with other sensors on the shared reset line

The J106 ties the reset pins of all six CSI ports to a single GPIO. The IMX296 driver SHALL NOT
disturb that shared line, so that IMX296 ports can be added, removed or re-populated without
resetting or unbinding sensors on the other ports.

#### Scenario: Shared reset line is not claimed exclusively

- **WHEN** the IMX296 driver requests the shared reset GPIO already held by another sensor or a hog
- **THEN** it SHALL treat the resulting busy condition as success and continue probing

#### Scenario: Shared reset line is never driven low

- **WHEN** the IMX296 driver powers its sensor on or off
- **THEN** it SHALL NOT drive the shared reset line low and SHALL NOT free it

#### Scenario: Sensor settles after a shared reset before it is addressed

- **WHEN** the shared reset line is pulsed shortly before an IMX296 is probed, as happens when
  another driver on the same line pulses it at boot
- **THEN** the driver SHALL wait for the sensor's documented power-on settle time and SHALL retry its
  first register access, so that the first sensor probed on each bus enumerates reliably

#### Scenario: Other populated ports keep working

- **WHEN** the kernel is booted after an IMX296 port is added or re-populated
- **THEN** every other port that enumerated before the change SHALL still bind and still capture

### Requirement: Argus recognises each IMX296 as a distinct camera

Every populated IMX296 port SHALL appear to Argus as its own camera, so that a camera-enumerating
application sees every populated port.

#### Scenario: Every populated port is enumerated

- **WHEN** an Argus client enumerates available camera devices
- **THEN** each populated IMX296 port SHALL appear as a distinct camera device

#### Scenario: Module identity does not collide

- **WHEN** the platform's camera module entries are declared for all populated ports
- **THEN** each module's identifying fields SHALL be unique, so no two ports alias onto one camera

## ADDED Requirements

### Requirement: ISP tuning matches the sensor

The Argus ISP tuning installed on the board SHALL be one calibrated for the sensor it is applied to.
Applying another sensor's tuning is a defect, not a cosmetic issue: it breaks automatic white balance
outright rather than merely shifting colour.

#### Scenario: Colour is approximately neutral on a neutral scene

- **WHEN** a neutral scene is captured through Argus with a sensor-appropriate tuning installed
- **THEN** the mean R, G and B channel levels SHALL be within a documented tolerance of each other,
  and SHALL NOT sit at the sensor's native unbalanced raw ratios

#### Scenario: Wrong-sensor tuning is recognisable

- **WHEN** the installed tuning was calibrated for a different sensor
- **THEN** the failure SHALL be identifiable from the channel ratios approximating the raw Bayer
  response, indicating that auto white balance is applying unity gains and never converging

#### Scenario: Tuning selection is explicit and reversible

- **WHEN** the deployment installs an ISP tuning
- **THEN** which tuning is installed SHALL be selectable, and the previous file SHALL be preserved,
  because the tuning file is global to all sensors on the board and a mixed-sensor board cannot have
  every sensor correct at once

### Requirement: Auto-exposure is bounded

Argus auto-exposure for the IMX296 SHALL be constrained so it cannot drive analogue gain to the top
of its range, which produces an image dominated by chroma noise.

#### Scenario: Gain is capped

- **WHEN** a capture pipeline is started against an IMX296
- **THEN** it SHALL apply an exposure-time and gain range that bounds gain well below the sensor's
  maximum, and the bounds SHALL be overridable for darker or brighter scenes

#### Scenario: Unbounded auto-exposure is a known failure, not a surprise

- **WHEN** auto-exposure is left unconstrained
- **THEN** the resulting behaviour — gain pinned at the top of the sensor's range while still
  exposing for highlights — SHALL be documented so it is diagnosed rather than rediscovered
