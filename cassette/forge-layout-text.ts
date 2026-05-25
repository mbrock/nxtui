#!/usr/bin/env bun
import { GlobalRegistrator } from '@happy-dom/global-registrator';

type LayoutNode = {
	id?: unknown;
	label?: unknown;
	mostSpecificType?: unknown;
	x?: number;
	y?: number;
};

type AtomInfo = {
	id: string;
	type: string;
};

type FieldInfo = {
	name: string;
	parentType: string;
	typePath: string[];
	tuples: string[][];
	order: number;
};

type InstanceInfo = {
	command: string;
	filename: string;
	version: string;
	atoms: Map<string, AtomInfo>;
	fields: FieldInfo[];
};

const defaultXml = 'docs/generated/forge-graphs/forge-graph-nxtrt-model.rkt--rich-runtime-shape-witness.xml';

const args = parseArgs(Bun.argv.slice(2));
if (args.help) {
	printHelp();
	process.exit(0);
}

GlobalRegistrator.register();

const xml = await Bun.file(args.xmlPath || defaultXml).text();
const instance = parseForgeXml(xml);
const { solveForgeGraphLayout } = await import('@forge-fm/forge-graph');
const layout = await quietConsole(() => solveForgeGraphLayout({
	xml,
	width: args.layoutWidth,
	height: args.layoutHeight,
}));

process.stdout.write(renderLayoutText(instance, layout.nodes as LayoutNode[], args));

