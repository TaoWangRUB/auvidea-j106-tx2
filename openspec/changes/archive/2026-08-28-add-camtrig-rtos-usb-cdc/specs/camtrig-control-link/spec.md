## Purpose

Defines the trigger source's command interface as a contract that is independent of any one cable:
one identical protocol carried over more than one transport at once, replies returned to whichever
transport asked, and a trigger that keeps running whether or not a host is attached.

## ADDED Requirements

### Requirement: The trigger runs without any control link

The trigger source SHALL begin emitting a valid waveform from built-in defaults at power-on, before
and without any host connection. Loss, absence, or failure of every control transport SHALL NOT stop
the waveform or alter its parameters. The control link exists to change parameters, never to sustain
them.

#### Scenario: No host has ever been attached

- **WHEN** the trigger source is powered on with no host connected to any transport
- **THEN** it SHALL start emitting the waveform at its built-in default frame rate and exposure
  without waiting for a command

#### Scenario: Host disconnects mid-run

- **WHEN** an attached host disconnects, powers down, or closes the port while the trigger is running
- **THEN** the waveform SHALL continue unchanged at its currently commanded parameters

#### Scenario: Control path faults do not reach the waveform

- **WHEN** the control path stalls, faults, or stops being serviced
- **THEN** the waveform SHALL continue to be emitted by the hardware timer at its last commanded
  parameters, because no part of generating it depends on the control path running

### Requirement: One protocol over concurrently available transports

The trigger source SHALL accept its command protocol over more than one physical transport, and the
protocol's syntax, semantics, and replies SHALL be identical on each. Transports SHALL be usable
concurrently, and no transport SHALL need to be selected, enabled, or configured for another to
work. A host tool SHALL require no change beyond naming a different port.

#### Scenario: Identical protocol on every transport

- **WHEN** the same command is issued over any supported transport
- **THEN** it SHALL be parsed identically, have the identical effect, and produce the identical reply
  text

#### Scenario: Transports do not depend on each other

- **WHEN** one transport has no host attached, is disconnected, or is unusable
- **THEN** every other transport SHALL remain fully functional for command and reply

#### Scenario: Concurrent hosts

- **WHEN** hosts are attached to two transports at the same time and both issue commands
- **THEN** each command SHALL be executed in full, and SHALL NOT be interleaved with another
  command's execution such that the trigger is left in a state neither command requested

### Requirement: Replies return to the transport that issued the command

A reply SHALL be delivered to the transport the command arrived on, so a host sees answers to its own
requests and not to another host's. Output that is not a reply to a command SHALL be delivered to
every available transport, since it belongs to no single requester.

#### Scenario: Reply routing

- **WHEN** a command arrives on one transport while another transport is also attached
- **THEN** the reply SHALL be written to the originating transport and SHALL NOT be written to the
  other

#### Scenario: Unsolicited output is broadcast

- **WHEN** the trigger source emits output that is not a reply to a command, such as its start-up
  banner or a burst-completion notice
- **THEN** that output SHALL be delivered to every attached transport

### Requirement: USB control transport presents a virtual serial port

Where a transport is provided over USB, the trigger source SHALL enumerate as a device class that a
host exposes as a standard serial port, requiring no vendor driver on Linux. It SHALL enumerate
whether or not the host asserts any modem control signal, and SHALL NOT require the host to open the
port before the trigger will run.

#### Scenario: Enumerates as a serial port

- **WHEN** the trigger source is connected to a Linux host running its normal firmware
- **THEN** the host SHALL enumerate it and expose a serial character device, with no vendor-specific
  driver installed

#### Scenario: Runs regardless of port state

- **WHEN** the host has enumerated the device but no application has opened the port, or the host
  never asserts a modem control signal
- **THEN** the trigger SHALL run normally and the device SHALL remain enumerated

#### Scenario: Output with no reader does not stall the trigger

- **WHEN** the device produces output while the host is not reading it, until buffering is exhausted
- **THEN** the waveform SHALL be unaffected, and the source SHALL discard or defer output rather than
  block the trigger

### Requirement: Firmware flashing stays available over the same connector

Adding a USB control transport SHALL NOT remove or degrade the ability to flash the firmware over the
same USB connector, and SHALL NOT introduce a firmware state from which flashing cannot be reached.
Entry into the flashing mode SHALL NOT depend on the firmware being functional.

#### Scenario: Flashing mode is reachable from any firmware state

- **WHEN** an operator invokes the board's hardware sequence for entering its ROM bootloader
- **THEN** the device SHALL enumerate in its firmware-update mode regardless of what the application
  firmware was doing, including when that firmware is faulted, hung, or absent

#### Scenario: The two USB modes are distinct

- **WHEN** the device is in firmware-update mode
- **THEN** it SHALL present the firmware-update interface rather than the control transport, and the
  operator SHALL be able to return to the control transport by leaving that mode
