# Quota OS（版本 016）

基于三级资源声明的微内核，保证一级任务永远不卡顿、二级任务运行不卡顿，三级任务则不保证。

A microkernel built on three-tier resource declarations: tier-1 tasks never stall, tier-2 tasks run without stalling, and tier-3 tasks get no guarantee.

AArch64，跑在 QEMU `virt` 上，用真实的 ARM 通用定时器和 GIC 中断。还是原型：单核、无 MMU 隔离、没有真实设备驱动，不能用于生产。

License: [Apache-2.0](LICENSE)

## 现在能直接跑的映像

`prebuilt/quotaos.bin` 是白底彩色四组菜单，软件指针仍是黑底白边。主页只放已经能跑的入口。右侧把完成样本和未完成观察分开：done max / p99 只含完成作业，not_started、unfinished 和 open bound 另计。不传 `-append` 就进这个界面。固定配置仍是 virt、GICv2、Cortex-A72、128 MB。横幅 `Version: 016 (66a295b)`。`66a295b` 是包含这版字节的提交的父提交。

sha256：`quotaos.bin` `6feb8d4b5318d425c2f64da7c5ef192617a5031ca0fb07e48733239d25737bf5`，`quotaos.elf` `bfc82905c12b13006fb422caa214a379baa89fd7429cdea4372dbde0d212dea8`。685480 字节。`prebuilt/quotaos-lvgl.bin` 是同一份拷贝。

上一份精简菜单是提交 `66a295b`，682896 字节，sha256 `1c91348a3e0935f677ea8ccf0ecdc8e1b75c82ce313daf7fd84b0169a3332164`。它的面板 lat max 只统计完成作业。分组菜单 `60fa448` 是 682232 字节，sha256 `dc71eb9a2ec4124d79c13b5e506d11f6614a124d35abf91fcb6278d09d2dfded`。白底指针里程碑仍是 `ac565ac`。这几份都不是现在的默认文件。

上一份串口内核完整留在 `prebuilt/quotaos-serial.bin`（93744 字节，sha256 `afed7949f5b3adc6dcb3d6c86e05356ed69fd315eca55fbeafae656f62880e05`，elf `bf6338345233296ffac12393f37e477078299ec91c65ce68ab90fa5e656b8f64`，横幅 `Version: 016 (af050b5)`）。下面历史段落里的 e4 日志对应的是那份串口内核，不是现在的默认文件。

要看见分组并点击，QEMU 需要这三个设备。下面这条命令在 QEMU 8.2.2 上跑过。主页点 tier-3 load，跑菜单 9 的 200 周期。同一脚本修前修后各跑宿主时钟 3 轮和 icount 3 轮。修前的 `66a295b` 在宿主 host-03 上有 3 次 `display-vsync` not_started（1404、1379、2532 us），面板 lat max 只有 359 us。修好后宿主 host-01 是 T1 miss 5、class a 5、done max 451 us、样本 395、open bound 1823 us、not_started 4、unfinished 1。icount 三轮都是 miss 0、done max 80 us、样本 400。class a 照算，500 us 和 300 us 的槽长没有放宽。这不是 30 轮验收，也不是硬件隔离。e4 不代替这次 GUI 压力。到屏幕发光未测，页数未测。截图 `docs/v016/lvgl/timing-host-01.png`。修前日志 `docs/v016/lvgl/timing-before/`，修后 `docs/v016/lvgl/timing-after/`。

```bash
qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a72 -m 128M \
  -display none -nic none \
  -device ramfb \
  -device virtio-keyboard-device \
  -device virtio-tablet-device \
  -kernel prebuilt/quotaos.bin
```

QEMU 会打印 `Could not open option rom 'vgabios-ramfb.bin'`。客户机仍然写了 `etc/ramfb`，`screendump` 拿到了上面的画面。

不加大设备、也不加 `-append` 时，串口是 `[boot] no bootarg, starting LVGL`，然后 `[fb] etc/ramfb not in fw_cfg`。这不是 `select>`。串口菜单只要 `-append serial`。`-append e4` 仍是 e4：同一份精简映像上 item9 PASS、item10 PASS、QA DONE（`docs/v016/lvgl/e4-slim.raw`）。这不是上面的 GUI 压力结果。串口菜单没有删，`-append serial` 仍是原来的 18 项。未接入的 6、10、13、14、15、16、18 写在覆盖范围页，不占主屏。Tier-1 仍是 13312 KB、1500 us。这一屏不是一级任务的硬件隔离。启动行仍是 EL1、MMU 关、一个内核地址空间。

文档里的 UTM 会删掉 Display，所以只换 `quotaos.bin` 不能承诺屏幕上有画面。UTM 这次没有实测，下面不写新的菜单名。

## 版本号规则

- 一个版本 = 一次提交，源码和 `prebuilt/` 一起提交。版本号在 `include/quotaos.h` 的 `QUOTAOS_VERSION`，每个版本提交 +1（三位数字）。提交说明以 `v007:` 这样的前缀开头。
- 启动横幅第二行是 `Version: 007 (<7 位提交号>)`。提交号由 `Makefile` 在编译时取 `git rev-parse --short=7 HEAD`。
- **横幅里的提交号 = 这个版本据以编译的提交，也就是版本提交的父提交。** 核对：`git rev-parse --short=7 <版本提交>^` 应等于横幅里的提交号。例：v007 的横幅是 `Version: 007 (82db71a)`。
- 复现某个版本的 `prebuilt/quotaos.bin`：在版本提交上用 `make SRC_COMMIT=<父提交>` 编译。例：在 f9119e9（v007）上 `make SRC_COMMIT=82db71a` 得到 sha256 `4672719363ac3fdde2d425dbd69a12e9e3241486e6d0bb50250e05d5c9557067`，与 v007 的 `prebuilt/quotaos.bin` 相同。直接 `make` 会把版本提交自己的提交号编进横幅，sha256 也就不同（f9119e9 上是 `7eaad174…`）。

## 进度

| 提交 | 内容 | 状态 | 证据 |
| --- | --- | --- | --- |
| A `bcbe7b5` | 清理：删掉探针、诊断菜单和策略开关，行为不变；构建可逐字节复现 | 已验收 | `docs/regress-A/`（菜单回归、行数统计） |
| B `5b82525` | Tier 1 截止期从理想周期起点记账，跳过和未完成都算错过 | icount 30 轮 0 次错过；**普通模式验收未通过，原因排查中** | `docs/tier1-B/`，见下文「Tier 1 截止期回归」 |
| C `252a90d` | 从设备树识别 GIC（中断控制器）v2 / v3 | 已验收 | `docs/gic-C/` |
| `d03cd22` | 启动横幅和 README 换成上面的定位句 | 已推送 | — |
| D（v007） | 严格时间分区（Tier 1 时间槽表）、系统预留、准入含保护时间、十个按意图命名的演示、T1MISS 行带 `J_us=` 和 class a/b | icount 下菜单 5–10 的 Tier 1 错过 0 次、起点抖动 0 tick、槽内 Tier 2/3 0 tick；普通模式照实列出，只作参考 | `docs/accept-D/`、`docs/regress-D/`，见下文「v007 验收」 |
| E1（v009） | 统一声明「层级，周期内占用 B」：CPU、内存、两个模拟设备的占用时间；按资源准入；不能打断的设备申请 Tier 1 被拒并写出缺的条件；超额被拒并写出资源名 | 第 2、4 项 icount 30 轮全部 PASS | `docs/accept-v009/`，见下文「v009（E1）」 |
| E2（v010） | 预算令牌：驱动（被动服务端）替谁干活就扣谁的预算，IPC 内核路径也记在调用方；设备 Tier 1 准入只用出厂声明值，拒绝列出全部缺的条件 | 第 8 项和 E1 各项 icount 30 轮 × GICv2/GICv3 全部 PASS | `docs/accept-v010/`，见下文「v010（E2）」 |
| E3（v011） | 设备闸门、超时强制中止并降到 Tier 3、健康监测（实测超出声明）和按策略处理已有 Tier 1 应用、首页提示、中断风暴限流、少报模式；另修 IPC 路径里定时器中断被推迟（v010 的 342 tick） | 第 1、3、5、6、7 项和策略检查 icount 30 轮 × GICv2/GICv3 全部 PASS | `docs/accept-v011/`，见下文「v011（E3）」 |
| E4（v012） | 内存带宽按周期限流（模拟访存计数，超额停到下个周期、不欠账）、缓存着色（每个 Tier 1 任务独占自己的颜色）；另修 v011 的槽前保护区余量（e2 里 0 tick）、出口尾巴里到期的中断，菜单 13 的 IPC 内核路径分三段并做整窗核对 | 第 9、10 项 icount 30 轮 × GICv2/GICv3 全部 PASS；真实隔离效果：缺真实板子 | `docs/accept-v012/`，见下文「v012（E4）」 |
| v013 | 菜单 13 两种计数口径逐项对账（窗口起止、窗口前后余量）；Tier 2 调用者的 IPC 内核路径超出部分改记调用者准入时多留的余量，系统预留 0 | e2 30 轮 × GICv2/GICv3 `E2 item8 PASS`，对账误差 0 | `docs/accept-v013/`，见下文「v013」 |
| v014 | 用过 IPC 余量的调用者本周期新调用扣到下个周期；演示 18（同一负载先不扣、再按规则） | `E2 headroom PASS` 30 轮 × GICv2/GICv3：每个调用者每周期最多用余量 1041 tick（准入 7500），两轮都用余量的调用者周期 0（不扣时 318） | `docs/accept-v014/`，见下文「v014」 |
| v015 | 扣不扣看预算主人，服务端替调用者转发的调用也扣；演示 18 加「Tier 2 经转发服务端」；内核每周期最大值按菜单和进入原因归因，窗口起点少清空槽 | `E2 headroom PASS` 和 `E2 headroom fwd PASS` 30 轮 × GICv2/GICv3：一个预算主人一个周期最多用余量 1005 / 1310 tick（准入 7500），用两次及以上的周期 0（不扣时 312 / 395）；qa 开机以来内核每周期最大 20355（预留 21875，余 1520） | `docs/accept-v015/`，见下文「v015」 |

分支 `rework-partitioning`，对比：https://github.com/leiquan/quota-os/compare/main...rework-partitioning

## 三级资源规则

| | 谁声明 | 规则 | 现在的代码 |
| --- | --- | --- | --- |
| 系统预留 | 内核 | 排在 Tier 1 前面的固定 CPU 和内存，不外借 | v007：内存 16 MB，CPU 350 us/周期（icount 实测最坏 15290 tick ≈ 245 us 加余量） |
| Tier 1 | 厂商，写死在映像里 | 每个任务一个固定值，写进按周期触发的时间槽表；用不完的槽空着，不借给任何人，也不许超过 | v007：时间槽表写死在 `kernel/resource.c`，槽尾由定时器掐断，空槽不外借 |
| Tier 2 | 应用清单 | 最低值（保底）每周期给足；建议值（弹性）只来自空闲，有压力时收回；先问用户 y/n 才启动，停止就释放 | 已实现（代码里叫保底/弹性） |
| Tier 3 | 不声明 | 系统决定，只分剩下的，碰不到上面三样 | 已实现（轮转） |

