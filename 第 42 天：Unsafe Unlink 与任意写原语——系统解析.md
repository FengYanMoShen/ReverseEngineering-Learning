# 第 42 天：Unsafe Unlink 与任意写原语——从链表不变量到内存写原语

---

## 【📌 一句话核心概括】
**Unsafe Unlink 利用堆溢出篡改双向链表空闲块的 `fd` 与 `bk` 指针，借由堆管理器在空闲块脱钩合并时执行的指针重接操作（`FD->bk = BK; BK->fd = FD`），将原本局部的堆越界放大为向任意目标地址写入受控指针的任意地址写原语（AAW）。**

## 【📖 通俗原理解释】
- **双向链表脱钩的机械本质**：ptmalloc 为在 $O(1)$ 时间内合并物理相邻的空闲堆块，在 Small Bin 和 Unsorted Bin 中维护双向循环链表。节点 `P` 脱钩时执行两步指针赋值：`FD = P->fd; BK = P->bk; FD->bk = BK; BK->fd = FD`。
- **经典无保护 Unlink 的任意写原语**：早期分配器无任何校验，若攻击者伪造 `P->fd = Target - 24` 且 `P->bk = Value`，脱钩第一步 `FD->bk = BK` 即等价于向 `*(Target) = Value` 实施任意写入；但第二步 `BK->fd = FD` 会强制将目标地址写回 `Value` 所在的偏移处，要求该写入区域必须具备可写权限，否则触发 `SIGSEGV`。
- **现代化双向指针完整性检查（Corrupted Double-Linked List）**：现代 glibc 引入严格的前后向互反校验宏：强制断言 `P->fd->bk == P && P->bk->fd == P`。若任一等式失配，分配器立即触发 `abort` 崩溃自毁。
- **现代安全绕过与全局指针折叠**：为突破完整性检查，攻击者必须找到一个已知保存了指向堆块 `P` 地址的内存位置（如全局指针数组 `chunk_ptr`）。通过令 `P->fd = &chunk_ptr - 24` 且 `P->bk = &chunk_ptr - 16`，精准满足验证条件，脱钩执行完毕后将使 `chunk_ptr` 指针自身折叠收缩为 `&chunk_ptr - 24`，从而将堆写入权限彻底提升为修改自身全局指针的完全任意地址读写原语。

---

## 【🎯 为什么需要它与设计动机】

1. **从“局部数据损坏”到“通用原语（Exploit Primitive）”的质变**
   在初级黑客眼中，越界写只是“破坏了相邻变量”。但在系统级安全研究者眼里，安全的核心是**“原语构建”**：如何把一个微小的越界能力，通过数据结构算法的机械齿轮，逐级放大为**任意地址读（AAR）**或**任意地址写（AAW）**。
2. **洞悉堆分配器底层的链表不变量（Invariants）**
   分配器为了在 $O(1)$ 时间复杂度内合并前后空闲块，在每个空闲 Chunk 中维护了双向循环链表。这种机制天然假定了“内存中的元数据是可信的”。Unlink 攻击以最直观的方式揭示了底层并发与性能优化在面对恶意输入时的内在脆弱性。
3. **安全攻防升级的里程碑标本**
   glibc 堆保护的发展史，本质上就是围绕 Unlink 的攻防拉锯史。从最初无任何校验的经典 Unsafe Unlink，到引入 `corrupted double-linked list` 双向完整性校验，再到现代 Safe-Linking 异或加密，彻底理解 Unlink 是掌握整个堆安全演化逻辑的核心钥匙。

---

## 【🔬 核心概念深度剖析】

### 1. 双向链表不变量与 Unlink 的代数展开

在双向循环链表中，任意合法节点 `P` 必须严格满足**双向对齐不变量**：
$$\text{P->fd->bk} == \text{P} \quad \land \quad \text{P->bk->fd} == \text{P}$$

```text
       前驱节点 (BK)              当前节点 (P)              后继节点 (FD)
     +---------------+          +---------------+          +---------------+
     | ...           |          | ...           |          | ...           |
     | fd  --------->|--------->| fd  --------->|--------->| fd            |
     | bk            |<---------|<-- bk         |<---------|<-- bk         |
     +---------------+          +---------------+          +---------------+
```

