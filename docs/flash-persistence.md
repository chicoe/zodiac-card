# Flash Persistence on RP2040 (dual-core Arduino)

Saving variables to flash so they survive power cycles.

## The problem

The RP2040 has two cores. Writing to flash requires disabling XIP (execute-in-place), which means **neither core can be executing from flash** during the erase/write. Standard solutions like the Pico SDK's `multicore_lockout` use the SIO FIFO to pause the other core, but this conflicts with how the Earle Philhower Arduino core uses the FIFO for USB stack communication — causing hangs at boot or on first save.

## The solution: cooperative pause

Instead of using multicore lockout, Core 0 is paused cooperatively using two `volatile bool` flags:

1. Core 1 sets a `saveActive` flag and waits for Core 0 to ack
2. Core 0 catches this flag inside an ISR or function already marked `__not_in_flash_func` (in RAM)
3. Core 0 sets an ack flag and spins with `tight_loop_contents()` — all in RAM
4. Core 1 sees the ack and erases/writes flash (both cores now executing from RAM only)
5. Core 1 clears `saveActive` — Core 0 resumes

The key requirement: Core 0's pause point **must be in a function marked `__not_in_flash_func`**, otherwise it will fault when XIP is disabled. A high-frequency ISR (like an audio sample callback) is ideal since Core 0 enters it within one period of the signal.

The flash erase takes ~50ms, causing a brief interruption of whatever Core 0 was doing (accepted).

## Flash layout

Use the last 4KB sector of flash — it won't be overwritten by firmware updates unless the binary grows very large.

```
uint32_t offset = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;
```

Start the layout with a magic byte to detect uninitialised flash on first boot. If the magic byte doesn't match, load defaults instead.

```
[0]     magic byte (pick any non-0xFF constant)
[1...]  your variables, packed as bytes
```

Reading at boot uses the XIP memory map — no library needed:
```cpp
const uint8_t *p = (const uint8_t *)(XIP_BASE + offset);
if (p[0] == MAGIC) { /* read p[1], p[2], ... */ }
```

Writing uses `flash_range_erase` + `flash_range_program` from `hardware/flash.h`. The write function must be `__not_in_flash_func`. Build a `FLASH_SECTOR_SIZE` (4096 byte) buffer filled with `0xFF`, write your data into it, then erase and program the sector.

## Debounce

Avoid writing on every change — flash has limited write cycles (~100k) and each write causes a ~50ms pause. A 1-second debounce works well: reset a deadline timer on each change, fire the actual write only when the deadline passes with no further changes.

```cpp
void scheduleSave() {
    saveDeadline = time_us_64() + 1000000ULL;
    saveDirty = true;
}

// in your Core 1 loop:
if (saveDirty && time_us_64() >= saveDeadline) {
    saveDirty = false;
    saveToFlash();
}
```

## UI slider tip

If a slider sends a value continuously while dragging (e.g. HTML `input[type=range]` with `on:input`), send the MIDI/serial message only on release (`on:change`) to avoid triggering saves mid-drag. Keep a local variable for the display so it still updates in real time while dragging.
