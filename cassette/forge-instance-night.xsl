<?xml version="1.0" encoding="UTF-8"?>
<!--
  Forge Instance Night

  Transforms Forge/Sterling Alloy XML into xtc markup for terminal
  rendering. This keeps Forge XML as the semantic artifact and treats
  text display as a stylesheet, matching the cassette trace pipeline.
-->
<xsl:stylesheet version="1.0"
  xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="xml" omit-xml-declaration="yes" indent="no"/>
  <xsl:strip-space elements="*"/>

  <xsl:param name="tuple_limit" select="6"/>
  <xsl:param name="view" select="'relations'"/>
  <xsl:key name="sig-by-id" match="sig" use="@ID"/>

  <xsl:template match="alloy">
    <root class="flex flex-col bg-slate-950 text-amber-50">
      <xsl:apply-templates select="instance"/>
    </root>
  </xsl:template>

  <xsl:template match="instance">
    <div class="flex flex-col">
      <xsl:apply-templates select="." mode="header"/>
      <xsl:choose>
        <xsl:when test="$view = 'objects'">
          <xsl:apply-templates select="." mode="objects"/>
        </xsl:when>
        <xsl:when test="$view = 'compact'">
          <xsl:apply-templates select="." mode="compact"/>
        </xsl:when>
        <xsl:otherwise>
          <xsl:apply-templates select="." mode="relations"/>
        </xsl:otherwise>
      </xsl:choose>
    </div>
  </xsl:template>

  <xsl:template match="instance" mode="relations">
      <div class="flex flex-row items-center px-1 pt-1 bg-slate-950">
        <box class="text-sky-300 px-2">sigs</box>
        <box class="text-slate-600 flex-1">
          <xsl:value-of select="count(sig[not(@builtin='yes')])"/>
          <xsl:text> user signatures</xsl:text>
        </box>
      </div>
      <xsl:apply-templates select="sig[not(@builtin='yes')]"/>

      <div class="flex flex-row items-center px-1 pt-1 bg-slate-950">
        <box class="text-emerald-300 px-2">fields</box>
        <box class="text-slate-600 flex-1">
          <xsl:value-of select="count(field[not(@label='no-field-guard')])"/>
          <xsl:text> relations</xsl:text>
        </box>
      </div>
      <xsl:apply-templates select="field[not(@label='no-field-guard')]"/>
  </xsl:template>

  <xsl:template match="instance" mode="objects">
    <div class="flex flex-row items-center px-1 pt-1 bg-slate-950">
      <box class="text-fuchsia-300 px-2">objects</box>
      <box class="text-slate-600 flex-1">
        <xsl:value-of select="count(sig[not(@builtin='yes')]/atom)"/>
        <xsl:text> atoms grouped by signature</xsl:text>
      </box>
    </div>
    <xsl:apply-templates select="sig[not(@builtin='yes')]" mode="objects"/>
  </xsl:template>

  <xsl:template match="instance" mode="compact">
    <div class="flex flex-row items-center px-1 pt-1 bg-slate-950">
      <box class="text-fuchsia-300 px-2">compact</box>
      <box class="text-slate-600 flex-1">
        <xsl:value-of select="count(sig[not(@builtin='yes')]/atom)"/>
        <xsl:text> atoms, one row each</xsl:text>
      </box>
    </div>
    <xsl:apply-templates select="sig[not(@builtin='yes')]" mode="compact"/>
  </xsl:template>

  <xsl:template match="instance" mode="header">
    <div class="flex flex-col bg-slate-900">
      <div class="flex flex-row items-center px-1">
        <box class="bg-sky-300 w-2">&#160;&#160;</box>
        <box class="text-sky-300 px-2">forge</box>
        <box class="text-amber-50 flex-1 overflow-hidden">
          <xsl:value-of select="@command"/>
        </box>
        <box class="text-slate-500 px-2">
          <xsl:text>bw </xsl:text><xsl:value-of select="@bitwidth"/>
        </box>
      </div>
      <div class="flex flex-row items-center px-1">
        <box class="bg-sky-300 w-2">&#160;&#160;</box>
        <box class="text-slate-500 px-2 flex-1 overflow-hidden">
          <xsl:value-of select="@filename"/>
        </box>
        <box class="text-slate-500">
          <xsl:value-of select="$view"/>
          <xsl:text> · </xsl:text>
          <xsl:value-of select="@version"/>
        </box>
      </div>
    </div>
  </xsl:template>

  <xsl:template match="sig" mode="objects">
    <xsl:variable name="sig-id" select="@ID"/>
    <div class="flex flex-row items-center px-1 bg-slate-900">
      <box class="bg-fuchsia-300 w-2">&#160;&#160;</box>
      <box class="text-fuchsia-300 px-2">
        <xsl:value-of select="@label"/>
      </box>
      <box class="text-slate-500 flex-1"/>
    </div>
    <xsl:apply-templates select="atom" mode="object"/>
  </xsl:template>

  <xsl:template match="atom" mode="object">
    <xsl:variable name="atom-label" select="@label"/>
    <xsl:variable name="sig-id" select="../@ID"/>
    <div class="flex flex-col">
      <div class="flex flex-row items-center px-1 bg-slate-950">
        <box class="text-slate-700 px-2">·</box>
        <box class="text-amber-50 px-2">
          <xsl:value-of select="@label"/>
        </box>
      </div>
      <div class="flex flex-col px-6 text-slate-300">
        <xsl:choose>
          <xsl:when test="../../field[@parentID=$sig-id and tuple/atom[1]/@label=$atom-label and not(@label='no-field-guard')]">
            <xsl:for-each select="../../field[@parentID=$sig-id and tuple/atom[1]/@label=$atom-label and not(@label='no-field-guard')]">
              <xsl:apply-templates select="." mode="object-field">
                <xsl:with-param name="atom-label" select="$atom-label"/>
              </xsl:apply-templates>
            </xsl:for-each>
          </xsl:when>
          <xsl:otherwise>
            <box class="text-slate-700">no populated fields</box>
          </xsl:otherwise>
        </xsl:choose>
      </div>
    </div>
  </xsl:template>

  <xsl:template match="field" mode="object-field">
    <xsl:param name="atom-label"/>
    <box>
      <xsl:value-of select="@label"/>
      <xsl:text>: </xsl:text>
      <xsl:for-each select="tuple[atom[1]/@label=$atom-label]">
        <xsl:if test="position() &gt; 1"><xsl:text>, </xsl:text></xsl:if>
        <xsl:apply-templates select="." mode="tuple-tail"/>
      </xsl:for-each>
    </box>
  </xsl:template>

  <xsl:template match="sig" mode="compact">
    <xsl:apply-templates select="atom" mode="compact"/>
  </xsl:template>

  <xsl:template match="atom" mode="compact">
    <xsl:variable name="atom-label" select="@label"/>
    <xsl:variable name="sig-id" select="../@ID"/>
    <div class="flex flex-row items-center px-1 bg-slate-900">
      <box class="bg-fuchsia-300 w-2">&#160;&#160;</box>
      <box class="text-fuchsia-300 px-2">
        <xsl:value-of select="../@label"/>
      </box>
      <box class="text-amber-50 px-2">
        <xsl:value-of select="@label"/>
      </box>
      <box class="text-slate-300 flex-1 overflow-hidden">
        <xsl:choose>
          <xsl:when test="../../field[@parentID=$sig-id and tuple/atom[1]/@label=$atom-label and not(@label='no-field-guard')]">
            <xsl:for-each select="../../field[@parentID=$sig-id and tuple/atom[1]/@label=$atom-label and not(@label='no-field-guard')]">
              <xsl:if test="position() &gt; 1"><xsl:text>  </xsl:text></xsl:if>
              <xsl:value-of select="@label"/>
              <xsl:text>=</xsl:text>
              <xsl:for-each select="tuple[atom[1]/@label=$atom-label]">
                <xsl:if test="position() &gt; 1"><xsl:text>,</xsl:text></xsl:if>
                <xsl:apply-templates select="." mode="tuple-tail"/>
              </xsl:for-each>
            </xsl:for-each>
          </xsl:when>
          <xsl:otherwise>
            <xsl:text>-</xsl:text>
          </xsl:otherwise>
        </xsl:choose>
      </box>
    </div>
  </xsl:template>

  <xsl:template match="sig">
    <div class="flex flex-row items-center px-1 bg-slate-900">
      <box class="bg-sky-300 w-2">&#160;&#160;</box>
      <box class="text-sky-300 px-2">
        <xsl:value-of select="@label"/>
      </box>
      <box class="text-slate-500 px-1">
        <xsl:value-of select="count(atom)"/>
        <xsl:text> atoms</xsl:text>
      </box>
      <box class="text-slate-300 flex-1 overflow-hidden">
        <xsl:for-each select="atom">
          <xsl:if test="position() &gt; 1"><xsl:text>, </xsl:text></xsl:if>
          <xsl:value-of select="@label"/>
        </xsl:for-each>
      </box>
    </div>
  </xsl:template>

  <xsl:template match="field">
    <div class="flex flex-col">
      <div class="flex flex-row items-center px-1 bg-slate-900">
        <box class="bg-emerald-300 w-2">&#160;&#160;</box>
        <box class="text-emerald-300 px-2">
          <xsl:value-of select="@label"/>
        </box>
        <box class="text-slate-500 px-1">
          <xsl:value-of select="count(tuple)"/>
          <xsl:text> tuples</xsl:text>
        </box>
        <box class="text-slate-500 flex-1 overflow-hidden">
          <xsl:apply-templates select="types" mode="signature"/>
        </box>
      </div>
      <div class="flex flex-col px-4 text-slate-300">
        <xsl:choose>
          <xsl:when test="tuple">
            <xsl:apply-templates select="tuple[position() &lt;= $tuple_limit]"/>
            <xsl:if test="count(tuple) &gt; $tuple_limit">
              <box class="text-slate-700">
                <xsl:text>...</xsl:text>
                <xsl:value-of select="count(tuple) - $tuple_limit"/>
                <xsl:text> more tuples.</xsl:text>
              </box>
            </xsl:if>
          </xsl:when>
          <xsl:otherwise>
            <box class="text-slate-700">empty relation</box>
          </xsl:otherwise>
        </xsl:choose>
      </div>
    </div>
  </xsl:template>

  <xsl:template match="types" mode="signature">
    <xsl:for-each select="type">
      <xsl:if test="position() &gt; 1"><xsl:text>-&gt;</xsl:text></xsl:if>
      <xsl:variable name="id" select="@ID"/>
      <xsl:choose>
        <xsl:when test="key('sig-by-id', $id)">
          <xsl:value-of select="key('sig-by-id', $id)[1]/@label"/>
        </xsl:when>
        <xsl:otherwise>
          <xsl:text>#</xsl:text><xsl:value-of select="$id"/>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:for-each>
  </xsl:template>

  <xsl:template match="tuple">
    <box>
      <xsl:for-each select="atom">
        <xsl:if test="position() &gt; 1"><xsl:text>-&gt;</xsl:text></xsl:if>
        <xsl:value-of select="@label"/>
      </xsl:for-each>
    </box>
  </xsl:template>

  <xsl:template match="tuple" mode="tuple-tail">
    <xsl:choose>
      <xsl:when test="count(atom) &gt; 1">
        <xsl:for-each select="atom[position() &gt; 1]">
          <xsl:if test="position() &gt; 1"><xsl:text>-&gt;</xsl:text></xsl:if>
          <xsl:value-of select="@label"/>
        </xsl:for-each>
      </xsl:when>
      <xsl:otherwise>
        <xsl:text>true</xsl:text>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

</xsl:stylesheet>