当分配器需要从空闲链表中摘除 `P`（例如执行合并或切片）时，执行经典的 **Unlink 宏指令**：

```c
#define unlink(P, BK, FD) { \
    FD = P->fd;             \
    BK = P->bk;             \
    FD->bk = BK;            \ // 写入 1
    BK->fd = FD;            \ // 写入 2
}
```

#### 代数拆解：Unlink 到底往哪里写了什么？
设在 64 位系统下：
- `offsetof(struct malloc_chunk, fd) = 16 (0x10)`
- `offsetof(struct malloc_chunk, bk) = 24 (0x18)`

当攻击者通过堆溢出伪造了 `P->fd` 和 `P->bk` 的值：
- **写入 1**：`FD->bk = BK`
  - 目标地址：`P->fd + 0x18`
  - 写入内容：`P->bk`（8 字节指针）
- **写入 2**：`BK->fd = FD`
  - 目标地址：`P->bk + 0x10`
  - 写入内容：`P->fd`（8 字节指针）

---

### 2. 所谓“任意写”的四维严密审视

很多初学者误以为 Unlink 可以“向任意地址写任意数值”，但严格的安全原语必须回答以下四问：

| 评估维度 | 经典 Unsafe Unlink 的真实边界 |
| :--- | :--- |
| **Where（地址）** | 写入地址**受到指针偏移约束**（目标地址必须能够被加减 `0x18` 或 `0x10`）。 |
| **What（内容）** | 写入的值**只能是另一个地址（指针宽度）**，无法直接写入任意自定义字符串或单字节。 |
| **Width（宽度）** | 硬件指针定长（64位系统下恒为 8 字节，32位系统下恒为 4 字节）。 |
| **Side Effects（副作用）** | **必须承受致命的“双向连带写入”！** 写入 2 会向 `BK + 0x10` 写入 `FD`。如果目标地址所在的内存页不可写（如只读代码段），CPU 立即在写入 2 处触发段错误崩溃！ |

---

### 3. 现代 glibc 的防线：双向握手完整性校验

为了彻底根除盲目的 Unsafe Unlink，现代 glibc（自 glibc 2.3.4 起）在 `unlink` 宏中强行加入了**物理一致性双向检查**：

```c
/* glibc 源码中的经典防御校验 */
if (__builtin_expect (FD->bk != P || BK->fd != P, 0))
    malloc_printerr ("corrupted double-linked list");
```

#### 防御机理：
- 调度员在修改前，先顺着 `P->fd` 找到后车，问一句：“你的 `bk` 是不是指着 P？”
- 再顺着 `P->bk` 找到前车，问一句：“你的 `fd` 是不是指着 P？”
- 只要攻击者随便伪造了一个非法地址，对方根本不可能刚好“反向指向 P”，校验当场爆雷，进程被强制终止。

---

### 4. 现代绕过艺术：巧用指针数组构造伪闭环（Modern Unlink）

在现代 CTF 与实战中，若要在开启双向检查的环境下利用 Unlink，必须满足一个苛刻前提：**程序在全局变量（或可预测的栈上）保存了指向堆块 P 的指针数组**！

假设全局数据段存在一个指针数组：`chunk_ptr_table[0] = P`（其内存地址已知为 `&chunk_ptr_table[0]`）：

```text
攻击者伪造 fake_chunk:
1. 设置 fake_chunk->fd = &chunk_ptr_table[0] - 0x18
2. 设置 fake_chunk->bk = &chunk_ptr_table[0] - 0x10

【检验阶段】
- 检查 FD->bk == P:
  FD = &chunk_ptr_table[0] - 0x18
  FD->bk 位于 FD + 0x18，即 (&chunk_ptr_table[0] - 0x18) + 0x18 = &chunk_ptr_table[0]
  *(&chunk_ptr_table[0]) 正好就是 P！校验通过！
- 检查 BK->fd == P:
  BK = &chunk_ptr_table[0] - 0x10
  BK->fd 位于 BK + 0x10，即 (&chunk_ptr_table[0] - 0x10) + 0x10 = &chunk_ptr_table[0]
  *(&chunk_ptr_table[0]) 同样是 P！校验通过！

【执行阶段结果】
FD->bk = BK  ===>  chunk_ptr_table[0] = &chunk_ptr_table[0] - 0x18 !
```
**神级质变**：全局指针 `chunk_ptr_table[0]` 被篡改成了**指向它自己附近内存的指针**！后续通过对 `chunk_ptr_table[0]` 的常规编辑，攻击者就能任意改写全局指针数组中的任意地址，从而彻底化身为真正的**无限制任意地址读写（AAR / AAW）**！

