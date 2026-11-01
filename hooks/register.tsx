/**
 * Claude-GISmod —— Claude Code mod 入口。
 *
 * - 三个 GIS 工具（mcp__gismod__inspect / analyze / visualize）交给 Python 内核执行；
 * - /gis 打开侧边面板：工作目录的数据集、分析结果与地图预览；
 * - 工具调用完成后自动刷新面板、选中新结果。
 */

import type { EngineInterface, Register, RenderSurface, ToolCallResult } from 'claude-code'

import { callKernel, describeError, type KernelConfig } from './kernel'
import { PANE_ID, renderPanel } from './panel'
import { logActivity, refreshInventory, selectSource, setTab } from './store'
import { TOOLS, resultText, type GeoTool } from './tools'

type Options = { python?: string; workdir?: string; timeoutSeconds?: number }

/** 一次会话里会变的东西；重载时 session.start 会重新填好。 */
type Session = {
  cwd: string
  surface: RenderSurface
  paneOpened: boolean
  cfg: () => KernelConfig
}

async function openPane($: EngineInterface, session: Session): Promise<void> {
  const opened = await $.ui.open({ id: PANE_ID, title: 'GIS' })
  session.paneOpened = opened.isPlaced
}

function titleOf(tool: GeoTool, input: Record<string, unknown>): string {
  if (typeof input.title === 'string' && input.title) return input.title
  if (typeof input.path === 'string') return input.path.split(/[\\/]/).pop() || tool.verb
  if (typeof input.source === 'string') return input.source
  return tool.verb
}

async function runTool(
  $: EngineInterface,
  call: Record<string, unknown>,
  tool: GeoTool,
  session: Session,
): Promise<ToolCallResult> {
  const { tool: _tool, tool_use_id, ...input } = call
  const id = String(tool_use_id)
  const title = titleOf(tool, input)
  const row = { id, tool: tool.spec.name, title: `${tool.verb} ${title}` }

  $.ui.status(`GIS · ${tool.verb}：${title}`)
  await logActivity($, { ...row, status: 'running', message: '' })
  if (!session.paneOpened && tool.action !== 'inspect') await openPane($, session)

  try {
    const result = await callKernel<Record<string, unknown>>($, session.cfg(), tool.action, input)
    const artifactId = typeof result.artifact_id === 'string' ? result.artifact_id : null
    const message = tool.action === 'inspect' ? `${result.count ?? '?'} 个要素` : (artifactId ?? '')
    await logActivity($, { ...row, status: 'ok', message })

    // 面板跟上：刷新清单并选中新结果（放到计时器里，不拖慢工具返回）
    if (artifactId) {
      $.clock.after(0, () => {
        void refreshInventory($, session.cfg())
          .then(() => setTab($, 'results'))
          .then(() => selectSource($, session.cfg(), artifactId, title, session.surface))
      })
    }

    if (tool.action === 'visualize' && typeof result.image_path === 'string') {
      return {
        result: resultText(result),
        context: [`地图已保存到 ${result.image_path}。用 Read 打开检查效果后再向用户汇报。`],
      }
    }
    return { result: resultText(result) }
  } catch (err) {
    const text = describeError(err)
    await logActivity($, { ...row, status: 'error', message: text.split('\n')[0]!.slice(0, 80) })
    return { deny: `GIS ${tool.spec.name} 失败 —— ${text}` }
  } finally {
    $.ui.status(undefined)
  }
}

export const register: Register = (on, options) => {
  const opts = options as Options
  const session: Session = {
    cwd: '.',
    surface: 'desktop',
    paneOpened: false,
    cfg: () => ({
      python: opts.python ?? '',
      workdir: (opts.workdir ?? '').trim() || session.cwd,
      timeoutMs: Math.min(600, Math.max(10, opts.timeoutSeconds ?? 300)) * 1000,
    }),
  }
  const [inspect, analyze, visualize] = TOOLS as [GeoTool, GeoTool, GeoTool]

  on('session.start', async ($, e, next) => {
    session.cwd = e.cwd
    session.surface = e.surface ?? session.surface
    for (const tool of TOOLS) {
      await $.tool.register(tool.spec)
    }
    await $.command.register({
      name: 'gis',
      description: '打开 GIS 面板：工作目录的空间数据、分析结果与地图预览',
    })
    return next(e)
  })

  on('command.run', { command: 'gis' }, async $ => {
    await openPane($, session)
    $.clock.after(0, () => void refreshInventory($, session.cfg()))
    return {
      text: session.paneOpened
        ? 'GIS 面板已打开。'
        : 'GIS 面板已就绪，当前界面暂时放不下侧边面板。',
    }
  })

  on('tool.call', { tool: 'mcp__gismod__inspect' }, ($, e) => runTool($, e, inspect, session))
  on('tool.call', { tool: 'mcp__gismod__analyze' }, ($, e) => runTool($, e, analyze, session))
  on('tool.call', { tool: 'mcp__gismod__visualize' }, ($, e) => runTool($, e, visualize, session))

  on('ui.render', { component: 'Pane', requestId: PANE_ID }, ($, e) => renderPanel($, e, session.cfg))
}
