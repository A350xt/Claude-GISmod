# 架构总览

> GeoCore —— Agent-native GIS Runtime。目标不是复刻 QGIS 的按钮，
> 而是：**让 Claude 通过极少数工具组合解决通用 2D 空间分析任务**。
> "覆盖率"必须由 benchmark 证明，不由主观判断（见 §基准与治理）。

## 一、三层结构

```text
 用户自然语言
      │
      ▼
 Claude Code
      │  mcp__geocore__inspect / analyze / visualize      ← 模型永远只见这 3 个工具
      ▼
 ┌──────────────────────────────────┐
 │ GeoCore mod（hooks/，TypeScript） │  tools.ts   工具描述与参数 schema
 │ 薄适配层 + 侧边面板               │  kernel.ts  $.process.run 调内核、封套解析、错误提示
 │                                  │  store.ts / panel.tsx  面板状态与绘制
 └───────────────┬──────────────────┘
                 │ stdin/stdout 单条 JSON 封套（每请求一个 python 进程）
                 ▼
 ┌──────────────────────────────────┐
 │ geocore（src/，Python，无状态）    │
 │  runtime   数据/CRS/Artifact      │
 │  analysis  原语执行器              │
 │  providers 地理栈后端              │
 │  viz       专题图 / 面板预览        │
 └──────────────────────────────────┘
```

设计基石：

1. **Python 内核完全无状态**——"会话"只由文件路径与 artifact id 表达；
   内核可被任何宿主以同一协议驱动（`python -m geocore run`），mod 只是其中一个宿主。
2. **mod 不依赖宿主框架内部**——只用 Claude Code 公开的 mod API
   （`$.tool.register`、`$.process.run`、`$.ui.open`、`$.state`），宿主升级不会牵动内核。
3. **模型不感知底层算法**——工具入参是 `proximity.buffer` 这类**原语名**，
   绝不是 QGIS/GeoPandas 函数名；后端可整体替换。
4. **分析与表达分离**——`analyze` 只产生数据与统计，地图由 `visualize` 输出；
   面板只负责"看"，分析始终在对话里由 Claude 完成。

## 二、数据世界与七类内核

内部数据模型只有三种；文件格式只是表象：

| 模型 | 本质 | 例 |
| --- | --- | --- |
| Object | 离散对象（几何 + 属性） | 行政区、道路、POI、地块 |
| Field | 连续场（每个位置一个值） | 高程、温度（Phase 2，Raster 为其常见表示） |
| Network | 连接系统（node/edge/cost） | 路网可达性（Phase 3） |

七类分析内核与实现进度：

| # | 内核类 | 回答的问题 | Phase 1 |
| --- | --- | --- | --- |
| 1 | Query & Measure | 什么在哪、多大多长、什么关系 | ✅ query.filter / select / measure |
| 2 | Proximity | 周围有什么、谁最近、影响范围 | ✅ buffer / within_distance / nearest |
| 3 | Overlay | 多层空间的交并差 | ✅ intersection / difference / union / clip |
| 4 | Zonal | 每个区域内有多少 / 平均多少 | ✅ zonal.summarize |
| 5 | Field | 连续空间如何变化 | — |
| 6 | Network | 沿连接关系如何移动 | — |
| 7 | Spatial Statistics | 是否聚集、是否存在模式 | — |

## 三、可靠性铁律（写入代码，非约定）

| 规则 | 实现 | 违反后果 |
| --- | --- | --- |
| 度量必须在米制坐标系 | 每请求确定唯一分析 CRS（投影输入直接采用；地理输入按范围选 UTM 并全程告警） | 无警告不得出数 |
| 缺失 CRS 只允许一种安全假定 | 坐标在经纬度值域内 → EPSG:4326 + warning；否则 E_CRS_AMBIGUOUS | 静默错 CRS 是事故 |
| 无效几何自动修复且留痕 | make_valid +代表性化简，逐条计数进 warnings | E_GEOMETRY_INVALID_UNFIXABLE 兜底 |
| 空结果是异常信号 | 默认报 E_EMPTY_RESULT 并提示 `"allow_empty": true` 出口 | 防"过滤过静悄悄变空集" |
| 中间步骤可复用 | analyze 将每步写入 artifact 的 GPKG 图层；`ar_xxx#stepId` 跨调用引用 | 防止二次分析误用最终表 |

## 四、GIS Result Artifact

每次 `analyze` 落盘一个目录，是可追溯的最小单元：

```text
<workdir>/artifacts/ar_<时间戳><随机>/
├── meta.json    标题/来源/步骤摘要/告警/CRS 决策/intermediate_layers 索引
├── plan.json    用户提交的原语操作序列（机器可回放）
├── result.gpkg  layer "result" = 最终结果；layer "step_<id>" = 各中间步骤
└── map.png      （经 visualize 后附加）
```

由此支撑多轮工作流："把刚才候选区域再排除自然保护区"
即一次新的 analyze，其输入为 `上轮artifact_id#candidates`。

## 五、Provider 抽象

`providers/base.Provider` 以"四类原语 × 纯函数"定义契约：
现由 `geostack.GeoStackProvider`（GeoPandas/Shapely/pyproj）实现；
QGIS headless 仅留插槽不做实现（本机 QGIS 3.16 过旧，属 Phase 4 决策）。
替换或新增 provider 不允许触碰工具词表。

## 六、基准与治理

- `tests/bench/starter.jsonl` 已含 **14 道**起始题，期望值分两级：
  construction truth（合成城市生成期统计，与 GIS 栈无关）与结构性谓词
  （范围/不相交/投影正确）。`tests/test_bench_starter.py` 是评测运行器雏形。
- 起始合成城市有已知近似：行政区界存在收缩缝隙（个别要素落缝），
  因此覆盖型断言使用 ≥85% 这类边界，文档化而非掩盖。
- **新增能力的两问治理**：① 属于七类中哪一类？② 为什么现有原语组合不出来？
  两问成立才新增第八类；否则只能加算法/后端/优化器，不加 Agent 工具。

## 七、已知边界（诚实清单）

- 线/面 zonal 的占比分母是数据全层量：一份数据跨多区会被重复计入，
  `share_pct` 语义是"占全层的比例和"，跨区求和不恒等于 100%（测试已固化该语义）。
- 大规模最近邻是 O(n×m) 向量化计算，超过 2500 万配对明确拒绝而不是慢死。
- Raster/网络/空间统计未实现，`inspect` 的 `capabilities` 会如实声明当前能力。
