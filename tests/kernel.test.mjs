// 原生内核黑盒测试：只通过 claude-gis 的 stdin/stdout 协议与落盘产物验证行为。
//
//   CLAUDE_GIS_EXE=native/build/windows/Release/claude-gis.exe node --test tests/
//
// 不依赖 Python 或任何 npm 包：分析结果一律请求 GeoJSON 输出，直接按 JSON 读回。
import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { describe, test } from 'node:test'
import { fileURLToPath } from 'node:url'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const DATA = path.join(HERE, 'data', 'synthetic').replace(/\\/g, '/')
const EXE = process.env.CLAUDE_GIS_EXE ?? ''
const GT = JSON.parse(fs.readFileSync(path.join(DATA, 'ground_truth.json'), 'utf8'))
const UTM = 32651 // 上海附近 UTM 51N

const skip = EXE ? false : '未设置 CLAUDE_GIS_EXE（指向 claude-gis 可执行文件）'

// ---------------------------------------------------------------- 工具

function tmp() {
  return fs.mkdtempSync(path.join(os.tmpdir(), 'claude-gis-'))
}

/** 原样跑一条请求，返回 { env, code } */
function raw(input, workdir = tmp()) {
  const p = spawnSync(EXE, ['run', '--workdir', workdir], { input, encoding: 'utf8', maxBuffer: 256 << 20 })
  const lines = (p.stdout ?? '').trim().split(/\r?\n/).filter(l => l.startsWith('{'))
  assert.ok(lines.length, `内核没有输出封套（exit=${p.status}）：${p.stderr}`)
  return { env: JSON.parse(lines.at(-1)), code: p.status }
}

/** 成功则返回 result，失败抛出带 code 的错误 */
function call(action, payload, workdir = tmp()) {
  const { env } = raw(JSON.stringify({ action, ...payload }), workdir)
  if (!env.ok) {
    const err = new Error(`${env.error.code}: ${env.error.message}`)
    Object.assign(err, env.error)
    throw err
  }
  return env.result
}

function analyze(workdir, payload) {
  return call('analyze', { output: { format: 'geojson' }, ...payload }, workdir)
}

/** 单步算子：返回 { rows, features, result } */
function step(workdir, op, params, extra = {}) {
  const result = analyze(workdir, { title: op, operations: [{ id: 's1', op, allow_empty: true, ...params }], ...extra })
  return { ...readResult(result), result }
}

function readResult(result) {
  const fc = JSON.parse(fs.readFileSync(result.result.path, 'utf8'))
  return { features: fc.features, rows: fc.features.map(f => f.properties) }
}

function expectError(fn, code) {
  assert.throws(fn, err => {
    assert.equal(err.code, code, err.message)
    return true
  })
}

/** 写一份 GeoJSON（坐标系用 OGR 认得的 crs 成员） */
function writeGeoJSON(dir, name, features, epsg = 4326) {
  const fc = {
    type: 'FeatureCollection',
    ...(epsg === 4326 ? {} : { crs: { type: 'name', properties: { name: `urn:ogc:def:crs:EPSG::${epsg}` } } }),
    features: features.map(([geometry, properties]) => ({ type: 'Feature', properties, geometry })),
  }
  const p = path.join(dir, name).replace(/\\/g, '/')
  fs.writeFileSync(p, JSON.stringify(fc))
  return p
}

const box = (x0, y0, x1, y1) => ({ type: 'Polygon', coordinates: [[[x0, y0], [x1, y0], [x1, y1], [x0, y1], [x0, y0]]] })
const point = (x, y) => ({ type: 'Point', coordinates: [x, y] })
const line = (...pts) => ({ type: 'LineString', coordinates: pts })
const approx = (got, want, tol, msg) => assert.ok(Math.abs(got - want) <= tol, `${msg ?? ''} got ${got}, want ${want}±${tol}`)

// 两个 1×1 km 区 + 8 个点 + 一条横穿两区的线（投影坐标）
function twoRegions(dir) {
  const regions = writeGeoJSON(dir, 'regions.geojson', [[box(0, 0, 1000, 1000), { rname: 'R左' }], [box(1000, 0, 2000, 1000), { rname: 'R右' }]], UTM)
  const pts = writeGeoJSON(
    dir,
    'pts.geojson',
    [[100, 100], [900, 900], [500, 500], [100, 800], [1100, 100], [1900, 900], [1500, 500], [1950, 100]].map(([x, y], i) => [
      point(x + 5, y + 5),
      { pid: i, pop: (i + 1) * 10 },
    ]),
    UTM,
  )
  const ln = writeGeoJSON(dir, 'line.geojson', [[line([0, 500], [2000, 500]), { lid: 'L1', weight: 2.0 }]], UTM)
  return { regions, pts, line: ln }
}

