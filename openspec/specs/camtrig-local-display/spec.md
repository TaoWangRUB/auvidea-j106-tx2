# camtrig-local-display Specification

## Purpose
Makes the trigger source readable on its own, without a host or a cable, by showing its live
operating state on the board's integrated panel — while guaranteeing that nothing about the rig ever
waits on that panel.

## Requirements

### Requirement: The display never affects the trigger

The display SHALL be strictly an observer. Rendering, panel faults, a disconnected panel, and
arbitrarily slow panel I/O SHALL NOT alter the emitted waveform, delay command execution, or stop the
trigger. The display SHALL occupy the lowest scheduling priority of any task in the firmware.

#### Scenario: Waveform is unaffected by rendering

- **WHEN** the display is repainting, however slowly
- **THEN** the emitted period and pulse width SHALL be unchanged, because the waveform is produced by
  timer hardware that the display path does not touch

#### Scenario: A missing or faulty panel changes nothing

- **WHEN** no panel is connected, or the panel does not respond
- **THEN** the trigger SHALL continue at its commanded parameters and the command interface SHALL
  remain fully responsive

#### Scenario: Commands outrank the display

- **WHEN** a command arrives while the display is rendering
- **THEN** the command SHALL be executed without waiting for the render to finish

### Requirement: The display shows the state needed to operate the rig unattended

The display SHALL show the trigger's current operating state, sufficient to tell whether the rig is
running and with what parameters, without attaching a host. It SHALL include at minimum the frame
rate, the exposure, whether the trigger is running, and the count of pulses emitted.

#### Scenario: Running state is visible at a glance

- **WHEN** an operator looks at the panel
- **THEN** it SHALL show whether the trigger is running or stopped, the current frame rate, and the
  current exposure

#### Scenario: Progress is observable

- **WHEN** the trigger is running
- **THEN** the displayed pulse count SHALL advance, so a stalled trigger is distinguishable from a
  running one without instruments

#### Scenario: Per-camera differences are not hidden

- **WHEN** the four channels do not all carry the same exposure
- **THEN** the display SHALL indicate that they differ rather than showing one channel's value as if
  it applied to all

### Requirement: Displayed values are never internally inconsistent

Values read for display SHALL be taken under the same mutual exclusion that guards command execution,
so a partially applied parameter change is never rendered.

#### Scenario: No half-applied state is shown

- **WHEN** a command that changes several parameters together is executing
- **THEN** the display SHALL show either the state before the change or the state after it, never a
  mixture

### Requirement: The display does not claim pins the trigger needs

The display SHALL be driven only from pins that no trigger channel uses. Where the hardware maps a
display signal and a trigger output onto the same pin, the trigger SHALL keep that pin and the
display SHALL be relocated or omitted.

#### Scenario: Trigger channels take precedence

- **WHEN** a display signal and a trigger channel contend for the same pin
- **THEN** the trigger channel SHALL keep the pin, because the rig's function is synchronising
  cameras and the display is an aid to operating it
