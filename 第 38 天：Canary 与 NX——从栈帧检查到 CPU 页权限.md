# 第 38 天：Canary 与 NX——从栈帧检查到 CPU 页权限

---

## 【📌 一句话核心概括】
**Canary 是由编译器在局部缓冲区与返回地址之间插入并在函数返回前校验的栈帧哨兵值（失配即调用 `__stack_chk_fail` 自爆），而 NX 则是由 CPU MMU 页表项第 63 位（XD/NX 位）从硬件总线层强制落实的“可写不可执行（$W \oplus X$）”内存页执行隔离。**

## 【📖 通俗原理解释】
- **Canary 哨兵的部署与校验机制**：函数序言中，编译器生成指令从线程控制块（TLS 架构寄存器段 `fs:0x28`）取出 8 字节随机值存放在 `[rbp - 0x8]` 处，作为缓冲区与 Saved RBP 之间的物理屏障；函数返回前比对该值与 `fs:0x28`，若线性连续溢出修改了该值，程序立即调用 `__stack_chk_fail()` 中断执行（`SIGABRT`），阻断 `ret` 指令被恶意劫持。
- **Canary 的保护盲区与绕过路径**：Canary 无法防范非线性内存跳跃写（如数组特定下标越界赋值）；此外 Canary 首字节在 Linux 上恒定为 `0x00`（用于阻断 `strcpy` 读取），若利用格式化字符串等泄露该值，或在多进程 `fork` 架构（子进程共享父进程相同 Canary）下通过逐字节爆破，仍可重构有效载荷并完整复原 Canary 绕过检测。
- **NX（No-Execute）的硬件页表防护**：早期的栈内存兼具可写与可执行属性（RWX）。现代 CPU 在 64 位页表项（PTE）的最高位（第 63 位）设置了 XD（Execute-Disable）位。操作系统将栈和堆对应的页表项此位置 1，MMU 硬件在尝试从该物理页取指译码时直接抛出权限异常（Page Fault），彻底终结了直接向栈注入并执行 Shellcode 的攻击手法。
- **NX 催生的代码复用演进**：NX 虽封锁了攻击者“自定义注入代码”，但无法阻止攻击者“复用程序与动态库中现有的合法可执行代码（如代码段中的 Gadgets 与 libc 函数）”，从而推动了后续 ROP 与 ret2libc 等现代代码复用攻击技术的诞生。

---

## 【🎯 为什么需要它与设计动机】

在漏洞攻防的发展史上，单纯靠程序员自觉检查数组长度是不现实的。操作系统与编译器团队从系统架构层面推出了这两项工业级防护：

1. **解决连续线性溢出的普适痛点（Stack Canary）**
   绝大多数栈溢出都是通过 `strcpy`、`read`、`gets` 从低地址向高地址进行线性连续覆盖。Canary 就像一道精准横亘在“局部变量区”与“控制数据区（RBP/RIP）”之间的物理绊线。只要你顺序向前溢出，就必定踩爆 Canary。
2. **根除“注入代码并执行”的时代（NX / DEP）**
   在早期的“黄金黑客时代”，栈内存同时具备“可写”与“可执行”权限（RWX）。攻击者只需将 Shellcode 写入栈并跳转即可通关。NX（No-Execute）推行了计算机体系结构的至高法则：**$W \oplus X$（写执行互斥：能写的区域绝不能执行，能执行的区域绝不能写）**，从物理上剥夺了数据页的代码执行权。

---

## 【🔬 核心概念深度剖析】

### 1. 四大经典防御体系的纵深对比

很多初学者容易将这四者混淆，我们通过全景表格划清权限与层次边界：

