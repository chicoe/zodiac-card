import {
	graphNodes,
	graphLinks,
	currentNodeId,
	currentNode2Id,
	nodeCount,
	knobMain,
	knobX,
	knobY,
	switchState,
	midiConnected,
	deviceNames,
	selectedDevice,
	scaleIndex,
	autoInterval,
	waveformIndex1,
	bassMode1,
	waveformIndex2,
	bassMode2,
	bpm1,
	bpm2,
	clockMode,
	clockMultLevel,
	isrPeakUs,
	isrSections,
	rxMsgCount,
	maxNodes,
	noteTxChannel,
	statusRxChannel
} from './stores.js';

// ── Protocol constants (must match firmware) ──
const CC_CURRENT_NODE = 20;
const CC_NODE_COUNT = 21;
const CC_KNOB_MAIN = 22;
const CC_KNOB_X = 23;
const CC_KNOB_Y = 24;
const CC_SWITCH = 25;
const CC_CURRENT_NODE2 = 26;
const CC_SCALE = 27;
const CC_AUTO_INTERVAL = 28;
const CC_WAVEFORM = 29;
const CC_BASS_MODE = 30;
const CC_WAVEFORM2 = 31;
const CC_BASS_MODE2 = 33;
const CC_MAX_NODES = 34;
const MSG_NODE = 0x02;
const MSG_CLOCK_STATUS = 0x03;
const MSG_PERF = 0x04;
const MSG_NODE_DELETED = 0x05;
const MSG_ROSTER = 0x06;
const MSG_DELETE_NODE = 0x10;
const MSG_DELETE_LINK = 0x11;
const MSG_ADD_LINK = 0x12;
const MSG_PULL_REQUEST = 0x13;
const MSG_SET_SCALE = 0x14;
const MSG_ADD_NODE = 0x15;
const MSG_SET_AUTO_INT = 0x16;
const MSG_SET_WAVEFORM = 0x17;
const MSG_SET_BASS_MODE = 0x18;
const MSG_SET_WAVEFORM2 = 0x19;
const MSG_SET_BASS_MODE2 = 0x1a;
const MSG_SET_MAX_NODES = 0x1b;
const MANUFACTURER_ID = 0x7d;

/** @type {MIDIOutput | null} */
let midiOutput = null;
/** @type {MIDIInput | null} */
let midiInput = null;
/** @type {MIDIAccess | null} */
let midiAccess = null;

// Configurable MIDI channels for non-Zodiac routing (hubs, IAC driver, DAWs).
// The card itself is fixed: it listens for notes on ch 1, sends status CCs on
// ch 1 and chain 1/2 notes on ch 2/3. When talking to an intermediary (IAC bus
// that also carries other traffic), these set the channels *we* use on that
// bus — the router is responsible for mapping them to the card's channels.
// Persisted in localStorage so an IAC setup survives reloads.
let noteTxCh = 0;    // 0-based channel for outgoing Note On (node creation)
let statusRxCh = -1; // 0-based channel filter for incoming CCs; -1 = accept any
let connectedDeviceIsCard = true; // filter only applies when routing via a non-card device

const CHANNEL_STORAGE_KEY = 'zodiac-midi-channels';

function saveChannelSettings() {
	try {
		localStorage.setItem(CHANNEL_STORAGE_KEY, JSON.stringify({
			tx: noteTxCh + 1,
			rx: statusRxCh < 0 ? 0 : statusRxCh + 1
		}));
	} catch { /* private mode etc. — non-fatal */ }
}

/** Set the 1-based channel used for outgoing notes */
export function setNoteTxChannel(ch) {
	noteTxCh = (ch - 1) & 0x0f;
	noteTxChannel.set(ch);
	saveChannelSettings();
}

/** Set the 1-based channel filter for incoming status CCs (0 = any) */
export function setStatusRxChannel(ch) {
	statusRxCh = ch <= 0 ? -1 : (ch - 1) & 0x0f;
	statusRxChannel.set(ch);
	saveChannelSettings();
}