准入按每种资源各算一遍：**系统预留 + 一级 + 二级保底之和 + 保护时间 ≤ 100%**。放不下就拒绝，并说明先关掉哪个应用（`REFUSED: please close … first`）；请求本身超过整机容量，说 `device does not meet minimum requirements`（旧设备场景）。v007 只算 CPU 和内存两种资源，CPU 已含系统预留和每个槽前 10 us 的保护时间（4 个槽共 40 us）；其他资源在 E 里。v010 起每个槽前的保护时间改为 20 us（4 个槽共 80 us），原因见「v010（E2）」。二级只算最低值（保底）：建议值不计入准入，只从空闲里给，有压力时收回。

## 演示菜单

d03cd22 及以前的菜单是：1 资源表、2 启动 Tier 2（先问 y/n）、3 关闭 Tier 2、4 优先级演示、5 分级开关、6 失范隔离、7 声明超过本机、8 内存压力、10 几十个 Tier 2 切片。

v007 起换成十个按意图命名的演示：

| 新 | 演示 | 对应的旧菜单 |
| --- | --- | --- |
| 1 | 资源总览（含系统预留、每个 Tier 1 的预留和实际最多用量） | 1 |
| 2 | 安装应用：允许/拒绝，也可以停止 | 2、3 |
| 3 | 资源不够：拒绝并说先关哪个 | 2 里的拒绝分支、10 |
| 4 | 旧设备：超过整机容量被拒 | 7 |
| 5 | Tier 1 准时：抖动和错过 | 4 |
| 6 | Tier 1 超时在槽尾被掐断 | 6 的一部分 |
| 7 | Tier 1 槽从不外借 | 新增 |
| 8 | Tier 2 最低值有保证，建议值在压力下收回 | 8 |
| 9 | Tier 3 过载不影响上层 | 4 |
| 10 | 任何一级的失控任务只拖慢自己 | 6 |
| 11 | 设备申请 Tier 1：缺的条件全部列出（v009 起） | 新增 |
| 12 | 超额申请被拒，说明是哪种资源（v009 起） | 新增 |
| 13 | 预算令牌：驱动替谁干活就扣谁的预算（v010 起） | 新增 |
| 14 | 设备超时：强制中止，设备降到 Tier 3（少报耗时的设备，v011 起） | 新增 |
| 15 | 中断风暴：多出来的中断被限流并计数（v011 起） | 新增 |
| 16 | 带宽限流：超额就停到下个周期，不欠账（v012 起） | 新增 |
| 17 | 缓存着色：每个 Tier 1 任务独占自己的缓存颜色（v012 起） | 新增 |
| 18 | IPC 余量：用过余量的调用者本周期新调用扣到下个周期（先不扣跑一遍对照，v014 起）；v015 起再跑一组「Tier 2 经转发服务端」（调用者 → 服务端 A → 服务端 B，都扣调用者） | 新增 |
| — | 分级开关（OFF 对照） | 5，删除：严格分区没有「关掉」这一档 |

## 资源控制设计（E 已实现：v009–v015，QEMU 上验证；真实硬件隔离效果缺真实板子）
- **统一声明**：所有资源（CPU、内存、设备占用时间、内存带宽、缓存分区）都用「周期 P 内占用 B」声明，三个层级的含义对每种资源都一样。
- **准入**：每种资源各算一遍：一级预留 + 二级保底之和 + 系统预留 + 保护时间 ≤ 容量。任何一种超了，就拒绝启动，并说明超的是哪一项。
- **一级条件**：一级用到的每样资源，要么独占，要么同时满足三条：能切分、能强制停下（从下令到停住的最长时间实测过，并算进保护时间）、最坏耗时有上限并且实测过。不能打断的设备一律不进一级。
- **预算令牌和设备闸门**：驱动替谁干活就扣谁的预算。闸门在放行前检查最坏耗时加切换开销是否放得下；实际耗时由系统自己测，超时就强制复位，并把设备降到三级。
- **共享硬件**：内存带宽按周期限制访问次数，缓存用着色分区，都按最坏情况留余量。
- **不在本设计范围内**：硬件故障靠看门狗和降级显示兜底；实际隔离效果要在真实的 MPAM 板子上验证，QEMU 上只能验证规则逻辑。

（v009 起统一声明和按资源准入已经实现，覆盖 CPU、内存和两个模拟设备，见下文「v009：E1」；v010 实现了预算令牌（驱动替谁干活就扣谁的预算），并把设备 Tier 1 准入改成只用出厂声明值，见「v010（E2）」；v011 实现了设备闸门、超时降级、健康监测和中断风暴限流，见「v011（E3）」；v012 实现了带宽限流（访存计数是模拟的）和缓存着色，见「v012（E4）」。真实硬件上的隔离效果没有验证：缺真实板子。）

补充（前两条 v011 实现，见「v011（E3）」；第三条 v012 实现，见「v012（E4）」）：

- 新设备一律从 Tier 3 开始。设备描述（出厂写死）四项：单次作业最长耗时、切换开销、从下令中止到停住的最长时间、能否打断。准入只用声明值（v010 已实现）；系统实测只用来核对，实测超出声明算设备故障（健康监测）：记一行日志（设备名、实测、声明、周期号），设备降出 Tier 1，之后的 Tier 1 申请以「实测超出声明」被拒；已在该设备上的 Tier 1 应用同时按各自配置（降级或报警）处理，每个应用记一行；首页显示提示；普通模式下这些日志行标「非 icount，仅供参考」。
- 超时：强制中止/复位，作业丢弃并计数，设备降到 Tier 3，记一行日志（设备名、实测耗时、声明耗时、周期号）。
- 内存带宽按 MemGuard 的做法：每周期访问次数预算，超了就停到下个周期，下个周期额度恢复，不欠账。Tier 1 的物理页按缓存颜色分配。

E 的验收项（测试给出，QEMU icount 模式每项 30 轮；第 2、4 项已在 v009 实现并通过，见「v009（E1）」；第 8 项在 v010 实现并通过，见「v010（E2）」；第 1、3、5、6、7 项在 v011 实现并通过，见「v011（E3）」；第 9、10 项在 v012 实现并通过，见「v012（E4）」）：

1. 少报耗时、但能打断的模拟设备：Tier 1 错过 0 次。
2. 不能打断的设备申请 Tier 1：100% 在准入时被拒，并说明缺哪一条。
3. 超时的设备被降到 Tier 3。
4. 超额申请在启动时被拒，并说明是哪种资源。
5. 中断风暴（每秒几万次）：Tier 1 错过 0 次，多出来的中断被限流并计数。
6. 每次实测的中止/复位时间都不超过设备描述里的上限，也不超过保护时间。
7. 降级日志行包含设备名、实测时间、声明时间、周期号。
8. 令牌记账：应用预算减少量等于实测的驱动耗时（误差 1 tick 内），驱动自己的预算不变。
9. 带宽限流不欠账：超额的周期被停住，下个周期额度恢复。
10. 每个 Tier 1 物理页的缓存颜色都正确。

在真实 MPAM 板子上对比「Tier 1 单独跑」和「Tier 3 疯狂访存」的隔离效果：**缺真实板子**，QEMU 上做不了，不算通过或失败。

## v007 验收（内核 sha256 `4672719363ac3fdd…`，编译于 82db71a）

命令（`acceptance.py` 用 `-append qa` 启动，内核依次跑菜单 1、3–10、1，每个计时演示 1000 个周期，打印 `QA DONE` 后结束）：

```bash
make
python3 tools/acceptance.py build/quotaos.bin /tmp/qa.txt icount qa 3     # 加 -icount shift=4,sleep=off
python3 tools/acceptance.py build/quotaos.bin /tmp/qa-n.txt normal qa 3   # 不加 icount
python3 tools/menu_regress.py run build/quotaos.bin /tmp/menus.txt 2
python3 tools/menu_regress.py compare docs/regress-D/menus-v007-gicv2.txt /tmp/menus.txt
```

icount（`docs/accept-D/qa-icount-run{1,2,3}.txt` 三次逐字节相同；`qa-icount-gicv2.txt` 只差 GIC 那一行）：

- 菜单 5、6、9、10：display-vsync 和 timer-server 各 1000 个作业，错过 0，起点偏移恒为 46 tick（抖动 0），class a/b 都是 0。
- 菜单 6：bad-driver 每周期最多 24989 tick（槽长 25000），在槽尾被掐断。
- 菜单 6、7、9：Tier 2/3 在 Tier 1 槽内 0 tick；空槽全部空转。
- 菜单 8：无压力 3000/4000 us，有压力 2000/2000 us，低于最低值的周期 0 个。
- 内核每周期最多 14678 tick（菜单 9），系统预留 21875 tick（350 us），超出 0 次。定时器中断进入比 CVAL 晚 3 tick。

已知：同一映像在 icount 下重复跑，偶尔「timer IRQ entry - CVAL」和「cut by the slot-end timer」两行的最大值是 5 而不是 3 tick，其他行不变；所以「重复运行逐字节相同」不是每次都成立，原因未查明（怀疑 QEMU 在 icount 下中断送达点受宿主机影响）。

普通模式（不加 icount，共用宿主机，负载均值约 4–5，`docs/accept-D/qa-normal-run{1,2,3}.txt`，负载记录在同名 `-host.txt`）：三次的 Tier 1 错过（各菜单合计）分别是 class a 1193 / class b 0、class a 938 / class b 0、class a 4327 / class b 488（第三次跳过周期 2297 个）。这些数字受宿主机时钟跳动影响，只作参考。

## v008：只改输出格式（内核 sha256 `15dcae5c5a66…`，编译于 f9119e9）

- 所有串口输出行不超过 80 列（中日韩字符算 2 列）：启动横幅的定位句在第一个「，」后换行；`[boot] system reserve`、`admission`、`last release`、`started …`、几行「结果」和 `T1MISS` 都拆成两行，续行缩进。数值和字段没有变。
- `tools/menu_regress.py compare` 改成空白归一：按所有空白（含换行）切词、每个中日韩字符单独算一个词，再逐词比较；计时行（含续行）的数字照旧屏蔽。拆行、对齐不算区别，改字或改功能数字算。v007 的期望输出没有重新生成。
- 证据（`docs/accept-v008/summary.txt`）：v007 和 v008 的 icount qa 日志去掉全部空白后、不屏蔽数字，除版本行外完全相同；菜单回归和 qa 日志的空白归一比较只差版本行（`docs/regress-v008/compare-v007-v008.txt`、`docs/accept-v008/compare-qa-icount-v007-v008.txt`）；最宽一行 79 列（`python3 tools/width_check.py <日志>` 列出超过 80 列的行）。
- icount 重复 10 次（`docs/accept-v008/repeat/`）：文件 sha256 有 3 种；去掉两行中断进入时延后 10 份完全相同。这两行的最大值有时是 5 tick 而不是 3 tick（见上文 v007 的「已知」），原因未查明。

