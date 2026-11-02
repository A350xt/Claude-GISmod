# 架构说明

> Claude-GISmod 的目标不是复刻 QGIS 的按钮，而是**让 Claude 只用两个工具组合解决通用的二维空间分析任务**。
> 覆盖率由基准题证明，不靠主观判断（见「基准与治理」）。

## 一、两层结构

```text
 用户自然语言
      │
      ▼
 Claude Code
      │  mcp__gismod__inspect / analyze        ← 模型只见这 2 个工具
      ▼
 ┌──────────────────────────────────────┐
 │ gismod mod（hooks/，TypeScript）       │  tools.ts   工具说明与参数 schema
 │ 薄适配层 + 界面                        │  kernel.ts  $.process.run 调内核、解析封套、修复提示
 │                                      │  setup.ts   查找 / 下载原生内核
 │                                      │  store.ts   状态（$.state）
 │                                      │  card.tsx   对话内结果卡片
 │                                      │  panel.tsx  侧边面板（/gis）
 └──────────────────┬───────────────────┘
                    │ stdin / stdout：每个请求一条 JSON 封套，一个进程
                    ▼
 ┌──────────────────────────────────────┐
 │ claude-gis（native/，C++，无状态）      │
 │  layer / raster   数据读写（GDAL）       │
 │  crs / resolver   坐标系与几何修复       │
 │  ops_vector / ops_raster  原语算子      │
 │  where            过滤表达式解析器        │
 │  artifact         产物落盘与引用          │
 │  preview          地图卡片 SVG / PNG     │
 └──────────────────────────────────────┘
```

设计原则：

1. **内核无状态**。「会话」只由文件路径和 artifact id 表达；任何宿主都能用同一协议
   （`claude-gis run`）驱动内核，mod 只是其中一个宿主。行为由 tests/ 里的黑盒测试约束。
2. **mod 只用公开 API**。`$.tool.register`、`$.process.run`、`$.ui.open`、`$.state`；
   不依赖任何宿主内部实现，宿主升级不牵动内核。
3. **模型不感知底层算法**。工具参数是 `proximity.buffer` 这样的**原语名**，
   不是 GDAL / GEOS 函数名；后端可以整体替换。
4. **分析与表达分离**。`analyze` 只产出数据和统计，地图由对话卡片和面板表达；
   面板只负责「看」，分析始终在对话里由 Claude 完成。

## 二、界面：对话卡片与侧边面板

**对话内结果卡片**。两个工具的请求都带上 `_card` 选项，内核在同一个进程里顺带生成地图卡片 SVG，
只交给界面，不进模型读到的结果。mod 按 tool_use_id 存进 `$.state`，并用 `ToolResult` 渲染钩子替换
对话里的原始结果块；ctrl+o 展开、出错或卡片缺失时交回引擎显示原文。

**原生内核**。`setup.ts` 依次检查 /config 指定的路径、`CLAUDE_GIS_EXE`、仓库里的开发构建、
已下载的 `~/.claude/gismod/bin/<平台>/`（用 `--version` 探测）；都没有就从 GitHub Releases
下载对应平台的 zip。进度写入状态，面板和状态栏显示；并发调用共享同一次准备。

**侧边面板**：

- `/gis` 打开；面板开着时，每次分析完成后自动刷新并选中新结果。
- 数据来源：内核的 `list` 动作扫描工作目录（深度 ≤ 3，跳过依赖与缓存目录），
  再加上 artifact 元数据里记录的 Claude 用过的输入文件。
- 预览：内核的 `preview` 动作把任一图层渲染成「地图卡片」SVG（≤ 120 KB，超出时逐级化简或抽样），
  带坐标格网与刻度、比例尺、指北针、坐标系标签，内嵌亮/暗两套配色；终端改用小 PNG。
- **显示坐标系**：`preview` 的 `crs` 参数为 `source` / `utm` / 任意 EPSG 串，
  只做显示重投影，不写回数据。Web 墨卡托下比例尺按纬度校正。
- 面板状态全部放在 `$.state`（契约见 `hooks/types.d.ts`），mod 重载不丢。

## 三、数据模型与七类分析

内部只有三种数据模型，文件格式只是表象：

| 模型 | 本质 | 例 |
| --- | --- | --- |
| Object | 离散对象（几何 + 属性） | 行政区、道路、POI、地块 |
| Field | 连续场（每个位置一个值） | 高程、温度，常以栅格表示 |
| Network | 连接系统（节点 / 边 / 代价） | 路网可达性 |

七类分析与实现状态：

