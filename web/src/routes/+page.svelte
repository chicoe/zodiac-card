<script>
	import '@fontsource/monaspace-krypton';
	import { onMount, onDestroy } from 'svelte';
	import { base } from '$app/paths';
	// 3D force graph (3d-force-graph + three) is imported dynamically in
	// onMount — client-only, and keeps the initial bundle chunk lean.
	import { connectMIDI, selectDevice, requestPull, sendDeleteNode, sendDeleteLink, sendAddLink, sendNoteOn, sendSetScale, sendSetAutoInterval, sendSetWaveform1, sendSetBassMode1, sendSetWaveform2, sendSetBassMode2, sendSetMaxNodes, setNoteTxChannel, setStatusRxChannel, isAutoDetectedName } from '$lib/midi.js';
	import {
		graphNodes, graphLinks, currentNodeId, currentNode2Id, nodeCount,
		knobMain, knobX, knobY, switchState,
		midiConnected, deviceNames, selectedDevice, scaleIndex, autoInterval,
		waveformIndex1, bassMode1, waveformIndex2, bassMode2,
		bpm1, bpm2, clockMode, clockMultLevel, isrPeakUs, isrSections, rxMsgCount, sysexRxCount,
		debugMidiInfo, maxNodes, noteTxChannel, statusRxChannel
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

	function handleDeviceChange(e) { selectDevice(e.target.value); }

	let containerEl = null;
	let sidebarEl = null;
	let sidebarW = 220;

	let contextMenu = null;
	let connectFromNode = null;
	let focusedNodeId = null;

	// ═══ 3D graph state ═══
	let graphEl = null;          // container div for the 3D canvas
	let graph3d = null;          // ForceGraph3D instance
	let THREE = null;            // three module (dynamic import)
	let SpriteText = null;       // three-spritetext ctor (dynamic import)
	let starTexture = null;
	const nodePool = new Map();  // node id → persistent sim-node object (keeps 3D positions across syncs)
	let pulseFrameId = null;
	let graphClickTs = 0;        // suppresses the window click-dismiss right after a canvas node/link click

	// Camera auto-orbit: constant slow azimuth spin + slower polar sway
	// (different rates per axis). Pauses while the user drags/zooms.
	const ORBIT_AZ_SPEED  = 0.04;  // rad/s around the vertical axis
	const ORBIT_POL_RATE  = 0.09;  // Hz-ish of the vertical sway oscillation
	const ORBIT_POL_AMP   = 0.06;  // rad/s peak polar drift
	let orbitPaused = false;
	let orbitResumeTimer = null;
	let lastAnimT = 0;
	let _sph = null;               // reusable THREE.Spherical

	// Star fade: untriggered nodes slowly dim to a floor; a playhead visit
	// (or node creation) lights them back to full and restarts the fade.
	const FADE_SECONDS = 30;
	const MIN_GLOW = 0.06;

	// Auto-fit: camera radius eases toward framing the whole constellation
	const FIT_PADDING = 1.12;   // fraction of margin around the bounding sphere
	const FIT_LERP    = 1.5;    // approach rate (per second)

	// Sidebar list is driven straight from the store now (no 2D sim tick)
	$: displayNodes = $graphNodes;

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

	// ═══════════════════════════════════════════════════════════
	// 3D force graph — star-like glow sprites, dashed links
	// ═══════════════════════════════════════════════════════════

	// Soft radial glow — tinted per-node via the sprite material color
	function makeStarTexture() {
		const c = document.createElement('canvas');
		c.width = c.height = 64;
		const ctx = c.getContext('2d');
		const g = ctx.createRadialGradient(32, 32, 0, 32, 32, 32);
		g.addColorStop(0, 'rgba(255,255,255,1)');
		g.addColorStop(0.12, 'rgba(255,255,255,0.9)');
		g.addColorStop(0.3, 'rgba(255,255,255,0.35)');
		g.addColorStop(1, 'rgba(255,255,255,0)');
		ctx.fillStyle = g;
		ctx.fillRect(0, 0, 64, 64);
		return new THREE.CanvasTexture(c);
	}

	function nodeBaseScale(pitch) { return 5 + (pitch / 4095) * 5; }

	function labelFor(node) { return nodeLabels.get(node.id) || ''; }

	// ═══ Star shader — gaussian core, soft halo, diffraction spikes,
	// per-star shimmer. Billboarded in the vertex shader (the quad is
	// expanded in view space, so it always faces the camera).
	function makeStarMaterial() {
		return new THREE.ShaderMaterial({
			transparent: true,
			depthWrite: false,
			blending: THREE.AdditiveBlending,
			uniforms: {
				uColor:     { value: new THREE.Color(C.primary) },
				uIntensity: { value: 1 },
				uScale:     { value: 6 },
				uTime:      { value: 0 },
				uSeed:      { value: Math.random() * 6.2832 }
			},
			vertexShader: `
				uniform float uScale;
				varying vec2 vP;
				void main() {
					vP = position.xy;
					vec4 mv = modelViewMatrix * vec4(0.0, 0.0, 0.0, 1.0);
					mv.xy += position.xy * uScale;
					gl_Position = projectionMatrix * mv;
				}
			`,
			fragmentShader: `
				uniform vec3  uColor;
				uniform float uIntensity;
				uniform float uTime;
				uniform float uSeed;
				varying vec2 vP;
				void main() {
					float r = length(vP);
					// white-hot core
					float core = exp(-r * r * 22.0);
					// wide soft halo
					float halo = exp(-r * 4.0) * 0.35;
					// 4-point diffraction spikes, slowly breathing
					float ax = abs(vP.x), ay = abs(vP.y);
					float spikeLen = 4.5 - 1.2 * sin(uTime * 0.9 + uSeed);
					float spikes = (exp(-ay * 34.0) * exp(-ax * spikeLen)
					              + exp(-ax * 34.0) * exp(-ay * spikeLen)) * 0.55;
					// gentle twinkle, dephased per star
					float tw = 0.88 + 0.12 * sin(uTime * 2.7 + uSeed * 3.1);
					float a = (core + halo + spikes) * uIntensity * tw;
					vec3 col = mix(uColor, vec3(1.0), core * 0.65);
					gl_FragColor = vec4(col * a, a);
				}
			`
		});
	}

	let _planeGeo = null, _hitGeo = null, _hitMat = null;

	function makeStarObject(node) {
		if (!_planeGeo) {
			_planeGeo = new THREE.PlaneGeometry(2, 2);
			_hitGeo = new THREE.SphereGeometry(4, 6, 6);
			// invisible but raycastable — clicks land on this, not the glow quad
			_hitMat = new THREE.MeshBasicMaterial({ transparent: true, opacity: 0, depthWrite: false });
		}
		const mat = makeStarMaterial();
		mat.uniforms.uScale.value = nodeBaseScale(node.pitch ?? 2048);
		const star = new THREE.Mesh(_planeGeo, mat);
		star.frustumCulled = false; // billboarded in-shader; default culling would clip at screen edges

		const hit = new THREE.Mesh(_hitGeo, _hitMat);

		const label = new SpriteText(labelFor(node), 1.8, C.textDim);
		label.fontFace = 'monospace';
		label.material.transparent = true;
		label.material.depthWrite = false;
		label.position.set(0, 5, 0);

		const group = new THREE.Group();
		group.add(hit);
		group.add(star);
		group.add(label);
		node.__mat = mat;
		node.__label = label;
		return group;
	}

	// Push store data into the 3D graph, reusing node objects so positions
	// (and the running simulation) survive incremental syncs.
	function syncGraph(nodes, links) {
		if (!graph3d) return;
		const liveIds = new Set(nodes.map((n) => n.id));
		for (const id of [...nodePool.keys()]) {
			if (!liveIds.has(id)) nodePool.delete(id);
		}
		const gNodes = nodes.map((n) => {
			let o = nodePool.get(n.id);
			if (!o) {
				// Spawn newcomers just outside the constellation and let the
				// link forces reel them in — far less disruptive than the
				// default spawn near the core.
				let maxR = 80;
				for (const e of nodePool.values()) {
					const d = Math.hypot(e.x || 0, e.y || 0, e.z || 0);
					if (d > maxR) maxR = d;
				}
				const th = Math.random() * Math.PI * 2;
				const ph = Math.acos(2 * Math.random() - 1);
				const r = maxR + 30;   // just past the edge — shorter glide, less pull on the flock
				o = {
					id: n.id,
					__lastTrig: performance.now(), // born bright
					x: r * Math.sin(ph) * Math.cos(th),
					y: r * Math.sin(ph) * Math.sin(th),
					z: r * Math.cos(ph)
				};
				nodePool.set(n.id, o);
			}
			o.pitch = n.pitch;
			return o;
		});
		const gLinks = links
			.filter((l) => l.source !== l.target) // self-links have zero length in 3D — skip rendering
			.map((l) => ({ source: l.source, target: l.target, weight: l.weight, sourceId: l.source, targetId: l.target }));
		graph3d.graphData({ nodes: gNodes, links: gLinks });
		updateNodeStyles();
	}

	// Recolor sprites / link materials from interaction + playhead state.
	// Opacity is owned by the anim loop (fade system) — this sets colors,
	// flags, and re-triggers the fade for playing nodes.
	function updateNodeStyles() {
		if (!graph3d) return;
		const now = performance.now();
		for (const [id, n] of nodePool) {
			const mat = n.__mat;
			if (!mat) continue;
			const isP1 = id === $currentNodeId;
			const isP2 = id === $currentNode2Id;
			const isFocusSource = focusedNodeId === id;
			const isFocusTarget = focusedOutputTargets.has(id);
			const isConnectSource = connectFromNode === id;
			const color =
				isP1 ? C.chain1 : isP2 ? C.chain2
				: isConnectSource || isFocusSource ? C.focus
				: isFocusTarget ? C.focusTarget : C.primary;
			mat.uniforms.uColor.value.set(color);
			if (n.__label) n.__label.color = (isP1 || isP2 || isFocusSource) ? color : C.textDim;
			n.__orphan = !nodesWithInput.has(id) && !isFocusSource && !isFocusTarget;
			n.__active = isP1 || isP2;
			if (n.__active) n.__lastTrig = now;   // light back up + restart the fade
		}
		const gd = graph3d.graphData();
		for (const l of gd.links) {
			if (!l.__mat) continue;
			const focused = focusedNodeId !== null && l.sourceId === focusedNodeId;
			l.__mat.color.set(focused ? C.linkFocus : C.link);
			l.__mat.opacity = focused ? 0.9 : 0.45;
		}
	}

	// Per-frame: camera auto-orbit + auto-fit, star fades, playhead twinkle
	function animLoop() {
		const now = performance.now();
		const t = now / 1000;
		const dt = Math.min(0.1, lastAnimT ? t - lastAnimT : 0.016);
		lastAnimT = t;

		// Constellation bounding radius (also reused for auto-fit)
		let maxR = 0;
		for (const n of nodePool.values()) {
			const d = Math.hypot(n.x || 0, n.y || 0, n.z || 0);
			if (d > maxR) maxR = d;
		}

		// Slow orbit — azimuth spins steadily, elevation sways at its own
		// rate — while the radius eases toward framing the whole network.
		// Operates on the camera's current spherical position, so user zoom
		// and manual rotation compose naturally (and pause interaction).
		if (graph3d && !orbitPaused && THREE) {
			const cam = graph3d.camera();
			if (!_sph) _sph = new THREE.Spherical();
			_sph.setFromVector3(cam.position);
			_sph.theta += ORBIT_AZ_SPEED * dt;
			_sph.phi += Math.sin(t * ORBIT_POL_RATE * 2 * Math.PI) * ORBIT_POL_AMP * dt;
			_sph.phi = Math.max(0.35, Math.min(Math.PI - 0.35, _sph.phi));
			if (maxR > 0) {
				const fovV = (cam.fov * Math.PI) / 180;
				const tanV = Math.tan(fovV / 2);
				const tanH = tanV * cam.aspect;
				const fitDist = Math.max(80, ((maxR + 14) / Math.min(tanV, tanH)) * FIT_PADDING);
				_sph.radius += (fitDist - _sph.radius) * Math.min(1, dt * FIT_LERP);
			}
			cam.position.setFromSpherical(_sph);
			cam.lookAt(0, 0, 0);
		}

		// Fades + twinkle (shader uniforms)
		for (const n of nodePool.values()) {
			const mat = n.__mat;
			if (!mat) continue;
			const base = nodeBaseScale(n.pitch ?? 2048);
			let glow;
			if (n.__active) {
				glow = 1;
				mat.uniforms.uScale.value = base * (1.5 + 0.3 * Math.sin(t * 6));
			} else {
				const age = (now - (n.__lastTrig || 0)) / 1000;
				glow = 1 - Math.min(1, age / FADE_SECONDS) * (1 - MIN_GLOW);
				mat.uniforms.uScale.value = base;
			}
			if (n.__orphan) glow *= 0.5;
			mat.uniforms.uIntensity.value = glow;
			mat.uniforms.uTime.value = t;
			if (n.__label) n.__label.material.opacity = Math.max(0.1, glow * 0.8);
		}

		pulseFrameId = requestAnimationFrame(animLoop);
	}

	async function initGraph3d() {
		const [{ default: ForceGraph3D }, three, spriteText] = await Promise.all([
			import('3d-force-graph'),
			import('three'),
			import('three-spritetext')
		]);
		THREE = three;
		SpriteText = spriteText.default;
		starTexture = makeStarTexture();

		graph3d = ForceGraph3D()(graphEl)
			.backgroundColor('rgba(0,0,0,0)')
			.showNavInfo(false)
			.nodeThreeObject((node) => makeStarObject(node))
			.nodeLabel((n) => `<span class="g3d-tip">[${String(n.id).padStart(3, '0')}] ${nodeLabels.get(n.id) || ''}</span>`)
			.linkMaterial((link) => {
				const mat = new THREE.LineDashedMaterial({
					color: C.link,
					transparent: true,
					opacity: 0.45,
					dashSize: 2.5,
					gapSize: 2
				});
				link.__mat = mat;
				return mat;
			})
			.linkPositionUpdate((obj, { start, end }) => {
				// Custom update so LineDashedMaterial gets its line distances
				const pos = obj.geometry?.getAttribute?.('position');
				if (!pos) return false;
				pos.setXYZ(0, start.x, start.y, start.z);
				pos.setXYZ(1, end.x, end.y, end.z);
				pos.needsUpdate = true;
				obj.computeLineDistances();
				return true;
			})
			.onNodeClick((node, ev) => {
				graphClickTs = Date.now();
				if (connectFromNode !== null) {
					if (connectFromNode !== node.id) sendAddLink(connectFromNode, node.id);
					connectFromNode = null;
					contextMenu = null;
					return;
				}
				focusedNodeId = node.id;
				contextMenu = { x: ev.clientX, y: ev.clientY, type: 'node',
					data: { id: node.id, pitch: node.pitch } };
			})
			.onLinkClick((link, ev) => {
				graphClickTs = Date.now();
				if (connectFromNode !== null) { connectFromNode = null; return; }
				contextMenu = { x: ev.clientX, y: ev.clientY, type: 'link',
					data: { sourceId: link.sourceId, targetId: link.targetId } };
			})
			.onNodeDragEnd((n) => {
				// release after drag so the constellation stays organic
				n.fx = n.fy = n.fz = undefined;
			});

		// Gentle physics: weak charge + soft links + heavy damping + slow
		// cooling — arrivals glide in and neighbors barely stir
		graph3d.d3Force('charge').strength(-45);
		graph3d.d3Force('link').distance(35).strength(0.15);
		graph3d.d3VelocityDecay(0.6);
		graph3d.d3AlphaDecay(0.04);
		graph3d.cameraPosition({ x: 0, y: 0, z: 220 });

		// Deep-space backdrop: two shells of faint distant stars — parallax
		// against the near constellation makes the orbit readable.
		const scene = graph3d.scene();
		const cam = graph3d.camera();
		cam.far = 8000;
		cam.updateProjectionMatrix();
		for (const [count, rMin, rMax, size, opacity] of [
			[700, 1000, 2200, 7, 0.75],
			[200, 650, 1000, 11, 0.9]
		]) {
			const pos = new Float32Array(count * 3);
			for (let i = 0; i < count; i++) {
				const th = Math.random() * Math.PI * 2;
				const ph = Math.acos(2 * Math.random() - 1);
				const r = rMin + Math.random() * (rMax - rMin);
				pos[i * 3]     = r * Math.sin(ph) * Math.cos(th);
				pos[i * 3 + 1] = r * Math.sin(ph) * Math.sin(th);
				pos[i * 3 + 2] = r * Math.cos(ph);
			}
			const geo = new THREE.BufferGeometry();
			geo.setAttribute('position', new THREE.BufferAttribute(pos, 3));
			const mat = new THREE.PointsMaterial({
				color: 0xffdf88,
				size,
				map: starTexture,
				transparent: true,
				opacity,
				sizeAttenuation: true,
				depthWrite: false,
				blending: THREE.AdditiveBlending
			});
			const pts = new THREE.Points(geo, mat);
			pts.raycast = () => {};   // backdrop must never intercept clicks
			scene.add(pts);
		}

		// Pause the auto-orbit while the user is dragging/zooming; resume
		// a few seconds after they let go.
		const controls = graph3d.controls();
		controls.addEventListener('start', () => {
			orbitPaused = true;
			clearTimeout(orbitResumeTimer);
		});
		controls.addEventListener('end', () => {
			clearTimeout(orbitResumeTimer);
			orbitResumeTimer = setTimeout(() => { orbitPaused = false; }, 4000);
		});

		syncGraph($graphNodes, $graphLinks);
		pulseFrameId = requestAnimationFrame(animLoop);
	}

	// ═══════════════════════════════════════════════════════════
	// Scale / keyboard
	// ═══════════════════════════════════════════════════════════
	const switchLabels = ['AUTO', 'MID', 'DOWN'];
	const SCALE_NAMES = ['Chromatic','Major','Minor','Pentatonic','Min Penta','Blues','Dorian','Mixolydian'];
	const WAVEFORM_NAMES = ['Sine','Triangle','Square','Pulse','Sawtooth','Supersaw'];
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
	let autoIntervalLocal = null;
	function handleAutoIntervalInput(e) { autoIntervalLocal = parseInt(e.target.value); }
	function handleAutoIntervalChange(e) { autoIntervalLocal = null; sendSetAutoInterval(parseInt(e.target.value)); }
	$: autoIntervalDisplay = autoIntervalLocal ?? $autoInterval;
	$: autoIntervalSec = (0.25 + (autoIntervalDisplay / 127) * (8 - 0.25)).toFixed(1);
	let maxNodesLocal = null;
	function handleMaxNodesInput(e) { maxNodesLocal = parseInt(e.target.value); }
	function handleMaxNodesChange(e) { maxNodesLocal = null; sendSetMaxNodes(parseInt(e.target.value)); }
	$: maxNodesDisplay = maxNodesLocal ?? $maxNodes;

	// Diagnostics rows (ISR timing) only shown with ?debug=true in the URL
	const debugMode = typeof window !== 'undefined'
		&& new URLSearchParams(window.location.search).get('debug') === 'true';

	// ═══ Performance mode — network only, everything else hidden ═══
	let perfMode = false;
	function handleKeydown(e) {
		if (e.key === 'Escape' && perfMode) perfMode = false;
	}
	// Effective sidebar offset: zero when the sidebar is hidden in perf mode,
	// so the SVG stretches to the full viewport and the network re-centers.
	$: effSidebarW = perfMode ? 0 : sidebarW;

	// Manual device routing (hub / IAC): channel pickers appear for non-card devices
	$: isManualDevice = $selectedDevice && !isAutoDetectedName($selectedDevice);
	function handleTxChChange(e) { setNoteTxChannel(parseInt(e.target.value)); }
	function handleRxChChange(e) { setStatusRxChannel(parseInt(e.target.value)); }
	function handleWaveform1Change(e) { sendSetWaveform1(parseInt(e.target.value)); }
	function handleBassMode1Toggle() { sendSetBassMode1(!$bassMode1); }
	function handleWaveform2Change(e) { sendSetWaveform2(parseInt(e.target.value)); }
	function handleBassMode2Toggle() { sendSetBassMode2(!$bassMode2); }
	// Clock divide/multiply level (signed half-steps from unity) → display string. 0 = ×1.
	function fmtMult(n) {
		if (n >= 0) {
			const m = 1 + 0.5 * n;             // ×1, ×1.5, ×2, …
			return '×' + (Number.isInteger(m) ? m : m.toFixed(1));
		}
		const d = 1 - 0.5 * n;                 // n < 0 → ÷1.5, ÷2, …
		return '÷' + (Number.isInteger(d) ? d : d.toFixed(1));
	}
	// clockMode: 0=both internal, 1=ext drives seq1 (knob mults seq2), 2=ext drives seq2 (knob mults seq1), 3=both external
	$: clockModeLabel = ['INTERNAL', 'EXT · SEQ1', 'EXT · SEQ2', 'EXT · BOTH'][$clockMode] || '?';
	$: multStr = $clockMode === 1 ? `${fmtMult($clockMultLevel)} SEQ2`
		: $clockMode === 2 ? `${fmtMult($clockMultLevel)} SEQ1`
		: $clockMode === 3 ? 'AUTO INT'
		: '×1 BOTH';
	$: rangeOct = ((1 + ($knobY * 35) / 4095) * 2 / 12).toFixed(1);
	function handleKeyClick(midiNote) { sendNoteOn(midiNote, 100); }

	let scaleSelectEl;
	$: if (scaleSelectEl) scaleSelectEl.selectedIndex = $scaleIndex;
	let waveformSelect1El, waveformSelect2El;
	$: if (waveformSelect1El) waveformSelect1El.selectedIndex = $waveformIndex1;
	$: if (waveformSelect2El) waveformSelect2El.selectedIndex = $waveformIndex2;

	// ═══════════════════════════════════════════════════════════
	// Resize
	// ═══════════════════════════════════════════════════════════
	function handleResize() {
		if (sidebarEl) sidebarW = sidebarEl.offsetWidth;
		if (graph3d && graphEl) {
			graph3d.width(graphEl.clientWidth).height(graphEl.clientHeight);
		}
	}

	// ═══════════════════════════════════════════════════════════
	// Context menu (node/link clicks arrive via the 3D graph callbacks)
	// ═══════════════════════════════════════════════════════════
	function dismissMenu() {
		// Canvas clicks bubble up to the window handler right after the 3D
		// library fires onNodeClick/onLinkClick — don't instantly close the
		// menu those clicks just opened.
		if (Date.now() - graphClickTs < 200) return;
		contextMenu = null;
		if (connectFromNode !== null) connectFromNode = null;
		focusedNodeId = null;
	}
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
	$: if (graph3d && ($graphNodes || $graphLinks)) syncGraph($graphNodes, $graphLinks);

	// Restyle stars/links whenever playheads or interaction state change
	$: if (graph3d && ($currentNodeId !== undefined || $currentNode2Id !== undefined
		|| focusedNodeId !== undefined || connectFromNode !== undefined || nodesWithInput)) {
		updateNodeStyles();
	}

	// Keep floating labels in sync with the scale (note names re-quantize)
	$: if (graph3d && nodeLabels) {
		for (const n of nodePool.values()) {
			if (n.__label && n.__label.text !== labelFor(n)) n.__label.text = labelFor(n);
		}
	}

	// Sort nodes by ID for the metadata list
	$: sortedNodes = [...displayNodes].sort((a, b) => a.id - b.id);

	let sidebarObserver = null;
	let graphObserver = null;
	onMount(() => {
		handleResize();
		if (sidebarEl) {
			sidebarObserver = new ResizeObserver(() => {
				sidebarW = sidebarEl.offsetWidth;
			});
			sidebarObserver.observe(sidebarEl);
		}
		initGraph3d().then(() => {
			if (graphEl) {
				graphObserver = new ResizeObserver(() => {
					if (graph3d) graph3d.width(graphEl.clientWidth).height(graphEl.clientHeight);
				});
				graphObserver.observe(graphEl);
				graph3d.width(graphEl.clientWidth).height(graphEl.clientHeight);
			}
		});
	});
	onDestroy(() => {
		if (pulseFrameId) cancelAnimationFrame(pulseFrameId);
		clearTimeout(orbitResumeTimer);
		if (sidebarObserver) sidebarObserver.disconnect();
		if (graphObserver) graphObserver.disconnect();
		if (graph3d) { graph3d._destructor?.(); graph3d = null; }
	});
</script>

<svelte:window on:click={dismissMenu} on:resize={handleResize} on:keydown={handleKeydown} />

<div class="crt" bind:this={containerEl}>
	<!-- 3D star-field network -->
	<div bind:this={graphEl} class="monitor"
		style="left: {effSidebarW}px; width: calc(100% - {effSidebarW}px); cursor: {connectFromNode !== null ? 'crosshair' : 'default'};"></div>

	<!-- Empty state overlay -->
	{#if !$midiConnected}
		<div class="empty-msg" style="left: {effSidebarW}px; width: calc(100% - {effSidebarW}px);">NO SIGNAL — CONNECT TO MIDI</div>
	{:else if displayNodes.length === 0}
		<div class="empty-msg" style="left: {effSidebarW}px; width: calc(100% - {effSidebarW}px);">NO SIGNAL — FLIP SWITCH UP TO CREATE NODES</div>
	{/if}

	<!-- CRT overlay layers -->
	<div class="scanlines"></div>
	<div class="flicker"></div>

	<!-- ═══ Header bar ═══ -->
	<div class="header" class:perf-hide={perfMode}>
		<span class="title">ZODIAC SEQUENCER CARD</span>
		<span class="subtitle"> | BETA v0.3.0</span>
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
			{#if isManualDevice}
				<select class="sel" value={$noteTxChannel} on:change={handleTxChChange} title="OUT: channel for notes we send to this device (e.g. IAC). Your router must deliver them to the card on CH 1.">
					{#each Array(16) as _, i}<option value={i + 1}>OUT CH {i + 1}</option>{/each}
				</select>
				<select class="sel" value={$statusRxChannel} on:change={handleRxChChange} title="IN: only read status CCs arriving on this channel — use when the bus (e.g. IAC) carries other traffic. ANY = no filter.">
					<option value={0}>IN CH ANY</option>
					{#each Array(16) as _, i}<option value={i + 1}>IN CH {i + 1}</option>{/each}
				</select>
			{/if}
			{#if $midiConnected}
				<button class="btn" on:click={requestPull}>↻ PULL</button>
			{/if}
			<button class="btn" on:click={() => (perfMode = true)} title="Show only the network (Esc to exit)">◱ PERFORMANCE MODE</button>
		</div>
	</div>

	<!-- ═══ Left metadata panel ═══ -->
	<div class="sidebar" class:perf-hide={perfMode} bind:this={sidebarEl} on:click|stopPropagation>
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
			<a class="btn btn-dl" href="{base}/zodiac-card-v0.3.uf2" download>⬇ DOWNLOAD .UF2</a>
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
			{#if debugMode}
				<div class="meta-row" title="All incoming MIDI messages, before any filtering"><span class="ml">MIDI_RX</span><span class="mv">{$rxMsgCount}</span></div>
			{/if}
			<div class="meta-row"><span class="ml">N_NODES</span><span class="mv">{String($nodeCount).padStart(2, '0')}</span></div>
			<div class="meta-row"><span class="ml">SEQ1_ID</span><span class="mv c1">{$currentNodeId >= 0 ? String($currentNodeId).padStart(3, '0') : '---'}</span></div>
			<div class="meta-row"><span class="ml">SEQ2_ID</span><span class="mv c2">{$currentNode2Id >= 0 ? String($currentNode2Id).padStart(3, '0') : '---'}</span></div>
			<div class="meta-row"><span class="ml">SEQ1_BPM</span><span class="mv c1">{$midiConnected ? $bpm1 : '---'}</span></div>
			<div class="meta-row"><span class="ml">SEQ2_BPM</span><span class="mv c2">{$midiConnected ? $bpm2 : '---'}</span></div>
			<div class="meta-row"><span class="ml">CLK_SRC</span><span class="mv">{$midiConnected ? clockModeLabel : '---'}</span></div>
			<div class="meta-row"><span class="ml">CLK_MULT</span><span class="mv">{$midiConnected ? multStr : '---'}</span></div>
			{#if debugMode}
				<div class="meta-row"><span class="ml">ISR_PEAK</span><span class="mv" style={$isrPeakUs > 41 ? 'color:#ff5533' : ''}>{$midiConnected ? $isrPeakUs + 'µs' : '---'}</span></div>
				<div class="meta-row" title="Per-section worst case: graph ops / control+clock / synthesis"><span class="ml">ISR_SECT</span><span class="mv">{$midiConnected ? `${$isrSections[0]}/${$isrSections[1]}/${$isrSections[2]}` : '---'}</span></div>
				<div class="meta-row" title="Complete SysEx messages received (node data travels as SysEx)"><span class="ml">SYSEX_RX</span><span class="mv">{$sysexRxCount}</span></div>
			{/if}
			<div class="meta-row"><span class="ml">LINK_PROB</span><span class="mv">{Math.round(($knobMain / 4095) * 100)}%</span></div>
			<div class="meta-row"><span class="ml">PITCH_RANGE</span><span class="mv">{rangeOct} OCT</span></div>
			<div class="meta-row"><span class="ml">SWITCH_POS</span><span class="mv">{switchLabels[$switchState] || '?'}</span></div>
		</details>

		{#if debugMode}
			<details class="side-section" open on:click={toggleDetails}>
				<summary class="side-title">MIDI DEBUG</summary>
				<div class="meta-row"><span class="ml">OUT</span><span class="mv">{$debugMidiInfo.outName.slice(0, 16)}</span></div>
				<div class="meta-row"><span class="ml">OUT_STATE</span><span class="mv">{$debugMidiInfo.outState || '---'}</span></div>
				{#each $debugMidiInfo.inputs as p}
					<div class="meta-row" title="{p.name} — {p.state}{p.bound ? ' (bound as sequencer input)' : ''}">
						<span class="ml">{p.bound ? '▶' : '·'}{p.name.slice(0, 13)}</span>
						<span class="mv">{p.count}</span>
					</div>
				{:else}
					<div class="meta-row"><span class="ml">INPUTS</span><span class="mv">NONE</span></div>
				{/each}
			</details>
		{/if}

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
		<div class="banner" class:perf-hide={perfMode} style="left: {effSidebarW}px;">
			⚠ SELECT TARGET FOR LINK FROM [{connectFromNode}] →
			<button class="btn btn-cancel" on:click|stopPropagation={() => { connectFromNode = null; }}>CANCEL</button>
		</div>
	{/if}

	<!-- ═══ Performance mode overlays ═══ -->
	{#if perfMode}
		<button class="perf-exit" on:click={() => (perfMode = false)} title="Exit performance mode (Esc)">◱</button>
		<div class="perf-dot" class:on={$midiConnected} title={$midiConnected ? 'CONNECTED' : 'DISCONNECTED'}></div>
	{/if}

	<!-- ═══ Bottom controls & keyboard ═══ -->
	<div class="bottom-bar" class:perf-hide={perfMode} style="left: {effSidebarW}px;">
		<div class="bottom-controls">
			<div class="bottom-ctrl">
				<span class="ctrl-label">SCALE</span>
				<select class="sel sel-input" bind:this={scaleSelectEl} on:change={handleScaleChange}>
					{#each SCALE_NAMES as name, i}<option value={i}>{name}</option>{/each}
				</select>
			</div>
			<div class="bottom-ctrl">
				<span class="ctrl-label">AUTO</span>
				<div class="slider-row">
					<input type="range" class="slider" min="0" max="127" value={$autoInterval} on:input={handleAutoIntervalInput} on:change={handleAutoIntervalChange}>
					<span class="mv">{autoIntervalSec}s</span>
				</div>
			</div>
			<div class="bottom-ctrl">
				<span class="ctrl-label">MAX NODES</span>
				<div class="slider-row">
					<input type="range" class="slider" min="2" max="64" step="1" value={$maxNodes} on:input={handleMaxNodesInput} on:change={handleMaxNodesChange}>
					<span class="mv">{maxNodesDisplay}</span>
				</div>
			</div>
			<div class="chain-ctrl chain1-ctrl">
				<span class="chain-label c1">CH1</span>
				<div class="bottom-ctrl">
					<span class="ctrl-label">WAVE</span>
					<select class="sel sel-chain1" bind:this={waveformSelect1El} on:change={handleWaveform1Change}>
						{#each WAVEFORM_NAMES as name, i}<option value={i}>{name}</option>{/each}
					</select>
				</div>
				<div class="bottom-ctrl">
					<span class="ctrl-label">RANGE</span>
					<button class="btn-toggle c1" class:active={$bassMode1} on:click={handleBassMode1Toggle}>
						{$bassMode1 ? 'BASS' : 'FULL'}
					</button>
				</div>
			</div>
			<div class="chain-ctrl chain2-ctrl">
				<span class="chain-label c2">CH2</span>
				<div class="bottom-ctrl">
					<span class="ctrl-label">WAVE</span>
					<select class="sel sel-chain2" bind:this={waveformSelect2El} on:change={handleWaveform2Change}>
						{#each WAVEFORM_NAMES as name, i}<option value={i}>{name}</option>{/each}
					</select>
				</div>
				<div class="bottom-ctrl">
					<span class="ctrl-label">RANGE</span>
					<button class="btn-toggle c2" class:active={$bassMode2} on:click={handleBassMode2Toggle}>
						{$bassMode2 ? 'BASS' : 'FULL'}
					</button>
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

	/* ═══ Performance mode ═══ */
	.perf-hide { display: none !important; }
	.perf-exit {
		position: absolute; top: 8px; right: 8px; z-index: 60;
		width: 28px; height: 28px;
		background: transparent; border: 1px solid #332b00; color: #806600;
		font-family: inherit; font-size: 13px; line-height: 1;
		cursor: pointer; opacity: 0.5; transition: opacity 0.15s;
	}
	.perf-exit:hover { opacity: 1; color: #ffcc00; border-color: #ffcc00; }
	.perf-dot {
		position: absolute; top: 17px; right: 48px; z-index: 60;
		width: 10px; height: 10px; border-radius: 50%;
		background: #402020;
	}
	.perf-dot.on { background: #00e5a0; box-shadow: 0 0 8px #00e5a0; }

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
		overflow: hidden;
	}

	.empty-msg {
		position: absolute;
		top: 50%;
		transform: translateY(-50%);
		text-align: center;
		color: #aa8800;
		font-size: 16px;
		pointer-events: none;
		z-index: 5;
	}

	/* Hover tooltip injected by 3d-force-graph */
	:global(.scene-tooltip) {
		font-family: 'Monaspace Krypton', monospace !important;
		color: #ffcc00 !important;
		font-size: 12px;
	}

	/* ═══ Header ═══ */
	.header {
		position: absolute; top: 0; left: 0; right: 0;
		background: rgba(5, 5, 0, 0.92);
		border-bottom: 1px solid #4a3d00;
		padding: 8px 16px;
		display: flex; align-items: center; gap: 12px;
		flex-wrap: wrap;
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

	@media (max-width: 1024px) {
		.sidebar {
			flex-wrap: nowrap;
			overflow-y: auto;
			overflow-x: hidden;
			width: 220px;
			padding-bottom: 12px;
		}
		.side-section {
			width: 196px;
		}
	}

	.sidebar::-webkit-scrollbar {
		width: 4px;
	}
	.sidebar::-webkit-scrollbar-track {
		background: #050500;
	}
	.sidebar::-webkit-scrollbar-thumb {
		background: #4a3d00;
		border-radius: 2px;
	}
	.sidebar::-webkit-scrollbar-thumb:hover {
		background: #aa8800;
	}
	.sidebar {
		scrollbar-width: thin;
		scrollbar-color: #4a3d00 #050500;
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
		display: flex; flex-direction: column; gap: 8px;
		background: rgba(5, 5, 0, 0.92);
		padding: 8px 16px;
		z-index: 20;
	}
	.bottom-controls {
		display: flex; gap: 12px; align-items: center;
		flex-wrap: wrap;
	}
	.bottom-ctrl { display: flex; align-items: center; gap: 6px; }
	.ctrl-label {
		font-size: 11px; color: #aa8800;
		letter-spacing: 2px; white-space: nowrap;
	}
	.chain-ctrl {
		display: flex; align-items: center; gap: 6px;
		padding: 4px 8px;
		border: 1px solid;
	}
	.chain1-ctrl { border-color: rgba(0, 229, 160, 0.25); }
	.chain2-ctrl { border-color: rgba(0, 204, 255, 0.25); }
	.chain-label {
		font-size: 11px; letter-spacing: 2px; font-weight: bold;
	}
	.chain-label.c1 { color: #00e5a0; }
	.chain-label.c2 { color: #00ccff; }
	.sel-chain1 { color: #00e5a0; border-color: #004a35; }
	.sel-chain1:focus { border-color: #00e5a0; }
	.sel-chain2 { color: #00ccff; border-color: #003a4a; }
	.sel-chain2:focus { border-color: #00ccff; }
	.btn-toggle.c1 { color: #00e5a0; border-color: #004a35; }
	.btn-toggle.c1:hover { background: rgba(0, 229, 160, 0.1); border-color: #00e5a0; }
	.btn-toggle.c1.active { background: rgba(0, 229, 160, 0.15); border-color: #00e5a0; box-shadow: 0 0 6px rgba(0, 229, 160, 0.3); }
	.btn-toggle.c2 { color: #00ccff; border-color: #003a4a; }
	.btn-toggle.c2:hover { background: rgba(0, 204, 255, 0.1); border-color: #00ccff; }
	.btn-toggle.c2.active { background: rgba(0, 204, 255, 0.15); border-color: #00ccff; box-shadow: 0 0 6px rgba(0, 204, 255, 0.3); }
	.slider-row .slider { min-width: 80px; }
	.kbd-section {
		display: flex; flex-direction: column; gap: 4px;
	}
	.kbd-label {
		font-size: 11px; color: #aa8800;
		letter-spacing: 2px; white-space: nowrap;
	}
	.keyboard {
		display: flex; flex-direction: column; gap: 2px;
		overflow-x: auto;
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
		padding: 4px 4px;
		font-size: 11px;
	}
	.note-btn.is-black.gap-after {
		margin-right: 44px;
	}

	@media (max-width: 900px) {
		.bottom-controls {
			gap: 8px;
		}
		.chain-ctrl {
			gap: 4px;
			padding: 3px 6px;
		}
		.note-btn {
			min-width: 36px;
			padding: 4px 4px;
			font-size: 11px;
		}
		.note-btn.is-black {
			min-width: 30px;
			padding: 3px 3px;
			font-size: 10px;
		}
		.note-btn.is-black.gap-after {
			margin-right: 36px;
		}
	}

	@media (max-width: 700px) {
		.bottom-bar {
			padding: 6px 8px;
		}
		.bottom-controls {
			gap: 6px;
		}
		.ctrl-label {
			font-size: 10px;
			letter-spacing: 1px;
		}
		.chain-label {
			font-size: 10px;
			letter-spacing: 1px;
		}
		.sel, .btn-toggle {
			font-size: 11px;
			padding: 3px 6px;
		}
		.note-btn {
			min-width: 28px;
			padding: 3px 2px;
			font-size: 10px;
		}
		.note-btn.is-black {
			min-width: 24px;
			font-size: 9px;
		}
		.note-btn.is-black.gap-after {
			margin-right: 28px;
		}
		.kbd-black-row {
			padding-left: 16px;
			gap: 2px;
		}
		.kbd-white-row {
			gap: 2px;
		}
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

	@media (max-width: 1024px) {
		.disclaimer {
			position: static;
			padding: 8px 0;
		}
	}

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
	.btn-toggle {
		background: transparent;
		border: 1px solid;
		padding: 4px 12px;
		font-family: inherit; font-size: 12px; letter-spacing: 1px;
		cursor: pointer; transition: all 0.15s;
		min-width: 52px;
	}

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