## v009：E1 统一声明和按资源准入（内核 sha256 `ede1ecfe159f…`，编译于 d2c58c4）

实现在 `kernel/decl.c`，菜单 11、12 可在 UTM / QEMU 串口里直接看到。

- 资源：CPU（us/周期）、内存（KB）、`sim-dma` 和 `sim-blk` 两个**模拟设备**（内核里的软件模型，不是真实硬件）的占用时间（us/周期）。周期 P = 10000 us。
- 准入，每种资源各算一遍：系统预留 + Tier 1 + Tier 2 最低值之和 + 保护时间 + 本次申请 ≤ 容量。CPU 的保护时间是 4 个槽 × 10 us；设备的保护时间是每条 Tier 1 声明一次实测停住时间。超了就 `REFUSED: <资源名>`，并打印那种资源的算式。Tier 3 不做准入。
- 设备描述（出厂写死）：单次作业最长耗时、切换开销、从下令中止到停住的上限、能否打断、能否切分。系统自己量：在关中断、并确认作业能在下一个 Tier 1 保护区之前做完的窗口里，跑 8 次完整作业、8 次中途中止，取最大值；不用设备自报。
- Tier 1 设备规则：不能打断的设备一律不进 Tier 1（独占也不行），拒绝里写 `missing: forcibly stoppable (… is non-preemptible; measured stop … > bound …)`；能打断的设备要么独占，要么同时满足能切分、实测停住时间 ≤ 上限、实测最长作业 ≤ 声明，缺哪条写哪条。
- `display-vsync` 在 `sim-dma` 上有一条 2000 us 的 Tier 1 声明，只是声明，这一版没有驱动真的去用设备（令牌和闸门在 E2、E3）。

验收（`python3 tools/acceptance.py build/quotaos.bin <out> icount e1`，引导参数 `e1` 依次跑菜单 11、12 和下面两项检查，然后 `QA DONE`）：30 轮日志在 `docs/accept-v009/e1/`，30 份逐字节相同（`SHA256SUMS`）。

- 第 2 项：`sim-blk`（不能打断）申请 Tier 1，B 从 100 到 10000 us 共 100 次：拒绝 100，写出缺的条件 100；对照组（`sim-dma` 的 Tier 1、`sim-blk` 的 Tier 2）都被接纳。`E1 item2 PASS` × 30。
- 第 4 项：7 个用例（只超 CPU、只超内存、只超 `sim-dma`、三样都超、CPU 多 1 us、CPU 正好用满、都放得下），判定 7/7 正确，拒绝里写出的资源名与超的资源完全一致 7/7。`E1 item4 PASS` × 30。
- 同一次启动里 Tier 1 错过 0 次（含设备实测的关中断窗口）。icount 下实测：`sim-dma` 最长作业 396 us、停住 7 us；`sim-blk` 最长作业 1991 us、中止后要 1770 us 才停（不能打断，只能等做完）。

普通模式（不加 icount）的数字只作参考：`docs/regress-v009/menus-v009-gicv2.txt` 那次 `sim-dma` 实测停住 21 us，超过上限 20 us，于是 `cam-app` 的 Tier 1 申请也被拒（`missing: forcibly stoppable`）；`docs/demo-output.txt` 那次是 16 us，被接纳。实测值受宿主机时钟跳动影响，规则按实测值保守处理。

旧功能没有变：v008 和 v009 的 icount qa 日志空白归一比较只差版本行（`docs/accept-v009/compare-qa-icount-v008-v009.txt`）；菜单回归只多出菜单 11、12 和帮助里的两行（`docs/regress-v009/compare-v008-v009.txt`）。

## v010：E2 预算令牌（内核 sha256 `a6f8603f3fc2…`，编译于 4f077b3）

实现在 `kernel/ipc.c`、`kernel/sched.c`，菜单 13 可在 UTM / QEMU 串口里直接看到。做法照 seL4 MCS 的调度上下文借用：

- 两个系统调用：`svc #1` 调用（x0 服务端编号，x1 请求），`svc #2` 回复并等下一个请求。驱动 `sim-dma-drv` 是被动服务端：自己不在任何运行队列里，只在有人调用时借调用方的预算运行。
- 驱动运行的时间记在调用方的预算上（调用方的已用量增加，驱动自己的不变）；调用、回复两次进内核的路径也记在调用方，不记在系统预留。服务端忙时调用方排队，回复后按轮转交给下一个。
- 演示：`cam-app`（Tier 2，最低值 3000 us）和 `bg-caller`（Tier 3）反复请驱动做 50..400 us 的模拟设备工作。

第 8 项（`python3 tools/acceptance.py build/quotaos.bin <out> icount e2 <2|3>`，引导参数 `e2` 跑菜单 13 共 1000 个周期，然后 `QA DONE`）：30 轮 × GICv2、30 轮 × GICv3，日志在 `docs/accept-v010/e2-gicv2/`、`docs/accept-v010/e2-gicv3/`，每组 30 份逐字节相同（`SHA256SUMS`）；两组之间只差 GIC 那一行和驱动自己的墙钟计时一行（只作参考）。

- 调用 15045 次；内核在调用期间量到：驱动运行 189502915 tick + IPC 内核路径 7547640 tick = 197050555 tick，调用方预算正好减少 197050555 tick；每次调用的「减少量 − 实测」最大 0 tick。驱动自己的预算变化 0 tick。`E2 item8 PASS` × 60。
- 同一窗口：Tier 1 错过 0、起点抖动 0 tick；内核每周期最多 11861 tick，系统预留 21875，超出 0 次。
- 说明：「实测」和「扣减」由同一个内核计时得出，这一项验证的是记账没有漏记、错记到驱动或系统预留；驱动自己用墙钟量的时间含被抢占的部分，只作参考。
- 观察到「timer IRQ entry - CVAL」最大 342 tick（v009 qa 里是 3–5 tick）：定时器到点时 CPU 正在 IPC 系统调用路径里（关中断），要等它返回才进中断。所以 Tier 2/3 的预算到点后最多晚这么多才被掐断；Tier 1 槽不受影响（见下一条）。

保护时间从每槽 10 us 改为 20 us：加入 IPC 后，第一次 e2 运行里有系统调用在保护区开始前一刻进内核，把 Tier 1 槽的起点推迟，起点抖动到 183 tick；改成 20 us 后抖动 0（上面 60 轮）。因此 CPU 保护时间 4 × 20 = 80 us，CPU 可准入的容量从 8110 降到 8070 us。

设备 Tier 1 准入改为只用出厂声明值（测试和研究员的反馈）：

- 判断只看声明的单次作业最长耗时、停住上限、能否打断、能否切分，所以每次启动结论一样；实测值照样量出来显示，只用来核对（实测超出声明的健康监测和降级在 E3）。设备保护时间 = Tier 1 声明条数 × 声明的停住上限。
- 拒绝列出全部缺的条件：`sim-blk` 写 `missing: 不能强制叫停` 和 `missing: 不可切分`（后者独占可免）；声明停住上限超过 Tier 1 限值 20 us 是固定拒绝，写 `missing: 停止上限超过 Tier 1 限值`。新增模拟设备 `sim-cam`（声明停住 30 us）演示这一条。
- 30 轮 × GICv2、30 轮 × GICv3（`docs/accept-v010/e1-gicv2/`、`e1-gicv3/`，每组逐字节相同）：`E1 item2 PASS`（100 次拒绝都同时写出两条缺的条件）、`E1 declared-bound PASS`（`sim-cam` 30 次 Tier 1 申请都写出停住上限这一条，Tier 2 对照被接纳）、`E1 item4 PASS`（边界用例改成 CPU 8071 拒绝 / 8070 放得下）；同一次启动 Tier 1 错过 0。

旧功能：v009 和 v010 的 icount qa 日志空白归一比较（`docs/accept-v010/compare-qa-icount-v009-v010-gicv3.txt`，同为 GICv3）只差版本行和保护时间带来的数字：guard 40→80、CPU 容量 8110→8070、剩余 2110→2070、合计 1890→1930。菜单回归（`docs/regress-v010/compare-v009-v010.txt`，普通模式）另外多出菜单 13、帮助一行、菜单 11 的新行、实测数字和普通模式下的 T1MISS 行（全是 class a 时钟跳动，class b 0，只作参考）。最宽一行不超过 80 列（`tools/width_check.py`）。

## v011：E3 设备闸门和超时降级（内核 sha256 `d2819b7c7bd1…`，编译于 181e8ac）

### 先修：IPC 路径里定时器中断被推迟

v010 的 e2 日志里「timer IRQ entry - CVAL」最大 342 tick（`docs/accept-v010/e2-gicv2/`）。原因：调度器在内核里读一次 `now`、按它算出期限（预算用完、槽前保护区、周期边界），之后还要走一段路才 eret；IPC 的内核路径更长，期限有时在这段路里就过了，写进比较值时已经是过去的时刻，中断在 eret 之后才进来。另外 IPC 内核路径的时间是事后才记到调用者头上，挑选时没算进去，调用者的 Tier 2 预算可能超出一整段内核路径。改法：

- 挑选前看下一个期限：离现在不到出口余量（500 tick，icount 实测挑选到 eret 最多 420 tick）就在内核里等到它再挑，所以写进比较值的期限一定在 eret 之后。等的时间算内核时间。
- 挑 Tier 2 时，把这一段稍后要记到它头上的 IPC 内核时间（到 eret 为止的上限）先扣掉。记账时 Tier 2 只记到预算上限为止，超出的部分改记系统预留并计数（e2 里 178 次，最多 1455 tick）。
- 出口改成先恢复寄存器、最后才读时间写比较值，从读时间到 eret 只剩 10 条指令。
- 新增诊断行：出口时期限已过的次数、挑选到 eret 的最大值、Tier 2 预算超出上限的最大值。

结果（icount，每组 30 轮）：e2 的「timer IRQ entry - CVAL」最大 3 tick（GICv2 30/30；GICv3 30/30），Tier 2 预算最多超出 3 tick，出口时期限已过 0 次；qa（GICv2、GICv3 各 1 次）最大 3 tick；e3 两个演示最大 3 tick，GICv3 有 4/30 轮在风暴演示里是 5 tick（即 v007 起记录的 3/5 tick 现象）。代价：qa 菜单 9 的内核每周期最多从 16415 升到 19286 tick（预留 21875，超出 0 次）。

### E3

实现在 `kernel/dev.c`（闸门、超时、中断）和 `kernel/decl.c`（验证、健康监测、策略）。模拟设备的「硬件」用 EL1 物理定时器代替：作业做完时它经 GIC 的 PPI 30 送来中断，和真设备一样是异步的；中止后设备什么时候停住由模型给出，内核轮询状态量出来。模拟设备是软件模型，不是真实硬件。

