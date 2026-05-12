-- Convert one agent run from traces/all.parquet into a trace XML
-- document the baltic-birch cassette stylesheet can render.
--
-- Parameters (set via SET VARIABLE before running):
--   run    : run_id to render (e.g. '1VGR6SG2')
--   turns  : how many turns from the start to include
--   lines  : preview lines per outcome

LOAD nanoarrow;
LOAD webbed;

WITH
  events AS (
    SELECT * FROM read_parquet('traces/all.parquet')
    WHERE run_id = getvariable('run')
      AND length(coalesce(payload_json, '')) > 0
  ),
  turned AS (
    SELECT *,
      sum(case when event_type = 'response.created' then 1 else 0 end)
        OVER (ORDER BY seq) AS turn_id
    FROM events
  ),
  thoughts AS (
    SELECT turn_id,
      regexp_replace(
        json_extract_string(payload_json, '$.text'),
        '^\*\*[^*]+\*\*\s*', ''
      ) AS thought
    FROM turned
    WHERE event_type = 'response.reasoning_summary_text.done'
  ),
  raw_calls AS (
    SELECT turn_id, seq, elapsed_ms AS call_ms,
      json_extract_string(payload_json, '$.name')      AS tool,
      json_extract_string(payload_json, '$.call_id')   AS call_id,
      json_extract_string(payload_json, '$.arguments') AS args
    FROM turned
    WHERE event_type = 'call'
  ),
  raw_results AS (
    SELECT
      elapsed_ms AS result_ms,
      json_extract_string(payload_json, '$.call_id') AS call_id,
      json_extract_string(payload_json, '$.output')  AS inner_json
    FROM turned
    WHERE event_type = 'result'
  ),
  -- Pull the right content field per tool: rg/bash/web → output,
  -- read → contents, facts → render the inner JSON inline.
  call_results AS (
    SELECT
      c.turn_id, c.seq, c.tool, c.args, c.call_id,
      r.inner_json,
      greatest(coalesce(r.result_ms - c.call_ms, 0), 0) AS duration_ms,
      case c.tool
        when 'rg_search'         then 'matches'
        when 'read_file'         then 'document'
        when 'bash'              then 'process'
        when 'web_fetch'         then 'document'
        else                          'fact'
      end AS kind,
      case c.tool
        when 'read_file' then json_extract_string(r.inner_json, '$.contents')
        when 'rg_search' then json_extract_string(r.inner_json, '$.output')
        when 'bash'      then json_extract_string(r.inner_json, '$.output')
        when 'web_fetch' then json_extract_string(r.inner_json, '$.output')
        else                  null
      end AS content,
      try_cast(json_extract_string(r.inner_json, '$.bytes') AS BIGINT) AS bytes
    FROM raw_calls c
    LEFT JOIN raw_results r USING (call_id)
  ),
  call_with_lines AS (
    SELECT *,
      coalesce(
        length(string_split(content, chr(10))),
        0
      ) AS total_lines,
      coalesce(
        list_aggregate(
          list_transform(
            list_filter(
              list_slice(string_split(coalesce(content,''), chr(10)),
                         1, getvariable('lines')::int),
              x -> length(trim(x)) > 0
            ),
            x -> '<line>' || html_escape(
                  case when length(x) > 160 then substr(x, 1, 158) || '…' else x end
                ) || '</line>'
          ),
          'string_agg', ''
        ),
        ''
      ) AS lines_xml
    FROM call_results
  ),
  result_xml AS (
    SELECT call_id, turn_id, seq, tool, args, duration_ms, kind, total_lines, bytes,
      case kind
        when 'matches' then
          '<result kind="matches" total_lines="' || total_lines
            || '" bytes="' || coalesce(bytes, 0) || '">'
            || lines_xml || '</result>'
        when 'document' then
          '<result kind="document" lines="' || total_lines
            || '" bytes="' || coalesce(bytes, 0) || '">'
            || lines_xml || '</result>'
        when 'process' then
          '<result kind="process" exit="0" bytes="' || coalesce(bytes, 0) || '">'
            || '<stream channel="stdout">' || lines_xml || '</stream></result>'
        else
          '<result kind="fact"><field name="output">'
            || html_escape(coalesce(substr(inner_json, 1, 80), ''))
            || '</field></result>'
      end AS xml
    FROM call_with_lines
  ),
  call_xml AS (
    SELECT turn_id, seq,
      '<call name="' || html_escape(tool)
        || '" status="ok" elapsed_ms="' || duration_ms || '">'
        || '<args>'
        || coalesce(
             case when args is null or args = '' or args = '{}' then ''
                  else regexp_replace(
                         regexp_replace(json_to_xml(args)::VARCHAR,
                                        '^<\?xml[^>]*\?>\s*', ''),
                         '^<root>(.*)</root>\s*$', '\1')
             end, '')
        || '</args>'
        || xml
        || '</call>' AS xml
    FROM result_xml
  ),
  turn_xml AS (
    SELECT t.turn_id,
      '<turn>'
        || '<thought>' || html_escape(t.thought) || '</thought>'
        || coalesce(
             (SELECT string_agg(c.xml, '' ORDER BY c.seq)
              FROM call_xml c WHERE c.turn_id = t.turn_id),
             ''
           )
        || '</turn>' AS xml
    FROM thoughts t
    WHERE t.turn_id <= getvariable('turns')::int
  )
SELECT
  '<?xml version="1.0" encoding="UTF-8"?>'
    || '<trace run_id="' || getvariable('run') || '">'
    || string_agg(xml, '' ORDER BY turn_id)
    || '</trace>' AS xml
FROM turn_xml;
