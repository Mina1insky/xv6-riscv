# Coding Agent 测试任务：验证 xv6-riscv Buddy 分配器

## 1. 任务目标

验证当前 `mm-buddy-slab` 分支中的 Buddy 物理页分配器是否正确、兼容且没有明显内存泄漏。

本任务以测试和诊断为主：

- 检查代码和分配器不变量；
- 构建并启动 xv6；
- 执行 Buddy 专项测试；
- 执行 xv6 原有回归测试；
- 记录可复现的 PASS/FAIL 证据；
- 若用户同时授权修复，可以修复确认属于 Buddy 的缺陷，并重新执行失败测试；
- 不得把文件系统测试失败解释为必须修改文件系统。

## 2. 权限与修改边界

开始前：

1. 在 xv6 仓库根目录工作。
2. 查找并完整读取适用的 `AGENTS.md`。
3. 当前分支必须是 `mm-buddy-slab`；若不是，停止并报告。
4. 执行 `git status --short`。所有已有修改都属于用户，不覆盖、不清理、不 stash。
5. 记录当前 `HEAD`。
6. 不执行 `git reset --hard`、`git checkout --`、`git clean` 等破坏性命令。
7. 未经明确要求，不 commit、不 push。

默认允许新增或修改测试相关代码：

```text
kernel/kalloc.c
kernel/defs.h
kernel/main.c        # 仅用于受条件编译保护的启动自测
```

若项目已经有专门的内核测试文件，可以优先使用现有测试框架。不要为了测试引入新的系统调用。

禁止修改：

```text
kernel/fs.c
kernel/bio.c
kernel/log.c
kernel/file.c
kernel/sysfile.c
mkfs/
```

不要为让测试通过而修改 `vm.c`、`proc.c`、`pipe.c` 或用户 ABI。若失败根因确实位于调用者，先报告证据并等待用户扩大任务范围。

## 3. 最终通过标准

只有以下项目全部满足，才能报告 Buddy 验证通过：

- 静态检查通过；
- `make clean && make` 成功且无新增 warning；
- xv6 能正常进入 shell；
- Buddy 初始化不变量检查通过；
- order 对齐测试通过；
- 拆分与逐级合并测试通过；
- 混合 order 测试通过；
- order 0 耗尽与完整恢复测试通过；
- 测试前后的空闲页数一致；
- `usertests` 完整通过；
- 没有修改文件系统或扩大实现范围。

任何未实际运行的项目都必须标为 `NOT RUN`，不能推测为通过。

## 4. 阶段 A：基线与静态检查

执行：

```bash
git branch --show-current
git rev-parse HEAD
git status --short
git diff --check
git diff --stat
rg '\\bkalloc\\s*\\(|\\bkfree\\s*\\(' kernel
```

记录：

- 分支名；
- 完整提交 SHA；
- 测试前已有修改；
- `kalloc`/`kfree` 调用点；
- Buddy 实际修改文件。

检查 `kernel/kalloc.c` 至少满足：

1. `kalloc()` 仅调用 `buddy_alloc(0)`，或具有完全等价行为。
2. `kfree()` 仅调用 `buddy_free(pa, 0)`，或具有完全等价行为。
3. `buddy_alloc()` 检查非法 order。
4. `buddy_free()` 检查地址、范围、order、状态和重复释放。
5. buddy 索引通过页索引异或计算。
6. 初始化从 `PGROUNDUP((uint64)end)` 开始。
7. 初始化块同时满足对齐和 `PHYSTOP` 边界。
8. 合并时从旧链表删除 buddy。
9. 合并后选择较小页索引作为块首页。
10. `free_pages` 不会在拆分或合并中重复增减。

若静态检查已经发现确定性错误，先记录错误；若任务仅授权测试，不修复代码，继续执行不会造成数据破坏的测试并最终报告。

## 5. 阶段 B：构建验证

执行：

```bash
make clean
make
```

通过条件：

- 命令退出码为 0；
- 无未定义符号；
- 无重复定义；
- 无格式化参数 warning；
- 内核成功链接；
- 文件系统镜像能够正常生成。

