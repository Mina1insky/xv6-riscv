# Agent 执行任务书：在 xv6 Buddy 上实现教学型 Slab

## 0. 任务身份与边界

你要修改的仓库是 `Mina1insky/xv6-riscv`，基线分支是 `mm-buddy-slab`。任务书编写时远端 HEAD 为：

```text
49f7fb044189cb9829d262a7e26b208893dc7f1c
```

该基线已经实现并测试 Buddy。你的任务是在它之上实现一个可检查、可回收、线程安全的教学型 Slab，并把 `struct pipe` 迁移为第一个真实使用者。

必须遵守：

- 不重写 Buddy；Slab 只能通过现有 `buddy_alloc(0)`/`buddy_free(..., 0)`取还页。
- 不修改页表、用户地址空间、文件系统格式或系统调用 ABI。
- 不迁移 `proc`、`file`、`inode`、`buf` 等其他对象。
- 不实现 per-CPU cache、NUMA、通用 `kmalloc`、高阶 slab 或后台回收。
- 不用动态分配创建 `kmem_cache` 描述符。
- 不用 `kalloc()` 作为 Slab 页提供者；直接调用 Buddy 接口，使层次关系明确。
- 保留用户已有修改；禁止 reset、checkout 覆盖或清理无关工作。
- 每个阶段先检查和测试，再进入下一阶段。

## 1. 开始前检查

按顺序执行并记录结果：

```bash
git status --short
git branch --show-current
git rev-parse HEAD
git log -1 --oneline
```

处理规则：

1. 若工作区有与本任务重叠的未提交修改，停止并报告，不得覆盖。
2. 若 `mm-buddy-slab` HEAD 已经前进，不要回退；重新阅读实际 `kernel/kalloc.c`、`kernel/defs.h`、`kernel/main.c`、`kernel/pipe.c` 和 `Makefile`，确认本文接口假设仍成立。
3. 若尚无 Slab 开发分支，从当前 Buddy 完成提交创建 `slab-dev`：

```bash
git switch -c slab-dev
```

4. 在修改前运行现有 Buddy 与 xv6 回归，确认基线本身正常。

当前应确认的 Buddy API：

```c
void *buddy_alloc(int order);
void buddy_free(void *pa, int order);
int buddy_check(void);
uint64 buddy_free_pages(void);
```

且 `kalloc()`/`kfree()` 仍是 order 0 包装。

## 2. 交付物

必须完成：

- 新增 `kernel/slab.c`。
- 修改 `kernel/defs.h`。
- 修改 `kernel/main.c`。
- 修改 `kernel/pipe.c`。
- 修改 `Makefile`，加入 `kernel/slab.o`。
- 增加可由编译宏启用的 Slab 自测；生产构建默认关闭。
- 给出测试日志摘要和最终 diff 摘要。

除非确有跨文件共享常量，不要新增 `kernel/slab.h`。公开接口放在 `defs.h`，内部结构留在 `slab.c`。

## 3. 固定设计

### 3.1 常量

在 `kernel/slab.c` 内定义，具体 magic 数值可自选，但含义必须一致：

```c
#define KMEM_CACHE_MAX       16
#define KMEM_CACHE_NAME_LEN  16
#define SLAB_MIN_STRIDE      16
#define SLAB_BITMAP_WORDS    4
#define SLAB_MAX_OBJECTS     (SLAB_BITMAP_WORDS * 64)
#define SLAB_MAGIC           /* non-zero uint64 constant */
```

再定义 slab 链表状态，例如：

```c
enum slab_state {
  SLAB_NONE = 0,
  SLAB_FREE,
  SLAB_PARTIAL,
  SLAB_FULL,
};
```

不要依赖宿主标准库；只能使用 xv6 已有类型、`memset`、锁、panic 和打印能力。

### 3.2 数据结构

实现等价于下列信息的结构，字段命名可调整：

```c
struct slab {
  uint64 magic;
  struct kmem_cache *cache;
  struct slab *next;
  struct slab *prev;
  void *freelist;
  uint inuse;
  uint total;
  uint state;
  uint64 allocmap[SLAB_BITMAP_WORDS];
};

struct slab_list {
  struct slab *head;
  uint count;
};

struct kmem_cache {
  struct spinlock lock;
  int used;
  char name[KMEM_CACHE_NAME_LEN];
  uint object_size;
  uint stride;
  uint align;
  uint object_offset;
  uint objects_per_slab;
  struct slab_list free;
  struct slab_list partial;
  struct slab_list full;
  uint64 live_objects;
  uint64 nr_alloc;
  uint64 nr_free;
  uint64 nr_grow;
  uint64 nr_reap;
  uint64 nr_fail;
};
```

全局对象：