- 新设备一律从 Tier 3 开始。开机时系统量一遍（8 次完整作业、8 次中止），实测没超过声明才可能进 Tier 1；准入本身仍只看声明值。`sim-blk` 不能打断，中止后要等做完，所以声明的停住上限改为 2000 us（等于最长作业）；它的 Tier 1 拒绝因此多列一条「停止上限超过 Tier 1 限值」。
- 闸门（驱动 `svc #3` 提交作业）：声明的最长作业 + 切换开销要放得进调用者这一周期剩下的设备预算，并且加上声明的停住上限要在下一个 Tier 1 保护区之前结束；设备在 Tier 3 时只放行 Tier 3 调用者。设备时间记在调用者的设备预算上。
- 超时：开始后过了声明的最长作业时间还没做完，内核强制中止、轮询到停住、作业丢弃并计数，写日志（设备名、实测、声明、周期号），设备降到 Tier 3。
- 健康监测：实测超出声明（超时，或验证时超出）就降级；之后的 Tier 1 申请被拒，写 `missing: 实测超出声明 (…)`。已在这个设备上的 Tier 1 声明同时按各自的策略处理（降级：转 Tier 3；报警：转 Tier 3 并标 ALARM），每条一行日志；首页（菜单 1）显示 NOTICE 和受影响的应用。普通模式（不是 icount）下这些行带「非 icount，仅供参考」。是不是 icount 由开机时测出（同一段指令循环三次的 tick 完全相同且等于指令数）。
- 中断：设备线在 Tier 1 保护区和槽内屏蔽；窗口里每周期最多处理声明的次数（`sim-dma` 40 次），或者驱动自己的 300 us 预算用完，就屏蔽到下个周期。处理时间记在当前作业的调用者头上，没有作业时记在驱动自己的预算上。驱动这 300 us 还没有算进 CPU 准入（已知缺口）。
- 内核里不直接打印：日志先写缓冲，演示结束后由 shell 打印。

验收（`python3 tools/acceptance.py build/quotaos.bin <out> icount e3 <2|3>`，引导参数 `e3` 依次跑菜单 14（1000 个周期）、菜单 1、菜单 15（1000 个周期），然后 `QA DONE`）：30 轮 × GICv2、30 轮 × GICv3，日志在 `docs/accept-v011/e3-gicv2/`、`e3-gicv3/`（`SHA256SUMS`；GICv2 30 份相同，GICv3 有 4 份只差风暴演示里中断进入那一行的 3/5 tick）。每轮结果都一样：

- 第 1 项（少报耗时、能打断的 `sim-dma`：声明 400 us，实际是请求的 1.5 倍）：Tier 1 错过 0、起点抖动 0；提交 7421 个作业，完成 4627，超时 2794（全部中止丢弃）；Tier 1 保护区开始时设备忙 0 次，作业或中止在下一个保护区之后才结束 0 次。`E3 item1 PASS` × 60。
- 第 3 项：第一次超时（周期 9）后 `sim-dma` 在 Tier 3，之后 `cam-app` 的 Tier 1 申请被拒并写出「实测超出声明」。`E3 item3 PASS` × 60。
- 第 6 项：2794 次中止，停住最多 7 us，声明上限 20 us、保护时间 20 us，超出 0 次；开机验证里能打断的设备的停住时间也都没超过。`E3 item6 PASS` × 60。
- 第 7 项：日志行 `[health] sim-dma timeout: measured 406 us > declared 400 us, period 9`。`E3 item7 PASS` × 60。
- 策略检查：`display-vsync`（降级）和 `hud-overlay`（报警）两条 Tier 1 声明在同一次降级里处理、各一行日志，之后这个设备上的 Tier 1 声明 0 条。`E3 policy PASS` × 60。
- 第 5 项（风暴：每 16 us 一次，62500 次/秒）：设备自己的计数 625453 次，处理 23024 次，没送达（被限流）602429 次；每周期最多处理 24 次（先碰到驱动的 300 us 预算，没到声明的 40 次）；Tier 1 保护区和槽内收到设备中断 0 次；Tier 1 错过 0；`cam-app` 每周期最少 3000 us（最低值 3000）。`E3 item5 PASS` × 60。

普通模式只作参考（`docs/accept-v011/e3-normal-gicv2.txt`，共用宿主机）：第 3、7 项和策略检查 PASS；第 1、5、6 项 FAIL：Tier 1 错过（class a 时钟跳动 4 次和 13 次，class b 0）、停住最多 43 us 超过上限 2 次、52 次作业或中止在下一个保护区之后才结束，都发生在宿主机停顿时。菜单回归那两次（`docs/regress-v011/menus-v011-gicv{2,3}.txt`）开机验证都通过。普通模式下宿主机停顿也可能让开机验证的实测超过声明，设备就会被降级：这是普通模式下准入结论唯一会变的途径，相应日志行带「非 icount，仅供参考」。

旧功能：e2 30 轮 × 2（`docs/accept-v011/e2-gicv2/`、`e2-gicv3/`）`E2 item8 PASS` × 60；e1（`docs/accept-v011/e1-icount-gicv{2,3}.txt`）三项 PASS。qa icount 与 v010 的空白归一比较（`docs/accept-v011/compare-qa-icount-v010-v011-gicv{2,3}.txt`）只多出版本行、开机的时钟/设备线/验证行、菜单 1 的设备状态和新诊断行；计时数字的变化见上（内核每周期、保护区路径、bad-driver 被掐断时的用量 24969 tick）。菜单回归（`docs/regress-v011/compare-v010-v011.txt`，普通模式）另外多出菜单 14、15、帮助两行，以及 `sim-blk` 停住上限 2000 带来的一条拒绝理由。最宽一行不超过 80 列。

## v012：E4 带宽限流和缓存着色（内核 sha256 `7448415e5eb1…`，编译于 fb77426）

### 先修：v011 的槽前保护区余量和出口尾巴

- **保护区余量**：v011 的 e2 日志里「slot guard」余量最小 0 tick（保护区 1250 tick），Tier 1 起点抖动 116 / 125 tick（`docs/accept-v011/e2-gicv2/e2-icount-01.txt`；v010 是余量 153、抖动 0）。Tier 1 错过仍是 0，但 v011 的说明里漏写了这一点。原因：保护区起点前刚进来的 IPC 回复（`svc #2`）在内核里要把 48 个任务扫一遍找下一个调用者，路径太长，跑进了保护区。改成每个服务端一个先来先服务的等待队列（队列满过才退回全扫）。e2 结果：余量最小 576 tick，抖动 0（`docs/accept-v012/e2-gicv{2,3}/`，60 份）。「path max」从进内核算起，进内核早于保护区起点时会超过 1250（e2 是 1285），要看的是余量。
- **出口尾巴里到期的中断**：出口从读计数器到 eret 是 10 条指令。剩余预算比这还短时，比较值在 eret 之前就到了，中断要等 eret 之后才进来。没修时，v012 的候选版本在 e2 里实测晚到 11 tick、e3 里 7 tick（日志没有提交，这两个数只说明为什么改）。改法：剩余预算短于 12 tick 时把期限推到读计数器后 12 tick，代价是 Tier 2 预算最多多跑几 tick，计入「overrun max」。新诊断行记被推迟的次数。

结果（icount）：

| 运行 | timer IRQ entry - CVAL 最大 | Tier 2 overrun 最大 | 期限推迟次数 | 保护区余量最小 | 内核每周期 平均 / 最大（预留 21875） | 日志 |
| --- | --- | --- | --- | --- | --- | --- |
| e2，30 轮 × GICv2/GICv3 | 3 | 14 | 7 | 576 | 12172 / 15600 | `docs/accept-v012/e2-gicv{2,3}/` |
| e3 超时演示 | 3 | 6 | 62 | 741 | 11577 / 15865 | `docs/accept-v012/e3-icount-gicv{2,3}.txt` |
| e3 风暴演示 | 3 | 3 | 0 | 588 | 14047 / 17404 | 同上 |
| e4 带宽演示，30 轮 × GICv2/GICv3 | 3 | 3 | 0 | 569 | 12428 / 15816 | `docs/accept-v012/e4-gicv{2,3}/` |
| qa 菜单 9 | 3（GICv3 那次 5） | 3 | 0 | 592 | 17561 / 19992 | `docs/accept-v012/qa-icount-gicv{2,3}.txt` |

qa 其他计时演示的中断进入最大都是 3 tick。GICv3 菜单 9 的 5 tick 就是 v007 起记录的 3/5 tick 现象，原因仍未查明。qa 菜单 9 的内核每周期最多从 v011 的 19286 升到 19992 tick，还在预留之内，超出 0 次。

### 菜单 13：IPC 内核路径分三段，整窗核对

「IPC kernel path charged to callers in total」现在拆成三段：调用窗口里（服务端绑着调用者时）、进入和排队（`svc #1` 到服务端接手之前）、回复返回（`svc #2` 之后回到调用者之前），三段相加正好等于总数。另外按周期结算做整窗核对：调用者预算减少总量（各任务每周期用量之和减去任务自己跑的部分）应等于驱动替它跑的时间加三段内核路径，打印误差。`E2 item8` 现在也要求三段之和等于总数、整窗误差为 0。e2 每轮（60 份结果相同）：

- 总数 26331404 = 调用窗口 11461753 + 进入和排队 1681762 + 回复返回 13187889；
- 整窗：调用者预算减少 231958764 = 驱动 205646283 + 调用窗口 11452417 + 进入 1680889 + 回复 13179175，误差 0 tick；
- `E2 item8 PASS` × 60。

### E4

实现在 `kernel/sched.c`（带宽）、`kernel/color.c`（着色）、`kernel/e4.c`（演示）和 `kernel/decl.c`（准入）。

- **带宽（MemGuard 的做法）**：每个任务每周期有访存次数预算。QEMU 没有访存事件计数器，所以访存数是模拟的：运行 tick × 任务的访存密度。内核按剩余预算算出还能跑多少 tick，和 CPU 预算取小的写进比较值（相当于计数器溢出中断）。用完就停到下个周期；下个周期额度恢复，超出的部分不带过去（不欠账）。准入多一种资源 `mem-bw`：模拟总线每周期 2000000 次，系统预留 100000，Tier 1 声明 250000（display-vsync 200000、timer-server 50000）。
- **缓存着色**：模型是 2 MB、16 路组相联、4 KB 页，共 32 种颜色（颜色 = 物理地址第 12–16 位）。系统预留之后的内存是页池，每种颜色一份。开机时 Tier 1 表里每一项按内存大小分到整数个自己独占的颜色，物理页从这些颜色里取，每页写上主人标记；剩下的颜色给其他任务共用。独占颜色里用不完的页别人不能用，算作内存的「保护量」，从应用可用内存里扣掉：128 MB 上 11776 KB（应用可用从 101376 KB 变成 89600 KB），64 MB 上 3584 KB。E1 第 4 项里「放得下」的例子因此从 90000 KB 改为 85000 KB。运行中新加的 Tier 1 应用不重新分颜色（已知限制）。

