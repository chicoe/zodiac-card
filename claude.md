# Zodiac Card — Development Context

## Project Overview

Zodiac Card (formerly "Chains Sequencer") is a Markov chain sequencer for the Music Thing Modular Workshop System Computer Module. It consists of RP2040 firmware and a SvelteKit web UI that communicate via USB MIDI.

Nodes hold pitches, links set transition probabilities. Two independent chains traverse the network to generate scale-quantized melodies. The web UI displays a force-directed graph with an amber CRT visual theme.

## Architecture

### Dual-Core RP2040

- **Core 0**: `ProcessSample()` at 24kHz — audio synthesis, clock processing, chain traversal, LED output, pending edit consumption
- **Core 1**: `MIDICore()` — USB MIDI CC/SysEx communication with browser UI, reads graph state via seqlock

Cross-core synchronization uses a **seqlock pattern** (`graphSeq`): Core 0 increments before and after writes (odd = writing), Core 1 retries reads if the sequence number changed or is odd.

### Web UI (SvelteKit 5)

- **Static adapter** — builds to `web/build/` for hosting or embedding
- **d3-force** for force-directed graph layout
- **WebMIDI API** for real-time communication (Chrome/Chromium only, requires SysEx permission)
- **Monaspace Krypton** font via `@fontsource/monaspace-krypton`


## Key Constants

| Constant | Value | Notes |
|----------|-------|-------|
| MAX_NODES | 16 | Oldest auto-deleted when full |
| MAX_LINKS | 8 | Per node |
| SAMPLE_RATE | 24000 | ProcessSample rate (192kHz ADC / 8 DMA) |
| Node IDs | 0–126 | Recycled, MIDI-safe 7-bit |
| Pitch range | MIDI 36–96 | C2–C7, ~5 octaves |
| Gate duty | 50% | Of step interval |
| Scales | 8 | Chromatic, Major(default), Minor, Pentatonic, Min Penta, Blues, Dorian, Mixolydian |
| Auto interval | 0.25s–8s | Default 4s (CC value 61 ≈ 4s) |

## MIDI Protocol

Manufacturer ID: `0x7D` (prototyping/private use)

### CC Status (Firmware → Browser)

| CC# | Constant | Data |
|-----|----------|------|
| 20 | CC_CURRENT_NODE | Chain 1 node ID (127 = none) |
| 21 | CC_NODE_COUNT | Total node count |
| 22 | CC_KNOB_MAIN | Knob Main (7-bit, >>5 from 12-bit) |
| 23 | CC_KNOB_X | Knob X (7-bit) |
| 24 | CC_KNOB_Y | Knob Y (7-bit) |
| 25 | CC_SWITCH | Switch (0=up, 1=mid, 2=down) |
| 26 | CC_CURRENT_NODE2 | Chain 2 node ID (127 = none) |
| 27 | CC_SCALE | Scale index (0–7) |
| 28 | CC_AUTO_INTERVAL | Auto interval (0–127 → 0.25s–8s) |

CC values are sent on-change with a deadband of ±1 for knobs. Knobs are polled at ~20Hz. All CCs are force-resent every 2 seconds.

### SysEx Messages

| Msg ID | Constant | Direction | Format |
|--------|----------|-----------|--------|
| 0x02 | MSG_NODE | FW→Browser | `F0 7D 02 <idx> <total> <nodeId> <pitch_hi> <pitch_lo> <linkCount> [<targetId> <wt_hi> <wt_lo>]×N F7` |
| 0x10 | MSG_DELETE_NODE | Browser→FW | `F0 7D 10 <nodeId> F7` |
| 0x11 | MSG_DELETE_LINK | Browser→FW | `F0 7D 11 <srcId> <tgtId> F7` |
| 0x12 | MSG_ADD_LINK | Browser→FW | `F0 7D 12 <srcId> <tgtId> <wt_hi> <wt_lo> F7` |
| 0x13 | MSG_PULL_REQUEST | Browser→FW | `F0 7D 13 F7` |
| 0x14 | MSG_SET_SCALE | Browser→FW | `F0 7D 14 <scaleIdx> F7` |
| 0x15 | MSG_ADD_NODE | Browser→FW | `F0 7D 15 <pitch_hi> <pitch_lo> <probability> F7` |
| 0x16 | MSG_SET_AUTO_INT | Browser→FW | `F0 7D 16 <val_0_127> F7` |

Node sync is incremental: one unsynced node per ~10ms via SysEx. `fullResyncRequested` flag marks all nodes as unsynced. Link targets are transmitted as node IDs (not array indices).

### MIDI Note On (Browser → Firmware)

`90 <note> <velocity>` — note 36–96 → pitch 0–4095, velocity → probability.

## Graph Mechanics

### Node Creation (`addNode()`)
1. If at capacity (16), delete oldest node (lowest `birthOrder`)
2. Assign recycled ID via `allocateNodeId()` (smallest unused 0–126)
3. Assign monotonically increasing `birthOrder` (uint32_t, never sent over MIDI)
4. Primary link: youngest existing node → new node (random weight)
5. Extra random links: 0–4 based on probability parameter (0%=0, 25%=1, 50%=2, 75%=3, 99%=4)
6. All link weights are random 1–4095
7. First node: no initial links (chains teleport to oldest when stuck)

### Node Deletion (`deleteNodeBody()`)
1. Remove all links pointing to deleted node
2. Adjust link target indices for compaction
3. Shift nodes array to fill gap
4. Fix `currentNode`, `currentNode2`, `lastCreatedNode` indices
5. Mark all nodes as unsynced (indices changed after compaction)

### Orphan Repair (`cleanupStrayNodesBody()`)
- Nodes with no incoming links: youngest node gets a link to them
- Nodes with no outgoing links: they get a link to the oldest node

