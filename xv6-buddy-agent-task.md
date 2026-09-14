# Coding Agent 执行任务：在原版 xv6-riscv 中实现 Buddy

## 任务目标

在用户已经拉取并跑通的 MIT 原版 `xv6-riscv` 仓库中，于 `mm-buddy-slab` 分支实现教学版 Buddy 物理页分配器。

实现后：

- 保持 `kalloc()`/`kfree()` 的单页接口和语义；
- 新增按 order 分配、释放连续物理页的接口；
- 不实现 Slab；
- 不修改文件系统；
- 不新增系统调用；
- 构建、启动和原有用户测试应通过。

## 执行权限与安全边界

1. 在仓库根目录工作。
2. 开始前读取仓库中的 `AGENTS.md`（若存在）并遵守。
3. 确认当前分支是 `mm-buddy-slab`；若不是，停止并报告，不要擅自切换到不明确的分支。
4. 执行 `git status --short`。已有修改属于用户：不得覆盖、清理、stash、reset 或 checkout 掉这些修改。
5. 记录 `git rev-parse HEAD`，在最终报告中注明实际基线提交。
6. 不提交、不 push，除非用户另行明确要求。
7. 不运行破坏性 Git 命令。

## 允许修改的文件

必须修改：

```text
kernel/kalloc.c
kernel/defs.h
```

仅在确有必要进行一次启动自测时允许修改：

```text
kernel/main.c
```

如果修改 `main.c`，自测调用必须由 `BUDDY_DEBUG` 条件编译保护，或在验证后恢复为无需 `main.c` 改动的状态。

禁止修改：

```text
kernel/fs.c
kernel/bio.c
kernel/log.c
kernel/file.c
kernel/sysfile.c
mkfs/
user/
```

也不要为了 Buddy 修改 `vm.c`、`proc.c` 或 `pipe.c`。它们应继续使用兼容的 `kalloc()`/`kfree()`。

若本地源码结构与 MIT 原版明显不同，以“保持调用者不变”的原则停止扩张范围并报告差异，不要自行做大面积适配。

## 基线检查

执行并记录：

```bash
git branch --show-current
git status --short
git rev-parse HEAD
rg '\\bkalloc\\s*\\(|\\bkfree\\s*\\(' kernel
```

构建基线（若尚未在此工作区验证）：

```bash
make
```

若基线本身不能构建，停止修改并报告原始错误。

## 实现规格

### 1. 常量与基本语义

在 `kernel/kalloc.c` 内定义：

```c
#define BUDDY_MAX_ORDER 14
#define BUDDY_NR_PAGES ((PHYSTOP - KERNBASE) / PGSIZE)
```

语义：

```text
order n = 2^n 个连续物理页
块大小 = PGSIZE << n
```

所有阶数对齐以 `KERNBASE` 对应的页索引 0 为基准。

### 2. 元数据

实现等价于以下结构的数据设计，命名可以为适应本地风格做小幅调整，但语义不能改变：

```c
enum page_state {
  PAGE_UNUSED = 0,
  PAGE_FREE,
  PAGE_ALLOCATED,
};

struct page {
  struct page *next;
  struct page *prev;
  short order;
  uchar state;
};

struct free_area {
  struct page *head;
  uint64 nr_blocks;
};

static struct page pages[BUDDY_NR_PAGES];

static struct {
  struct spinlock lock;
  struct free_area area[BUDDY_MAX_ORDER + 1];
  uint64 managed_start;
  uint64 managed_first_index;
  uint64 free_pages;
  uint64 nr_alloc;
  uint64 nr_free;
  uint64 nr_split;
  uint64 nr_merge;
  uint64 nr_fail;
} buddy;
```

规则：只有块首页描述符携带有效 state/order；块内其他页保持 `PAGE_UNUSED`。静态 `pages[]` 位于 BSS，初始化的受管起点必须重新使用链接器符号 `end` 计算。

### 3. 地址辅助函数

实现内部辅助函数：

```c
pa_to_index(pa) = (pa - KERNBASE) / PGSIZE
index_to_pa(i)  = KERNBASE + i * PGSIZE
page_to_index(page)
page_to_pa(page)
```

计算 buddy：

```c
buddy_index = index ^ (1ULL << order);
```

禁止以 `managed_start` 或 `end` 为异或基址。禁止直接对绝对物理地址进行未经说明的异或。

### 4. 每阶双向空闲链表

实现：

```c
static void free_list_add(struct page *, int order);
static void free_list_del(struct page *, int order);
```

`add` 必须设置 state/order、维护 head/prev/next、增加 `nr_blocks`。