// Restore persisted channel settings (browser only; ssr is disabled)
if (typeof localStorage !== 'undefined') {
	try {
		const s = JSON.parse(localStorage.getItem(CHANNEL_STORAGE_KEY) || '{}');
		if (Number.isInteger(s.tx) && s.tx >= 1 && s.tx <= 16) {
			noteTxCh = (s.tx - 1) & 0x0f;
			noteTxChannel.set(s.tx);
		}
		if (Number.isInteger(s.rx) && s.rx >= 1 && s.rx <= 16) {
			statusRxCh = (s.rx - 1) & 0x0f;
			statusRxChannel.set(s.rx);
		}
	} catch { /* corrupt entry — keep defaults */ }
}

/** True if a device name matches the auto-detect list (i.e. is the card itself) */
export function isAutoDetectedName(name) {
	return AUTO_DETECT_NAMES.some((k) => name && name.toLowerCase().includes(k.toLowerCase()));
}


// Internal node map for incremental sync (keyed by node ID)
/** @type {Map<number, {pitch: number, links: Array<{target: number, weight: number}>}>} */
const internalNodes = new Map();
let totalNodeCount = 0;
let rosterMissingStreak = 0; // consecutive rosters listing nodes we don't have

/** Clear local state and request a full graph dump from firmware */
export function requestPull() {
	if (!midiOutput) return;
	clearGraphState();
	midiOutput.send([0xf0, MANUFACTURER_ID, MSG_PULL_REQUEST, 0xf7]);
}

/** Send delete node command to firmware */
export function sendDeleteNode(nodeId) {
	if (!midiOutput) return;
	midiOutput.send([0xf0, MANUFACTURER_ID, MSG_DELETE_NODE, nodeId & 0x7f, 0xf7]);
}

/** Send delete link command to firmware */
export function sendDeleteLink(sourceId, targetId) {
	if (!midiOutput) return;
	midiOutput.send([
		0xf0, MANUFACTURER_ID, MSG_DELETE_LINK,
		sourceId & 0x7f, targetId & 0x7f,
		0xf7
	]);
}

/** Send add link command to firmware */
export function sendAddLink(sourceId, targetId, weight = 2048) {
	if (!midiOutput) return;
	midiOutput.send([
		0xf0, MANUFACTURER_ID, MSG_ADD_LINK,
		sourceId & 0x7f, targetId & 0x7f,
		(weight >> 7) & 0x7f, weight & 0x7f,
		0xf7
	]);
}

/** Send a MIDI Note On to create a node (note=pitch, velocity=probability) */
export function sendNoteOn(note, velocity = 100) {
	if (!midiOutput) return;
	midiOutput.send([0x90 | noteTxCh, note & 0x7f, velocity & 0x7f]);
}

/** Send set scale command to firmware */
export function sendSetScale(index) {
	scaleIndex.set(index);
	if (!midiOutput) return;
	midiOutput.send([0xf0, MANUFACTURER_ID, MSG_SET_SCALE, index & 0x7f, 0xf7]);
}

/** Send set auto interval command to firmware (0–127) */
export function sendSetAutoInterval(val) {
	autoInterval.set(val);
	if (!midiOutput) return;
	midiOutput.send([0xf0, MANUFACTURER_ID, MSG_SET_AUTO_INT, val & 0x7f, 0xf7]);
}

/** Send set max nodes command to firmware (2–16) */
export function sendSetMaxNodes(val) {
	maxNodes.set(val);
	if (!midiOutput) return;
	midiOutput.send([0xf0, MANUFACTURER_ID, MSG_SET_MAX_NODES, val & 0x7f, 0xf7]);
}

/** Send set waveform command for chain 1 (0–7) */
export function sendSetWaveform1(idx) {
	waveformIndex1.set(idx);
	if (!midiOutput) return;
	midiOutput.send([0xf0, MANUFACTURER_ID, MSG_SET_WAVEFORM, idx & 0x7f, 0xf7]);
}

/** Send set bass mode command for chain 1 */
export function sendSetBassMode1(enabled) {
	bassMode1.set(enabled);
	if (!midiOutput) return;
	midiOutput.send([0xf0, MANUFACTURER_ID, MSG_SET_BASS_MODE, enabled ? 1 : 0, 0xf7]);
}

