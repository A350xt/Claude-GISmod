/** 面板状态（$.state 原子）与读写它们的动作。所有值的类型见 types/index.d.ts。 */

import { atom, read, update } from 'claude-code'
import type { EngineInterface, RenderSurface } from 'claude-code'

import type {
  GeoActivity,
  GeoArtifact,
  GeoDataset,
  GeoInventory,
  GeoPreview,
  GeoTab,
} from '../types'
import { callKernel, describeError, type KernelConfig } from './kernel'

// 结构变化时改 shape 标签：旧版本存下的值会被当作初始值丢弃
const inventory = atom({ plugin: 'geocore', key: 'inventory' } as const, null, {
  shape: 'inventory-1',
})
const preview = atom({ plugin: 'geocore', key: 'preview' } as const, null, {
  shape: 'preview-2',
})
const activity = atom({ plugin: 'geocore', key: 'activity' } as const, [], {
  shape: 'activity-1',
})
const tab = atom({ plugin: 'geocore', key: 'tab' } as const, 'data' as GeoTab)
const selected = atom({ plugin: 'geocore', key: 'selected' } as const, null)
const crs = atom({ plugin: 'geocore', key: 'crs' } as const, 'source')
const busy = atom({ plugin: 'geocore', key: 'busy' } as const, null)
const lastError = atom({ plugin: 'geocore', key: 'error' } as const, null)

// ------------------------------------------------------------------ 清单

type RawInventory = {
  datasets?: Array<{ name: string; path: string; format: string; size_bytes: number; origin: string }>
  artifacts?: Array<{
    artifact_id: string
    kind?: string
    title?: string
    created?: string
    count?: number
    geometry_types?: string[]
    summary?: string[]
    warnings?: number
    steps?: Array<{ id?: string } | string>
    images?: string[]
  }>
}

function toInventory(raw: RawInventory, now: number): GeoInventory {
  const datasets: GeoDataset[] = (raw.datasets ?? []).map(d => ({
    name: d.name,
    path: d.path,
    format: d.format,
    sizeBytes: d.size_bytes,
    origin: d.origin,
  }))
  const artifacts: GeoArtifact[] = (raw.artifacts ?? []).map(a => ({
    id: a.artifact_id,
    kind: a.kind ?? 'analysis',
    title: a.title ?? '',
    created: a.created ?? '',
    count: a.count ?? 0,
    geometryTypes: a.geometry_types ?? [],
    summary: (a.summary ?? []).map(String),
    warnings: a.warnings ?? 0,
    steps: (a.steps ?? []).map(s => (typeof s === 'string' ? s : String(s.id ?? ''))).filter(Boolean),
    images: a.images ?? [],
  }))
  return { datasets, artifacts, loadedAt: now }
}

export async function refreshInventory($: EngineInterface, cfg: KernelConfig): Promise<void> {
  await update($, busy, () => '正在读取工作目录…')
  try {
    const raw = await callKernel<RawInventory>($, cfg, 'list', { datasets_dirs: [cfg.workdir] })
    await update($, inventory, () => toInventory(raw, Date.now()))
    await update($, lastError, () => null)
  } catch (err) {
    await update($, lastError, () => describeError(err))
  } finally {
    await update($, busy, () => null)
  }
}

// ------------------------------------------------------------------ 预览

type RawPreview = {
  kind: 'vector' | 'raster'
  svg: string
  png_base64: string | null
  warnings: string[]
  meta: {
    count?: number
    geometry_types?: Record<string, number>
    crs?: string | null
    crs_label?: string
    crs_units?: string
    source_crs?: string | null
    bounds?: [number, number, number, number] | null
    fields?: Array<{ name: string; dtype: string }>
    shape?: [number, number]
    cellsize?: number
    nodata_pct?: number
    quantiles?: Record<string, number>
  }
}

const fmt = (n: number | undefined) => {
  if (n === undefined || !Number.isFinite(n)) return '—'
  const a = Math.abs(n)
  return a >= 1000 ? Math.round(n).toLocaleString('en-US') : a >= 1 ? n.toFixed(2) : n.toPrecision(3)
}

