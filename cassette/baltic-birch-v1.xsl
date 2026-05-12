<?xml version="1.0" encoding="UTF-8"?>
<!--
  Baltic Birch v1 — a cassette stylesheet.

  Transforms an agent trace XML document into xtc markup. Each XSL
  template defines one cassette variant; the result is layout-only
  XML that xtc renders to ANSI.

  The full pipeline:
      trace.xml  ->  this stylesheet  ->  xtc xml  ->  ansi
-->
<xsl:stylesheet version="1.0"
  xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="xml" omit-xml-declaration="yes" indent="no"/>
  <xsl:strip-space elements="*"/>

  <!-- ================================================================
       Root: a trace is a stack of turns; a turn renders standalone too.
       ================================================================ -->
  <xsl:template match="trace">
    <root class="flex flex-col bg-stone-900 text-stone-200">
      <xsl:apply-templates select="turn"/>
    </root>
  </xsl:template>

  <xsl:template match="turn[parent::trace]">
    <div class="flex flex-col">
      <xsl:apply-templates/>
    </div>
  </xsl:template>

  <xsl:template match="turn">
    <root class="flex flex-col bg-stone-900 text-stone-200">
      <xsl:apply-templates/>
    </root>
  </xsl:template>

  <!-- ================================================================
       Thought cassette: cyan prose with a hair of left padding.
       ================================================================ -->
  <xsl:template match="thought">
    <div class="text-cyan-400 px-1 pt-1 pb-1">
      <xsl:value-of select="normalize-space(.)"/>
    </div>
  </xsl:template>

  <!-- ================================================================
       Call cassette: header row + result window + affordances.
       Dispatches by @status to pick the right spine variant.
       ================================================================ -->
  <xsl:template match="call">
    <div class="flex flex-col">
      <xsl:apply-templates select="." mode="header"/>
      <xsl:apply-templates select="result"/>
      <xsl:apply-templates select="affordances"/>
    </div>
  </xsl:template>

  <!-- Header row: status spine, tool name, args, timing, count chip. -->
  <xsl:template match="call" mode="header">
    <div class="flex flex-row items-center px-1 bg-stone-800">
      <xsl:apply-templates select="." mode="spine"/>
      <box>
        <xsl:attribute name="class">
          <xsl:text>px-2 </xsl:text>
          <xsl:apply-templates select="." mode="tool-color"/>
        </xsl:attribute>
        <xsl:value-of select="@name"/>
      </box>
      <box class="text-stone-600 flex-1 overflow-hidden">
        <xsl:value-of select="args"/>
      </box>
      <xsl:if test="@elapsed_ms">
        <box class="text-stone-600 px-2"><xsl:value-of select="@elapsed_ms"/>ms</box>
      </xsl:if>
      <xsl:apply-templates select="result" mode="meta"/>
    </div>
  </xsl:template>

  <!-- Spine: the two-cell colored chip at the left edge. -->
  <xsl:template match="call[@status='ok']" mode="spine">
    <box>
      <xsl:attribute name="class">
        <xsl:text>text-stone-900 px-1 </xsl:text>
        <xsl:apply-templates select="." mode="tool-bg"/>
      </xsl:attribute>
      <xsl:text>✓</xsl:text>
    </box>
  </xsl:template>

  <xsl:template match="call[@status='error']" mode="spine">
    <box class="text-stone-900 bg-rose-400 px-1">!</box>
  </xsl:template>

  <xsl:template match="call[@status='pending_approval']" mode="spine">
    <box class="text-stone-900 bg-amber-300 px-1">?</box>
  </xsl:template>

  <xsl:template match="call[@status='running']" mode="spine">
    <box class="text-amber-300 px-1">⠋</box>
  </xsl:template>

  <xsl:template match="call[@status='denied']" mode="spine">
    <box class="text-stone-900 bg-stone-500 px-1">N</box>
  </xsl:template>

  <!-- ================================================================
       Tool kind identity: foreground and background variants of the
       per-tool color. One pair of templates per kind. New tool? Add a
       pair here and you're done.
       ================================================================ -->
  <xsl:template match="call[@name='rg_search']" mode="tool-color">text-amber-500</xsl:template>
  <xsl:template match="call[@name='rg_search']" mode="tool-bg">bg-amber-400</xsl:template>

  <xsl:template match="call[@name='read_file']" mode="tool-color">text-emerald-500</xsl:template>
  <xsl:template match="call[@name='read_file']" mode="tool-bg">bg-emerald-400</xsl:template>

  <xsl:template match="call[@name='bash']" mode="tool-color">text-orange-500</xsl:template>
  <xsl:template match="call[@name='bash']" mode="tool-bg">bg-orange-400</xsl:template>

  <xsl:template match="call[starts-with(@name, 'nxt_')]" mode="tool-color">text-lime-500</xsl:template>
  <xsl:template match="call[starts-with(@name, 'nxt_')]" mode="tool-bg">bg-lime-400</xsl:template>

  <xsl:template match="call[@name='web_fetch']" mode="tool-color">text-violet-500</xsl:template>
  <xsl:template match="call[@name='web_fetch']" mode="tool-bg">bg-violet-400</xsl:template>

  <!-- Generic fallback so unrecognized tools still get a coherent color. -->
  <xsl:template match="call" mode="tool-color">text-teal-500</xsl:template>
  <xsl:template match="call" mode="tool-bg">bg-teal-400</xsl:template>

  <!-- ================================================================
       Meta chip (right edge of header): kind-specific count summary.
       ================================================================ -->
  <xsl:template match="result[@kind='matches']" mode="meta">
    <box class="text-stone-400">⨉<xsl:value-of select="@total_lines"/><xsl:text> </xsl:text><xsl:value-of select="round(@bytes div 1024)"/>K</box>
  </xsl:template>

  <xsl:template match="result[@kind='document']" mode="meta">
    <box class="text-stone-400"><xsl:value-of select="@lines"/>L <xsl:value-of select="round(@bytes div 1024)"/>K</box>
  </xsl:template>

  <xsl:template match="result[@kind='process']" mode="meta">
    <box class="text-stone-400">exit <xsl:value-of select="@exit"/></box>
  </xsl:template>

  <xsl:template match="result[@kind='fact']" mode="meta"/>
  <xsl:template match="result[@kind='error']" mode="meta"/>

  <!-- ================================================================
       The cassette window: kind-specific peek into the outcome.
       ================================================================ -->

  <!-- matches: bullet list of file:line hits + "...N more" footnote. -->
  <xsl:template match="result[@kind='matches']">
    <div class="flex flex-col px-2 text-slate-400">
      <xsl:apply-templates select="line"/>
      <xsl:if test="@total_lines &gt; count(line)">
        <box class="text-stone-600">...<xsl:value-of select="@total_lines - count(line)"/> more lines.</box>
      </xsl:if>
    </div>
  </xsl:template>

  <!-- document: head-of-file preview, dim slate. -->
  <xsl:template match="result[@kind='document']">
    <div class="flex flex-col px-2 text-slate-400">
      <xsl:apply-templates select="line"/>
      <xsl:if test="@lines &gt; count(line)">
        <box class="text-stone-600">...<xsl:value-of select="@lines - count(line)"/> more lines.</box>
      </xsl:if>
    </div>
  </xsl:template>

  <!-- process: stdout+stderr interleaved; channel tinted distinctly. -->
  <xsl:template match="result[@kind='process']">
    <div class="flex flex-col px-2">
      <xsl:apply-templates select="stream"/>
    </div>
  </xsl:template>

  <xsl:template match="stream[@channel='stdout']">
    <xsl:apply-templates select="line" mode="stdout"/>
  </xsl:template>

  <xsl:template match="stream[@channel='stderr']">
    <xsl:apply-templates select="line" mode="stderr"/>
  </xsl:template>

  <xsl:template match="line" mode="stdout">
    <box class="text-slate-400"><xsl:value-of select="."/></box>
  </xsl:template>

  <xsl:template match="line" mode="stderr">
    <box class="text-rose-300"><xsl:value-of select="."/></box>
  </xsl:template>

  <!-- fact: no window, the meta is already enough — but render the
       fact's fields as a single dim row so the consumer sees them. -->
  <xsl:template match="result[@kind='fact']">
    <div class="flex flex-row px-2 text-slate-400">
      <xsl:for-each select="field">
        <box class="text-stone-600 pr-1"><xsl:value-of select="@name"/>:</box>
        <box class="pr-2"><xsl:value-of select="."/></box>
      </xsl:for-each>
    </div>
  </xsl:template>

  <!-- error: rose message, no chrome. -->
  <xsl:template match="result[@kind='error']">
    <div class="flex flex-row px-2 text-rose-300">
      <box><xsl:value-of select="message"/></box>
    </div>
  </xsl:template>

  <!-- Generic line passthrough. -->
  <xsl:template match="line">
    <box><xsl:value-of select="."/></box>
  </xsl:template>

  <!-- ================================================================
       Affordances strip: bracketed key hints in a warmer accent so
       they advertise "you can do this." Same surface the model parses
       as its action menu.
       ================================================================ -->
  <xsl:template match="affordances">
    <div class="flex flex-row px-2 pt-1">
      <xsl:for-each select="affordance">
        <box class="text-amber-200 bg-stone-800 px-1">
          <xsl:value-of select="@key"/>
        </box>
        <box class="text-stone-400 pl-1 pr-2">
          <xsl:value-of select="."/>
        </box>
      </xsl:for-each>
    </div>
  </xsl:template>

</xsl:stylesheet>