// ================================================================ 基准题

describe('起始基准题（合成城市「临江市」）', { skip }, () => {
  const tasks = fs
    .readFileSync(path.join(HERE, 'bench', 'starter.jsonl'), 'utf8')
    .split(/\r?\n/)
    .filter(l => l.trim() && !l.startsWith('//'))
    .map(l => JSON.parse(l))
  const lookup = { ...GT.counts_from_construction, ...GT.aggregates }

  for (const task of tasks) {
    test(task.id, () => {
      const ops = JSON.parse(JSON.stringify(task.operations).replaceAll('{DATA}', DATA))
      const result = analyze(tmp(), { title: task.id, operations: ops })
      const exp = task.expect
      const count = result.result.count
      const { rows, features } = readResult(result)
      const colSum = col => rows.reduce((s, r) => s + (Number(r[col]) || 0), 0)

      if ('count_equals' in exp) assert.equal(count, exp.count_equals)
      if ('count_equals_field' in exp) {
        const [key, sub] = exp.count_equals_field
        assert.equal(count, sub == null ? lookup[key] : lookup[key][sub])
      }
      if ('count_equals_field_flat' in exp) assert.equal(count, GT.counts_from_construction[exp.count_equals_field_flat[0]])
      if ('count_between' in exp) assert.ok(count >= exp.count_between[0] && count <= exp.count_between[1], `count=${count}`)
      if ('min_count' in exp) assert.ok(count >= exp.min_count)
      if ('max_count' in exp) assert.ok(count <= exp.max_count)
      if ('has_columns' in exp) for (const c of exp.has_columns) assert.ok(rows.length && c in rows[0], `缺列 ${c}`)
      if ('col_sum_equals' in exp) approx(colSum(exp.col_sum_equals[0]), exp.col_sum_equals[1], 1e-6)
      if ('col_sum_equals_agg' in exp) approx(colSum(exp.col_sum_equals_agg[0]), GT.aggregates[exp.col_sum_equals_agg[1]], 1e-6)
      if ('col_sum_within_pct_of_agg' in exp) {
        const [col, scope, key, minPct] = exp.col_sum_within_pct_of_agg
        const full = (scope === 'counts' ? GT.counts_from_construction : GT.aggregates)[key]
        const got = colSum(col)
        assert.ok(got <= full * 1.0001, `${col} 合计 ${got} 超过全量 ${full}`)
        assert.ok(got >= (full * minPct) / 100, `${col} 覆盖率不足 ${got}/${full}`)
      }
      if ('disjoint_ref' in exp) {
        // 用内核自己验证：结果与参照层的重叠面积应为 0（共享边界不算重叠）
        const wd = path.dirname(path.dirname(path.dirname(result.result.path)))
        const ref = exp.disjoint_ref.replace('{DATA}', DATA)
        const check = analyze(wd, {
          operations: [
            { id: 'x', op: 'overlay.intersection', a: result.artifact_id, b: ref, allow_empty: true },
            { id: 'm', op: 'query.measure', input: '@x', measures: ['area_sqkm'], allow_empty: true },
          ],
        })
        // 输入已有 area_sqkm 列时，度量写到 area_sqkm_calc
        const sqkm = readResult(check).rows.reduce((s2, r) => s2 + (r.area_sqkm_calc ?? r.area_sqkm), 0)
        assert.ok(sqkm < 1e-6, `结果与参照层仍重叠 ${sqkm} km²`)
      }
      if ('summary_contains_any_km' in exp) {
        const m = result.summary.join('').match(/([\d.,]+)\s*km/)
        const km = m ? Number(m[1].replace(/,/g, '')) : NaN
        assert.ok(km > exp.summary_contains_any_km[0] && km < exp.summary_contains_any_km[1], `km=${km}`)
      }
      if (exp.projected_analysis) assert.match(result.crs.analysis, /^EPSG:32[67]\d\d$/)
      assert.equal(features.length, count)
    })
  }
})

// ================================================================ 协议与错误

