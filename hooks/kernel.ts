/**
 * 内核桥：stdin 一条 JSON 请求，stdout 一条封套。
 *
 * 只用 $.process.run（参数数组、无 shell）调用 C++ 原生内核 claude-gis；
 * 内核由 setup.ts 找到或从 GitHub Releases 下载。
 */

import type { EngineInterface } from 'claude-code'

import { ensureEngine, type Engine } from './setup'

export type KernelAction =
  | 'inspect'
  | 'analyze'
  | 'show'
  | 'operations'
  | 'preview'
  | 'list'

export type KernelConfig = {
  /** /config 里设置的原生内核路径；空则自动 */
  engine: string
  /** 产物目录 artifacts/ 所在的工作目录 */
  workdir: string
  timeoutMs: number
}

export class KernelError extends Error {
  constructor(
    readonly code: string,
    message: string,
    readonly details: Record<string, unknown> = {},
  ) {
    super(message)
    this.name = 'KernelError'
  }
}

type Envelope = {
  ok: boolean
  result?: Record<string, unknown>
  card?: Record<string, unknown>
  error?: { code: string; message: string; details?: Record<string, unknown> }
}

function lastEnvelope(stdout: string): Envelope | null {
  const lines = stdout.split(/\r?\n/).map(l => l.trim()).filter(Boolean)
  for (let i = lines.length - 1; i >= 0; i--) {
    const line = lines[i]!
    if (!line.startsWith('{')) continue
    try {
      const parsed = JSON.parse(line) as Envelope
      if (parsed && typeof parsed === 'object' && 'ok' in parsed) return parsed
    } catch {
      // 某些库会打印含 { 的日志行，继续往前找
    }
  }
  return null
}

function hintsFor(raw: string): string[] {
  const low = raw.toLowerCase()
  const hints: string[] = []
  if (low.includes('proj.db')) {
    hints.push('PROJ 数据库版本冲突：系统的 PROJ_LIB/PROJ_DATA 指向了别的安装（如 PostGIS）')
  }
  if (/permission|being used by another process/.test(low)) {
    hints.push('文件被占用：关闭正在使用该数据的程序（如 QGIS）后重试')
  }
  return hints
}

/** 调用内核；card 给出时内核会顺带生成结果卡片（只给界面用）。 */
export async function invokeKernel(
  $: EngineInterface,
  cfg: KernelConfig,
  action: KernelAction,
  payload: Record<string, unknown>,
  card?: { crs: string; png: boolean },
): Promise<{ result: Record<string, unknown>; card: Record<string, unknown> | null }> {
  let engine: Engine
  try {
    engine = await ensureEngine($, { engine: cfg.engine })
  } catch (err) {
    throw new KernelError('E_ENV_SETUP', `GIS 内核不可用：${err instanceof Error ? err.message : String(err)}`, {
      hints: ['在 /config → gismod → 原生内核路径 指定 claude-gis，或检查网络后重试（首次需从 GitHub Releases 下载）'],
    })
  }
  const argv = [engine.exe, 'run', '--workdir', cfg.workdir]
  const label = engine.exe

  let ran
  try {
    ran = await $.process.run(argv, {
      cwd: cfg.workdir,
      env: {},
      stdin: JSON.stringify({ action, ...payload, ...(card ? { _card: card } : {}) }),
      timeoutMs: cfg.timeoutMs,
    })
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err)
    throw new KernelError('E_KERNEL_START', `无法运行 GIS 内核（${label}）：${msg}`, { hints: hintsFor(msg) })
  }
  if (ran.isStdoutTruncated) {
    throw new KernelError('E_KERNEL_OVERSIZED', '内核输出超过 4MB，已截断；请缩小分析范围')
  }
  const env = lastEnvelope(ran.stdout)
  if (!env) {
    throw new KernelError('E_KERNEL_BROKEN', `GIS 内核没有返回合法结果（exit=${ran.exitCode}）`, {
      stderr_tail: ran.stderr.slice(-1500),
      hints: hintsFor(ran.stdout + '\n' + ran.stderr),
    })
  }
  if (!env.ok || env.error) {
    const e = env.error ?? { code: 'E_INTERNAL', message: '内核返回了空错误' }
    throw new KernelError(e.code, e.message, e.details ?? {})
  }
  return { result: env.result ?? {}, card: env.card ?? null }
}

export async function callKernel<T = Record<string, unknown>>(
  $: EngineInterface,
  cfg: KernelConfig,
  action: KernelAction,
  payload: Record<string, unknown> = {},
): Promise<T> {
  return (await invokeKernel($, cfg, action, payload)).result as T
}

/** 给模型看的错误文本：错误码 + 信息 + 可修复细节（不含过长 traceback）。 */
export function describeError(err: unknown): string {
  if (err instanceof KernelError) {
    const details = { ...err.details }
    if (typeof details.traceback === 'string') {
      details.traceback = details.traceback.slice(-1200)
    }
    const extra = Object.keys(details).length ? `\n${JSON.stringify(details, null, 1)}` : ''
    return `${err.code}: ${err.message}${extra}`
  }
  return err instanceof Error ? err.message : String(err)
}