function parseArgs(argv: string[]) {
	let xmlPath = '';
	let layoutWidth = 900;
	let layoutHeight = 520;
	let incoming = false;
	let metadata = false;
	let sentences = false;
	let help = false;

	for (let i = 0; i < argv.length; i += 1) {
		const arg = argv[i];
		switch (arg) {
			case '--layout-width':
				layoutWidth = Number(argv[++i] || layoutWidth);
				break;
			case '--layout-height':
				layoutHeight = Number(argv[++i] || layoutHeight);
				break;
			case '--incoming':
				incoming = true;
				break;
			case '--metadata':
				metadata = true;
				break;
			case '--sentences':
			case '--sentence':
				sentences = true;
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

	return { xmlPath, layoutWidth, layoutHeight, incoming, metadata, sentences, help };
}

function printHelp() {
	console.log(`Usage:
  bun cassette/forge-layout-text.ts [XML]
  bun cassette/forge-layout-text.ts --sentences
  bun cassette/forge-layout-text.ts --incoming
  bun cassette/forge-layout-text.ts --metadata`);
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

function parseForgeXml(xml: string): InstanceInfo {
	const doc = new DOMParser().parseFromString(xml, 'application/xml');
	const instance = doc.querySelector('instance');
	if (!instance) {
		throw new Error('Forge XML did not contain an <instance>.');
	}

	const sigNames = new Map<string, string>();
	const atoms = new Map<string, AtomInfo>();
	for (const sig of Array.from(instance.querySelectorAll('sig'))) {
		const id = sig.getAttribute('ID') || '';
		const label = sig.getAttribute('label') || id;
		if (!id || sig.getAttribute('builtin') === 'yes') {
			continue;
		}
		sigNames.set(id, label);
		for (const atom of Array.from(sig.querySelectorAll(':scope > atom'))) {
			const atomId = atom.getAttribute('label') || '';
			if (atomId) {
				atoms.set(atomId, { id: atomId, type: label });
			}
		}
	}

	const fields: FieldInfo[] = [];
	for (const [order, field] of Array.from(instance.querySelectorAll('field')).entries()) {
		const name = field.getAttribute('label') || '';
		if (!name || name === 'no-field-guard') {
			continue;
		}
		const parentType = sigNames.get(field.getAttribute('parentID') || '') || '';
		const typePath = Array.from(field.querySelectorAll(':scope > types > type'))
			.map((type) => sigNames.get(type.getAttribute('ID') || '') || `#${type.getAttribute('ID') || '?'}`);
		const tuples = Array.from(field.querySelectorAll(':scope > tuple'))
			.map((tuple) => Array.from(tuple.querySelectorAll(':scope > atom'))
				.map((atom) => atom.getAttribute('label') || '')
				.filter(Boolean));
		fields.push({ name, parentType, typePath, tuples, order });
	}

	return {
		command: instance.getAttribute('command') || '',
		filename: instance.getAttribute('filename') || '',
		version: instance.getAttribute('version') || '',
		atoms,
		fields,
	};
}

function renderLayoutText(
	instance: InstanceInfo,
	layoutNodes: LayoutNode[],
	options: { incoming: boolean; metadata: boolean; sentences: boolean }
) {
	const nodes = layoutNodes
		.map((node) => ({
			id: String(node.id || node.label || ''),
			type: String(node.mostSpecificType || instance.atoms.get(String(node.id || node.label || ''))?.type || ''),
			x: finite(node.x, 0),
			y: finite(node.y, 0),
		}))
		.filter((node) => node.id)
		.sort((a, b) => a.y - b.y || a.x - b.x || a.id.localeCompare(b.id));

	const order = new Map(nodes.map((node, index) => [node.id, index]));
	const lines: string[] = [];
	if (options.metadata) {
		lines.push(`forge ${words(instance.command || 'instance')}`);
		if (instance.filename) {
			lines.push(`source ${instance.filename}`);
		}
		if (instance.version) {
			lines.push(`forge version ${instance.version}`);
		}
		lines.push(`order layout y, then layout x`);
		lines.push('');
	}

	if (options.sentences) {
		return renderSentenceText(instance, nodes, order, options);
	}

	for (const node of nodes) {
		lines.push(`${prettyAtom(node.id, node.type)}`);
		for (const fieldLine of outgoingLines(instance, node.id, order)) {
			lines.push(`  ${fieldLine}`);
		}
		if (options.incoming) {
			for (const fieldLine of incomingLines(instance, node.id, order)) {
				lines.push(`  ${fieldLine}`);
			}
		}
		lines.push('');
	}

	while (lines.at(-1) === '') {
		lines.pop();
	}
	return lines.join('\n') + '\n';
}

function renderSentenceText(
	instance: InstanceInfo,
	nodes: Array<{ id: string; type: string; x: number; y: number }>,
	order: Map<string, number>,
	options: { incoming: boolean; metadata: boolean }
) {
	const lines: string[] = [];
	if (options.metadata) {
		lines.push(`forge ${words(instance.command || 'instance')}`);
		if (instance.filename) {
			lines.push(`source ${instance.filename}`);
		}
		if (instance.version) {
			lines.push(`forge version ${instance.version}`);
		}
		lines.push(`order layout y, then layout x`);
		lines.push('');
	}

	for (const node of nodes) {
		const phrases = outgoingLines(instance, node.id, order);
		if (options.incoming) {
			phrases.push(...incomingLines(instance, node.id, order));
		}
		if (!phrases.length) {
			continue;
		}
		lines.push(`${capitalize(prettyAtom(node.id, node.type))} ${joinEnglish(phrases)}.`);
	}
	return lines.join('\n') + '\n';
}

function outgoingLines(instance: InstanceInfo, atomId: string, order: Map<string, number>) {
	const lines: string[] = [];
	for (const field of instance.fields) {
		const matching = field.tuples.filter((tuple) => tuple[0] === atomId);
		if (!matching.length) {
			continue;
		}
		const values = matching
			.map((tuple) => tuple.slice(1))
			.sort((a, b) => tupleOrder(a, b, order))
			.map((tuple) => prettyTuple(tuple, instance, words(field.name)));
		lines.push(`${words(field.name)} ${values.join(', ')}`);
	}
	return lines;
}

function incomingLines(instance: InstanceInfo, atomId: string, order: Map<string, number>) {
	const lines: string[] = [];
	for (const field of instance.fields) {
		const matching = field.tuples.filter((tuple) => tuple.slice(1).includes(atomId));
		if (!matching.length) {
			continue;
		}
		const values = matching
			.sort((a, b) => tupleOrder(a, b, order))
			.map((tuple) => `${prettyAtom(tuple[0], instance.atoms.get(tuple[0])?.type || '')} via ${words(field.name)}`);
		lines.push(`referenced by ${values.join(', ')}`);
	}
	return lines;
}

function tupleOrder(a: string[], b: string[], order: Map<string, number>) {
	const max = Math.max(a.length, b.length);
	for (let i = 0; i < max; i += 1) {
		const left = order.get(a[i]) ?? Number.MAX_SAFE_INTEGER;
		const right = order.get(b[i]) ?? Number.MAX_SAFE_INTEGER;
		if (left !== right) {
			return left - right;
		}
		const nameCompare = String(a[i] || '').localeCompare(String(b[i] || ''));
		if (nameCompare) {
			return nameCompare;
		}
	}
	return 0;
}

function finite(value: unknown, fallback: number) {
	return typeof value === 'number' && Number.isFinite(value) ? value : fallback;
}

function prettyTuple(tuple: string[], instance: InstanceInfo, fieldWords = '') {
	if (!tuple.length) {
		return 'true';
	}
	return tuple
		.map((atom, index) => prettyAtom(atom, instance.atoms.get(atom)?.type || '', index === 0 ? fieldWords : ''))
		.join(' to ');
}

function prettyAtom(atom: string, type: string, contextWords = '') {
	const parsed = splitNumberSuffix(atom);
	const typeWords = words(type || parsed.base);
	const atomBaseWords = words(parsed.base);
	if (parsed.number && contextAlreadyNamesType(contextWords, typeWords)) {
		return parsed.number;
	}
	if (!parsed.number) {
		return atomBaseWords;
	}
	if (sameWords(typeWords, atomBaseWords)) {
		return `${typeWords} ${parsed.number}`;
	}
	return `${atomBaseWords} ${parsed.number}`;
}

function splitNumberSuffix(value: string) {
	const match = /^(.+?)(\d+)$/.exec(String(value || '').trim());
	return {
		base: match ? match[1] : value,
		number: match ? match[2] : '',
	};
}

function words(value: string) {
	return String(value || '')
		.replace(/([a-z0-9])([A-Z])/g, '$1 $2')
		.replace(/([A-Z]+)([A-Z][a-z])/g, '$1 $2')
		.replace(/[_-]+/g, ' ')
		.replace(/\s+/g, ' ')
		.trim()
		.toLowerCase();
}

function sameWords(left: string, right: string) {
	return left.replace(/\s+/g, '') === right.replace(/\s+/g, '');
}

function contextAlreadyNamesType(contextWords: string, typeWords: string) {
	if (!contextWords || !typeWords) {
		return false;
	}
	const context = contextWords.split(/\s+/);
	const type = typeWords.split(/\s+/);
	if (type.length > context.length) {
		return false;
	}
	const tail = context.slice(context.length - type.length);
	return tail.join(' ') === type.join(' ');
}

function joinEnglish(items: string[]) {
	if (items.length === 0) {
		return '';
	}
	if (items.length === 1) {
		return items[0];
	}
	if (items.length === 2) {
		return `${items[0]} and ${items[1]}`;
	}
	return `${items.slice(0, -1).join(', ')}, and ${items.at(-1)}`;
}

function capitalize(value: string) {
	return value ? value[0].toUpperCase() + value.slice(1) : value;
}
