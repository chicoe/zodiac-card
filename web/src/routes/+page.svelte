<script>
	import '@fontsource/monaspace-krypton';
	import { onMount, onDestroy } from 'svelte';
	import { forceSimulation, forceLink, forceManyBody, forceCenter, forceCollide } from 'd3-force';
	import { connectMIDI, selectDevice, requestPull, sendDeleteNode, sendDeleteLink, sendAddLink, sendNoteOn, sendSetScale, sendSetAutoInterval } from '$lib/midi.js';
	import {
		graphNodes, graphLinks, currentNodeId, currentNode2Id, nodeCount,
		knobMain, knobX, knobY, switchState,
		midiConnected, deviceNames, selectedDevice, scaleIndex, autoInterval
	} from '$lib/stores.js';

	// ═══════════════════════════════════════════════════════════
	// Color Palette — Amber / Yellow CRT
	// ═══════════════════════════════════════════════════════════
	const C = {
		bg:         '#050500',
		primary:    '#ffcc00',      // warm amber-yellow
		primaryDim: '#4a3d00',
		chain1:     '#00e5a0',      // green
		chain2:     '#00ccff',      // cyan
		link:       '#aa8800',
		linkFocus:  '#ffcc00',
		focus:      '#ffcc00',
		focusTarget:'#ccaa00',
		danger:     '#ff3333',
		border:     '#4a3d00',
		borderDim:  '#2a2200',
		text:       '#ffcc00',
		textDim:    '#aa8800',
		textMuted:  '#554400',
	};

	const RING_DUR_FAST = 500;
	const RING_DUR_SLOW = 1200;
	const PING_INTERVAL = 1800;
	const TRAVEL_DUR = 120;
	const NODE_HIT_R = 20;

	function handleDeviceChange(e) { selectDevice(e.target.value); }

	let WIDTH = 900;
	let HEIGHT = 600;

	let displayNodes = [];
	let displayLinks = [];
	let simulation = null;

	let dragNode = null;
	let didDrag = false;
	let svgEl = null;
	let containerEl = null;
	let sidebarEl = null;
	let sidebarW = 220;

	let contextMenu = null;
	let connectFromNode = null;
	let focusedNodeId = null;

	const positionMap = new Map();
	let simNodes = [];

	// ═══════════════════════════════════════════════════════════
	// Pitch helpers
	// ═══════════════════════════════════════════════════════════
	const NOTE_NAMES = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];

	function quantizeMidi(midiNote, scaleIdx) {
		const intervals = SCALE_INTERVALS[scaleIdx];
		if (!intervals || intervals.length >= 12) return midiNote;
		const octave = Math.floor(midiNote / 12);
		const degree = ((midiNote % 12) + 12) % 12;
		let best = intervals[0], bestDist = 99;
		for (const n of intervals) {
			const d = Math.abs(degree - n);
			const dist = Math.min(d, 12 - d);
			if (dist < bestDist) { bestDist = dist; best = n; }
		}
		return octave * 12 + best;
	}

	function pitchToMidi(pitch) { return Math.round(36 + (pitch * 60) / 4096); }

	function midiToName(midi) {
		return NOTE_NAMES[((midi % 12) + 12) % 12] + (Math.floor(midi / 12) - 1);
	}

	function rawPitchToNote(pitch) { return midiToName(pitchToMidi(pitch)); }

	function computeLabels(nodes, si) {
		return new Map(nodes.map(n => {
			const midi = quantizeMidi(pitchToMidi(n.pitch), si);
			return [n.id, midiToName(midi)];
		}));
	}

	function nodeRadius(pitch) { return 3 + (pitch / 4095) * 5; }

	// ═══════════════════════════════════════════════════════════
	// Force simulation
	// ═══════════════════════════════════════════════════════════
	function updateSimulation(nodes, links) {
		if (simulation) simulation.stop();
		simNodes = nodes.map((n) => {
			const existing = positionMap.get(n.id);
			return {
				id: n.id, pitch: n.pitch,
				x: existing?.x ?? WIDTH / 2 + (Math.random() - 0.5) * 100,
				y: existing?.y ?? HEIGHT / 2 + (Math.random() - 0.5) * 100
			};
		});
		const simLinks = links.map((l) => ({
			source: simNodes.find((n) => n.id === l.source),
			target: simNodes.find((n) => n.id === l.target),
			weight: l.weight, sourceId: l.source, targetId: l.target
		})).filter((l) => l.source && l.target);

		simulation = forceSimulation(simNodes)
			.force('link', forceLink(simLinks).id((d) => d.id).distance(120).strength(0.2))
			.force('charge', forceManyBody().strength(-180))
			.force('center', forceCenter(WIDTH / 2, HEIGHT / 2))
			.force('collide', forceCollide(24))
			.alphaDecay(0.05)
			.on('tick', () => {
				displayNodes = simNodes.map((n) => ({ id: n.id, pitch: n.pitch, x: n.x, y: n.y }));
				displayLinks = simLinks.map((l) => ({
					sourceId: l.sourceId, targetId: l.targetId,
					x1: l.source.x, y1: l.source.y,
					x2: l.target.x, y2: l.target.y,
					weight: l.weight
				}));
				displayNodes.forEach((n) => positionMap.set(n.id, { x: n.x, y: n.y }));
				if (simulation.alpha() < 0.01) simulation.stop();
			});
	}

	function selfLoopPath(x, y) {
		return `M ${x - 3} ${y - 10} C ${x - 22} ${y - 38} ${x + 22} ${y - 38} ${x + 3} ${y - 10}`;
	}

	// ═══════════════════════════════════════════════════════════
	// Scale / keyboard
	// ═══════════════════════════════════════════════════════════
	const switchLabels = ['AUTO', 'MID', 'DOWN'];
	const SCALE_NAMES = ['Chromatic','Major','Minor','Pentatonic','Min Penta','Blues','Dorian','Mixolydian'];
	const SCALE_INTERVALS = [
		[0,1,2,3,4,5,6,7,8,9,10,11],
		[0,2,4,5,7,9,11],
		[0,2,3,5,7,8,10],
		[0,2,4,7,9],
		[0,3,5,7,10],
		[0,3,5,6,7,10],
		[0,2,3,5,7,9,10],
		[0,2,4,5,7,9,10],
	];
	$: activeScaleIntervals = SCALE_INTERVALS[$scaleIndex] || SCALE_INTERVALS[0];
	$: scaleNotes = new Set(activeScaleIntervals);

	const KEYBOARD_KEYS = [];
	for (let midi = 48; midi <= 71; midi++) {
		const name = NOTE_NAMES[midi % 12];
		const octave = Math.floor(midi / 12) - 1;
		const isBlack = name.includes('#');
		// Black keys after D# and A# need extra gap (no black between E-F and B-C)
		const gapAfter = isBlack && (name === 'D#' || name === 'A#');
		KEYBOARD_KEYS.push({ midi, name: name + octave, isBlack, gapAfter });
	}
	function handleScaleChange(e) { sendSetScale(parseInt(e.target.value)); }
	function handleAutoIntervalChange(e) { sendSetAutoInterval(parseInt(e.target.value)); }
	$: autoIntervalSec = (0.25 + ($autoInterval / 127) * (8 - 0.25)).toFixed(1);
	$: speedBpm = Math.round(1440000 / (2400 + ((4095 - $knobX) * 93600) / 4095));
	$: rangeOct = ((1 + ($knobY * 35) / 4095) * 2 / 12).toFixed(1);
	function handleKeyClick(midiNote) { sendNoteOn(midiNote, 100); }

	let scaleSelectEl;
	$: if (scaleSelectEl) scaleSelectEl.selectedIndex = $scaleIndex;

	// ═══════════════════════════════════════════════════════════
	// Ring animation system
	// ═══════════════════════════════════════════════════════════
	let rings = [];
	let traveler1 = null;
	let traveler2 = null;
	let _prevNode1 = -1;
	let _prevNode2 = -1;
	let lastPing1Time = 0;
	let lastPing2Time = 0;
	let animFrameId = null;
	let animNow = 0;

	function easeOutCubic(t) { return 1 - Math.pow(1 - t, 3); }

	function addRing(nodeId, color, duration = RING_DUR_SLOW) {
		rings = [...rings, { nodeId, color, start: performance.now(), duration }];
	}

	function onChainChange1(newId) {
		if (newId === _prevNode1) return;
		const oldId = _prevNode1;
		_prevNode1 = newId;
		if (newId < 0) { traveler1 = null; return; }
		const toPos = positionMap.get(newId);
		if (!toPos) return;
		addRing(newId, C.chain1, RING_DUR_FAST);
		lastPing1Time = performance.now();
		const fromPos = oldId >= 0 ? positionMap.get(oldId) : null;
		if (fromPos) {
			traveler1 = {
				sx: fromPos.x, sy: fromPos.y, ex: toPos.x, ey: toPos.y,
				x: fromPos.x, y: fromPos.y, start: performance.now(),
				color: C.chain1
			};
		}
		ensureAnimLoop();
	}

	function onChainChange2(newId) {
		if (newId === _prevNode2) return;
		const oldId = _prevNode2;
		_prevNode2 = newId;
		if (newId < 0) { traveler2 = null; return; }
		const toPos = positionMap.get(newId);
		if (!toPos) return;
		addRing(newId, C.chain2, RING_DUR_FAST);
		lastPing2Time = performance.now();
		const fromPos = oldId >= 0 ? positionMap.get(oldId) : null;
		if (fromPos) {
			traveler2 = {
				sx: fromPos.x, sy: fromPos.y, ex: toPos.x, ey: toPos.y,
				x: fromPos.x, y: fromPos.y, start: performance.now(),
				color: C.chain2
			};
		}
		ensureAnimLoop();
	}

	$: onChainChange1($currentNodeId);
	$: onChainChange2($currentNode2Id);

	function ensureAnimLoop() {
		if (animFrameId) return;
		animFrameId = requestAnimationFrame(animLoop);
	}

	$: if ($currentNodeId >= 0 || $currentNode2Id >= 0) ensureAnimLoop();

	function animLoop() {
		const now = performance.now();
		animNow = now;

		if (traveler1) {
			const t = Math.min(1, (now - traveler1.start) / TRAVEL_DUR);
			const e = easeOutCubic(t);
			traveler1.x = traveler1.sx + (traveler1.ex - traveler1.sx) * e;
			traveler1.y = traveler1.sy + (traveler1.ey - traveler1.sy) * e;
			if (t >= 1) traveler1 = null;
		}
		if (traveler2) {
			const t = Math.min(1, (now - traveler2.start) / TRAVEL_DUR);
			const e = easeOutCubic(t);
			traveler2.x = traveler2.sx + (traveler2.ex - traveler2.sx) * e;
			traveler2.y = traveler2.sy + (traveler2.ey - traveler2.sy) * e;
			if (t >= 1) traveler2 = null;
		}

		if (_prevNode1 >= 0 && now - lastPing1Time > PING_INTERVAL) {
			addRing(_prevNode1, C.chain1, RING_DUR_SLOW);
			lastPing1Time = now;
		}
		if (_prevNode2 >= 0 && now - lastPing2Time > PING_INTERVAL) {
			addRing(_prevNode2, C.chain2, RING_DUR_SLOW);
			lastPing2Time = now;
		}

		rings = rings.filter(r => (now - r.start) < r.duration);

		if (traveler1) traveler1 = traveler1;
		if (traveler2) traveler2 = traveler2;

		const hasPlaying = _prevNode1 >= 0 || _prevNode2 >= 0;
		if (traveler1 || traveler2 || rings.length > 0 || hasPlaying) {
			animFrameId = requestAnimationFrame(animLoop);
		} else {
			animFrameId = null;
		}
	}

	// ═══════════════════════════════════════════════════════════
	// Resize
	// ═══════════════════════════════════════════════════════════
	function handleResize() {
		if (!containerEl) return;
		if (sidebarEl) sidebarW = sidebarEl.offsetWidth;
		WIDTH = containerEl.clientWidth;
		HEIGHT = containerEl.clientHeight;
		if (simulation) {
			simulation.force('center', forceCenter(WIDTH / 2, HEIGHT / 2));
			simulation.alpha(0.3).restart();
		}
	}

	// ═══════════════════════════════════════════════════════════
	// Drag handlers
	// ═══════════════════════════════════════════════════════════
	function getSVGPoint(e) {
		if (!svgEl) return { x: 0, y: 0 };
		const pt = svgEl.createSVGPoint();
		pt.x = e.clientX; pt.y = e.clientY;
		return pt.matrixTransform(svgEl.getScreenCTM().inverse());
	}

	function onNodeMouseDown(e, nodeId) {
		if (e.button !== 0 || connectFromNode !== null) return;
		e.preventDefault(); e.stopPropagation();
		const sn = simNodes.find((n) => n.id === nodeId);
		if (!sn) return;
		dragNode = sn; didDrag = false; focusedNodeId = nodeId;
		sn.fx = sn.x; sn.fy = sn.y;
		if (simulation) simulation.alphaTarget(0.3).restart();
		window.addEventListener('mousemove', onDragMove);
		window.addEventListener('mouseup', onDragEnd);
	}
	function onDragMove(e) {
		if (!dragNode) return;
		didDrag = true;
		const pt = getSVGPoint(e);
		dragNode.fx = pt.x; dragNode.fy = pt.y;
	}
	function onDragEnd() {
		if (!dragNode) return;
		dragNode.fx = null; dragNode.fy = null; dragNode = null;
		if (simulation) simulation.alphaTarget(0);
		window.removeEventListener('mousemove', onDragMove);
		window.removeEventListener('mouseup', onDragEnd);
	}

	// ═══════════════════════════════════════════════════════════
	// Click / context menu
	// ═══════════════════════════════════════════════════════════
	function onNodeClick(e, node) {
		if (didDrag) { didDrag = false; return; }
		if (connectFromNode !== null) {
			if (connectFromNode !== node.id) sendAddLink(connectFromNode, node.id);
			connectFromNode = null; contextMenu = null; return;
		}
		e.preventDefault(); e.stopPropagation();
		focusedNodeId = node.id;
		contextMenu = { x: e.clientX, y: e.clientY, type: 'node',
			data: { id: node.id, pitch: node.pitch } };
	}
	function onLinkClick(e, sourceId, targetId) {
		if (connectFromNode !== null) { connectFromNode = null; return; }
		e.preventDefault(); e.stopPropagation();
		contextMenu = { x: e.clientX, y: e.clientY, type: 'link',
			data: { sourceId, targetId } };
	}
	function dismissMenu() { contextMenu = null; if (connectFromNode !== null) connectFromNode = null; focusedNodeId = null; }
	function handleDeleteNode(id) { sendDeleteNode(id); contextMenu = null; }
	function handleConnectNode(id) { connectFromNode = id; contextMenu = null; }
	function handleDeleteLink(s, t) { sendDeleteLink(s, t); contextMenu = null; }

	function toggleDetails(e) {
		const tag = e.target.tagName;
		if (tag === 'A' || tag === 'BUTTON' || tag === 'SELECT' || tag === 'INPUT') return;
		if (tag === 'SUMMARY') return; // let native toggle handle it
		e.preventDefault();
		const details = e.currentTarget;
		details.open = !details.open;
	}

	// ═══════════════════════════════════════════════════════════
	// Reactive derivations
	// ═══════════════════════════════════════════════════════════
	$: focusedOutputTargets = focusedNodeId !== null
		? new Set($graphLinks.filter(l => l.source === focusedNodeId).map(l => l.target)) : new Set();
	$: nodesWithInput = new Set($graphLinks.map(l => l.target));
	$: nodeLabels = computeLabels(displayNodes, $scaleIndex);
	$: if ($graphNodes || $graphLinks) updateSimulation($graphNodes, $graphLinks);

	// Sort nodes by ID for the metadata list
	$: sortedNodes = [...displayNodes].sort((a, b) => a.id - b.id);

	let sidebarObserver = null;
	onMount(() => {
		positionMap.clear(); handleResize();
		if (sidebarEl) {
			sidebarObserver = new ResizeObserver(() => {
				sidebarW = sidebarEl.offsetWidth;
			});
			sidebarObserver.observe(sidebarEl);
		}
	});
	onDestroy(() => {
		if (simulation) simulation.stop();
		if (animFrameId) cancelAnimationFrame(animFrameId);
		if (sidebarObserver) sidebarObserver.disconnect();
		window.removeEventListener('mousemove', onDragMove);
		window.removeEventListener('mouseup', onDragEnd);
	});