describe('协议与错误码', { skip }, () => {
  test('inspect 返回几何、坐标系、字段与质量', () => {
    const r = call('inspect', { path: `${DATA}/parcels.geojson` })
    assert.equal(r.kind, 'vector')
    assert.equal(r.count, 40)
    assert.equal(r.crs, 'EPSG:4326')
    assert.ok(r.fields.some(f => f.name === 'landuse'))
    assert.equal(r.quality.invalid_geometries, 0)
  })

  test('非法 JSON → E_BAD_REQUEST 封套，退出码 2', () => {
    const { env, code } = raw('{not json')
    assert.equal(env.ok, false)
    assert.equal(env.error.code, 'E_BAD_REQUEST')
    assert.equal(code, 2)
  })

  test('未知 action 列出可用动作', () => {
    const { env } = raw(JSON.stringify({ action: 'visualize' }))
    assert.equal(env.error.code, 'E_BAD_REQUEST')
    assert.ok(env.error.details.available.includes('analyze'))
  })

  test('未知算子 → E_OP_UNKNOWN 并列出词表', () => {
    expectError(() => analyze(tmp(), { operations: [{ id: 'a', op: 'query.nope', input: `${DATA}/roads.geojson` }] }), 'E_OP_UNKNOWN')
    const ops = call('operations', {}).operations
    assert.ok(ops.includes('raster.cost_distance') && ops.includes('zonal.summarize'))
  })

  test('输入不存在 → E_INPUT_MISSING', () => {
    expectError(() => analyze(tmp(), { operations: [{ id: 'a', op: 'query.measure', input: `${DATA}/nope.geojson` }] }), 'E_INPUT_MISSING')
  })

  test('空结果默认报错，allow_empty 时继续', () => {
    const op = { id: 'f', op: 'query.filter', input: `${DATA}/roads.geojson`, where: "`class` == '不存在'" }
    expectError(() => analyze(tmp(), { operations: [op] }), 'E_EMPTY_RESULT')
    const ok = analyze(tmp(), { operations: [{ ...op, allow_empty: true }] })
    assert.equal(ok.result.count, 0)
  })

  test('坏 where 是领域错误而不是崩溃', () => {
    expectError(() => step(tmp(), 'query.filter', { input: `${DATA}/roads.geojson`, where: 'class ==' }), 'E_BAD_REQUEST')
    expectError(() => step(tmp(), 'query.filter', { input: `${DATA}/roads.geojson`, where: "__import__('os').system('x')" }), 'E_BAD_REQUEST')
  })

  test('show 返回产物元数据；损坏的 meta 不让清单失败', () => {
    const wd = tmp()
    const r = analyze(wd, { operations: [{ id: 'a', op: 'query.measure', input: `${DATA}/districts.geojson` }] })
    assert.equal(call('show', { artifact_id: r.artifact_id }, wd).artifact_id, r.artifact_id)
    fs.mkdirSync(path.join(wd, 'artifacts', 'ar_20240101000000zzzz'))
    fs.writeFileSync(path.join(wd, 'artifacts', 'ar_20240101000000zzzz', 'meta.json'), '{broken')
    const inv = call('list', {}, wd)
    assert.equal(inv.skipped_corrupt, 1)
    assert.equal(inv.artifacts.length, 1)
  })
})

// ================================================================ 产物与引用

describe('中间步骤与跨调用引用', { skip }, () => {
  const SITE = [
    { id: 'a', op: 'query.filter', input: `${DATA}/roads.geojson`, where: "`class` == '主干道'" },
    { id: 'b', op: 'proximity.buffer', input: '@a', distance_m: 2000, dissolve: true },
    { id: 'c', op: 'overlay.intersection', a: `${DATA}/parcels.geojson`, b: '@b' },
  ]

  test('gpkg 产物里保存每一步，可用 ar_xxx#步骤 再次引用', () => {
    const wd = tmp()
    const first = call('analyze', { title: 't', operations: SITE }, wd)
    assert.equal(first.result.format, 'gpkg')
    assert.deepEqual(Object.keys(first.intermediate_layers).sort(), ['a', 'b', 'c'])
    const second = analyze(wd, { operations: [{ id: 'm', op: 'query.measure', input: `${first.artifact_id}#a`, measures: ['length_km'] }] })
    assert.equal(second.result.count, 4)
  })

  test('不存在的中间步骤 → E_INPUT_MISSING 并列出可用步骤', () => {
    const wd = tmp()
    const first = call('analyze', { operations: SITE }, wd)
    try {
      analyze(wd, { operations: [{ id: 'm', op: 'query.measure', input: `${first.artifact_id}#nope` }] })
      assert.fail('应当报错')
    } catch (err) {
      assert.equal(err.code, 'E_INPUT_MISSING')
      assert.ok(err.details.available_steps.includes('a'))
    }
  })
})

