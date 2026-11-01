/**
 * GeoCore 侧边面板。
 *
 *   GeoCore                           [刷新]
 *   3 个数据集 · 2 个结果 · …/workdir
 *   [数据集 3] [分析结果 2]
 *   ┌ 列表 ───────────────────────────┐
 *   ┌ 预览：标题 / 徽标 / 地图卡片 /      ┐
 *   │ 坐标系 [下拉切换] / 范围 / 字段 / 操作 │
 *   最近调用
 *
 * 只负责「看」：分析由 Claude 在对话里完成；按钮只把提示词填进输入框。
 */

import type { EngineInterface, RenderInput, RenderSurface } from 'claude-code'

import type { GeoArtifact, GeoDataset, GeoPreview } from '../types'
import type { KernelConfig } from './kernel'
import { readPanelState, refreshInventory, selectSource, setCrs, setTab } from './store'

export const PANE_ID = 'geocore'
const LIST_ROWS = 8

/** 可切换的显示坐标系 */
const CRS_OPTIONS = [
  { value: 'source', label: '数据原坐标系' },
  { value: 'EPSG:4326', label: 'WGS 84 经纬度（EPSG:4326）' },
  { value: 'EPSG:4490', label: 'CGCS2000 经纬度（EPSG:4490）' },
  { value: 'EPSG:3857', label: 'Web 墨卡托（EPSG:3857）' },
  { value: 'utm', label: 'UTM 自动分带（米）' },
] as const

const size = (bytes: number) =>
  bytes >= 1 << 20 ? `${(bytes / (1 << 20)).toFixed(1)} MB` : `${Math.max(1, Math.round(bytes / 1024))} KB`

const shortTime = (iso: string) => (iso.length >= 16 ? iso.slice(5, 16).replace('T', ' ') : iso)

const tail = (p: string, max: number) => (p.length <= max ? p : `…${p.slice(p.length - max + 1)}`)

const fileUrl = (p: string) => `file:///${p.replace(/\\/g, '/').replace(/^\/+/, '')}`

const FORMAT_LABEL: Record<string, string> = {
  geojson: 'GeoJSON', gpkg: 'GeoPackage', shapefile: 'Shapefile', csv: 'CSV', tif: 'GeoTIFF',
}

function artifactGlyph(a: GeoArtifact): string {
  if (a.kind === 'map') return '▣'
  const t = a.geometryTypes.join(' ')
  if (/Polygon/.test(t)) return '▰'
  if (/Line/.test(t)) return '╱'
  if (/Point/.test(t)) return '●'
  return '▦'
}