`del` 必须正确处理头节点和中间节点、清空 prev/next、把被摘节点重置为 `PAGE_UNUSED`/`order = -1`、减少 `nr_blocks`。

内部辅助函数默认由调用者保证已持锁，或仅在单 CPU 启动初始化阶段调用。用注释明确这一约定。

### 5. 重写 `kinit()`

删除原来依赖逐页 `kfree()` 的 `freerange()` 初始化路径，或让它不再参与初始化。

初始化步骤：

1. 初始化 Buddy 锁。
2. 清零各阶链表和统计。
3. 把所有描述符设置为 `PAGE_UNUSED`、`order = -1`、指针为空。
4. 设置：

   ```c
   managed_start = PGROUNDUP((uint64)end);
   managed_first_index = pa_to_index(managed_start);
   limit = pa_to_index(PHYSTOP);
   ```

5. 从 `managed_first_index` 开始，将空闲范围分解为“尽可能大的、按 order 对齐且不越界”的块。
6. 每块加入相应空闲链表，并精确累计 `free_pages`。
7. 调用不重复加锁的内部一致性检查；失败则 `panic("buddy init")`。

不得把内核镜像、静态元数据或 `[KERNBASE, managed_start)` 加入空闲链表。

### 6. 实现 `buddy_alloc(int order)`

要求：

1. 非法 order 返回 0。
2. 持锁从请求 order 向更高 order 搜索第一个非空链表。
3. 无块时增加 `nr_fail`，释放锁并返回 0。
4. 摘下高阶块。
5. 逐阶拆分；每次让左半块继续拆，把右半块加入下一低阶链表。
6. 最终块首页标记为 `PAGE_ALLOCATED`，记录请求 order。
7. `free_pages` 减少 `1ULL << order`，`nr_alloc` 增加。
8. 释放锁。
9. 沿用 xv6 调试行为，用 `5` 填充整个返回块。
10. 返回内核可直接使用的物理地址指针。

`memset` 的长度必须是整个块，不是固定一个页。

### 7. 实现 `buddy_free(void *pa, int order)`

参数检查：

- order 合法，否则 panic；
- pa 非空且按 `PGSIZE` 对齐；
- pa 位于 `[managed_start, PHYSTOP)`；
- 块首页索引满足 `2^order` 页对齐；
- 块尾不越过 `PHYSTOP`；
- 锁内确认首页为 `PAGE_ALLOCATED`；
- 锁内确认记录的 order 等于传入 order。

非法地址、重复释放、释放块内部地址或 wrong-order free 都必须 panic，不能静默接受。

验证通过后：

1. 在块进入空闲链表前用 `1` poison 整个块。第一版允许为保证正确性在 Buddy 锁内执行 poison。
2. 将当前头描述符重置为未挂链状态。
3. 当 `order < BUDDY_MAX_ORDER` 时计算 buddy。
4. buddy 必须完整位于受管范围内，且其首页为同阶 `PAGE_FREE`，才能合并。
5. 从该阶链表删除 buddy。
6. 选择两个索引中较小者作为新块首页，order 增加，继续尝试。
7. 最终块加入相应空闲链表。
8. `free_pages` 只增加原始释放块的页数；每次合并不得重复增加。
9. 更新 `nr_free` 和 `nr_merge`。

### 8. 兼容包装

必须保持：

```c
void *
kalloc(void)
{
  return buddy_alloc(0);
}

void
kfree(void *pa)
{
  buddy_free(pa, 0);
}
```

现有调用者不应修改。

### 9. 检查与观测接口

实现：

```c
static int buddy_check_locked(void);
int buddy_check(void);
uint64 buddy_free_pages(void);
void buddy_dump(void);
```

`buddy_check_locked()` 至少验证：

- 节点位于 `pages[]`；
- state/order 正确；
- prev/next 一致；
- 地址满足 order 对齐；
- 块位于受管区且不越界；
- 实际节点数等于每阶 `nr_blocks`；
- 加权空闲页数等于 `free_pages`；
- `order < BUDDY_MAX_ORDER` 时不存在可继续合并的一对同阶空闲 buddy；
- 每条链表遍历次数不超过 `BUDDY_NR_PAGES`，防止链表环死循环。

公开函数自行获取 Buddy 锁。内部持锁路径只能调用 `buddy_check_locked()`，避免递归加锁。

`buddy_dump()` 输出每阶块数、总空闲页数和 alloc/free/split/merge/fail 计数。先检查本地 `printk` 支持的格式，不引入编译器格式警告。

在 `kernel/defs.h` 的 `// kalloc.c` 区域增加：