验收（`python3 tools/acceptance.py build/quotaos.bin <out> icount e4 <2|3>`，引导参数 `e4` 跑菜单 16（1000 个周期）和菜单 17，然后 `QA DONE`）：30 轮 × GICv2、30 轮 × GICv3，日志在 `docs/accept-v012/e4-gicv2/`、`e4-gicv3/`（`SHA256SUMS`；每组 30 份完全相同）。

| 项 | QEMU icount 结果（60 轮都一样） | 真实隔离效果 |
| --- | --- | --- |
| 9 带宽限流不欠账 | `mem-hog`（Tier 3，每 tick 4 次，预算 150000）1000 个周期都被停住；每周期 150012 次，最多超出 12 次（上限 81 = 20 tick 的访存）；停住后同一周期再跑 0 次；停住的下一个周期又用到 150012（额度恢复）。`cam-app`（Tier 2，预算 400000）每周期最多 375006 次，被停 0 次，CPU 每周期最少 3000 us（最低值 3000）。准入：`bw-greedy` 申请 1700000 被拒（100000 + 250000 + 0 + 1700000 = 2050000 > 2000000）。Tier 1 错过 0。`E4 item9 PASS` × 60 | 缺真实板子 |
| 10 Tier 1 物理页颜色正确 | 128 MB：页池 28672 页、每色 896 页；5 项 Tier 1 共 3328 页占 7 种颜色，颜色正确 3328 页、主人标记完好 3328 页；Tier 3 申请 512 页全部来自共用颜色，落在 Tier 1 颜色里 0 页。`E4 item10 PASS` × 60 | 缺真实板子（QEMU 没有缓存模型） |

长跑（`python3 tools/acceptance.py prebuilt/quotaos.bin <out> icount qa-long <2|3>`，菜单 9 的负载跑 100000 个周期，内核就是 v012 的 `prebuilt/quotaos.bin`）：GICv2、GICv3 各一次，日志在 `docs/qa-long-v012/`（两份只差 GIC 那一行开机信息）。Tier 1 两个任务各 100000 个作业，错过 0，起点 41 tick、抖动 0；Tier 2/3 在 Tier 1 槽内 0 tick；game 每周期收到 3000 us（最低值 2000），低于最低值 0 个周期；audio-mix 100000 个作业错过 0，最长响应 492 us（期限 4000 us）；内核每周期平均 17558、最多 20549 tick（预留 21875，超出 0 次）；timer IRQ entry - CVAL 最大 5 tick（n=1400000，即上面的 3/5 tick 现象）；保护区余量最小 592 tick；Tier 2 预算最多超出 3 tick。每次宿主机耗时 287 s。

普通模式只作参考（`docs/accept-v012/e4-normal-gicv2.txt`，共用宿主机）：第 10 项 PASS；第 9 项 FAIL：宿主机停顿时 `mem-hog` 一个周期最多超出 62864 次，Tier 1 错过 4 次（class a 时钟跳动，class b 0）。

旧功能（icount）：e1 三项 PASS（`docs/accept-v012/e1-icount-gicv{2,3}.txt`）；e3 第 1、3、5、6、7 项和策略检查 PASS（`docs/accept-v012/e3-icount-gicv{2,3}.txt`）；e2 见上。qa 与 v011 的比较（`docs/accept-v012/compare-qa-icount-v011-v012-gicv{2,3}.txt`）只多出版本行、着色开机行、应用可用内存（101376 → 89600 KB 及相关数字）、拒绝理由里的「color padding」和新诊断行。菜单回归（`docs/regress-v012/menus-v012-gicv{2,3}.txt`，与 v011 的比较见 `docs/regress-v012/compare-v011-v012.txt`，普通模式）：开机验证两次都通过；除了上面这些，多出菜单 16、17 和帮助两行；其余差别是普通模式下每次都不同的计数（调用次数、作业数、中断数、实测耗时）。最宽一行不超过 80 列。

## v013：菜单 13 对账和 IPC 余量（内核 sha256 `ed778b57b055…`，编译于 36f3c33）

### 两种口径为什么对不上（v012 的 26331404 和 26312481）

v012 菜单 13 有两种计数：累计计数（三段内核路径、驱动运行）从演示开始清零那一刻算到演示结束读出那一刻；整窗核对的 w_* 只加记录窗口里结束了的周期。v013 在窗口开始和结束时各拍一次累计计数的快照，把差别拆成「窗口前」和「窗口后」两段。e2 每轮（`docs/accept-v013/e2-gicv{2,3}/`，每组 30 份完全相同，GICv2 和 GICv3 只差开机的 GIC 行和清零时刻 60 tick）：

- 清零在 tick 6232416（GICv3 6232476），记录窗口是周期 9..1008 = tick 6250000..631250000，读出在 631254612。清零落在周期 8 里，所以窗口前有一段不到一个周期的余量；窗口最后一个周期在演示任务被收掉之前已经结算（`end_period` 在 `rec` 还是 1 时运行），所以窗口后是 0。
- 逐项：驱动运行 0 + 207874191 + 0 = 207874191；调用窗口 0 + 11581183 + 0 = 11581183；进入和排队 856 + 1687487 + 0 = 1688343；回复返回 0 + 13364995 + 0 = 13364995。四项误差都是 0。
- 另一个差别是口径不同：第一行的「driver running」是按回复了的调用累计的（207868839），不是按记账时刻。207868839 = 记账时刻累计 207874191 + 清零时还没回复的调用在清零前已跑的 7504 − 结束时还没回复的 1 个调用已跑的 12856，误差 0。
- 改代码前先用一个没提交的调试版（v012 的记账加上窗口快照）在 e2 上查过，结论一样：差别全在窗口前，窗口后是 0。那次的数字没有提交日志，所以这里不引用。

`E2 item8` 现在还要求这四项「窗口前 + 窗口 + 窗口后 = 累计」逐项误差 0、回复口径和记账口径相差为 0。菜单回归（普通模式）里两次也都是误差 0（`docs/regress-v013/menus-v013-gicv{2,3}.txt`）。

### IPC 内核路径超出调用者预算的部分

v010–v012 里，Tier 2 调用者预算在 IPC 内核路径中途用完时，超出的部分记在系统预留上（v012 的 e2：145 次，共 76531，最多 892 tick），和「全部记在调用者头上」矛盾。v013 改成全部记在调用者头上；Tier 2 超出预算上限的部分从它准入时多留的 IPC 余量里出。准入时用 IPC 的 Tier 2 应用按「最低值 + 120 us」算 CPU（e2 里 `cam-app`：350 + 1500 + 80 + 0 + 3120 = 5050 ≤ 10000）。120 us = 7500 tick 大于实测最长的一次内核停留（v012 qa-long 6622 tick，v013 qa-long 6467 tick）。规则（v015 起）：扣不扣看预算主人。一个预算主人（调用者）本周期一旦用了 IPC 余量，之后本周期任何记在它头上的新调用都扣住，到下个周期开头再放出去，包括服务端替它转发给别的服务端的调用；已经在内核路径上的调用照常做完。所以一个预算主人一个周期里只会用一次余量，不会在保底轮和建议轮各用一次，也不会经转发服务端再用一次。测试见「v015」（v014 只扣调用者自己发的调用，服务端转发的不扣，见「v014」）。e2：158 次，共 83437 tick，单次最多 866，一个周期最多 866（≤ 7500），记到系统预留 0。

对系统预留余量的影响（内核每周期，预留 21875 tick）：

| 运行 | v012 平均 / 最大 | v013 平均 / 最大 | 日志 |
| --- | --- | --- | --- |
| e2 | 12172 / 15600 | 12078 / 15596 | `docs/accept-v012/e2-gicv2/`、`docs/accept-v013/e2-gicv2/` |
| e3 超时演示 | 11577 / 15865 | 11437 / 13864 | `docs/accept-v01{2,3}/e3-icount-gicv2.txt` |
| qa 菜单 9 | 17561 / 19992 | 17700 / 19992 | `docs/accept-v01{2,3}/qa-icount-gicv2.txt` |
| qa-long（100000 个周期，没有 IPC） | 17558 / 20549 | 17697 / 20394 | `docs/qa-long-v012/qa-long-icount-gicv2.txt`、`docs/accept-v013/qa-long-icount-gicv2.txt` |

qa-long 不用 IPC，变化只是代码变了；最大值离预留还差 1481 tick。v013 的 qa-long 只跑了 GICv2 一次，Tier 1 错过 0、抖动 0，其余结果和 v012 相同（逐行比较只差版本行、内核大小、Tier 3 合计 4819 / 4821 us、内核每周期和 entry→eret 最大值）。

旧功能（icount，GICv2 和 GICv3 各一次）：e1 三项、e3 第 1、3、5、6、7 项和策略、e4 第 9、10 项都 PASS（`docs/accept-v013/`）；中断进入 − CVAL 最大都是 3 tick。qa 与 v012 的比较（`docs/accept-v013/compare-qa-icount-v012-v013-gicv{2,3}.txt`）只差版本行。菜单回归与 v012 的比较见 `docs/regress-v013/compare-v012-v013.txt`。

## v014：用过 IPC 余量就扣住新调用（内核 sha256 `cbca322f60f0…`，编译于 7065679）

规则：一个调用者本周期一旦用了 IPC 余量（预算在 IPC 内核路径里到头），它这个周期再发的新调用（`svc #1`）一律扣住，任务挂起，到下个周期开头再替它发出去；已经在内核路径上的调用照常做完。服务端替别人发的调用不扣（这是个漏洞：调用者能经服务端转发在同一周期再用一次余量；v015 已改，见「v015」）。扣住的调用记一个数，周期开头只在有扣住的调用时才扫一遍任务表。（最早的版本每个周期开头都扫，内核每周期的开销明显变大，所以改成这样；那次的日志没有提交，这里不引用数字。）

测试（`python3 tools/acceptance.py build/quotaos.bin <out> icount e2h <2|3>`，引导参数 `e2h` 跑菜单 18：同一负载先关掉规则跑 1000 个周期作对照，再按规则跑 1000 个周期）。负载是两个 Tier 2 调用者：`cam-app` 最低 3000 us、建议 1000 us，`hud-app` 最低 1500 us、建议 500 us，各自准入时加 120 us 余量（350 + 1500 + 80 + 0 + 3120 + 1620 = 6670 ≤ 10000），加上 Tier 3 的 `bg-caller`，都向 `sim-dma-drv` 发 5..60 us 的短调用。两个 Tier 2 轮流用保底轮，一个调用者的保底轮会在内核路径里到头，后面还有建议轮，所以同一周期里保底轮和建议轮都可能到头。30 轮 × GICv2、30 轮 × GICv3（`docs/accept-v014/e2h-gicv{2,3}/`，`SHA256SUMS`）：

