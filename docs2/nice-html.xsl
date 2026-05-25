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
            width: min(1120px, calc(100% - 2rem));
            margin: 0 auto;
            padding: 4.5rem 0 5rem;
          }
          header {
            max-width: 760px;
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
            max-width: 760px;
            font-size: 1.08rem;
          }
          forge-doc-graph {
            aspect-ratio: 21 / 10;
            background: transparent;
            display: block;
            margin: 2.25rem 0 3rem;
            max-height: 620px;
            min-height: 320px;
            position: relative;
            width: min(1280px, calc(100vw - 2rem));
            margin-left: calc((100% - min(1280px, calc(100vw - 2rem))) / 2);
          }
          forge-doc-graph forge-graph {
            aspect-ratio: inherit;
            background: transparent;
            display: block;
            height: 100%;
            min-height: inherit;
            max-height: inherit;
            width: 100%;
          }
          section {
            max-width: 760px;
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
</xsl:stylesheet>
