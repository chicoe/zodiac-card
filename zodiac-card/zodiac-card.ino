/*
  Chains Sequencer — Music Thing Modular Workshop System
  Markov chain sequencer: nodes with weighted links form a probabilistic graph.
  Two independent chains traverse the same graph with separate clocks.

  Core 0: ProcessSample() at 24kHz — audio, clock, chain traversal, LEDs.
  Core 1: MIDICore() — USB MIDI CC/SysEx to browser UI.

  Controls:
    Knob X:   Clock control, role depends on what's patched into Pulse In 1/2:
                - no clock in:   internal clock speed for both chains
                - Pulse 1 only:  divide/multiply chain 1's clock to drive chain 2
                - Pulse 2 only:  divide/multiply chain 2's clock to drive chain 1
                - both patched:  auto-generation interval (only once the knob is moved)
              Divide/multiply in 0.5 ratio steps: …÷2, ÷1.5, ×1, ×1.5, ×2… (center = ×1).
    Knob Main / CV 1:  Link probability for new nodes
    Knob Y / CV 2:     Pitch range for new nodes
    Switch Up:    Auto mode — creates a new node every N seconds (configurable)
    Switch Mid:   Normal operation (scale-quantized)
    Switch Down flick:  Cycle to next scale
    MIDI Note On: Create node (note=pitch, velocity=probability)

  Chain 1: Pulse 1 clock (if connected), else Knob X internal / derived → Audio 1, CV 1, Pulse 1
  Chain 2: Pulse 2 clock (if connected), else Knob X internal / derived → Audio 2, CV 2, Pulse 2

  CC status (firmware → browser):
    CC 20: chain 1 current node ID (127 = none)
    CC 21: node count
    CC 22-24: knobs (7-bit)
    CC 25: switch state
    CC 26: chain 2 current node ID (127 = none)
    CC 27: current scale index
    CC 28: auto interval (0–127, maps to 0.25–8s)
*/

#include "ComputerCard.h"
#include "WebInterface.h"
#include <math.h>
#include "hardware/structs/rosc.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

// ═══════════════════════════════════════════════════════════════════
// Constants
// ═══════════════════════════════════════════════════════════════════
static constexpr int      MAX_NODES            = 64;  // hard array cap (indices must fit int8_t, IDs 0–126)
static constexpr int      DEFAULT_MAX_NODES    = 16;  // default runtime cap (UI slider: 2..MAX_NODES)
static constexpr int      REPAIR_SLICE         = 8;   // nodes processed per ISR tick during sliced graph ops.
                                                      // Kept small: USB preemption can add ~25µs to any sample
                                                      // (IRQ priorities are untouchable — see note in setup()),
                                                      // so real work must leave that much headroom under ~62µs.
static constexpr int      MAX_LINKS            = 8;
static constexpr uint32_t SAMPLE_RATE          = 24000;   // ProcessSample called at 24kHz (192kHz ADC / 8 DMA transfers)
static constexpr uint32_t MIN_STEP_INTERVAL    = 2400;    // 50ms  (~20 Hz)
static constexpr uint32_t MAX_STEP_INTERVAL    = 96000;   // 2s    (~0.5 Hz)
static constexpr uint32_t DEFAULT_STEP_INTERVAL= 24000;   // 500ms (~2 Hz)

// Clock divide/multiply — Knob X when exactly one clock input is patched.
// Quantized in 0.5 ratio steps: …÷2, ÷1.5, ×1, ×1.5, ×2… (unity at knob center).
// Level n (signed half-steps from unity): n>=0 multiplies clock by (2+n)/2, n<0 divides by (2-n)/2.
static constexpr int32_t CLOCK_MULT_STEPS = 6;   // max multiply = 1 + 0.5*6 = ×4   ← tweak after testing
static constexpr int32_t CLOCK_DIV_STEPS  = 6;   // max divide   = 1 + 0.5*6 = ÷4
static constexpr int32_t CLOCK_KNOB_MOVE_THRESH = 64;  // Knob X motion (of 4095) that grabs auto-interval when both clocks patched
static constexpr uint32_t CLOCK_BOTH_ARM_SAMPLES = 12000; // both clocks must read connected for 0.5s straight before Knob X can grab auto-interval (jack-detect can flicker on floating inputs)
static constexpr int32_t  CLOCK_KNOB_JITTER      = 16;    // ignore Knob X wiggle below this while driving auto-interval

// Clock is treated as 2 pulses per quarter note (Pocket Operator / Korg Volca standard).
// UI BPM is musical BPM = steps-per-minute / CLOCK_PPQN.
static constexpr uint32_t CLOCK_PPQN = 2;

static constexpr uint32_t GATE_LENGTH_PCT      = 50;      // gate = 50% of step
static constexpr uint32_t SINE_TABLE_SIZE      = 256;
static constexpr int      NUM_SCALES           = 8;
static constexpr int      NUM_WAVEFORMS        = 6;
static constexpr int      BASS_THRESHOLD       = 47;  // MIDI notes above this get octave-shifted down in bass mode

// Flash persistence — last 4KB sector
// Layout: [0] magic | [1] scaleIndex | [2-5] autoInterval (uint32_t) | [6] waveform1 | [7] bassMode1 | [8] waveform2 | [9] bassMode2 | [10] maxNodes
static constexpr uint8_t  FLASH_MAGIC         = 0xAC;
static constexpr uint32_t FLASH_TARGET_OFFSET = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;
// Each flash write pauses Core 0 for ~50ms — enforce a hard minimum gap so
// noise-triggered save requests can never pile up into repeated stalls.
static constexpr uint64_t FLASH_MIN_WRITE_SPACING_US = 10ULL * 1000000ULL;  // ≥10s between writes

// ═══════════════════════════════════════════════════════════════════
// Scale definitions — intervals from root (in semitones)
// ═══════════════════════════════════════════════════════════════════
struct Scale {
    const char *name;
    uint8_t notes[12];
    uint8_t count;
};

static const Scale SCALES[NUM_SCALES] = {
    {"Chromatic",   {0,1,2,3,4,5,6,7,8,9,10,11}, 12},
    {"Major",       {0,2,4,5,7,9,11,0,0,0,0,0},  7},
    {"Minor",       {0,2,3,5,7,8,10,0,0,0,0,0},  7},
    {"Pentatonic",  {0,2,4,7,9,0,0,0,0,0,0,0},   5},
    {"Min Penta",   {0,3,5,7,10,0,0,0,0,0,0,0},  5},
    {"Blues",        {0,3,5,6,7,10,0,0,0,0,0,0},  6},
    {"Dorian",      {0,2,3,5,7,9,10,0,0,0,0,0},  7},
    {"Mixolydian",  {0,2,4,5,7,9,10,0,0,0,0,0},  7},
};

// ═══════════════════════════════════════════════════════════════════
// Sine lookup table — generated at startup
// ═══════════════════════════════════════════════════════════════════
static int16_t sineTable[SINE_TABLE_SIZE];

static void initSineTable() {
    for (int i = 0; i < (int)SINE_TABLE_SIZE; i++) {
        sineTable[i] = (int16_t)(sinf(2.0f * 3.14159265f * i / SINE_TABLE_SIZE) * 2047.0f);
    }
}

// ═══════════════════════════════════════════════════════════════════
// LFSR random number generator
// ═══════════════════════════════════════════════════════════════════
static uint32_t rngState = 12345;

static void seedRNG() {
    rngState = 0;
    for (int i = 0; i < 32; i++) {
        rngState = (rngState << 1) | (rosc_hw->randombit & 1);
    }
    if (rngState == 0) rngState = 12345;
}

static uint32_t __not_in_flash_func(fastRandom)() {
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return rngState;
}

// ═══════════════════════════════════════════════════════════════════
// Data structures
// ═══════════════════════════════════════════════════════════════════
struct Link {
    uint8_t  target;   // index of target node
    uint16_t weight;   // probability weight (0–4095)
};

struct Node {
    uint8_t  id;                // recycled 0–126, MIDI-safe (7-bit)
    uint32_t birthOrder;        // ever-increasing for age tracking (never sent over MIDI)
    int32_t  pitch;             // 0–4095
    Link     links[MAX_LINKS];
    uint8_t  linkCount;
    bool     active;
    bool     synced;            // for MIDI sync
};

// ═══════════════════════════════════════════════════════════════════
// ZodiacCard — main Markov chain sequencer class
// ═══════════════════════════════════════════════════════════════════
class ZodiacCard : public WebInterfaceComputerCard {
public:
    // --- Graph state ---
    Node nodes[MAX_NODES];
    volatile int   nodeCount   = 0;
    volatile uint8_t maxNodes  = DEFAULT_MAX_NODES;  // runtime node cap (2..MAX_NODES), set from UI, persisted
    volatile int8_t currentNode = -1;     // -1 = no nodes (array index)
    int8_t  lastCreatedNode = -1;          // last node added (array index)
    volatile uint32_t graphSeq = 0;       // seqlock for cross-core sync
    uint32_t nextBirthOrder = 0;           // monotonically increasing age counter

    // Allocate the smallest unused node ID in 0–126 range
    uint8_t __not_in_flash_func(allocateNodeId)() {
        bool used[127] = {};
        for (int i = 0; i < nodeCount; i++) {
            if (nodes[i].id < 127) used[nodes[i].id] = true;
        }
        for (uint8_t id = 0; id < 127; id++) {
            if (!used[id]) return id;
        }
        return 0;  // fallback (shouldn't happen while MAX_NODES ≤ 127)
    }

    // --- Chain 1 state ---
    uint32_t stepInterval = DEFAULT_STEP_INTERVAL;
    uint32_t stepCounter  = 0;
    bool     receivedFirstPulse = false;
    bool     gateActive  = false;
    uint32_t gateTimer   = 0;
    uint32_t gateDuration = 0;
    uint32_t sinePhase      = 0;
    uint32_t phaseIncrement = 0;
    uint32_t sinePhase1b    = 0;   // supersaw detuned osc +
    uint32_t sinePhase1c    = 0;   // supersaw detuned osc -

    // --- Chain 2 state (independent traversal, same graph) ---
    volatile int8_t  currentNode2 = -1;
    uint32_t stepInterval2 = DEFAULT_STEP_INTERVAL;
    uint32_t stepCounter2  = 0;
    bool     receivedFirstPulse2 = false;
    bool     gateActive2  = false;
    uint32_t gateTimer2   = 0;
    uint32_t gateDuration2 = 0;
    uint32_t sinePhase2     = 0;
    uint32_t phaseIncrement2 = 0;
    uint32_t sinePhase2b    = 0;
    uint32_t sinePhase2c    = 0;