| | 不扣（对照） | 按规则 |
| --- | --- | --- |
| 用余量的次数、合计 | 1715 次，827422 tick | 1588 次，778081 tick |
| 单次最多 | 948 tick | 1041 tick |
| 一个调用者一个周期最多（准入 7500） | 1754 | 1041 |
| 保底轮和建议轮都用了余量的调用者周期 | 318 | 0 |
| 扣住的新调用 / 下个周期放出 | 0 / 0 | 1088 / 1088 |
| Tier 1 错过、起点抖动 | 0、0 | 0、0 |
| 预算对账（整窗、两种口径） | 误差 0 | 误差 0 |
| 内核每周期 平均 / 最大（预留 21875） | 12670 / 16002 | 11984 / 16642 |
| timer IRQ entry - CVAL 最大 | 3（GICv3 有 1/30 轮是 5） | 3（GICv2 有 1/30 轮是 5） |
| 保护区余量最小 | 550 | 545 |

`E2 headroom PASS` × 60。判据：按规则时一个调用者一个周期最多用余量 ≤ 7500 tick、两轮都用了余量的调用者周期是 0，而且同一负载不扣时这个数大于 0（说明测试确实碰到了这种情况）、有扣住的调用、Tier 1 错过 0、对账误差 0。每组 30 份里 29 份完全相同。另外 1 份只差中断进入那一行的 5 tick（GICv2 第 25 份还差驱动自计时 3 tick），也就是 v007 起记录的 3/5 tick 现象。

旧功能（icount，GICv2 和 GICv3 各一次，`docs/accept-v014/`）：e1 三项、e2 `E2 item8`、e3 第 1、3、5、6、7 项和策略、e4 第 9、10 项都 PASS，中断进入 − CVAL 最大都是 3 tick。e2 只有一个 Tier 2 调用者，它用完余量后本周期就没有预算了，所以扣住 0 次。用余量 147 次，一个周期最多 911 tick；内核每周期 12152 / 15627。qa 和 v013 的比较只差版本行（`docs/accept-v014/compare-qa-icount-v013-v014-gicv{2,3}.txt`）；菜单 9 内核每周期 17791 / 20058，离预留还差 1817 tick（v013 是 17700 / 19992）。菜单回归（`docs/regress-v014/menus-v014-gicv{2,3}.txt`，普通模式；与 v013 的比较见 `docs/regress-v014/compare-v013-v014.txt`）两个内存配置都跑完。菜单 18 要跑两遍 1000 个周期，脚本给它的等待时间从 30 s 改为 90 s。GICv3 那次开机验证时宿主机停顿，`sim-dma` 实测 413 us 超过声明 400 us 被降级（日志标「非 icount，仅供参考」）。

## v015：扣住看预算主人，含转发服务端（内核 sha256 `99ec3fa2ea6a…`，编译于 559daed）

v014 的漏洞：服务端替调用者转发出去的调用不扣，调用者用过一次余量后，还能经服务端在同一周期再用一次。v015 的规则见上文「IPC 内核路径超出调用者预算的部分」：扣不扣看预算主人（`sc`），本周期已经用过余量的预算主人，任何记在它头上的新调用都扣到下个周期开头，包括服务端转发的；已经在内核路径上的调用照常做完。实现上调用链可以多层（调用者 → 服务端 A → 服务端 B，最多走 8 层），每次调用扣了多少用预算主人的累计用量相减来量，转发的那次调用算进外层调用。

测试（`python3 tools/acceptance.py prebuilt/quotaos.bin <out> icount e2h <2|3>`，引导参数 `e2h` 跑菜单 18，四段各 1000 个周期）：前两段和 v014 一样（直接调驱动，先关规则作对照、再按规则）；后两段是「Tier 2 经转发服务端」：`cam-app`、`hud-app`、`bg-caller` 改为调转发服务端 A（`fwd_entry`），A 把请求原样转给驱动 B，全程扣调用者的预算，同样先关规则、再按规则。负载里调用者的保底轮会在内核路径里到头、后面还有建议轮，所以同一周期里两轮都可能用到余量。30 轮 × GICv2、30 轮 × GICv3（`docs/accept-v015/e2h-gicv{2,3}/`，`SHA256SUMS`）：每组 30 份完全相同；GICv2 和 GICv3 只差 GIC 那行开机信息和对照段里一次调用的 60 tick（in-call 55910 / 55850）。

| `docs/accept-v015/e2h-gicv2/e2h-icount-01.txt` | 直接调驱动 不扣 | 直接调驱动 按规则 | 经转发 不扣 | 经转发 按规则 |
| --- | --- | --- | --- | --- |
| 用余量的次数、合计、单次最多 | 1755 次，839443，993 | 1584 次，784596，1005 | 1965 次，919329，1015 | 1464 次，686603，1310 |
| 一个预算主人一个周期最多用余量（准入 7500） | 1875 | 1005 | 1839 | 1310 |
| 用了两次及以上的预算主人周期（其中保底轮和建议轮都用） | 312（312） | 0（0） | 395（395） | 0（0） |
| 扣住的新调用 / 下个周期开头放出 | 0 / 0 | 1079 / 1079 | 0 / 0 | 1012 / 1012 |
| 转发出去 / 收到回复的调用 | 0 / 0 | 0 / 0 | 61903 / 61903 | 52778 / 52778 |
| Tier 1 错过 | 0 | 0 | 0 | 0 |
| 对账误差（整窗、各段） | 0 | 0 | 0 | 0 |
| 中断进入 − CVAL 最大 | 3 | 3 | 3 | 3 |
| 内核每周期 最大 / 平均 | 16965 / 13231 | 17251 / 12552 | 17297 / 12372 | 17910 / 11723 |
| 保护区余量最小 | 528 | 527 | 553 | 553 |

`E2 headroom PASS`、`E2 headroom fwd PASS` 各 60 次（每份还有两行 `E2 item8 PASS`）。判据：按规则时一个预算主人一个周期最多用余量 ≤ 7500 tick、用了两次及以上的周期是 0，同一负载不扣时这个数大于 0，有扣住的调用，Tier 1 错过 0，对账误差 0。

出口余量：第一次跑转发段时挑选到 eret 最多 545 tick，超过原来的出口余量 500，有一次期限在 eret 前已经过了、中断进入 − CVAL 到了 14 / 9 tick（那次的日志没有提交，这里不再引用）。出口余量改为 700 tick 后，上表四段都是 3 tick，挑选到 eret 最多 553（转发段）/ 531。qa 在 500 和 700 下内核每周期的数字相同。

### 系统预留余量：开机以来最大值和它是哪个周期

测试按 qa 最后一次菜单 1 的「measured since boot」算余量。v014 是 20943 tick（周期 6016，余 932），v013 是 20906（`docs/accept-v01{3,4}/qa-icount-gicv2.txt`）。v015 在菜单 1 的这一行下面加了这个周期的归因：当时在跑哪个菜单，内核时间按「上一次进内核的原因」分成定时器中断、Tier 1 槽前保护区等待、设备中断、svc（IPC 和设备）、svc（其他），再写出其中周期边界工作（结算上个周期、开始新周期）和在内核里等期限的时间。记在调用者头上的 IPC 内核路径不算内核每周期，也不在这里。

- 定位（诊断日志，见 `docs/accept-v015/diag/NOTE.md`）：没改之前的 v015 候选，最大 21774 tick 在周期 6015，是菜单 10 记录窗口的第一个周期，其中周期边界工作 4629 tick；v014 的周期 6016 同样是菜单 10 记录窗口的第一个周期（v014 qa 共 7016 个周期，v015 共 7015）。逐周期打印显示记录窗口的第一个周期边界工作明显多：菜单 9 窗口前每周期内核 15713、边界 1630，窗口第一个周期 5014 内核 20305、边界 4914，之后稳定在 18490 左右、边界 3091；窗口收尾那个周期（6014，收掉演示任务）内核 19888、边界 4110。原因：窗口开始时把 48 个任务的窗口统计全部清零，空槽和已退出的任务也清。
- 小改动：窗口开始时只清还在的任务（空槽、已退出的任务在 `task_create` 时会清）。菜单 10 窗口第一个周期从 21774 降到 19610（诊断日志）。
- 结果（`docs/accept-v015/qa-icount-gicv{2,3}.txt`，两份只差 GIC 那行）：开机以来最大 **20355 tick/周期，周期 5014 of 7015，余 1520**（预留 21875）。归因：菜单 9（Tier 3 过载）记录窗口的第一个周期；定时器中断 15634（12 次）、槽前保护区等待 2562（2 次）、设备中断 0、svc（IPC 和设备）0、svc（其他）2159（3 次），其中周期边界工作 4893、等期限 0。剩下的热点是窗口开始时清还在的任务的窗口统计（菜单 9 有几十个 Tier 3 任务），比稳定时多约 1800 tick；没有再改。
- 菜单 9 窗口内核每周期 18532 / 20355（平均 / 最大，含窗口第一个周期）；v014 是 17791 / 20058。平均多了约 740 tick；这一版每次进内核多记一次分类、跨周期和等期限时多读计数器，还有调用链代码，各占多少没有单独量。

### 旧功能和比较

icount，GICv2 和 GICv3 各一次（`docs/accept-v015/`）：e1 三项、e2 `E2 item8`、e3 第 1、3、5、6、7 项和策略、e4 第 9、10 项都 PASS；中断进入 − CVAL 最大 3 tick（e3 GICv3 风暴段 5 tick，即 v007 起记录的 3/5 tick 现象）。e2 用余量 159 次，一个周期最多 940 tick。

和 v014 的比较分两种写（`tools/cmp_logs.py`，`docs/accept-v015/compare-*-v014-v015-gicv{2,3}.txt`、`docs/regress-v015/compare-v014-v015-gicv{2,3}.txt`）：「逐字比较」不做任何处理；「数字全换成 # 后比较」只看文字和结构。

| 日志（GICv2） | 逐字比较 删 / 加行 | 数字换成 # 后 删 / 加行 | 数字换掉后剩下的区别 |
| --- | --- | --- | --- |
| e1 | 2 / 2 | 1 / 1 | 版本行 |
| e2 | 31 / 33 | 1 / 3 | 版本行；新增「2 次及以上 / 转发」两行 |
| e3 | 28 / 33 | 1 / 6 | 版本行；菜单 1 新增 5 行归因 |
| e4 | 7 / 7 | 1 / 1 | 版本行 |
| qa | 45 / 55 | 1 / 11 | 版本行；两次菜单 1 各新增 5 行归因 |
| e2h 第 1 份 | 67 / 215 | 8 / 156 | 版本行、新增的转发两段和新的判据行 |
| 菜单回归（普通模式） | 322 / 502 | 41 / 221 | 菜单 18 多两段、归因行；其余是普通模式下宿主机停顿带来的 T1MISS（class a）行和列宽 |