- 一个 cache 描述符数组 `caches[KMEM_CACHE_MAX]`。
- 一把只保护描述符创建的锁。
- 一个 `slab_ready` 标志。

不要把 cache 描述符本身放进 Slab。

### 3.3 页内布局算法

实现本地 `align_up(value, align)`；所有加法和对齐计算必须先防溢出。

创建 cache 时：

1. `object_size` 必须大于 0。
2. `align == 0` 时设为 `sizeof(void *)`。
3. align 小于指针宽度时提升为指针宽度。
4. align 必须是 2 的幂，且不得大于 `PGSIZE`。
5. `stride = align_up(max(object_size, SLAB_MIN_STRIDE), align)`。
6. `object_offset = align_up(sizeof(struct slab), align)`。
7. `objects_per_slab = min((PGSIZE - object_offset) / stride, SLAB_MAX_OBJECTS)`。
8. 若对象区越界或 `objects_per_slab == 0`，创建失败并释放描述符槽。

不要写死 pipe 的每页对象数；编译后的结构尺寸决定结果。

## 4. 必须实现的公开 API

在 `kernel/defs.h` 前置声明：

```c
struct kmem_cache;
```

并添加：

```c
// slab.c
void slabinit(void);
struct kmem_cache *kmem_cache_create(char *, uint, uint);
void *kmem_cache_alloc(struct kmem_cache *);
void kmem_cache_free(struct kmem_cache *, void *);
uint kmem_cache_shrink(struct kmem_cache *);
int kmem_cache_check(struct kmem_cache *);
void kmem_cache_dump(struct kmem_cache *);
```

在 pipe 部分增加：

```c
void pipeinit(void);
```

具体语义：

- `slabinit()`：初始化描述符全局锁和所有槽，只允许 boot CPU 在启动阶段调用。
- `kmem_cache_create()`：只建立描述符，不向 Buddy 申请页；失败返回 0。
- `kmem_cache_alloc()`：成功返回对象槽首地址，失败返回 0。
- `kmem_cache_free()`：非法参数或状态 panic。
- `kmem_cache_shrink()`：摘除并归还所有 `SLAB_FREE` 页，返回归还页数。
- `kmem_cache_check()`：获取一次 cache 锁，调用只在持锁下工作的内部检查函数。
- `kmem_cache_dump()`：在锁保护下打印布局、slab 数、对象数和统计。

## 5. 内部辅助函数

至少划分以下职责，函数名可以变化：

- `slab_list_add(list, slab, state)`
- `slab_list_del(list, slab, expected_state)`
- `slab_move(cache, slab, new_state)`
- `slab_grow_locked(cache)`
- `slab_object_index(cache, slab, obj, &index)`
- `slab_check_locked(cache)`
- bitmap 的 test/set/clear 和 popcount 辅助函数

规则：

- 带 `_locked` 的函数由调用者持有 `cache->lock`。
- 公开 `check()` 只获取一次锁，内部不得再次获取。
- 链表删除时清空 `next/prev` 并设为 `SLAB_NONE`，再插入新链。
- 所有链表计数必须与增删同步更新。

## 6. slab grow

`slab_grow_locked(cache)` 必须：

1. 在 cache 锁已持有时调用 `buddy_alloc(0)`。
2. 失败时增加 `nr_fail` 并返回 0。
3. 把整页先清零，再初始化 `struct slab`。
4. 写入 magic、cache、total、`SLAB_NONE` 和空位图。
5. 从对象区末端或起点依次串出页内 freelist；每个节点必须是合法槽首地址。
6. 把新 slab 加到 cache 的 `free` 链表。
7. 增加 `nr_grow`。

固定锁顺序是：

```text
cache->lock -> buddy.lock
```

Buddy 不得调用 Slab，因此不允许出现反向顺序。

## 7. 对象分配

`kmem_cache_alloc(cache)`：

1. 拒绝空 cache、未使用描述符或 Slab 未初始化状态。
2. 获取 cache 锁。
3. 选择 `partial.head`；没有则选择 `free.head`；仍没有则 grow。
4. 验证 slab magic、归属、freelist 非空、`inuse < total`。
5. 弹出 freelist 头。
6. 计算对象索引，验证对应 bit 目前为 0，再置 1。
7. `inuse++`、`live_objects++`、`nr_alloc++`。
8. 根据新值迁移到 `partial` 或 `full`。
9. 释放 cache 锁。
10. 用 `0x05` 填充整个 `stride`，返回对象。

不要在返回前自动清零；pipe 会初始化自己的有效状态字段。poison 用于暴露错误依赖。

## 8. 对象释放

`kmem_cache_free(cache, obj)`：