    // --- Switch gesture detection ---
    bool     prevSwitchUp     = true;   // init true to avoid false edge on boot
    bool     prevSwitchDown   = true;   // init true to avoid false edge on boot

    // --- Auto mode (Switch Up) ---
    bool     autoMode         = false;
    bool     autoMakeNode     = false;
    uint32_t autoTimer        = 0;
    volatile uint32_t autoInterval = 4 * SAMPLE_RATE;  // default 4s (96000 samples)

    // --- Knob X → auto-interval takeover (when both clock inputs are patched) ---
    bool     bothClockPrev    = false;  // were both clocks connected last sample
    bool     bothClockEngaged = false;  // has Knob X moved enough to grab the auto-interval
    int32_t  bothClockKnobRef = 0;      // Knob X value captured when both-clock mode began
    uint32_t bothClockStableCnt = 0;    // consecutive samples both clocks have read connected (debounce)
    int32_t  bothClockLastKx  = -1000;  // last Knob X value applied to auto-interval (jitter gate)

    // --- Deferred graph ops: at most ONE heavy op (evict/repair/create) per
    // ISR sample, so ProcessSample never blows its ~41.6µs budget. Overruns
    // starve the ADC DMA and permanently freeze all knob/switch reads. ---
    int32_t  pendingCreatePitch = -1;   // >= 0: a node creation is queued (single slot)
    uint16_t pendingCreateProb  = 0;
    bool     needCleanup        = false;  // run orphan repair on the next sample

    // Deleted-node IDs queued for UI notification (Core 0 writes, Core 1 drains)
    volatile uint8_t deletedIds[64];
    volatile uint8_t deletedHead = 0, deletedTail = 0;

    // Sliced orphan repair: the full O(nodes·links) pass costs ~45µs at 64
    // nodes — over budget in one sample — so it runs 16 nodes per tick.
    // Any graph edit sets needCleanup, which (re)starts the repair from scratch.
    uint8_t repairPhase    = 0;   // 0 idle, 1 scanning incoming links, 2 applying fixes
    uint8_t repairCursor   = 0;
    int8_t  repairYoungest = -1;
    int8_t  repairOldest   = -1;
    bool    repairIncoming[MAX_NODES] = {};

    // Sliced delete link-fixup: after a swap-delete, the scan that removes
    // links to the deleted slot / retargets links to the moved node runs 16
    // nodes per tick (the full scan is ~500 link-touches on a dense 64-node
    // graph ≈ 50µs — over budget in one sample). Between slices every link
    // is index-valid; worst transient is a chain following a stale link once.
    int8_t  delFixSlot   = -1;    // slot the deleted node occupied (moved node now lives there); -1 = idle
    uint8_t delFixLast   = 0;     // old last index (now out of range; links there → delFixSlot)
    uint8_t delFixCursor = 0;

    // --- Quantization ---
    bool quantize = true;
    bool useChromatic = false;
    volatile uint8_t scaleIndex = 1;  // index into SCALES[], default Major
    volatile uint8_t waveformIndex1 = 0;  // chain 1: 0=sine 1=triangle 2=square 3=pulse 4=sawtooth 5=supersaw
    volatile bool    bassMode1 = false;   // chain 1 bass mode: fold notes above BASS_THRESHOLD down by octaves
    volatile uint8_t waveformIndex2 = 0;  // chain 2 waveform
    volatile bool    bassMode2 = false;   // chain 2 bass mode

    // --- MIDI CC numbers ---
    static constexpr uint8_t CC_CURRENT_NODE = 20;
    static constexpr uint8_t CC_NODE_COUNT   = 21;
    static constexpr uint8_t CC_KNOB_MAIN    = 22;
    static constexpr uint8_t CC_KNOB_X       = 23;
    static constexpr uint8_t CC_KNOB_Y       = 24;
    static constexpr uint8_t CC_SWITCH       = 25;
    static constexpr uint8_t CC_CURRENT_NODE2 = 26;
    static constexpr uint8_t CC_SCALE         = 27;
    static constexpr uint8_t CC_AUTO_INTERVAL  = 28;
    static constexpr uint8_t CC_WAVEFORM      = 29;
    static constexpr uint8_t CC_BASS_MODE     = 30;
    static constexpr uint8_t CC_WAVEFORM2     = 31;
    static constexpr uint8_t CC_BASS_MODE2    = 33;  // skip 32 (bank select LSB)
    static constexpr uint8_t CC_MAX_NODES     = 34;

    // --- SysEx message types ---
    static constexpr uint8_t MSG_NODE         = 0x02;
    static constexpr uint8_t MSG_CLOCK_STATUS = 0x03;  // FW→browser: per-chain BPM + divide/multiply factor
    static constexpr uint8_t MSG_PERF         = 0x04;  // FW→browser: worst-case ProcessSample µs (diagnostics)
    static constexpr uint8_t MSG_NODE_DELETED = 0x05;  // FW→browser: node ID removed from the graph
    static constexpr uint8_t MSG_ROSTER       = 0x06;  // FW→browser: full list of live node IDs (safety net)
    static constexpr uint8_t MSG_DELETE_NODE   = 0x10;
    static constexpr uint8_t MSG_DELETE_LINK   = 0x11;
    static constexpr uint8_t MSG_ADD_LINK      = 0x12;
    static constexpr uint8_t MSG_PULL_REQUEST  = 0x13;
    static constexpr uint8_t MSG_SET_SCALE     = 0x14;
    static constexpr uint8_t MSG_ADD_NODE      = 0x15;
    static constexpr uint8_t MSG_SET_AUTO_INT   = 0x16;
    static constexpr uint8_t MSG_SET_WAVEFORM   = 0x17;
    static constexpr uint8_t MSG_SET_BASS_MODE  = 0x18;
    static constexpr uint8_t MSG_SET_WAVEFORM2  = 0x19;
    static constexpr uint8_t MSG_SET_BASS_MODE2 = 0x1A;
    static constexpr uint8_t MSG_SET_MAX_NODES  = 0x1B;

    uint8_t midiTxBuf[64];
    absolute_time_t nextMidiServiceTime;
    volatile bool fullResyncRequested = false;

    // --- Pending edits from UI (Core 1 writes, Core 0 reads) ---
    struct PendingEdit {
        uint8_t cmd;        // MSG_DELETE_NODE, MSG_DELETE_LINK, MSG_ADD_LINK, MSG_ADD_NODE, or 0 = none
        uint8_t nodeId;     // target node ID (or MIDI note for MSG_ADD_NODE)
        uint8_t targetId;   // for link ops: the other node ID (or velocity for MSG_ADD_NODE)
        uint16_t weight;    // for add link
    };
    volatile PendingEdit pendingEdit = {0, 0, 0, 0};

    // Set by Core 0 (switch flick), consumed by Core 1
    volatile bool pendingFlashSave = false;

    // ISR timing probe — worst-case ProcessSample duration (µs) since the last
    // MIDI report. Written by Core 0, read+reset by Core 1. Diagnostics for
    // ADC-starvation debugging: budget is ~41.6µs per sample at 24kHz.
    volatile uint32_t isrMaxUs = 0;
    // Per-section maxima: A = graph ops, B = control/clock, C = synthesis+chain2.
    // A structurally tiny section showing a large max = IRQ preemption landed
    // there (wall-clock measurement), not real work.
    volatile uint32_t isrMaxA = 0, isrMaxB = 0, isrMaxC = 0;
    // Core 1 sets flashSaveActive to pause Core 0 inside ProcessSample() (RAM)
    volatile bool flashSaveActive  = false;
    volatile bool core0InPause     = false;

    uint64_t flashSaveDeadline = 0;
    bool     flashSaveDirty    = false;

    // Debounced save — resets the 1-second timer on each call
    void scheduleSave() {
        flashSaveDeadline = time_us_64() + 1000000ULL;
        flashSaveDirty    = true;
    }

    // Writes scaleIndex and autoInterval to the last flash sector.
    // Must be called from Core 1. Pauses Core 0 cooperatively via flashSaveActive:
    // Core 0 spins inside ProcessSample() which is __not_in_flash_func (RAM),
    // making it safe to disable XIP for erase/write.
    void __not_in_flash_func(saveToFlash)() {
        static uint8_t buf[FLASH_SECTOR_SIZE];
        for (int i = 0; i < FLASH_SECTOR_SIZE; i++) buf[i] = 0xFF;
        buf[0] = FLASH_MAGIC;
        buf[1] = (uint8_t)scaleIndex;
        uint32_t ai = autoInterval;
        buf[2] = (uint8_t)(ai);
        buf[3] = (uint8_t)(ai >> 8);
        buf[4] = (uint8_t)(ai >> 16);
        buf[5] = (uint8_t)(ai >> 24);
        buf[6] = (uint8_t)waveformIndex1;
        buf[7] = bassMode1 ? 1u : 0u;
        buf[8] = (uint8_t)waveformIndex2;
        buf[9] = bassMode2 ? 1u : 0u;
        buf[10] = (uint8_t)maxNodes;

        flashSaveActive = true;
        while (!core0InPause) tight_loop_contents();
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
        flash_range_program(FLASH_TARGET_OFFSET, buf, FLASH_SECTOR_SIZE);
        restore_interrupts(ints);
        flashSaveActive = false;
    }

    // ───────────────────────────────────────────────────────────
    // Bass mode: fold notes above BASS_THRESHOLD down by octaves
    // ───────────────────────────────────────────────────────────
    int32_t __not_in_flash_func(applyBassMode)(int32_t midiNote, bool bm) {
        if (!bm) return midiNote;
        while (midiNote > BASS_THRESHOLD) midiNote -= 12;
        return midiNote;
    }

    // ───────────────────────────────────────────────────────────
    // Pitch → sine phase increment (called once per node change)
    // Uses float since it runs infrequently.
    // Maps pitch 0–4095 to ~5 octaves (MIDI 36–96, C2–C7).
    // ───────────────────────────────────────────────────────────
    // Quantize a MIDI note to the current scale
    int32_t quantizeToScale(int32_t midiNote) {
        if (!quantize || useChromatic || scaleIndex >= NUM_SCALES) return midiNote;
        const Scale &s = SCALES[scaleIndex];
        if (s.count >= 12) return midiNote; // chromatic = no change
        int32_t octave = midiNote / 12;
        int32_t degree = midiNote % 12;
        if (degree < 0) { degree += 12; octave--; }
        // Find nearest scale degree
        int32_t best = s.notes[0];
        int32_t bestDist = 99;
        for (int i = 0; i < s.count; i++) {
            int32_t d = degree - (int32_t)s.notes[i];
            if (d < 0) d = -d;
            if (d < bestDist) { bestDist = d; best = s.notes[i]; }
            // also check wrapping
            int32_t d2 = 12 - d;
            if (d2 < bestDist) { bestDist = d2; best = s.notes[i]; }
        }
        return octave * 12 + best;
    }

