import { graphXml } from "./generated/forge-graphs/forge-doc-graphs.manifest";

function ready() {
	if (document.readyState === "loading") {
		return new Promise((resolve) => {
			document.addEventListener("DOMContentLoaded", resolve, { once: true });
		});
	}
	return Promise.resolve();
}

function placeholderKey(placeholder: Element) {
	const frg = placeholder.getAttribute("frg");
	const run = placeholder.getAttribute("run");
	if (!frg || !run) {
		return undefined;
	}
	return { frg, run, key: `${frg}\0${run}` };
}

function appendInlineXml(graph: Element, xml: string) {
	const xmlScript = document.createElement("script");
	xmlScript.type = "application/xml";
	xmlScript.textContent = xml;
	graph.append(xmlScript);
}

function upgradeGraphPlaceholder(placeholder: Element) {
	const graphRef = placeholderKey(placeholder);
	if (!graphRef) {
		placeholder.textContent = "Forge graph is missing frg or run.";
		return;
	}

	const xml = graphXml[graphRef.key];
	if (!xml) {
		placeholder.textContent = `Forge graph was not generated for ${graphRef.frg}: ${graphRef.run}.`;
		return;
	}

	const graph = document.createElement("forge-graph");
	graph.setAttribute("title", placeholder.getAttribute("title") || `${graphRef.frg}: ${graphRef.run}`);
	graph.setAttribute("height", placeholder.getAttribute("height") || "560");
	appendInlineXml(graph, xml);

	const cnd = placeholder.getAttribute("cnd");
	if (cnd) {
		graph.setAttribute("cnd", cnd);
	}

	placeholder.replaceChildren(graph);
}

function upgradeAllForgeDocGraphs() {
	for (const placeholder of document.querySelectorAll("forge-doc-graph")) {
		upgradeGraphPlaceholder(placeholder);
	}
}

ready().then(upgradeAllForgeDocGraphs);
