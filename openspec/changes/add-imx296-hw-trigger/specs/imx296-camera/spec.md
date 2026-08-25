## MODIFIED Requirements

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