---

## 【🛠️ 工业实操与反汇编/调试验证】

我们编写一个教学级精简双向链表分配器，完整重现 Unsafe Unlink 的写入原语发生全过程。

### 1. 模拟链表与漏洞代码（`unlink_core.c`）

```c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

typedef struct Node {
    uint64_t prev_size;
    uint64_t size;
    struct Node *fd;
    struct Node *bk;
} Node;

// 经典无校验 Unlink 宏
void unlink_classic(Node *P) {
    Node *FD = P->fd;
    Node *BK = P->bk;
    printf("[*] Performing Unlink on %p...\n", P);
    printf("    [Write 1] Writing BK(%p) into FD->bk(%p)\n", BK, &FD->bk);
    FD->bk = BK;
    printf("    [Write 2] Writing FD(%p) into BK->fd(%p)\n", FD, &BK->fd);
    BK->fd = FD;
}

// 模拟目标受害者变量 (模拟 GOT 表或函数指针)
uint64_t target_function_ptr = 0x401000;
// 模拟可写的临时跳板内存 (防止 Write 2 引发不可写崩溃)
uint64_t write_buffer[4] = {0};

int main(void) {
    printf("[*] Address of target_function_ptr: %p (Current value: 0x%lx)\n", 
           &target_function_ptr, target_function_ptr);

    // 申请一块内存作为恶意伪造的 Node P
    Node *P = (Node *)malloc(sizeof(Node));

    // 攻击目标：想要把 target_function_ptr 改为 0xdeadbeef
    // 分析公式：
    // Write 1 发生于: FD + offsetof(bk) = target_function_ptr
    // 因此: FD = &target_function_ptr - offsetof(Node, bk)
    P->fd = (Node *)((uintptr_t)&target_function_ptr - offsetof(Node, bk));

    // Write 2 发生于: BK + offsetof(fd)
    // 为了防止崩溃，BK 必须指向可写内存:
    P->bk = (Node *)((uintptr_t)&write_buffer[0] - offsetof(Node, fd));

    printf("[*] Forged P->fd = %p\n", P->fd);
    printf("[*] Forged P->bk = %p\n", P->bk);

    // 触发 Unlink
    unlink_classic(P);

    printf("\n[+] Result verification:\n");
    printf("[+] target_function_ptr is now: %p (Successfully overwritten!)\n", 
           (void *)target_function_ptr);

    free(P);
    return 0;
}
```

### 2. 编译并运行验证

```bash
gcc -O0 -g unlink_core.c -o unlink_core
./unlink_core
```

终端执行回显：
```text
[*] Address of target_function_ptr: 0x555555558040 (Current value: 0x401000)
[*] Forged P->fd = 0x555555558028
[*] Forged P->bk = 0x555555558050
[*] Performing Unlink on 0x5555555592a0...
    [Write 1] Writing BK(0x555555558050) into FD->bk(0x555555558040)
    [Write 2] Writing FD(0x555555558028) into BK->fd(0x555555558060)

[+] Result verification:
[+] target_function_ptr is now: 0x555555558050 (Successfully overwritten!)
```
**实验实证**：`target_function_ptr` 的值原本是 `0x401000`，在执行完 `unlink_classic` 后被精准替换！数据结构内部原生的维护逻辑，被完全转化为武器化的内存写原语！

---

## 【⚠️ 常见陷阱与避坑指南】