export async function renderPanel($: EngineInterface, e: RenderInput<'Pane'>, cfg: () => KernelConfig) {
  const { Box, Text, Button } = $.ui.resolve(e)
  const width = e.props.bodyColumns
  const s = await readPanelState($)
  const inv = s.inventory

  // ---------------------------------------------------------------- 头部
  const subtitle = inv
    ? `${inv.datasets.length} 个数据集 · ${inv.artifacts.length} 个结果 · ${tail(cfg().workdir, Math.max(16, width - 28))}`
    : tail(cfg().workdir, Math.max(20, width - 2))

  const header = (
    <Box flexDirection="column" marginBottom={1}>
      <Box flexDirection="row" justifyContent="space-between" alignItems="center">
        <Text bold size="title" weight="bold">
          GeoCore
        </Text>
        <Button key="refresh" label={s.busy ? '读取中…' : '↻ 刷新'} onPress={() => refreshInventory($, cfg())} />
      </Box>
      <Text dimColor size="footnote" wrap="truncate-start">
        {subtitle}
      </Text>
    </Box>
  )

  if (!inv) {
    return (
      <Box flexDirection="column">
        {header}
        {s.error ? (
          errorBox($, e, s.error)
        ) : (
          <Box flexDirection="column" borderStyle="round" borderDimColor paddingX={1} paddingY={1}>
            <Text>{s.busy ?? '还没读取工作目录。'}</Text>
            <Text dimColor size="footnote">
              点「刷新」扫描这里的 GeoJSON / GPKG / SHP / CSV / GeoTIFF，以及 Claude 的分析结果。
            </Text>
          </Box>
        )}
      </Box>
    )
  }

  // ---------------------------------------------------------------- 标签
  const tabs = (
    <Box flexDirection="row" gap={1} marginBottom={1}>
      <Button
        key="tab-data"
        label={`数据集 ${inv.datasets.length}`}
        variant={s.tab === 'data' ? 'primary' : 'secondary'}
        onPress={() => setTab($, 'data')}
      />
      <Button
        key="tab-results"
        label={`分析结果 ${inv.artifacts.length}`}
        variant={s.tab === 'results' ? 'primary' : 'secondary'}
        onPress={() => setTab($, 'results')}
      />
    </Box>
  )

  // ---------------------------------------------------------------- 列表
  const datasetRow = (d: GeoDataset) => {
    const on = s.selected === d.path
    return (
      <Button key={`ds:${d.path}`} plain onPress={ev => selectSource($, cfg(), d.path, d.name, ev.surface)}>
        <Text color={on ? 'suggestion' : 'subtle'}>{on ? '▸ ' : '◇ '}</Text>
        <Text bold={on} color={on ? 'suggestion' : undefined}>
          {d.name}
        </Text>
        <Text dimColor>{`  ${FORMAT_LABEL[d.format] ?? d.format} · ${size(d.sizeBytes)}`}</Text>
        {d.origin === 'agent-used' ? <Text color="claude">{' · 已分析'}</Text> : ''}
      </Button>
    )
  }

  const artifactRow = (a: GeoArtifact) => {
    const on = s.selected === a.id
    const meta = a.kind === 'map' ? `地图 · ${shortTime(a.created)}` : `${a.count} 要素 · ${shortTime(a.created)}`
    return (
      <Button key={`ar:${a.id}`} plain onPress={ev => selectSource($, cfg(), a.id, a.title || a.id, ev.surface)}>
        <Text color={on ? 'suggestion' : 'subtle'}>{on ? '▸ ' : `${artifactGlyph(a)} `}</Text>
        <Text bold={on} color={on ? 'suggestion' : undefined}>
          {a.title || a.id}
        </Text>
        <Text dimColor>{`  ${meta}`}</Text>
        {a.warnings > 0 ? <Text color="warning">{`  ⚠ ${a.warnings}`}</Text> : ''}
      </Button>
    )
  }

  const rows = s.tab === 'data' ? inv.datasets.map(datasetRow) : inv.artifacts.map(artifactRow)
  const emptyHint =
    s.tab === 'data'
      ? '这里没有找到空间数据。把文件放进工作目录，或直接把路径告诉 Claude。'
      : '还没有分析结果。在对话里让 Claude 做一次空间分析，结果会出现在这里。'

  const list = (
    <Box flexDirection="column" borderStyle="round" borderDimColor paddingX={1} marginBottom={1}>
      {rows.length === 0 ? <Text dimColor>{emptyHint}</Text> : rows.slice(0, LIST_ROWS)}
      {rows.length > LIST_ROWS ? <Text dimColor size="footnote">{`另有 ${rows.length - LIST_ROWS} 项`}</Text> : ''}
    </Box>
  )

  // ---------------------------------------------------------------- 最近调用
  const recent =
    s.activity.length > 0 ? (
      <Box flexDirection="column" marginTop={1}>
        <Text dimColor size="footnote" weight="semibold">
          最近调用
        </Text>
        {s.activity.slice(0, 3).map(r => (
          <Text wrap="truncate-end">
            <Text color={r.status === 'error' ? 'error' : r.status === 'ok' ? 'success' : 'warning'}>
              {r.status === 'running' ? '◌ ' : r.status === 'ok' ? '✓ ' : '✕ '}
            </Text>
            {r.title}
            <Text dimColor>{r.message ? `  ${r.message}` : ''}</Text>
          </Text>
        ))}
      </Box>
    ) : (
      ''
    )

  return (
    <Box flexDirection="column">
      {header}
      {tabs}
      {list}
      {s.error ? errorBox($, e, s.error) : ''}
      {s.busy ? <Text color="suggestion">{`◌ ${s.busy}`}</Text> : ''}
      {s.preview ? previewCard($, e, s.preview, inv.artifacts, s.crs, width, cfg) : ''}
      {recent}
    </Box>
  )
}

function errorBox($: EngineInterface, e: RenderInput<'Pane'>, text: string) {
  const { Box, Text } = $.ui.resolve(e)
  const [head, ...rest] = text.split('\n')
  return (
    <Box flexDirection="column" borderStyle="round" borderColor="error" paddingX={1} marginBottom={1}>
      <Text color="error" bold>
        {`✕ ${head ?? ''}`}
      </Text>
      {rest.length > 0 ? <Text dimColor size="footnote">{rest.join('\n').slice(0, 900)}</Text> : ''}
    </Box>
  )
}

function crsPicker(
  $: EngineInterface,
  e: RenderInput<'Pane'>,
  value: string,
  cfg: () => KernelConfig,
  surface: RenderSurface,
) {
  if (e.surface === 'mobile') {
    // 移动端没有下拉框：用一排按钮
    const { Box, Button } = $.ui.resolve(e)
    return (
      <Box flexDirection="row" flexWrap="wrap" gap={1}>
        {CRS_OPTIONS.map(o => (
          <Button
            key={`crs:${o.value}`}
            label={o.value === 'source' ? '原坐标系' : o.value === 'utm' ? 'UTM' : o.value}
            variant={value === o.value ? 'primary' : 'secondary'}
            onPress={() => setCrs($, cfg(), o.value, surface)}
          />
        ))}
      </Box>
    )
  }
  const { Select } = $.ui.resolve(e)
  return (
    <Select
      key="crs"
      label="坐标系"
      value={value}
      options={CRS_OPTIONS.map(o => ({ value: o.value, label: o.label }))}
      onSelect={(v, ev) => setCrs($, cfg(), v, ev.surface)}
    />
  )
}

