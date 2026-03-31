# Zodiac Card


A **Markov chain sequencer** for the [Music Thing Modular Workshop System Computer Module](https://www.musicthing.co.uk/workshopsystem/). Nodes in a probabilistic graph are traversed at each clock step, generating scale-quantized pitched output based on weighted transitions. Two independent chains can traverse the same graph simultaneously.

**Disclaimer:** 
This is a prototype built late at night, with heavy LLM usage and barely any testing, its use as an example or in production is highly discouraged. That being said, it is open source so do whatever you want.


## How It Works

Build a graph of pitch nodes connected by probabilistic links. Each clock tick, the sequencer randomly walks to the next node based on outgoing link weights — creating evolving, non-repeating melodic sequences. New nodes are linked to the most recently created node, with extra random links controlled by the probability knob.

## Controls

| Control | Function |
|---------|----------|
| **Knob Main** / CV 1 | Link probability for new nodes (0–4 extra random links) |
| **Knob X** | Clock speed for internal clock |
| **Knob Y** / CV 2 | Pitch range for new nodes (narrow → wide, centered on C4) |
| **Switch Up** | Auto mode — creates a node on a timer (interval configurable) |
| **Switch Mid** | Normal play (scale-quantized) |
| **Switch Down (flick)** | Cycle to next scale |

## I/O

| I/O | Signal |
|-----|--------|
| **Audio Out L** | Chain 1 pitched sine oscillator |
| **Audio Out R** | Chain 2 pitched sine oscillator |
| **CV Out 1** | Chain 1 pitch (1V/Oct) |
| **CV Out 2** | Chain 2 pitch (1V/Oct) |
| **Pulse In 1** | Chain 1 external clock |
| **Pulse In 2** | Chain 2 external clock |
| **Pulse Out 1** | Chain 1 gate (50% duty cycle) |
| **Pulse Out 2** | Chain 2 gate (50% duty cycle) |
| **CV In 1** | Probability CV (replaces Knob Main) |
| **CV In 2** | Pitch CV (replaces Knob Y random pitch) |

## Dual Chains

Chain 1 uses Knob X as internal clock, or Pulse In 1 for external clock. Chain 2 uses Pulse In 2 for external clock (or mirrors Chain 1's speed when no external clock). Both chains traverse the same graph independently, producing separate audio, CV, and gate outputs.

## Scales

Eight built-in scales, cycled by flicking the switch down:

Chromatic · Major (default) · Minor · Pentatonic · Minor Pentatonic · Blues · Dorian · Mixolydian

## Auto Mode

With the switch held up, a new node is created automatically on a configurable timer (0.25s–8s, default 4s). The pitch is random within the range set by Knob Y (or CV In 2), and link probability follows Knob Main (or CV In 1).

## Node Management

- **Max 16 nodes** — when full, the oldest node is automatically deleted to make room
- Node IDs are recycled in the 0–126 range (MIDI-safe 7-bit)
- Orphan repair: nodes without incoming links get a link from the youngest node; nodes without outgoing links get a link to the oldest node

## LEDs

LEDs 0–5 show the current Chain 1 node ID in binary (6 bits = IDs 0–63). Brighter when the gate is active.

## Web Interface

A SvelteKit browser UI connects via USB MIDI (WebMIDI API, Chrome/Chromium only) and displays:

- **Force-directed graph** visualization of all nodes and links
- **Real-time chain traversal** with animated traveling dots and expanding rings
- **Collapsible sidebar** with status, controls, inputs/outputs documentation, and node list
- **Piano keyboard** for creating nodes at specific pitches via MIDI Note On
- **Scale selector** and **auto interval slider** in the bottom bar
- **Context menu** on nodes/links for connect and delete operations
- **CRT amber theme** with Monaspace Krypton font

### Running the Web UI

```bash
cd zodiac-card/web
npm install
npm run dev
```

Open `http://localhost:5173` in Chrome and click **Connect MIDI** with the card plugged in via USB.

## Build

### Firmware

- **Arduino IDE** with [Earle Philhower RP2040 core](https://github.com/earlephilhower/arduino-pico) v5.5.0+
- **USB Stack**: Set to **"Pico SDK"** in Tools menu
- Board: Raspberry Pi Pico

### Web UI

```bash
cd zodiac-card/web
npm install
npm run build    # static output in web/build/
```

## MIDI Protocol

Manufacturer ID: `0x7D` (prototyping/private use)

### CC Status (Firmware → Browser, on change)

| CC# | Data |
|-----|------|
| 20 | Chain 1 current node ID (127 = none) |
| 21 | Node count |
| 22 | Knob Main (7-bit) |
| 23 | Knob X (7-bit) |
| 24 | Knob Y (7-bit) |
| 25 | Switch state (0=up, 1=mid, 2=down) |
| 26 | Chain 2 current node ID (127 = none) |
| 27 | Current scale index (0–7) |
| 28 | Auto interval (0–127, maps to 0.25s–8s) |

### SysEx Messages

#### Node Data (Firmware → Browser, incremental sync)

```
F0 7D 02 <nodeIdx> <total> <nodeId> <pitch_hi> <pitch_lo> <linkCount>
  [<targetId> <weight_hi> <weight_lo>] × linkCount
F7
```

Link targets are sent as node IDs (not array indices).

#### Pull Request (Browser → Firmware)

```
F0 7D 13 F7
```

#### Delete Node (Browser → Firmware)

```
F0 7D 10 <nodeId> F7
```

#### Delete Link (Browser → Firmware)

```
F0 7D 11 <sourceId> <targetId> F7
```

#### Add Link (Browser → Firmware)

```
F0 7D 12 <sourceId> <targetId> <weight_hi> <weight_lo> F7
```

#### Set Scale (Browser → Firmware)

```
F0 7D 14 <scaleIndex> F7
```

#### Add Node (Browser → Firmware)

```
F0 7D 15 <pitch_hi> <pitch_lo> <probability> F7
```

#### Set Auto Interval (Browser → Firmware)

```
F0 7D 16 <value_0_127> F7
```

### MIDI Note On (Browser → Firmware)

```
90 <note> <velocity>
```

Note 36–96 maps to pitch 0–4095. Velocity maps to probability.