1. `cache == 0` 或 `obj == 0` 时 panic。
2. 先确认对象地址在 `[KERNBASE, PHYSTOP)`，再按页向下取整；不要对明显越界地址解引用页头。
3. 获取 cache 锁。
4. 检查页头 magic、所属 cache 和 slab state。
5. 通过 `object_offset/stride/total` 验证对象恰好是槽首地址。
6. 位图 bit 必须为 1，否则 panic，错误文本应能区分 double free。
7. 清 bit，`inuse--`、`live_objects--`、`nr_free++`。
8. 用 `0x01` 填满整个槽，再把 freelist next 写入槽首，避免 poison 覆盖链指针。
9. 按新占用量迁移：full 到 partial，partial 到 free。
10. 若 free 链表超过 1 页，摘除一张空页，清除其 magic，记下页地址并增加 `nr_reap`。
11. 释放 cache 锁。
12. 若有摘除页，在锁外调用 `buddy_free(page, 0)`。

不得在 `buddy_free()` 后再次读写该 slab 页头。

## 9. shrink 与检查器

### 9.1 shrink

`kmem_cache_shrink()` 必须循环摘除 cache 的所有 free slab，释放 cache 锁后逐页归还 Buddy。为避免使用动态临时数组，可以每次摘一页、解锁、归还、再加锁继续。

不得回收 partial 或 full slab。

### 9.2 检查器

`slab_check_locked(cache)` 必须至少验证：

- 三条链表均无环；遍历节点数不超过 Buddy 总页数上限。
- 每个节点的 magic、cache、state 与所在链匹配。
- slab 页地址页对齐。
- `total == cache->objects_per_slab`，`inuse <= total`。
- free 链 `inuse == 0`。
- partial 链 `0 < inuse < total`。
- full 链 `inuse == total` 且 freelist 为空。
- 位图中 1 的数量等于 `inuse`。
- freelist 无环、无重复，每个节点是合法对象槽且对应 bit 为 0。
- freelist 长度等于 `total - inuse`。
- 三条链汇总的在用对象数等于 `cache->live_objects`。
- 每条链实际节点数等于其记录的 count。

检查器只返回 1/0，不在遍历过程中修改结构。

## 10. 初始化接线

### 10.1 Makefile

在 `OBJS` 中紧跟 `kernel/kalloc.o` 加入：

```make
$K/slab.o \
```

### 10.2 main.c

在 `kinit()` 后立即调用：

```c
slabinit();
```

在 `fileinit()` 后、任何用户进程启动前调用：

```c
pipeinit();
```

### 10.3 pipe.c

增加：

```c
static struct kmem_cache *pipe_cache;
```

实现：

```c
void
pipeinit(void)
{
  pipe_cache = kmem_cache_create("pipe", sizeof(struct pipe), 0);
  if (pipe_cache == 0)
    panic("pipe cache");
}
```

替换：

- `pipealloc()` 中 `kalloc()` -> `kmem_cache_alloc(pipe_cache)`。
- `pipealloc()` 的 bad 路径 `kfree(pi)` -> `kmem_cache_free(pipe_cache, pi)`。
- `pipeclose()` 最终 `kfree(pi)` -> `kmem_cache_free(pipe_cache, pi)`。

不要改变 pipe 的锁释放位置、读写唤醒、计数器初始化和 file 字段设置。

## 11. 自测实现

用 `SLAB_DEBUG` 或 `SLAB_SELFTEST` 宏保护测试。生产构建不定义该宏。

建议在 `slabinit()` 完成后运行只使用局部测试 cache 的自测，不能依赖 pipe cache 已创建。每个测试结束都调用 `kmem_cache_shrink()`。

### T1：创建参数

- 合法尺寸和默认对齐创建成功。
- size 0、非法对齐、超大对象创建失败。
- 失败创建不能消耗永久描述符槽。

### T2：基本分配释放

- 从 64 字节 cache 分配 3 个对象。
- 验证非空、对齐、不相等。
- 写满每个对象。
- 释放后检查 live count 和结构一致性。

### T3：跨 slab 与状态迁移

- 获取 `objects_per_slab`。
- 分配 `objects_per_slab + 3` 个对象，强制两个 slab。
- 验证 full 和 partial 状态出现。
- 交错释放，覆盖 full->partial->free。

测试代码若不能直接访问内部状态，可放在 `slab.c` 内并调用内部检查辅助函数，但不得把内部结构暴露给其他生产文件。

### T4：多尺寸与对齐

- 测试 16、24、64、128、512、1024 字节 cache。
- 验证每个返回地址满足 cache align。
- 每种尺寸分配至跨页并写入边界字节。

### T5：确定性压力

