import { expect, test } from 'claude-code/testing'

const PANE = {
  plugin: 'geocore',
  component: 'Pane',
  requestId: 'geocore',
  props: {
    title: 'GeoCore',
    isFocused: false,
    bodyColumns: 60,
    placement: 'dock',
    scroll: { offset: 0, bodyRows: 40 },
    view: {},
  },
} as const

const envelope = (result: unknown) => ({
  value: {
    exitCode: 0,
    stdout: `${JSON.stringify({ ok: true, result })}\n`,
    stderr: '',
    isStdoutTruncated: false,
    isStderrTruncated: false,
  },
})

const INVENTORY = {
  datasets: [
    { name: 'roads', path: '/w/roads.geojson', format: 'geojson', size_bytes: 20480, origin: 'workspace' },
  ],
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

function engine(on: Parameters<Parameters<typeof test>[1]>[1], requests: unknown[], failWith?: string) {
  on('command.register', () => ({ value: { command: 'geocore' } }))
  on('tool.register', (_, e) => ({ value: { tool: `mcp__geocore__${(e as { name: string }).name}` } }))
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('session.start', (_, e) => ({ cwd: e.cwd }))
  on('env.get', () => ({ value: undefined }))
  on('fs.exists', () => ({ value: false }))
  on('process.run', (_, e) => {
    const req = JSON.parse(String((e as { stdin?: string }).stdin ?? (e as { init?: { stdin?: string } }).init?.stdin ?? '{}'))
    requests.push(req)
    if (failWith)
      return {
        value: {
          exitCode: 2,
          stdout: `${JSON.stringify({ ok: false, error: { code: failWith, message: '输入不存在' } })}
`,
          stderr: '',
          isStdoutTruncated: false,
          isStderrTruncated: false,
        },
      }
    if (req.action === 'list') return envelope(INVENTORY)
    if (req.action === 'inspect') return envelope({ count: 22, crs: 'EPSG:4326' })
    if (req.action === 'analyze') return envelope({ artifact_id: 'ar_1', result: { count: 7 } })
    if (req.action === 'preview')
      return envelope({
        kind: 'vector',
        svg: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"></svg>',
        png_base64: null,
        warnings: [],
        meta: {
          count: 7,
          geometry_types: { Polygon: 7 },
          crs: req.crs === 'EPSG:4326' ? 'EPSG:4326' : 'EPSG:32651',
          crs_label: req.crs === 'EPSG:4326' ? 'EPSG:4326 · WGS 84' : 'EPSG:32651 · WGS 84 / UTM zone 51N',
          crs_units: req.crs === 'EPSG:4326' ? '度' : 'metre',
          source_crs: 'EPSG:32651',
          bounds: [1, 2, 3, 4],
          fields: [{ name: 'id', dtype: 'int64' }],
        },
      })
    return envelope({})
  })
}

test('inspect 工具把请求交给 Python 内核并返回结果', async ($, on) => {
  const requests: unknown[] = []
  engine(on, requests)
  await $.session.start({ cwd: '/w', surface: 'desktop', isInteractive: true })

  const ran = await $.tool.call({ tool: 'mcp__geocore__inspect', path: '/w/roads.geojson' })
  expect(String(ran.result)).toContain('"count":22')
  expect(requests[0]).toEqual({ action: 'inspect', path: '/w/roads.geojson' })
})

test('内核报错时工具拒绝并带上错误码', async ($, on) => {
  engine(on, [], 'E_INPUT_MISSING')
  await $.session.start({ cwd: '/w', surface: 'desktop', isInteractive: true })

  const ran = await $.tool.call({ tool: 'mcp__geocore__inspect', path: '/nope.geojson' })
  expect(String(ran.deny ?? ran.text)).toContain('E_INPUT_MISSING')
})

test('面板：刷新后列出数据集，点选后显示预览', async ($, on) => {
  engine(on, [])
  await $.session.start({ cwd: '/w', surface: 'desktop', isInteractive: true })

  for (const surface of ['desktop', 'terminal'] as const) {
    const ui = await $.ui.mount({ ...PANE, surface })
    await ui.press({ key: 'refresh' })
    await ui.press({ key: 'tab-data' }) // 标签状态跨挂载保留
    expect(await ui.find({ key: 'tab-data' })).toBeDefined()
    expect(await ui.find({ key: 'ds:/w/roads.geojson' })).toBeDefined()

    await ui.press({ key: 'tab-results' })
    await ui.press({ key: 'ar:ar_1' })
    expect((await ui.find({ type: 'Text', text: /^7 要素$/ }))?.text).toBe('7 要素')
    expect((await ui.find({ type: 'Text', text: /^显示：/ }))?.text).toContain('UTM zone 51N')
    expect(await ui.find({ key: 'ask-analyze' })).toBeDefined()
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