    // ───────────────────────────────────────────────────────────
    // Random pitch from chromatic range, controlled by knob
    // Knob 0 (left) = 3 semitones around C4, Knob 4095 (right) = full MIDI 36–96
    // Quantization to scale happens only at playback time
    // Returns pitch value 0–4095
    // ───────────────────────────────────────────────────────────
    int32_t __not_in_flash_func(randomPitchInRange)(int32_t knobVal) {
        // Center = MIDI 60 (C4), range = MIDI 36–96 (61 notes)
        static constexpr int CENTER = 60;
        static constexpr int LO = 36;
        static constexpr int HI = 96;
        static constexpr int MAX_HALF = (HI - CENTER) > (CENTER - LO)
            ? (HI - CENTER) : (CENTER - LO);

        // Knob maps: 1 semitone each side (3 total) → full range
        int halfRange = 1 + (int32_t)(knobVal * (MAX_HALF - 1)) / 4095;
        int lo = CENTER - halfRange;
        int hi = CENTER + halfRange;
        if (lo < LO) lo = LO;
        if (hi > HI) hi = HI;

        int midiNote = lo + (int)(fastRandom() % (hi - lo + 1));

        // Convert MIDI note to pitch 0–4095
        int32_t pitch = ((midiNote - 36) * 4096) / 60;
        if (pitch < 0)    pitch = 0;
        if (pitch > 4095) pitch = 4095;
        return pitch;
    }

    uint32_t pitchToPhaseInc(int32_t pitch, bool bm) {
        float semitones;
        if (quantize) {
            int32_t midiNote = 36 + (pitch * 60 + 2048) / 4096;
            if (midiNote < 36) midiNote = 36;
            if (midiNote > 96) midiNote = 96;
            midiNote = (int32_t)quantizeToScale(midiNote);
            midiNote = applyBassMode(midiNote, bm);
            semitones = (float)(midiNote - 69);
        } else {
            float fNote = 36.0f + (pitch * 60.0f) / 4096.0f;
            int32_t midiNote = (int32_t)fNote;
            midiNote = applyBassMode(midiNote, bm);
            semitones = (float)(midiNote - 69);
        }
        float freq = 440.0f * powf(2.0f, semitones / 12.0f);
        return (uint32_t)(freq * 65536.0f * SINE_TABLE_SIZE / SAMPLE_RATE);
    }

    void __not_in_flash_func(updatePhaseIncrement)(int32_t pitch) {
        phaseIncrement = pitchToPhaseInc(pitch, bassMode1);
        sinePhase1b = sinePhase;  // sync supersaw oscillators to main phase
        sinePhase1c = sinePhase;
    }

    void __not_in_flash_func(updatePhaseIncrement2)(int32_t pitch) {
        phaseIncrement2 = pitchToPhaseInc(pitch, bassMode2);
        sinePhase2b = sinePhase2;
        sinePhase2c = sinePhase2;
    }

    // ───────────────────────────────────────────────────────────
    // Waveform rendering — called once per sample from ProcessSample
    // idx: 8-bit phase index (0–255)
    // phaseInc: phase increment for this chain
    // phaseB/phaseC: supersaw extra oscillator phases (persistent state)
    // ───────────────────────────────────────────────────────────
    int16_t __not_in_flash_func(renderWaveform)(uint8_t idx, uint32_t phaseInc,
            uint32_t &phaseB, uint32_t &phaseC, uint8_t waveIdx) {
        // All waveforms target similar perceived loudness.
        // Sine/triangle/supersaw: peak ±2047 (sine-like harmonic content).
        // Square/pulse: ±1024 (−6dB, rich harmonics sound ~2× louder at equal amplitude).
        // Sawtooth: ×0.625 (−4dB, bright harmonics but less extreme than square).
        switch (waveIdx) {
            case 0:  // Sine
                return sineTable[idx];
            case 1:  // Triangle
                return (idx < 128)
                    ? ((int16_t)idx * 32 - 2048)
                    : (2047 - (int16_t)(idx - 128) * 32);
            case 2:  // Square 50%
                return (idx < 128) ? 1024 : -1024;
            case 3:  // Pulse 25%
                return (idx < 64) ? 1024 : -1024;
            case 4:  // Sawtooth
                return (int16_t)(((int32_t)(idx * 16 - 2048) * 5) >> 3);
            case 5: {  // Supersaw (3 detuned sines, ±~13 cents)
                phaseB += phaseInc + (phaseInc >> 7);
                phaseC += phaseInc - (phaseInc >> 7);
                int32_t s = (int32_t)sineTable[idx]
                          + sineTable[(phaseB >> 16) & 0xFF]
                          + sineTable[(phaseC >> 16) & 0xFF];
                s = (s * 2) / 3;
                if (s > 2047)  s = 2047;
                if (s < -2047) s = -2047;
                return (int16_t)s;
            }
            default:
                return sineTable[idx];
        }
    }

    // ───────────────────────────────────────────────────────────
    // Find index of youngest node (highest id), optionally excluding one
    // ───────────────────────────────────────────────────────────
    int8_t __not_in_flash_func(findYoungestNode)(int8_t excludeIdx = -1) {
        int8_t best = -1;
        uint32_t bestAge = 0;
        for (int i = 0; i < nodeCount; i++) {
            if (i == excludeIdx) continue;
            if (best < 0 || nodes[i].birthOrder > bestAge) {
                bestAge = nodes[i].birthOrder;
                best = (int8_t)i;
            }
        }
        return best;
    }

    // ───────────────────────────────────────────────────────────
    // Find index of oldest node (lowest id)
    // ───────────────────────────────────────────────────────────
    int8_t __not_in_flash_func(findOldestNode)() {
        if (nodeCount == 0) return -1;
        int8_t best = 0;
        uint32_t bestAge = nodes[0].birthOrder;
        for (int i = 1; i < nodeCount; i++) {
            if (nodes[i].birthOrder < bestAge) {
                bestAge = nodes[i].birthOrder;
                best = (int8_t)i;
            }
        }
        return best;
    }

    // ───────────────────────────────────────────────────────────
    // Advance to next node via weighted random selection
    // ───────────────────────────────────────────────────────────
    void __not_in_flash_func(advanceChain)() {
        if (nodeCount == 0 || currentNode < 0) return;

        Node &cur = nodes[currentNode];
        if (cur.linkCount == 0) {
            // No outgoing links: teleport to oldest node (lowest id)
            int8_t oldIdx = findOldestNode();
            if (oldIdx >= 0) {
                currentNode = oldIdx;
                updatePhaseIncrement(nodes[oldIdx].pitch);
                gateActive = true;
                gateTimer = 0;
                gateDuration = (stepInterval * GATE_LENGTH_PCT) / 100;
            }
            return;
        }

        uint32_t totalWeight = 0;
        for (int i = 0; i < cur.linkCount; i++) {
            totalWeight += cur.links[i].weight;
        }
        if (totalWeight == 0) return;

        uint32_t r = fastRandom() % totalWeight;
        uint32_t cumulative = 0;
        for (int i = 0; i < cur.linkCount; i++) {
            cumulative += cur.links[i].weight;
            if (r < cumulative) {
                int8_t next = (int8_t)cur.links[i].target;
                if (next >= 0 && next < nodeCount && nodes[next].active) {
                    currentNode = next;
                }
                break;
            }
        }

        updatePhaseIncrement(nodes[currentNode].pitch);
        gateActive = true;
        gateTimer = 0;
        gateDuration = (stepInterval * GATE_LENGTH_PCT) / 100;
    }

    // ───────────────────────────────────────────────────────────
    // Advance chain 2 — independent traversal of the same graph
    // ───────────────────────────────────────────────────────────
    void __not_in_flash_func(advanceChain2)() {
        if (nodeCount == 0 || currentNode2 < 0) return;

        Node &cur = nodes[currentNode2];
        if (cur.linkCount == 0) {
            // No outgoing links: teleport to oldest node (lowest id)
            int8_t oldIdx = findOldestNode();
            if (oldIdx >= 0) {
                currentNode2 = oldIdx;
                updatePhaseIncrement2(nodes[oldIdx].pitch);
                gateActive2 = true;
                gateTimer2 = 0;
                gateDuration2 = (stepInterval2 * GATE_LENGTH_PCT) / 100;
            }
            return;
        }

        uint32_t totalWeight = 0;
        for (int i = 0; i < cur.linkCount; i++) {
            totalWeight += cur.links[i].weight;
        }
        if (totalWeight == 0) return;

        uint32_t r = fastRandom() % totalWeight;
        uint32_t cumulative = 0;
        for (int i = 0; i < cur.linkCount; i++) {
            cumulative += cur.links[i].weight;
            if (r < cumulative) {
                int8_t next = (int8_t)cur.links[i].target;
                if (next >= 0 && next < nodeCount && nodes[next].active) {
                    currentNode2 = next;
                }
                break;
            }
        }

        updatePhaseIncrement2(nodes[currentNode2].pitch);
        gateActive2 = true;
        gateTimer2 = 0;
        gateDuration2 = (stepInterval2 * GATE_LENGTH_PCT) / 100;
    }

    // ───────────────────────────────────────────────────────────
    // MIDI Note On → queue a new node (runs on Core 1)
    //   note → pitch (0–4095), velocity → probability (0–4095)
    // ───────────────────────────────────────────────────────────
    void ProcessIncomingNoteOn(uint8_t note, uint8_t velocity) override {
        // Map MIDI note 36-96 → pitch 0-4095
        int32_t pitch = ((int32_t)(note - 36) * 4096) / 60;
        if (pitch < 0)    pitch = 0;
        if (pitch > 4095) pitch = 4095;
        // Map velocity 1-127 → probability 32-4095
        uint16_t prob = (uint16_t)(velocity * 32);
        if (prob < 1)     prob = 1;
        if (prob > 4095)  prob = 4095;
        // Queue as pending edit for Core 0
        pendingEdit.nodeId   = (uint8_t)((pitch >> 7) & 0x7F); // pitch high
        pendingEdit.targetId = (uint8_t)(pitch & 0x7F);         // pitch low
        pendingEdit.weight   = prob;
        pendingEdit.cmd      = MSG_ADD_NODE;
    }

