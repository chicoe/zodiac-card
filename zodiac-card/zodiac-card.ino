/*
  Chains Sequencer — Music Thing Modular Workshop System
  Markov chain sequencer: nodes with weighted links form a probabilistic graph.
  Two independent chains traverse the same graph with separate clocks.

  Core 0: ProcessSample() at 24kHz — audio, clock, chain traversal, LEDs.
  Core 1: MIDICore() — USB MIDI CC/SysEx to browser UI.

  Controls:
    Knob X:   Speed for chain 1 (internal clock)
    Knob Main / CV 1:  Link probability for new nodes
    Knob Y / CV 2:     Pitch range for new nodes
    Switch Up:    Auto mode — creates a new node every N seconds (configurable)
    Switch Mid:   Normal operation (scale-quantized)
    Switch Down flick:  Cycle to next scale
    MIDI Note On: Create node (note=pitch, velocity=probability)

  Chain 1: internal clock (Knob X) or Pulse 1 → Audio 1, CV 1, Pulse 1
  Chain 2: Pulse 2 clock (if connected) → Audio 2, CV 2, Pulse 2

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
static constexpr int      MAX_NODES            = 16;
static constexpr int      MAX_LINKS            = 8;
static constexpr uint32_t SAMPLE_RATE          = 24000;   // ProcessSample called at 24kHz (192kHz ADC / 8 DMA transfers)
static constexpr uint32_t MIN_STEP_INTERVAL    = 2400;    // 50ms  (~20 Hz)
static constexpr uint32_t MAX_STEP_INTERVAL    = 96000;   // 2s    (~0.5 Hz)
static constexpr uint32_t DEFAULT_STEP_INTERVAL= 24000;   // 500ms (~2 Hz)
static constexpr uint32_t GATE_LENGTH_PCT      = 50;      // gate = 50% of step
static constexpr uint32_t SINE_TABLE_SIZE      = 256;
static constexpr int      NUM_SCALES           = 8;

// Flash persistence — last 4KB sector
// Layout: [0] magic | [1] scaleIndex | [2-5] autoInterval (uint32_t)
static constexpr uint8_t  FLASH_MAGIC         = 0xAC;
static constexpr uint32_t FLASH_TARGET_OFFSET = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;

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

static uint32_t fastRandom() {
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
    volatile int8_t currentNode = -1;     // -1 = no nodes (array index)
    int8_t  lastCreatedNode = -1;          // last node added (array index)
    volatile uint32_t graphSeq = 0;       // seqlock for cross-core sync
    uint32_t nextBirthOrder = 0;           // monotonically increasing age counter

    // Allocate the smallest unused node ID in 0–126 range
    uint8_t allocateNodeId() {
        bool used[127] = {};
        for (int i = 0; i < nodeCount; i++) {
            if (nodes[i].id < 127) used[nodes[i].id] = true;
        }
        for (uint8_t id = 0; id < 127; id++) {
            if (!used[id]) return id;
        }
        return 0;  // fallback (shouldn't happen with MAX_NODES=16)
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

    // --- Switch gesture detection ---
    bool     prevSwitchUp     = true;   // init true to avoid false edge on boot
    bool     prevSwitchDown   = true;   // init true to avoid false edge on boot

    // --- Auto mode (Switch Up) ---
    bool     autoMode         = false;
    bool     autoMakeNode     = false;
    uint32_t autoTimer        = 0;
    volatile uint32_t autoInterval = 4 * SAMPLE_RATE;  // default 4s (96000 samples)

    // --- Quantization ---
    bool quantize = true;
    bool useChromatic = false;
    volatile uint8_t scaleIndex = 1;  // index into SCALES[], default Major

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

    // --- SysEx message types ---
    static constexpr uint8_t MSG_NODE         = 0x02;
    static constexpr uint8_t MSG_DELETE_NODE   = 0x10;
    static constexpr uint8_t MSG_DELETE_LINK   = 0x11;
    static constexpr uint8_t MSG_ADD_LINK      = 0x12;
    static constexpr uint8_t MSG_PULL_REQUEST  = 0x13;
    static constexpr uint8_t MSG_SET_SCALE     = 0x14;
    static constexpr uint8_t MSG_ADD_NODE      = 0x15;
    static constexpr uint8_t MSG_SET_AUTO_INT   = 0x16;

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

        flashSaveActive = true;
        while (!core0InPause) tight_loop_contents();
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
        flash_range_program(FLASH_TARGET_OFFSET, buf, FLASH_SECTOR_SIZE);
        restore_interrupts(ints);
        flashSaveActive = false;
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
    int32_t randomPitchInRange(int32_t knobVal) {
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

    uint32_t pitchToPhaseInc(int32_t pitch) {
        float semitones;
        if (quantize) {
            int32_t midiNote = 36 + (pitch * 60 + 2048) / 4096;
            if (midiNote < 36) midiNote = 36;
            if (midiNote > 96) midiNote = 96;
            midiNote = (int32_t)quantizeToScale(midiNote);
            semitones = (float)(midiNote - 69);
        } else {
            float fNote = 36.0f + (pitch * 60.0f) / 4096.0f;
            semitones = fNote - 69.0f;
        }
        float freq = 440.0f * powf(2.0f, semitones / 12.0f);
        return (uint32_t)(freq * 65536.0f * SINE_TABLE_SIZE / SAMPLE_RATE);
    }

    void updatePhaseIncrement(int32_t pitch) {
        phaseIncrement = pitchToPhaseInc(pitch);
    }

    void updatePhaseIncrement2(int32_t pitch) {
        phaseIncrement2 = pitchToPhaseInc(pitch);
    }

    // ───────────────────────────────────────────────────────────
    // Find index of youngest node (highest id), optionally excluding one
    // ───────────────────────────────────────────────────────────
    int8_t findYoungestNode(int8_t excludeIdx = -1) {
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
    int8_t findOldestNode() {
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
    void advanceChain() {
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
    void advanceChain2() {
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
    void addNode(int32_t pitch, uint16_t probability) {
        graphSeq++;  // begin atomic write — entire delete+cleanup+add is one operation

        // At capacity: delete the oldest node (lowest id) to make room
        if (nodeCount >= MAX_NODES) {
            int8_t oldestIdx = findOldestNode();
            if (oldestIdx >= 0) {
                deleteNodeBody(oldestIdx);
                cleanupStrayNodesBody();
            }
        }

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

            // Extra links: probability 0–4095 maps to 0–4 extra links
            // 0% = 0 extra, ~25% = 1, ~50% = 2, ~75% = 3, ~99% = 4
            int maxExtra = (int)((uint32_t)probability * 4 / 4095);
            if (maxExtra > 4) maxExtra = 4;

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
    // Delete a node: remove it, fix all link targets, compact
    // Does NOT touch graphSeq — caller must manage the seqlock.
    // ───────────────────────────────────────────────────────────
    void deleteNodeBody(int8_t idx) {
        if (idx < 0 || idx >= nodeCount) return;

        // Remove links to deleted node from all other nodes, adjust targets
        for (int i = 0; i < nodeCount; i++) {
            if (i == idx) continue;
            Node &n = nodes[i];
            for (int j = 0; j < n.linkCount; ) {
                if (n.links[j].target == (uint8_t)idx) {
                    // Remove link by shifting
                    for (int k = j; k < n.linkCount - 1; k++) {
                        n.links[k] = n.links[k + 1];
                    }
                    n.linkCount--;
                    n.synced = false;
                } else {
                    if (n.links[j].target > (uint8_t)idx) {
                        n.links[j].target--;
                    }
                    j++;
                }
            }
        }

        // Shift nodes down to compact
        for (int i = idx; i < nodeCount - 1; i++) {
            nodes[i] = nodes[i + 1];
            nodes[i].synced = false;
        }
        nodeCount--;

        // Fix currentNode
        if (nodeCount == 0) {
            currentNode = -1;
            phaseIncrement = 0;
            gateActive = false;
        } else if (currentNode == idx) {
            currentNode = (int8_t)(fastRandom() % nodeCount);
            updatePhaseIncrement(nodes[currentNode].pitch);
        } else if (currentNode > idx) {
            currentNode--;
        }

        // Fix currentNode2
        if (nodeCount == 0) {
            currentNode2 = -1;
            phaseIncrement2 = 0;
            gateActive2 = false;
        } else if (currentNode2 == idx) {
            currentNode2 = (int8_t)(fastRandom() % nodeCount);
            updatePhaseIncrement2(nodes[currentNode2].pitch);
        } else if (currentNode2 > idx) {
            currentNode2--;
        }

        // Fix lastCreatedNode
        if (nodeCount == 0) {
            lastCreatedNode = -1;
        } else if (lastCreatedNode == idx) {
            lastCreatedNode = -1;  // reset — next addNode will fall back to currentNode
        } else if (lastCreatedNode > idx) {
            lastCreatedNode--;
        }

        // Mark all nodes as unsynced after compaction (indices changed)
        for (int i = 0; i < nodeCount; i++) {
            nodes[i].synced = false;
        }
    }

    // Wrapper with seqlock for standalone delete calls
    void deleteNode(int8_t idx) {
        graphSeq++;
        deleteNodeBody(idx);
        graphSeq++;
    }

    // ───────────────────────────────────────────────────────────
    // Find array index by node ID. Returns -1 if not found.
    // ─────────────────────────────────────────────────────────────
    int8_t findNodeByID(uint8_t id) {
        for (int i = 0; i < nodeCount; i++) {
            if (nodes[i].id == id) return (int8_t)i;
        }
        return -1;
    }

    // ─────────────────────────────────────────────────────────────
    // Delete a specific link from a node (by source and target IDs)
    // ─────────────────────────────────────────────────────────────
    void deleteLinkByID(uint8_t sourceId, uint8_t targetId) {
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
    void addLinkByID(uint8_t sourceId, uint8_t targetId, uint16_t weight) {
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
    void deleteNodeByID(uint8_t id) {
        int8_t idx = findNodeByID(id);
        if (idx >= 0) deleteNode(idx);
    }

    // ─────────────────────────────────────────────────────────────
    // Repair graph after node deletion:
    //   - Nodes with no incoming links get a link from the youngest node
    //   - Nodes with no outgoing links get a link to the oldest node
    // Does NOT touch graphSeq — caller must manage the seqlock.
    // ─────────────────────────────────────────────────────────────
    void cleanupStrayNodesBody() {
        if (nodeCount <= 1) return;

        int8_t youngestIdx = findYoungestNode();
        int8_t oldestIdx   = findOldestNode();

        // Fix nodes with no incoming links: youngest → them
        for (int i = 0; i < nodeCount; i++) {
            if (i == youngestIdx) continue;  // youngest is the source, skip
            bool hasIncoming = false;
            for (int j = 0; j < nodeCount; j++) {
                if (j == i) continue;
                for (int k = 0; k < nodes[j].linkCount; k++) {
                    if (nodes[j].links[k].target == (uint8_t)i) {
                        hasIncoming = true;
                        break;
                    }
                }
                if (hasIncoming) break;
            }
            if (!hasIncoming && youngestIdx >= 0) {
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
            flashSaveDirty = false;
            saveToFlash();
        }

        if (!time_reached(nextMidiServiceTime)) return;
        nextMidiServiceTime = make_timeout_time_us(1000);

        static int16_t lastCN = -2, lastNC = -1;
        static int16_t lastKM = -1, lastKX = -1, lastKY = -1, lastSW = -1;
        static int16_t lastCN2 = -2, lastScale = -1, lastAutoInt = -1;
        uint64_t now = time_us_64();

        // CC: Current node (send node ID, not array index)
        int16_t curCN = (currentNode >= 0 && currentNode < nodeCount)
            ? (int16_t)nodes[currentNode].id : -1;
        if (curCN != lastCN) {
            SendCC(CC_CURRENT_NODE, curCN >= 0 ? (uint8_t)curCN : 127);
            lastCN = curCN;
        }

        // CC: Current node 2
        int16_t curCN2 = (currentNode2 >= 0 && currentNode2 < nodeCount)
            ? (int16_t)nodes[currentNode2].id : -1;
        if (curCN2 != lastCN2) {
            SendCC(CC_CURRENT_NODE2, curCN2 >= 0 ? (uint8_t)curCN2 : 127);
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

        // Periodic full CC resend every 2s
        static uint64_t lastPeriodicTime = 0;
        if (now - lastPeriodicTime >= 2000000) {
            lastPeriodicTime = now;
            lastCN = -2; lastNC = -1; lastCN2 = -2; lastScale = -1; lastAutoInt = -1;
            lastKM = -1; lastKX = -1; lastKY = -1; lastSW = -1;
        }

        // Handle full resync request
        if (fullResyncRequested) {
            fullResyncRequested = false;
            for (int i = 0; i < nodeCount; i++) {
                nodes[i].synced = false;
            }
            lastCN = -2; lastNC = -1; lastCN2 = -2; lastScale = -1; lastAutoInt = -1;
            lastKM = -1; lastKX = -1; lastKY = -1; lastSW = -1;
        }

        // SysEx: Node sync (one node per ~10ms)
        static uint64_t lastSysExTime = 0;
        if (now - lastSysExTime >= 10000) {
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

    // ───────────────────────────────────────────────────────────
    // ProcessSample — Core 0, 48kHz ISR
    // ───────────────────────────────────────────────────────────
    void __not_in_flash_func(ProcessSample)() override {

        // ═══════════════════════════════════════════
        // 0. PROCESS PENDING EDITS FROM UI
        // ═══════════════════════════════════════════
        {
            uint8_t cmd = pendingEdit.cmd;
            if (cmd != 0) {
                uint8_t nid = pendingEdit.nodeId;
                uint8_t tid = pendingEdit.targetId;
                uint16_t w  = pendingEdit.weight;
                pendingEdit.cmd = 0;  // consume

                if (cmd == MSG_DELETE_NODE) {
                    deleteNodeByID(nid);
                    cleanupStrayNodes();
                } else if (cmd == MSG_DELETE_LINK) {
                    deleteLinkByID(nid, tid);
                    cleanupStrayNodes();
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
                    addNode(pitch, (uint16_t)prob);
                }
            }
        }

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

        // --- Auto-create node on next sequence step ---
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
            addNode(pitch, (uint16_t)prob);
        }

        // ═══════════════════════════════════════════
        // 3. CLOCK — advance chain
        // ═══════════════════════════════════════════
        bool externalClock = Connected(Input::Pulse1);
        bool shouldAdvance = false;

        if (externalClock) {
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
            int32_t knobX = KnobVal(Knob::X);
            stepInterval = MIN_STEP_INTERVAL
                + ((uint32_t)(4095 - knobX)
                   * (MAX_STEP_INTERVAL - MIN_STEP_INTERVAL)) / 4095;
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

        // ═══════════════════════════════════════════
        // 5. AUDIO — sine wave of current node pitch
        // ═══════════════════════════════════════════
        int16_t audioOut = 0;
        if (currentNode >= 0 && nodeCount > 0) {
            sinePhase += phaseIncrement;
            audioOut = sineTable[(sinePhase >> 16) & 0xFF];
        }
        AudioOut1(audioOut);

        // ═══════════════════════════════════════════
        // 6. CV OUTPUT — 1V/oct
        // ═══════════════════════════════════════════
        if (currentNode >= 0 && nodeCount > 0) {
            int32_t pitch = nodes[currentNode].pitch;
            if (quantize) {
                uint8_t midiNote = (uint8_t)(36 + (pitch * 60) / 4096);
                if (midiNote > 96) midiNote = 96;
                CVOut1MIDINote(midiNote);
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

        bool externalClock2 = Connected(Input::Pulse2);
        bool shouldAdvance2 = false;

        if (externalClock2) {
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
            // No external clock: use Knob X value directly
            int32_t knobX2 = KnobVal(Knob::X);
            stepInterval2 = MIN_STEP_INTERVAL
                + ((uint32_t)(4095 - knobX2)
                   * (MAX_STEP_INTERVAL - MIN_STEP_INTERVAL)) / 4095;
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
            audioOut2 = sineTable[(sinePhase2 >> 16) & 0xFF];
        }
        AudioOut2(audioOut2);

        // Chain 2 CV output
        if (currentNode2 >= 0 && nodeCount > 0) {
            int32_t pitch2 = nodes[currentNode2].pitch;
            if (quantize) {
                uint8_t midiNote2 = (uint8_t)(36 + (pitch2 * 60) / 4096);
                if (midiNote2 > 96) midiNote2 = 96;
                CVOut2MIDINote(midiNote2);
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
    }

    card.nextMidiServiceTime = get_absolute_time();
    card.EnableNormalisationProbe();
    card.beginMIDI("Zodiac Card");
}

void loop() {
    card.Run();
}
