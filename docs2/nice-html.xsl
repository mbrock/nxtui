<?xml version="1.0" encoding="UTF-8"?>
<xsl:stylesheet version="1.0"
                xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="html" encoding="UTF-8" indent="yes"/>
  <xsl:strip-space elements="dexp-list dexp-symbol dexp-number predicate predicates"/>

  <xsl:template match="/doc-page">
    <html>
      <head>
        <meta charset="utf-8"/>
        <meta name="viewport" content="width=device-width, initial-scale=1"/>
        <title><xsl:value-of select="title"/></title>
        <script type="module" src="forge-doc-graphs.js"></script>
        <style>
          :root {
            color-scheme: light;
            --ink: #172026;
            --muted: #60707a;
            --paper: #fbfaf7;
            --line: #d9d4ca;
            --soft: #f0eee8;
            --accent: #2c6f73;
          }
          * { box-sizing: border-box; }
          body {
            font: 17px/1.62 ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
            margin: 0;
            color: var(--ink);
            background: var(--paper);
          }
          main {
            width: min(760px, calc(100% - 2rem));
            margin: 0 auto;
            padding: 4.5rem 0 5rem;
          }
          header {
            margin-bottom: 2rem;
          }
          h1 {
            font-size: clamp(2.4rem, 7vw, 4.8rem);
            line-height: 0.96;
            margin: 0 0 1rem;
            letter-spacing: 0;
          }
          h2 {
            font-size: 1.35rem;
            margin: 2.75rem 0 0.75rem;
            padding-top: 0.25rem;
          }
          p { margin: 0.85rem 0; }
          code {
            font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
            font-size: 0.92em;
            background: var(--soft);
            padding: 0.08rem 0.28rem;
            border-radius: 4px;
          }
          .xref {
            color: var(--accent);
            text-decoration: underline;
            text-decoration-thickness: 0.08em;
            text-underline-offset: 0.18em;
          }
          .source {
            color: var(--muted);
            margin: 0;
          }
          .intro {
            font-size: 1.08rem;
          }
          forge-doc-graph {
            background: transparent;
            display: block;
            margin: 2rem 0 2.75rem;
            width: 100%;
          }
          forge-doc-graph forge-graph {
            background: transparent;
            display: block;
            width: 100%;
          }
          .model-doc {
            margin: 2.25rem 0 3.5rem;
          }
          .run-list {
            display: grid;
            gap: 1.2rem;
            margin-top: 2rem;
          }
          .run {
            border-top: 1px solid var(--line);
            padding-top: 0.75rem;
          }
          .run h3 {
            margin-top: 0;
          }
          .run p {
            margin: 0.25rem 0 0;
          }
          .dexp {
            align-items: center;
            display: inline-flex;
            flex-wrap: wrap;
            gap: 0.08rem 0.28rem;
          }
          .dexp.list {
            border: 0 solid color-mix(in srgb, var(--accent), transparent 45%);
            border-radius: 6px;
            border-width: 0 1px;
            margin: 0.03rem 0;
            padding: 0.02rem 0.28rem;
          }
          .dexp.list .dexp.list {
            border-color: color-mix(in srgb, var(--muted), transparent 52%);
          }
          .dexp.symbol {
            color: var(--ink);
            font: inherit;
            font-size: 0.95rem;
          }
          .dexp.number {
            color: var(--accent);
            font: inherit;
            font-size: 0.95rem;
          }
          .dexp.string {
            color: var(--muted);
            font: inherit;
            font-size: 0.95rem;
          }
          .dexp.string:before { content: "“"; }
          .dexp.string:after { content: "”"; }
          .dexp.list > .dexp.symbol:first-child {
            color: var(--accent);
            font-weight: 650;
          }
          .dexp.list[data-callee='block'] {
            align-items: flex-start;
            border-right-width: 0;
            flex-direction: column;
          }
          .dexp.list[data-callee='runtime-model'],
          .dexp.list[data-callee='ontology'],
          .dexp.list[data-callee='classes'],
          .dexp.list[data-callee='relations'],
          .dexp.list[data-callee='signatures'],
          .dexp.list[data-callee='predicates'],
          .dexp.list[data-callee='checks'] {
            align-items: flex-start;
            border-right-width: 0;
            flex-direction: column;
          }
          .dexp.list[data-callee='all'],
          .dexp.list[data-callee='some'],
          .dexp.list[data-callee='lone'] {
            align-items: flex-start;
          }
          .dexp.list[data-callee='all'] > :nth-child(n+3),
          .dexp.list[data-callee='some'] > :nth-child(n+3),
          .dexp.list[data-callee='lone'] > :nth-child(n+3) {
            margin-left: 1rem;
          }
          .dexp.list[data-callee='=>'],
          .dexp.list[data-callee='=='],
          .dexp.list[data-callee='in'] {
            column-gap: 0.32rem;
          }
          ul {
            margin: 0.75rem 0 1.25rem;
            padding-left: 1.3rem;
          }
          li + li { margin-top: 0.25rem; }
        </style>
      </head>
      <body>
        <main>
          <header>
            <h1><xsl:value-of select="title"/></h1>
            <p class="source"><xsl:value-of select="source"/></p>
          </header>
          <div class="intro">
            <xsl:apply-templates select="paragraph"/>
          </div>
          <xsl:apply-templates select="forge-graph"/>
          <xsl:apply-templates select="model-section"/>
          <xsl:apply-templates select="section"/>
        </main>
      </body>
    </html>
  </xsl:template>

  <xsl:template match="section">
    <section id="{@id}">
      <h2><xsl:value-of select="title"/></h2>
      <xsl:apply-templates select="*[not(self::title)]"/>
    </section>
  </xsl:template>

  <xsl:template match="paragraph">
    <p><xsl:apply-templates/></p>
  </xsl:template>

  <xsl:template match="code">
    <code><xsl:apply-templates/></code>
  </xsl:template>

  <xsl:template match="xref">
    <span class="xref" data-refid="{@refid}"><xsl:apply-templates/></span>
  </xsl:template>

  <xsl:template match="list">
    <ul><xsl:apply-templates/></ul>
  </xsl:template>

  <xsl:template match="item">
    <li><xsl:apply-templates/></li>
  </xsl:template>

  <xsl:template match="item/paragraph">
    <xsl:apply-templates/>
  </xsl:template>

  <xsl:template match="block">
    <xsl:apply-templates/>
  </xsl:template>

  <xsl:template match="forge-graph">
    <forge-doc-graph frg="{@frg}" run="{@run}" title="{@title}"></forge-doc-graph>
  </xsl:template>

  <xsl:template match="model-section">
    <section class="model-doc model">
      <xsl:apply-templates select="dexp-list"/>
      <div class="run-list">
        <xsl:for-each select="runs/run">
          <section class="run">
            <h3><xsl:value-of select="forge-graph/@title"/></h3>
            <p class="source">scope <code><xsl:value-of select="@scope"/></code></p>
            <xsl:apply-templates select="forge-graph"/>
          </section>
        </xsl:for-each>
      </div>
    </section>
  </xsl:template>

  <xsl:template match="dexp-list">
    <div class="dexp value list" data-callee="{@callee}"><xsl:apply-templates/></div>
  </xsl:template>

  <xsl:template match="dexp-symbol">
    <span class="dexp value symbol"><xsl:value-of select="@display"/></span>
  </xsl:template>

  <xsl:template match="dexp-number">
    <span class="dexp value number"><xsl:value-of select="@value"/></span>
  </xsl:template>

  <xsl:template match="dexp-string">
    <span class="dexp value string"><xsl:value-of select="@value"/></span>
  </xsl:template>
</xsl:stylesheet>