    // ───────────────────────────────────────────────────────────
    // Add a new node to the graph
    //   - links youngest existing node → new node
    //   - extra random links based on probability (0–4 extra)
    //   - all link weights are random
    //   - first node gets no initial links
    // ───────────────────────────────────────────────────────────
    void __not_in_flash_func(addNode)(int32_t pitch, uint16_t probability) {
        graphSeq++;  // begin atomic write — entire delete+cleanup+add is one operation

        // Hard-full guard. The deferred-ops scheduler evicts BEFORE calling
        // addNode, so this should never trigger — but creating while full
        // would write past the array, and evicting here would collide with
        // the sliced delete fixup, so refuse instead.
        if (nodeCount >= MAX_NODES) return;

        int newIdx = nodeCount;
        Node &n = nodes[newIdx];
        n.id        = allocateNodeId();
        n.birthOrder = nextBirthOrder++;
        n.pitch     = pitch;
        n.linkCount = 0;
        n.active    = true;
        n.synced    = false;

        if (nodeCount > 0) {
            // Primary link: youngest existing node → new node
            int8_t youngestIdx = findYoungestNode((int8_t)newIdx);
            if (youngestIdx >= 0) {
                Node &youngest = nodes[youngestIdx];
                if (youngest.linkCount < MAX_LINKS) {
                    youngest.links[youngest.linkCount].target = (uint8_t)newIdx;
                    youngest.links[youngest.linkCount].weight = (uint16_t)(1 + (fastRandom() % 4095));
                    youngest.linkCount++;
                    youngest.synced = false;
                }
            }

            // Extra links: each of 4 possible slots is rolled independently with
            // probability `probability/4095`. Smooth binomial — 1% ≈ always 0 extras,
            // 100% = always 4 extras, 50% averages 2.
            int maxExtra = 0;
            for (int t = 0; t < 4; t++) {
                if ((fastRandom() % 4096) < probability) maxExtra++;
            }

            for (int e = 0; e < maxExtra && n.linkCount < MAX_LINKS; e++) {
                uint8_t randomTarget;
                int attempts = 0;
                do {
                    randomTarget = (uint8_t)(fastRandom() % (nodeCount + 1));  // +1 includes all existing + self check
                    attempts++;
                } while ((randomTarget == (uint8_t)newIdx) && attempts < 10);

                if (randomTarget == (uint8_t)newIdx) continue;
                if (randomTarget >= (uint8_t)(nodeCount)) continue;

                // Check for duplicate
                bool alreadyLinked = false;
                for (int i = 0; i < n.linkCount; i++) {
                    if (n.links[i].target == randomTarget) {
                        alreadyLinked = true;
                        break;
                    }
                }
                if (!alreadyLinked) {
                    n.links[n.linkCount].target = randomTarget;
                    n.links[n.linkCount].weight = (uint16_t)(1 + (fastRandom() % 4095));
                    n.linkCount++;
                }
            }
        }
        // First node: no initial links (teleport to oldest in advanceChain)

        nodeCount++;
        lastCreatedNode = newIdx;  // track for next creation

        // Start playing immediately if first node
        if (nodeCount == 1) {
            currentNode = 0;
            updatePhaseIncrement(n.pitch);
            gateActive  = true;
            gateTimer   = 0;
            gateDuration = (stepInterval * GATE_LENGTH_PCT) / 100;
            currentNode2 = 0;
            updatePhaseIncrement2(n.pitch);
        }

        graphSeq++;  // end write
    }

    // ───────────────────────────────────────────────────────────
    // Delete a node — the cheap atomic part only: swap-with-last, playhead
    // fixups, UI notification. The expensive link-fixup scan is armed here
    // (delFixSlot) and completed in slices by the deferred-ops scheduler.
    // Until the fixup finishes: links → delFixSlot (deleted) transiently hit
    // the moved node; links → old last slot are out of range and advanceChain
    // ignores them. Both are index-valid, so nothing can read garbage.
    // Does NOT touch graphSeq — caller must manage the seqlock.
    // ───────────────────────────────────────────────────────────
    void __not_in_flash_func(deleteNodeBody)(int8_t idx) {
        if (idx < 0 || idx >= nodeCount) return;
        int8_t lastIdx = (int8_t)(nodeCount - 1);

        // Queue UI notification (ring drained by Core 1 → MSG_NODE_DELETED)
        deletedIds[deletedHead & 63] = nodes[idx].id;
        deletedHead = (uint8_t)(deletedHead + 1);

        // Move the last node into the hole (single struct copy)
        if (idx != lastIdx) {
            nodes[idx] = nodes[lastIdx];
        }
        nodeCount--;

        // Arm the sliced link fixup (top priority in the deferred scheduler)
        delFixSlot   = idx;
        delFixLast   = (uint8_t)lastIdx;
        delFixCursor = 0;

        // Fix currentNode: deleted → re-pick, moved (was lastIdx) → follow
        if (nodeCount == 0) {
            currentNode = -1;
            phaseIncrement = 0;
            gateActive = false;
        } else if (currentNode == idx) {
            currentNode = (int8_t)(fastRandom() % nodeCount);
            updatePhaseIncrement(nodes[currentNode].pitch);
        } else if (currentNode == lastIdx) {
            currentNode = idx;
        }

        // Fix currentNode2
        if (nodeCount == 0) {
            currentNode2 = -1;
            phaseIncrement2 = 0;
            gateActive2 = false;
        } else if (currentNode2 == idx) {
            currentNode2 = (int8_t)(fastRandom() % nodeCount);
            updatePhaseIncrement2(nodes[currentNode2].pitch);
        } else if (currentNode2 == lastIdx) {
            currentNode2 = idx;
        }

        // Fix lastCreatedNode
        if (nodeCount == 0) {
            lastCreatedNode = -1;
        } else if (lastCreatedNode == idx) {
            lastCreatedNode = -1;  // reset — next addNode will fall back to currentNode
        } else if (lastCreatedNode == lastIdx) {
            lastCreatedNode = idx;
        }
    }

    // Wrapper with seqlock for standalone delete calls
    void __not_in_flash_func(deleteNode)(int8_t idx) {
        graphSeq++;
        deleteNodeBody(idx);
        graphSeq++;
    }

    // ───────────────────────────────────────────────────────────
    // Find array index by node ID. Returns -1 if not found.
    // ─────────────────────────────────────────────────────────────
    int8_t __not_in_flash_func(findNodeByID)(uint8_t id) {
        for (int i = 0; i < nodeCount; i++) {
            if (nodes[i].id == id) return (int8_t)i;
        }
        return -1;
    }

    // ─────────────────────────────────────────────────────────────
    // Delete a specific link from a node (by source and target IDs)
    // ─────────────────────────────────────────────────────────────
    void __not_in_flash_func(deleteLinkByID)(uint8_t sourceId, uint8_t targetId) {
        int8_t srcIdx = findNodeByID(sourceId);
        int8_t tgtIdx = findNodeByID(targetId);
        if (srcIdx < 0 || tgtIdx < 0) return;

        graphSeq++;
        Node &n = nodes[srcIdx];
        for (int j = 0; j < n.linkCount; ) {
            if (n.links[j].target == (uint8_t)tgtIdx) {
                for (int k = j; k < n.linkCount - 1; k++) {
                    n.links[k] = n.links[k + 1];
                }
                n.linkCount--;
                n.synced = false;
                break;  // remove only first match
            } else {
                j++;
            }
        }
        graphSeq++;
    }

    // ─────────────────────────────────────────────────────────────
    // Add a link between two nodes (by IDs)
    // ─────────────────────────────────────────────────────────────
    void __not_in_flash_func(addLinkByID)(uint8_t sourceId, uint8_t targetId, uint16_t weight) {
        int8_t srcIdx = findNodeByID(sourceId);
        int8_t tgtIdx = findNodeByID(targetId);
        if (srcIdx < 0 || tgtIdx < 0) return;

        Node &n = nodes[srcIdx];
        if (n.linkCount >= MAX_LINKS) return;

        // Don't duplicate
        for (int i = 0; i < n.linkCount; i++) {
            if (n.links[i].target == (uint8_t)tgtIdx) return;
        }

        graphSeq++;
        n.links[n.linkCount].target = (uint8_t)tgtIdx;
        n.links[n.linkCount].weight = weight;
        n.linkCount++;
        n.synced = false;
        graphSeq++;
    }

    // ─────────────────────────────────────────────────────────────
    // Delete a node by ID
    // ─────────────────────────────────────────────────────────────
    void __not_in_flash_func(deleteNodeByID)(uint8_t id) {
        int8_t idx = findNodeByID(id);
        if (idx >= 0) deleteNode(idx);
    }

    // ─────────────────────────────────────────────────────────────
    // Repair graph after node deletion:
    //   - Nodes with no incoming links get a link from the youngest node
    //   - Nodes with no outgoing links get a link to the oldest node
    // Does NOT touch graphSeq — caller must manage the seqlock.
    // ─────────────────────────────────────────────────────────────
    void __not_in_flash_func(cleanupStrayNodesBody)() {
        if (nodeCount <= 1) return;

        int8_t youngestIdx = findYoungestNode();
        int8_t oldestIdx   = findOldestNode();

        // One pass over all links: mark nodes that have an incoming link from
        // another node. This runs inside the audio ISR — it must stay O(nodes·links).
        // (The old per-node rescan was O(nodes²·links) ≈ 2000+ iterations at capacity,
        // blowing the ~41µs sample budget and starving the ADC DMA.)
        bool hasIncoming[MAX_NODES] = {};
        for (int j = 0; j < nodeCount; j++) {
            for (int k = 0; k < nodes[j].linkCount; k++) {
                uint8_t t = nodes[j].links[k].target;
                if (t < (uint8_t)nodeCount && t != (uint8_t)j) hasIncoming[t] = true;
            }
        }

        // Fix nodes with no incoming links: youngest → them
        for (int i = 0; i < nodeCount; i++) {
            if (i == youngestIdx) continue;  // youngest is the source, skip
            if (!hasIncoming[i] && youngestIdx >= 0) {
                Node &youngest = nodes[youngestIdx];
                if (youngest.linkCount < MAX_LINKS) {
                    // Check not already linked
                    bool already = false;
                    for (int k = 0; k < youngest.linkCount; k++) {
                        if (youngest.links[k].target == (uint8_t)i) { already = true; break; }
                    }
                    if (!already) {
                        youngest.links[youngest.linkCount].target = (uint8_t)i;
                        youngest.links[youngest.linkCount].weight = (uint16_t)(1 + (fastRandom() % 4095));
                        youngest.linkCount++;
                        youngest.synced = false;
                    }
                }
            }
        }

        // Fix nodes with no outgoing links: them → oldest
        for (int i = 0; i < nodeCount; i++) {
            if (nodes[i].linkCount > 0) continue;
            if (oldestIdx < 0 || oldestIdx == i) continue;
            nodes[i].links[0].target = (uint8_t)oldestIdx;
            nodes[i].links[0].weight = (uint16_t)(1 + (fastRandom() % 4095));
            nodes[i].linkCount = 1;
            nodes[i].synced = false;
        }
    }

