#!/usr/bin/env bun
import { GlobalRegistrator } from '@happy-dom/global-registrator';

type LayoutNode = {
	id?: unknown;
	label?: unknown;
	mostSpecificType?: unknown;
	x?: number;
	y?: number;
};

type LayoutLink = {
	source?: unknown;
	target?: unknown;
	label?: unknown;
	relName?: unknown;
};

type Box = {
	node: LayoutNode;
	label: string;
	x: number;
	y: number;
	w: number;
	h: number;
};

const defaultXml = 'docs/generated/forge-graphs/forge-graph-nxtrt-model.rkt--rich-runtime-shape-witness.xml';

const args = parseArgs(Bun.argv.slice(2));
if (args.help) {
	printHelp();
	process.exit(0);
}

GlobalRegistrator.register();

const { solveForgeGraphLayout } = await import('@forge-fm/forge-graph');
const xml = await Bun.file(args.xmlPath || defaultXml).text();
const layout = await quietConsole(() => solveForgeGraphLayout({
	xml,
	width: args.layoutWidth,
	height: args.layoutHeight,
}));

process.stdout.write(renderUnicodeGraph(layout.nodes as LayoutNode[], layout.links as LayoutLink[], {
	width: args.width,
	height: args.height,
	showLabels: args.showLabels,
}));

function parseArgs(argv: string[]) {
	let xmlPath = '';
	let width = terminalWidth();
	let height = 34;
	let layoutWidth = 900;
	let layoutHeight = 520;
	let showLabels = true;
	let help = false;

	for (let i = 0; i < argv.length; i += 1) {
		const arg = argv[i];
		switch (arg) {
			case '--width':
				width = Number(argv[++i] || width);
				break;
			case '--height':
				height = Number(argv[++i] || height);
				break;
			case '--layout-width':
				layoutWidth = Number(argv[++i] || layoutWidth);
				break;
			case '--layout-height':
				layoutHeight = Number(argv[++i] || layoutHeight);
				break;
			case '--no-labels':
				showLabels = false;
				break;
			case '--help':
			case '-h':
				help = true;
				break;
			default:
				if (arg.startsWith('--')) {
					throw new Error(`unknown option: ${arg}`);
				}
				xmlPath = arg;
				break;
		}
	}

	return {
		xmlPath,
		width: clamp(Math.floor(width), 48, 180),
		height: clamp(Math.floor(height), 18, 80),
		layoutWidth,
		layoutHeight,
		showLabels,
		help,
	};
}

function printHelp() {
	console.log(`Usage:
  bun cassette/forge-unicode-graph.ts [XML]
  bun cassette/forge-unicode-graph.ts --width 100 --height 32
  bun cassette/forge-unicode-graph.ts --no-labels`);
}

async function quietConsole<T>(fn: () => Promise<T>): Promise<T> {
	const originalLog = console.log;
	console.log = () => {};
	try {
		return await fn();
	} finally {
		console.log = originalLog;
	}
}

function renderUnicodeGraph(nodes: LayoutNode[], links: LayoutLink[], options: { width: number; height: number; showLabels: boolean }) {
	const grid = createGrid(options.width, options.height);
	const boxes = buildBoxes(nodes, options.width, options.height);
	const nodeToBox = new Map<LayoutNode, Box>(boxes.map((box) => [box.node, box]));
	const labels: Array<{ x: number; y: number; text: string }> = [];

	for (const link of links) {
		const source = endpoint(link.source, nodes);
		const target = endpoint(link.target, nodes);
		const sourceBox = source ? nodeToBox.get(source) : undefined;
		const targetBox = target ? nodeToBox.get(target) : undefined;
		if (!sourceBox || !targetBox) {
			continue;
		}
		const label = drawEdge(grid, sourceBox, targetBox, edgeLabel(link), options.showLabels);
		if (label) {
			labels.push(label);
		}
	}

	for (const label of labels) {
		writeText(grid, label.x, label.y, label.text);
	}

	for (const box of boxes) {
		drawBox(grid, box);
	}

	return grid.map((row) => row.join('').replace(/\s+$/u, '')).join('\n') + '\n';
}