/** Send set waveform command for chain 2 (0–7) */
export function sendSetWaveform2(idx) {
	waveformIndex2.set(idx);
	if (!midiOutput) return;
	midiOutput.send([0xf0, MANUFACTURER_ID, MSG_SET_WAVEFORM2, idx & 0x7f, 0xf7]);
}

/** Send set bass mode command for chain 2 */
export function sendSetBassMode2(enabled) {
	bassMode2.set(enabled);
	if (!midiOutput) return;
	midiOutput.send([0xf0, MANUFACTURER_ID, MSG_SET_BASS_MODE2, enabled ? 1 : 0, 0xf7]);
}

/** Clear all graph state — called when connection is (re)established */
function clearGraphState() {
	internalNodes.clear();
	totalNodeCount = 0;
	graphNodes.set([]);
	graphLinks.set([]);
	currentNodeId.set(-1);
	currentNode2Id.set(-1);
	nodeCount.set(0);
}

/** Rebuild Svelte stores from internal node map */
function rebuildGraph() {
	/** @type {Array<{id: number, pitch: number, links: Array<{target: number, weight: number}>}>} */
	const nodes = [];
	/** @type {Array<{source: number, target: number, weight: number}>} */
	const links = [];

	const knownIds = new Set(internalNodes.keys());

	for (const [id, data] of internalNodes) {
		nodes.push({ id, pitch: data.pitch, links: data.links });
		for (const link of data.links) {
			// Only add links to nodes we already know about
			if (knownIds.has(link.target)) {
				links.push({ source: id, target: link.target, weight: link.weight });
			}
		}
	}

	graphNodes.set(nodes);
	graphLinks.set(links);
}

/** @param {MIDIMessageEvent} event */
function handleMIDI(event) {
	const data = event.data;
	if (!data || data.length === 0) return;

	// Raw activity counter, before any filtering — proves the wire is alive
	rxMsgCount.update((n) => n + 1);

	const status = data[0] & 0xf0;

	if (status === 0xb0 && data.length >= 3) {
		// CC message — the channel filter only applies when routing through a
		// non-card device (IAC/hub). Direct card connections always pass:
		// a persisted filter must never silently mute the card itself.
		if (!connectedDeviceIsCard && statusRxCh >= 0 && (data[0] & 0x0f) !== statusRxCh) return;
		const cc = data[1];
		const val = data[2];
		switch (cc) {
			case CC_CURRENT_NODE:
				currentNodeId.set(val === 127 ? -1 : val);
				break;
			case CC_NODE_COUNT:
				nodeCount.set(val);
				totalNodeCount = val;
				break;
			case CC_KNOB_MAIN:
				knobMain.set(val << 5);
				break;
			case CC_KNOB_X:
				knobX.set(val << 5);
				break;
			case CC_KNOB_Y:
				knobY.set(val << 5);
				break;
			case CC_SWITCH:
				switchState.set(val);
				break;
			case CC_CURRENT_NODE2:
				currentNode2Id.set(val === 127 ? -1 : val);
				break;
			case CC_SCALE:
				scaleIndex.set(val);
				break;
			case CC_AUTO_INTERVAL:
				autoInterval.set(val);
				break;
			case CC_WAVEFORM:
				waveformIndex1.set(val);
				break;
			case CC_BASS_MODE:
				bassMode1.set(val !== 0);
				break;
			case CC_WAVEFORM2:
				waveformIndex2.set(val);
				break;
			case CC_BASS_MODE2:
				bassMode2.set(val !== 0);
				break;
			case CC_MAX_NODES:
				maxNodes.set(val);
				break;
		}
	} else if (data[0] === 0xf0) {
		// SysEx
		handleSysEx(data);
	}
}

