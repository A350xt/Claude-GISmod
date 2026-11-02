# Claude-GISmod

**一个给 Claude Code 用的 GIS 空间分析 mod。**
用自然语言提需求，Claude 用两个 GIS 工具完成缓冲、叠加、分区统计、栅格成本距离等分析。
每一步的结果直接以地图卡片的形式出现在对话里；侧边面板用来浏览数据和历史结果，坐标系一键切换。
内核是一个 C++ 原生程序（GDAL / GEOS / PROJ），装上即用，不需要 Python。

> 这是社区项目，不是 Anthropic 官方插件。插件 id 是 `gismod`。

```text
你：找出主干道 2 公里内、不在洪水区的地块，按区统计面积
Claude：inspect → analyze（过滤→缓冲→求交→扣除→度量→分区）→ 汇报
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
| 地图卡片 | 坐标格网、比例尺、指北针，WGS 84 / CGCS2000 / Web 墨卡托 / UTM 一键切换 |

支持 GeoJSON、GeoPackage、Shapefile、带经纬度列的 CSV、GeoTIFF；中文路径、字段、标题全程可用。

## 安装

在 Claude Code 终端里：

```text
/plugin install gismod --marketplace A350xt/Claude-GISmod
```

桌面版：点输入框旁的 **+** → Plugins → Add marketplace，填 `A350xt/Claude-GISmod` → Add plugin。

第一次调用 GIS 工具时，mod 会从本仓库的 GitHub Releases 下载对应平台的原生内核
（`claude-gis`，Windows x64 / Linux x64 / macOS Apple Silicon），放到 `~/.claude/gismod/bin/`，之后每次直接可用。
其他平台请自行编译 `native/`，然后在 `/config` → **gismod → 原生内核路径** 指定（或设环境变量 `CLAUDE_GIS_EXE`）。

## 使用

**对话**：直接说需求并给出数据路径。Claude 会先 `inspect` 摸清数据，再 `analyze`。每次调用的结果在对话里显示成一张卡片：
地图（格网、比例尺、指北针、坐标系）、要素数与几何类型、每一步做了什么、告警；
「在面板中查看」跳到侧边面板；按 ctrl+o 可看工具返回的原始数据。

**面板**：输入 `/gis` 打开。

- **数据集**：工作目录里扫描到的空间数据，以及 Claude 分析时用过的文件；
- **分析结果**：每次分析的产物，带要素数和告警；
- 点任意一项看地图卡片（终端里用 kitty / iTerm2 图片显示）；
- **坐标系下拉框**：在数据原坐标系、WGS 84（EPSG:4326）、CGCS2000（EPSG:4490）、
  Web 墨卡托（EPSG:3857）、UTM 自动分带之间切换，刻度、比例尺和范围随之换算。只改显示，不改数据；
- 「让 Claude 分析」按钮把提示词填进输入框，确认后再发送。

**接着上次的结果分析**：每次分析写入 `<工作目录>/artifacts/ar_xxx/`，
之后可以说「在 ar_xxx 的基础上再排除保护区」，或引用中间步骤 `ar_xxx#步骤id`。

### 设置

`/config` → gismod：

| 项 | 默认 | 说明 |
| --- | --- | --- |
| 原生内核路径 | 自动 | 依次尝试 `CLAUDE_GIS_EXE`、仓库里的开发构建、已下载的版本；都没有就从 GitHub Releases 下载 |
| 工作目录 | 当前会话目录 | 分析产物 `artifacts/` 写在这里，也是面板扫描数据的地方 |
| 单次调用超时 | 300 秒 | 10–600 |

## 两个工具

| 工具 | 作用 |
| --- | --- |
| `mcp__gismod__inspect` | 理解数据：几何类型、坐标系、范围、字段与样例、质量问题、可做的分析 |
| `mcp__gismod__analyze` | 执行分析计划：按步骤调用原语，步骤间用 `@步骤id` 串联，结果可跨对话引用 |

