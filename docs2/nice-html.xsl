<?xml version="1.0" encoding="UTF-8"?>
<xsl:stylesheet version="1.0"
                xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="html" encoding="UTF-8" indent="yes"/>

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
            border-top: 1px solid var(--line);
            margin: 3rem 0 3.5rem;
            padding-top: 2rem;
          }
          .model-doc h2 {
            margin-top: 0;
          }
          .model-doc h3 {
            font-size: 1rem;
            margin: 1.6rem 0 0.55rem;
          }
          .model-table {
            border-collapse: collapse;
            font-size: 0.9rem;
            line-height: 1.35;
            margin: 0.75rem 0 1.2rem;
            width: 100%;
          }
          .model-table th {
            color: var(--muted);
            font-weight: 650;
            text-align: left;
          }
          .model-table th,
          .model-table td {
            border-bottom: 1px solid var(--line);
            padding: 0.45rem 0.55rem 0.45rem 0;
            vertical-align: top;
          }
          .model-table code {
            background: transparent;
            padding: 0;
          }
          .signature-list,
          .run-list {
            display: grid;
            gap: 1.2rem;
          }
          .signature,
          .run {
            border-top: 1px solid var(--line);
            padding-top: 0.8rem;
          }
          .signature h3,
          .run h3 {
            margin-top: 0;
          }
          .inline-list {
            display: flex;
            flex-wrap: wrap;
            gap: 0.35rem 0.7rem;
            margin: 0.5rem 0 1rem;
          }
          .inline-list code {
            white-space: nowrap;
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
          <xsl:apply-templates select="ontology-section"/>
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

  <xsl:template match="ontology-section">
    <section class="model-doc ontology">
      <h2><xsl:value-of select="title"/></h2>
      <p>
        <code><xsl:value-of select="ontology-prefix"/></code>
        <xsl:text> </xsl:text>
        <span class="source"><xsl:value-of select="ontology-base"/></span>
      </p>
      <h3>Classes</h3>
      <table class="model-table">
        <thead>
          <tr><th>Name</th><th>Parent</th><th>IRI</th></tr>
        </thead>
        <tbody>
          <xsl:for-each select="classes/class">
            <tr>
              <td><code><xsl:value-of select="@name"/></code></td>
              <td><code><xsl:value-of select="@parent"/></code></td>
              <td><xsl:value-of select="@iri"/></td>
            </tr>
          </xsl:for-each>
        </tbody>
      </table>
      <h3>Properties</h3>
      <table class="model-table">
        <thead>
          <tr><th>Name</th><th>Domain</th><th>Range</th><th>Forge</th></tr>
        </thead>
        <tbody>
          <xsl:for-each select="properties/property">
            <tr>
              <td><code><xsl:value-of select="@name"/></code></td>
              <td><code><xsl:value-of select="@domain"/></code></td>
              <td><code><xsl:value-of select="@range"/></code></td>
              <td><code><xsl:value-of select="@forge-name"/></code></td>
            </tr>
          </xsl:for-each>
        </tbody>
      </table>
    </section>
  </xsl:template>

  <xsl:template match="model-section">
    <section class="model-doc model">
      <h2><xsl:value-of select="title"/></h2>
      <h3>Signatures</h3>
      <div class="signature-list">
        <xsl:for-each select="signatures/signature">
          <section class="signature">
            <h3><code><xsl:value-of select="@name"/></code></h3>
            <table class="model-table">
              <thead>
                <tr><th>Field</th><th>Multiplicity</th><th>Range</th><th>Temporal</th><th>Forge</th></tr>
              </thead>
              <tbody>
                <xsl:for-each select="field">
                  <tr>
                    <td><code><xsl:value-of select="@name"/></code></td>
                    <td><code><xsl:value-of select="@multiplicity"/></code></td>
                    <td><code><xsl:value-of select="@range"/></code></td>
                    <td><xsl:if test="@variable='true'">var</xsl:if></td>
                    <td><code><xsl:value-of select="@forge-name"/></code></td>
                  </tr>
                </xsl:for-each>
              </tbody>
            </table>
          </section>
        </xsl:for-each>
      </div>
      <h3>Predicates</h3>
      <div class="inline-list">
        <xsl:for-each select="predicates/predicate">
          <code><xsl:value-of select="@name"/></code>
        </xsl:for-each>
      </div>
      <h3>Checks</h3>
      <div class="inline-list">
        <xsl:for-each select="checks/check">
          <code><xsl:value-of select="@name"/></code>
        </xsl:for-each>
      </div>
      <h3>Runs</h3>
      <div class="run-list">
        <xsl:for-each select="runs/run">
          <section class="run">
            <h3><code><xsl:value-of select="@name"/></code></h3>
            <p class="source">scope <code><xsl:value-of select="@scope"/></code></p>
            <xsl:apply-templates select="forge-graph"/>
          </section>
        </xsl:for-each>
      </div>
    </section>
  </xsl:template>
</xsl:stylesheet>