| 防护技术 | 英文全称 | 裁决与运行层次 | 保护目标与核心动作 | 被绕过的典型场景 |
| :--- | :--- | :--- | :--- | :--- |
| **Canary** | Stack Protector | **编译器插入的汇编代码** | 保护函数返回地址不被顺序覆盖；函数退出前比对，异常则 `abort`。 | 非线性任意写（跳跃写）、未初始化内存泄漏、Fork 进程字节爆破。 |
| **NX / DEP** | No-Execute / Data Execution Prevention | **CPU 硬件 MMU + 内核页表** | 栈/堆页标记为不可执行（RW-）；阻止在数据内存中取指执行。 | **代码复用技术（ROP / ret2libc / ret2win）**。 |
| **ASLR** | Address Space Layout Randomization | **Linux 内核加载器** | 随机化栈、堆、动态链接库的虚拟映射基址。 | 地址信息泄露（Infoleak）反推 Base、部分字节覆盖（Partial Overwrite）。 |
| **PIE** | Position-Independent Executable | **编译器与链接器** | 将主程序编译为共享对象格式，让主程序代码段也参与地址随机化。 | 地址信息泄露泄露主程序函数地址。 |

---

### 2. Stack Canary 底层实现机制深度剖析

#### ① 哨兵从哪里来？（TLS 与 `fs:0x28`）
- 在 Linux x86-64 环境下，每个线程都有一个**线程控制块（TCB / TLS）**。
- 内核与动态链接器在初始化线程时，通过硬件段寄存器 `FS` 指向该结构。
- 随机 Canary 母版就存放在 **`%fs:0x28`** 的 8 字节内存中（由系统强随机数发生器生成）。

#### ② Canary 的字节构成：末位永远为 `\x00`
一个典型的 64 位 Canary 十六进制值类似于：
```text
0x5b3a8c19e421fd00
                  ^^
                  最低有效字节 (Lowest Byte) 永远是 0x00！
```
> **精妙设计**：为什么末位必须是 `0x00`？
> 因为 C 语言中最臭名昭著的溢出函数（如 `strcpy`、`strcat`、`sprintf`）都是以 `\x00`（ASCII NUL）作为字符串结束符！
> 攻击者如果试图通过字符串函数将 Canary 泄露出来，`printf("%s", buf)` 读到末尾的 `0x00` 就会立刻停下，**天然防止哨兵值被字符串函数打印泄露**！

#### ③ 编译器的进出桩汇编（Prologue & Epilogue）

```asm
【函数入口 Prologue】
    mov    rax, QWORD PTR fs:0x28     ; 1. 从 TLS 安全密室取出 Canary 母版
    mov    QWORD PTR [rbp-0x8], rax   ; 2. 秘密埋设在 [rbp-0x8] 处 (紧挨着 Saved RBP)
    xor    eax, eax                   ; 3. 硬件清空 RAX 寄存器，销毁残留痕迹

【函数退出 Epilogue】
    mov    rax, QWORD PTR [rbp-0x8]   ; 1. 从栈上取出 Canary 副本
    sub    rax, QWORD PTR fs:0x28     ; 2. 与 TLS 母版做减法比较 (xor rax, fs:0x28 也可)
    jne    .Lstack_chk_fail           ; 3. 不为 0 说明被篡改！直接跳向绝壁！
    leave                             ; 4. 校验通过，正常平栈
    ret                               ; 5. 放心返回调用方

.Lstack_chk_fail:
    call   __stack_chk_fail           ; 打印 "*** stack smashing detected ***" 并强制 abort() 崩溃
```

---

### 3. CPU 硬件级 NX：MMU 页表项第 63 位（XD/NX Bit）

NX 不是操作系统的软件循环判断，而是**物理 CPU 芯片在取指周期完成的硬件门禁**：

```text
64 位虚拟地址转换页表项 (PTE, Page Table Entry):
[63] [62 ................................ 12] [11 ......... 0]
 |                   物理页框基地址                权限属性标志位
 +---> XD / NX 位 (Execute-Disable Bit)
       - 值为 0: 允许 CPU 在该物理内存页中抓取机器指令译码并执行
       - 值为 1: 严禁取指！若 CPU 的 RIP 指向该页，MMU 立即向 CPU 引脚触发硬件中断 #PF (Page Fault)
```

当 Linux 启动时，内核通过 `sys_brk` 分配堆、通过 ELF 程序头为栈分配空间时，统一将页表项的第 63 位设为 `1`（不可执行）。
一旦栈溢出攻击者将 `RIP` 强行导向栈中的 Shellcode 地址，CPU 在取指的瞬间硬件报警，内核直接发送 `SIGSEGV`（信号代码 `SEGV_ACCERR`，权限违规）瞬间斩杀进程！