失败时保存第一处有效编译错误以及相关上下文，不要只报告最后一行 `make` 错误。

## 6. 阶段 C：Buddy 一致性检查

如果尚未实现，增加以下调试接口；若仅授权测试且不允许改实现，则把缺失标为测试覆盖缺口：

```c
static int buddy_check_locked(void);
int buddy_check(void);
uint64 buddy_free_pages(void);
void buddy_dump(void);
```

`buddy_check_locked()` 至少验证：

- 空闲链表节点位于 `pages[]` 中；
- 节点为块首页；
- `state == PAGE_FREE`；
- 节点 order 与所在 `free_area` 一致；
- `prev`/`next` 双向关系一致；
- 块相对 `KERNBASE` 满足 `2^order` 页对齐；
- 块完整位于 `[managed_start, PHYSTOP)`；
- 实际节点数等于 `nr_blocks`；
- 各阶块折算的总页数等于 `free_pages`；
- `order < BUDDY_MAX_ORDER` 时，不存在仍可合并的一对同阶空闲 buddy；
- 单链表遍历次数不超过物理页总数，以检测链表环。

公开的 `buddy_check()` 自行加锁。已经持锁的路径只能调用 `buddy_check_locked()`。

## 7. 阶段 D：内核专项自测

测试代码必须使用条件编译：

```c
#ifdef BUDDY_DEBUG
static void buddy_selftest(void);
#endif
```

如果从 `kinit()` 调用，自测应位于 Buddy 初始化完成之后、其他子系统开始分配内存之前。只执行一次。

不要在内核栈上声明大型数组。耗尽测试的指针表必须是静态数组。

### D1. 基础分配与计数恢复

测试步骤：

1. 保存 `before = buddy_free_pages()`。
2. 分配两个 order 0 块和一个 order 2 块。
3. 所有返回值必须非空。
4. 三个物理地址范围不能重叠。
5. 分配后空闲页数应为 `before - 6`。
6. 使用正确 order、以不同顺序释放。
7. 最终 `buddy_free_pages() == before`。
8. 最终 `buddy_check() == 1`。

### D2. 全 order 对齐

对 `0..BUDDY_MAX_ORDER` 逐个尝试分配：

```c
mask = (PGSIZE << order) - 1;
aligned = (((uint64)pa - KERNBASE) & mask) == 0;
```

若分配成功，必须满足对齐并使用相同 order 释放。高 order 因可用内存布局而返回 0 不一定是错误，但 order 0～若干合理小阶必须成功。

每一轮后执行计数恢复和一致性检查。

### D3. 拆分与重新合并

执行可确定的合并测试：

1. 分配一个 order 3 块，记录地址 `base`。
2. 释放该 order 3 块。
3. 连续申请八个 order 0 块。
4. 验证这八个块不重叠；如果分配器从其他同阶空闲块取页，不强制它们位于 `base` 区间。
5. 乱序释放八个 order 0 块。
6. 执行 `buddy_check()`。
7. 再次尝试 order 3 分配，并正确释放。
8. 确认总空闲页数恢复。

不要依赖空闲链表恰好采用某种取块顺序，除非实现明确规定并且测试只检查该规定。

### D4. 非顺序释放

至少申请 16 个 order 0 页，然后按以下类型顺序释放：

```text
奇数索引 → 偶数索引
高地址 → 低地址
固定伪随机排列
```

每组结束后验证：

- 空闲页数恢复；
- `buddy_check()` 通过；
- 能再次分配至少一个合理的高阶块。

### D5. 混合 order 压力测试

使用静态记录表：

```c
struct test_alloc {
  void *pa;
  int order;
  int used;
};
```

建议：

- 记录表至少 256 项；
- order 限制在 0～5；
- 使用确定性伪随机序列，保证失败可复现；
- 循环至少 5000 次；
- 随机选择分配或释放；
- 分配成功后检查对齐和与所有现存块不重叠；
- 释放必须使用记录中的原始 order；
- 结束时释放全部残留块；
- 最终空闲页数必须与开始时一致；
- 最终执行 `buddy_check()`。

