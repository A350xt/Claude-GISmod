import { expect, test } from 'claude-code/testing'

const PANE = {
  plugin: 'gismod',
  component: 'Pane',
  requestId: 'gis',
  props: {
    title: 'GIS',
    isFocused: false,
    bodyColumns: 60,
    placement: 'dock',
    scroll: { offset: 0, bodyRows: 40 },
    view: {},
  },
} as const

const ran = (stdout: string, exitCode = 0) => ({
  value: { exitCode, stdout, stderr: '', isStdoutTruncated: false, isStderrTruncated: false },
})
const envelope = (body: Record<string, unknown>) => ran(`${JSON.stringify({ ok: true, ...body })}\n`)

const INVENTORY = {
  datasets: [{ name: 'roads', path: '/w/roads.geojson', format: 'geojson', size_bytes: 20480, origin: 'workspace' }],
  artifacts: [
    {
      artifact_id: 'ar_1',
      kind: 'analysis',
      title: '物流选址',
      created: '2026-11-01T10:20:00',
      count: 7,
      geometry_types: ['Polygon'],
      summary: ['候选地块 7 个'],
      warnings: 0,
      steps: [{ id: 'a' }],
      images: [],
    },
  ],
}

const PREVIEW = (crs?: string) => ({
  kind: 'vector',
  svg: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"></svg>',
  png_base64: null,
  warnings: [],
  meta: {
    count: 7,
    geometry_types: { Polygon: 7 },
    crs: crs === 'EPSG:4326' ? 'EPSG:4326' : 'EPSG:32651',
    crs_label: crs === 'EPSG:4326' ? 'EPSG:4326 · WGS 84' : 'EPSG:32651 · WGS 84 / UTM zone 51N',
    crs_units: crs === 'EPSG:4326' ? '度' : 'metre',
    source_crs: 'EPSG:32651',
    bounds: [1, 2, 3, 4],
    fields: [{ name: 'id', dtype: 'int64' }],
  },
})

type Opts = { failWith?: string; installed?: boolean; downloadFails?: boolean; commands?: string[][]; registered?: string[] }

/** 站在引擎的位置回答 mod 的调用；process.run 模拟 claude-gis 原生内核与下载命令。 */
// eslint-disable-next-line @typescript-eslint/no-explicit-any
function engine(on: any, requests: Array<Record<string, unknown>>, opts: Opts = {}) {
  let present = opts.installed ?? true
  on('command.register', () => ({ value: { command: 'gis' } }))
  on('tool.register', (_: unknown, e: any) => {
    opts.registered?.push(e.name)
    return { value: { tool: `mcp__gismod__${e.name}` } }
  })
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('ui.status', () => ({ value: undefined }))
  on('ui.toast', () => ({ value: undefined }))
  on('session.start', (_: unknown, e: any) => ({ cwd: e.cwd }))
  on('env.get', (_: unknown, e: any) => ({ value: e.name === 'USERPROFILE' ? 'C:/Users/t' : undefined }))
  on('fs.exists', () => ({ value: present }))
  on('process.run', (_: unknown, e: any) => {
    const argv: string[] = e.argv ?? []
    opts.commands?.push(argv)
    if (argv[1] === '--version') return present ? ran('claude-gis 0.4.0 (GDAL 3.12)') : ran('', 1)
    if (argv[0] === 'curl') {
      if (opts.downloadFails) return ran('', 22)
      present = true
      return ran('')
    }
    if (['cmd', 'mkdir', 'uname', 'tar', 'unzip', 'chmod'].includes(argv[0]!)) return ran('')
    const req = JSON.parse(String(e.stdin ?? e.init?.stdin ?? '{}'))
    requests.push(req)
    if (opts.failWith)
      return ran(`${JSON.stringify({ ok: false, error: { code: opts.failWith, message: '输入不存在' } })}\n`, 2)
    if (req.action === 'list') return envelope({ result: INVENTORY })
    if (req.action === 'inspect') return envelope({ result: { count: 22, crs: 'EPSG:4326' }, card: PREVIEW(req._card?.crs) })
    if (req.action === 'analyze')
      return envelope({
        result: { artifact_id: 'ar_1', result: { count: 7 }, summary: ['缓冲区 2000 m', '叠加求交：7 个要素'], warnings: [] },
        card: PREVIEW(req._card?.crs),
      })
    if (req.action === 'preview') return envelope({ result: PREVIEW(req.crs) })
    return envelope({ result: {} })
  })
}

test('inspect 工具把请求交给原生内核，结果里不带卡片', async ($, on) => {
  const requests: Array<Record<string, unknown>> = []
  engine(on, requests)
  await $.session.start({ cwd: '/w', surface: 'desktop', isInteractive: true })

  const out = await $.tool.call({ tool: 'mcp__gismod__inspect', path: '/w/roads.geojson' })
  expect(String(out.result)).toContain('"count":22')
  expect(String(out.result)).not.toContain('<svg')
  expect(requests[0]).toEqual({ action: 'inspect', path: '/w/roads.geojson', _card: { crs: 'source', png: false } })
})