function buildBoxes(nodes: LayoutNode[], width: number, height: number): Box[] {
	const xs = nodes.map((node) => finite(node.x, 0));
	const ys = nodes.map((node) => finite(node.y, 0));
	const minX = Math.min(...xs);
	const maxX = Math.max(...xs);
	const minY = Math.min(...ys);
	const maxY = Math.max(...ys);
	const padX = 4;
	const padY = 2;

	return nodes.map((node) => {
		const label = atomLabel(node);
		const w = clamp(label.length + 4, 7, 18);
		const h = 3;
		const cx = scale(finite(node.x, 0), minX, maxX, padX + Math.ceil(w / 2), width - padX - Math.ceil(w / 2) - 1);
		const cy = scale(finite(node.y, 0), minY, maxY, padY + 1, height - padY - 2);
		return {
			node,
			label,
			x: Math.round(cx - w / 2),
			y: Math.round(cy - h / 2),
			w,
			h,
		};
	});
}

function drawBox(grid: string[][], box: Box) {
	const right = box.x + box.w - 1;
	const bottom = box.y + box.h - 1;
	for (let y = box.y; y <= bottom; y += 1) {
		for (let x = box.x; x <= right; x += 1) {
			put(grid, x, y, ' ');
		}
	}
	put(grid, box.x, box.y, '┌');
	put(grid, right, box.y, '┐');
	put(grid, box.x, bottom, '└');
	put(grid, right, bottom, '┘');
	for (let x = box.x + 1; x < right; x += 1) {
		put(grid, x, box.y, '─');
		put(grid, x, bottom, '─');
	}
	for (let y = box.y + 1; y < bottom; y += 1) {
		put(grid, box.x, y, '│');
		put(grid, right, y, '│');
	}
	writeText(grid, box.x + 2, box.y + 1, box.label.slice(0, box.w - 4));
}

function drawEdge(grid: string[][], source: Box, target: Box, label: string, showLabel: boolean) {
	const points = orthogonalRoute(source, target);

	if (points.length < 2) {
		return;
	}

	for (let i = 0; i < points.length - 1; i += 1) {
		const prev = points[i - 1];
		const current = points[i];
		const next = points[i + 1];
		putLine(grid, current.x, current.y, routeChar(prev, current, next));
	}

	const beforeArrow = points.at(-2);
	const arrowAt = points.at(-1);
	if (beforeArrow && arrowAt) {
		put(grid, arrowAt.x, arrowAt.y, arrowChar(arrowAt.x - beforeArrow.x, arrowAt.y - beforeArrow.y));
	}

	if (showLabel && label) {
		const middle = points[Math.floor(points.length / 2)];
		const text = label.slice(0, 14);
		const lx = clamp(middle.x - Math.floor(text.length / 2), 0, grid[0].length - text.length);
		const ly = clamp(middle.y - 1, 0, grid.length - 1);
		return { x: lx, y: ly, text };
	}
	return undefined;
}

function orthogonalRoute(source: Box, target: Box) {
	const start = portToward(source, target);
	const end = portToward(target, source);
	const horizontalFirst = Math.abs(start.x - end.x) >= Math.abs(start.y - end.y);
	const midX = Math.round((start.x + end.x) / 2);
	const midY = Math.round((start.y + end.y) / 2);
	const vertices = horizontalFirst
		? [
			start,
			{ x: midX, y: start.y },
			{ x: midX, y: end.y },
			end,
		]
		: [
			start,
			{ x: start.x, y: midY },
			{ x: end.x, y: midY },
			end,
		];

	return expandOrthogonalPath(vertices)
		.filter((point) => !insideBox(point.x, point.y, source))
		.filter((point) => !insideBox(point.x, point.y, target));
}

function portToward(box: Box, other: Box) {
	const self = center(box);
	const them = center(other);
	const dx = them.x - self.x;
	const dy = them.y - self.y;
	if (Math.abs(dx) >= Math.abs(dy)) {
		return {
			x: dx >= 0 ? box.x + box.w : box.x - 1,
			y: self.y,
		};
	}
	return {
		x: self.x,
		y: dy >= 0 ? box.y + box.h : box.y - 1,
	};
}

function expandOrthogonalPath(vertices: Array<{ x: number; y: number }>) {
	const points: Array<{ x: number; y: number }> = [];
	for (let i = 0; i < vertices.length - 1; i += 1) {
		const a = vertices[i];
		const b = vertices[i + 1];
		const dx = Math.sign(b.x - a.x);
		const dy = Math.sign(b.y - a.y);
		let x = a.x;
		let y = a.y;
		if (points.length === 0) {
			points.push({ x, y });
		}
		while (x !== b.x || y !== b.y) {
			if (x !== b.x) {
				x += dx;
			} else if (y !== b.y) {
				y += dy;
			}
			const last = points.at(-1);
			if (!last || last.x !== x || last.y !== y) {
				points.push({ x, y });
			}
		}
	}
	return points;
}

