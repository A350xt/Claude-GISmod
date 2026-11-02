# 工具 API 契约（Claude-GISmod v0.3）

三个工具构成 Claude 的全部 GIS 表达面，由 mod 注册为
`mcp__gismod__inspect / analyze / visualize`（定义见 [hooks/tools.ts](../hooks/tools.ts)）。

内核层面每个请求返回一条 JSON 封套：

```json
{ "ok": true,  "result": { ... } }
{ "ok": false, "error": { "code": "…", "message": "…", "details": { … } } }
```

成功时 mod 把 `result` 交给 Claude；失败时工具调用被拒绝，Claude 读到 `错误码: 信息 + details`。

错误码（稳定契约）：

| code | 触发场景 | 模型应如何反应 |
| --- | --- | --- |
| `E_BAD_REQUEST` | 参数缺失/类型错/where 非法/非法 artifact_id | 读取 details 修正后重试 |
| `E_INPUT_MISSING` | 路径不存在 / 图层或字段缺失 / artifact 不存在 | 先 `inspect` 摸清输入 |
| `E_CRS_AMBIGUOUS` | 数据缺 CRS 且坐标不在经纬度值域 | 让用户提供 CRS 或先转换 |
| `E_OP_UNKNOWN` | op 名不在词表 | 使用 details.available 中的名字 |
| `E_GEOMETRY_INVALID_UNFIXABLE` | make_valid 也救不了 | 放弃该层或请用户清洗 |
| `E_EMPTY_RESULT` | 步骤产出 0 要素且未标 allow_empty | 自查过滤条件；确属预期加 `"allow_empty": true` |
| `E_OUTPUT_ERROR` | artifact 写出失败 | 检查 workdir 权限 |
| `E_INTERNAL` | 未预期异常（附 traceback） | 上报用户 |

---

## inspect（mcp__gismod__inspect）

请求：`{ "path": "<GeoJSON|GPKG|SHP|CSV(含 lon/lat 列)>" }`
GPKG 可选 `"layer"`；多图层时缺省取第一层。

响应要点：`kind/count/crs/extent_bbox/geometry_types/fields[{name,dtype,samples}]/
quality{invalid_geometries,null_geometries,missing_crs,wgs84_assumable_if_missing}/
capabilities[]/notes[]`。

使用教条：**遇到陌生数据，第一步永远是 inspect**。由 quality 决定是否需要预处理，
由 capabilities 声明当前内核能做什么。

## analyze（mcp__gismod__analyze）

请求：`{ title?, crs?, operations: Op[], output?: { format: "gpkg"|"geojson" } }`

`Op = { id, op, <按词表传参>, allow_empty? }`。步骤按序执行；
后续步骤引用前序结果的三种写法：

| 写法 | 作用域 |
| --- | --- |
| `"@<id>"` | 同一请求内的中间结果 |
| `"ar_xxx…"` | 已有 artifact 的最终结果 |
| `"ar_xxx…#<stepId>"` | 该 artifact 内持久化的中间步骤图层 |

### 操作词表

```text
query.filter            { input, where }
query.select            { input, predicate?, ref }        # intersects|within|contains|touches|crosses|overlaps
query.measure           { input, measures?, group_by? }   # area_sqkm area_ha length_km count → 追加同名列并给汇总行；
                                                          # group_by 给出后每组数值在 steps[].table 直接返回

proximity.buffer        { input, distance_m, dissolve?, quad_segs?=16 }
proximity.within_distance { input, ref, distance_m }      # 距参照层 distance_m 以内筛选
proximity.nearest       { input, ref, k?=1, join_attrs? } # 输出 nearest_rank / nearest_distance_m / nearest_<attr>

overlay.intersection    { a, b }                          # 属性合并；两输入同名列归一为 <col>_a / <col>_b
overlay.difference      { a, b }
overlay.union           { a, b }
overlay.clip            { a, b }                          # 只保留 a 的属性

zonal.summarize         { regions, data, stats?, field? } # regions 必须为面
trajectory.build        { input, time_field, group_by? }  # 点按时间排序连成轨迹线（如风暴路径/GPS），
                                                          # 输出 point_count/start_time/end_time/duration_h
```

