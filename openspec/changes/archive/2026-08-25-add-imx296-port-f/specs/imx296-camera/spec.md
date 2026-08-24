## Purpose

Brings a Sony IMX296 global-shutter camera up on a J106/TX2 CSI port so it can be captured from
raw V4L2 and Argus, while the IMX219 rolling-shutter modules on the remaining ports keep working
unchanged on the same shared reset line, shared MCLK and shared CSI/VI resources.

## ADDED Requirements

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

The IMX296 SHALL be addressed at the I²C address the carrier presents on the bus, not the sensor's
native address, because the J106 address-shifter remaps the south sensor of each CSI pair.

#### Scenario: Port F sensor binds at the shifted address

- **WHEN** the device tree declares the port-F IMX296 at I²C address `0x18` on bus `i2c@c250000`
- **THEN** the driver SHALL bind to it and the sensor SHALL appear as a bound I²C client on that bus

#### Scenario: Address alias does not create a second camera

- **WHEN** the same die also answers at an aliased address on the same bus
- **THEN** only one sensor node SHALL be declared, so exactly one camera is registered for the port

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

The port carrying the IMX296 SHALL be configured for single-lane MIPI CSI-2, independently of the
two-lane configuration used by the IMX219 ports.

#### Scenario: Port F is configured for one lane

- **WHEN** the device tree describes the IMX296's sensor endpoint and the matching NVCSI and VI
  endpoints for that port
- **THEN** all of them SHALL declare a lane width of 1

#### Scenario: Lane budget reflects the mixed configuration

- **WHEN** the platform's total CSI lane count is declared
- **THEN** it SHALL equal the sum of the lanes actually used by all populated ports, counting the
  IMX296 port as one lane and each IMX219 port as two

### Requirement: Exposure and gain control

The driver SHALL expose working exposure and gain controls through the standard camera control
interface, with limits derived from the sensor's own timing model.

#### Scenario: Exposure is applied

- **WHEN** userspace sets an exposure value within the advertised range
- **THEN** the driver SHALL program the sensor's shutter registers accordingly and report success

#### Scenario: Gain is applied

- **WHEN** userspace sets a gain value within the advertised range
- **THEN** the driver SHALL program the sensor's gain register accordingly and report success

#### Scenario: Out-of-range values are clamped, not passed through

- **WHEN** a requested exposure or gain lies outside what the current mode's frame timing permits
- **THEN** the driver SHALL clamp the request to the achievable limit rather than programming an
  invalid register value

### Requirement: Coexistence with IMX219 modules on the shared reset line

The J106 ties the reset pins of all six CSI ports to a single GPIO. The IMX296 driver SHALL NOT
disturb that shared line, so that adding the IMX296 cannot reset or unbind the IMX219 sensors.

#### Scenario: Shared reset line is not claimed exclusively

- **WHEN** the IMX296 driver requests the shared reset GPIO already held by an IMX219 sensor
- **THEN** it SHALL treat the resulting busy condition as success and continue probing

#### Scenario: Shared reset line is never driven low

- **WHEN** the IMX296 driver powers its sensor on or off
- **THEN** it SHALL NOT drive the shared reset line low and SHALL NOT free it, leaving the five
  IMX219 sensors held out of reset

#### Scenario: IMX219 ports keep working

- **WHEN** the kernel carrying the IMX296 driver is booted with the mixed camera configuration
- **THEN** every IMX219 port that enumerated before the change SHALL still bind and still capture

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

### Requirement: Argus recognises the IMX296 as a distinct camera

The IMX296 SHALL appear to Argus as its own camera, separate from the IMX219 cameras, so that a
camera-enumerating application sees every populated port.

#### Scenario: Distinct Argus camera is enumerated

- **WHEN** an Argus client enumerates available camera devices
- **THEN** the IMX296 port SHALL appear as a distinct camera device alongside the IMX219 cameras

#### Scenario: Module identity does not collide

- **WHEN** the platform's camera module entries are declared for all populated ports
- **THEN** the IMX296 module's identifying fields SHALL be unique, so it does not alias onto an
  IMX219 camera

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
