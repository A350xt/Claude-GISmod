# Claude-GISmod

**一个给 Claude Code 用的 GIS 空间分析 mod。**
用自然语言提需求，Claude 用三个 GIS 工具完成缓冲、叠加、分区统计、栅格成本距离、专题制图等分析。
每一步的结果直接以地图卡片的形式出现在对话里；侧边面板用来浏览数据和历史结果，坐标系一键切换。
装上即用：第一次使用时自动准备 Python 环境。

> 这是社区项目，不是 Anthropic 官方插件。插件 id 是 `gismod`。

```text
你：找出主干道 2 公里内、不在洪水区的地块，按区统计面积，出一张分级设色图
Claude：inspect → analyze（过滤→缓冲→求交→扣除→度量→分区）→ visualize → 亲自看图后汇报
对话：每次调用下面是一张卡片——地图、要素数、分析步骤、告警，点一下就能在面板里继续看
```

## 能做什么

| 类别 | 例子 |
| --- | --- |
| 查询与度量 | 属性过滤、空间选择（相交/包含/在内）、面积/长度/计数，按字段分组汇总 |
| 邻近 | 缓冲区、距离范围内筛选、最近邻（带距离和属性） |
| 叠加 | 求交、求差、合并、裁剪 |
| 分区统计 | 每个区里有多少点、多长的线、多大面积，属性加权求和 |
| 轨迹 | 按时间把点连成轨迹线（GPS、台风路径） |
| 栅格（22 个算子） | 矢量栅格化、距离场、成本距离与最短路径、重分类、加权叠加、邻域统计、等值线、转面、采样 |
| 制图 | 单色 / 分类 / 分级设色 / 点径专题图，栅格色带；可加底图、指定坐标系 |

支持 GeoJSON、GeoPackage、Shapefile、带经纬度列的 CSV、GeoTIFF；中文路径、字段、标题全程可用。

## 安装

在 Claude Code 终端里：

```text
/plugin install gismod --marketplace A350xt/Claude-GISmod
```

桌面版：点输入框旁的 **+** → Plugins → Add marketplace，填 `A350xt/Claude-GISmod` → Add plugin。