const GEOM_LABEL: Record<string, string> = {
  Point: '点', MultiPoint: '多点', LineString: '线', MultiLineString: '多线',
  Polygon: '面', MultiPolygon: '多面',
}

function chipsOf(raw: RawPreview): string[] {
  const m = raw.meta
  if (raw.kind === 'raster') {
    const [rows, cols] = m.shape ?? [0, 0]
    const q = m.quantiles ?? {}
    return [`栅格 ${rows}×${cols}`, `像元 ${fmt(m.cellsize)}`, `值域 ${fmt(q.min)} ~ ${fmt(q.max)}`,
      `NoData ${m.nodata_pct ?? 0}%`]
  }
  const types = Object.entries(m.geometry_types ?? {}).map(([t, n]) => `${GEOM_LABEL[t] ?? t} ${n}`)
  return [`${m.count ?? 0} 要素`, ...types]
}

function extentOf(raw: RawPreview): string {
  const b = raw.meta.bounds
  if (!b) return ''
  const geographic = raw.meta.crs_units === '度'
  const f = (v: number) => (geographic ? v.toFixed(4) + '°' : fmt(v))
  return `X ${f(b[0])} ~ ${f(b[2])}　Y ${f(b[1])} ~ ${f(b[3])}`
}

async function loadPreview(
  $: EngineInterface,
  cfg: KernelConfig,
  source: string,
  label: string,
  surface: RenderSurface,
): Promise<void> {
  const displayCrs = await read($, crs)
  await update($, busy, () => `正在预览 ${label}…`)
  try {
    const raw = await callKernel<RawPreview>($, cfg, 'preview', {
      source,
      crs: displayCrs,
      png: surface === 'terminal',
    })
    const next: GeoPreview = {
      source,
      label,
      kind: raw.kind,
      chips: chipsOf(raw),
      crsLabel: raw.meta.crs_label ?? raw.meta.crs ?? '无坐标系',
      sourceCrs: raw.meta.source_crs ?? null,
      extent: extentOf(raw),
      fields: (raw.meta.fields ?? []).map(f => f.name),
      svg: raw.svg,
      png: raw.png_base64,
      warnings: raw.warnings ?? [],
    }
    await update($, preview, () => next)
    await update($, lastError, () => null)
  } catch (err) {
    await update($, lastError, () => describeError(err))
  } finally {
    await update($, busy, () => null)
  }
}

export async function selectSource(
  $: EngineInterface,
  cfg: KernelConfig,
  source: string,
  label: string,
  surface: RenderSurface,
): Promise<void> {
  await update($, selected, () => source)
  await loadPreview($, cfg, source, label, surface)
}

/** 切换显示坐标系，并用新坐标系重画当前预览。 */
export async function setCrs(
  $: EngineInterface,
  cfg: KernelConfig,
  value: string,
  surface: RenderSurface,
): Promise<void> {
  await update($, crs, () => value)
  const current = await read($, preview)
  if (current) await loadPreview($, cfg, current.source, current.label, surface)
}

// ------------------------------------------------------------------ 工具调用记录

export async function logActivity($: EngineInterface, entry: GeoActivity): Promise<void> {
  await update($, activity, list => {
    const rest = list.filter(a => a.id !== entry.id)
    return [entry, ...rest].slice(0, 20)
  })
}

export async function setTab($: EngineInterface, value: GeoTab): Promise<void> {
  await update($, tab, () => value)
}

/** 面板一次绘制需要的全部状态（读取即订阅，值变化时面板自动重绘）。 */
export async function readPanelState($: EngineInterface) {
  return {
    inventory: await read($, inventory),
    tab: await read($, tab),
    selected: await read($, selected),
    crs: await read($, crs),
    preview: await read($, preview),
    busy: await read($, busy),
    error: await read($, lastError),
    activity: await read($, activity),
  }
}