zonal 说明：
- 点数据：stats ∈ count,sum,mean,min,max（sum/mean 等需 field）
- 线数据：total_length_km, share_pct（对每区求落入长度）；field.sum 为按长度分摊的加权贡献
- 面数据：count,total_area_sqkm,share_pct；field.sum 按交叠面积分摊、field.mean 为面积加权均值
- 输出列前缀 `data_`（如 `data_count`、`data_beds_sum_apportioned`）
- ⚠ 一份数据跨多个区会重复计入各区；`share_pct` 是"占全层比例"，跨区求和 ≠100%

trajectory.build {input, time_field, group_by?}  点按时间排序连轨迹线（输出 point_count/start/end/duration_h）

### 栅格词表（raster 域，GeoTIFF 出入）

```text
基建    raster.info {input}
        raster.create {bounds, cellsize, crs?, value?}
        raster.from_vector {input, cellsize|template, field?, all_touched?}
        raster.align {input, template, method?}          # nearest/bilinear/cubic
        raster.mask {input, mask, invert?, fill?}        # mask=矢量或栅格；fill 数值=填值
        raster.merge {inputs[]}                           # 镶嵌，重叠先到先得
距离    raster.distance {input, cellsize?, max_distance_m?, pad_m?}
        raster.distance_decay {input, d0, f0?, mode?}    # linear/square/exponential
        raster.cost_distance {source, cost}              # 累积成本 = cost×米；NoData/≤0 不可通行
        raster.cost_path {source, cost, to}              # 成本最短路径（to 取第一个点）
        raster.nearest {input, ref}                      # 每像元=最近参照要素序号
逐像元  raster.con {input, condition, true?, false?}     # 条件如 'value >= 500' 或 'nodata'；
                                                        # 比较条件不改动 NoData；值可为 'nodata'
        raster.calc {inputs:{别名:引用}, expression}      # 白名单表达式（无 I/O/随机）
        raster.reclassify {input, mapping:[[lo,hi,new]…], nodata?}
        raster.breaks {input, classes, method?}          # equal_interval/quantile/jenks，只出断点表
        raster.histogram {input, bins?}                  # 直方图+累计频率
        raster.weighted_sum {inputs:[{ref,weight}…]}     # 权重自动归一化
邻域    raster.focal {input, stat, kernel_size?}         # mean/min/max/median/std（默认 3×3）
        raster.zonal_stats {regions, data, stats?}       # count/mean/min/max/sum/majority
转出    raster.polygonize {input, field?, dissolve?}     # 同值溶解；接 query/overlay 闭环
        raster.contour {input, levels?|interval?}        # 等值线 LineString（level 字段）
        raster.sample {raster, points, field?}           # 栅格值采样到点（输出矢量）
```

栅格三条基建约定：
1. **模板显式化**：组合类算子（calc/weighted_sum/mask/merge 等）默认对齐到
   首个输入的 origin/cellsize/行列数/CRS，不一致自动重采样并写 warnings；
2. **统计摘要随产物**：每个栅格步骤的 summaries 自带行列/像元/NoData 占比/
   分位数；`raster.info` 的 `steps[].table` 是完整元信息表；全 NoData 结果
   按 E_EMPTY_RESULT 处理（可 allow_empty）；
3. **nodata 统一 NaN**：内部 float32，落盘 GeoTIFF（result.tif + step_<id>.tif），
   跨请求引用 `ar_xxx#stepId` 与矢量产物同构。

设计原则：求值与判据分离（breaks/histogram 只出表）、一算子一语义
（con/reclassify/calc/focal 各司其职）、镜像矢量域命名
（raster.distance↔proximity.buffer、raster.zonal_stats↔zonal.summarize、
raster.from_vector/polygonize 互逆、raster.mask↔overlay.clip）。