// ================================================================ 坐标系策略

describe('坐标系策略', { skip }, () => {
  test('纯过滤保持源坐标系', () => {
    const r = step(tmp(), 'query.filter', { input: `${DATA}/roads.geojson`, where: "`class` == '高速'" }).result
    assert.equal(r.crs.analysis, 'EPSG:4326')
  })

  test('度量类操作统一到 UTM 并留痕', () => {
    const r = step(tmp(), 'proximity.buffer', { input: `${DATA}/hospitals.geojson`, distance_m: 500 }).result
    assert.equal(r.crs.analysis, `EPSG:${UTM}`)
    assert.ok(r.warnings.some(w => w.includes('统一坐标系')))
  })

  test('显式 crs 覆盖；非法 crs 报错', () => {
    const r = step(tmp(), 'proximity.buffer', { input: `${DATA}/hospitals.geojson`, distance_m: 500 }, { crs: 'EPSG:3857' }).result
    assert.equal(r.crs.analysis, 'EPSG:3857')
    expectError(() => step(tmp(), 'query.measure', { input: `${DATA}/districts.geojson` }, { crs: 'EPSG:abc' }), 'E_BAD_REQUEST')
  })

  test('缺坐标系且不在经纬度值域 → E_CRS_AMBIGUOUS', () => {
    const dir = tmp()
    const csv = path.join(dir, 'bad.csv').replace(/\\/g, '/')
    fs.writeFileSync(csv, 'lon,lat,v\n500000,3400000,1\n')
    expectError(() => call('inspect', { path: csv }), 'E_CRS_AMBIGUOUS')
  })
})

// ================================================================ 矢量算子

describe('查询与度量', { skip }, () => {
  test('属性过滤：工业地块 7 块', () => {
    const { rows, result } = step(tmp(), 'query.filter', { input: `${DATA}/parcels.geojson`, where: "`landuse` == '工业'" })
    assert.equal(rows.length, GT.aggregates.parcels_by_landuse['工业'])
    assert.ok(result.summary[0].includes('7 条命中'))
  })

  test('关键字列名用反引号；in / and / 数值比较', () => {
    const { rows } = step(tmp(), 'query.filter', { input: `${DATA}/roads.geojson`, where: "`class` == '高速'" })
    assert.deepEqual(rows.map(r => r.road_id).sort(), ['R_H1', 'R_H2'])
    const many = step(tmp(), 'query.filter', { input: `${DATA}/parcels.geojson`, where: "landuse in ['商业', '工业'] and not (landuse == '绿地')" })
    assert.equal(many.rows.length, 17)
    const dense = step(tmp(), 'query.filter', { input: `${DATA}/parcels.geojson`, where: 'pop_density > 20000' })
    assert.ok(dense.rows.every(r => r.pop_density > 20000))
  })

  test('投影坐标下 1 km² 精确；经纬度方块不掉进度数陷阱', () => {
    const dir = tmp()
    const sq = writeGeoJSON(dir, 'sq.geojson', [[box(0, 0, 1000, 1000), { name: 'sq1' }]], UTM)
    const { rows, result } = step(dir, 'query.measure', { input: sq, measures: ['area_sqkm', 'area_ha'] })
    approx(rows[0].area_sqkm, 1.0, 1e-6)
    approx(rows[0].area_ha, 100.0, 1e-4)
    assert.ok(result.summary[0].includes('总面积 1.000 km²'))
    const deg = writeGeoJSON(dir, 'deg.geojson', [[box(121.47, 31.23, 121.48, 31.24), { name: 'd' }]])
    const d = step(dir, 'query.measure', { input: deg, measures: ['area_sqkm'] })
    assert.ok(d.rows[0].area_sqkm > 0.8 && d.rows[0].area_sqkm < 1.3)
  })

  test('分组度量明细进 steps[].table', () => {
    const { result } = step(tmp(), 'query.measure', { input: `${DATA}/parcels.geojson`, measures: ['count', 'area_sqkm'], group_by: 'landuse' })
    const table = result.steps[0].table
    assert.deepEqual(table.columns.slice(0, 2), ['landuse', 'count'])
    const byUse = Object.fromEntries(table.rows.map(r => [r[0], r[1]]))
    assert.deepEqual(byUse, GT.aggregates.parcels_by_landuse)
  })

  test('空间选择：左半区 4 个点', () => {
    const dir = tmp()
    const t = twoRegions(dir)
    const left = writeGeoJSON(dir, 'left.geojson', [[box(0, 0, 1000, 1000), { rname: 'R左' }]], UTM)
    assert.equal(step(dir, 'query.select', { input: t.pts, predicate: 'intersects', ref: left }).rows.length, 4)
  })
})

