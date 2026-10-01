<p align="right">
  <strong>简体中文</strong> · <a href="CHANGELOG.md">English</a>
</p>

# Changelog

## 2026-10-01

- 把"取数失败"从猜变成可查。屏幕上以前只会写 `DIRECT FAIL`,而域名解析失败、连不上主机、TLS 握手
  失败、证书被拒、Key 无效、响应格式变了——这几种完全不同的病因在屏幕上长得一模一样。新增
  `main/we_diag.{h,c}` 记录每次刷新的快照:卡在哪一步,以及每个平台的 `esp_err_t`、mbedTLS 错误码、
  证书校验标志、HTTP 状态码、响应体预览、尝试次数与耗时,再带上空闲堆 / 历史最小堆 / 最大连续块
  (这台无 PSRAM 的芯片上,TLS 握手能不能成基本由"最大连续块"决定)。失败行现在把短码顶在金额位置
  (`DNS` / `CONN` / `TOUT` / `TLS` / `X509` / `401` / `JSON` / `NOKEY` / `NOSTAT`),状态行写
  `DIRECT FAIL 401`。门户新增 `GET /diag`:同一个 Basic Auth 之后的纯文本页面(`no-store`),
  打印这份快照 + 设备当前**实际生效**的配置(WiFi SSID、各平台 Key 的长度与尾号,以及"存进 NVS 的
  Key 含空白/控制字符"的显式告警),配置页上有入口,整段复制就能发出来。

- API Key 在保存时、以及每次发请求前各清洗一遍:丢掉所有 ASCII 空白与控制字符,并剥掉一起被复制
  进来的 `Bearer ` 前缀。带尾随换行的 Key 会被塞进 `Authorization` 头,服务端直接按非法请求拒掉,
  在屏幕上和"Key 填错了"完全无法区分。只剩空白的输入按"没填"处理,不会覆盖已存的 Key
  (与增量保存的语义一致)。

- 取数加固:首轮一个平台都没成功时整组重试一轮(`FETCH_TRIES`);刷新任务栈从 6KB 提到 8KB
  (mbedTLS 握手很吃栈);SNTP 配三台服务器(`CONFIG_LWIP_SNTP_MAX_SERVERS=3`,即
  `ntp.aliyun.com` / `cn.pool.ntp.org` / `time1.cloud.tencent.com`),避免单台 NTP 被拦之后
  时间一直校不上却又没有任何提示。

- 更正上一条里的错误结论。ESP-IDF 5.5.3 的 `CONFIG_MBEDTLS_HAVE_TIME_DATE` 默认是 `n`,也就是
  **本固件根本不校验证书有效期**,时间从来不是 TLS 握手失败的原因。发请求前先校时这个顺序仍然保留
  (`UPDATED` 行、记账天界都用得到,而且顺带证明 DNS/UDP 通不通),但它不是修好 `DIRECT FAIL` 的原因。

- host 测试:新增 `tests/test_we_diag.c`(错误/状态短码映射、快照往返、超长串截断、极小缓冲与空指针),
  `tests/test_we_cfg.c` 补上 Key 清洗与"增量更新 Key"的用例;`tools/validate.sh` 会跑新套件。

- 修掉诊断本身的一个 bug(v1.1.3):`esp_http_client_get_and_clear_last_tls_error()` 的**返回值**
  才是 esp-tls 的分类码(`0x8000` 段,说明断在哪一层),之前把它当成"成功/失败标志",非零时反而把
  出参清零——真失败时 `/diag` 的 `tls_code` 恒为 0,诊断页在最需要它的时候恰好是瞎的。现在分类码
  存进 `we_diag_row_t.tls_err`,屏幕短码由 `we_diag_tag_from_pair()` 在两个错误码里挑更有信息量的
  那个(分类码优先于笼统的 `ESP_ERR_HTTP_CONNECT 0x7002`)。同时新增**独立网络自检**:整轮取数
  全失败后,用 `esp_tls` 的 `is_plain_tcp` 只做 DNS+TCP 重跑同一条连接路径(不握手、不申请大缓冲,
  也是一次内存低压探测),用自持的错误句柄把 `0x8001`(域名没解析)/`0x8004`(TCP 连不上)/
  `0x8006`(超时)和 socket errno(111 被拒 / 110 超时 / 101 无路由 / 113 主机不可达 / 104 被重置)
  分开,并快照本机 IP、网关与 DHCP 下发的 DNS,一并打印进 `/diag` 的"网络自检"段——`0x7002` 这个
  兜底码终于说得出人话。

- 用 v1.1.3 的诊断抓到 `DIRECT FAIL` 的真凶并修掉(v1.1.4):实测 `/diag` 显示
  `heap: free=81444 min_free=176 largest_block=29696`——总空闲 81KB,但**最大连续块只有 29.7KB**,
  历史最低堆一度只剩 **176 字节**。mbedTLS 握手要分配一整块 16KB 接收缓冲(IDF 默认
  `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384`)再叠 4KB 发送缓冲,在这种碎片化的堆上直接分配失败,
  于是屏幕只剩一个 `TLS` 短码。原版固件没有 mDNS/门户/备份/诊断这些常驻开销,同一台设备自然握手得动
  ——这就是"原版能连、社区版连不上"的完整解释。修复(全部是 sdkconfig 层面,不改取数逻辑):
  RX 缓冲 16384→6144(余额接口响应只有几百字节,证书链通常 4KB 左右,单条 TLS 记录极少超过 4KB)、
  TX 缓冲 4096→1024(请求体为 0,只发 ClientHello)、握手后不再保留对端证书(再省约 4KB)。
  合计把握手峰值砍掉约 17KB。`/diag` 的 heap 行现在会在最大连续块不足 24KB 时显式标注
  "连续块不足,握手大概率失败"。

## 2026-09-05

- **feature/community-skeleton**:Token 余额玩法社区版——SoftAP 配置门户(`we_portal`)、配置模型(`we_cfg`)、平台适配层(`we_provider`,DeepSeek/Kimi HTTPS 直连余额)、动态平台行、锁屏签名昵称化;新增 `we_cfg`/`we_provider` host 测试。
- 本次改动未触碰基线看板固件:多 WiFi 自动找网、host 取数、锁屏、分级调光均维持原状。


## Unreleased

- 修掉配网后刷新余额只报 `DIRECT FAIL` 的问题。两个互相独立的原因：其一，门户保存是在空结构上
  重建配置（`we_cfg_init`）而不是合并进已存配置，而表单的 API Key / WiFi 密码输入框又从不回填，
  于是「只填了 WiFi」这一次提交会把已配的平台 Key 全部抹掉——刷新时没有可查对象，余额页只剩
  `DIRECT FAIL`。现在保存改为在已存配置上做增量合并（新增 `we_cfg_set_wifi` /
  `we_cfg_set_prov_key` / `we_cfg_del_prov`）：字段留空即保持原值（WiFi 密码留空会沿用该 SSID
  的旧密码），删除平台需勾选行内新增的「清除该平台」复选框；平台行会显式标出是否已配置
  （`已配置(尾号 cdef),留空保持不变`），保存出 0 个平台 Key 时打一条告警日志。其二，SNTP
  等待原本排在取数循环**之后**，第一次刷新因此拿不到 `UPDATED` 时间，记账天界也是用 1970 的时钟
  算出来的；现在改为发请求前先校时。（本条原本把证书包判成"每张证书都尚未生效"——那是错的，
  见 2026-10-01 那条。）一个平台 Key 都没有时页面直接显示 `NO API KEY`，不再误导成网络问题。

- 干净检出即可完成固件构建：`tools/validate.sh --firmware` 现在先 `reconfigure` 拉取依赖，再重打 `esp_lvgl_port` 的 FAP_SCREENSHOT_V1 取帧钩子补丁（`scripts/patch-esp-lvgl-port.py`），最后才编译——`managed_components/` 不入库，重新解析依赖会覆盖打过补丁的源码。补上 `.github/workflows/build-firmware.yml` 与 `.github/workflows/static-checks.yml`，打 tag 即可构建并发布合并固件；`espressif/mdns` 钉到 1.13.1，让依赖解析可复现。补丁脚本改为按代码形态定位锚点，锚点缺失即让构建失败——此前上游改动后会写成「改了一半」的源码，编译照过、只在链接期报 `lvgl_port_display_set_snapshot_cb` 未定义。

- 门户新增配置与记录的整份备份/恢复：`GET /backup` 下载一个 JSON 文档，内含完整 `we_cfg`
  配置(nickname、Wi-Fi 档案、平台 API Key)与 `balhist` 逐日记账(各平台每日基线，31 天)；
  `POST /restore` 把两者一起写回 NVS 并重启。**两个 blob 必须一起写**——`hist_check_rows()`
  一旦发现平台顺序与 `row_id` 不符就会清空全部历史，只恢复 Key 等于白丢热力图。
  记账数据模型抽到新头文件 `main/we_hist.h`，使新增的 `main/we_backup.c` 能直接读写该 NVS blob，
  不必伸手进 `app_balance.c` 内部。WiFi 与 API Key 字段超长一律拒绝而非截断；
  恢复被拒时 NVS 保持原样不受破坏；导出时余额四舍五入到 4 位小数以保持文件可读，
  `days` 按日期升序排列。配置页新增「备份与恢复」区块(下载链接 + 文件上传)，
  HTTP 服务栈由 6KB 提升到 8KB。

- 配置门户由「热点配网」改为「局域网后台」：设备先用已保存的 WiFi 档案以 STA 模式接入家庭网络，
  屏幕显示局域网地址与 mDNS 短名（`http://token-journey.local/`）；只有在全部档案都失败或尚未配网时
  才回落 `BAL-XXXX` 热点，保证新机也能完成配网。由于局域网内任何设备都能访问门户（可改写配置），
  统一加了 HTTP Basic Auth（用户名 `admin`，密码 `12345678`）；并在空闲 5 分钟后自动关闭，
  避免长时间常连 Wi-Fi 耗电（STA 常连约 80-100mA）。新增 `espressif/mdns` 组件；
  修复 `demo_radio_network_prepare()` 对 `ESP_ERR_INVALID_STATE` 的容忍，使门户与余额刷新能共用网络栈；
  门户占用无线时余额刷新自动跳过本轮。

- Token Journey 看板布局调整:主页电量胶囊移到 `174,11` 并缩小为 `50×19`(圆角改为按 `h/2` 推导),
  图例两端文字改为各自定位(`HIGH` 在 `15,283`、`LOW` 在 `194,283`),
  `bat_pill_create()` 改为由调用方传入几何参数(余额页保持 `184,8,50×22`);
  胶囊与图例坐标抽为 `BAT_PILL_*` / `LEGEND_*` 宏,可与可视化布局编辑器往返回写。

- 将小程序 BLE 安装兼容提升为二创模板强制契约：固定保护 `cardid`/Recovery 分区，
  保留上键持续 5 秒进入 Recovery 的 bootloader hook，并在 CI 强制校验合并镜像结构、
  分区表 MD5/范围、3 MB 应用上限和保护分区数据不入包。
- 规定多应用发布的 Release 标题约定：tag 按 `v<版本>-<应用名>`（如 `v0.1.0-voice-keychain`）命名，让 Release 标题同时带版本与应用名；发布成功后核对标题，保证一眼扫 Release 列表就能区分是哪个应用。
- 新增发布后收尾流程：`issue-suggestions` skill 用于把用户反馈作为 issue 提交到上游项目；`experience-pr` skill 用于把可复用的开发经验作为文档 PR 提交；新增 `docs/experiences/` 目录保存单条经验文件；并配套 `project-completion`、`file-issues` 与经验索引文档。
- 精简仓库根目录：将 GitHub 可识别的社区治理文档迁入 `.github/`，将变更记录迁入 `docs/`，同步全部引用，并在仓库检查中加入根目录文档白名单。
- 全仓库文档语言规范：所有维护中的 Markdown 默认 `.md` 文件使用英文，简体中文使用配对的 `.zh_CN.md`，双方提供语言切换；静态检查会阻止缺失配对、缺失切换链接或英文默认页混入中文正文。
- AI 开发流程一期：精简按任务加载的上下文入口，统一本地/CI 验证脚本，新增 PR 自动构建与模板，并提交依赖锁文件以提高构建可复现性。
- PR 审查修复：GitHub Actions 固定到完整 commit SHA，构建与发布 job 按最小权限拆分，同步 checkout 关闭凭证持久化；补充 Feature Request / Usage Question issue 表单；启用并修正私密安全报告兜底说明；清理 README 路径、CI 触发条件与历史分支描述漂移。
- 语言规范变更：commit 标题、PR 标题与 body 由"默认中文"改为**使用英文**（`docs/contribution/commit-and-pr.md` 更新）；中文写作规范（全角标点）适用范围剔除 PR/MR 描述（`doc-conventions.md` 更新）。
- CI 构建改造：`build-firmware.yml` 显式传入 `SDKCONFIG_DEFAULTS=sdkconfig.defaults` 再 `idf.py build`，由 defaults 启用自定义分区表（`CONFIG_PARTITION_TABLE_CUSTOM=y`，文件名为 `partitions.csv`）；`CONFIG_ESPTOOLPY_HEADER_FLASHSIZE_UPDATE` 改为 `n`，再用 `idf.py merge-bin -o build/FoloToy-AI-Passport-full.bin` 合并可直刷完整固件；产物精简为仅 full.bin；`actions/cache` 升级到 v5 以消除 GitHub Actions Node.js 20 弃用警告；CI 文档同步更新。
- 合并上游 PR #6（wireless-low-power-demos）以解决 PR #4 冲突：引入无线/低功耗 demo（`main/demo_wifi.c`、`demo_ble.c`、`demo_radio.c`、`demo_low_power.c`）、`partitions.csv`（NVS/PHY/3 MB factory-app 分区）、`main/CMakeLists.txt`/`main.c`/`demo.h`/`sdkconfig.defaults` 更新；同步硬件指南的 Wi-Fi/BLE/低功耗章节；README 能力契约表补充 Wi-Fi/Bluetooth LE/Low power 三项（中英双语）。
- 提交规范补充：`docs/contribution/commit-and-pr.md` 明确 PR 标题与 commit 标题使用相同的 Conventional Commit 格式和英文祈使句，不用名词短语当标题。
- CI 与文档清理：`sync-main.yml` 移除 `test_mode` 残留模板注释；`docs/development/coding-conventions.md` 将「Redis TTL」条目泛化为「缓存组件」条目（当前固件无 TTL 约束需求，消除从模板带入的无关约定）。
- 补充通用规范（借鉴 Shinku）：`docs/contribution/doc-conventions.md` 新增中文全角标点规范（正文 `，`；`（`）`，代码/命令/路径保留英文原样）、凭证不入仓规范（token/密钥/私钥绝不入仓，提交前 git diff 扫描敏感前缀）、文件删除安全规范（删除走系统回收站，不用 rm -rf/git clean -fd）。
- 代码注释规范强化：`docs/development/coding-conventions.md` 补充完善注释要求——函数说明（用途/参数/返回值/副作用/线程上下文/内存所有权/初始化顺序）、变量说明（语义/取值范围/生命周期/同步要求）、逻辑注释（状态机/时序/寄存器/魔数依据），覆盖范围宁多勿少，中文注释保留英文技术术语。
- 文档去 AI 化：`docs/README.md` / `docs/README.zh_CN.md` 移除 AI 专属章节（Entry point、Source-of-truth、提需求格式、BSP 边界、Runtime invariants、验收交付格式、构建命令），README 只保留给人看的项目介绍、硬件能力契约、demo 案例与项目结构；构建命令章节删除（与 `docs/development/build-and-test.md` 重复）。
- 新增 `docs/development/agent-guide.md`：集中承载"AI 如何在本仓库工作"（上下文建立顺序、事实来源优先级、提需求格式、BSP 边界、运行时规则、交付格式），并链接 build-and-test 与硬件指南，不重复构建命令与验收矩阵。
- 同步更新索引：`AGENTS.md` 规则索引新增 agent-guide 条目；`docs/INDEX.md` 与 `docs/development/README.md` 新增 agent-guide 索引行。
- 文档补充：`docs/fork-guide.md` 说明「为什么根目录不放置 README」——根目录 README 预留给 fork 开发者自行放置（上游留空），fork 后可将自己的内容写入根目录 `README.md` 介绍 fork 后的项目；GitHub 显示优先级（根 README > docs/README.md）契合该预留意图。
- 分支合并：创建 `main-update` 分支（基于与上游一致的 main），将 `feature/repo-structure`、`ci/build-firmware`、`ci/sync-main` 三个分支合并进来，统一 docs 结构（CI 文档归入 `docs/development/`，workflow 文件随 ci 分支引入 `.github/workflows/`）；解决 development/software-design README 的 add/add 冲突。
- 合并后审查修复：`docs/INDEX.md` 补充 CI 文档索引；`docs/fork-guide.md` 修正 workflow 引用为 `.github/workflows/sync-main.yml`；`docs/README` 双语项目结构块补充 `.github/workflows/` 与 CI 文档说明。
- ci 分支 CI 文档路径调整：`ci/build-firmware` 的 `docs/software-design/CI-build-and-release.md` 与 `ci/sync-main` 的 `docs/software-design/CI-sync-main.md` 均移入各分支的 `docs/development/`（CI 属工程规范）；`docs/software-design/README.md` 保留为软件设计索引；feature 分支的 software-design 索引同步更新引用。
- fork 补充文档目录迁移：`assets/docs/` 移至 `docs/assets/`（文档素材归入 docs/ 更合理），新增 `docs/assets/.gitkeep` 空目录占位；同步更新 AGENTS.md / INDEX / doc-conventions / fork-guide 的路径引用。
- 文档结构调整：根目录不再放 README——上游英文 README 移入 `docs/README.md`、中文移入 `docs/README.zh_CN.md`（GitHub 从 docs/ 识别主 README）；原 `docs/README.md` 根总索引更名为 `docs/INDEX.md`；同步更新 AGENTS.md / CONTRIBUTING / SUPPORT / fork-guide / doc-conventions 的路径引用。
- 初始化项目文档：新增 `AGENTS.md`、`CLAUDE.md` 和 `CHANGELOG.md`。
- 仓库结构规整：上游英文 `README.md` 更名为 `README.en_US.md`，保留 `README.zh_CN.md`。
- 新增目录骨架：`docs/`（software-design / hardware-design）、`assets/`（fonts / images / music，各含 `README.md`）、`skills/`。
- 将上游硬件开发指南归位到 `docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md`。
- 文档规范：子目录 readme 统一为大写 `README.md`；补充 fork 用户约定（main 只动根 README）。
- 扩展 fork 用户约定：`main` 分支允许修改根目录 `README.md` 和 `assets/docs/`（README 不足以说明项目时存放补充文档与素材）。
- 新增 `assets/docs/` 目录约定：上游 main 只保留空目录 `.gitkeep`，内容文件仅存在于 fork；使用方法规范写入 AGENTS.md「给 fork 用户」约定。
- CI 文档迁移：`docs/software-design/CI.md` 从本分支移除，迁至 `ci/build-firmware` 分支并改名为 `docs/software-design/CI-build-and-release.md`。
- 补充 `main` 分支策略说明：解释 `main` 保持干净的两大原因（与上游同步无冲突 + 多小项目按分支整理）；例外——执意 main 开发需停用 CI 自动同步；提醒 fork 用户默认 action 关闭需手动启用（此条为整个 CI 的通用要求，统一写入 AGENTS.md）。
- 文档拆分：将 `AGENTS.md` 按主题拆为公共文档——新增 `docs/contribution/`（doc-conventions.md、commit-and-pr.md）与 `docs/development/`（build-and-test.md、coding-conventions.md），新增 `docs/fork-guide.md`；`AGENTS.md` 精简为简介 + 项目概述 + 必读文档索引。
- 同步更新索引：`docs/software-design/README.md`、`README.en_US.md` / `README.zh_CN.md` 的 `docs/` 目录说明。
- 参考 cindy 仓库文档组织完善索引：新增 `docs/README.md` 根总索引；AGENTS.md 规则索引按触发场景改写（附触发条件）；`docs/contribution/` 与 `docs/development/` 的 README 补充收录标准。
- 引入社区治理文档（参照 cindy 改写，放仓库根目录）：新增 `CONTRIBUTING.md` / `.zh_CN.md`（贡献指南，针对 ESP-IDF/AI agent/fork 场景改写）、`CODE_OF_CONDUCT.md` / `.zh_CN.md`（贡献者公约）、`SECURITY.md` / `.zh_CN.md`（安全报告流程）、`SUPPORT.md` / `.zh_CN.md`（支持渠道）；AGENTS.md 与 docs/README.md 同步引用。