| # | 类别 | 回答的问题 | 状态 |
| --- | --- | --- | --- |
| 1 | Query & Measure | 什么在哪、多大多长、什么关系 | ✅ query.filter / select / measure |
| 2 | Proximity | 周围有什么、谁最近、影响范围 | ✅ buffer / within_distance / nearest |
| 3 | Overlay | 多层空间的交并差 | ✅ intersection / difference / union / clip |
| 4 | Zonal | 每个区域内有多少 / 平均多少 | ✅ zonal.summarize、raster.zonal_stats |
| 5 | Field | 连续空间如何变化 | ✅ 22 个 raster.* 算子（插值、地形、水文待做） |
| 6 | Network | 沿连接关系如何移动 | — |
| 7 | Spatial Statistics | 是否聚集、是否存在模式 | — |

## 四、可靠性约定（写在代码里，不靠提示）

| 规则 | 实现 | 违反的后果 |
| --- | --- | --- |
| 度量必须在米制坐标系 | 每个请求确定唯一分析坐标系（已投影的输入直接用；地理坐标按范围选 UTM，并写入告警） | 不出没有说明的数字 |
| 缺坐标系只做一种安全假定 | 坐标落在经纬度值域 → 假定 EPSG:4326 并告警；否则报 E_CRS_AMBIGUOUS | 防止静默用错坐标系 |
| 无效几何自动修复并留痕 | make_valid + 代表性化简，逐条计数写入 warnings | 修不好报 E_GEOMETRY_INVALID_UNFIXABLE |
| 空结果视为异常 | 默认报 E_EMPTY_RESULT，并提示 `"allow_empty": true` | 防止过滤条件悄悄变成空集 |
| 中间步骤可复用 | 每一步写入 artifact 的 GPKG 图层，可用 `ar_xxx#步骤id` 跨调用引用 | 防止二次分析误用最终表 |
| 外部 PROJ 环境隔离 | 只使用随程序发布的 proj.db | 防止读到 PostGIS 等其他安装的 proj.db |

## 五、分析产物（Artifact）

每次 `analyze` 落盘一个目录，是可追溯的最小单元：

```text
<工作目录>/artifacts/ar_<时间戳><随机>/
├── meta.json    标题、输入、步骤摘要、告警、坐标系决策、中间图层索引
├── plan.json    提交的原语操作序列（可回放）
└── result.gpkg  图层 "result" 是最终结果；"step_<id>" 是各中间步骤（栅格为 result.tif / step_<id>.tif）
```

多轮工作流因此很自然：「把刚才的候选区域再排除自然保护区」就是一次新的 `analyze`，
输入写 `上次的artifact_id#candidates`。

## 六、基准与治理

- `tests/bench/starter.jsonl` 有 14 道起始题，期望值分两级：构造真值（合成城市生成时的统计，
  与 GIS 栈无关）和结构性断言（范围、不相交、投影正确）。`tests/kernel.test.mjs` 是评测运行器。
- 合成城市有已知近似：行政区界有收缩缝隙，个别要素落在缝里，所以覆盖类断言用 ≥ 85% 这样的边界。
- **新增能力先答两问**：属于七类中的哪一类？为什么现有原语组合不出来？
  第二问成立才新增类别；否则只加算法、后端或优化，不加工具。

## 七、已知边界

- 线 / 面的分区统计里，占比的分母是整层总量：一份数据跨多个区会被重复计入，
  `share_pct` 表示「占整层的比例」，各区相加不一定等于 100%。
- 最近邻是 O(n×m) 逐对计算，超过 2500 万配对直接拒绝，而不是慢慢跑死。
- 网络分析和空间统计尚未实现，`inspect` 返回的 `capabilities` 会如实说明当前能力。
- 面板列表一次最多显示 8 项；预览 SVG 超过体积上限时会化简或抽样显示。
- 不出 PNG 专题图；结果地图由 SVG 地图卡片表达。

## 八、原生内核实现

- 依赖：GDAL（矢量/栅格读写、栅格化、转面、等值线、重投影）、GEOS（经 OGR 几何 API：缓冲、叠加、谓词、修复）、
  PROJ（坐标系）、nlohmann/json；vcpkg 静态链接，发布物是单个可执行文件 + `share/proj/proj.db`。
- 启动时只使用随程序发布的 proj.db（忽略系统里 PostGIS 等写入的 PROJ_LIB），避免版本冲突。
- `where` 表达式：自写的解析器，兼容 pandas query 的常用写法（比较/逻辑/in/算术/反引号列名/时间比较/str 方法）。
- 欧氏距离场与就近分配：Felzenszwalb 精确距离变换；成本距离：8 邻域 Dijkstra。