重叠检查使用半开区间：

```text
[pa, pa + (PGSIZE << order))
```

### D6. order 0 耗尽与恢复

使用文件作用域静态数组保存返回指针：

```c
static void *test_pages[BUDDY_NR_PAGES];
```

测试步骤：

1. 保存初始空闲页数。
2. 重复 `buddy_alloc(0)` 直到返回 0。
3. 成功分配数量应等于测试开始时的空闲页数。
4. 此时 `buddy_free_pages()` 应为 0。
5. 再次申请 order 0 必须返回 0。
6. 逆序释放全部页。
7. 空闲页数必须完整恢复。
8. `buddy_check()` 必须通过。

注意：自测执行期间不得有其他 CPU 或子系统并发使用 Buddy。最适合在单 CPU 启动初始化阶段执行。

### D7. 分配失败不破坏状态

保存空闲页数与各阶统计，执行：

```c
buddy_alloc(-1);
buddy_alloc(BUDDY_MAX_ORDER + 1);
```

两者应返回 0，并且：

- 空闲页数不变；
- 空闲链表不变；
- `buddy_check()` 通过。

是否将非法 order 计入 `nr_fail` 应与实现约定一致，并在报告中注明。

## 8. 阶段 E：预期 panic 测试

以下测试必须彼此隔离，因为每项都应终止当前内核。一次启动只执行一个用例。

### E1. 重复释放

```c
void *p = buddy_alloc(0);
buddy_free(p, 0);
buddy_free(p, 0);
```

期望：第二次释放触发明确 panic。

### E2. 错误 order

```c
void *p = buddy_alloc(2);
buddy_free(p, 1);
```

期望：触发 wrong-order panic。

### E3. 块内部地址

```c
void *p = buddy_alloc(2);
buddy_free((char *)p + PGSIZE, 2);
```

期望：因 order 对齐、块首页状态或地址检查触发 panic。

### E4. 未对齐地址

```c
void *p = buddy_alloc(0);
buddy_free((char *)p + 1, 0);
```

期望：触发地址对齐 panic。

### E5. 越界地址

仅使用明确、安全且不会先执行 `memset` 的验证路径测试 `managed_start - PGSIZE` 或 `PHYSTOP`。期望在写内存之前 panic。

执行这些测试前先审查 `buddy_free()`：它必须先验证地址和分配状态，再 poison。若代码会在验证前写入测试地址，不执行越界测试，报告安全缺陷。

每项记录实际 panic 文本。测试后删除临时调用，或保持在默认关闭的条件编译块中。

## 9. 阶段 F：启动和原有回归

关闭耗时的启动自测或仅保留快速自测，然后执行：

```bash
make clean
make
make qemu
```

通过条件：

- 正常打印启动信息；
- 所有 hart 正常初始化；
- 没有死锁或重复加锁 panic；
- 能到达 `init: starting sh`；
- 能获得 shell 提示符。

在 xv6 shell 中执行：

```text
echo buddy-ok
ls
cat README
```

接着运行：

```text
usertests -q
```

如果本地基线不支持 `-q`：

```text
usertests
```

要求完整出现成功结论。若超时或中断，标记为 `INCOMPLETE`，记录最后完成的测试，不能写 PASS。

若仓库提供以下程序，再运行：

```text
forktest
stressfs
```

`stressfs` 失败不授权修改文件系统。先检查 Buddy 是否重复分配、错误合并或破坏页内容。

## 10. 阶段 G：重复启动与稳定性

至少重复三轮：

```text
构建或复用同一构建产物
→ 启动 xv6
→ 执行最小用户命令
→ 执行关键测试
→ 正常退出 QEMU
```

如果存在多核相关的偶发问题，增加到 10 轮。记录每轮是否通过，不得只保留最后一轮。

避免用可能损坏磁盘镜像的方式强制终止仍在写盘的 QEMU。优先使用仓库提供的正常退出快捷键或测试脚本。