where 表达式：pandas query 语法；**Python 关键字列名必须用反引号**，
如 `` `class` == '高速' ``（引擎自动改写为安全别名）。危险关键字直接拒绝。
时间列已自动解析为 datetime，可直接比较：`time >= "2024-06-01"`。

响应 result：`artifact_id/title/result{path,format,count,geometry_types}/
summary[](中文摘要)/steps[]{id,op,count,summary,table?}/warnings[]/crs{analysis,sources}/
inputs[]/intermediate_layers{stepId:{layer,count}}`。

取数通道（不必再读 artifact 文件）：
- `preview`：最终结果前 10 行（非几何列，最多 12 列）；
- `steps[].table`：分组明细（measure 的 group_by、trajectory 的逐轨迹）。

CRS 承诺（v0.2 起）：
- 度量类操作（proximity/overlay/zonal/measure）→ 整次分析统一到一个米制投影坐标系
  （首个输入决定：已投影→采用；地理→按范围选 UTM），所有重投影都记入 warnings；
- 纯属性/拓扑/轨迹管线 → **保持源 CRS**，不悄悄重投影；
- 顶层 `crs` 参数可显式覆盖：`"source"`（跟随首个输入）或 `"EPSG:32622"` 等固定坐标系。

编码承诺：全链路 UTF-8（子进程 `-X utf8` + stdin/stdout 重配）。中文路径、
中文文件名、中文属性值、中文标题在任何 locale 的宿主（含中文 Windows 的 GBK
默认代码页）下均原样往返。

## visualize（mcp__gismod__visualize）

请求：`{ source, crs?, base?, base_style?, title?, style? }`
source 同上三种引用写法；写入既有 artifact 目录或新建 map 类型 artifact。
`crs` 为出图坐标系：`"source"`（默认，保持数据原坐标系）、`"utm"`（自动分带）或 `"EPSG:4490"` 等。
`base` 为底图层（如陆地/行政区），垫在主图层之下；`base_style` 可调
`{ color, edgecolor, alpha }`。

style 键：`mode`(single|point|categorical|choropleth)、`field`、
`classification`(quantile|equal_interval)、`classes`(2-9)、`cmap`、`color`、
`size`、`size_field` + `size_range:[min,max]`（point 模式按数值字段缩放点径）、
`category_colors`（categorical 模式 `{"类别值": "#rrggbb"}` 显式控色）、
`figsize`、`dpi`。
响应：`image_path + legend[] + warnings[]`。中文字体默认 Microsoft YaHei
（回退 SimHei / Noto Sans CJK）；中文标题与中文文件名均支持。

栅格 source（.tif 路径或 raster 产物的 artifact_id）：
`style.mode=continuous`（`cmap` + `vmin/vmax` 或 `stretch:[p_lo,p_hi]`
百分位拉伸，默认 [2,98]，`unit` 定色标名）或
`categorical`（`category_colors` 类别色表）。

## 底层协议（供其他宿主实现者）

```bash
python -m claude_gis run --workdir <目录>   # stdin:  {"action": "analyze", ...参数}
                                           # stdout: 一条 JSON 封套；exit 0 成功 / 2 领域错误 / 1 内部错误
```

| action | 用途 |
| --- | --- |
| `inspect` / `analyze` / `visualize` | 三个工具的实现 |
| `compose` | 多图层成品版面（图例、比例尺、指北针） |
| `show` | 读取某个 artifact 的元数据（参数 `artifact_id`） |
| `operations` | 当前可用的操作词表，便于自省 |
| `list` | 面板清单：`{datasets_dirs: [...]}` → 数据集与 artifacts |
| `preview` | 面板预览：`{source, crs?, png?}` → 地图卡片 SVG、可选 PNG、摘要；`crs` 只影响显示 |

任何 action 都可以带 `"_card": {"crs": "...", "png": false}`：成功时封套多一个与 `result` 并列的 `card`
（地图卡片或成图缩略图），供界面显示；卡片生成失败只写进 `card.error`，不影响主结果。