/** @param {Uint8Array} data */
function handleSysEx(data) {
	// F0 7D [payload...] F7
	if (data.length < 4 || data[1] !== MANUFACTURER_ID) return;
	const payload = data.slice(2, data.length - 1);
	if (payload.length === 0) return;

	if (payload[0] === MSG_PERF && payload.length >= 3) {
		isrPeakUs.set((payload[1] << 7) | payload[2]);
		if (payload.length >= 9) {
			isrSections.set([
				(payload[3] << 7) | payload[4],
				(payload[5] << 7) | payload[6],
				(payload[7] << 7) | payload[8]
			]);
		}
		return;
	}

	if (payload[0] === MSG_NODE_DELETED && payload.length >= 2) {
		// Explicit deletion from firmware — exact, replaces the old
		// "evict lowest ID on count drop" guess
		if (internalNodes.delete(payload[1])) rebuildGraph();
		return;
	}

	if (payload[0] === MSG_ROSTER && payload.length >= 2) {
		// Authoritative list of live node IDs (every ~2s): prune anything
		// the firmware no longer has; if we're missing nodes for two
		// consecutive rosters, an incremental message was lost — re-pull.
		const cnt = payload[1];
		const liveIds = new Set();
		for (let i = 0; i < cnt && 2 + i < payload.length; i++) liveIds.add(payload[2 + i]);

		let changed = false;
		for (const id of [...internalNodes.keys()]) {
			if (!liveIds.has(id)) {
				internalNodes.delete(id);
				changed = true;
			}
		}

		let missing = 0;
		for (const id of liveIds) if (!internalNodes.has(id)) missing++;
		if (missing > 0) {
			rosterMissingStreak++;
			if (rosterMissingStreak >= 2) {
				rosterMissingStreak = 0;
				requestPull();
				return;
			}
		} else {
			rosterMissingStreak = 0;
		}

		if (changed) rebuildGraph();
		return;
	}

	if (payload[0] === MSG_CLOCK_STATUS && payload.length >= 7) {
		clockMode.set(payload[1]);
		bpm1.set((payload[2] << 7) | payload[3]);
		bpm2.set((payload[4] << 7) | payload[5]);
		clockMultLevel.set(payload[6] - 64);   // undo firmware +64 bias → signed level
		return;
	}

	if (payload[0] === MSG_NODE && payload.length >= 7) {
		const nodeIdx = payload[1];
		const total = payload[2];
		const nodeId = payload[3];
		const pitch = (payload[4] << 7) | payload[5];
		const linkCount = payload[6];

		/** @type {Array<{target: number, weight: number}>} */
		const links = [];
		for (let i = 0; i < linkCount; i++) {
			const offset = 7 + i * 3;
			if (offset + 2 < payload.length) {
				links.push({
					target: payload[offset],
					weight: (payload[offset + 1] << 7) | payload[offset + 2]
				});
			}
		}

		internalNodes.set(nodeId, { pitch, links });
		totalNodeCount = total;
		rebuildGraph();
	}
}

/**
 * Try to auto-connect to "Chains Sequencer" MIDI device.
 * @param {MIDIAccess} access
 */
/** Refresh the list of available MIDI device names */
function refreshDeviceList(access) {
	/** @type {string[]} */
	const names = [];
	for (const output of access.outputs.values()) {
		if (output.name) names.push(output.name);
	}
	deviceNames.set(names);
}

/** Names to auto-detect as the sequencer device */
const AUTO_DETECT_NAMES = ['Zodiac', 'pico'];

function autoConnect(access, allowFallback = false) {
	refreshDeviceList(access);

	// Skip if already connected to open ports (e.g. a different device's statechange)
	if (midiOutput?.state === 'connected' && midiInput?.state === 'connected') return;

	if (midiInput) midiInput.onmidimessage = null;
	midiOutput = null;
	midiInput = null;

	// Try to find a known device by name
	for (const keyword of AUTO_DETECT_NAMES) {
		if (midiOutput) break;
		for (const output of access.outputs.values()) {
			if (output.name && output.name.toLowerCase().includes(keyword.toLowerCase())) {
				midiOutput = output;
				break;
			}
		}
	}
	for (const keyword of AUTO_DETECT_NAMES) {
		if (midiInput) break;
		for (const input of access.inputs.values()) {
			if (input.name && input.name.toLowerCase().includes(keyword.toLowerCase())) {
				midiInput = input;
				input.onmidimessage = handleMIDI;
				break;
			}
		}
	}

	// If no known device found by name, optionally fall back to a lone device.
	// Only on an explicit user connect: on automatic statechange reconnects
	// (e.g. the card resetting), falling back would silently grab whatever is
	// left (IAC driver, a hub) — losing the card should just disconnect, and
	// the card is re-grabbed by name when it reappears.
	if ((!midiOutput || !midiInput) && allowFallback) {
		const outputs = [...access.outputs.values()];
		const inputs = [...access.inputs.values()];
		if (outputs.length === 1 && inputs.length >= 1) {
			midiOutput = outputs[0];
			midiInput = inputs[0];
			midiInput.onmidimessage = handleMIDI;
		}
	}

	if (midiOutput && midiInput) {
		connectedDeviceIsCard = isAutoDetectedName(midiOutput.name || '');
		midiConnected.set(true);
		selectedDevice.set(midiOutput.name || '');
		clearGraphState();
		requestPull();
	} else {
		midiConnected.set(false);
	}
}