---

### 4. Canary 的三种经典绕过姿势

1. **逐字节泄露并原样填回（Canary Leak）**
   - 如果存在 `read()` 刚好能覆盖到 Canary 的最低字节（`\x00`），将其改写为非零字符（如 `A`）；
   - 随后的 `puts(buf)` 或 `printf("%s", buf)` 就会因为找不到 `\x00` 截断符，一路向下打印，将 Canary 剩余的 7 个高位随机字节完整吐出！
   - 攻击者在后续真正的溢出 Payload 中，把泄露出的 Canary 原样放回 `[rbp-8]` 位置，校验直接绿灯放行！
2. **多进程 Fork 爆破（Fork-Server Brute-force）**
   - 诸如 Nginx、Apache、部分 CTF 靶机采用 `fork()` 创建子进程处理网络请求。
   - `fork()` 采用写时复制（COW），**子进程继承父进程一模一样的内存空间与 TLS Canary**！
   - 子进程崩溃不会导致父进程死亡。攻击者可以对 Canary 剩余的 7 个字节逐字节尝试（每个字节从 0x00 试到 0xFF，最多试 256 次）。原本 $2^{56}$ 的恐怖爆破量被降维打击为仅需 $7 \times 256 = 1792$ 次！
3. **任意地址写跳过（Arbitrary Write / Index Bypass）**
   - 若漏洞是数组下标越界（如 `array[index] = value`），攻击者可以直接将 `index` 算到返回地址的位置，直接改写 `Saved RIP`，**根本不碰中间的 Canary**，校验自然永远发现不了！

---

## 【🛠️ 工业实操与反汇编/调试验证】

### 1. 验证 Canary 触发过程的源码（`canary_demo.c`）

```c
#include <stdio.h>
#include <string.h>

void vulnerable_function(void) {
    char buf[16];
    printf("[*] Enter text: ");
    // 允许输入 64 字节，必然击穿 16 字节 buf 并破坏 Canary
    gets(buf); 
    printf("[+] You entered: %s\n", buf);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    vulnerable_function();
    puts("[+] Process completed normally.");
    return 0;
}
```

### 2. 编译并用 checksec 检视防御状态

```bash
# 默认开启 Canary 和 NX
gcc -O0 canary_demo.c -o canary_demo
checksec --file=canary_demo
```
终端输出验证：
```text
    Arch:     amd64-64-little
    RELRO:    Partial RELRO
    Stack:    Canary found    <-- Canary 开启
    NX:       NX enabled      <-- NX 开启
    PIE:      PIE enabled
```

### 3. 反汇编观察 Canary 埋设与校验

```bash
objdump -d -M intel canary_demo | grep -A 25 "<vulnerable_function>:"
```
关键汇编片段：
```text
0000000000001169 <vulnerable_function>:
  1169: 55                    push   rbp
  116a: 48 89 e5              mov    rbp,rsp
  116d: 48 83 ec 20           sub    rsp,0x20
  1171: 64 48 8b 04 25 28 00  mov    rax,QWORD PTR fs:0x28  <-- 从 fs:0x28 提取 Canary
  1178: 00 00 
  117a: 48 89 45 f8           mov    QWORD PTR [rbp-0x8],rax <-- 埋在 [rbp-0x8]
  117e: 31 c0                 xor    eax,eax                <-- 清空 rax 痕迹
  ...
  11a9: 48 8b 45 f8           mov    rax,QWORD PTR [rbp-0x8] <-- 取出栈上 Canary
  11ad: 64 48 2b 04 25 28 00  sub    rax,QWORD PTR fs:0x28  <-- 与母版核验
  11b4: 00 00 
  11b6: 74 05                 je     11bd <vulnerable_function+0x54>
  11b8: e8 a3 fe ff ff        call   1060 <__stack_chk_fail@plt> <-- 失败自爆
  11bd: c9                    leave
  11be: c3                    ret
```

### 4. 运行崩溃验证

```bash
./canary_demo
[*] Enter text: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
[+] You entered: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
*** stack smashing detected ***: terminated
Aborted (core dumped)
```
系统准确拦截了攻击，打印了著名的 `stack smashing detected`，进程在 `ret` 发生前被强制枪决！