可靠性约定写在内核里而不是靠提示：度量一律在米制坐标系下进行、缺坐标系只做唯一安全假定、
无效几何自动修复并留痕、空结果默认报错。完整契约见 [docs/tool-api.md](docs/tool-api.md)。

## 项目结构

```text
.claude-plugin/     mod 清单（plugin.json）与 marketplace.json
hooks/              mod（TypeScript，Claude Code mod API）
  register.tsx        入口：注册工具、/gis 命令、对话卡片与面板
  tools.ts            两个工具的说明与参数 schema
  kernel.ts           调用内核（stdin JSON → stdout 封套）、给出修复提示
  setup.ts            找到或下载原生内核
  store.ts            状态：清单、预览、坐标系、对话卡片
  card.tsx / panel.tsx 对话卡片 / 侧边面板
  types.d.ts          状态的类型契约
  gis.test.tsx        mod 测试
native/             C++ 原生内核 claude-gis（CMake + vcpkg：GDAL / GEOS / PROJ）
  src/                协议、数据读写、坐标系、算子、where 解析器、地图卡片
tests/              原生内核黑盒测试（node --test，无依赖）
  data/synthetic/     合成测试城市「临江市」
  bench/              基准题
.github/workflows/  native.yml：三平台编译、黑盒测试、打 v* 标签时发布
docs/               架构说明、工具契约
```

## 开发

原生内核（CMake ≥ 3.21、C++20 编译器、[vcpkg](https://github.com/microsoft/vcpkg)；首次编译依赖约 30–60 分钟）：

```bash
export VCPKG_ROOT=~/vcpkg                  # Windows: set VCPKG_ROOT=C:/path/to/vcpkg
cd native
cmake --preset windows                     # 或 linux / macos
cmake --build --preset windows
build/windows/Release/claude-gis --version
```

测试：

```bash
CLAUDE_GIS_EXE=native/build/windows/Release/claude-gis.exe node --test tests/kernel.test.mjs   # 内核黑盒测试
claude plugin validate .                                                         # mod 静态检查
claude plugin test .                                                             # mod 测试
```

本地调试 mod：`claude --plugin-dir <本仓库路径>`，保存后自动重载；开发构建 `native/build/...` 会被优先使用。

发布：推送 `v*` 标签，CI 编译三个平台、跑测试、把 `claude-gis-<平台>.zip` 发到 Releases，mod 自动下载最新版。

不经过 Claude 直接调用内核（一条 JSON 请求进，一条 JSON 结果出）：

```bash
echo '{"action":"inspect","path":"/abs/path/parcels.geojson"}' | claude-gis run --workdir .
```

## 路线图

- [x] 矢量核心：查询度量 / 邻近 / 叠加 / 分区统计，中间步骤可复用，14 道基准题
- [x] 实测加固：全链路 UTF-8、时间字段、轨迹、分组度量、坐标系策略
- [x] 栅格核心：22 个 `raster.*` 算子
- [x] Claude Code mod：对话内结果卡片、侧边面板、坐标系切换
- [x] C++ 原生内核：单个可执行文件，不再依赖 Python
- [ ] 专题图：在 SVG 地图卡片上做分级设色 / 分类着色与图例
- [ ] 栅格进阶：插值、坡度坡向、水文、视域
- [ ] 网络分析与空间统计

## 新能力的两问

1. 它属于现有七类分析中的哪一类？
2. 为什么现有原语组合不出来？

第二问成立才新增类别；否则只加算法、后端或优化——**不加新工具**。

## 文档

- [架构说明](docs/architecture.md)
- [工具 API 契约](docs/tool-api.md)

## 许可

原生内核依赖 GDAL（MIT）、GEOS（LGPL-2.1，静态链接时请留意其条款）、PROJ（MIT）、nlohmann/json（MIT）。
