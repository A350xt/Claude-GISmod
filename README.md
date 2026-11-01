# GeoCore

**给 Claude Code 用的 GIS 空间分析 mod。**
Claude 只用 3 个工具就能做缓冲、叠加、分区统计、栅格成本距离、专题制图等分析；
侧边面板显示工作目录里的数据、分析结果和地图预览。

```text
你：「找出主干道 2 公里内、不在洪水区的地块，按区统计面积，出一张分级设色图」
Claude：inspect → analyze（过滤→缓冲→求交→扣除→度量→分区）→ visualize → 看图后汇报
面板：自动出现新结果，点一下看形状、字段和告警
```

## 安装

**1. 装 mod**（在 Claude Code 终端里）：

```text
/plugin install geocore --marketplace A350xt/dsh-geocore
```

桌面版：点输入框旁的 **+** → Plugins → Add marketplace 填 `A350xt/dsh-geocore` → Add plugin。

**2. 准备 Python**（≥3.11，需要 geopandas 等地理栈）。任选一种：

```bash
# A) 推荐：在本仓库目录建虚拟环境，mod 会自动找到 .venv
uv venv .venv && uv pip install -e ".[viz,raster]"

# B) 用你已有的 Python 环境
pip install geopandas shapely pyproj pyogrio matplotlib rasterio scipy
```

用 B 时，在 `/config` 里把 **geocore → Python 解释器** 设为该环境的 python 路径
（或设置环境变量 `GEOCORE_PYTHON`）。内核源码随 mod 分发，不需要单独安装 geocore 包。

## 使用

- 直接在对话里提空间分析需求，附上数据路径即可；Claude 会先 `inspect` 再 `analyze`。
- `/geocore` 打开面板：
  - **数据集**：工作目录下扫描到的 GeoJSON / GPKG / SHP / CSV，以及 Claude 用过的文件；
  - **分析结果**：每次 `analyze` / `visualize` 的产物，带要素数、告警、生成的地图链接；
  - 点任意一项看地图卡片：坐标格网与刻度、比例尺、指北针，亮/暗主题自适应
    （终端里用 kitty/iTerm2 图片显示）；
  - **坐标系可切换**：预览下方的下拉框在数据原坐标系、WGS 84、CGCS2000、Web 墨卡托、
    UTM 自动分带之间切换，范围与刻度随之换算（只影响显示，不改数据）；
  - 正式出图同样可指定坐标系：让 Claude「用 EPSG:4490 出图」即可（visualize 的 `crs` 参数）；
    「让 Claude 分析」「出专题图」把提示词填进输入框，确认后发送。
- 分析产物写在 `<工作目录>/artifacts/ar_xxx/`，可在后续对话里用 `ar_xxx` 或 `ar_xxx#步骤id` 继续引用。

可选设置（`/config` → geocore）：Python 解释器、工作目录（默认当前会话目录）、单次调用超时。

## 三个工具

| 工具 | 一句话 |
| --- | --- |
| `mcp__geocore__inspect` | 理解数据：几何/CRS/字段/质量/可做的分析 |
| `mcp__geocore__analyze` | 执行分析计划：操作词表 + `@步骤` / `ar_xxx#步骤` 链式引用 |
| `mcp__geocore__visualize` | 出专题图 PNG：分级设色 / 分类 / 点径 / 栅格色带 + 图例 |

完整契约（错误码、操作词表、语义警示）：[docs/tool-api.md](docs/tool-api.md)

## 目录结构

```text
.claude-plugin/        mod 清单（plugin.json）与 marketplace.json
hooks/                 mod 代码（TypeScript，Claude Code mod API）
  register.tsx           入口：注册工具、/geocore 命令、面板
  tools.ts               三个工具的描述与参数 schema
  kernel.ts              调 Python 内核（$.process.run）、解析封套、错误提示
  store.ts               面板状态（$.state）与刷新/预览动作
  panel.tsx              面板绘制
types/index.d.ts       面板状态的类型契约
tests-mod/             mod 测试（claude plugin test .）
src/geocore/           Python 内核（无状态，也可单独当库/CLI 用）
  runtime/ analysis/ providers/ viz/
tests/                 内核测试 + bench/starter.jsonl 基准题
datasets/synthetic/    合成城市「临江市」测试数据
scripts/               合成数据生成、选址端到端演示
docs/                  架构、工具契约
```

## 开发

```bash
uv venv .venv && uv pip install -e ".[viz,raster,dev]"
python scripts/make_synthetic_data.py     # 生成合成城市（测试数据）
.venv/Scripts/python -m pytest -q         # 内核测试（Linux/macOS 用 .venv/bin/python）
claude plugin validate .                  # mod 静态检查
claude plugin test .                      # mod 测试
```

本地调试 mod：`claude --plugin-dir <本仓库路径>`，改动保存后自动重载。

不经过 Claude 直接驱动内核：

```bash
echo '{"action":"inspect","path":"datasets/synthetic/parcels.geojson"}' | python -m geocore run --workdir .
```

## 路线图

- [x] **Phase 1 — Vector Core**：数据准备管线、Query&Measure / Proximity / Overlay / Zonal、
      Artifact 中间步骤持久化与跨调用引用、专题图、14 道基准题
- [x] **Phase 1.5 — 实测加固**：全链路 UTF-8、datetime 一等支持、`trajectory.build`、
      分组度量明细、CRS 策略 v2、制图增强、错误附带修复提示
- [x] **Phase 2 — Field GIS 核心**：22 个 `raster.*` 算子 + 栅格 inspect / visualize
- [x] **v0.2 — Claude Code mod**：工具与面板改为 Claude Code mod，去掉 DeepSeek Harness 依赖
- [ ] **Phase 1.6 — 时间序列表达**：时间聚合算子、折线/极坐标等非地图图表
- [ ] **Phase 2.5 — Field GIS 进阶**：插值、地形派生、水文链、视域
- [ ] **Phase 3 — Network & Spatial Statistics**
- [ ] **Phase 4 — Scale**：PostGIS/DuckDB-Spatial 后端、并行与缓存、全量 benchmark

## 治理铁律（新 PR 必答两问）

1. 这个能力属于现有七类中的哪一类？
2. 为什么现有原语组合不出来？

只有第二问成立才允许新增第八类；否则只加算法、后端或优化器——**不加工具**。

## 文档

- [架构总览](docs/architecture.md)
- [工具 API 契约](docs/tool-api.md)

## 许可

内核依赖均为 BSD/MIT/Apache 系纯 Python 地理栈。