describe('邻近', { skip }, () => {
  test('缓冲 100 m：面积 = 1 km² + 周边带 + 四个圆角', () => {
    const dir = tmp()
    const sq = writeGeoJSON(dir, 'sq.geojson', [[box(0, 0, 1000, 1000), { name: 'sq1' }]], UTM)
    const { rows, result } = step(dir, 'proximity.buffer', { input: sq, distance_m: 100 })
    const m = analyze(dir, { operations: [{ id: 'm', op: 'query.measure', input: result.artifact_id, measures: ['area_sqkm'] }] })
    approx(readResult(m).rows[0].area_sqkm * 1e6, 1e6 + 4 * 100 * 1000 + Math.PI * 100 * 100, 300)
    assert.equal(rows[0].buffer_distance_m, 100)
  })

  test('融合缓冲只剩 1 个面', () => {
    const dir = tmp()
    assert.equal(step(dir, 'proximity.buffer', { input: twoRegions(dir).pts, distance_m: 300, dissolve: true }).rows.length, 1)
  })

  test('within_distance 选对一侧', () => {
    const dir = tmp()
    const pts = writeGeoJSON(dir, 'p.geojson', [[point(50, 0), { pid: '近' }], [point(5000, 0), { pid: '远' }]], UTM)
    const ref = writeGeoJSON(dir, 'r.geojson', [[point(0, 0), { id: 1 }]], UTM)
    assert.deepEqual(step(dir, 'proximity.within_distance', { input: pts, ref, distance_m: 100 }).rows.map(r => r.pid), ['近'])
  })

  test('nearest k=2 按距离排序并带出参照属性', () => {
    const dir = tmp()
    const v = writeGeoJSON(dir, 'v.geojson', [[point(0, 0), { vid: 'V' }]], UTM)
    const h = writeGeoJSON(dir, 'h.geojson', [[point(300, 0), { name: '远院' }], [point(100, 0), { name: '近院' }], [point(900, 0), { name: '最远' }]], UTM)
    const rows = step(dir, 'proximity.nearest', { input: v, ref: h, k: 2, join_attrs: ['name'] }).rows.sort((a, b) => a.nearest_rank - b.nearest_rank)
    assert.equal(rows.length, 2)
    assert.equal(rows[0].nearest_name, '近院')
    approx(rows[0].nearest_distance_m, 100, 1e-6)
    assert.equal(rows[1].nearest_name, '远院')
  })
})

describe('叠加（精确面积）', { skip }, () => {
  // A = 2×1 km；B = 1×1 km，与 A 重叠 1000×750
  function pair() {
    const dir = tmp()
    return {
      dir,
      a: writeGeoJSON(dir, 'a.geojson', [[box(0, 0, 2000, 1000), { tag: 'A' }]], UTM),
      b: writeGeoJSON(dir, 'b.geojson', [[box(500, 250, 1500, 1250), { tag: 'B' }]], UTM),
    }
  }
  const area = (dir, artifact) =>
    readResult(analyze(dir, { operations: [{ id: 'm', op: 'query.measure', input: artifact, measures: ['area_sqkm'] }] })).rows.reduce(
      (s, r) => s + r.area_sqkm,
      0,
    )

  test('求交 0.75 km²，同名列改为 _a / _b', () => {
    const { dir, a, b } = pair()
    const r = step(dir, 'overlay.intersection', { a, b })
    approx(area(dir, r.result.artifact_id), 0.75, 1e-9)
    assert.equal(r.rows[0].tag_a, 'A')
    assert.equal(r.rows[0].tag_b, 'B')
  })

  test('求差 1.25 km²，只保留 A 的属性', () => {
    const { dir, a, b } = pair()
    const r = step(dir, 'overlay.difference', { a, b })
    approx(area(dir, r.result.artifact_id), 1.25, 1e-9)
    assert.deepEqual(Object.keys(r.rows[0]), ['tag'])
  })

  test('合并 2.25 km²', () => {
    const { dir, a, b } = pair()
    approx(area(dir, step(dir, 'overlay.union', { a, b }).result.artifact_id), 2.25, 1e-9)
  })

  test('裁剪只保留 A 的属性', () => {
    const { dir, a, b } = pair()
    const r = step(dir, 'overlay.clip', { a, b })
    approx(area(dir, r.result.artifact_id), 0.75, 1e-9)
    assert.ok('tag' in r.rows[0] && !('tag_b' in r.rows[0]))
  })
})