**不需要再手动配 Python。** 第一次调用 GIS 工具时，mod 会自动在 `~/.claude/gismod/venv`
建一个 Python 3.12 环境并装好 geopandas、rasterio 等地理库（有 [uv](https://docs.astral.sh/uv/) 时约 1–3 分钟，
没有 uv 时退回 `python -m venv` + pip）。进度显示在状态栏和面板里，之后每次直接可用。

想用自己的环境，在 `/config` → **gismod → Python 解释器** 里填它的路径（或设环境变量 `CLAUDE_GIS_PYTHON`），
只要装了 `geopandas shapely pyproj pyogrio matplotlib rasterio scipy` 即可。

## 使用

**对话**：直接说需求并给出数据路径。Claude 会先 `inspect` 摸清数据，再 `analyze`，需要时 `visualize` 出图，
并打开生成的 PNG 自查后再汇报。每次调用的结果在对话里显示成一张卡片：

- 数据概览 / 分析结果：地图卡片（格网、比例尺、指北针、坐标系）、要素数与几何类型、每一步做了什么、告警；
- 专题图：成图缩略图，「打开原图」看高清 PNG；
- 「在面板中查看」跳到侧边面板继续浏览；按 ctrl+o 可看工具返回的原始数据。

**面板**：输入 `/gis` 打开。

- **数据集**：工作目录里扫描到的空间数据，以及 Claude 分析时用过的文件；
- **分析结果**：每次分析和出图的产物，带要素数、告警和地图链接；
- 点任意一项，看地图卡片：坐标格网与刻度、比例尺、指北针、坐标系标签，亮/暗主题自适应
  （终端里用 kitty / iTerm2 图片显示）；
- **坐标系下拉框**：在数据原坐标系、WGS 84（EPSG:4326）、CGCS2000（EPSG:4490）、
  Web 墨卡托（EPSG:3857）、UTM 自动分带之间切换，刻度、比例尺和范围随之换算。只改显示，不改数据；
- 「让 Claude 分析」「出专题图」按钮把提示词填进输入框，确认后再发送。

**接着上次的结果分析**：每次分析写入 `<工作目录>/artifacts/ar_xxx/`，
之后可以说「在 ar_xxx 的基础上再排除保护区」，或引用中间步骤 `ar_xxx#步骤id`。

**正式出图换坐标系**：说「用 EPSG:4490 出图」即可（`visualize` 的 `crs` 参数）。

### 设置

`/config` → gismod：

| 项 | 默认 | 说明 |
| --- | --- | --- |
| Python 解释器 | 自动 | 依次尝试 `CLAUDE_GIS_PYTHON`、mod 目录的 `.venv`、`~/.claude/gismod/venv`；都不可用时自动创建后者 |
| 工作目录 | 当前会话目录 | 分析产物 `artifacts/` 写在这里，也是面板扫描数据的地方 |
| 单次调用超时 | 300 秒 | 10–600 |

## 三个工具

| 工具 | 作用 |
| --- | --- |
| `mcp__gismod__inspect` | 理解数据：几何类型、坐标系、范围、字段与样例、质量问题、可做的分析 |
| `mcp__gismod__analyze` | 执行分析计划：按步骤调用原语，步骤间用 `@步骤id` 串联，结果可跨对话引用 |
| `mcp__gismod__visualize` | 出专题图 PNG（含图例），可指定底图和坐标系 |

可靠性约定写在内核里而不是靠提示：度量一律在米制坐标系下进行、缺坐标系只做唯一安全假定、
无效几何自动修复并留痕、空结果默认报错。完整契约见 [docs/tool-api.md](docs/tool-api.md)。

## 项目结构

```text
.claude-plugin/     mod 清单（plugin.json）与 marketplace.json
hooks/              mod（TypeScript，Claude Code mod API）
  register.tsx        入口：注册工具、/gis 命令、对话卡片与面板
  tools.ts            三个工具的说明与参数 schema
  kernel.ts           调用 Python 内核、解析结果、给出修复提示
  setup.ts            查找或自动创建 Python 环境
  store.ts            状态：清单、预览、坐标系、对话卡片
  card.tsx            对话内结果卡片
  panel.tsx           侧边面板
  types.d.ts          状态的类型契约
  gis.test.tsx        mod 测试
kernel/             Python 内核 claude_gis（无状态，也能单独当库或命令行用）
  claude_gis/         runtime / analysis / providers / viz
  tests/              内核测试 + bench 基准题
  datasets/synthetic/ 合成测试城市「临江市」
  scripts/            合成数据生成、选址端到端演示
  pyproject.toml
docs/               架构说明、工具契约
```

## 开发

```bash
uv venv .venv && uv pip install -e "./kernel[viz,raster,dev]"   # mod 会优先用这个 .venv
cd kernel
python scripts/make_synthetic_data.py      # 生成合成城市（测试数据）
../.venv/Scripts/python -m pytest -q       # 内核测试（Linux/macOS 用 ../.venv/bin/python）
cd ..
claude plugin validate .                   # mod 静态检查
claude plugin test .                       # mod 测试
```

本地调试 mod：`claude --plugin-dir <本仓库路径>`，保存后自动重载。

不经过 Claude 直接调用内核（一条 JSON 请求进，一条 JSON 结果出）：

```bash
cd kernel
echo '{"action":"inspect","path":"datasets/synthetic/parcels.geojson"}' | python -m claude_gis run --workdir .
```

## 路线图

- [x] 矢量核心：查询度量 / 邻近 / 叠加 / 分区统计，中间步骤可复用，专题图，14 道基准题
- [x] 实测加固：全链路 UTF-8、时间字段、轨迹、分组度量、坐标系策略、制图增强
- [x] 栅格核心：22 个 `raster.*` 算子，栅格 inspect / visualize
- [x] Claude Code mod：三个工具、对话内结果卡片、侧边面板、坐标系切换、自动准备 Python 环境
- [ ] 时间序列表达：时间聚合、折线 / 极坐标等非地图图表
- [ ] 栅格进阶：插值、坡度坡向、水文、视域
- [ ] 网络分析与空间统计
- [ ] 规模化：PostGIS / DuckDB-Spatial 后端、缓存、全量基准

## 新能力的两问

1. 它属于现有七类分析中的哪一类？
2. 为什么现有原语组合不出来？

第二问成立才新增类别；否则只加算法、后端或优化——**不加新工具**。

## 文档

- [架构说明](docs/architecture.md)
- [工具 API 契约](docs/tool-api.md)

## 许可

内核依赖均为 BSD / MIT / Apache 系的纯 Python 地理库。