### Chain Traversal (`advanceChain()`)
- Weighted random selection from outgoing links
- No outgoing links: teleport to oldest node (lowest `birthOrder`)

### Pending Edit System
Core 1 (MIDI) writes to `pendingEdit` struct, Core 0 consumes it at start of each `ProcessSample()`. Only one pending edit at a time (single slot).

## Controls Mapping

| Control | Firmware Variable | Function |
|---------|-------------------|----------|
| Knob Main / CV In 1 | `KnobVal(Knob::Main)` | Link probability for new nodes |
| Knob X | `KnobVal(Knob::X)` | Internal clock speed (both chains when no ext clock) |
| Knob Y / CV In 2 | `KnobVal(Knob::Y)` | Pitch range (narrow at 0, full at 4095) |
| Switch Up | `Switch::Up` | Auto mode (timer creates nodes) |
| Switch Mid | `Switch::Middle` | Normal play |
| Switch Down (flick) | `Switch::Down` edge | Cycle to next scale |

### I/O

| I/O | Function |
|-----|----------|
| Audio Out L/R | Chain 1/2 sine oscillator |
| CV Out 1/2 | Chain 1/2 V/Oct pitch (`CVOut1MIDINote`) |
| Pulse In 1/2 | Chain 1/2 external clock |
| Pulse Out 1/2 | Chain 1/2 gate |
| CV In 1 | Probability CV (replaces Knob Main when connected) |
| CV In 2 | Pitch CV (replaces random pitch when connected) |

## Web UI Design

### Visual Theme
- **CRT amber/yellow** — primary color `#ffcc00`, background `#050500`
- Chain 1: green `#00e5a0`, Chain 2: cyan `#00ccff`
- Interactive elements (keyboard, scale selector): red-amber `#ff9955`
- Subtle scan lines (6% opacity), gentle vignette (15% at edges), minimal flicker
- Font: Monaspace Krypton (monospace)

### Layout
- **Header bar**: Title "ZODIAC CARD | BETA v0.2", MIDI connect button, device selector, pull button
- **Left sidebar** (flex-wrap column, `width: max-content`): Collapsible sections (ABOUT, CONTROLS, INPUTS, OUTPUTS) using `<details>`/`<summary>`, .UF2 download link, STATUS panel, NODE LIST. Uses ResizeObserver to dynamically track width.
- **Main area**: SVG force-directed graph with grid background, radar circles, crosshair, corner brackets
- **Bottom bar**: Scale selector, auto interval slider, piano keyboard (2 rows: black keys top, white keys bottom with gaps at E-F and B-C boundaries), disclaimer with credit links
- **Context menu**: Right-click nodes for CONNECT TO / DELETE, right-click links for DELETE

### Animation System
- **Expanding rings**: Fast (500ms) on chain change, slow (1200ms) ping every 1.8s
- **Traveling dots**: 120ms ease-out-cubic between nodes on chain advance
- Links rendered as dashed lines (`stroke-dasharray: 4 4`)
- Orphan nodes (no incoming links) rendered at 40% opacity

### MIDI Connection
- Auto-connects to device named "Chains" on page load
- Falls back to single-device auto-connect
- Device selector dropdown for manual selection
- Requests full graph pull on connect

### Stores (`stores.js`)
All state is in Svelte writable stores: `graphNodes`, `graphLinks`, `currentNodeId`, `currentNode2Id`, `nodeCount`, `knobMain`, `knobX`, `knobY`, `switchState`, `midiConnected`, `deviceNames`, `selectedDevice`, `scaleIndex` (default 1 = Major), `autoInterval` (default 61 ≈ 4s).

### Incremental Sync (`midi.js`)
- `internalNodes` Map keyed by node ID stores pitch + links
- On each MSG_NODE SysEx, updates the map and calls `rebuildGraph()`
- When CC_NODE_COUNT reports fewer nodes than the map, evicts lowest-ID entries
- `requestPull()` sends MSG_PULL_REQUEST to get full resync

## Build Commands

### Firmware
1. Open `zodiac_card.ino` in Arduino IDE
2. Select board: Raspberry Pi Pico
3. Set USB Stack to "Pico SDK" (Tools menu)
4. Requires [Earle Philhower RP2040 core](https://github.com/earlephilhower/arduino-pico) v5.5.0+
5. Build & upload

### Web UI
```bash
cd zodiac-card/web
npm install
npm run dev      # development at localhost:5173
npm run build    # static output in web/build/
```

## Design Decisions

- **Recycled node IDs** (0–126): Prevents 7-bit overflow; smallest unused ID is reused
- **birthOrder (uint32_t)**: Separate from ID, used for age tracking (youngest/oldest), never sent over MIDI
- **Seqlock not mutex**: Non-blocking reads on Core 1 — never stalls MIDI
- **Single pending edit slot**: Simple, one-edit-at-a-time from UI → firmware
- **Scale quantization at playback**: Pitch stored as raw 0–4095, quantized only during synthesis — scale changes retroactively affect all nodes
- **Auto-evict oldest**: When graph is full, oldest node (lowest birthOrder) is deleted automatically
- **Static SvelteKit build**: No server needed, output is pure HTML/JS/CSS
- **Two-row keyboard layout**: Black keys on top row with explicit gaps after D# and A# to match piano layout

## Known Considerations

- WebMIDI requires Chrome/Chromium (not supported in Firefox/Safari)
- WebMIDI SysEx requires user gesture for permission
- USB device name in firmware is still "Zodiac Card" (`beginMIDI("Zodiac Card")`) — MIDI auto-connect searches for "Zodiac" substring
- The .UF2 download link in the sidebar is a placeholder (`href="#"`)
- `vite.config.js.timestamp-*` files in web/ are build artifacts that should be gitignored