/** Forward Note On/Off from external MIDI controllers to the sequencer */
function handleExternalMIDI(event) {
	const data = event.data;
	if (!data || data.length < 3 || !midiOutput) return;
	const status = data[0] & 0xf0;
	// Forward Note On and Note Off (channel 1 only) to the sequencer,
	// re-stamped onto the configured TX channel
	if ((status === 0x90 || status === 0x80) && (data[0] & 0x0f) === 0) {
		midiOutput.send([status | noteTxCh, data[1], data[2]]);
	}
}

/** Listen to all MIDI inputs except the sequencer and forward notes */
function setupExternalInputForwarding(access) {
	for (const input of access.inputs.values()) {
		if (input !== midiInput) {
			input.onmidimessage = handleExternalMIDI;
		}
	}
}

/** Connect to MIDI (called from UI) */
export async function connectMIDI() {
	try {
		midiAccess = await navigator.requestMIDIAccess({ sysex: true });
		autoConnect(midiAccess, true);   // explicit user connect → fallback allowed
		setupExternalInputForwarding(midiAccess);
		midiAccess.addEventListener('statechange', (event) => {
			if (!midiAccess) return;
			// If the state change is for our connected device, force-clear so
			// autoConnect will re-establish and call clearGraphState + requestPull.
			// This handles fast resets where the browser only sees one statechange
			// (the reconnect) rather than a disconnect/reconnect pair.
			const port = event.port;
			if (port === midiOutput || port === midiInput) {
				if (midiInput) midiInput.onmidimessage = null;
				midiOutput = null;
				midiInput = null;
				midiConnected.set(false);
			}
			autoConnect(midiAccess);
			setupExternalInputForwarding(midiAccess);
		});
	} catch (e) {
		console.error('MIDI access denied:', e);
	}
}

/**
 * Manually select a MIDI device by name
 * @param {string} name
 */
export function selectDevice(name) {
	if (!midiAccess) return;

	// Disconnect old input
	if (midiInput) midiInput.onmidimessage = null;
	midiOutput = null;
	midiInput = null;

	// Find output by name
	for (const output of midiAccess.outputs.values()) {
		if (output.name === name) {
			midiOutput = output;
			break;
		}
	}

	if (midiOutput) {
		// First try exact name match for input
		for (const input of midiAccess.inputs.values()) {
			if (input.name === name) {
				midiInput = input;
				break;
			}
		}
		// If no exact match, find input by matching manufacturer + port index
		// (MIDI devices often have different input/output names)
		if (!midiInput) {
			const outputs = [...midiAccess.outputs.values()];
			const inputs = [...midiAccess.inputs.values()];
			const outIdx = outputs.indexOf(midiOutput);
			if (outIdx >= 0 && outIdx < inputs.length) {
				midiInput = inputs[outIdx];
			} else if (inputs.length === 1) {
				// Only one input available — use it
				midiInput = inputs[0];
			}
		}
	}

	if (midiInput) {
		midiInput.onmidimessage = handleMIDI;
	}

	if (midiOutput && midiInput) {
		connectedDeviceIsCard = isAutoDetectedName(name);
		midiConnected.set(true);
		selectedDevice.set(name);
		clearGraphState();
		requestPull();
	} else {
		midiConnected.set(false);
		selectedDevice.set('');
	}
}