- 使用固定种子的 xorshift。
- 至少 128 个记录槽、5000 次分配/释放操作。
- 活对象不能重叠。
- 每隔固定次数调用检查器。
- 最终释放所有对象并 shrink。

### T6：Buddy 页恢复

- 测试前记录 `buddy_free_pages()`。
- 让 cache 增长多个 slab。
- 释放全部对象并 shrink。
- 最终空闲页数必须等于测试前。
- 最终 `buddy_check()` 必须返回真。

### P1–P5：独立预期 panic

用 `SLAB_PANIC_CASE=N` 每次启动只运行一个：

1. double free。
2. wrong cache free。
3. interior pointer free。
4. non-slab Buddy page free via cache。
5. corrupted magic 后 free 或 check。

panic 前的合法分配必须成功；每个用例记录实际 panic 文本。

## 12. pipe 回归与压力

生产构建：

```bash
make clean && make && make fs.img
```

至少在 `CPUS=1` 和 `CPUS=3` 下测试。进入 xv6 shell：

```text
echo hello | cat
cat README | wc
forktest
stressfs
usertests -q
```

如果新增临时用户测试程序做 pipe 压力，完成后必须从生产 diff 移除，除非用户明确要求保留。

pipe 压力必须覆盖：

- 循环创建和关闭 pipe。
- 同时保持多个 pipe 存活，使 pipe cache 跨 slab。
- 父子进程分别关闭读写端。
- 读端提前关闭时写端正确失败，而不是 use-after-free。
- 最后 pipe cache 的 live object 数回到 0，检查器通过。

连续三次冷启动运行 `usertests -q`，每次都应看到 `ALL TESTS PASSED`，且不得出现 panic、hang 或 freelist/check 错误。

## 13. 编译与调试要求

自测构建示例：

```bash
make clean
make DETFLAGS="-ffile-prefix-map=$(pwd)=. -DSLAB_SELFTEST"
make fs.img
make qemu
```

预期 panic 示例：

```bash
make clean
make DETFLAGS="-ffile-prefix-map=$(pwd)=. -DSLAB_PANIC_CASE=1"
make fs.img
make qemu
```

若使用已有 Buddy 测试补丁，必须明确它是否已应用，测试结束后恢复生产源码。不要同时启用会互相消耗大量内存的 exhaust 测试，除非已确认顺序和基线预期。

遇到失败时优先记录：

- cache 名称、object size、stride、objects per slab。
- free/partial/full slab 数。
- live object 数和 alloc/free/grow/reap/fail 统计。
- Buddy free pages 以及 `buddy_check()` 结果。
- 失败前最后一次状态迁移。

## 14. 提交顺序

每个提交必须能构建。建议：

```text
mm: add slab cache descriptors and page layout
mm: implement slab allocation freeing and reclaim
mm: add slab consistency checks and debug selftests
ipc: allocate pipe objects from a slab cache
docs: record slab design and verification results
```

不要把所有改动压成一个无法 bisect 的提交。未经用户要求不要 push、开 PR 或合并回集成分支。

## 15. 最终验收清单

- [ ] `git diff --check` 无错误。
- [ ] 生产构建无 warning，且默认不运行 Slab 自测。
- [ ] Slab 只从 `buddy_alloc(0)` 获取页，只用 `buddy_free(page, 0)` 归还页。
- [ ] cache 描述符来自固定数组，无初始化递归。
- [ ] 页内对象布局对齐且不越过 PGSIZE。
- [ ] free/partial/full 状态迁移和链表计数正确。
- [ ] 位图、inuse、freelist 三者一致。
- [ ] double free、wrong cache、interior pointer 能被拒绝。
- [ ] 回收页在解锁后归还 Buddy，归还后不再访问。
- [ ] 自测跨 slab、乱序释放、压力和 shrink 全通过。
- [ ] 自测结束 Buddy 空闲页恢复，`buddy_check()` 为真。
- [ ] pipe 分配与所有释放路径都使用同一个 pipe cache。
- [ ] shell 管道、`forktest`、`stressfs` 正常。
- [ ] `usertests -q` 在 3 次冷启动、默认多核下全部通过。
- [ ] 文件系统、页表和用户 ABI 无改动。
- [ ] 最终报告列出修改文件、测试命令、结果、已知限制和后续建议。

## 16. 停止条件

出现以下情况不要猜测或扩大修改范围，应停止并报告：

- Buddy API 或状态语义与本文假设不一致。
- 当前工作区存在无法安全合并的用户修改。
- 必须修改文件系统、页表或用户 ABI 才能继续。
- 发现 pipe 生命周期存在无法在现有锁顺序下解决的竞态。
- 基线 Buddy 测试在任何 Slab 修改前就失败。
- 为通过测试需要隐藏 panic、跳过一致性检查或放宽非法释放验证。