## 11. 失败定位表

| 现象 | 优先检查 |
|---|---|
| 启动早期 panic | `managed_start`、初始化分块、静态元数据占用 |
| `walk`/页表异常 | order 0 重复分配、错误合并、poison 时机 |
| 随机文件系统失败 | 已使用页被再次分配、空闲链表损坏 |
| 只在多核失败 | Buddy 锁覆盖范围、公开检查函数递归加锁 |
| 释放后页数偏大 | 合并时重复增加 `free_pages` |
| 释放后页数偏小 | 拆分统计错误、块未加入链表、内存泄漏 |
| 高阶分配地址不对齐 | 对齐基准或初始化分块错误 |
| `buddy_check()` 卡死 | 链表成环、`next/prev` 更新错误 |
| wrong-order 未被发现 | 块首页 state/order 元数据维护错误 |
| 耗尽后仍报告空闲页 | `free_pages` 或链表计数错误 |

## 12. 修复后的回归规则

若用户授权 agent 修复：

1. 先保存失败证据。
2. 只修改确认的 Buddy 根因。
3. 先重跑最小失败用例。
4. 再重跑该阶段全部测试。
5. 最后重新执行 `make clean && make`、启动和完整 `usertests`。
6. 修复一个测试不能代替完整回归。
7. 不得通过删除断言、降低检查强度或跳过失败测试来制造 PASS。

若用户只授权测试，禁止修改实现；只可创建临时测试代码，并在结束前说明留下或移除了哪些测试改动。

## 13. 最终报告格式

必须按以下结构向用户报告：

```text
Buddy 测试报告

基线
- 分支：mm-buddy-slab
- HEAD：<完整 SHA>
- 测试前已有修改：<列出或无>

环境
- 编译器：<版本>
- QEMU：<版本>
- CPU/hart 配置：<实际值>

结果
- git diff --check：PASS/FAIL
- make clean && make：PASS/FAIL
- 初始化一致性检查：PASS/FAIL/NOT RUN
- 基础分配与恢复：PASS/FAIL/NOT RUN
- 全 order 对齐：PASS/FAIL/NOT RUN
- 拆分与合并：PASS/FAIL/NOT RUN
- 混合 order 压力：PASS/FAIL/NOT RUN
- order 0 耗尽恢复：PASS/FAIL/NOT RUN
- 非法 order：PASS/FAIL/NOT RUN
- 重复释放 panic：PASS/FAIL/NOT RUN
- wrong-order panic：PASS/FAIL/NOT RUN
- 启动进入 shell：PASS/FAIL/NOT RUN
- usertests：PASS/FAIL/INCOMPLETE/NOT RUN
- forktest：PASS/FAIL/NOT AVAILABLE/NOT RUN
- stressfs：PASS/FAIL/NOT AVAILABLE/NOT RUN
- 重复启动：<通过轮数>/<执行轮数>

内存计数
- 测试前 free_pages：<值>
- 测试后 free_pages：<值>
- 是否一致：YES/NO

失败详情
- 首个失败用例：<名称或无>
- 可复现步骤：<命令/测试入口>
- 实际输出：<关键原文>
- 预期结果：<内容>
- 根因判断：<证据充分的判断；不确定则明确写不确定>

代码变更
- 测试新增/修改文件：<列表>
- 实现修复文件：<列表或未授权修复>
- 文件系统修改：无

结论
- READY：全部必需测试通过
- NOT READY：存在失败或关键项目未执行
```

## 14. Agent 最终约束

- 不得把“能进入 shell”当作完整验证。
- 不得把部分 `usertests` 输出当作全部通过。
- 不得隐瞒预期 panic 测试未运行。
- 不得把调试打印造成的时序变化当作并发问题已经解决。
- 不得修改文件系统来掩盖 Buddy 内存破坏。
- 不得猜测测试结果。
- 每个 PASS 必须对应实际执行过的命令或检查。
- 最终应明确给出 `READY` 或 `NOT READY`。

