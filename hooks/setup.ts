/**
 * 原生内核准备：/config 的 gismod.engine → CLAUDE_GIS_EXE → 仓库开发构建（native/build/...）
 * → 已下载的 ~/.claude/gismod/bin/<平台>/ → 从 GitHub Releases 下载。
 */

import { atom, read, update } from 'claude-code'
import type { EngineInterface } from 'claude-code'

import type { GeoEnv } from './types'

export type Engine = { kind: 'native'; exe: string }

const RELEASE_BASE = 'https://github.com/A350xt/Claude-GISmod/releases/latest/download'

const env = atom({ plugin: 'gismod', key: 'env' } as const, null, { shape: 'env-2' })

async function setStatus($: EngineInterface, value: GeoEnv): Promise<void> {
  await update($, env, () => value)
  $.ui.status(value.state === 'installing' ? `GIS · ${value.message}` : undefined)
}

/** 面板读取环境状态（读即订阅）。 */
export async function readEnv($: EngineInterface): Promise<GeoEnv | null> {
  return read($, env)
}

export const isWindows = (p: string) => /^[A-Za-z]:[\\/]/.test(p) || p.includes('\\')

async function home($: EngineInterface): Promise<string | null> {
  const h = (await $.env.get('USERPROFILE')) ?? (await $.env.get('HOME'))
  return h ? h.replace(/\\/g, '/') : null
}

async function run($: EngineInterface, argv: string[], what: string, timeoutMs = 600_000): Promise<void> {
  let ran
  try {
    ran = await $.process.run(argv, { timeoutMs })
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

// ================================================================ 原生内核

type Platform = { asset: string; exe: string; windows: boolean }

async function platform($: EngineInterface): Promise<Platform | null> {
  if (isWindows($.plugin.root)) return { asset: 'claude-gis-windows-x64', exe: 'claude-gis.exe', windows: true }
  try {
    const ran = await $.process.run(['uname', '-sm'], { timeoutMs: 10_000 })
    const [os, arch] = ran.stdout.trim().split(/\s+/)
    if (os === 'Darwin' && arch === 'arm64') return { asset: 'claude-gis-macos-arm64', exe: 'claude-gis', windows: false }
    if (os === 'Linux' && (arch === 'x86_64' || arch === 'amd64'))
      return { asset: 'claude-gis-linux-x64', exe: 'claude-gis', windows: false }
  } catch {
    // 没有 uname：当作不支持
  }
  return null
}

async function nativeWorks($: EngineInterface, exe: string): Promise<boolean> {
  try {
    const ran = await $.process.run([exe, '--version'], { timeoutMs: 20_000 })
    return ran.exitCode === 0 && ran.stdout.includes('claude-gis')
  } catch {
    return false
  }
}

async function nativeCandidates($: EngineInterface, configured: string, plat: Platform | null): Promise<string[]> {
  const list: string[] = []
  if (configured.trim()) list.push(configured.trim())
  const fromEnv = await $.env.get('CLAUDE_GIS_EXE')
  if (fromEnv?.trim()) list.push(fromEnv.trim())
  const root = $.plugin.root
  if (plat) {
    const builds = plat.windows
      ? [`${root}/native/build/windows/Release/${plat.exe}`]
      : [`${root}/native/build/linux/${plat.exe}`, `${root}/native/build/macos/${plat.exe}`]
    for (const p of builds) if (await $.fs.exists(p)) list.push(p)
    const h = await home($)
    if (h) {
      const managed = `${h}/.claude/gismod/bin/${plat.asset}/${plat.exe}`
      if (await $.fs.exists(managed)) list.push(managed)
    }
  }
  return list
}

/** 从 GitHub Releases 下载对应平台的包并解压到 ~/.claude/gismod/bin/<平台>/ */
async function downloadNative($: EngineInterface, plat: Platform): Promise<string> {
  const h = await home($)
  if (!h) throw new Error('找不到用户目录')
  const dir = `${h}/.claude/gismod/bin/${plat.asset}`
  const zip = `${h}/.claude/gismod/bin/${plat.asset}.zip`
  await setStatus($, { state: 'installing', message: '正在下载 GIS 原生内核（约 30–60 MB）…' })
  if (plat.windows) {
    await run($, ['cmd', '/c', 'mkdir', dir.replace(/\//g, '\\')], '创建目录', 20_000).catch(() => undefined)
  } else {
    await run($, ['mkdir', '-p', dir], '创建目录', 20_000)
  }
  await run($, ['curl', '-fsSL', '--retry', '2', '-o', zip, `${RELEASE_BASE}/${plat.asset}.zip`], '下载原生内核', 300_000)
  await setStatus($, { state: 'installing', message: '正在解压 GIS 原生内核…' })
  // Windows 10+ 自带 bsdtar（tar.exe）可解 zip；macOS 的 tar 也是 bsdtar；Linux 用 unzip
  const extract = plat.windows || !(await has($, ['unzip', '-v'])) ? ['tar', '-xf', zip, '-C', dir] : ['unzip', '-oq', zip, '-d', dir]
  await run($, extract, '解压原生内核', 120_000)
  const exe = `${dir}/${plat.exe}`
  if (!plat.windows) await run($, ['chmod', '+x', exe], '设置可执行权限', 10_000)
  if (!(await nativeWorks($, exe))) throw new Error('原生内核已下载，但无法运行')
  return exe
}

/** 不安装、不下载：只看本机是否已有能用的原生内核（session.start 时用来决定注册哪些工具）。 */
export async function detectNative($: EngineInterface, configured: string): Promise<string | null> {
  const plat = await platform($)
  for (const exe of await nativeCandidates($, configured, plat)) if (await nativeWorks($, exe)) return exe
  return null
}

// ================================================================ 入口

export type EngineConfig = { engine: string }

let ready: Promise<Engine> | null = null

/** 返回可用的原生内核；本机没有就从 GitHub Releases 下载。并发调用共享同一次准备。 */
export function ensureEngine($: EngineInterface, cfg: EngineConfig): Promise<Engine> {
  if (ready) return ready
  ready = (async () => {
    const plat = await platform($)
    for (const exe of await nativeCandidates($, cfg.engine, plat)) {
      if (await nativeWorks($, exe)) {
        await setStatus($, { state: 'ready', message: '原生内核就绪', engine: 'native', path: exe })
        return { kind: 'native', exe } as Engine
      }
    }
    try {
      if (!plat) throw new Error('当前平台没有预编译的原生内核（支持 Windows x64 / Linux x64 / macOS Apple Silicon）；请自行编译 native/ 并在 /config 指定路径')
      const exe = await downloadNative($, plat)
      await setStatus($, { state: 'ready', message: '原生内核已下载', engine: 'native', path: exe })
      $.ui.toast('GIS：原生内核已准备好')
      return { kind: 'native', exe } as Engine
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err)
      await setStatus($, { state: 'failed', message })
      throw err
    }
  })()
  ready.catch(() => {
    ready = null
  })
  return ready
}

/** 配置变化后重新查找。 */
export function resetEngine(): void {
  ready = null
}