```c
void *buddy_alloc(int);
void  buddy_free(void *, int);
int   buddy_check(void);
void  buddy_dump(void);
uint64 buddy_free_pages(void);
```

保留原来的 `kalloc`、`kfree`、`kinit` 声明。

### 10. 调试自测

在 `kernel/kalloc.c` 内实现受 `#ifdef BUDDY_DEBUG` 保护的自测，或使用同等的不进入正式路径的方法。

自测至少：

1. 保存 `buddy_free_pages()`。
2. 申请两个 order 0 和一个 order 2 块。
3. 验证 order 2 块相对 `KERNBASE` 的对齐。
4. 以不同顺序释放。
5. 验证 `buddy_check()` 成功。
6. 验证空闲页数完全恢复。

若在 `kinit()` 内直接执行自测，注意初始化阶段的锁状态以及公开检查函数的加锁行为，不要造成重复加锁。

正式提交状态不能依赖持续打印大量调试信息。

## 实现中的关键不变量

1. 同一个块只能处于 allocated、free-list 或 unused/interior 三种情况之一。
2. 只有块首页能出现在空闲链表。
3. 任意空闲块都满足自身 order 对齐。
4. 两个可合并的同阶 buddy 不应长期同时留在空闲链表。
5. `free_pages` 表示实际空闲页数，不表示空闲块数量。
6. 拆分不改变总空闲页数；完成一次分配时才扣除请求页数。
7. 合并不改变总空闲页数；完成一次释放时只增加原块页数。
8. `kalloc/kfree` 始终只操作 order 0。

## 验证步骤

### A. 变更范围

```bash
git diff --check
git diff --stat
git status --short
git diff -- kernel/kalloc.c kernel/defs.h kernel/main.c
```

确认没有批准范围之外的文件被修改。如果出现其他文件，判断是否为修改前已存在；不得篡改用户已有变更。

### B. 构建

```bash
make clean
make
```

要求无新增 warning、无链接错误。

### C. 启动和回归

优先使用仓库已有的非交互测试方式。若只能交互运行：

```bash
make qemu
```

进入 xv6 shell 后运行：

```text
usertests -q
```

若该基线不接受 `-q`，运行 `usertests`。

再进行基本检查：

```text
echo hi
ls
cat README
```

退出 QEMU 使用该仓库提示的正常快捷键，不粗暴终止可能仍在写磁盘镜像的进程。

若完整 `usertests` 因环境或超时无法跑完，至少报告：已完成到哪个测试、最后输出、是否发生 panic；不得声称全部通过。

### D. 失败诊断顺序

发生 panic 或测试失败时依次检查：

1. `managed_start` 是否在静态 `pages[]` 之后。
2. 初始化块是否同时满足对齐和范围限制。
3. 拆分时右半块索引是否为 `index + 2^current`。
4. 分配块首页的 state/order 是否正确。
5. 合并前是否验证 buddy state/order。
6. buddy 是否从旧链表删除。
7. 合并后是否选择较小索引。
8. `free_pages` 是否在拆分/合并中被重复修改。
9. poison 是否覆盖了已加入链表的 `next/prev`。
10. 是否错误地把绝对地址对齐当成其他基准的对齐。

## 完成条件

全部满足才算完成：

- 仅批准的文件包含本任务新增修改。
- `make clean && make` 成功。
- xv6 启动进入 shell。
- `kalloc()`/`kfree()` 的所有原调用点无需修改。
- Buddy 拆分、释放、合并和耗尽失败路径正确。
- 自测后空闲页计数恢复。
- `buddy_check()` 通过。
- `usertests` 通过；若没跑完，明确标为未验证。
- 没有修改文件系统、磁盘镜像格式、用户 ABI 或系统调用表。

## 最终回复格式

完成后向用户报告：

```text
基线提交：<git rev-parse HEAD>
当前分支：mm-buddy-slab

修改文件：
- kernel/kalloc.c：<简述>
- kernel/defs.h：<简述>
- kernel/main.c：<若未修改写“未修改”>

实现要点：
- 最大 order：14
- kalloc/kfree：order 0 兼容包装
- 初始化方式：最大对齐块分解
- 检查：<已实现内容>

验证：
- git diff --check：PASS/FAIL
- make clean && make：PASS/FAIL
- 启动到 shell：PASS/FAIL/未验证
- buddy 自测：PASS/FAIL/未验证
- usertests：PASS/FAIL/未完整验证

未解决问题：<无或具体列出>
```

不要只说“已完成”；必须如实给出实际执行过的验证及其结果。

