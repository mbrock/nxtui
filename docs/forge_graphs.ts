#!/usr/bin/env bun

import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { mkdir, readdir, readFile, rm, writeFile } from "node:fs/promises";
import path from "node:path";

type Graph = {
	frg: string;
	run: string;
	title?: string;
	height?: string;
	cnd?: string;
	_source: string;
};

const root = path.resolve(import.meta.dir, "..");
const docs = path.join(root, "docs");
const generated = path.join(docs, "generated", "forge-graphs");
const generatedManifest = path.join(generated, "forge-doc-graphs.manifest.ts");
const browserEntry = path.join(docs, "forge-doc-graphs.ts");
const browserBundle = path.join(docs, "forge-doc-graphs.js");

const scanRoots = [
	path.join(root, "src-ng"),
	path.join(root, "src", "nxt"),
	path.join(root, "src", "nxtai"),
	path.join(root, "src", "nxtio"),
	path.join(root, "test"),
];

function rel(file: string) {
	return path.relative(root, file);
}

function slugify(value: string) {
	return value
		.replaceAll("\\", "/")
		.replace(/^\.\//, "")
		.replace(/[^A-Za-z0-9_.-]+/g, "-")
		.replace(/^-+|-+$/g, "") || "graph";
}

function graphXmlName(frg: string, run: string) {
	return `forge-graph-${slugify(frg)}--${slugify(run)}.xml`;
}

function parseAttributes(attrs: string) {
	const values: Record<string, string> = {};
	for (const match of attrs.matchAll(/([A-Za-z_:][-A-Za-z0-9_:.]*)\s*=\s*("([^"]*)"|'([^']*)')/g)) {
		values[match[1].toLowerCase()] = match[3] ?? match[4] ?? "";
	}
	return values;
}

async function* walk(dir: string): AsyncGenerator<string> {
	if (!existsSync(dir)) {
		return;
	}

	for (const entry of await readdir(dir, { withFileTypes: true })) {
		const entryPath = path.join(dir, entry.name);
		if (entry.isDirectory()) {
			yield* walk(entryPath);
		} else {
			yield entryPath;
		}
	}
}

async function scanGraphs() {
	const graphs: Graph[] = [];
	for (const scanRoot of scanRoots) {
		for await (const file of walk(scanRoot)) {
			if (![".h", ".hpp", ".hh", ".hxx", ".md"].includes(path.extname(file))) {
				continue;
			}

			const text = await readFile(file, "utf8");
			const source = rel(file);
			for (const match of text.matchAll(/<forge-doc-graph\b([^>]*)>/gi)) {
				const graph = parseAttributes(match[1]) as Partial<Graph>;
				graph._source = source;
				graphs.push(graph as Graph);
			}
			for (const match of text.matchAll(/[@\\]forgegraph\{([^,{}]+),([^,{}]+),([^{}]+)\}/g)) {
				graphs.push({
					frg: match[1].trim(),
					run: match[2].trim(),
					title: match[3].trim(),
					_source: source,
				});
			}
		}
	}
	return graphs;
}

function resolveFrg(frg: string, source: string) {
	if (path.isAbsolute(frg)) {
		return frg;
	}

	const rootRelative = path.join(root, frg);
	if (existsSync(rootRelative)) {
		return rootRelative;
	}

	return path.join(root, path.dirname(source), frg);
}

async function exportGraph(graph: Graph) {
	if (!graph.frg || !graph.run) {
		throw new Error(`forge-doc-graph in ${graph._source} needs frg and run attributes`);
	}

	const frgPath = path.resolve(resolveFrg(graph.frg, graph._source));
	if (!existsSync(frgPath)) {
		throw new Error(`Forge spec does not exist: ${graph.frg} (from ${graph._source})`);
	}

	const output = path.join(generated, graphXmlName(graph.frg, graph.run));
	await mkdir(path.dirname(output), { recursive: true });
	const args = path.extname(frgPath) === ".rkt"
		? [frgPath, "--direct-xml", output, "--run", graph.run]
		: [
			frgPath,
			"-O",
			"run_sterling",
			"off",
			"-O",
			"export_run",
			graph.run,
			"-O",
			"export_xml",
			output,
		];
	const result = spawnSync("racket", args, {
		cwd: root,
		encoding: "utf8",
	});

	if (result.status !== 0) {
		if (result.stdout) {
			process.stdout.write(result.stdout);
		}
		if (result.stderr) {
			process.stderr.write(result.stderr);
		}
		process.exit(result.status ?? 1);
	}

	return output;
}

async function writeManifest(graphs: Graph[]) {
	const uniqueGraphs = [...new Map(graphs.map((graph) => [`${graph.frg}\0${graph.run}`, graph])).values()];
	const imports = uniqueGraphs.map((graph, index) => {
		const xmlPath = `./${graphXmlName(graph.frg, graph.run)}`;
		return `import graph${index}Xml from ${JSON.stringify(xmlPath)} with { type: "text" };`;
	});
	const records = uniqueGraphs.map((graph, index) => `\t${JSON.stringify(`${graph.frg}\0${graph.run}`)}: graph${index}Xml,`);

	await writeFile(generatedManifest, `${imports.join("\n")}
import "@forge-fm/forge-graph";

export const graphXml = {
${records.join("\n")}
};
`);
}

async function prepare() {
	await mkdir(generated, { recursive: true });
	const graphs = await scanGraphs();
	const seen = new Set<string>();
	const uniqueGraphs: Graph[] = [];
	for (const graph of graphs) {
		const key = `${graph.frg}\0${graph.run}`;
		if (seen.has(key)) {
			continue;
		}
		seen.add(key);
		uniqueGraphs.push(graph);
		const output = await exportGraph(graph);
		console.log(`generated ${rel(output)}`);
	}
	await writeManifest(uniqueGraphs);
	console.log(`generated ${rel(generatedManifest)}`);
}

async function install() {
	const result = spawnSync("bun", [
		"build",
		browserEntry,
		"--target=browser",
		"--outfile",
		browserBundle,
		"--sourcemap=none",
	], {
		cwd: root,
		encoding: "utf8",
	});

	if (result.stdout) {
		process.stdout.write(result.stdout);
	}
	if (result.stderr) {
		process.stderr.write(result.stderr);
	}
	if (result.status !== 0) {
		process.exit(result.status ?? 1);
	}
}

const command = process.argv[2];
if (!["prepare", "install", "all"].includes(command ?? "")) {
	console.error("usage: bun docs/forge_graphs.ts prepare|install|all");
	process.exit(2);
}

if (command === "prepare" || command === "all") {
	await prepare();
}
if (command === "install" || command === "all") {
	await install();
}