GICv3 的行数见各比较文件开头（e3 29 / 34，菜单回归 339 / 527 和 64 / 252，其余同 GICv2）。

e3 超时演示的数字和 v014 差得多：v014 提交作业 1783、完成 1108、超时 675，v015 是 7221、4501、2720（判据照样 PASS）。查了一下（`docs/accept-v015/diag/e3-bisect/`）：在 v014 的源码上只加这一版的归因计数（不改调度逻辑），就变成 5163 / 3236 / 1927；只把调用链挑选换回 v014 的写法，是 6001。所以这个演示的作业数对内核路径长短很敏感（`bg-caller` 是 Tier 3，作业能不能在下一个保护区之前做完，看它拿到时间的位置），不是规则变了。这个敏感的原因没有再往下查。

菜单回归（普通模式，`docs/regress-v015/menus-v015-gicv{2,3}.txt`）两个内存配置都跑完，class b 错过 0。菜单 18 现在跑四段，脚本给它的等待时间从 90 s 改为 180 s。GICv2 那次菜单 14 里 `sim-dma` 中止用了 46 us（声明 20 us），日志标「非 icount，仅供参考」。

## v016：DTB 快照、显示和键盘接线。WebKit 引擎没有链进来

这一版不是图形 WebKit。`-append webkit` 的串口写着 `engine linked: no` 和 `HTML/CSS/JS is not running`。ramfb 画的是四条色带（设备自测，checksum `0xfffda800`），不是网页。virtio 键盘收到的是 `EV_KEY code 30`，日志写明它不是 `PlatformKeyboardEvent`。启动行是 `[boot] EL1h, MMU off. No hardware isolation.` MMU 关着，没有硬件隔离。

源码对照过的 WebKit 是 tag `WebKit-7625.2.8.18.3`，提交 `4d967f4ca7bdd75319e732247e714c2bc634555e`（那棵树里的 WPE 版本号 2.53.3）。没有把源码树放进仓库。用 `aarch64-linux-gnu-g++ -std=c++20 -ffreestanding` 编译 `Source/WTF/wtf/posix/OSAllocatorPOSIX.cpp` 失败，第一条错误是 `bmalloc/BPlatform.h` 不存在（`docs/v016/compile-OSAllocatorPOSIX.txt`）。`WebKitCommon.cmake` 里没有 JavaScriptCore 就把 WebCore 关掉，没有 WebCore 就把 WebKit 关掉。`ALL_PORTS` 里没有 Quota OS。

下一件能在这个内核里写的移植依赖是 `WTF::OSAllocator`（`reserve` / `commit` / `decommit`、保护区、可执行页）。POSIX 实现走 `mmap` 和 `mprotect`。MMU 关着，可执行页和保护区做不了，只能拒绝。内存必须在 Tier 1 着色之前从连续区域划出来，而且只在 `-append webkit` 时划，这样 128M 上的准入数字保持现在测到的：CPU 容量 8070 us，应用内存 89600 KB，着色补齐 11776 KB。这个分配器本版没有做。没有它，JavaScriptCore 和 WebCore 链不进来。后面还缺独立的 C++ 运行时、bmalloc、ICU、libxml2、sqlite、harfbuzz、png、jpeg、webp，以及把 `GraphicsContext` 画到已经有的 XRGB8888 表面、把 `EV_KEY` 做成 `PlatformKeyboardEvent`。WPE 2.53.3 还要 glib、libsoup、epoxy。

64M：交付的 v015 映像在本机 QEMU 8.2.2 上停在「没有 GIC」（`docs/baseline-4c2a559/boot-64m-serial.raw`）。原因是着色去写 Tier 1 页，写进了放在页池里的 DTB。v016 在着色前把 DTB 拷进内核 BSS。64M 现在能到 `select>`，补齐仍是 3584 KB，菜单 1 的应用内存容量 32256 KB（`docs/v016/final-64-serial.raw`）。128M 菜单 1 的准入式子仍是 `reserve 350 + tier-1 1500 + tier-2 min 0`（`docs/v016/final-128-serial.raw`）。

e4 的 icount 在改 idle 之前没有 PASS。`tools/acceptance.py` 要等到 `QA DONE` 才落盘，所以卡住时看起来像没输出。未改的映像有一次跑了约 14 分钟后被 SIGKILL，串口停在设备验证那一行（`docs/v016/e4-sigterm-ignored-serial.raw`；当时用的 `timeout 20` 只发了 SIGTERM，QEMU 没退出，所以那份不是 20 秒的记录）。同一条命令、外部 75 秒、到点 SIGKILL、边跑边写串口，三份都停在同一行：v014 提交 `559daede18af8e201a41e41ab5e046ba7c8b66ec` 的原 prebuilt（sha256 `cbca322f60f042c317c2cf02657c733df3010a2258e7a3f929720d86ddafcba0`），交付的 v015（sha256 `99ec3fa2ea6ab795a3297feeda020a175afae126b894323267b22614e7078bb6`），和当时的本版（sha256 `8dbd8a40329c16a2c7c5de704cb1ea5c5ffc9abb9867977818b69f81aa18c95a`）。那三份都不是 PASS。原因是后来量出来的，不是当时猜的：QEMU 8.2.2 的 `-icount shift=4,sleep=off` 在 `wfi` 里不把虚拟时间推到已经写好的比较值。诊断映像在空槽里打印 `slot-idle now 693043 cval 718750 ctl 0x1 shell 1`（shell 仍是 READY，槽主人不在，定时器开着，比较值还在 411 us 之后），然后 `idle` 执行 `wfi`，8 秒内没有下一条。`-d int` 两秒里 icount 只有 4 次异常返回，最后一条 eret 的 PC `0x40084c94` 是 `idle_entry` 的 `wfi`；同一映像不加 icount，两秒里有 266 次 IRQ。icount 下 `idle` 改为空转，比较值到了中断照常进来。槽尾、保护区、准入没有改。设备验证仍跑 8 次作业和 8 次中止。修过的映像 sha256 `4fc7ce6d401c1e47765743732e5c2cebdef0f85451608eb0c25a13e07146bb9b`，icount e4 在约 3 秒时打印 `E4 item9 PASS`、`E4 item10 PASS`、`QA DONE`（Tier 1 错过 0，item9 超出最多 12，上限 81）。这是开发自测，不是独立 QA。同一映像不加 icount 的对照里 item9 是 FAIL（超出最多 68780，超过 81；class b 错过 0），不记成通过。回归脚本 `tools/e4_bound.py` 边写边存，到点 SIGKILL；只有内核自己打出两行 PASS 和 `QA DONE` 才退出 0。v014 原映像用它跑 20 秒仍是 NOT-PASS。

```bash
qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a72 -m 128M \
  -nographic -nic none -kernel <image> -append e4 -icount shift=4,sleep=off
```

停住的原始记录还在 `docs/v016/e4-bounded/`（v014 1145 字节、v015 1145、当时的 v016 1245，都没有 `sim-dma` 行）。诊断串口在 `docs/v016/cause/`。修过之后的 icount 记录在 `docs/v016/e4-fix/v016-icount-complete.raw`，普通时钟对照在 `v016-normal-control.raw`。本机 QEMU 是 8.2.2（Debian `1:8.2.2+ds-0ubuntu1.18`），不是旧记录用的 QEMU 10.0。仓库里更早的 e4 PASS 不能代替这次的日志。

工具链：`aarch64-linux-gnu-gcc` 13.3.0。在 `4c2a559` 上 `make SRC_COMMIT=559daed` 得不到交付映像的 sha256（交付的 bin 是 `99ec3fa2…`，这次编译器得到 `a250441e…`）。记录在 `docs/baseline-4c2a559/`。修 idle 之前的本版映像是 `8dbd8a40…`（`docs/v016/sha256-final.txt`）。现在的映像是上面的 `4fc7ce6d…`。这些都是开发自测，不是独立 QA。WebKit 仍然没有链进来。

## 编译

一条命令：

```bash
make
```

需要 `aarch64-linux-gnu-gcc`（Debian/Ubuntu 的包名是 `gcc-aarch64-linux-gnu`）。macOS 上如果工具链叫 `aarch64-elf-gcc`：

```bash
make CROSS=aarch64-elf-
```

产物：

- `build/quotaos.bin`：给 QEMU `-kernel` 用的原始映像。带 Linux arm64 引导头，QEMU 会把设备树指针放进 `x0`。
- `build/quotaos.elf`：同一份内核的 ELF，方便反汇编。
- `prebuilt/` 里有一份已经编好的副本，不装交叉编译器也能跑。

构建可逐字节复现：每个源文件单独编译到 `build/`，`-ffile-prefix-map` 去掉构建路径，链接时 `--build-id=none`，映像里不含时间戳。用同一版 `aarch64-linux-gnu-gcc` 在任何目录下 `make`，`build/` 与 `prebuilt/` 里的 `.bin` 和 `.elf` 的 sha256 应完全一致：

```bash
make clean && make && sha256sum build/quotaos.bin build/quotaos.elf prebuilt/quotaos.bin prebuilt/quotaos.elf
```

## 在 Apple Silicon Mac 上用 Homebrew QEMU 跑

```bash
brew install qemu
qemu-system-aarch64 \
  -machine virt,gic-version=2 \
  -cpu cortex-a72 \
  -m 128M \
  -nographic \
  -nic none \
  -kernel prebuilt/quotaos.bin
```

看到 `select>` 就可以输入菜单编号，回车确认。退出 QEMU：按 `Ctrl-A`，再按 `X`。

这条命令在 x86_64 的 QEMU 上同样跑通（本仓库的实录就是这样采的）。Apple Silicon 上 Homebrew 的 `qemu-system-aarch64` 不加速时走 TCG，和这里是同一条客户机路径；`-cpu cortex-a72` 不会用到 Hypervisor.framework。

## 用 UTM 跑

UTM 底层就是 QEMU。请用**模拟（Emulate）**，不要用虚拟化（Virtualize）。虚拟化要求 `-cpu host`，这个内核按 Cortex-A72 编写。

UTM 是沙盒应用，在 QEMU 参数里手写 `-kernel /路径/quotaos.bin` 会报 `failed to load`，即使路径没错。内核文件必须通过 UTM 自己的文件选择框选中，「新建 Linux 虚拟机 + Boot from kernel image」这条路已在 Apple M 芯片的 Mac 上跑通。