    // Wrapper with seqlock for standalone cleanup calls
    void cleanupStrayNodes() {
        graphSeq++;
        cleanupStrayNodesBody();
        graphSeq++;
    }

    // ─────────────────────────────────────────────────────────────
    // ProcessIncomingSysEx — runs on Core 1
    // ─────────────────────────────────────────────────────────────
    void ProcessIncomingSysEx(uint8_t *data, uint32_t size) override {
        if (size < 1) return;
        uint8_t cmd = data[0];

        if (cmd == MSG_PULL_REQUEST) {
            fullResyncRequested = true;
            return;
        }

        if (cmd == MSG_DELETE_NODE && size >= 2) {
            pendingEdit.nodeId = data[1];
            pendingEdit.targetId = 0;
            pendingEdit.weight = 0;
            pendingEdit.cmd = MSG_DELETE_NODE;
            return;
        }

        if (cmd == MSG_DELETE_LINK && size >= 3) {
            pendingEdit.nodeId = data[1];   // source ID
            pendingEdit.targetId = data[2]; // target ID
            pendingEdit.weight = 0;
            pendingEdit.cmd = MSG_DELETE_LINK;
            return;
        }

        if (cmd == MSG_ADD_LINK && size >= 5) {
            pendingEdit.nodeId = data[1];   // source ID
            pendingEdit.targetId = data[2]; // target ID
            pendingEdit.weight = ((uint16_t)data[3] << 7) | data[4];
            pendingEdit.cmd = MSG_ADD_LINK;
            return;
        }

        if (cmd == MSG_SET_SCALE && size >= 2) {
            uint8_t idx = data[1];
            if (idx < NUM_SCALES) {
                scaleIndex = idx;
                scheduleSave();
            }
            return;
        }

        if (cmd == MSG_ADD_NODE && size >= 4) {
            // data[1] = pitch high 7 bits, data[2] = pitch low 7 bits, data[3] = probability (7-bit)
            pendingEdit.nodeId   = data[1];  // pitch high
            pendingEdit.targetId = data[2];  // pitch low
            pendingEdit.weight   = ((uint16_t)data[3]) * 32;
            pendingEdit.cmd      = MSG_ADD_NODE;
            return;
        }

        if (cmd == MSG_SET_AUTO_INT && size >= 2) {
            // data[1] = 0–127 maps to 0.25s–8s
            uint8_t val = data[1];
            if (val > 127) val = 127;
            // Linear map: 0 → 0.25s (6000 samples), 127 → 8s (192000 samples)
            autoInterval = 6000 + ((uint32_t)val * (192000 - 6000)) / 127;
            scheduleSave();
            return;
        }

        if (cmd == MSG_SET_WAVEFORM && size >= 2) {
            uint8_t idx = data[1];
            if (idx < NUM_WAVEFORMS) { waveformIndex1 = idx; scheduleSave(); }
            return;
        }

        if (cmd == MSG_SET_BASS_MODE && size >= 2) {
            bassMode1 = (data[1] != 0);
            scheduleSave();
            return;
        }

        if (cmd == MSG_SET_WAVEFORM2 && size >= 2) {
            uint8_t idx = data[1];
            if (idx < NUM_WAVEFORMS) { waveformIndex2 = idx; scheduleSave(); }
            return;
        }

        if (cmd == MSG_SET_BASS_MODE2 && size >= 2) {
            bassMode2 = (data[1] != 0);
            scheduleSave();
            return;
        }

        if (cmd == MSG_SET_MAX_NODES && size >= 2) {
            uint8_t mn = data[1];
            if (mn >= 2 && mn <= MAX_NODES) {
                maxNodes = mn;   // Core 0 enforces (evicts surplus one per sample)
                scheduleSave();
            }
            return;
        }
    }

