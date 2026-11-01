/**
 * Python 内核桥：`python -X utf8 -m geocore run`，stdin 一条 JSON 请求，stdout 一条封套。
 *
 * 不依赖任何宿主框架：只用 $.process.run（参数数组、无 shell）。
 * 内核源码就在本 mod 的 src/ 下，通过 PYTHONPATH 指过去，无需 pip install 本包，
 * 只要解释器装了 geopandas 等依赖。
 */

import type { EngineInterface } from 'claude-code'

export type KernelAction =
  | 'inspect'
  | 'analyze'
  | 'visualize'
  | 'compose'
  | 'show'
  | 'operations'
  | 'preview'
  | 'list'

export type KernelConfig = {
  /** 解释器路径（options.python）；空则自动探测 */
  python: string
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
  error?: { code: string; message: string; details?: Record<string, unknown> }
}

const isWindowsPath = (p: string) => /^[A-Za-z]:[\\/]/.test(p) || p.includes('\\')

/** 解释器探测顺序：配置 → GEOCORE_PYTHON → mod 自带 .venv → python */
export async function resolvePython($: EngineInterface, configured: string): Promise<string> {
  if (configured.trim()) return configured.trim()
  const fromEnv = await $.env.get('GEOCORE_PYTHON')
  if (fromEnv?.trim()) return fromEnv.trim()
  const root = $.plugin.root
  const candidates = isWindowsPath(root)
    ? [`${root}/.venv/Scripts/python.exe`]
    : [`${root}/.venv/bin/python`]
  for (const c of candidates) {
    if (await $.fs.exists(c)) return c
  }
  return isWindowsPath(root) ? 'python' : 'python3'
}

async function kernelEnv($: EngineInterface): Promise<Record<string, string>> {
  const root = $.plugin.root
  const sep = isWindowsPath(root) ? ';' : ':'
  const existing = (await $.env.get('PYTHONPATH')) ?? ''
  return {
    PYTHONPATH: existing ? `${root}/src${sep}${existing}` : `${root}/src`,
    PYTHONIOENCODING: 'utf-8',
    PYTHONUTF8: '1',
  }
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

/** 从崩溃输出里提炼可操作的提示。 */
function hintsFor(raw: string): string[] {
  const low = raw.toLowerCase()
  const hints: string[] = []
  if (low.includes('no module named') || raw.includes('ModuleNotFoundError')) {
    hints.push('解释器缺少依赖：在 mod 目录执行 `uv venv .venv && uv pip install -e ".[viz,raster]"`，或在 /config 里把 geocore.python 指向已装 geopandas 的解释器')
  }
  if (low.includes('proj.db')) {
    hints.push('PROJ 数据库版本冲突：系统的 PROJ_LIB/PROJ_DATA 指向了别的安装（如 PostGIS）')
  }
  if (/permission|being used by another process/.test(low)) {
    hints.push('文件被占用：关闭正在使用该数据的程序（如 QGIS）后重试')
  }
  return hints
}

export async function callKernel<T = Record<string, unknown>>(
  $: EngineInterface,
  cfg: KernelConfig,
  action: KernelAction,
  payload: Record<string, unknown> = {},
): Promise<T> {
  const python = await resolvePython($, cfg.python)
  let ran
  try {
    ran = await $.process.run(
      [python, '-X', 'utf8', '-m', 'geocore', 'run', '--workdir', cfg.workdir],
      {
        cwd: cfg.workdir,
        env: await kernelEnv($),
        stdin: JSON.stringify({ action, ...payload }),
        timeoutMs: cfg.timeoutMs,
      },
    )
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err)
    throw new KernelError('E_KERNEL_START', `无法运行 Python 内核（${python}）：${msg}`, {
      hints: ['确认解释器路径正确；或在 /config 设置 geocore.python', ...hintsFor(msg)],
    })
  }
  if (ran.isStdoutTruncated) {
    throw new KernelError('E_KERNEL_OVERSIZED', '内核输出超过 4MB，已截断；请缩小分析范围')
  }
  const env = lastEnvelope(ran.stdout)
  if (!env) {
    const raw = ran.stdout + '\n' + ran.stderr
    throw new KernelError('E_KERNEL_BROKEN', `Python 内核没有返回合法结果（exit=${ran.exitCode}）`, {
      stderr_tail: ran.stderr.slice(-1500),
      hints: hintsFor(raw),
    })
  }
  if (!env.ok || env.error) {
    const e = env.error ?? { code: 'E_INTERNAL', message: '内核返回了空错误' }
    throw new KernelError(e.code, e.message, e.details ?? {})
  }
  return (env.result ?? {}) as T
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