describe('分区统计', { skip }, () => {
  test('点：计数与字段求和', () => {
    const dir = tmp()
    const t = twoRegions(dir)
    const rows = step(dir, 'zonal.summarize', { regions: t.regions, data: t.pts, stats: ['count', 'sum'], field: 'pop' }).rows
    const by = Object.fromEntries(rows.map(r => [r.rname, r]))
    assert.equal(by['R左'].data_count, 4)
    assert.equal(by['R右'].data_count, 4)
    approx(by['R左'].data_pop_sum, 100, 1e-9)
    approx(rows.reduce((s, r) => s + r.data_pop_sum, 0), 360, 1e-9)
  })

  test('线：长度按区切分、占比合计 100%', () => {
    const dir = tmp()
    const t = twoRegions(dir)
    const rows = step(dir, 'zonal.summarize', { regions: t.regions, data: t.line, stats: ['total_length_km', 'share_pct'] }).rows
    for (const r of rows) approx(r.data_length_km, 1.0, 2e-3)
    approx(rows.reduce((s, r) => s + r.data_length_share_pct, 0), 100, 1e-6)
  })

  test('面：一半落入区内，按面积分摊精确为一半', () => {
    const dir = tmp()
    const region = writeGeoJSON(dir, 'region.geojson', [[box(0, 0, 1000, 1000), { rname: 'R' }]], UTM)
    const parcel = writeGeoJSON(dir, 'parcel.geojson', [[box(500, 0, 1500, 1000), { value: 200.0 }]], UTM)
    const [row] = step(dir, 'zonal.summarize', { regions: region, data: parcel, stats: ['count', 'total_area_sqkm', 'share_pct', 'sum'], field: 'value' }).rows
    approx(row.data_area_sqkm, 0.5, 1e-9)
    approx(row.data_area_share_pct, 50, 1e-9)
    approx(row.data_value_sum_apportioned, 100, 1e-9)
  })

  test('sum 不给 field / 字段不存在 → 报错', () => {
    const dir = tmp()
    const t = twoRegions(dir)
    expectError(() => step(dir, 'zonal.summarize', { regions: t.regions, data: t.pts, stats: ['sum'] }), 'E_BAD_REQUEST')
    expectError(() => step(dir, 'zonal.summarize', { regions: t.regions, data: t.pts, stats: ['sum'], field: 'nope' }), 'E_INPUT_MISSING')
  })
})

// ================================================================ 时间、中文

describe('时间字段与中文', { skip }, () => {
  function trackCsv(dir) {
    const p = path.join(dir, '台风轨迹.csv').replace(/\\/g, '/')
    const rows = ['name,lon,lat,time']
    const t0 = Date.UTC(2024, 8, 1, 0, 0, 0)
    for (const [name, base] of [['A', 120.0], ['B', 125.0]])
      for (let i = 0; i < 5; i++) rows.push(`${name},${base + i * 0.2},${20 + i * 0.1},${new Date(t0 + i * 6 * 3600e3).toISOString().slice(0, 19)}`)
    fs.writeFileSync(p, rows.join('\n'))
    return p
  }

  test('CSV 时间列自动识别，可直接与 ISO 文本比较', () => {
    const dir = tmp()
    const csv = trackCsv(dir)
    const ins = call('inspect', { path: csv })
    assert.equal(ins.fields.find(f => f.name === 'time').dtype.startsWith('datetime'), true)
    assert.match(ins.fields.find(f => f.name === 'time').samples[0], /^2024-09-01T/)
    const later = step(dir, 'query.filter', { input: csv, where: 'time >= "2024-09-01T12:00:00"' })
    assert.equal(later.rows.length, 6)
  })

  test('轨迹构建：按分组连线并给出起止时间', () => {
    const dir = tmp()
    const r = step(dir, 'trajectory.build', { input: trackCsv(dir), time_field: 'time', group_by: 'name' })
    assert.equal(r.rows.length, 2)
    assert.equal(r.rows[0].point_count, 5)
    approx(r.rows[0].duration_h, 24, 1e-9)
    assert.equal(r.features[0].geometry.type, 'LineString')
  })

  test('中文路径、中文列名、中文标题全程 UTF-8', () => {
    const dir = tmp()
    const p = writeGeoJSON(dir, '医疗设施.geojson', [[point(121.5, 31.2), { 名称: '第一医院', 类型: '医院' }], [point(121.6, 31.3), { 名称: '诊所', 类型: '诊所' }]])
    const r = analyze(dir, { title: '中文标题·端到端', operations: [{ id: 'f', op: 'query.filter', input: p, where: "类型 == '医院'" }] })
    assert.equal(r.title, '中文标题·端到端')
    assert.equal(readResult(r).rows[0]['名称'], '第一医院')
  })
})