    // ───────────────────────────────────────────────────────────
    // MIDICore — runs on Core 1
    // Sends CC status and SysEx node data to the browser.
    // ───────────────────────────────────────────────────────────
    void MIDICore() override {
        if (pendingFlashSave) {
            pendingFlashSave = false;
            scheduleSave();
        }
        if (flashSaveDirty && time_us_64() >= flashSaveDeadline) {
            // Hard spacing between writes: keeps retrying until the gap has
            // passed, so the save still lands — just never back-to-back.
            static uint64_t lastFlashWrite = 0;
            uint64_t nowFW = time_us_64();
            if (nowFW - lastFlashWrite >= FLASH_MIN_WRITE_SPACING_US) {
                lastFlashWrite = nowFW;
                flashSaveDirty = false;
                saveToFlash();
            }
        }

        if (!time_reached(nextMidiServiceTime)) return;
        nextMidiServiceTime = make_timeout_time_us(1000);

        static int16_t lastCN = -2, lastNC = -1;
        static int16_t lastKM = -1, lastKX = -1, lastKY = -1, lastSW = -1;
        static int16_t lastCN2 = -2, lastScale = -1, lastAutoInt = -1;
        static int16_t lastWaveform = -1, lastBassMode = -1, lastWaveform2 = -1, lastBassMode2 = -1;
        static int8_t lastMidiNote1 = -1, lastMidiNote2 = -1;
        static int32_t lastBpm1 = -1, lastBpm2 = -1, lastClkMode = -1, lastClkLevel = -999;
        static int16_t lastMaxNodes = -1;
        uint64_t now = time_us_64();

        // CC: Current node (send node ID, not array index)
        int16_t curCN = (currentNode >= 0 && currentNode < nodeCount)
            ? (int16_t)nodes[currentNode].id : -1;
        if (curCN != lastCN) {
            SendCC(CC_CURRENT_NODE, curCN >= 0 ? (uint8_t)curCN : 127);
            // MIDI Note output on channel 2 (index 1)
            if (lastMidiNote1 >= 0) SendNoteOff(1, (uint8_t)lastMidiNote1);
            if (curCN >= 0) {
                int32_t pitch = nodes[currentNode].pitch;
                int32_t mn = 36 + (pitch * 60) / 4096;
                if (mn > 96) mn = 96;
                mn = applyBassMode(mn, bassMode1);
                SendNoteOn(1, (uint8_t)mn, 100);
                lastMidiNote1 = (int8_t)mn;
            } else {
                lastMidiNote1 = -1;
            }
            lastCN = curCN;
        }

        // CC: Current node 2
        int16_t curCN2 = (currentNode2 >= 0 && currentNode2 < nodeCount)
            ? (int16_t)nodes[currentNode2].id : -1;
        if (curCN2 != lastCN2) {
            SendCC(CC_CURRENT_NODE2, curCN2 >= 0 ? (uint8_t)curCN2 : 127);
            // MIDI Note output on channel 3 (index 2)
            if (lastMidiNote2 >= 0) SendNoteOff(2, (uint8_t)lastMidiNote2);
            if (curCN2 >= 0) {
                int32_t pitch = nodes[currentNode2].pitch;
                int32_t mn = 36 + (pitch * 60) / 4096;
                if (mn > 96) mn = 96;
                mn = applyBassMode(mn, bassMode2);
                SendNoteOn(2, (uint8_t)mn, 100);
                lastMidiNote2 = (int8_t)mn;
            } else {
                lastMidiNote2 = -1;
            }
            lastCN2 = curCN2;
        }

        // CC: Node count
        int16_t curNC = (int16_t)nodeCount;
        if (curNC != lastNC) {
            SendCC(CC_NODE_COUNT, (uint8_t)curNC);
            lastNC = curNC;
        }

        // CC: Switch
        Switch sw = SwitchVal();
        int16_t curSW = (sw == Switch::Up) ? 0 : (sw == Switch::Middle) ? 1 : 2;
        if (curSW != lastSW) {
            SendCC(CC_SWITCH, (uint8_t)curSW);
            lastSW = curSW;
        }

        // CC: Scale index
        int16_t curScale = (int16_t)scaleIndex;
        if (curScale != lastScale) {
            SendCC(CC_SCALE, (uint8_t)scaleIndex);
            lastScale = curScale;
        }

        // CC: Auto interval (convert samples back to 0–127)
        int16_t curAutoInt = (int16_t)(((autoInterval - 6000) * 127) / (192000 - 6000));
        if (curAutoInt < 0) curAutoInt = 0;
        if (curAutoInt > 127) curAutoInt = 127;
        if (curAutoInt != lastAutoInt) {
            SendCC(CC_AUTO_INTERVAL, (uint8_t)curAutoInt);
            lastAutoInt = curAutoInt;
        }

        // CC: Waveform index (chain 1)
        int16_t curWaveform = (int16_t)waveformIndex1;
        if (curWaveform != lastWaveform) {
            SendCC(CC_WAVEFORM, (uint8_t)waveformIndex1);
            lastWaveform = curWaveform;
        }

        // CC: Bass mode (chain 1)
        int16_t curBassMode = bassMode1 ? 1 : 0;
        if (curBassMode != lastBassMode) {
            SendCC(CC_BASS_MODE, (uint8_t)curBassMode);
            lastBassMode = curBassMode;
        }

        // CC: Waveform index (chain 2)
        int16_t curWaveform2 = (int16_t)waveformIndex2;
        if (curWaveform2 != lastWaveform2) {
            SendCC(CC_WAVEFORM2, (uint8_t)waveformIndex2);
            lastWaveform2 = curWaveform2;
        }

        // CC: Bass mode (chain 2)
        int16_t curBassMode2 = bassMode2 ? 1 : 0;
        if (curBassMode2 != lastBassMode2) {
            SendCC(CC_BASS_MODE2, (uint8_t)curBassMode2);
            lastBassMode2 = curBassMode2;
        }

        // CC: Max nodes
        int16_t curMaxNodes = (int16_t)maxNodes;
        if (curMaxNodes != lastMaxNodes) {
            SendCC(CC_MAX_NODES, (uint8_t)curMaxNodes);
            lastMaxNodes = curMaxNodes;
        }

        // CC: Knobs at ~20 Hz
        static uint64_t lastKnobTime = 0;
        if (now - lastKnobTime >= 50000) {
            lastKnobTime = now;
            uint8_t km = (uint8_t)(KnobVal(Knob::Main) >> 5);
            if (abs((int)km - (int)lastKM) > 1) { SendCC(CC_KNOB_MAIN, km); lastKM = km; }
            uint8_t kx = (uint8_t)(KnobVal(Knob::X) >> 5);
            if (abs((int)kx - (int)lastKX) > 1) { SendCC(CC_KNOB_X, kx); lastKX = kx; }
            uint8_t ky = (uint8_t)(KnobVal(Knob::Y) >> 5);
            if (abs((int)ky - (int)lastKY) > 1) { SendCC(CC_KNOB_Y, ky); lastKY = ky; }
        }

        // SysEx: Clock status (per-chain BPM + divide/multiply factor) at ~10 Hz, on change.
        // BPM = steps per minute = 60 * SAMPLE_RATE / stepInterval (also the external clock rate in PPM).
        static uint64_t lastClockTime = 0;
        if (now - lastClockTime >= 100000) {
            lastClockTime = now;

            bool p1 = Connected(Input::Pulse1);
            bool p2 = Connected(Input::Pulse2);
            // 0 = both internal, 1 = Pulse1 drives (knob mult chain 2), 2 = Pulse2 drives (knob mult chain 1), 3 = both external
            uint8_t clkMode = (p1 && p2) ? 3 : p1 ? 1 : p2 ? 2 : 0;

            uint32_t si1 = stepInterval  ? stepInterval  : 1;
            uint32_t si2 = stepInterval2 ? stepInterval2 : 1;
            uint32_t bpm1 = 60u * SAMPLE_RATE / (si1 * CLOCK_PPQN);  // musical BPM (2 PPQN)
            uint32_t bpm2 = 60u * SAMPLE_RATE / (si2 * CLOCK_PPQN);
            if (bpm1 > 16383) bpm1 = 16383;   // clamp to 14-bit
            if (bpm2 > 16383) bpm2 = 16383;

            int32_t clkLevel = clockMultLevel(KnobVal(Knob::X));  // signed half-steps from unity

            if ((int32_t)bpm1 != lastBpm1 || (int32_t)bpm2 != lastBpm2 ||
                (int32_t)clkMode != lastClkMode || clkLevel != lastClkLevel) {
                uint8_t buf[7];
                buf[0] = MSG_CLOCK_STATUS;
                buf[1] = clkMode;
                buf[2] = (uint8_t)((bpm1 >> 7) & 0x7F);
                buf[3] = (uint8_t)(bpm1 & 0x7F);
                buf[4] = (uint8_t)((bpm2 >> 7) & 0x7F);
                buf[5] = (uint8_t)(bpm2 & 0x7F);
                buf[6] = (uint8_t)((clkLevel + 64) & 0x7F);  // bias +64 → unsigned 7-bit
                SendSysEx(buf, 7);
                lastBpm1 = bpm1; lastBpm2 = bpm2;
                lastClkMode = clkMode; lastClkLevel = clkLevel;
            }
        }

        // SysEx: ISR timing probe report every ~1s (diagnostics)
        static uint64_t lastPerfTime = 0;
        if (now - lastPerfTime >= 1000000) {
            lastPerfTime = now;
            uint32_t us = isrMaxUs;
            uint32_t uA = isrMaxA, uB = isrMaxB, uC = isrMaxC;
            isrMaxUs = 0;   // reset measurement window
            isrMaxA = 0; isrMaxB = 0; isrMaxC = 0;
            if (us > 16383) us = 16383;
            if (uA > 16383) uA = 16383;
            if (uB > 16383) uB = 16383;
            if (uC > 16383) uC = 16383;
            uint8_t pbuf[9];
            pbuf[0] = MSG_PERF;
            pbuf[1] = (uint8_t)((us >> 7) & 0x7F);
            pbuf[2] = (uint8_t)(us & 0x7F);
            pbuf[3] = (uint8_t)((uA >> 7) & 0x7F);
            pbuf[4] = (uint8_t)(uA & 0x7F);
            pbuf[5] = (uint8_t)((uB >> 7) & 0x7F);
            pbuf[6] = (uint8_t)(uB & 0x7F);
            pbuf[7] = (uint8_t)((uC >> 7) & 0x7F);
            pbuf[8] = (uint8_t)(uC & 0x7F);
            SendSysEx(pbuf, 9);
        }

        // Periodic full CC resend every 2s
        static uint64_t lastPeriodicTime = 0;
        if (now - lastPeriodicTime >= 2000000) {
            lastPeriodicTime = now;
            lastCN = -2; lastNC = -1; lastCN2 = -2; lastScale = -1; lastAutoInt = -1;
            lastKM = -1; lastKX = -1; lastKY = -1; lastSW = -1;
            lastWaveform = -1; lastBassMode = -1; lastWaveform2 = -1; lastBassMode2 = -1;
            lastBpm1 = -1; lastBpm2 = -1; lastClkMode = -1; lastClkLevel = -999; lastMaxNodes = -1;
        }

        // Handle full resync request
        if (fullResyncRequested) {
            fullResyncRequested = false;
            for (int i = 0; i < nodeCount; i++) {
                nodes[i].synced = false;
            }
            lastCN = -2; lastNC = -1; lastCN2 = -2; lastScale = -1; lastAutoInt = -1;
            lastKM = -1; lastKX = -1; lastKY = -1; lastSW = -1;
            lastWaveform = -1; lastBassMode = -1; lastWaveform2 = -1; lastBassMode2 = -1;
            lastBpm1 = -1; lastBpm2 = -1; lastClkMode = -1; lastClkLevel = -999; lastMaxNodes = -1;
        }

        // SysEx: notify UI of deleted nodes. At most 2 per pass (~1ms) —
        // a burst (cap slashed 20→5) would otherwise overflow the USB MIDI
        // TX buffer and silently drop messages, leaving ghost nodes in the UI.
        for (int d = 0; d < 2 && deletedTail != deletedHead; d++) {
            uint8_t did = deletedIds[deletedTail & 63];
            deletedTail = (uint8_t)(deletedTail + 1);
            uint8_t dbuf[2] = { MSG_NODE_DELETED, (uint8_t)(did & 0x7F) };
            SendSysEx(dbuf, 2);
        }

        // SysEx: roster broadcast every ~2s — the authoritative list of live
        // node IDs. Lets the UI prune ghosts / detect gaps even if an
        // incremental message was ever lost on the wire.
        // Initialized to 1s so the roster grid sits between the 2s CC
        // force-resend grid — keeps the two periodic bursts from stacking
        // into one long USB service window (visible as ISR_PEAK spikes).
        static uint64_t lastRosterTime = 1000000;
        if (now - lastRosterTime >= 2000000 && deletedTail == deletedHead) {
            lastRosterTime = now;
            uint8_t ids[MAX_NODES];
            int cnt = 0;
            uint32_t rsA, rsB;
            do {
                rsA = graphSeq;
                if (rsA & 1u) continue;
                cnt = nodeCount;
                if (cnt > MAX_NODES) cnt = MAX_NODES;
                for (int i = 0; i < cnt; i++) ids[i] = nodes[i].id;
                rsB = graphSeq;
            } while (rsA != rsB || (rsB & 1u));

            uint8_t rbuf[2 + MAX_NODES];
            rbuf[0] = MSG_ROSTER;
            rbuf[1] = (uint8_t)cnt;
            for (int i = 0; i < cnt; i++) rbuf[2 + i] = ids[i] & 0x7F;
            SendSysEx(rbuf, 2 + cnt);
        }

        // SysEx: Node sync (one node per ~3ms — a full 64-node resync in ~200ms)
        static uint64_t lastSysExTime = 0;
        if (now - lastSysExTime >= 3000) {
            uint32_t seqA, seqB;
            int unsyncedIdx = -1;
            Node snap;
            int total = 0;

            do {
                seqA = graphSeq;
                if (seqA & 1u) continue;
                total = nodeCount;
                unsyncedIdx = -1;
                for (int i = 0; i < total; i++) {
                    if (!nodes[i].synced) {
                        unsyncedIdx = i;
                        snap = nodes[i];
                        break;
                    }
                }
                seqB = graphSeq;
            } while (seqA != seqB || (seqB & 1u));

            if (unsyncedIdx >= 0) {
                lastSysExTime = now;

                // [MSG_NODE, idx, total, nodeId, pitch_hi, pitch_lo, linkCount,
                //  {targetId, weight_hi, weight_lo} × linkCount]
                // Link targets are sent as node IDs (not array indices)
                int pos = 0;
                midiTxBuf[pos++] = MSG_NODE;
                midiTxBuf[pos++] = (uint8_t)unsyncedIdx;
                midiTxBuf[pos++] = (uint8_t)total;
                midiTxBuf[pos++] = snap.id;
                midiTxBuf[pos++] = (snap.pitch >> 7) & 0x7F;
                midiTxBuf[pos++] = snap.pitch & 0x7F;
                midiTxBuf[pos++] = snap.linkCount;

                for (int j = 0; j < snap.linkCount; j++) {
                    // Resolve target array index → node ID
                    uint8_t tIdx = snap.links[j].target;
                    uint8_t tId = (tIdx < total) ? nodes[tIdx].id : snap.id;
                    midiTxBuf[pos++] = tId & 0x7F;
                    midiTxBuf[pos++] = (snap.links[j].weight >> 7) & 0x7F;
                    midiTxBuf[pos++] = snap.links[j].weight & 0x7F;
                }

                SendSysEx(midiTxBuf, pos);
                nodes[unsyncedIdx].synced = true;
            }
        }
    }

    // Clock divide/multiply level from Knob X: signed half-steps from unity (0 = ×1).
    // Right half → +1..+CLOCK_MULT_STEPS (×1.5, ×2, …); left half → -1..-CLOCK_DIV_STEPS (÷1.5, ÷2, …).
    // Rounded so unity occupies a symmetric band across the center detent, easy to land on.
    int32_t __not_in_flash_func(clockMultLevel)(int32_t knob) {
        int32_t n;
        if (knob >= 2048) {
            n = ((int32_t)(knob - 2048) * CLOCK_MULT_STEPS + 1023) / 2047;
            if (n > CLOCK_MULT_STEPS) n = CLOCK_MULT_STEPS;
        } else {
            n = -(((int32_t)(2048 - knob) * CLOCK_DIV_STEPS + 1024) / 2048);
            if (n < -CLOCK_DIV_STEPS) n = -CLOCK_DIV_STEPS;
        }
        return n;
    }

    // Derive a driven chain's step interval from the other chain's clock and Knob X.
    uint32_t __not_in_flash_func(derivedStepInterval)(uint32_t base, int32_t knobX) {
        int32_t n = clockMultLevel(knobX);
        uint32_t iv;
        if (n >= 0) {
            // multiply clock by (2+n)/2 → interval shrinks
            iv = (uint32_t)((uint64_t)base * 2 / (2 + n));
        } else {
            // divide clock by (2-n)/2 → interval grows  (n is negative)
            iv = (uint32_t)((uint64_t)base * (2 - n) / 2);
        }
        if (iv < MIN_STEP_INTERVAL) iv = MIN_STEP_INTERVAL;
        return iv;
    }

