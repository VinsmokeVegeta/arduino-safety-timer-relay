# Password-Protected Programmable Safety Timer & Relay Controller

A fail-safe embedded controller on Arduino Uno (ATmega328P) written in Embedded C style (fixed `char` buffers, `stdint.h` types, zero dynamic memory, and a custom 4x4 matrix keypad scanner). It enforces dual authentication (4-digit PIN + physical safety interlock switch), non-blocking `millis()` timing, and a hardware-interrupt (`INT0`) Emergency Stop.

## Hardware Architecture & Pinout

| Peripheral | Arduino Uno Pin | Interface / Purpose |
| :--- | :--- | :--- |
| 16x2 I2C LCD (`0x27`) | `A4` (SDA), `A5` (SCL) | Real-time FSM state, masked PIN, and countdown display |
| 4x4 Matrix Keypad | `D12–D9` (Rows), `D8–D5` (Cols) | Custom C row-column scanner for PIN (`1234`), delay (`1–99 s`), reset |
| Emergency Stop Button | `D2` (`INT0`, `INPUT_PULLUP`) | `FALLING`-edge external interrupt for immediate relay shutoff |
| Safety Toggle Switch | `D3` (`INPUT_PULLUP`, Active LOW) | Physical hardware interlock required to arm and actuate |
| Relay Module + Load LED | `D4` (`IN`), `COM/NO` contacts | Energized for a fixed 3-second pulse at countdown expiry |

## 4-State Finite State Machine (FSM)

```text
  [Power ON]
      |
      v
  +-------+   Valid PIN (1234) + Safety Switch ON   +-------+
  | IDLE  | --------------------------------------> | ARMED |
  +-------+                                         +---+---+
      ^                                                 |
      | Safety Switch OFF + '#' Reset                   | Delay (1-99 s) + '#'
      |                                                 v
  +-------+   E-Stop (INT0) OR Safety Switch OFF   +-----------+
  | ABORT | <------------------------------------- | COUNTDOWN |
  +-------+                                        +-----+-----+
      ^                                                  | T = 0 (3 s Relay Pulse)
      +------------------ Auto Return to IDLE -----------+ 

````
Live : https://wokwi.com/projects/481324495754224641
Demo Screenshots : (simulation_proof)