| 陷阱与盲区 | 深度机理与表象 | 正确解法与工程底线 |
| :--- | :--- | :--- |
| **忽视 Write 2 导致崩溃** | 只关注把想要的数据写进了目标变量，却把 `P->bk` 设为一个非法或只读地址，程序在执行第二行 `BK->fd = FD` 时因非法内存写入当场暴毙。 | 构造利用链时必须**瞻前顾后**：确保两个写入地址所在内存页均具备合法可写（`W`）权限。 |
| **混淆无校验 Unlink 与现代环境** | 在 glibc 2.35+ 上生搬硬套十几年前的经典 Unsafe Unlink 脚本，被 `corrupted double-linked list` 拦截后不知所措。 | 明确区分系统环境版本。现代版本优先依赖指针数组自闭合技巧构造指针重定向，或采用 Fastbin/Tcache Poisoning 等单链表攻击。 |
| **把 Unlink 当作独立利用** | 单纯的 Unlink 往往只改写了一次内存。若不能把改写的指针与后续程序逻辑（如二次输入、数组寻址）结合，就无法达成代码执行。 | 将 Unlink 视作**跳板原语**，配合全局数组实现任意地址改写，最终接管 GOT 表或返回地址。 |

---

## 【💼 经典面试与工程实操自测】

### Q1：经典 Unsafe Unlink 的核心代码 `FD->bk = BK; BK->fd = FD;` 会引发哪两次具体的内存写入？为什么说它是受约束的？
> **参考答案**：
> 1. **两次写入细节**：
>    - 写入 1：向地址 `P->fd + offsetof(Node, bk)` 写入数值 `P->bk`；
>    - 写入 2：向地址 `P->bk + offsetof(Node, fd)` 写入数值 `P->fd`。
> 2. **受约束表现**：
>    - **地址与值互相绑定**：写入的值必然是另一个写入地址的前驱/后继，无法将完全无关的立即数（如任意 64 位整数）写入指定内存；
>    - **必须成对写入**：两次写相伴相生，任何一个目标地址不可写都会导致崩溃；
>    - **宽度固定**：写入宽度被系统硬件指针宽度死死限制（64 位系统下为 8 字节）。

### Q2：现代 glibc 是如何通过增加检查阻止经典 Unsafe Unlink 的？攻击者又是如何利用全局指针数组实现绕过的？
> **参考答案**：
> - **防御检查**：glibc 在摘链前强制执行双向指针校验：`if (FD->bk != P || BK->fd != P) abort();`。攻击者随意伪造的地址由于无法自证指向 `P`，直接触发断言自爆。
> - **绕过技巧**：
>   如果程序在全局内存（BSS 段）中保留了一个指向 `P` 的指针（如 `array[0] = P`，地址为 `&array[0]`），攻击者可将伪造 Chunk 的 `fd` 设为 `&array[0] - 0x18`，将 `bk` 设为 `&array[0] - 0x10`。
>   - 此时 `FD->bk` 寻址到 `&array[0]`，其内容正是 `P`；`BK->fd` 同样寻址到 `&array[0]`，内容也是 `P`。完全通过双向校验！
>   - 摘链完成后，`array[0]` 的值被重写为 `&array[0] - 0x18`，使得全局指针数组实现自我折叠，获得任意内存修改能力。

### Q3：为什么现代系统在堆分配器中越来越倾向于单链表设计（如 Tcache/Fastbin），这对安全带来了哪些连锁影响？
> **参考答案**：
> - **性能驱动**：单链表（Singly Linked List）只需维护单一的 `next/fd` 指针，入链和出链只需一次指针赋值（`P->fd = Head; Head = P`），不仅节约堆块元数据开销，而且性能极高、易于实现每线程无锁缓存。
> - **安全影响**：
>   - 单链表不需要执行复杂的双向 Unlink 过程，因此规避了经典双向链表摘链被利用的攻击面；
>   - 但是单链表由于缺少反向回查机制，天然容易受到 **Double Free（成环死循环）** 和 **Tcache Poisoning（单指针覆盖伪造任意堆块）** 攻击，逼迫后续 glibc 引入了 `tcache_entry.key` 校验和 Safe-Linking 异或混淆等专用补丁。