    // ───────────────────────────────────────────────────────────
    // ProcessSample — Core 0, 48kHz ISR
    // ───────────────────────────────────────────────────────────
    void __not_in_flash_func(ProcessSample)() override {
        uint32_t isrT0 = time_us_32();  // ISR timing probe

        // ═══════════════════════════════════════════
        // 0. PROCESS PENDING EDITS FROM UI
        // ═══════════════════════════════════════════
        {
            // Hold off on UI edits while a sliced delete fixup is running —
            // e.g. an addLink to the moved node would be wrongly removed.
            // The edit stays queued (single slot) and lands a few samples later.
            uint8_t cmd = pendingEdit.cmd;
            if (cmd != 0 && delFixSlot < 0) {
                uint8_t nid = pendingEdit.nodeId;
                uint8_t tid = pendingEdit.targetId;
                uint16_t w  = pendingEdit.weight;
                pendingEdit.cmd = 0;  // consume

                if (cmd == MSG_DELETE_NODE) {
                    deleteNodeByID(nid);
                    needCleanup = true;   // orphan repair on a later sample
                } else if (cmd == MSG_DELETE_LINK) {
                    deleteLinkByID(nid, tid);
                    needCleanup = true;
                } else if (cmd == MSG_ADD_LINK) {
                    addLinkByID(nid, tid, w);
                } else if (cmd == MSG_ADD_NODE) {
                    // nid = pitch high 7 bits, tid = pitch low 7 bits
                    int32_t pitch = ((int32_t)nid << 7) | (int32_t)tid;
                    if (pitch > 4095) pitch = 4095;
                    // Use knob Main for link probability (same as auto mode)
                    int32_t prob = Connected(Input::CV1)
                        ? (CVIn1() + 2048) : KnobVal(Knob::Main);
                    if (prob < 1)    prob = 1;
                    if (prob > 4095) prob = 4095;
                    // Queue — the deferred-ops block below paces evict/create
                    pendingCreatePitch = pitch;
                    pendingCreateProb  = (uint16_t)prob;
                }
            }
        }

        // ═══════════════════════════════════════════
        // 0.5 DEFERRED GRAPH OPS — at most ONE small op per sample.
        // Eviction and creation get their own ISR tick; orphan repair is
        // sliced 16 nodes per tick. Anything bigger overruns the ~41.6µs
        // budget, starves the ADC DMA, and freezes all analog controls.
        // ═══════════════════════════════════════════
        if (needCleanup) {
            // (Re)start sliced repair — also restarts cleanly if the graph
            // changed mid-repair, since every edit path sets needCleanup.
            needCleanup  = false;
            repairPhase  = 1;
            repairCursor = 0;
            for (int i = 0; i < MAX_NODES; i++) repairIncoming[i] = false;
        }

        if (delFixSlot >= 0) {
            // Sliced delete fixup: remove links to the deleted slot, retarget
            // links to the old last slot (where the moved node used to live).
            // Removal is checked first, so a link retargeted to delFixSlot in
            // this pass is never mistaken for a stale link on a later slice.
            graphSeq++;
            int end = delFixCursor + REPAIR_SLICE;
            if (end > nodeCount) end = nodeCount;
            for (int j = delFixCursor; j < end; j++) {
                Node &n = nodes[j];
                for (int k = 0; k < n.linkCount; ) {
                    uint8_t t = n.links[k].target;
                    if (t == (uint8_t)delFixSlot) {
                        for (int m = k; m < n.linkCount - 1; m++) {
                            n.links[m] = n.links[m + 1];
                        }
                        n.linkCount--;
                        n.synced = false;
                    } else {
                        if (t == delFixLast) {
                            n.links[k].target = (uint8_t)delFixSlot;
                        }
                        k++;
                    }
                }
            }
            delFixCursor = (uint8_t)end;
            if (delFixCursor >= nodeCount) {
                delFixSlot  = -1;
                needCleanup = true;   // orphan repair follows the fixup
            }
            graphSeq++;
        } else if (repairPhase == 1) {
            // Phase 1 (read-only): scan a slice of nodes, mark link targets
            int end = repairCursor + REPAIR_SLICE;
            if (end > nodeCount) end = nodeCount;
            for (int j = repairCursor; j < end; j++) {
                Node &n = nodes[j];
                for (int k = 0; k < n.linkCount; k++) {
                    uint8_t t = n.links[k].target;
                    if (t < (uint8_t)nodeCount && t != (uint8_t)j) repairIncoming[t] = true;
                }
            }
            repairCursor = (uint8_t)end;
            if (repairCursor >= nodeCount) {
                repairYoungest = findYoungestNode();
                repairOldest   = findOldestNode();
                repairPhase    = 2;
                repairCursor   = 0;
            }
        } else if (repairPhase == 2) {
            // Phase 2 (mutating, under seqlock): repair a slice of orphans
            if (nodeCount <= 1) {
                repairPhase = 0;
            } else {
                graphSeq++;
                int end = repairCursor + REPAIR_SLICE;
                if (end > nodeCount) end = nodeCount;
                for (int j = repairCursor; j < end; j++) {
                    // No incoming links: youngest → j
                    if (j != repairYoungest && !repairIncoming[j]
                        && repairYoungest >= 0 && repairYoungest < nodeCount) {
                        Node &y = nodes[repairYoungest];
                        if (y.linkCount < MAX_LINKS) {
                            bool already = false;
                            for (int k = 0; k < y.linkCount; k++) {
                                if (y.links[k].target == (uint8_t)j) { already = true; break; }
                            }
                            if (!already) {
                                y.links[y.linkCount].target = (uint8_t)j;
                                y.links[y.linkCount].weight = (uint16_t)(1 + (fastRandom() % 4095));
                                y.linkCount++;
                                y.synced = false;
                            }
                        }
                    }
                    // No outgoing links: j → oldest
                    if (nodes[j].linkCount == 0
                        && repairOldest >= 0 && repairOldest != j && repairOldest < nodeCount) {
                        nodes[j].links[0].target = (uint8_t)repairOldest;
                        nodes[j].links[0].weight = (uint16_t)(1 + (fastRandom() % 4095));
                        nodes[j].linkCount = 1;
                        nodes[j].synced = false;
                    }
                }
                graphSeq++;
                repairCursor = (uint8_t)end;
                if (repairCursor >= nodeCount) repairPhase = 0;
            }
        } else if (nodeCount > (int)maxNodes) {
            // Cap lowered below current count: evict one oldest per visit
            int8_t surplusIdx = findOldestNode();
            if (surplusIdx >= 0) {
                graphSeq++;
                deleteNodeBody(surplusIdx);
                graphSeq++;
                needCleanup = true;
            }
        } else if (pendingCreatePitch >= 0) {
            if (nodeCount >= (int)maxNodes) {
                // Make room first; creation runs on a later sample
                int8_t oldestIdx = findOldestNode();
                if (oldestIdx >= 0) {
                    graphSeq++;
                    deleteNodeBody(oldestIdx);
                    graphSeq++;
                    needCleanup = true;
                }
            } else {
                addNode(pendingCreatePitch, pendingCreateProb);
                pendingCreatePitch = -1;
            }
        }

        uint32_t isrTA = time_us_32();  // end of section A (graph ops)

        // ═══════════════════════════════════════════
        // 1. SWITCH STATE & QUANTIZATION
        // ═══════════════════════════════════════════
        Switch sw = SwitchVal();
        quantize = true;
        useChromatic = false;  // always scale-quantized

        // ═══════════════════════════════════════════
        // 2. SWITCH GESTURE DETECTION
        //    Up = auto mode (timer-based node creation)
        //    Down flick = cycle to next scale
        // ═══════════════════════════════════════════
        bool currentSwitchUp   = (sw == Switch::Up);
        bool currentSwitchDown = (sw == Switch::Down);

        // --- Auto mode (Switch Up) ---
        if (currentSwitchUp && !prevSwitchUp) {
            // Just flicked up: enter auto mode, immediately request a node
            autoMode = true;
            autoMakeNode = true;
            autoTimer = 0;
        }
        if (currentSwitchUp) {
            // While held up: run timer
            autoTimer++;
            if (autoTimer >= autoInterval) {
                autoMakeNode = true;
                autoTimer = 0;
            }
        }
        if (!currentSwitchUp && prevSwitchUp) {
            // Released from up: leave auto mode
            autoMode = false;
        }
        prevSwitchUp = currentSwitchUp;

        // --- Scale cycling (Switch Down flick) ---
        if (currentSwitchDown && !prevSwitchDown) {
            // Flick down: cycle to next scale
            scaleIndex = (scaleIndex + 1) % NUM_SCALES;
            pendingFlashSave = true;
        }
        prevSwitchDown = currentSwitchDown;

        // --- Auto-create node: queue it for the deferred-ops block ---
        if (autoMakeNode) {
            autoMakeNode = false;
            int32_t pitch;
            if (Connected(Input::CV2)) {
                pitch = CVIn2() + 2048;
                if (pitch < 0)    pitch = 0;
                if (pitch > 4095) pitch = 4095;
            } else {
                pitch = randomPitchInRange(KnobVal(Knob::Y));
            }
            int32_t prob = Connected(Input::CV1)
                ? (CVIn1() + 2048)
                : KnobVal(Knob::Main);
            if (prob < 1)     prob = 1;
            if (prob > 4095)  prob = 4095;
            pendingCreatePitch = pitch;
            pendingCreateProb  = (uint16_t)prob;
        }

        // ═══════════════════════════════════════════
        // 3. CLOCK — role of Knob X depends on patched clock inputs
        // ═══════════════════════════════════════════
        bool pulse1Connected = Connected(Input::Pulse1);
        bool pulse2Connected = Connected(Input::Pulse2);

        // Both clocks patched: Knob X takes over the auto-generation interval,
        // but only once it has actually been moved (so plugging in doesn't snap it).
        bool bothClocks = pulse1Connected && pulse2Connected;
        if (bothClocks) {
            int32_t kx = KnobVal(Knob::X);
            if (!bothClockPrev) {
                bothClockEngaged   = false;
                bothClockStableCnt = 0;
            }
            if (bothClockStableCnt < CLOCK_BOTH_ARM_SAMPLES) {
                // Jack-detect debounce: floating inputs can flicker "connected",
                // so require a solid 0.5s of both-connected before arming.
                bothClockStableCnt++;
                bothClockKnobRef = kx;   // track start position until armed
            } else if (!bothClockEngaged) {
                int32_t d = kx - bothClockKnobRef;
                if (d < 0) d = -d;
                if (d > CLOCK_KNOB_MOVE_THRESH) {
                    bothClockEngaged = true;
                    bothClockLastKx  = kx;
                }
            }
            if (bothClockEngaged) {
                // Jitter gate: only respond to real knob motion, not ADC noise
                int32_t dk = kx - bothClockLastKx;
                if (dk < 0) dk = -dk;
                if (dk >= CLOCK_KNOB_JITTER) {
                    bothClockLastKx = kx;
                    // Same range as the UI slider: 6000–192000 samples (0.25s–8s)
                    uint32_t ai = 6000 + ((uint32_t)kx * (192000 - 6000)) / 4095;
                    if (ai != autoInterval) {
                        autoInterval = ai;
                        pendingFlashSave = true;  // debounced 1s save on Core 1
                    }
                }
            }
        } else {
            bothClockEngaged   = false;
            bothClockStableCnt = 0;
        }
        bothClockPrev = bothClocks;

        bool shouldAdvance = false;

        if (pulse1Connected) {
            // Chain 1 follows the external clock on Pulse In 1
            if (PulseIn1RisingEdge()) {
                if (receivedFirstPulse && stepCounter >= MIN_STEP_INTERVAL) {
                    stepInterval = stepCounter;
                }
                receivedFirstPulse = true;
                stepCounter = 0;
                shouldAdvance = true;
            }
            stepCounter++;
        } else {
            receivedFirstPulse = false;
            if (pulse2Connected) {
                // Only Pulse 2 patched: Knob X divides/multiplies chain 2's clock to drive chain 1
                stepInterval = derivedStepInterval(stepInterval2, KnobVal(Knob::X));
            } else {
                // No external clock: Knob X sets chain 1's internal tempo directly
                int32_t knobX = KnobVal(Knob::X);
                stepInterval = MIN_STEP_INTERVAL
                    + ((uint32_t)(4095 - knobX)
                       * (MAX_STEP_INTERVAL - MIN_STEP_INTERVAL)) / 4095;
            }
            stepCounter++;
            if (stepCounter >= stepInterval) {
                stepCounter = 0;
                shouldAdvance = true;
            }
        }

        if (shouldAdvance) {
            advanceChain();
        }

        // ═══════════════════════════════════════════
        // 4. GATE TIMING
        // ═══════════════════════════════════════════
        if (gateActive) {
            gateTimer++;
            if (gateTimer >= gateDuration) {
                gateActive = false;
            }
        }

        uint32_t isrTB = time_us_32();  // end of section B (control/clock)

        // ═══════════════════════════════════════════
        // 5. AUDIO — waveform of current node pitch
        // ═══════════════════════════════════════════
        int16_t audioOut = 0;
        if (currentNode >= 0 && nodeCount > 0) {
            sinePhase += phaseIncrement;
            audioOut = renderWaveform((sinePhase >> 16) & 0xFF, phaseIncrement, sinePhase1b, sinePhase1c, waveformIndex1);
        }
        AudioOut1(audioOut);

        // ═══════════════════════════════════════════
        // 6. CV OUTPUT — 1V/oct
        // ═══════════════════════════════════════════
        if (currentNode >= 0 && nodeCount > 0) {
            int32_t pitch = nodes[currentNode].pitch;
            if (quantize) {
                int32_t midiNote = 36 + (pitch * 60) / 4096;
                if (midiNote > 96) midiNote = 96;
                midiNote = applyBassMode(midiNote, bassMode1);
                CVOut1MIDINote((uint8_t)midiNote);
            } else {
                CVOut1((int16_t)(pitch - 2048));
            }
        } else {
            CVOut1(0);
        }

        // ═══════════════════════════════════════════
        // 7. PULSE OUTPUT — gate
        // ═══════════════════════════════════════════
        PulseOut1(gateActive);

        // ═══════════════════════════════════════════
        // 8. CHAIN 2 — independent traversal via Pulse 2
        // ═══════════════════════════════════════════
        if (nodeCount > 0 && currentNode2 < 0) {
            currentNode2 = 0;
            updatePhaseIncrement2(nodes[0].pitch);
        } else if (nodeCount == 0) {
            currentNode2 = -1;
            phaseIncrement2 = 0;
            gateActive2 = false;
        }

        // pulse1Connected / pulse2Connected computed in section 3 (same ProcessSample scope)
        bool shouldAdvance2 = false;

        if (pulse2Connected) {
            // Chain 2 follows the external clock on Pulse In 2
            if (PulseIn2RisingEdge()) {
                if (receivedFirstPulse2 && stepCounter2 >= MIN_STEP_INTERVAL) {
                    stepInterval2 = stepCounter2;
                }
                receivedFirstPulse2 = true;
                stepCounter2 = 0;
                shouldAdvance2 = true;
            }
            stepCounter2++;
        } else {
            receivedFirstPulse2 = false;
            if (pulse1Connected) {
                // Only Pulse 1 patched: Knob X divides/multiplies chain 1's clock to drive chain 2
                stepInterval2 = derivedStepInterval(stepInterval, KnobVal(Knob::X));
            } else {
                // No external clock: Knob X sets chain 2's internal tempo directly
                int32_t knobX2 = KnobVal(Knob::X);
                stepInterval2 = MIN_STEP_INTERVAL
                    + ((uint32_t)(4095 - knobX2)
                       * (MAX_STEP_INTERVAL - MIN_STEP_INTERVAL)) / 4095;
            }
            stepCounter2++;
            if (stepCounter2 >= stepInterval2) {
                stepCounter2 = 0;
                shouldAdvance2 = true;
            }
        }

        if (shouldAdvance2 && nodeCount > 0 && currentNode2 >= 0) {
            advanceChain2();
        }

        // Chain 2 gate timing
        if (gateActive2) {
            gateTimer2++;
            if (gateTimer2 >= gateDuration2) {
                gateActive2 = false;
            }
        }

        // Chain 2 audio output
        int16_t audioOut2 = 0;
        if (currentNode2 >= 0 && nodeCount > 0) {
            sinePhase2 += phaseIncrement2;
            audioOut2 = renderWaveform((sinePhase2 >> 16) & 0xFF, phaseIncrement2, sinePhase2b, sinePhase2c, waveformIndex2);
        }
        AudioOut2(audioOut2);

        // Chain 2 CV output
        if (currentNode2 >= 0 && nodeCount > 0) {
            int32_t pitch2 = nodes[currentNode2].pitch;
            if (quantize) {
                int32_t midiNote2 = 36 + (pitch2 * 60) / 4096;
                if (midiNote2 > 96) midiNote2 = 96;
                midiNote2 = applyBassMode(midiNote2, bassMode2);
                CVOut2MIDINote((uint8_t)midiNote2);
            } else {
                CVOut2((int16_t)(pitch2 - 2048));
            }
        } else {
            CVOut2(0);
        }

        // Chain 2 pulse output
        PulseOut2(gateActive2);

        // ═══════════════════════════════════════════
        // 9. LEDs — show current node ID in binary
        //    6 LEDs = 6 bits = IDs 0–63
        // ═══════════════════════════════════════════
        if (currentNode >= 0 && currentNode < nodeCount) {
            uint8_t id = nodes[currentNode].id;
            uint16_t brightness = gateActive ? 4095 : 2000;
            for (int i = 0; i < 6; i++) {
                if (id & (1 << i)) {
                    LedBrightness(i, brightness);
                } else {
                    LedOff(i);
                }
            }
        } else {
            for (int i = 0; i < 6; i++) LedOff(i);
        }

        // ISR timing probe — track worst case (measured before the flash pause,
        // so it reflects real DSP/graph work, not the intentional save stall)
        uint32_t isrTEnd = time_us_32();
        uint32_t isrDt = isrTEnd - isrT0;
        if (isrDt > isrMaxUs) isrMaxUs = isrDt;
        uint32_t dA = isrTA - isrT0;
        uint32_t dB = isrTB - isrTA;
        uint32_t dC = isrTEnd - isrTB;
        if (dA > isrMaxA) isrMaxA = dA;
        if (dB > isrMaxB) isrMaxB = dB;
        if (dC > isrMaxC) isrMaxC = dC;

        if (flashSaveActive) {
            core0InPause = true;
            while (flashSaveActive) tight_loop_contents();
            core0InPause = false;
        }
    }
};

