/** GeoCore mod 的状态契约：面板读取的每个 $.state 值都在这里声明。 */

export type GeoDataset = {
  name: string
  path: string
  format: string
  sizeBytes: number
  /** agent-used = Claude 分析时用过；workspace = 工作目录扫描到 */
  origin: string
}

export type GeoArtifact = {
  id: string
  kind: string
  title: string
  created: string
  count: number
  geometryTypes: string[]
  summary: string[]
  warnings: number
  steps: string[]
  images: string[]
}

export type GeoInventory = {
  datasets: GeoDataset[]
  artifacts: GeoArtifact[]
  loadedAt: number
}

export type GeoPreview = {
  source: string
  label: string
  kind: 'vector' | 'raster'
  /** 摘要徽标，如「40 要素」「Polygon」 */
  chips: string[]
  /** 当前显示坐标系的名称，如「EPSG:4326 · WGS 84」 */
  crsLabel: string
  /** 数据本身的坐标系 */
  sourceCrs: string | null
  /** 范围（按显示坐标系） */
  extent: string
  fields: string[]
  svg: string
  /** 终端用的 PNG（base64）；桌面端为 null，直接画 svg */
  png: string | null
  warnings: string[]
}

export type GeoActivity = {
  id: string
  tool: string
  title: string
  status: 'running' | 'ok' | 'error'
  message: string
}

export type GeoTab = 'data' | 'results'

declare module 'claude-code' {
  interface PluginState {
    geocore: {
      inventory: Shaped<GeoInventory | null>
      tab: GeoTab
      selected: string | null
      /** 预览的显示坐标系：source | utm | EPSG:xxxx */
      crs: string
      preview: Shaped<GeoPreview | null>
      busy: string | null
      error: string | null
      activity: Shaped<GeoActivity[]>
    }
  }
}
