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
	autoInterval
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
const MSG_NODE = 0x02;
const MSG_DELETE_NODE = 0x10;
const MSG_DELETE_LINK = 0x11;
const MSG_ADD_LINK = 0x12;
const MSG_PULL_REQUEST = 0x13;
const MSG_SET_SCALE = 0x14;
const MSG_ADD_NODE = 0x15;
const MSG_SET_AUTO_INT = 0x16;
const MANUFACTURER_ID = 0x7d;

/** @type {MIDIOutput | null} */
let midiOutput = null;
/** @type {MIDIInput | null} */
let midiInput = null;
/** @type {MIDIAccess | null} */
let midiAccess = null;

// Internal node map for incremental sync (keyed by node ID)
/** @type {Map<number, {pitch: number, links: Array<{target: number, weight: number}>}>} */
const internalNodes = new Map();
let totalNodeCount = 0;

/** Request full graph dump from firmware */
export function requestPull() {
	if (!midiOutput) return;
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
	midiOutput.send([0x90, note & 0x7f, velocity & 0x7f]);
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

/** Evict lowest-ID (oldest) entries from internalNodes until size <= target */
function evictOldest(targetSize) {
	while (internalNodes.size > targetSize) {
		let minId = Infinity, minKey = null;
		for (const id of internalNodes.keys()) {
			if (id < minId) { minId = id; minKey = id; }
		}
		if (minKey !== null) internalNodes.delete(minKey);
		else break;
	}
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

	const status = data[0] & 0xf0;

	if (status === 0xb0 && data.length >= 3) {
		// CC message
		const cc = data[1];
		const val = data[2];
		switch (cc) {
			case CC_CURRENT_NODE:
				currentNodeId.set(val === 127 ? -1 : val);
				break;
			case CC_NODE_COUNT:
				nodeCount.set(val);
				totalNodeCount = val;
				// If we have more entries than firmware reports, evict oldest
				if (internalNodes.size > val) {
					evictOldest(val);
					if (val === 0) rebuildGraph();
					else {
						rebuildGraph();
						requestPull();
					}
				}
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

		// Evict stale entries: firmware deletes oldest (lowest ID) first,
		// so remove lowest-ID entries until size matches total.
		if (internalNodes.size > total) {
			evictOldest(total);
		}

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

function autoConnect(access) {
	refreshDeviceList(access);

	// If already connected, skip
	if (midiOutput && midiInput) return;

	// Try to find "Chains" device first
	for (const output of access.outputs.values()) {
		if (output.name && output.name.includes('Chains')) {
			midiOutput = output;
			break;
		}
	}
	for (const input of access.inputs.values()) {
		if (input.name && input.name.includes('Chains')) {
			midiInput = input;
			input.onmidimessage = handleMIDI;
			break;
		}
	}

	// If no "Chains" device found, auto-connect if exactly one device
	if (!midiOutput || !midiInput) {
		const outputs = [...access.outputs.values()];
		const inputs = [...access.inputs.values()];
		if (outputs.length === 1 && inputs.length === 1) {
			midiOutput = outputs[0];
			midiInput = inputs[0];
			midiInput.onmidimessage = handleMIDI;
		}
	}

	if (midiOutput && midiInput) {
		midiConnected.set(true);
		selectedDevice.set(midiOutput.name || '');
		requestPull();
	} else {
		midiConnected.set(false);
	}
}

/** Connect to MIDI (called from UI) */
export async function connectMIDI() {
	try {
		midiAccess = await navigator.requestMIDIAccess({ sysex: true });
		autoConnect(midiAccess);
		midiAccess.addEventListener('statechange', () => {
			if (midiAccess) autoConnect(midiAccess);
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

	for (const output of midiAccess.outputs.values()) {
		if (output.name === name) {
			midiOutput = output;
			break;
		}
	}
	for (const input of midiAccess.inputs.values()) {
		if (input.name === name) {
			midiInput = input;
			input.onmidimessage = handleMIDI;
			break;
		}
	}
	if (midiOutput && midiInput) {
		midiConnected.set(true);
		selectedDevice.set(name);
		requestPull();
	} else {
		midiConnected.set(false);
		selectedDevice.set('');
	}
}
