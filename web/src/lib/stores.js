import { writable } from 'svelte/store';

/**
 * @typedef {{ id: number, pitch: number, links: Array<{target: number, weight: number}> }} GraphNode
 * @typedef {{ source: number, target: number, weight: number }} GraphLink
 */

/** @type {import('svelte/store').Writable<GraphNode[]>} */
export const graphNodes = writable([]);

/** @type {import('svelte/store').Writable<GraphLink[]>} */
export const graphLinks = writable([]);

/** @type {import('svelte/store').Writable<number>} */
export const currentNodeId = writable(-1);

/** @type {import('svelte/store').Writable<number>} */
export const currentNode2Id = writable(-1);

/** @type {import('svelte/store').Writable<number>} */
export const nodeCount = writable(0);

/** @type {import('svelte/store').Writable<number>} */
export const knobMain = writable(0);

/** @type {import('svelte/store').Writable<number>} */
export const knobX = writable(0);

/** @type {import('svelte/store').Writable<number>} */
export const knobY = writable(0);

/** @type {import('svelte/store').Writable<number>} */
export const switchState = writable(0);

/** @type {import('svelte/store').Writable<boolean>} */
export const midiConnected = writable(false);

/** @type {import('svelte/store').Writable<string[]>} */
export const deviceNames = writable([]);

/** @type {import('svelte/store').Writable<string>} */
export const selectedDevice = writable('');

/** @type {import('svelte/store').Writable<number>} */
export const scaleIndex = writable(1);

/** @type {import('svelte/store').Writable<number>} */
export const autoInterval = writable(61);