1. 先拿到 `prebuilt/quotaos.bin`（克隆仓库，或单独下载这个文件）。
2. 打开 UTM，点「+」→ Emulate → Linux。
3. 勾选 Boot from kernel image。在 Kernel 一栏点 Browse，选中 `quotaos.bin`。initrd、启动参数都留空。
4. 架构选 ARM64 (aarch64)。内存设 **128 MB**，CPU 1 核。旧设备场景把内存改成 64 MB。
5. 不加磁盘，向导加了就删掉，这个系统不读盘。保存。
6. 编辑这台虚拟机：
   - System 里机器类型确认是 `virt`。
   - **QEMU Machine Properties** 填 `gic-version=2`（新版在 QEMU 页、Arguments 上方；旧版在 System 页，要先打开 Show Advanced Settings）。从 252a90d 起内核也能从设备树认出 GICv3，不填也能启动（命令行 QEMU 10.0 的 `gic-version=2/3/max` 验证过，日志 `docs/gic-C/`；UTM 上还没有实测）。
   - QEMU → Arguments 里可加 `-cpu cortex-a72`。**不要**写 `-kernel`，UTM 会自己加。
   - 左边栏 Devices 下把 Display 删掉，再新建一个 Serial（Built-in Terminal）。较新的 UTM 在没有显示设备时会自动打开串口窗口，已有就不用再加。
7. 启动。当前 `quotaos.bin` 启动参数为空，串口里是 `[boot] no bootarg, starting LVGL`，不是下面的菜单。下面的菜单属于 `quotaos-serial.bin`，或这版加引导参数 `serial`。
8. 启动横幅里会有一行 `[boot] GICv3 from DTB: …`（或 `GICv2 …`），说明认出了中断控制器。如果是 `[boot] ERROR: no GICv2 … or GICv3 … interrupt-controller node`，说明设备树里没有这两种 GIC，内核会停在这里，不会 PANIC。

旧版本（2259e51 及以前）只会 GICv2，在 UTM 的 GICv3 上写 GICv2 CPU 接口 0x08010000 会 `PANIC sync ec=0x25 esr=0x96000050 elr=0x400821e0`，当时必须在 QEMU Machine Properties 里填 `gic-version=2`。现在不需要了；填了也照样能跑。

当前 `prebuilt/quotaos.bin` 启动参数为空时进入白底 LVGL，不再进入 `select>`。董事长已用 ramfb 看到过画面。自定义参数每个 argv 单独一条，依次是 `-device`、`virtio-keyboard-device`、`-device`、`virtio-tablet-device`。四段写在同一条里不会生效。Arguments 里的 `-cpu cortex-a72` 保持不变，不要写 `-kernel`。QEMU 8.2.2 上点 +1 和输入 ab 已测过。UTM 上的按钮和文本还没有确认。串口菜单用 `prebuilt/quotaos-serial.bin`，或这版加引导参数 `serial`。

UTM 和不加 icount 的 QEMU 里，客户机时钟跟着宿主机走，宿主机一卡就会出现时钟跳动，看到的时延和错过次数只作参考。

## 模拟旧设备

```bash
qemu-system-aarch64 \
  -machine virt,gic-version=2 \
  -cpu cortex-a72 \
  -m 64M \
  -nographic \
  -nic none \
  -kernel prebuilt/quotaos.bin
```

启动横幅里的 RAM 应是 65536 KB（64MB），设备树地址和 128MB 那次不同。然后：

- `2` → `1` → `y`：Pixel Racer 只要 8MB 和 3000us，仍能启动。
- `4`：Neural Forge 声明 81920 KB，大于扣掉系统预留和 Tier 1 之后的 35840 KB（v007 的 `docs/regress-D/menus-v007-gicv2.txt` 64M 段）。输出 `device does not meet minimum requirements`，不询问、不启动。

UTM 里把虚拟机内存改成 64MB 应得到同样的结果（UTM 上未实测）。

## Tier 1 截止期回归（commit B 5b82525）

记账口径（`kernel/sched.c`）：

- 每个周期任务的释放时刻就是理想周期起点 `epoch + k*10000us`，`epoch` 是第一个任务上 CPU 的时刻。延迟 = 第一次上 CPU 的时刻 − 理想起点。调度器实际放行的时刻另记为 `dispatch_lag`，不拿来抵消迟到。
- 虚拟时钟一次跳过几个整周期，每个周期性 Tier 1 任务每个被跳过的周期都记一次错过（`skipped`），并在 `T1JUMP` 里记下跳之前的周期号、跳过的周期数和时钟间隔。没有豁免。
- 周期结束时作业还没完成，记一次错过（`unfinished`）。
- 分级 OFF 期间释放的作业记进 `misses_off`（对照组），不进保证统计。

回归脚本每轮启动一次，在分级 ON 下依次跑菜单 1、4、6、10、4、6、1，读最后一次菜单 1 的计数：

```bash
git checkout 5b82525 && make
python3 tools/tier1_rounds.py build/quotaos.bin /tmp/t1-icount 30 4   # -icount shift=4,sleep=off
python3 tools/tier1_rounds.py build/quotaos.bin /tmp/t1-normal 30      # 不用 icount
```

固定配置：`qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a72 -m 128M -nographic -nic none -kernel build/quotaos.bin`，icount 模式再加 `-icount shift=4,sleep=off`（一条指令 16ns，等于一拍 CNTPCT）。`sleep=off` 是必须的：默认的 `sleep=on` 下，客户机在 `wfi` 里等串口输入时虚拟时钟跟着宿主机实时走，宿主机一忙，定时器中断就晚到几毫秒。

5b82525 的结果（原始日志在 `docs/tier1-B/`，每轮一个 `round-NN.txt`，汇总在 `summary.txt`）：

| 模式（来源） | 任务 | 作业数 | 错过 | 其中跳过周期 / 未完成 | 延迟 p50 | p99 | 最大 | 截止期 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| icount shift=4，30 轮（开发自测） | display-vsync | 212292 | **0** | 0 / 0 | 19us | 26us | 236us | 800us |
| icount shift=4，30 轮（开发自测） | timer-server | 212296 | **0** | 0 / 0 | 112us | 119us | 330us | 1500us |
| 不用 icount，30 轮（开发自测） | display-vsync | 4862 | **65** | 5 / 0 | 54us | 1202us | 7486us | 800us |
| 不用 icount，30 轮（开发自测） | timer-server | 4862 | **52** | 5 / 2 | 149us | 1401us | 7585us | 1500us |
| 不用 icount，30 轮（测试验收） | display-vsync | 4782 | **1631** | 167 / 197 | 53us | 9072us | 10308us | 800us |
| 不用 icount，30 轮（测试验收） | timer-server | 4785 | **1361** | 167 / 57 | 141us | 9422us | 11463us | 1500us |

每行的出处（都是 5b82525 的源码，30 轮）：

- 开发自测：内核 sha256 `08bdaf40…`，日志 `docs/tier1-B/icount-shift4/` 和 `docs/tier1-B/normal/`（`round-NN.txt` + `summary.txt`）。
- 测试验收：测试在同一台共用机器上用 5b82525 源码重新编译后跑的，日志在 `/workspace/quota-os-qa/regress-5b82525/captures/tier1/normal/`（`round-NN.txt` + `summary.txt`，这个目录不在仓库里）。这一组 4792 个周期、跳过 172 个、时钟跳动 14030 次。

开发自测的周期数和跳过：icount 212300 个周期、跳过 0 个、时钟跳动 0 次；不用 icount 4862 个周期、跳过 5 个、时钟跳动 16236 次。

不用 icount 时客户机时钟就是宿主机时钟，QEMU 线程被宿主机调走多久，定时器中断就晚多久（`dispatch_lag_max` 7483us）。宿主机是一台多人共用的机器，两次普通模式跑的时候负载不同：开发自测那次 1 分钟负载均值约 3，15 分钟约 18（`docs/tier1-B/normal/summary.txt` 末尾的 `uptime`）；测试验收那次的负载没有记在它的 `summary.txt` 里。两组错过次数差了二十多倍，不能互相替代，都照实列出。**普通模式验收未通过，原因排查中**：正在给每次错过记 `T1MISS` 行和同一时段的时钟跳动总长 `J`，按测试给的规则区分「时钟跳动造成」和「内核问题」，还没有结论。

## 中断控制器（GICv2 / GICv3，commit C 252a90d）

`kernel/gic.c` 从设备树的 interrupt-controller 节点决定驱动哪种硬件：

- `arm,cortex-a15-gic` 或 `arm,gic-400`：GICv2，distributor + 内存映射的 CPU interface。
- `arm,gic-v3`：GICv3，distributor（亲和路由、Group 1）+ 本核的 redistributor（按 MPIDR 找）+ `ICC_*` 系统寄存器 CPU interface。从 EL2 启动时 `boot/startup.S` 先打开 `ICC_SRE_EL2`。
- 都没有：打印 `[boot] ERROR: …` 并停住，不去碰不存在的寄存器。

检查命令（日志在 `docs/gic-C/`）：

```bash
python3 tools/gic_boot_check.py build/quotaos.bin /tmp/gic
python3 tools/menu_regress.py run build/quotaos.bin /tmp/menus-v3.txt 3
python3 tools/menu_regress.py compare docs/gic-C/menus-gicv2.txt /tmp/menus-v3.txt
```

`gic_boot_check.py` 在 `gic-version=2`、`3`、`max`、默认 `virt`、`virtualization=on`（EL2 启动）和 `-smp 2` 下都启动到 `select>`。菜单回归在 GICv2 和 GICv3 下除了横幅里那行 GIC 之外逐行一致。

## 目录

```text
boot/        启动、异常向量、链接脚本
kernel/      串口、设备树、GIC、定时器、资源账本、调度、菜单、统一声明（decl.c）、预算令牌 IPC 和 E3 演示（ipc.c）、设备闸门（dev.c）
include/     公共类型
prebuilt/    编好的 quotaos.bin / quotaos.elf
docs/demo-output.txt     串口实录
docs/regress-A/ docs/regress-B/ docs/tier1-B/ docs/gic-C/ docs/accept-D/ docs/regress-D/ docs/accept-v008/ docs/regress-v008/ docs/accept-v009/ docs/regress-v009/ docs/accept-v010/ docs/regress-v010/ docs/accept-v011/ docs/regress-v011/ docs/accept-v012/ docs/regress-v012/ docs/qa-long-v012/ docs/accept-v013/ docs/regress-v013/ docs/accept-v014/ docs/regress-v014/ docs/accept-v015/ docs/regress-v015/   各提交的验收日志
tools/qemu_serial.py     驱动 QEMU 串口的公共模块
tools/capture_demos.py   自动按菜单跑完并检查提示符
tools/menu_regress.py    菜单功能回归：跑固定流程，屏蔽计时数字后逐行比较
tools/tier1_rounds.py    Tier 1 截止期回归（B 的菜单流程，只适用于 5b82525 的映像）
tools/acceptance.py      v007 起的验收：用 -append qa / qa-long 启动，跑到 QA DONE
tools/width_check.py     列出超过 80 列的行（中日韩字符算 2 列）
tools/gic_boot_check.py  在几种 GIC 配置下检查能否启动到 select>
```

```bash
make
python3 tools/capture_demos.py                                   # 重写 docs/demo-output.txt
python3 tools/menu_regress.py run build/quotaos.bin /tmp/menus.txt
python3 tools/menu_regress.py compare docs/regress-D/menus-v007-gicv2.txt /tmp/menus.txt
```
