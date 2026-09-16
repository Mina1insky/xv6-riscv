# xv6 Slab 实现与验证指南

> 面向阅读、评审与学习记录
>
> 分支：`slab-dev`（自 `mm-buddy-slab` 的 Buddy 基线 `6eb4dad` 创建，冻结标签 `buddy-v1`）
>
> 范围：在已完成并测试的 Buddy 分配器之上实现教学型 Slab，并把 `struct pipe` 迁移为首个真实使用者

## 0. 实现速览

| 项目 | 内容 |
|---|---|
| 基线 | `buddy-v1` = `6eb4dad`（Buddy 实现 + 测试文档） |
| 开发分支 | `slab-dev` |
| 提交 | `99ef4f3` mm: add slab allocator core with checks and selftests<br>`453db5a` mm: reclaim empty slabs when the cache is idle<br>`5301054` ipc: allocate pipe objects from a slab cache<br>`8b1ba86` docs: record slab implementation and verification results<br>`5a4e89c` test: avoid uninitialized pointer reads in slab T2<br>`9108f2f` mm: add quiescent slab cache destruction<br>`1ad4b39` test: cover slab lifecycle layout and repeated reclaim<br>`92eabdc` test: stress pipe slab allocation on multiple harts<br>docs: record slab completion and verification results（本文件所在提交） |
| 修改文件 | 新增 [`kernel/slab.c`](kernel/slab.c)、[`user/pstress.c`](user/pstress.c)；修改 [`kernel/defs.h`](kernel/defs.h#L70-L82)、[`kernel/main.c`](kernel/main.c#L20)、[`kernel/pipe.c`](kernel/pipe.c#L22-L97)、[`Makefile`](Makefile#L11) |
| 调试宏 | `SLAB_SELFTEST`（T1–T9 自测）、`SLAB_PANIC_CASE=1..5`（预期 panic），生产构建不定义 |
| 页来源 | 仅 `buddy_alloc(0)` / `buddy_free(page, 0)`，未修改 `kalloc.c` |
| 生命周期 | `kmem_cache_create/destroy` 成对；槽位可复用；destroy 为静默接口 |
| 测试结论 | T1–T9 PASS；P1–P5 按预期 panic；`pstress` PASS；Buddy D1–D7 PASS；`usertests -q` CPUS=3 ×3 与 CPUS=1 ×1 全部 `ALL TESTS PASSED` |
| 提交状态 | 已 commit 到本地 `slab-dev`，未 push |

## 1. 目标与边界

Slab 是 Buddy 之上的内核对象分配层：Buddy 管页，Slab 管页内小对象。

```mermaid
flowchart TD
    A["内核子系统：pipe、未来 kmalloc"] --> B["kmem_cache"]
    B --> C["Slab：页内对象切分与复用"]
    C --> D["Buddy：buddy_alloc(0) / buddy_free(page,0)"]
    D --> E["物理内存"]
```

首版明确不做：NUMA、per-CPU cache、后台回收线程、constructor/destructor、通用 `kmalloc`、动态分配 cache 描述符、迁移 `proc`/`file`/`inode`/`buf`；不修改页表、文件系统格式与系统调用 ABI。

## 2. 分层与锁顺序

固定锁顺序：

```text
caches_lock                （只保护描述符槽的创建/销毁/枚举）
cache->lock -> buddy.lock  （增长时可持有 cache 锁进入 Buddy）
```

- `slab_grow_locked()` 在持有 cache 锁时调用 [`buddy_alloc(0)`](kernel/kalloc.c#L259)，因此形成上面的顺序。
- 归还页时必须先摘链、清除 magic、释放 cache 锁，再调用 `buddy_free()`；归还后不再访问该页头。
- `caches_lock` 绝不会与 `cache->lock` 同时持有，也不会在持有它时进入 Buddy。
- Buddy 从不调用 Slab，不存在反向依赖。
- `kmem_cache_destroy()` 是**静默接口**：调用者必须保证没有其他 CPU 正在 alloc/free；销毁流程见 5.5。
- `slabinit()` 由 boot CPU 在启动阶段调用一次（[`kernel/main.c:20`](kernel/main.c#L20)）。

## 3. 常量与数据结构

常量见 [`slab.c:22-28`](kernel/slab.c#L22-L28)：

```c
#define KMEM_CACHE_MAX       16
#define KMEM_CACHE_NAME_LEN  16
#define SLAB_MIN_STRIDE      16
#define SLAB_BITMAP_WORDS    4
#define SLAB_MAX_OBJECTS     (SLAB_BITMAP_WORDS * 64)
#define SLAB_MAGIC           0x5A15AB5A15AB5A15ULL
#define SLAB_MAX_PAGES       ((PHYSTOP - KERNBASE) / PGSIZE)
```

三种链表状态（[`slab.c:30-35`](kernel/slab.c#L30-L35)）：`SLAB_NONE`、`SLAB_FREE`、`SLAB_PARTIAL`、`SLAB_FULL`。每个 slab 一页，页首是 [`struct slab`](kernel/slab.c#L37-L47)，包含 magic、所属 cache、链表指针、页内 freelist、`inuse/total`、状态和 256 位分配位图；[`struct slab_list`](kernel/slab.c#L49-L52) 保存链表头与节点数。

cache 描述符见 [`slab.c:54-72`](kernel/slab.c#L54-L72)：名称、`object_size/stride/align/object_offset/objects_per_slab`、三条 slab 链表、`live_objects` 与 alloc/free/grow/reap/fail 统计。描述符来自固定数组 [`caches[16]`](kernel/slab.c#L74)，由 [`caches_lock`](kernel/slab.c#L75) 保护创建/销毁，避免“用 Slab 分配描述符”的启动递归。

位图的作用：可靠识别重复释放，并让检查器交叉验证 `inuse`、位图与 freelist（[`bitmap_popcount()`](kernel/slab.c#L119)）。

## 4. 页内对象布局

`kmem_cache_create()` 的布局计算（[`slab.c:377-435`](kernel/slab.c#L377-L435)）：

```c
  sz = object_size < SLAB_MIN_STRIDE ? SLAB_MIN_STRIDE : object_size;
  if (align_up_checked(sz, align, &stride) != 0)
    return 0;
  if (align_up_checked(sizeof(struct slab), align, &offset) != 0)
    return 0;
  if (offset >= PGSIZE)
    return 0;
  count = (PGSIZE - offset) / stride;
  if (count > SLAB_MAX_OBJECTS)
    count = SLAB_MAX_OBJECTS;
  if (count == 0)
    return 0;
  if ((uint64)offset + (uint64)stride * count > PGSIZE)
    return 0;
```

规则：`align == 0` 或小于指针宽度时提升为指针宽度；必须是 2 的幂且不超过 `PGSIZE`；`stride = align_up(max(object_size, 16), align)`；对象区不越界且至少一个对象；失败不占用描述符槽。

实际 pipe cache 的布局（调试构建 `kmem_cache_dump` 输出）：

```text
slab pipe: size 552 stride 552 align 8 off 88 per-slab 7
```

即 `sizeof(struct pipe)=552`，每页 7 个对象，而迁移前每个 pipe 独占一页。

## 5. 核心实现

### 5.1 slabinit

[`slabinit()`](kernel/slab.c#L350)（L350-375）：初始化 `caches_lock` 与 16 个槽，置 `slab_ready`，随后按需运行自测或 panic 用例。只允许 boot CPU 调用。

### 5.2 slab 增长

[`slab_grow_locked()`](kernel/slab.c#L214)（L214-249）：持 cache 锁调用 `buddy_alloc(0)`；失败 `nr_fail++`；整页先清零再初始化页头，按 `object_offset + i*stride` 从尾到头串出页内 freelist，加入 `free` 链并 `nr_grow++`。freelist 节点就是空闲对象槽本身。

### 5.3 对象分配

[`kmem_cache_alloc()`](kernel/slab.c#L437)（L437-484）：拒绝空/未使用/未初始化 cache；优先 `partial.head`，其次 `free.head`，都为空则增长；校验 magic/归属/freelist/`inuse<total`；弹出 freelist 头，计算槽号并置位；`inuse++`、`live_objects++`、`nr_alloc++`；按新占用量迁移到 `PARTIAL` 或 `FULL`；释放锁后用 `0x05` 填满整个 `stride` 并返回。

### 5.4 对象释放与回收

[`kmem_cache_free()`](kernel/slab.c#L486)（L486-547）：空指针 panic；地址必须落在 `[KERNBASE, PHYSTOP)`；按页向下取整得到页头后才在锁内检查 magic/归属/状态；`slab_object_index()` 保证对象恰好是槽首（否则 panic `slab free interior`）；位图 bit 必须为 1（否则 panic `slab free double`）；清位、减计数、`0x01` poison 整个槽后再把槽首作为 freelist next 入链；按占用量迁移。

空 slab 的回收策略（L527-544）：

```c
  for (;;) {
    int idle = cache->partial.count == 0 && cache->full.count == 0;

    if (cache->free.count > 1 || (idle && cache->free.count > 0)) {
      reap = cache->free.head;
      slab_list_del(&cache->free, reap, SLAB_FREE);
      reap->magic = 0;
      cache->nr_reap++;
      release(&cache->lock);
      buddy_free(reap, 0);
      acquire(&cache->lock);
      continue;
    }
    break;
  }
```

- 缓存仍活跃（存在 partial/full slab）时保留一张热空页；
- 当整个 cache 空闲（没有 partial/full）时，归还全部空页，使 Buddy 空闲页计数精确恢复（见第 6 节的原因）。

### 5.5 shrink 与 destroy

[`kmem_cache_shrink()`](kernel/slab.c#L550)（L550-572）：循环摘除所有 `SLAB_FREE` slab，锁外逐页 `buddy_free`，返回归还页数；不回收 partial/full。

[`kmem_cache_destroy()`](kernel/slab.c#L581)（L581-604）是新增的静默生命周期接口：

```c
// Destroy a cache.  This is a quiescent interface: the caller must
// guarantee that no other CPU is concurrently allocating from or freeing
// to this cache.  Returns 0 on success, or -1 if the cache is invalid or
// still has objects in use.  All empty slabs are returned to Buddy and
// the descriptor slot becomes reusable.
int
kmem_cache_destroy(struct kmem_cache *cache)
{
  struct slab *s;

  // Reading used without cache->lock is safe only because the caller
  // guarantees quiescence (see the comment above).
  if (cache == 0 || !cache->used || !slab_ready)
    return -1;

  for (;;) {
    acquire(&cache->lock);
    if (cache->live_objects != 0 ||
        cache->partial.count != 0 || cache->full.count != 0) {
      release(&cache->lock);
      return -1;
    }
    if (cache->free.head == 0) {
      release(&cache->lock);
      break;
    }
    s = cache->free.head;
    slab_list_del(&cache->free, s, SLAB_FREE);
    s->magic = 0;
    release(&cache->lock);
    buddy_free(s, 0);
  }

  acquire(&caches_lock);
  memset(cache, 0, sizeof(*cache));
  cache->used = 0;
  release(&caches_lock);
  return 0;
}
```

语义：成功 0；`cache==0`、未激活或仍有在用对象（`live_objects/partial/full` 非零）返回 -1，不 panic；归还每页都在 cache 锁之外进行；描述符清零时 `used=0` 放最后，槽位可被后续 `kmem_cache_create()` 复用。

### 5.6 检查器与观测

[`slab_check_locked()`](kernel/slab.c#L321) 及两个辅助（[`slab_check_freelist`](kernel/slab.c#L252)、[`slab_check_list`](kernel/slab.c#L273)）验证：

| 不变量 | 实现位置 |
|---|---|
| 链表无环（节点数上限 `SLAB_MAX_PAGES`） | [`L284`](kernel/slab.c#L284) |
| 页对齐、位于 `[KERNBASE, PHYSTOP)` | [`L286`](kernel/slab.c#L286) |
| magic/cache/state 与所在链一致 | [`L288`](kernel/slab.c#L288) |
| `prev` 关系正确 | [`L290`](kernel/slab.c#L290) |
| `total == objects_per_slab`、`inuse <= total` | [`L292`](kernel/slab.c#L292) |
| free 链 `inuse == 0` | [`L294`](kernel/slab.c#L294) |
| partial 链 `0 < inuse < total` | [`L296`](kernel/slab.c#L296) |
| full 链 `inuse == total` 且 freelist 为空 | [`L298`](kernel/slab.c#L298) |
| 非 full 链 freelist 非空 | [`L300`](kernel/slab.c#L300) |
| 位图 popcount == `inuse`，超出 `total` 的位为 0 | [`L302-L306`](kernel/slab.c#L302-L306) |
| freelist 无环、无重复、槽合法且 bit 为 0、长度 == `total-inuse` | [`slab_check_freelist`](kernel/slab.c#L252) |
| 三链在用对象汇总 == `live_objects`，实际节点数 == `count` | [`L310-L316`](kernel/slab.c#L310-L316) |

[`kmem_cache_check()`](kernel/slab.c#L616) 只加一次锁；[`kmem_cache_dump()`](kernel/slab.c#L630) 在锁内打印布局与统计。

## 6. 回收策略与设计取舍

任务书 §8 第 10 步原义是“free 链超过 1 页时摘除一张空页”，即长期保留一张热空页。实现中先按该策略实现，随后在 `usertests -q` 中观察到：

```text
FAILED -- lost some free pages 32260 (out of 32261)
```

原因是 `usertests` 的 `drivetests()` 会在测试前后用 `sbrk` 统计空闲物理页；`pipe1` 关闭所有 pipe 后，pipe cache 保留的热空页使计数少 1，导致原有测试失败。若维持该策略，则无法满足任务书“`usertests -q` 通过”的验收项。

最终采用的策略兼容两者：

1. cache 活跃时保留一张热空页（任务书默认行为）；
2. cache 完全空闲时归还全部空页（等价于自动 shrink）。

调试构建下的实测统计（pipe 压力后）：

```text
slab pipe: slabs free 0 partial 0 full 0, live 0
slab pipe: alloc 232 free 232 grow 4 reap 4 fail 0
```

`grow == reap`、`alloc == free`、`live == 0`，说明对象与页都完整归还；同时 `usertests` 的空闲页校验通过。该差异已在第 10 节记录。

## 7. pipe 接入

[`kernel/pipe.c`](kernel/pipe.c) 的修改：

- 静态 `pipe_cache` 与初始化（[`L22-L37`](kernel/pipe.c#L22-L37)）：

```c
static struct kmem_cache *pipe_cache;

void
pipeinit(void)
{
  pipe_cache = kmem_cache_create("pipe", sizeof(struct pipe), 0);
  if (pipe_cache == 0)
    panic("pipe cache");
}
```

- `pipealloc()` 用 `kmem_cache_alloc(pipe_cache)` 取代 `kalloc()`（[`L48`](kernel/pipe.c#L48)），bad 路径用同一 cache 释放（[`L67`](kernel/pipe.c#L67)）。
- `pipeclose()` 最终释放改用 `kmem_cache_free(pipe_cache, pi)`（[`L88`](kernel/pipe.c#L88)），锁释放位置、读写唤醒和字段初始化均未改变。
- [`main.c:31`](kernel/main.c#L31) 在 `fileinit()` 之后、任何用户进程启动之前调用 `pipeinit()`。

调试构建下 `pipeclose()` 会在释放后调用 `kmem_cache_check()`，并在跨过多个 slab 且 live 归零时 dump 统计（[`L89-L98`](kernel/pipe.c#L89-L98)）；这段代码由 `#ifdef SLAB_SELFTEST` 保护，生产构建编译掉。

## 8. 自测、压力与预期 panic

### 8.1 内核自测（`SLAB_SELFTEST`）

在 `slabinit()` 末尾运行（[`slab_selftest()`](kernel/slab.c#L1180)）：

| 用例 | 内容 | 实际输出 |
|---|---|---|
| T1 | 创建参数：合法创建；size 0、非 2 幂对齐、对齐过大、超大对象失败；失败不消耗槽；销毁后槽位恢复 | `slab: T1 ok` |
| T2 | 64B cache 分配 3 个对象，非空/对齐/互异（只比较已分配对象）/写满/释放/计数 | `slab: T2 ok` |
| T3 | 跨 slab（`n+3` 个对象）与状态迁移：full+partial、热页保留、idle 时全部回收 | `slab: T3 ok (62 objects/slab)` |
| T4 | 16/24/64/128/512/1024 字节 6 种 cache，跨页、对齐、边界字节、两两不重叠 | `slab: T4 ok` |
| T5 | 固定种子 xorshift、256 记录槽、5000 次混合分配释放、每 100 次 check | `slab: T5 ok` |
| T6 | 两个 slab：空 slab 热保留、`shrink` 精确归还一张、释放后页数恢复 | `slab: T6 ok` |
| T7 | cache 生命周期：busy destroy 返回 -1、正常 destroy、槽位复用（>16 次创建/销毁） | `slab: T7 ok` |
| T8 | 边界布局：size 1/15/16/17/24/64/512/1024 与 align 0/8/16/64/PGSIZE；非法参数不占槽 | `slab: T8 ok` |
| T9 | 8 轮跨 slab 增长/乱序释放：每轮 `grow == reap`、页数恢复、check 通过 | `slab: T9 ok` |

### 8.2 预期 panic（`SLAB_PANIC_CASE=1..5`）

每次启动只跑一个（[`slab_panic_test()`](kernel/slab.c#L1206)）：

| 用例 | 操作 | 实际 panic 文本 |
|---|---|---|
| P1 | 同一对象释放两次（保留同 slab 另一对象，确保走到位图检查） | `panic: slab free double` |
| P2 | 用错误 cache 释放对象 | `panic: slab free slab` |
| P3 | 释放对象内部地址 | `panic: slab free interior` |
| P4 | 通过 cache 释放原始 Buddy 页 | `panic: slab free slab` |
| P5 | 篡改 slab magic 后释放 | `panic: slab free slab` |

### 8.3 pipe 压力程序

[`user/pstress.c`](user/pstress.c) 作为常驻测试程序提交（`Makefile` 的 `UPROGS` 含 `_pstress`）：

- `loop_create_close(200)` 循环创建/关闭；
- `many_alive(5)`：5 个子进程各持 5 个 pipe，同时存活 25 个对象，使 pipe cache 跨多个 slab（受 `NOFILE=16` 限制，不能由单进程堆叠）；
- `fork_pipes(30)`：父子交叉关闭读/写端；
- `closed_read()/closed_write()`：对端提前关闭后写失败/读到 EOF；
- 输出 `pstress: start` 与 `pstress: OK`。

## 9. 验证记录

### 9.1 环境与命令

- gcc 9.3.0、QEMU 7.2.0；`make clean && make && make fs.img`。
- 自测构建：`make DETFLAGS="-ffile-prefix-map=$(pwd)=. -DSLAB_SELFTEST"`。
- panic 用例：`make DETFLAGS="-ffile-prefix-map=$(pwd)=. -DSLAB_PANIC_CASE=N"`。
- 运行使用 pexpect 脚本 `/tmp/opencode/xv6_run.py`（`XV6_CPUS` 可选，检测到 panic 立即失败）与 `/tmp/opencode/xv6_panic.py`；日志在 `/tmp/opencode/`。

### 9.2 结果

| 验证项 | 结果 | 证据 |
|---|---|---|
| 修改前基线 `usertests -q` | PASS | `ALL TESTS PASSED` |
| 生产构建（`-Werror`） | PASS | 无 warning/链接错误 |
| 生产二进制无自测路径 | PASS | `strings kernel/kernel \| grep -c 'slab selftest'` = 0 |
| T1–T9 自测 | PASS | `slab: T1..T9 ok`，`all selftests passed, free pages 32538` |
| selftest 槽位归零 | PASS | `slab selftest slots` 断言未触发 |
| P1–P5 预期 panic | PASS | 5 个 panic 文本与预期逐字一致 |
| pipe 压力（`pstress`） | PASS | 调试与生产构建均 `pstress: OK`；`grow == reap`、`live 0` |
| 调试构建 `usertests -q` | PASS | `ALL TESTS PASSED`，每 close 执行 `kmem_cache_check` 无 panic |
| Buddy D1–D7（含 slab 代码） | PASS | `buddy: D1..D7 ok`，`free pages 32474` |
| CPUS=3 冷启动 ×3 `usertests -q` | PASS | 3/3 `ALL TESTS PASSED`，无 panic |
| CPUS=1 冷启动 `usertests -q` | PASS | `ALL TESTS PASSED` |
| shell 管道 `echo hello \| cat` | PASS | 输出 `hello` |
| `forktest`、`stressfs` | PASS | `fork test OK`，无 panic |
| `git diff --check` | PASS | 无空白错误 |
| 文件系统/页表/用户 ABI | 未修改 | diff 仅内核分配器与测试程序 |

Buddy D6 从纯 Buddy 构建的 32476 页变为 32474 页：`slab.c` 的代码与静态数据使内核 `end` 上移 2 页，属预期。

### 9.3 关键输出片段

```text
slab: T1 ok
slab: T2 ok
slab: T3 ok (62 objects/slab)
slab: T4 ok
slab: T5 ok
slab: T6 ok
slab: T7 ok
slab: T8 ok
slab: T9 ok
slab: all selftests passed, free pages 32538

slab pipe: size 552 stride 552 align 8 off 88 per-slab 7
...
pipe cache with no live pipes:
slab pipe: slabs free 0 partial 0 full 0, live 0
slab pipe: alloc 232 free 232 grow 4 reap 4 fail 0
pstress: OK
...
usertests starting
...
ALL TESTS PASSED
```

## 10. 与任务书的差异

1. **空 slab 回收策略**：任务书 §8 第 10 步要求“free 链超过 1 页时只摘除一张”，实现为“活跃时保留一张热页、完全空闲时全部归还”。原因是前者使 `usertests` 的 `countfree()` 少一页而失败（实测 `lost some free pages 32260 (out of 32261)`），与任务书 §12/§15 的 usertests 验收冲突。
2. **提交拆分**：任务书建议 5 个提交，实际核心工作用了 4 个提交（core+checks+selftests、idle reclaim、pipe 接入、实现指南），完善阶段按完善计划再增加 4 个功能提交 + 1 个文档提交。
3. **调试辅助函数**：新增 `kmem_cache_live()`、`kmem_cache_grow_count()` 仅在 `SLAB_SELFTEST` 下编译，供 pipe 调试仪器使用；生产构建不包含。
4. **P1 用例调整**：空闲 slab 会被自动回收，若只分配一个对象，第一次释放后 slab 即归还 Buddy，第二次释放会报 `slab free slab` 而非 `slab free double`。P1 因此保留同 slab 的第二个对象，确保测试的是位图重复释放路径。
5. **destroy 的并发限制**：`kmem_cache_destroy()` 只支持静默销毁（见完善计划 §5），不支持与 alloc/free 并发，函数注释中已明确。
6. **`pstress` 保留提交**：按完善计划要求，`user/pstress.c` 不再临时删除，而是作为常驻压力测试提交。

## 11. 复现步骤

```bash
git switch slab-dev
make clean && make && make fs.img          # 生产构建
make qemu                                  # 进入 shell 后执行 usertests -q / pstress

# Slab 自测
make clean
make DETFLAGS="-ffile-prefix-map=$(pwd)=. -DSLAB_SELFTEST" && make fs.img
make qemu                                  # 启动即输出 T1-T9

# 预期 panic 用例 N=1..5
make clean
make DETFLAGS="-ffile-prefix-map=$(pwd)=. -DSLAB_PANIC_CASE=N" && make fs.img
make qemu
```

## 12. 参考

- 任务书：[`xv6-slab-agent-task.md`](xv6-slab-agent-task.md)
- 设计计划：[`xv6-slab-migration-plan.md`](xv6-slab-migration-plan.md)
- 完善计划：[`xv6-slab-completion-plan.md`](xv6-slab-completion-plan.md)
- Slab 实现：[`kernel/slab.c`](kernel/slab.c)
- 压力程序：[`user/pstress.c`](user/pstress.c)
- Buddy 实现：[`kernel/kalloc.c`](kernel/kalloc.c)、[`xv6-buddy-implementation-guide.md`](xv6-buddy-implementation-guide.md)
- pipe 接入：[`kernel/pipe.c`](kernel/pipe.c)、[`kernel/main.c`](kernel/main.c#L20)
- 对外声明：[`kernel/defs.h:70-82`](kernel/defs.h#L70-L82)
