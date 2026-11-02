/**
 * 对话内结果卡片：替换 gismod 三个工具在对话里的结果块。
 *
 *   ▰ 主干道 2km 内地块                 空间分析
 *   32 要素 · 面 32 · EPSG:32651 · WGS 84 / UTM zone 51N
 *   [ 地图卡片 ]
 *   1. 属性过滤…  2. 缓冲区…  3. 叠加求交…
 *   ⚠ 告警
 *   ar_xxx   [在面板中查看]  [打开地图]
 *
 * ctrl+o 展开时交回引擎，显示原始 JSON 结果。
 */

import type { EngineInterface, RenderInput } from 'claude-code'

import type { KernelConfig } from './kernel'
import { showInPanel } from './panel'
import { readCard } from './store'

const KIND_LABEL: Record<string, string> = {
  'mcp__gismod__inspect': '数据概览',
  'mcp__gismod__analyze': '空间分析',
  'mcp__gismod__visualize': '专题图',
}

const GLYPH: Record<string, string> = { vector: '▰', raster: '▦', map: '▣' }

const fileUrl = (p: string) => `file:///${p.replace(/\\/g, '/').replace(/^\/+/, '')}`

export async function renderCard(
  $: EngineInterface,
  e: RenderInput<'ToolResult'>,
  cfg: () => KernelConfig,
) {
  const card = await readCard($, e.requestId)
  if (!card) return null
  const { Box, Text, Button, Link } = $.ui.resolve(e)
  const columns = e.viewport?.columns ?? 80

  let picture
  if (e.surface === 'terminal') {
    const { Image } = $.ui.resolve(e)
    const cols = Math.max(24, Math.min(columns - 6, 90))
    picture = card.png ? (
      <Image source={{ png: card.png }} columns={cols} rows={Math.round(cols * 0.36)} alt={`${card.title} 地图`} />
    ) : (
      ''
    )
  } else {
    const { Svg } = $.ui.resolve(e)
    picture = <Svg source={card.svg} width={Math.min(560, Math.max(280, columns * 7))} alt={`${card.title} 地图`} />
  }

  const meta = [...card.chips, card.crsLabel].filter(Boolean)

  return (
    <Box flexDirection="column" borderStyle="round" borderDimColor paddingX={1} marginTop={1}>
      <Box flexDirection="row" justifyContent="space-between">
        <Text bold wrap="truncate-end">
          <Text color="claude">{`${GLYPH[card.kind] ?? '▰'} `}</Text>
          {card.title}
        </Text>
        <Text dimColor size="footnote">
          {KIND_LABEL[card.tool] ?? 'GIS'}
        </Text>
      </Box>
      {meta.length > 0 ? (
        <Text dimColor size="footnote" wrap="truncate-end">
          {meta.join(' · ')}
        </Text>
      ) : (
        ''
      )}

      <Box marginTop={1} marginBottom={1}>
        {picture}
      </Box>

      {card.steps.length > 0 ? (
        <Box flexDirection="column">
          {card.steps.slice(0, 6).map((step, i) => (
            <Text size="footnote" wrap="truncate-end">
              <Text dimColor>{`${i + 1}. `}</Text>
              {step}
            </Text>
          ))}
        </Box>
      ) : (
        ''
      )}

      {card.warnings.length > 0 ? (
        <Box flexDirection="column" marginTop={card.steps.length > 0 ? 1 : 0}>
          {card.warnings.slice(0, 2).map(w => (
            <Text color="warning" size="footnote" wrap="truncate-end">{`⚠ ${w}`}</Text>
          ))}
          {card.warnings.length > 2 ? (
            <Text dimColor size="footnote">{`另有 ${card.warnings.length - 2} 条提示（ctrl+o 查看完整结果）`}</Text>
          ) : (
            ''
          )}
        </Box>
      ) : (
        ''
      )}

      <Box flexDirection="row" alignItems="center" gap={1} marginTop={1}>
        {card.artifactId ? (
          <Text dimColor size="footnote" mono>
            {card.artifactId}
          </Text>
        ) : (
          ''
        )}
        <Button
          key={`panel:${e.requestId}`}
          label="在面板中查看"
          onPress={ev => showInPanel($, cfg(), card.artifactId, card.title, ev.surface)}
        />
        {card.imagePath ? <Link href={fileUrl(card.imagePath)} label="打开原图" /> : ''}
      </Box>
    </Box>
  )
}
