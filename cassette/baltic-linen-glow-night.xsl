<?xml version="1.0" encoding="UTF-8"?>
<!--
  Baltic Linen Glow Night — a cassette stylesheet.

  An evolution of Baltic Birch v1, tuned for:
    - cool Baltic-night backdrop (slate-950 rather than warm stone-900)
    - linen-cream body text that reads as candle-lit
    - glow-toned tool accents (-300 shades on dark = emissive feel)
    - terse single-word display names for the common tool kinds
      (the trace XML keeps the canonical @name; this stylesheet just
      chooses how to *show* it)

  The full pipeline:
      trace.xml  ->  this stylesheet  ->  xtc xml  ->  ansi
-->
<xsl:stylesheet version="1.0"
  xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="xml" omit-xml-declaration="yes" indent="no"/>
  <xsl:strip-space elements="*"/>

  <!-- Render mode:
         'full' (default) — full cassette: header + window + affordances
         'rack'           — only headers, doubled in height for breathing room
       Switch with: xsltproc - -stringparam mode rack ... -->
  <xsl:param name="mode" select="'full'"/>

  <!-- ================================================================
       Root: a trace is a stack of turns; a turn renders standalone too.
       ================================================================ -->
  <xsl:template match="trace">
    <root class="flex flex-col bg-slate-950 text-amber-50">
      <xsl:apply-templates select="turn"/>
    </root>
  </xsl:template>

  <xsl:template match="turn[parent::trace]">
    <div class="flex flex-col">
      <xsl:apply-templates/>
    </div>
  </xsl:template>

  <xsl:template match="turn">
    <root class="flex flex-col bg-slate-950 text-amber-50">
      <xsl:apply-templates/>
    </root>
  </xsl:template>

  <!-- ================================================================
       Thought cassette: glow-blue prose, slight luminance.
       ================================================================ -->
  <xsl:template match="thought">
    <div class="text-sky-300 px-1 pt-1 pb-1">
      <xsl:value-of select="normalize-space(.)"/>
    </div>
  </xsl:template>

  <!-- ================================================================
       Call cassette: dispatches by $mode.
         full mode → header row + result window + affordances
         rack mode → just a 2-row header, no content snippets
       ================================================================ -->
  <xsl:template match="call">
    <xsl:choose>
      <xsl:when test="$mode = 'rack'">
        <xsl:apply-templates select="." mode="rack"/>
      </xsl:when>
      <xsl:otherwise>
        <div class="flex flex-col">
          <xsl:apply-templates select="." mode="header"/>
          <xsl:apply-templates select="result"/>
          <xsl:apply-templates select="affordances"/>
        </div>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <!-- ================================================================
       Rack cassette: a 2-row header, no content window. The extra row
       holds the args generously (wrapped by xtc); the meta column on
       the right shows timing on top, outcome stats below — using the
       breathing room to *be neat* rather than to cram.
       ================================================================ -->
  <xsl:template match="call" mode="rack">
    <div class="flex flex-col bg-slate-900">
      <!-- Row 1: spine | tool name | timing -->
      <div class="flex flex-row items-center">
        <xsl:apply-templates select="." mode="rack-spine"/>
        <box class="flex-1 px-2">
          <xsl:attribute name="class">
            <xsl:text>flex-1 px-2 </xsl:text>
            <xsl:apply-templates select="." mode="tool-color"/>
          </xsl:attribute>
          <xsl:apply-templates select="." mode="display-name"/>
        </box>
        <box class="text-slate-500 px-2">
          <xsl:if test="@elapsed_ms">
            <xsl:value-of select="@elapsed_ms"/><xsl:text>ms</xsl:text>
          </xsl:if>
        </box>
      </div>
      <!-- Row 2: spine | args (single-line, ellipsized) | outcome stats -->
      <div class="flex flex-row items-center">
        <xsl:apply-templates select="." mode="rack-spine"/>
        <box class="text-slate-400 flex-1 px-2 overflow-x-hidden">
          <xsl:choose>
            <xsl:when test="string-length(args) &gt; 60">
              <xsl:value-of select="concat(substring(args, 1, 58), '…')"/>
            </xsl:when>
            <xsl:otherwise><xsl:value-of select="args"/></xsl:otherwise>
          </xsl:choose>
        </box>
        <box class="px-2">
          <xsl:apply-templates select="result" mode="rack-meta"/>
        </box>
      </div>
    </div>
  </xsl:template>

  <!-- Rack spine: 2-cell colored block. Explicit non-breaking-space
       content so xtc gives it actual cells; w-2 fixes the width.
       Status differentiation via background variant. -->
  <xsl:template match="call[@status='ok']" mode="rack-spine">
    <box>
      <xsl:attribute name="class">
        <xsl:text>w-2 </xsl:text>
        <xsl:apply-templates select="." mode="tool-bg"/>
      </xsl:attribute>
      <xsl:text>&#160;&#160;</xsl:text>
    </box>
  </xsl:template>

  <xsl:template match="call[@status='error']" mode="rack-spine">
    <box class="w-2 bg-rose-400"><xsl:text>&#160;&#160;</xsl:text></box>
  </xsl:template>

  <xsl:template match="call[@status='pending_approval']" mode="rack-spine">
    <box class="w-2 bg-amber-200"><xsl:text>&#160;&#160;</xsl:text></box>
  </xsl:template>

  <xsl:template match="call[@status='running']" mode="rack-spine">
    <box class="w-2 bg-amber-300"><xsl:text>&#160;&#160;</xsl:text></box>
  </xsl:template>

  <xsl:template match="call[@status='denied']" mode="rack-spine">
    <box class="w-2 bg-slate-500"><xsl:text>&#160;&#160;</xsl:text></box>
  </xsl:template>

  <!-- Rack right-column outcome chip: kind-specific, dim slate so the
       timing above reads as the primary number. -->
  <xsl:template match="result[@kind='matches']" mode="rack-meta">
    <box class="text-slate-500">⨉<xsl:value-of select="@total_lines"/><xsl:text> </xsl:text><xsl:value-of select="round(@bytes div 1024)"/>K</box>
  </xsl:template>

  <xsl:template match="result[@kind='document']" mode="rack-meta">
    <box class="text-slate-500"><xsl:value-of select="@lines"/>L <xsl:value-of select="round(@bytes div 1024)"/>K</box>
  </xsl:template>

  <xsl:template match="result[@kind='process']" mode="rack-meta">
    <box class="text-slate-500">exit <xsl:value-of select="@exit"/></box>
  </xsl:template>

  <xsl:template match="result[@kind='fact']" mode="rack-meta"/>
  <xsl:template match="result[@kind='error']" mode="rack-meta">
    <box class="text-rose-400">error</box>
  </xsl:template>
  <xsl:template match="result" mode="rack-meta"/>

  <xsl:template match="call" mode="header">
    <div class="flex flex-row items-center px-1 bg-slate-900">
      <xsl:apply-templates select="." mode="spine"/>
      <box>
        <xsl:attribute name="class">
          <xsl:text>px-2 </xsl:text>
          <xsl:apply-templates select="." mode="tool-color"/>
        </xsl:attribute>
        <xsl:apply-templates select="." mode="display-name"/>
      </box>
      <box class="text-slate-600 flex-1 overflow-hidden">
        <xsl:value-of select="args"/>
      </box>
      <xsl:if test="@elapsed_ms">
        <box class="text-slate-600 px-2"><xsl:value-of select="@elapsed_ms"/>ms</box>
      </xsl:if>
      <xsl:apply-templates select="result" mode="meta"/>
    </div>
  </xsl:template>

  <!-- ================================================================
       Display names: terse single-word labels per tool kind. The
       trace XML keeps the canonical @name; this is purely visual.
       ================================================================ -->
  <xsl:template match="call[@name='rg_search']" mode="display-name">find</xsl:template>
  <xsl:template match="call[@name='read_file']" mode="display-name">file</xsl:template>
  <xsl:template match="call[@name='bash']" mode="display-name">bash</xsl:template>
  <xsl:template match="call[@name='web_fetch']" mode="display-name">fetch</xsl:template>
  <xsl:template match="call[@name='nxt_current_time']" mode="display-name">time</xsl:template>
  <xsl:template match="call[@name='nxt_terminal_size']" mode="display-name">size</xsl:template>
  <xsl:template match="call[@name='nxt_echo']" mode="display-name">echo</xsl:template>
  <xsl:template match="call" mode="display-name"><xsl:value-of select="@name"/></xsl:template>

  <!-- ================================================================
       Spine: status chip at the left edge of the header row.
       For ok status the chip carries the tool-bg color (the glow);
       for other states it carries a status-meaning color.
       ================================================================ -->
  <xsl:template match="call[@status='ok']" mode="spine">
    <box>
      <xsl:attribute name="class">
        <xsl:text>text-slate-950 px-1 </xsl:text>
        <xsl:apply-templates select="." mode="tool-bg"/>
      </xsl:attribute>
      <xsl:text>✓</xsl:text>
    </box>
  </xsl:template>

  <xsl:template match="call[@status='error']" mode="spine">
    <box class="text-slate-950 bg-rose-300 px-1">!</box>
  </xsl:template>

  <xsl:template match="call[@status='pending_approval']" mode="spine">
    <box class="text-slate-950 bg-amber-200 px-1">?</box>
  </xsl:template>

  <xsl:template match="call[@status='running']" mode="spine">
    <box class="text-amber-200 px-1">⠋</box>
  </xsl:template>

  <xsl:template match="call[@status='denied']" mode="spine">
    <box class="text-slate-50 bg-slate-500 px-1">N</box>
  </xsl:template>

  <!-- ================================================================
       Tool kind identity: foreground and background per-tool color.
       Glow shades (-300) on a deep slate ground read as emissive.
       ================================================================ -->
  <xsl:template match="call[@name='rg_search']" mode="tool-color">text-amber-300</xsl:template>
  <xsl:template match="call[@name='rg_search']" mode="tool-bg">bg-amber-300</xsl:template>

  <xsl:template match="call[@name='read_file']" mode="tool-color">text-emerald-300</xsl:template>
  <xsl:template match="call[@name='read_file']" mode="tool-bg">bg-emerald-300</xsl:template>

  <xsl:template match="call[@name='bash']" mode="tool-color">text-orange-300</xsl:template>
  <xsl:template match="call[@name='bash']" mode="tool-bg">bg-orange-300</xsl:template>

  <xsl:template match="call[starts-with(@name, 'nxt_')]" mode="tool-color">text-lime-300</xsl:template>
  <xsl:template match="call[starts-with(@name, 'nxt_')]" mode="tool-bg">bg-lime-300</xsl:template>

  <xsl:template match="call[@name='web_fetch']" mode="tool-color">text-violet-300</xsl:template>
  <xsl:template match="call[@name='web_fetch']" mode="tool-bg">bg-violet-300</xsl:template>

  <xsl:template match="call" mode="tool-color">text-teal-300</xsl:template>
  <xsl:template match="call" mode="tool-bg">bg-teal-300</xsl:template>

  <!-- ================================================================
       Meta chip (right edge of header): kind-specific count summary.
       ================================================================ -->
  <xsl:template match="result[@kind='matches']" mode="meta">
    <box class="text-slate-400">⨉<xsl:value-of select="@total_lines"/><xsl:text> </xsl:text><xsl:value-of select="round(@bytes div 1024)"/>K</box>
  </xsl:template>

  <xsl:template match="result[@kind='document']" mode="meta">
    <box class="text-slate-400"><xsl:value-of select="@lines"/>L <xsl:value-of select="round(@bytes div 1024)"/>K</box>
  </xsl:template>

  <xsl:template match="result[@kind='process']" mode="meta">
    <box class="text-slate-400">exit <xsl:value-of select="@exit"/></box>
  </xsl:template>

  <xsl:template match="result[@kind='fact']" mode="meta"/>
  <xsl:template match="result[@kind='error']" mode="meta"/>

  <!-- ================================================================
       The cassette window: kind-specific peek into the outcome.
       Slate-300 reads as soft luminance, slate-700 recedes.
       ================================================================ -->
  <xsl:template match="result[@kind='matches']">
    <div class="flex flex-col px-2 text-slate-300">
      <xsl:apply-templates select="line"/>
      <xsl:if test="@total_lines &gt; count(line)">
        <box class="text-slate-700">...<xsl:value-of select="@total_lines - count(line)"/> more lines.</box>
      </xsl:if>
    </div>
  </xsl:template>

  <xsl:template match="result[@kind='document']">
    <div class="flex flex-col px-2 text-slate-300">
      <xsl:apply-templates select="line"/>
      <xsl:if test="@lines &gt; count(line)">
        <box class="text-slate-700">...<xsl:value-of select="@lines - count(line)"/> more lines.</box>
      </xsl:if>
    </div>
  </xsl:template>

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
    <box class="text-slate-300"><xsl:value-of select="."/></box>
  </xsl:template>

  <xsl:template match="line" mode="stderr">
    <box class="text-rose-300"><xsl:value-of select="."/></box>
  </xsl:template>

  <xsl:template match="result[@kind='fact']">
    <div class="flex flex-row px-2 text-slate-300">
      <xsl:for-each select="field">
        <box class="text-slate-500 pr-1"><xsl:value-of select="@name"/>:</box>
        <box class="pr-2"><xsl:value-of select="."/></box>
      </xsl:for-each>
    </div>
  </xsl:template>

  <xsl:template match="result[@kind='error']">
    <div class="flex flex-row px-2 text-rose-300">
      <box><xsl:value-of select="message"/></box>
    </div>
  </xsl:template>

  <xsl:template match="line">
    <box><xsl:value-of select="."/></box>
  </xsl:template>

  <!-- ================================================================
       Affordances strip: warm linen key-hints against deep slate.
       Same surface the model parses as its action menu.
       ================================================================ -->
  <xsl:template match="affordances">
    <div class="flex flex-row px-2 pt-1">
      <xsl:for-each select="affordance">
        <box class="text-amber-200 bg-slate-800 px-1">
          <xsl:value-of select="@key"/>
        </box>
        <box class="text-slate-400 pl-1 pr-2">
          <xsl:value-of select="."/>
        </box>
      </xsl:for-each>
    </div>
  </xsl:template>

</xsl:stylesheet>