// ================================================================ 栅格

describe('栅格算子', { skip }, () => {
  function squareDir() {
    const dir = tmp()
    const sq = writeGeoJSON(dir, 'sq.geojson', [[box(0, 0, 1000, 1000), { v: 7 }]], UTM)
    const pts = writeGeoJSON(dir, 'pts.geojson', [[point(100, 100), { id: 1 }], [point(900, 900), { id: 2 }]], UTM)
    return { dir, sq, pts }
  }

  test('矢量栅格化 → info：像元数与统计', () => {
    const { dir, sq } = squareDir()
    const r = call('analyze', { operations: [{ id: 'r', op: 'raster.from_vector', input: sq, cellsize: 100, field: 'v' }, { id: 'i', op: 'raster.info', input: '@r' }] }, dir)
    assert.equal(r.result.format, 'tif')
    assert.equal(r.steps[0].count, 100)
    const q = Object.fromEntries(r.steps[1].table.rows)
    assert.equal(q.min, 7)
    assert.equal(q.max, 7)
  })

  test('距离场：源处为 0、单位为米；截断为 NoData', () => {
    const { dir, pts } = squareDir()
    const r = call('analyze', { operations: [{ id: 'd', op: 'raster.distance', input: pts, cellsize: 10, max_distance_m: 200 }, { id: 'i', op: 'raster.info', input: '@d' }] }, dir)
    const q = Object.fromEntries(r.steps[1].table.rows)
    assert.equal(q.min, 0)
    assert.ok(q.max <= 200)
  })

  test('成本距离与最短路径', () => {
    const { dir, sq, pts } = squareDir()
    const r = call(
      'analyze',
      {
        output: { format: 'geojson' },
        operations: [
          { id: 'c', op: 'raster.from_vector', input: sq, cellsize: 50 },
          { id: 'src', op: 'query.filter', input: pts, where: 'id == 1' },
          { id: 'dst', op: 'query.filter', input: pts, where: 'id == 2' },
          { id: 'cd', op: 'raster.cost_distance', cost: '@c', source: '@src' },
          { id: 'path', op: 'raster.cost_path', cost: '@c', source: '@src', to: '@dst' },
        ],
      },
      dir,
    )
    const [row] = readResult(r).rows
    approx(row.total_cost, Math.SQRT2 * 800, 60)
    assert.ok(row.n_cells >= 15)
  })

  test('条件、计算、重分类、断点、直方图、加权叠加、邻域', () => {
    const { dir, sq, pts } = squareDir()
    const r = call(
      'analyze',
      {
        operations: [
          { id: 'd', op: 'raster.distance', input: pts, cellsize: 20 },
          { id: 'con', op: 'raster.con', input: '@d', condition: 'value >= 300', true: 1, false: 0 },
          { id: 'calc', op: 'raster.calc', inputs: { a: '@d', b: '@con' }, expression: 'a * b + sqrt(4)' },
          { id: 'br', op: 'raster.breaks', input: '@d', classes: 4, method: 'quantile' },
          { id: 'cls', op: 'raster.reclassify', input: '@d', mapping: [[0, 200, 1], [200, 100000, 2]] },
          { id: 'h', op: 'raster.histogram', input: '@d', bins: 10 },
          { id: 'w', op: 'raster.weighted_sum', inputs: [{ ref: '@con', weight: 3 }, { ref: '@cls', weight: 1 }] },
          { id: 'f', op: 'raster.focal', input: '@d', stat: 'mean', kernel_size: 3 },
          { id: 'z', op: 'raster.zonal_stats', regions: sq, data: '@cls', stats: ['count', 'majority'] },
        ],
      },
      dir,
    )
    const byId = Object.fromEntries(r.steps.map(s => [s.id, s]))
    assert.equal(byId.br.table.rows.length, 4)
    assert.equal(byId.h.table.rows.at(-1)[3], 100)
    assert.ok(byId.calc.count > 0 && byId.w.count > 0 && byId.f.count > 0)
    assert.equal(byId.z.count, 1)
  })

  test('转面（同值溶解）与等值线', () => {
    const { dir, pts } = squareDir()
    const r = call(
      'analyze',
      {
        operations: [
          { id: 'd', op: 'raster.distance', input: pts, cellsize: 20 },
          { id: 'cls', op: 'raster.reclassify', input: '@d', mapping: [[0, 200, 1], [200, 100000, 2]] },
          { id: 'p', op: 'raster.polygonize', input: '@cls', field: 'level' },
          { id: 'c', op: 'raster.contour', input: '@d', levels: [100, 300] },
        ],
      },
      dir,
    )
    assert.equal(r.steps.find(s => s.id === 'p').count, 2)
    assert.ok(r.steps.find(s => s.id === 'c').count >= 2)
  })

  test('矢量算子拒绝栅格输入', () => {
    const { dir, sq } = squareDir()
    expectError(
      () => analyze(dir, { operations: [{ id: 'r', op: 'raster.from_vector', input: sq, cellsize: 100 }, { id: 'b', op: 'proximity.buffer', input: '@r', distance_m: 10 }] }),
      'E_BAD_REQUEST',
    )
  })
})

