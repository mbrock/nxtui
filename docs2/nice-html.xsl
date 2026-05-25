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
          .term-cloud,
          .relation-list,
          .signature-list,
          .run-list,
          .inline-list {
            display: grid;
            gap: 0.65rem;
          }
          .term-cloud {
            grid-template-columns: repeat(auto-fit, minmax(8rem, 1fr));
          }
          .term,
          .relation,
          .signature,
          .run {
            border-top: 1px solid var(--line);
            padding-top: 0.75rem;
          }
          .term strong,
          .relation strong,
          .signature h3,
          .run h3 {
            margin-top: 0;
          }
          .term p,
          .relation p,
          .signature p,
          .run p {
            margin: 0.25rem 0 0;
          }
          .phrase-list {
            list-style: none;
            margin: 0.45rem 0 0;
            padding: 0;
          }
          .phrase-list li {
            margin: 0.2rem 0;
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
          .predicate-list {
            display: grid;
            gap: 1rem;
          }
          .predicate {
            border-top: 1px solid var(--line);
            padding-top: 0.75rem;
          }
          .predicate h3 {
            margin-top: 0;
          }
          .dexp {
            align-items: center;
            display: flex;
            flex-wrap: wrap;
            gap: 0.22rem 0.42rem;
          }
          .dexp.list {
            border: 0 solid color-mix(in srgb, var(--accent), transparent 45%);
            border-radius: 8px;
            border-width: 0 1.25px;
            margin: 0.12rem 0;
            padding: 0.12rem 0.45rem;
          }
          .dexp.list .dexp.list {
            border-color: color-mix(in srgb, var(--muted), transparent 52%);
          }
          .dexp.symbol {
            color: var(--ink);
            font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
            font-size: 0.9rem;
          }
          .dexp.number {
            color: var(--accent);
            font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
            font-size: 0.9rem;
          }
          .dexp.list > .dexp.symbol:first-child {
            color: var(--accent);
            font-weight: 700;
          }
          .dexp.list[data-callee='block'],
          .dexp.list[data-callee='all'],
          .dexp.list[data-callee='some'],
          .dexp.list[data-callee='lone'],
          .dexp.list[data-callee='always'],
          .dexp.list[data-callee='next-state'] {
            align-items: flex-start;
            flex-direction: column;
          }
          .dexp.list[data-callee='block'] > .dexp.symbol:first-child,
          .dexp.list[data-callee='all'] > .dexp.symbol:first-child,
          .dexp.list[data-callee='some'] > .dexp.symbol:first-child,
          .dexp.list[data-callee='lone'] > .dexp.symbol:first-child,
          .dexp.list[data-callee='always'] > .dexp.symbol:first-child,
          .dexp.list[data-callee='next-state'] > .dexp.symbol:first-child {
            width: 100%;
          }
          .dexp.list[data-callee='bindings'] {
            align-items: flex-start;
            border-color: color-mix(in srgb, var(--line), transparent 20%);
            flex-direction: column;
          }
          .dexp.list[data-callee='binding'] {
            border-color: transparent;
            padding-left: 0;
          }
          .dexp.list[data-callee='=>'],
          .dexp.list[data-callee='=='],
          .dexp.list[data-callee='in'] {
            column-gap: 0.5rem;
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
        The runtime domain uses the <code><xsl:value-of select="ontology-prefix"/></code>
        vocabulary at <span class="source"><xsl:value-of select="ontology-base"/></span>.
      </p>
      <h3>Classes</h3>
      <div class="term-cloud">
        <xsl:for-each select="classes/class">
          <div class="term">
            <strong><code><xsl:value-of select="@name"/></code></strong>
            <xsl:if test="@parent != ''">
              <p class="source">a kind of <code><xsl:value-of select="@parent"/></code></p>
            </xsl:if>
          </div>
        </xsl:for-each>
      </div>
      <h3>Relations</h3>
      <div class="relation-list">
        <xsl:for-each select="properties/property">
          <div class="relation">
            <strong><xsl:value-of select="translate(@name, '-', ' ')"/></strong>
            <p>
              <code><xsl:value-of select="@domain"/></code>
              <xsl:text> </xsl:text>
              <xsl:value-of select="translate(@name, '-', ' ')"/>
              <xsl:text> </xsl:text>
              <code><xsl:value-of select="@range"/></code>
            </p>
          </div>
        </xsl:for-each>
      </div>
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
            <xsl:choose>
              <xsl:when test="field">
                <ul class="phrase-list">
                  <xsl:for-each select="field">
                    <li>
                      <xsl:value-of select="translate(@name, '-', ' ')"/>
                      <xsl:text> </xsl:text>
                      <xsl:choose>
                        <xsl:when test="@multiplicity='one'">exactly one </xsl:when>
                        <xsl:when test="@multiplicity='lone'">at most one </xsl:when>
                        <xsl:otherwise>any number of </xsl:otherwise>
                      </xsl:choose>
                      <code><xsl:value-of select="@range"/></code>
                      <xsl:if test="@variable='true'">
                        <xsl:text>, changing over time</xsl:text>
                      </xsl:if>
                    </li>
                  </xsl:for-each>
                </ul>
              </xsl:when>
              <xsl:otherwise>
                <p class="source">No fields.</p>
              </xsl:otherwise>
            </xsl:choose>
          </section>
        </xsl:for-each>
      </div>
      <h3>Predicates</h3>
      <div class="predicate-list">
        <xsl:for-each select="predicates/predicate">
          <section class="predicate">
            <h3><xsl:value-of select="translate(@name, '-', ' ')"/></h3>
            <xsl:apply-templates select="dexp-list"/>
          </section>
        </xsl:for-each>
      </div>
      <h3>Witnesses</h3>
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
    <span class="dexp value symbol"><xsl:value-of select="translate(@name, '-', ' ')"/></span>
  </xsl:template>

  <xsl:template match="dexp-number">
    <span class="dexp value number"><xsl:value-of select="@value"/></span>
  </xsl:template>
</xsl:stylesheet>