test('内核报错时工具拒绝并带上错误码', async ($, on) => {
  engine(on, [], { failWith: 'E_INPUT_MISSING' })
  await $.session.start({ cwd: '/w', surface: 'desktop', isInteractive: true })

  const out = await $.tool.call({ tool: 'mcp__gismod__inspect', path: '/nope.geojson' })
  expect(String(out.deny ?? out.text)).toContain('E_INPUT_MISSING')
})

test('分析结果在对话里画成卡片', async ($, on) => {
  engine(on, [])
  const written: string[] = []
  on('state.set', (_: unknown, e: any, next: any) => {
    if (e.key === 'cards' && e.id) written.push(e.id)
    return next(e)
  })
  await $.session.start({ cwd: '/w', surface: 'desktop', isInteractive: true })
  await $.tool.call({ tool: 'mcp__gismod__analyze', title: '物流选址', operations: [{ id: 'a', op: 'x' }] })
  expect(written.length).toBe(1)

  for (const surface of ['desktop', 'terminal'] as const) {
    const ui = await $.ui.mount({
      plugin: 'gismod',
      component: 'ToolResult',
      requestId: written[0]!,
      surface,
      props: { tool: 'mcp__gismod__analyze', tool_use_id: written[0]!, output: '{}', isErrored: false },
    })
    expect((await ui.find({ type: 'Text', text: /物流选址/ }))?.text).toContain('物流选址')
    expect(await ui.find({ type: 'Text', text: /叠加求交/ })).toBeDefined()
    expect(await ui.find({ key: `panel:${written[0]}` })).toBeDefined()
    await ui.unmount()
  }
})

test('本机没有原生内核时从 GitHub Releases 下载', async ($, on) => {
  const commands: string[][] = []
  engine(on, [], { installed: false, commands })
  await $.session.start({ cwd: '/w', surface: 'desktop', isInteractive: true })

  const out = await $.tool.call({ tool: 'mcp__gismod__inspect', path: '/w/roads.geojson' })
  expect(String(out.result)).toContain('"count":22')
  const curl = commands.find(c => c[0] === 'curl')!
  expect(curl.at(-1)).toBe('https://github.com/A350xt/Claude-GISmod/releases/latest/download/claude-gis-windows-x64.zip')
  expect(commands.some(c => c[0] === 'tar' && c.includes('C:/Users/t/.claude/gismod/bin/claude-gis-windows-x64'))).toBe(true)
})

test('下载失败时工具报 E_ENV_SETUP 并给出修复提示', async ($, on) => {
  engine(on, [], { installed: false, downloadFails: true })
  await $.session.start({ cwd: '/w', surface: 'desktop', isInteractive: true })

  const out = await $.tool.call({ tool: 'mcp__gismod__inspect', path: '/w/roads.geojson' })
  expect(String(out.deny ?? out.text)).toContain('E_ENV_SETUP')
})

test('面板：刷新后列出数据集，点选后显示预览', async ($, on) => {
  engine(on, [])
  await $.session.start({ cwd: '/w', surface: 'desktop', isInteractive: true })

  for (const surface of ['desktop', 'terminal'] as const) {
    const ui = await $.ui.mount({ ...PANE, surface })
    await ui.press({ key: 'refresh' })
    await ui.press({ key: 'tab-data' }) // 标签状态跨挂载保留
    expect(await ui.find({ key: 'ds:/w/roads.geojson' })).toBeDefined()

    await ui.press({ key: 'tab-results' })
    await ui.press({ key: 'ar:ar_1' })
    expect((await ui.find({ type: 'Text', text: /^7 要素$/ }))?.text).toBe('7 要素')
    expect((await ui.find({ type: 'Text', text: /^显示：/ }))?.text).toContain('UTM zone 51N')
    await ui.unmount()
  }
})

test('坐标系切换：重新请求预览并显示新坐标系', async ($, on) => {
  const requests: Array<Record<string, unknown>> = []
  engine(on, requests)
  await $.session.start({ cwd: '/w', surface: 'desktop', isInteractive: true })

  const ui = await $.ui.mount({ ...PANE, surface: 'desktop' })
  await ui.press({ key: 'refresh' })
  await ui.press({ key: 'tab-results' })
  await ui.press({ key: 'ar:ar_1' })
  await ui.select({ key: 'crs', value: 'EPSG:4326' })

  const last = requests.filter(r => r.action === 'preview').at(-1)
  expect(last).toEqual({ action: 'preview', source: 'ar_1', crs: 'EPSG:4326', png: false })
  const shown = (await ui.find({ type: 'Text', text: /^显示：/ }))?.text
  expect(shown).toContain('EPSG:4326')
  expect(shown).toContain('数据原为 EPSG:32651')
  await ui.unmount()
})

test('只注册 inspect 和 analyze，并直接调用本机已有的 claude-gis', async ($, on) => {
  const commands: string[][] = []
  const registered: string[] = []
  engine(on, [], { commands, registered })
  await $.session.start({ cwd: '/w', surface: 'desktop', isInteractive: true })
  expect(registered).toEqual(['inspect', 'analyze'])

  const out = await $.tool.call({ tool: 'mcp__gismod__inspect', path: '/w/roads.geojson' })
  expect(String(out.result)).toContain('"count":22')
  const kernelCall = commands.find(c => c[1] === 'run')!
  expect(kernelCall[0]).toContain('claude-gis')
  expect(commands.some(c => c[0] === 'curl')).toBe(false)
})
