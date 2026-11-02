/**
 * Python 环境：找到一个装好地理库的解释器；找不到就自动建一个。
 *
 * 查找顺序：/config 里设置的 → CLAUDE_GIS_PYTHON → mod 目录的 .venv（开发用）
 *          → 托管环境 ~/.claude/gismod/venv。
 * 都不可用时，在托管位置自动创建：有 uv 用 uv（快），没有就用系统 python -m venv + pip。
 * 同一时间只跑一次准备流程，结果缓存到本次加载结束。
 */

import { atom, read, update } from 'claude-code'
import type { EngineInterface } from 'claude-code'

import type { GeoEnv } from './types'

export const DEPENDENCIES = [
  'geopandas>=1.0', 'shapely>=2.0', 'pyproj>=3.6', 'pyogrio>=0.7', 'pandas>=2.0', 'numpy>=1.26',
  'matplotlib>=3.8', 'rasterio>=1.3', 'scipy>=1.11',
]

/** 只检查真正用到的模块，越快越好。 */
const PROBE = 'import geopandas, shapely, pyproj, matplotlib, rasterio, scipy; print("ok")'

const env = atom({ plugin: 'gismod', key: 'env' } as const, null, { shape: 'env-1' })

async function setStatus($: EngineInterface, value: GeoEnv): Promise<void> {
  await update($, env, () => value)
  $.ui.status(value.state === 'installing' ? `GIS · ${value.message}` : undefined)
}

/** 面板读取环境状态（读即订阅）。 */
export async function readEnv($: EngineInterface): Promise<GeoEnv | null> {
  return read($, env)
}

export const isWindows = (p: string) => /^[A-Za-z]:[\\/]/.test(p) || p.includes('\\')
const venvPython = (dir: string) => (isWindows(dir) ? `${dir}/Scripts/python.exe` : `${dir}/bin/python`)

export async function kernelEnv($: EngineInterface): Promise<Record<string, string>> {
  const root = $.plugin.root
  const sep = isWindows(root) ? ';' : ':'
  const existing = (await $.env.get('PYTHONPATH')) ?? ''
  const kernel = `${root}/kernel`
  return {
    PYTHONPATH: existing ? `${kernel}${sep}${existing}` : kernel,
    PYTHONIOENCODING: 'utf-8',
    PYTHONUTF8: '1',
  }
}

async function managedDir($: EngineInterface): Promise<string | null> {
  const home = (await $.env.get('USERPROFILE')) ?? (await $.env.get('HOME'))
  return home ? `${home.replace(/\\/g, '/')}/.claude/gismod/venv` : null
}

async function works($: EngineInterface, python: string): Promise<boolean> {
  try {
    const ran = await $.process.run([python, '-c', PROBE], { env: await kernelEnv($), timeoutMs: 60_000 })
    return ran.exitCode === 0 && ran.stdout.includes('ok')
  } catch {
    return false
  }
}

async function candidates($: EngineInterface, configured: string): Promise<string[]> {
  const list: string[] = []
  if (configured.trim()) list.push(configured.trim())
  const fromEnv = await $.env.get('CLAUDE_GIS_PYTHON')
  if (fromEnv?.trim()) list.push(fromEnv.trim())
  for (const dir of [`${$.plugin.root}/.venv`, await managedDir($)]) {
    if (dir && (await $.fs.exists(venvPython(dir)))) list.push(venvPython(dir))
  }
  return list
}

async function run($: EngineInterface, argv: string[], what: string): Promise<void> {
  let ran
  try {
    ran = await $.process.run(argv, { timeoutMs: 600_000 })
  } catch (err) {
    throw new Error(`${what}失败：${err instanceof Error ? err.message : String(err)}`)
  }
  if (ran.exitCode !== 0) {
    throw new Error(`${what}失败（exit ${ran.exitCode}）：${(ran.stderr || ran.stdout).slice(-800)}`)
  }
}

async function has($: EngineInterface, argv: string[]): Promise<boolean> {
  try {
    return (await $.process.run(argv, { timeoutMs: 20_000 })).exitCode === 0
  } catch {
    return false
  }
}

/** 在托管位置建环境；progress 收到每一步的中文说明。 */
async function install($: EngineInterface): Promise<string> {
  const dir = await managedDir($)
  if (!dir) throw new Error('找不到用户目录，无法自动创建 Python 环境')
  const python = venvPython(dir)

  if (await has($, ['uv', '--version'])) {
    await setStatus($, { state: 'installing', message: '正在用 uv 创建 Python 3.12 环境…' })
    await run($, ['uv', 'venv', dir, '--python', '3.12', '--allow-existing'], '创建环境')
    await setStatus($, { state: 'installing', message: '正在安装地理分析库（geopandas、rasterio 等），首次约 1–3 分钟…' })
    await run($, ['uv', 'pip', 'install', '--python', python, ...DEPENDENCIES], '安装依赖')
  } else {
    const base = isWindows(dir) ? 'python' : 'python3'
    if (!(await has($, [base, '--version']))) {
      throw new Error('没有找到 uv，也没有找到系统 Python。请先安装 uv（https://docs.astral.sh/uv/）后重试')
    }
    await setStatus($, { state: 'installing', message: '正在创建 Python 环境（未找到 uv，改用 venv + pip，会慢一些）…' })
    await run($, [base, '-m', 'venv', dir], '创建环境')
    await setStatus($, { state: 'installing', message: '正在安装地理分析库，首次可能需要几分钟…' })
    await run($, [python, '-m', 'pip', 'install', '--disable-pip-version-check', ...DEPENDENCIES], '安装依赖')
  }
  if (!(await works($, python))) throw new Error('环境已创建，但地理库仍无法导入')
  return python
}

let ready: Promise<string> | null = null

/**
 * 返回一个可用的解释器；必要时自动安装。并发调用共享同一次准备。
 * 进度写进状态 env（面板显示）与状态栏。
 */
export function ensurePython($: EngineInterface, configured: string): Promise<string> {
  if (ready) return ready
  ready = (async () => {
    for (const python of await candidates($, configured)) {
      if (await works($, python)) {
        await setStatus($, { state: 'ready', message: '环境就绪', python })
        return python
      }
    }
    try {
      const python = await install($)
      await setStatus($, { state: 'ready', message: 'Python 环境已自动准备好', python })
      $.ui.toast('GIS：Python 环境已自动准备好')
      return python
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err)
      await setStatus($, { state: 'failed', message })
      throw err
    }
  })()
  // 失败后允许下次重试
  ready.catch(() => {
    ready = null
  })
  return ready
}

/** 配置变化（/config 改了解释器）后重新查找。 */
export function resetPython(): void {
  ready = null
}