---

## 【⚠️ 常见陷阱与避坑指南】

| 陷阱与盲区 | 深度机理与表象 | 正确解法与工程底线 |
| :--- | :--- | :--- |
| **误以为 Canary 越界即死** | 以为输入超出 buffer 瞬间系统就会中断。实际上 Canary 只在函数执行到最后退出时才比较，函数体内部执行期间恶意数据已在生效。 | 深刻理解“检查时差”：局部变量覆盖攻击（如改写同栈帧内的函数指针或标志位）在 Canary 检查前就已经生效。 |
| **混淆 NX 与 ASLR** | 认为“开启了 NX 程序地址就会变”。NX 掌管的是**内存页的执行权限（W^X）**，ASLR 掌管的是**加载地址的随机化**，二者互不隶属。 | 使用 `checksec` 独立分析各项指标，不要张冠李戴。 |
| **多线程环境下的 Canary 特性** | 在多线程程序中，主线程和子线程的 Canary 是否相同？glibc 中每个线程有独立的 TCB，**不同线程的 Canary 母版值不同**。 | 在攻击多线程目标时，不能简单用主线程泄露的 Canary 去套用子线程。 |

---

## 【💼 经典面试与工程实操自测】

### Q1：为什么 64 位 Linux 系统的 Stack Canary 值的最低位字节（LSB）必须是 `\x00`？
> **参考答案**：
> - 栈溢出中最普遍的漏洞输入函数是 C 标准库中的字符串操作函数（如 `strcpy`、`strcat`、`sprintf`、`gets`）。这些函数无一例外以 `\x00`（ASCII NUL）作为字符串的终结符。
> - 将 Canary 的最低有效字节设计为 `\x00`，具有双重拦截效果：
>   1. **防写入**：使用 `strcpy` 试图溢出时，遇到 `\x00` 就会强行停止，无法继续向后写入覆盖更高位的返回地址；
>   2. **防泄露**：若程序员使用 `printf("%s", buf)` 打印临近缓冲区时，输出会在 Canary 最前面的 `\x00` 处自动截断，保护了后方 7 字节随机值不被泄露。

### Q2：现代操作系统推行的 W^X（Write XOR Execute）原则具体含义是什么？它是由软件实现的还是硬件实现的？
> **参考答案**：
> - **原则含义**：任何一个虚拟内存页面，在同一时刻**绝对不能同时拥有“可写（Write）”和“可执行（Execute）”权限**。数据区（栈、堆、全局数据）只能是 RW-；代码区（.text）只能是 R-X。
> - **实现机制**：由**硬件与操作系统内核协同实现**。CPU 硬件的 MMU（内存管理单元）在 64 位页表项（PTE）的最高位（第 63 位）提供了 XD/NX（Execute-Disable）位。内核在构建进程虚拟地址空间时，通过页表将数据页的 XD/NX 位置 1。当 CPU 尝试从该页取指时，MMU 硬件抛出页错误异常，内核直接终止进程。

### Q3：如果一个二进制程序同时开启了 Canary、NX、PIE、ASLR（全防护），在存在栈溢出的情况下，经典攻击链的步骤是什么？
> **参考答案**：
> 1. **第一阶段：信息泄露（Infoleak）**
>    - 借助未初始化的栈读或格式化字符串漏洞，泄露 Canary 随机值；
>    - 泄露栈上保存的函数返回地址，结合符号虚拟地址（RVA）计算出主程序基址（PIE Base）或标准 C 库基址（libc Base）。
> 2. **第二阶段：控制流构造（ROP 链搭建）**
>    - 利用泄露的基址，在内存现有的代码段中搜寻所需的汇编片段（Gadgets）；
>    - 构造 Payload：`垃圾填充 + 泄露的真实 Canary + 伪造的 RBP + ROP 链（如 pop rdi; ret -> "/bin/sh" -> system）`。
> 3. **第三阶段：触发劫持**
>    - Canary 原样填回通过 epilogue 核验，函数正常执行 `ret` 移交控制流，进入 ROP 链执行提权。