// ═══════════════════════════════════════════════════════════════════
// Arduino entry points
// ═══════════════════════════════════════════════════════════════════
ZodiacCard card;

void setup() {
    initSineTable();
    seedRNG();

    // Load persisted settings from flash (XIP memory-mapped read, no library needed)
    const uint8_t* flash = (const uint8_t*)(XIP_BASE + FLASH_TARGET_OFFSET);
    if (flash[0] == FLASH_MAGIC) {
        uint8_t si = flash[1];
        if (si < NUM_SCALES) card.scaleIndex = si;
        uint32_t ai;
        memcpy(&ai, &flash[2], sizeof(ai));
        if (ai >= 6000 && ai <= 192000) card.autoInterval = ai;
        uint8_t wi = flash[6];
        if (wi < NUM_WAVEFORMS) card.waveformIndex1 = wi;
        card.bassMode1 = (flash[7] == 1);
        uint8_t wi2 = flash[8];
        if (wi2 < NUM_WAVEFORMS) card.waveformIndex2 = wi2;
        card.bassMode2 = (flash[9] == 1);
        uint8_t mn = flash[10];
        if (mn >= 2 && mn <= MAX_NODES) card.maxNodes = mn;  // 0xFF on pre-existing flash → keep default
    }

    card.nextMidiServiceTime = get_absolute_time();
    card.EnableNormalisationProbe();
    card.beginMIDI("Zodiac Card");

    // NOTE: do NOT touch core-0 IRQ priorities. Both directions were tried
    // and BOTH kill the MIDI stream (card enumerates but never transmits):
    //   - demoting USBCTRL_IRQ below the audio DMA (0xC0)   → dead stream
    //   - raising DMA_IRQ_0 above USB (0x00, USB untouched) → dead stream
    // The USB stack evidently requires its IRQ to outrank the audio DMA.
    // Consequence: USB preemption adds ~20-25µs to occasional ProcessSample
    // wall times (visible in ISR_SECT as inflated no-op sections). That tax
    // is tolerable as long as REAL ISR work stays small — the ADC FIFO only
    // overflows past ~62µs total — so keep per-sample graph ops minimal.
}

void loop() {
    card.Run();
}