function previewCard(
  $: EngineInterface,
  e: RenderInput<'Pane'>,
  pv: GeoPreview,
  artifacts: GeoArtifact[],
  crs: string,
  width: number,
  cfg: () => KernelConfig,
) {
  const { Box, Text, Button, Link } = $.ui.resolve(e)
  const artifact = artifacts.find(a => a.id === pv.source)
  const isArtifact = pv.source.startsWith('ar_')
  const fill = (text: string) => () => {
    void $.prompt.fill({ text, mode: 'replace' })
  }

  let picture
  if (e.surface === 'terminal') {
    const { Image } = $.ui.resolve(e)
    const cols = Math.max(20, Math.min(width - 4, 90))
    picture = pv.png ? (
      <Image key="map" source={{ png: pv.png }} columns={cols} rows={Math.round(cols * 0.36)} alt={`${pv.label} 预览`} />
    ) : (
      ''
    )
  } else {
    const { Svg } = $.ui.resolve(e)
    picture = <Svg source={pv.svg} alt={`${pv.label} · ${pv.crsLabel}`} />
  }

  return (
    <Box flexDirection="column" borderStyle="round" borderDimColor paddingX={1} paddingY={1}>
      <Text bold size="heading" wrap="truncate-end">
        {pv.label}
      </Text>
      <Box flexDirection="row" flexWrap="wrap" columnGap={1} marginBottom={1}>
        {pv.chips.map((c, i) => (
          <Text color={i === 0 ? 'claude' : 'subtle'} size="footnote">
            {i === 0 ? c : `· ${c}`}
          </Text>
        ))}
      </Box>

      {picture}

      <Box flexDirection="column" marginTop={1}>
        {crsPicker($, e, crs, cfg, e.surface)}
        <Text dimColor size="footnote" wrap="truncate-end">
          {`显示：${pv.crsLabel}${pv.sourceCrs && crs !== 'source' ? `（数据原为 ${pv.sourceCrs}）` : ''}`}
        </Text>
        {pv.extent ? (
          <Text dimColor size="footnote" mono wrap="truncate-end">
            {pv.extent}
          </Text>
        ) : (
          ''
        )}
      </Box>

      {pv.fields.length > 0 ? (
        <Box flexDirection="column" marginTop={1}>
          <Text dimColor size="footnote" weight="semibold">{`字段 ${pv.fields.length}`}</Text>
          <Text size="footnote" wrap="truncate-end">
            {pv.fields.slice(0, 10).join(' · ')}
            {pv.fields.length > 10 ? ' …' : ''}
          </Text>
        </Box>
      ) : (
        ''
      )}

      {artifact && artifact.summary.length > 0 ? (
        <Box flexDirection="column" marginTop={1}>
          <Text dimColor size="footnote" weight="semibold">
            分析步骤
          </Text>
          {artifact.summary.slice(0, 5).map((line, i) => (
            <Text size="footnote" wrap="truncate-end">{`${i + 1}. ${line}`}</Text>
          ))}
        </Box>
      ) : (
        ''
      )}

      {pv.warnings.length > 0 ? (
        <Box flexDirection="column" marginTop={1}>
          {pv.warnings.slice(0, 3).map(w => (
            <Text color="warning" size="footnote" wrap="truncate-end">{`⚠ ${w}`}</Text>
          ))}
        </Box>
      ) : (
        ''
      )}

      {artifact?.images.length ? (
        <Box flexDirection="column" marginTop={1}>
          {artifact.images.slice(-2).map(img => (
            <Link href={fileUrl(img)} label={`🗺 打开地图 ${img.split(/[\\/]/).pop()}`} />
          ))}
        </Box>
      ) : (
        ''
      )}

      <Box flexDirection="row" gap={1} marginTop={1}>
        <Button
          key="ask-analyze"
          variant="primary"
          label={isArtifact ? '接着分析' : '让 Claude 分析'}
          onPress={fill(
            isArtifact
              ? `基于 GeoCore 结果 ${pv.source}（${pv.label}）继续分析：`
              : `用 GeoCore 看一下 ${pv.source}，说说这份数据的内容和能做哪些分析。`,
          )}
        />
        <Button key="ask-map" label="出专题图" onPress={fill(`用 GeoCore 把 ${pv.source} 画成专题图：`)} />
      </Box>
    </Box>
  )
}