// ================================================================ 面板支撑

describe('预览、清单与卡片', { skip }, () => {
  test('preview：SVG 地图卡片、要素数、坐标系切换', () => {
    const r = call('preview', { source: `${DATA}/hospitals.geojson` })
    assert.equal(r.kind, 'vector')
    assert.equal(r.meta.count, 12)
    assert.ok(r.svg.startsWith('<svg') && r.svg.length <= 120000)
    for (const [crs, expect] of [['EPSG:3857', 'EPSG:3857'], ['EPSG:4490', 'EPSG:4490'], ['utm', `EPSG:${UTM}`]]) {
      const p = call('preview', { source: `${DATA}/parcels.geojson`, crs })
      assert.equal(p.meta.crs, expect)
      assert.equal(p.meta.source_crs, 'EPSG:4326')
    }
    expectError(() => call('preview', { source: `${DATA}/parcels.geojson`, crs: 'EPSG:abc' }), 'E_BAD_REQUEST')
  })

  test('preview 终端 PNG', () => {
    const r = call('preview', { source: `${DATA}/parcels.geojson`, png: true })
    assert.equal(Buffer.from(r.png_base64, 'base64').subarray(1, 4).toString(), 'PNG')
  })

  test('list：扫描工作目录 + 列出产物', () => {
    const wd = tmp()
    analyze(wd, { title: '清单测试', operations: [{ id: 'a', op: 'query.measure', input: `${DATA}/districts.geojson` }] })
    const inv = call('list', { datasets_dirs: [DATA] }, wd)
    const names = inv.datasets.map(d => d.name)
    for (const n of ['parcels', 'roads', 'hospitals', 'poi']) assert.ok(names.includes(n), n)
    assert.equal(inv.artifacts[0].title, '清单测试')
    assert.equal(inv.datasets.find(d => d.name === 'districts').origin, 'agent-used')
  })

  test('_card：结果之外附带卡片，且不进入 result', () => {
    const { env } = raw(
      JSON.stringify({ action: 'analyze', _card: { crs: 'EPSG:4326' }, operations: [{ id: 'a', op: 'proximity.buffer', input: `${DATA}/hospitals.geojson`, distance_m: 500 }] }),
    )
    assert.ok(env.ok)
    assert.ok(!JSON.stringify(env.result).includes('<svg'))
    assert.ok(env.card.svg.startsWith('<svg'))
    assert.equal(env.card.meta.crs, 'EPSG:4326')
  })
})
