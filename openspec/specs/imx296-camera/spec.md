# imx296-camera Specification

## Purpose
Brings a Sony IMX296 global-shutter camera up on a J106/TX2 CSI port so it can be captured from
raw V4L2 and Argus, while the IMX219 rolling-shutter modules on the remaining ports keep working
unchanged on the same shared reset line, shared MCLK and shared CSI/VI resources.
## Requirements

### Requirement: IMX296 sensor identification

The driver SHALL positively identify an IMX296 before registering it, by reading the sensor
information register and rejecting any device that does not report model 296. Identification SHALL
account for the sensor information register being unreadable while the sensor is in standby.

#### Scenario: Genuine IMX296 is identified

- **WHEN** the driver probes a device that reports a sensor information value whose model field
  (`(value >> 6) & 0x1ff`) equals `296`
- **THEN** the probe SHALL succeed, the detected variant (colour or monochrome) SHALL be recorded
  from the value's mono flag, and the variant SHALL be logged

#### Scenario: Identification requires leaving standby

- **WHEN** the driver reads the sensor information register while the sensor is still in standby
- **THEN** the driver SHALL first take the sensor out of standby and wait for it to settle before
  treating the value read as authoritative, so a zero reading is not mistaken for a missing sensor

#### Scenario: Non-IMX296 device is rejected

- **WHEN** the probed device does not report model `296`
- **THEN** the probe SHALL fail with an error and SHALL NOT register a V4L2 sub-device

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

### Requirement: IMX296 capture mode

The driver and device tree SHALL advertise the IMX296's full pixel array as a RAW10 mode, and the
advertised frame rate SHALL be one the sensor can actually sustain.

#### Scenario: Mode is advertised to V4L2

- **WHEN** userspace enumerates formats on the port's video node
- **THEN** a 1456×1088 RAW10 Bayer mode SHALL be offered

#### Scenario: Raw V4L2 capture succeeds

- **WHEN** frames are captured from the port's `/dev/video*` node in the advertised mode with CSI
  bypass enabled
- **THEN** capture SHALL produce frames of the advertised size with no CSI or VI errors reported

#### Scenario: Advertised rate is honest

- **WHEN** a maximum frame rate is declared for the mode in the device tree
- **THEN** it SHALL correspond to a rate the programmed register configuration achieves, and SHALL
  NOT advertise a rate that has no register table behind it

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

### Requirement: Exposure and gain control

The driver SHALL expose working exposure and gain controls through the standard camera control
interface, with limits derived from the sensor's own timing model. Where the sensor is streaming
under an external hardware trigger, exposure is set by the trigger pulse rather than by the sensor's
shutter registers, and the driver SHALL report that honestly instead of claiming a write it cannot
honour.

#### Scenario: Exposure is applied

- **WHEN** userspace sets an exposure value within the advertised range and the sensor is
  free-running
- **THEN** the driver SHALL program the sensor's shutter registers accordingly and report success

#### Scenario: Gain is applied

- **WHEN** userspace sets a gain value within the advertised range
- **THEN** the driver SHALL program the sensor's gain register accordingly and report success, in
  free-running and externally triggered modes alike

#### Scenario: Out-of-range values are clamped, not passed through

- **WHEN** a requested exposure or gain lies outside what the current mode's frame timing permits
- **THEN** the driver SHALL clamp the request to the achievable limit rather than programming an
  invalid register value

#### Scenario: Exposure is owned by the trigger when externally triggered

- **WHEN** userspace sets an exposure value while the sensor is streaming under an external
  hardware trigger
- **THEN** the driver SHALL NOT program the sensor's shutter registers, SHALL accept the call
  without failing the stream, and SHALL make clear — at least once per stream — that exposure is
  being taken from the trigger pulse width instead

#### Scenario: Frame-rate control cannot corrupt triggered readout

- **WHEN** the platform re-applies the frame-rate control while the sensor is streaming under an
  external hardware trigger
- **THEN** the driver SHALL hold the sensor's frame-length register at the value the active readout
  mode requires, so a re-applied minimum frame rate cannot stretch it and stall capture

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

### Requirement: Independence from the shared MCLK rate

The IMX296 SHALL operate without requiring a change to the MCLK rate fanned out to the IMX219
sensors, because a rate that suits one sensor family does not suit the other.

#### Scenario: Shared MCLK rate is left alone

- **WHEN** the IMX296 is brought up
- **THEN** the shared camera master clock rate SHALL remain unchanged for the IMX219 ports

#### Scenario: Self-clocked module is not blocked on MCLK

- **WHEN** the fitted module supplies its own input clock
- **THEN** the driver SHALL still complete probe and streaming even if the shared master clock is
  not enabled on its behalf

### Requirement: Argus recognises each IMX296 as a distinct camera

Every populated IMX296 port SHALL appear to Argus as its own camera, so that a camera-enumerating
application sees every populated port.

#### Scenario: Every populated port is enumerated

- **WHEN** an Argus client enumerates available camera devices
- **THEN** each populated IMX296 port SHALL appear as a distinct camera device

#### Scenario: Module identity does not collide

- **WHEN** the platform's camera module entries are declared for all populated ports
- **THEN** each module's identifying fields SHALL be unique, so no two ports alias onto one camera

### Requirement: Driver is delivered as a reproducible patch

The driver SHALL be delivered in the repository's established form so the build is reproducible on
a clean kernel source tree.

#### Scenario: Patch applies to a clean tree

- **WHEN** the patch is applied to an unmodified L4T R32.7.6 kernel source tree
- **THEN** it SHALL apply without conflict or fuzz, adding the driver source and the build-system
  entries that let it be configured and compiled

#### Scenario: Patch is independent of the IMX219 patch

- **WHEN** the IMX296 patch and the existing IMX219 shared-reset patch are both applied
- **THEN** neither SHALL conflict with the other, and each SHALL remain independently revertible

#### Scenario: Build produces the expected kernel

- **WHEN** the kernel is built with the driver enabled
- **THEN** the resulting image SHALL report the same kernel version string as the board's running
  kernel, so existing out-of-tree modules stay loadable

### Requirement: Deployment remains reversible

Deploying the IMX296 support SHALL keep the existing rollback path intact.

#### Scenario: Previous boot configuration is retained

- **WHEN** the new kernel image and device tree are deployed
- **THEN** a previously working boot entry SHALL remain selectable, and the on-partition device tree
  SHALL NOT be modified

#### Scenario: Bad deployment is recoverable

- **WHEN** a deployed image or device tree fails to boot
- **THEN** the working boot entry SHALL be selectable from the boot menu over the serial console

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
