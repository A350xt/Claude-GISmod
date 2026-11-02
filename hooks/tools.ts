/**
 * 模型可见的两个 GIS 工具：mcp__gismod__inspect / analyze。
 *
 * 治理铁律：工具面只有这两个。新能力进 analyze 的操作词表，不新增工具；
 * 结果地图由对话卡片与 /gis 面板表达（原生内核不出 PNG 专题图）。
 */

import type { ToolSpec } from 'claude-code'
import type { KernelAction } from './kernel'

export type GeoTool = {
  spec: ToolSpec
  action: KernelAction
  /** 状态栏/面板上显示的动词 */
  verb: string
}

const OPS = [
  '矢量算子：',
  '- query.filter {input, where}  属性过滤（pandas query 语法；关键字列名用反引号如 `class`；时间列已自动解析，可写 time >= "2024-06-01"）',
  '- query.select {input, predicate, ref}  空间选择；predicate ∈ intersects/within/contains/touches/crosses/overlaps',
  '- query.measure {input, measures, group_by?}  度量；measures ⊆ [area_sqkm, area_ha, length_km, count]；group_by 的分组结果在 steps[].table',
  '- proximity.buffer {input, distance_m, dissolve?, quad_segs?}',
  '- proximity.within_distance {input, ref, distance_m}',
  '- proximity.nearest {input, ref, k?, join_attrs?}',
  '- overlay.intersection / difference / union {a, b}；overlay.clip {a, b}（只保留 a 属性）',
  '- zonal.summarize {regions, data, stats?, field?}  分区统计（点计数/属性和、线长占比、面面积占比与加权）',
  '- trajectory.build {input, time_field, group_by?}  点按时间连成轨迹线',
  '',
  '栅格算子（GeoTIFF 输入/输出；多输入自动对齐到首个输入的网格）：',
  '- raster.info {input}；raster.create {bounds, cellsize, crs?, value?}；raster.from_vector {input, cellsize|template, field?}',
  '- raster.align {input, template, method?}；raster.mask {input, mask, invert?, fill?}；raster.merge {inputs[]}',
  '- raster.distance {input, cellsize?, max_distance_m?}；raster.distance_decay {input, d0, f0?, mode?}',
  '- raster.cost_distance {source, cost}；raster.cost_path {source, cost, to}；raster.nearest {input, ref}',
  '- raster.con {input, condition, true, false}；raster.calc {inputs:{别名:引用}, expression}',
  '- raster.reclassify {input, mapping:[[lo,hi,new]…]}；raster.breaks {input, classes, method?}；raster.histogram {input, bins?}',
  '- raster.weighted_sum {inputs:[{ref,weight}…]}；raster.focal {input, stat, kernel_size?}；raster.zonal_stats {regions, data, stats?}',
  '- raster.polygonize {input, field?, dissolve?}；raster.contour {input, levels?|interval?}；raster.sample {raster, points, field?}',
].join('\n')

export const TOOLS: GeoTool[] = [
  {
    action: 'inspect',
    verb: '查看数据',
    spec: {
      name: 'inspect',
      description: [
        'GIS：理解一份空间数据（不做分析）。遇到陌生地理数据，先 inspect 再分析。',
        '支持 GeoJSON / GeoPackage / Shapefile / CSV（含经纬度列）/ GeoTIFF。',
        '返回几何类型、CRS、范围、要素数、字段与样本值、质量告警（无效几何/缺 CRS）与可做的分析类别。',
      ].join('\n'),
      inputSchema: {
        type: 'object',
        properties: {
          path: { type: 'string', description: '数据文件的绝对路径（正斜杠）' },
          layer: { type: 'string', description: 'GPKG 图层名；缺省取第一层' },
        },
        required: ['path'],
      },
    },
  },
  {
    action: 'analyze',
    verb: '空间分析',
    spec: {
      name: 'analyze',
      description: [
        'GIS：执行空间分析。按顺序运行 operations 中的 GIS 原语步骤，后一步可用 "@步骤id" 引用前一步结果。',
        '',
        OPS,
        '',
        '规则：',
        '- 距离单位米。度量类操作自动投影到米制 CRS；纯过滤/选择保持源 CRS；顶层 crs 可覆盖（"source" 或 "EPSG:32651"）。',
        '- 结果写入 <workdir>/artifacts/ 并返回 artifact_id；响应自带 preview（前 10 行）与 steps[].table。',
        '- 后续引用：@步骤id（同一请求）、artifact_id（最终结果）、artifact_id#步骤id（已保存的中间步骤）。',
        '- 空结果默认报 E_EMPTY_RESULT；确属预期时给该步加 "allow_empty": true。',
        '- 永远不要自己计算坐标、面积或长度，一律用本工具度量。',
      ].join('\n'),
      inputSchema: {
        type: 'object',
        properties: {
          title: { type: 'string', description: '本次分析的标题（会显示在面板里）' },
          crs: { type: 'string', description: "分析坐标系覆盖：'source' 或 'EPSG:xxxx'；缺省自动" },
          operations: {
            type: 'array',
            minItems: 1,
            items: {
              type: 'object',
              properties: {
                id: { type: 'string', description: '步骤标识，供 @引用' },
                op: { type: 'string', description: '操作名，见词表' },
                allow_empty: { type: 'boolean' },
              },
              required: ['id', 'op'],
              additionalProperties: true,
            },
          },
          output: {
            type: 'object',
            properties: { format: { type: 'string', enum: ['gpkg', 'geojson'] } },
          },
        },
        required: ['operations'],
      },
    },
  },
]

/** 结果给模型的文本上限：超过就截断并提示缩小范围。 */
const MAX_RESULT_CHARS = 120_000

export function resultText(value: unknown): string {
  const text = JSON.stringify(value)
  return text.length > MAX_RESULT_CHARS
    ? `${text.slice(0, MAX_RESULT_CHARS)}…[结果过大已截断：请缩小分析范围或用 artifact 引用继续]`
    : text
}