function endpoint(value: unknown, nodes: LayoutNode[]) {
	if (typeof value === 'number') {
		return nodes[value];
	}
	return value as LayoutNode | undefined;
}

function atomLabel(node: LayoutNode) {
	return String(node.label || node.id || '?').replace(/\s+/gu, '');
}

function edgeLabel(link: LayoutLink) {
	return labelWords(String(link.label || link.relName || '')).toLowerCase();
}

function labelWords(label: string) {
	return label
		.replace(/([a-z0-9])([A-Z])/g, '$1 $2')
		.replace(/([A-Z]+)([A-Z][a-z])/g, '$1 $2')
		.replace(/\s+/g, ' ')
		.trim();
}

function createGrid(width: number, height: number) {
	return Array.from({ length: height }, () => Array.from({ length: width }, () => ' '));
}

function put(grid: string[][], x: number, y: number, char: string) {
	if (y < 0 || y >= grid.length || x < 0 || x >= grid[y].length) {
		return;
	}
	grid[y][x] = char;
}

function putLine(grid: string[][], x: number, y: number, char: string) {
	const existing = grid[y]?.[x];
	if (!existing) {
		return;
	}
	if (existing !== ' ' && existing !== char) {
		grid[y][x] = mergeLine(existing, char);
		return;
	}
	grid[y][x] = char;
}

function writeText(grid: string[][], x: number, y: number, text: string) {
	for (let i = 0; i < text.length; i += 1) {
		put(grid, x + i, y, text[i]);
	}
}

function routeChar(
	prev: { x: number; y: number } | undefined,
	current: { x: number; y: number },
	next: { x: number; y: number } | undefined
) {
	if (!prev || !next) {
		if (!next) {
			return ' ';
		}
		return segmentChar(next.x - current.x, next.y - current.y);
	}
	const inDir = direction(current.x - prev.x, current.y - prev.y);
	const outDir = direction(next.x - current.x, next.y - current.y);
	if ((inDir === 'L' || inDir === 'R') && (outDir === 'L' || outDir === 'R')) {
		return '─';
	}
	if ((inDir === 'U' || inDir === 'D') && (outDir === 'U' || outDir === 'D')) {
		return '│';
	}
	const dirs = new Set([inDir, outDir]);
	if (dirs.has('R') && dirs.has('D')) {
		return '┌';
	}
	if (dirs.has('L') && dirs.has('D')) {
		return '┐';
	}
	if (dirs.has('R') && dirs.has('U')) {
		return '└';
	}
	if (dirs.has('L') && dirs.has('U')) {
		return '┘';
	}
	return '┼';
}

function segmentChar(dx: number, dy: number) {
	if (dy === 0) {
		return '─';
	}
	if (dx === 0) {
		return '│';
	}
	return '┼';
}

function direction(dx: number, dy: number) {
	if (Math.abs(dx) >= Math.abs(dy) && dx !== 0) {
		return dx > 0 ? 'R' : 'L';
	}
	return dy > 0 ? 'D' : 'U';
}

function arrowChar(dx: number, dy: number) {
	if (Math.abs(dx) >= Math.abs(dy)) {
		return dx >= 0 ? '▶' : '◀';
	}
	return dy >= 0 ? '▼' : '▲';
}

function mergeLine(a: string, b: string) {
	if (a === b) {
		return a;
	}
	if ('┌┐└┘│─┼'.includes(a) || '│─┌┐└┘'.includes(b)) {
		return '┼';
	}
	return b;
}

function center(box: Box) {
	return {
		x: Math.round(box.x + (box.w - 1) / 2),
		y: Math.round(box.y + (box.h - 1) / 2),
	};
}

function insideBox(x: number, y: number, box: Box) {
	return x >= box.x && x < box.x + box.w && y >= box.y && y < box.y + box.h;
}

function scale(value: number, min: number, max: number, outMin: number, outMax: number) {
	if (Math.abs(max - min) < 1e-6) {
		return (outMin + outMax) / 2;
	}
	return outMin + ((value - min) / (max - min)) * (outMax - outMin);
}

function finite(value: unknown, fallback: number) {
	return typeof value === 'number' && Number.isFinite(value) ? value : fallback;
}

function clamp(value: number, min: number, max: number) {
	return Math.max(min, Math.min(max, value));
}

function terminalWidth() {
	return Number(process.stdout.columns || 100);
}