</script>

<svelte:window on:click={dismissMenu} on:resize={handleResize} />

<div class="crt" bind:this={containerEl}>
	<svg bind:this={svgEl} class="monitor" style="left: {sidebarW}px; width: calc(100% - {sidebarW}px);" viewBox="0 0 {WIDTH} {HEIGHT}">
		<defs>
			<pattern id="grid" width="40" height="40" patternUnits="userSpaceOnUse">
				<path d="M 40 0 L 0 0 0 40" fill="none" stroke="#1a1500" stroke-width="0.5" />
			</pattern>
		</defs>

		<!-- Grid background -->
		<rect width={WIDTH} height={HEIGHT} fill="url(#grid)" />

		<!-- Range circles (radar style) -->
		{#each [80, 160, 240] as r}
			<circle cx={WIDTH / 2} cy={HEIGHT / 2} r={r}
				fill="none" stroke="#1a1500" stroke-width="0.5" stroke-dasharray="4 8" />
		{/each}

		<!-- Crosshair -->
		<line x1={WIDTH / 2} y1="0" x2={WIDTH / 2} y2={HEIGHT}
			stroke="#1a1500" stroke-width="0.5" stroke-dasharray="8 8" />
		<line x1="0" y1={HEIGHT / 2} x2={WIDTH} y2={HEIGHT / 2}
			stroke="#1a1500" stroke-width="0.5" stroke-dasharray="8 8" />

		<!-- Corner brackets -->
		<g stroke={C.border} stroke-width="1" fill="none" opacity="0.4">
			<polyline points="30,10 10,10 10,30" />
			<polyline points="{WIDTH - 30},10 {WIDTH - 10},10 {WIDTH - 10},30" />
			<polyline points="10,{HEIGHT - 30} 10,{HEIGHT - 10} 30,{HEIGHT - 10}" />
			<polyline points="{WIDTH - 10},{HEIGHT - 30} {WIDTH - 10},{HEIGHT - 10} {WIDTH - 30},{HEIGHT - 10}" />
		</g>

		<!-- Expanding rings -->
		{#each rings as ring}
			{@const node = displayNodes.find(n => n.id === ring.nodeId)}
			{#if node}
				{@const age = Math.max(0, Math.min(1, (animNow - ring.start) / ring.duration))}
				{@const radius = 6 + age * 40}
				{@const opacity = 0.9 * (1 - age)}
				<circle cx={node.x} cy={node.y} r={radius}
					fill="none" stroke={ring.color} stroke-width="1.5"
					opacity={opacity} pointer-events="none" />
			{/if}
		{/each}

		<!-- Links -->
		{#each displayLinks as link}
			{@const isFocused = focusedNodeId !== null && link.sourceId === focusedNodeId}
			{@const color = isFocused ? C.linkFocus : C.link}
			{@const opacity = isFocused ? 0.8 : 0.5}
			{#if link.sourceId === link.targetId}
				{@const node = displayNodes.find((n) => n.id === link.sourceId)}
				{#if node}
					<path d={selfLoopPath(node.x, node.y)} fill="none" stroke="transparent"
						stroke-width="14" style="cursor: pointer"
						on:click|stopPropagation={(e) => onLinkClick(e, link.sourceId, link.targetId)} />
					<path d={selfLoopPath(node.x, node.y)} fill="none"
						stroke={color} stroke-width="0.8" opacity={opacity}
						stroke-dasharray="4 4" pointer-events="none" />
				{/if}
			{:else}
				<line x1={link.x1} y1={link.y1} x2={link.x2} y2={link.y2}
					stroke="transparent" stroke-width="14" style="cursor: pointer"
					on:click|stopPropagation={(e) => onLinkClick(e, link.sourceId, link.targetId)} />
				<line x1={link.x1} y1={link.y1} x2={link.x2} y2={link.y2}
					stroke={color} stroke-width="0.8" opacity={opacity}
					stroke-dasharray="4 4" pointer-events="none" />
			{/if}
		{/each}

		<!-- Nodes -->
		{#each displayNodes as node}
			{@const isFocusSource = focusedNodeId === node.id}
			{@const isFocusTarget = focusedOutputTargets.has(node.id)}
			{@const isOrphan = !nodesWithInput.has(node.id) && !isFocusSource && !isFocusTarget}
			{@const isP1 = node.id === $currentNodeId}
			{@const isP2 = node.id === $currentNode2Id}
			{@const isPlaying = isP1 || isP2}
			{@const fillColor = isP1 ? C.chain1 : isP2 ? C.chain2 : isFocusSource ? C.focus : isFocusTarget ? C.focusTarget : C.primary}
			{@const r = nodeRadius(node.pitch)}
			{@const isConnectSource = connectFromNode === node.id}
			<g style="cursor: {connectFromNode !== null ? 'crosshair' : 'grab'}"
				opacity={isOrphan ? 0.4 : 1}
				on:mousedown|stopPropagation={(e) => onNodeMouseDown(e, node.id)}
				on:click|stopPropagation={(e) => onNodeClick(e, node)}>
				<!-- hit area -->
				<circle cx={node.x} cy={node.y} r={NODE_HIT_R} fill="transparent" />
				<!-- node dot -->
				<circle cx={node.x} cy={node.y} r={r}
					fill={isConnectSource ? C.focus : fillColor} />
				<!-- crosshair on focused node -->
				{#if isFocusSource}
					<line x1={node.x - 16} y1={node.y} x2={node.x + 16} y2={node.y}
						stroke={C.focus} stroke-width="0.5" opacity="0.5" />
					<line x1={node.x} y1={node.y - 16} x2={node.x} y2={node.y + 16}
						stroke={C.focus} stroke-width="0.5" opacity="0.5" />
				{/if}
			</g>
			<!-- label -->
			<text x={node.x + 12} y={node.y - 10} class="node-label"
				fill={isP1 ? C.chain1 : isP2 ? C.chain2 : isFocusSource ? C.focus : C.textDim}
				opacity={isOrphan ? 0.3 : 1}
				pointer-events="none">{nodeLabels.get(node.id) || ''}</text>
		{/each}

		<!-- Traveling dots -->
		{#if traveler1}
			<circle cx={traveler1.x} cy={traveler1.y} r="3"
				fill={traveler1.color} pointer-events="none" />
		{/if}
		{#if traveler2}
			<circle cx={traveler2.x} cy={traveler2.y} r="3"
				fill={traveler2.color} pointer-events="none" />
		{/if}

		<!-- Empty state -->
		{#if !$midiConnected}
			<text x={WIDTH / 2} y={HEIGHT / 2} text-anchor="middle"
				fill={C.textDim} font-size="16"
				font-family="'Monaspace Krypton', monospace">
				NO SIGNAL — CONNECT TO MIDI
			</text>
		{:else if displayNodes.length === 0}
			<text x={WIDTH / 2} y={HEIGHT / 2} text-anchor="middle"
				fill={C.textDim} font-size="16"
				font-family="'Monaspace Krypton', monospace">
				NO SIGNAL — FLIP SWITCH UP TO CREATE NODES
			</text>
		{/if}
	</svg>

	<!-- CRT overlay layers -->
	<div class="scanlines"></div>
	<div class="flicker"></div>

	<!-- ═══ Header bar ═══ -->
	<div class="header">
		<span class="title">ZODIAC CARD</span>
		<span class="subtitle"> | BETA v0.2</span>
		<div class="hdr-rule"></div>
		<div class="conn-bar">
			<button class="btn" class:connected={$midiConnected} on:click={connectMIDI}>
				{$midiConnected ? '● CONNECTED' : 'CONNECT MIDI'}
			</button>
			{#if $deviceNames.length > 0}
				<select class="sel" value={$selectedDevice} on:change={handleDeviceChange}>
					<option value="" disabled>SELECT DEVICE…</option>
					{#each $deviceNames as name}<option value={name}>{name}</option>{/each}
				</select>
			{/if}
			{#if $midiConnected}
				<button class="btn" on:click={requestPull}>↻ PULL</button>
			{/if}
		</div>
	</div>

	<!-- ═══ Left metadata panel ═══ -->
	<div class="sidebar" bind:this={sidebarEl} on:click|stopPropagation>
		<details class="side-section" open on:click={toggleDetails}>
			<summary class="side-title">ABOUT</summary>
			<div class="help-desc">
				Zodiac Card is a Markov chain sequencer for the <a class="disc-link" href="https://www.musicthing.co.uk/workshopsystem/" target="_blank">MTM Computer Module</a>. 
				<br/>- Nodes hold pitches, move the switch up to create new nodes.
				<br/>- Links represent transition probabilities between nodes and are created automatically based on the probability set on the main knob.
				<br/>- Two independent sequencers traverse the network to generate melodies. 
				<br/>- Sequencing speed and pitch range can be modulated with the knobs or CV and pulse inputs.
				<br/>
				<br/>To install, download the .UF2 file below, save it to the card and connect via USB MIDI (Chrome only).
			</div>
			<a class="btn btn-dl" href="/zodiac-card-v0.2.uf2" download>⬇ DOWNLOAD .UF2</a>
		</details>

		<details class="side-section" on:click={toggleDetails}>
			<summary class="side-title">CONTROLS</summary>
			<div class="help-items">
				<div class="help-row"><span class="hl">MAIN KNOB</span><span class="hv">Link creation probability</span></div>
				<div class="help-row"><span class="hl">X KNOB</span><span class="hv">Clock / speed</span></div>
				<div class="help-row"><span class="hl">Y KNOB</span><span class="hv">Pitch range</span></div>
				<div class="help-row"><span class="hl">SWITCH ↑</span><span class="hv">Node creation mode</span></div>
				<div class="help-row"><span class="hl">SWITCH —</span><span class="hv">Normal play</span></div>
				<div class="help-row"><span class="hl">SWITCH ↓</span><span class="hv">Change scale</span></div>
			</div>
		</details>

		<details class="side-section" on:click={toggleDetails}>
			<summary class="side-title">INPUTS</summary>
			<div class="help-items">
				<div class="help-row"><span class="hl">AUDIO IN 1</span><span class="hv">-</span></div>
				<div class="help-row"><span class="hl">AUDIO IN 2</span><span class="hv">-</span></div>
				<div class="help-row"><span class="hl">CV IN 1</span><span class="hv">Probability CV</span></div>
				<div class="help-row"><span class="hl">CV IN 2</span><span class="hv">Pitch range CV</span></div>
				<div class="help-row"><span class="hl">PULSE IN 1</span><span class="hv">Seq 1 clock</span></div>
				<div class="help-row"><span class="hl">PULSE IN 2</span><span class="hv">Seq 2 clock</span></div>
			</div>
		</details>

		<details class="side-section" on:click={toggleDetails}>
			<summary class="side-title">OUTPUTS</summary>
			<div class="help-items">
				<div class="help-row"><span class="hl">AUDIO OUT 1</span><span class="hv">Seq 1 pitched sine wave</span></div>
				<div class="help-row"><span class="hl">AUDIO OUT 2</span><span class="hv">Seq 2 pitched sine wave</span></div>
				<div class="help-row"><span class="hl">CV OUT 1</span><span class="hv">Seq 1 pitch CV</span></div>
				<div class="help-row"><span class="hl">CV OUT 2</span><span class="hv">Seq 2 pitch CV</span></div>
				<div class="help-row"><span class="hl">PULSE OUT 1</span><span class="hv">Seq 1 gate</span></div>
				<div class="help-row"><span class="hl">PULSE OUT 2</span><span class="hv">Seq 2 gate</span></div>
			</div>
		</details>

		<details class="side-section" open on:click={toggleDetails}>
			<summary class="side-title">STATUS</summary>
			<div class="meta-row"><span class="ml">N_NODES</span><span class="mv">{String($nodeCount).padStart(2, '0')}</span></div>
			<div class="meta-row"><span class="ml">SEQ1_ID</span><span class="mv c1">{$currentNodeId >= 0 ? String($currentNodeId).padStart(3, '0') : '---'}</span></div>
			<div class="meta-row"><span class="ml">SEQ2_ID</span><span class="mv c2">{$currentNode2Id >= 0 ? String($currentNode2Id).padStart(3, '0') : '---'}</span></div>
			<div class="meta-row"><span class="ml">LINK_PROB</span><span class="mv">{Math.round(($knobMain / 4095) * 100)}%</span></div>
			<div class="meta-row"><span class="ml">MAIN_SPEED</span><span class="mv">{speedBpm} BPM</span></div>
			<div class="meta-row"><span class="ml">PITCH_RANGE</span><span class="mv">{rangeOct} OCT</span></div>
			<div class="meta-row"><span class="ml">SWITCH_POS</span><span class="mv">{switchLabels[$switchState] || '?'}</span></div>
		</details>

		<details class="side-section" open on:click={toggleDetails}>
			<summary class="side-title">NODE LIST</summary>
			<div class="node-list">
				{#each sortedNodes as node}
					{@const isP1 = node.id === $currentNodeId}
					{@const isP2 = node.id === $currentNode2Id}
					{@const label = nodeLabels.get(node.id) || '?'}
					<div class="node-entry"
						class:active-c1={isP1}
						class:active-c2={isP2 && !isP1}>
						<span class="ne-id">{String(node.id).padStart(3, '0')}</span>
						<span class="ne-note">{label}</span>
						{#if isP1}<span class="ne-tag c1">SEQ1</span>{/if}
						{#if isP2}<span class="ne-tag c2">SEQ2</span>{/if}
					</div>
				{:else}
					<div class="node-entry empty">NO NODES</div>
				{/each}
			</div>
		</details>
		
		<div class="disclaimer side-section">
			Built by <a class="disc-link" href="https://incomputable.io" target="_blank"><strong>incomputable.io</strong></a> <br/>for the <a class="disc-link" href="https://www.musicthing.co.uk/workshopsystem/" target="_blank">Computer Module by Music Thing Modular</a>
			<br/>
			<br/>
			This work is licensed under <br/> <a class="disc-link" href="https://creativecommons.org/licenses/by-nc-sa/4.0/">CC BY-NC-SA 4.0</a>
		</div>
	</div>
	

	<!-- ═══ Connect banner ═══ -->
	{#if connectFromNode !== null}
		<div class="banner" style="left: {sidebarW}px;">
			⚠ SELECT TARGET FOR LINK FROM [{connectFromNode}] →
			<button class="btn btn-cancel" on:click|stopPropagation={() => { connectFromNode = null; }}>CANCEL</button>
		</div>
	{/if}

	<!-- ═══ Bottom controls & keyboard ═══ -->
	<div class="bottom-bar" style="left: {sidebarW}px;">
		<div class="bottom-top-row">
			<div class="bottom-ctrl">
				<span class="ctrl-label">SCALE</span>
				<select class="sel sel-input" bind:this={scaleSelectEl} on:change={handleScaleChange}>
					{#each SCALE_NAMES as name, i}<option value={i}>{name}</option>{/each}
				</select>
			</div>
			<div class="bottom-ctrl">
				<span class="ctrl-label">AUTO INTERVAL</span>
				<div class="slider-row">
					<input type="range" class="slider" min="0" max="127" value={$autoInterval} on:input={handleAutoIntervalChange}>
					<span class="mv">{autoIntervalSec}s</span>
				</div>
			</div>
		</div>
		<div class="kbd-section">
			<span class="kbd-label">INPUT KEYBOARD</span>
			<div class="keyboard">
				<div class="kbd-black-row">
					{#each KEYBOARD_KEYS.filter(k => k.isBlack) as key}
						<button class="note-btn is-black"
							class:in-scale={scaleNotes.has(key.midi % 12)}
							class:gap-after={key.gapAfter}
							on:click={() => handleKeyClick(key.midi)}>
							{key.name}
						</button>
					{/each}
				</div>
				<div class="kbd-white-row">
					{#each KEYBOARD_KEYS.filter(k => !k.isBlack) as key}
						<button class="note-btn"
							class:in-scale={scaleNotes.has(key.midi % 12)}
							on:click={() => handleKeyClick(key.midi)}>
							{key.name}
						</button>
					{/each}
				</div>
			</div>
		</div>
		
	</div>

	<!-- ═══ Context menu ═══ -->
	{#if contextMenu}
		<div class="ctx-menu" style="left: {contextMenu.x}px; top: {contextMenu.y}px"
			on:click|stopPropagation>
			{#if contextMenu.type === 'node'}
				<div class="ctx-header">[NODE {contextMenu.data.id}] {rawPitchToNote(contextMenu.data.pitch)}</div>
				<div class="ctx-item" on:click={() => handleConnectNode(contextMenu.data.id)}>CONNECT TO…</div>
				<div class="ctx-sep"></div>
				<div class="ctx-item ctx-danger" on:click={() => handleDeleteNode(contextMenu.data.id)}>DELETE NODE</div>
			{:else if contextMenu.type === 'link'}
				<div class="ctx-item ctx-danger"
					on:click={() => handleDeleteLink(contextMenu.data.sourceId, contextMenu.data.targetId)}>
					DELETE LINK [{contextMenu.data.sourceId}] → [{contextMenu.data.targetId}]
				</div>
			{/if}
		</div>
	{/if}
</div>

<style>
	:global(*) { box-sizing: border-box; margin: 0; padding: 0; }
	:global(body) {
		font-family: 'Monaspace Krypton', monospace;
		background: #000; color: #ffcc00; overflow: hidden;
	}

	/* ═══ CRT Container ═══ */
	.crt {
		position: fixed; inset: 0; background: #050500;
	}

	/* Vignette — subtle */
	.crt::before {
		content: '';
		position: absolute; inset: 0;
		background: radial-gradient(ellipse at center, transparent 60%, rgba(0,0,0,0.15) 90%, rgba(0,0,0,0.3) 100%);
		pointer-events: none; z-index: 50;
	}

	/* Scan lines — subtle */
	.scanlines {
		position: absolute; inset: 0;
		background: repeating-linear-gradient(
			0deg,
			transparent,
			transparent 2px,
			rgba(0, 0, 0, 0.06) 2px,
			rgba(0, 0, 0, 0.06) 3px
		);
		pointer-events: none; z-index: 51;
	}

	/* Subtle flicker */
	.flicker {
		position: absolute; inset: 0;
		pointer-events: none; z-index: 52;
		animation: crt-flicker 0.08s infinite alternate;
		opacity: 0;
	}
	@keyframes crt-flicker {
		0% { opacity: 0; }
		50% { opacity: 0.015; background: rgba(255, 204, 0, 0.02); }
		100% { opacity: 0; }
	}

	.monitor {
		position: absolute;
		top: 0; bottom: 0; right: 0;
		height: 100%;
		display: block;
	}

	/* ═══ Header ═══ */
	.header {
		position: absolute; top: 0; left: 0; right: 0;
		background: rgba(5, 5, 0, 0.92);
		border-bottom: 1px solid #4a3d00;
		padding: 8px 16px;
		display: flex; align-items: center; gap: 16px;
		z-index: 20;
	}
	.hdr-rule {
		flex: 1; height: 1px;
		background: linear-gradient(90deg, #4a3d00, transparent);
	}
	.title {
		font-size: 18px; color: #ffcc00;
		letter-spacing: 4px; font-weight: bold;
		text-shadow: 0 0 12px rgba(255, 204, 0, 0.5);
		white-space: nowrap;
	}
	.conn-bar { display: flex; align-items: center; gap: 8px; flex-shrink: 0; }

	/* ═══ Left Sidebar ═══ */
	.sidebar {
		position: absolute;
		top: 40px; bottom: 0; left: 0;
		width: min-content;

		background: rgba(5, 5, 0, 0.92);
		/* border-right: 1px solid #4a3d00; */
		padding: 12px;
		padding-bottom: 130px;
		z-index: 20;
		overflow: visible;
		display: flex;
		flex-direction: column;
		flex-wrap: wrap;
		align-content: flex-start;
		gap: 0 12px;
	}
	.side-section {
		width: 270px;
		border: 1px solid #4a3d0090;
		background-color: #00000090;
		padding: 10px 10px;
	}
	.side-section, details.side-section {
		margin-bottom: 10px;
		/* flex-shrink: 0; */
	}
	.side-rule {
		width: 196px;
		border: none; height: 1px;
		background: #2a2200;
		margin: 4px 0;
		display: none;
	}
	.side-title {
		font-size: 11px; color: #aa8800;
		letter-spacing: 2px; margin-bottom: 6px;
		cursor: default;
	}
	summary.side-title {
		cursor: pointer; list-style: none;
		user-select: none;
	}
	summary.side-title::before {
		content: '▸ '; color: #6a5500;
	}
	details[open] > summary.side-title::before {
		content: '▾ ';
	}
	summary.side-title::-webkit-details-marker { display: none; }

	/* Metadata rows */
	.meta-row {
		display: flex; justify-content: space-between;
		padding: 3px 0;
		font-size: 14px;
	}
	.ml { color: #aa8800; }
	.mv { color: #ffcc00; text-align: right; }
	.c1 { color: #00e5a0; text-shadow: 0 0 6px rgba(0, 229, 160, 0.4); }
	.c2 { color: #00ccff; text-shadow: 0 0 6px rgba(0, 204, 255, 0.4); }
	.mv.c1 { opacity: 0.6;}
	.mv.c2 { opacity: 0.6; }

	.slider-row { display: flex; align-items: center; gap: 6px; }

	/* ═══ Node list ═══ */
	.node-list {
		display: flex; flex-direction: column; gap: 1px;
	}
	.node-entry {
		display: flex; align-items: center; gap: 6px;
		padding: 4px 4px;
		font-size: 14px;
		color: #ffcc00;
		border-left: 2px solid transparent;
	}
	.node-entry.active-c1 {
		border-left-color: #00e5a0;
		background: rgba(0, 229, 160, 0.06);
	}
	.node-entry.active-c2 {
		border-left-color: #00ccff;
		background: rgba(0, 204, 255, 0.06);
	}
	.node-entry.empty { color: #554400; font-size: 13px; }
	.ne-id { color: #aa8800; font-size: 12px; min-width: 30px; }
	.ne-note { color: #ffcc00; flex: 1; }
	.ne-tag {
		font-size: 9px; letter-spacing: 1px;
		padding: 0 3px;
		border: 1px solid;
		opacity: 0.5;
	}
	.ne-tag.c1 { color: #00e5a0; border-color: #00e5a0; }
	.ne-tag.c2 { color: #00ccff; border-color: #00ccff; }

	/* ═══ Connect banner ═══ */
	.banner {
		position: absolute; top: 40px; right: 0;
		background: rgba(30, 15, 0, 0.92);
		border-bottom: 1px solid #6a4400;
		padding: 8px 16px;
		color: #ffcc00; font-size: 14px;
		display: flex; align-items: center; gap: 12px;
		z-index: 20;
	}

	/* ═══ Bottom bar ═══ */
	.bottom-bar {
		position: absolute; bottom: 0; right: 0;
		display: flex; flex-direction: row; gap: 12px;
		flex-wrap: wrap; 
		align-items: center;
		justify-content: space-around;
		background: rgba(5, 5, 0, 0.92);
		/* border-top: 1px solid #4a3d00; */
		padding: 8px 16px;
		z-index: 20;
	}
	.bottom-top-row {
		display: flex; gap: 20px; align-items: flex-end;
		margin-bottom: 8px;
	}
	.bottom-ctrl { display: flex; align-items: center; gap: 8px; }
	.ctrl-label {
		font-size: 11px; color: #aa8800;
		letter-spacing: 2px; white-space: nowrap;
	}
	.kbd-section {
		display: flex; flex-direction: column; gap: 4px;
	}
	.kbd-label {
		font-size: 11px; color: #aa8800;
		letter-spacing: 2px; white-space: nowrap;
	}
	.keyboard {
		display: flex; flex-direction: column; gap: 2px;
	}
	.kbd-black-row {
		display: flex; flex-wrap: nowrap; gap: 3px;
		padding-left: 22px;
	}
	.kbd-white-row {
		display: flex; flex-wrap: nowrap; gap: 3px;
	}
	.note-btn.is-black {
		min-width: 38px;
		padding: 4px 4px;.
		font-size: 11px;
	}
	.note-btn.is-black.gap-after {
		margin-right: 44px;
	}
	.disclaimer {
		position: fixed; bottom: 10px; left: 12px;
		/* margin-top: 6px; */
		font-size: 12px; color: #aa8800;
		letter-spacing: 0.5px;
		text-align: left;
		/* width: 270px; */
		/* background-color: rgba(0, 0, 0, 0.06); */
		/* border: 1px solid #4a3d00; */
		padding: 8px;

	}
	.disclaimer strong { color: #6a5500; font-weight: normal; }
	.disc-link { color: #6a5500; text-decoration: underline; }
	.disc-link:hover { color: #aa8800; }

	/* ═══ Note buttons ═══ */
	.note-btn {
		background: transparent;
		border: 1px solid #332b00;
		color: #332b00;
		font-family: 'Monaspace Krypton', monospace;
		font-size: 13px; letter-spacing: 0.5px;
		padding: 6px 8px; cursor: pointer;
		transition: all 0.1s;
		min-width: 44px; text-align: center;
	}
	.note-btn.in-scale {
		border-color: #5a3000;
		color: #ff9955;
		text-shadow: 0 0 6px rgba(255, 153, 85, 0.3);
	}
	.note-btn.in-scale:hover {
		background: rgba(255, 153, 85, 0.12);
		border-color: #ff9955;
	}
	.note-btn.in-scale:active {
		background: #ff9955;
		color: #000;
	}
	.note-btn.is-black.in-scale {
		color: #ff7744;
		border-color: #4a2200;
	}
	.note-btn.is-black.in-scale:hover {
		background: rgba(255, 119, 68, 0.12);
		border-color: #ff7744;
	}
	.note-btn.is-black.in-scale:active {
		background: #ff7744;
		color: #000;
	}

	/* ═══ Buttons & controls ═══ */
	.btn {
		background: transparent; color: #ffcc00;
		border: 1px solid #4a3d00;
		padding: 4px 12px;
		font-family: inherit; font-size: 12px; letter-spacing: 1px;
		cursor: pointer; transition: all 0.15s;
	}
	.btn:hover { background: rgba(255, 204, 0, 0.1); border-color: #ffcc00; }
	.btn.connected {
		color: #00e5a0; border-color: #00e5a0;
		text-shadow: 0 0 6px rgba(0, 229, 160, 0.5);
	}
	.btn-cancel { color: #ff6644; border-color: #4a2200; }
	.btn-cancel:hover { background: rgba(255, 100, 68, 0.1); border-color: #ff6644; }

	.sel {
		background: rgba(5, 5, 0, 0.9); color: #ffcc00;
		border: 1px solid #4a3d00;
		padding: 4px 8px; font-family: inherit; font-size: 12px;
		cursor: pointer;
	}
	.sel:focus { border-color: #ffcc00; outline: none; }
	.sel-full { width: 100%; }
	.sel-input {
		color: #ff9955; border-color: #5a3000;
	}
	.sel-input:focus { border-color: #ff9955; }

	.slider {
		flex: 1; height: 4px;
		-webkit-appearance: none; appearance: none;
		background: #3a2200; outline: none; cursor: pointer;
		vertical-align: middle;
	}
	.slider::-webkit-slider-thumb {
		-webkit-appearance: none; width: 10px; height: 14px;
		background: #ff9955; cursor: pointer; border: none;
	}

	/* ═══ SVG Node labels ═══ */
	.node-label {
		font-family: 'Monaspace Krypton', monospace;
		font-size: 13px; letter-spacing: 0.5px;
		user-select: none;
	}

	/* ═══ Context menu ═══ */
	.ctx-menu {
		position: fixed;
		background: rgba(5, 5, 0, 0.96);
		border: 1px solid #4a3d00;
		padding: 2px 0;
		z-index: 1000; min-width: 200px;
		box-shadow: 0 0 20px rgba(255, 204, 0, 0.08);
	}
	.ctx-header {
		padding: 8px 14px; font-size: 14px;
		color: #ffcc00; border-bottom: 1px solid #2a2200;
		font-family: 'Monaspace Krypton', monospace;
		letter-spacing: 1px;
	}
	.ctx-item {
		padding: 8px 14px; font-size: 13px;
		font-family: 'Monaspace Krypton', monospace;
		color: #6a5500; cursor: pointer;
		letter-spacing: 0.5px;
	}
	.ctx-item:hover { background: rgba(255, 204, 0, 0.08); color: #ffcc00; }
	.ctx-danger { color: #ff3333; }
	.ctx-danger:hover { background: rgba(255, 0, 0, 0.06); color: #ff4444; }
	.ctx-sep { border-top: 1px solid #2a2200; margin: 2px 0; }

	/* ═══ Help / instructions rows ═══ */
	.help-items {
		padding-left: 10px;
		/* border-left: 1px solid #2a2200; */
	}
	.help-row {
		display: flex; flex-direction: column;
		padding: 2px 0;
	}
	.hl {
		font-size: 11px; color: #aa8800;
		letter-spacing: 1px;
	}
	.hv {
		font-size: 12px; color: #6a5500;
		padding-left: 15px;
	}

	/* Help description */
	.help-desc {
		font-size: 12px; color: #ccaa44;
		line-height: 1.4;
	}

	/* ═══ Download button ═══ */
	.btn-dl {
		display: block; text-align: center;
		color: #ff9955; border-color: #5a3000;
		padding: 6px 12px; font-size: 12px;
		margin-top: 12px;
		margin-bottom: 4px;
		text-decoration: none;
	}
	.btn-dl:hover {
		background: rgba(255, 153, 85, 0.1);
		border-color: #ff9955;
	}
</style>
