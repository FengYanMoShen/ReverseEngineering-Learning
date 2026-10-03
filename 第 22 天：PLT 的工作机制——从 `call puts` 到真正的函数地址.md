# 第 22 天：PLT 的工作机制——从 `call puts` 到真正的函数地址

---

## 【📌 一句话核心概括】
**过程链接表（PLT）位于只读代码段，通过间接跳转指令解引用可读写数据段中的全局偏移表项（`GOT[n]`），在首次调用时触发动态链接器（`_dl_runtime_resolve`）将外部共享库函数的真实内存地址回写覆盖到 GOT 表中，实现只读代码共享与符号运行期延迟绑定（Lazy Binding）。**

## 【📖 通俗原理解释】
- **为什么代码段必须与真实地址解耦**：共享库（如 `libc.so`）因 ASLR 和多进程映射，其装载基址每次运行各不相同。若直接在代码段（`.text`）硬编码函数物理地址，代码段就必须在加载时被写修改，这不仅破坏多进程对物理代码页的 COW 共享，更违背了现代系统的 W^X（可写即不可执行）安全原则。
- **PLT（只读跳板）与 GOT（可写地址表）的分工**：`PLT` 存放在只读代码段（`r-x`），提供固定入口（如 `call puts@plt`）；`GOT` 存放在可读写数据段（`rw-`），存储外部符号的实际函数指针。代码段只做相对寻址间接跳转，可变的绝对地址全部收敛在 GOT 数据表中修改。
- **首次调用流程（未解析回环与解析器介入）**：首次调用时，`GOT[puts]` 中预填的并非真实函数地址，而是该 PLT 项内部的下一条指令地址（`push reloc_arg`）。程序跳入 GOT 后立即弹回 PLT 继续压入重定位编号，随后跳转到公共的 `PLT0`，由 `PLT0` 调用动态链接器的解析函数 `_dl_runtime_resolve`，算出 `puts` 在 libc 中的实际加载地址，并当场覆盖填入 `GOT[puts]`。
- **后续调用流程（单指令直达）**：第二次及之后再调用 `puts@plt` 时，首条指令 `jmp [GOT[puts]]` 读取到的已是 libc 中 `puts` 的真实物理地址，一跳直达目标函数，彻底消除后续解析开销。

---

```
PLT / GOT 延迟绑定（Lazy Binding）全生命周期拓扑：

[ 代码段: 只读可执行 R-X ]                      [ 数据段: 可读写 RW- ]
  main() 函数                                    .got.plt 数据表
┌─────────────────────────┐                   ┌─────────────────────────────┐
│ 40112f: call puts@plt   │ ──┐               │ 403fe8: GOT[0] (.dynamic)   │
└─────────────────────────┘   │               ├─────────────────────────────┤
                              ▼               │ 403ff0: GOT[1] (link_map)   │
  puts@plt (专属前台)                         ├─────────────────────────────┤
┌─────────────────────────┐                   │ 403ff8: GOT[2] (dl_resolve) │
│ 401030: jmp [GOT[puts]] │ ──┼──────────────>├─────────────────────────────┤
│ 401036: push 0 (重定位号)│ <─┤ (首次未解析)   │ 404000: GOT[puts]           │
│ 40103b: jmp PLT0        │ ─┐│               │  初始: 401036 (指回前台)     │
└─────────────────────────┘  ││               │  解析后: 0x7ffff7e1... (P)   │
                             ││               └──────────────┬──────────────┘
  PLT0 (公共大堂)             ││                              │ (后续调用直接直达)
┌─────────────────────────┐  ││                              │
│ 401020: push [GOT[1]]   │ <┘│                              ▼
│ 401026: jmp  [GOT[2]]   │ ──┼───────────────────> libc.so.6: puts()
└─────────────────────────┘   │                     真实执行输出逻辑
                              ▼
                     动态链接器 ld.so
               _dl_runtime_resolve(link_map, 0)
                 1. 查找 libc 中 puts 真实地址 P
                 2. 回写 GOT[puts] = P
                 3. 修复栈并 jump 到 P
```

---

## 一、为什么必须引入 PLT/GOT 中间层？

在系统安全与底层架构设计中，PLT/GOT 的诞生是为了平衡三大工程矛盾：

1. **位置无关性与代码共享（Position Independence）**：
   Linux 允许多个进程共享同一份动态库的物理内存代码页（Page Cache）。如果把 `puts` 的绝对地址硬编码进代码指令，由于不同进程中共享库映射基址不同（ASLR），代码段就必须被修改，导致只读代码页发生写时复制（COW），破坏多进程内存共享。
2. **装载耗时与懒绑定（Lazy Binding）**：
   一个大型大型程序（如大型 GUI 或浏览器）往往依赖数百个共享库、数万个外部符号。如果程序在启动的一瞬间必须把这数万个符号的绝对地址全部解析完毕，启动时间将从毫秒级恶化为秒级。**懒绑定原则：只有被实际调用到的函数，才在首次运行时付出解析代价**。
3. **写保护与代码段安全性（W^X 内存法则）**：
   现代操作系统强制执行 $W \oplus X$（可写与可执行互斥）。代码段（`.text`、`.plt`）必须是只读可执行（`R-X`），绝对不能被动态覆写；而重定位所需要修改的地址槽必须集中收拢到专门的可读写数据段（`.got`、`.got.plt`，权限为 `RW-`）。

---

## 二、标准实验样本：可观测经典 PLT 的编译参数

为了看清未经现代安全机制包装的经典 PLT 结构，我们编写最简验证代码：

```c
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    if (puts("first") == EOF) {
        return EXIT_FAILURE;
    }
    if (puts("second") == EOF) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
```

使用以下严密控制参数编译（避免 CET 分离式 PLT 与全局立即绑定）：

```bash
gcc -std=c17 -Wall -Wextra -Wpedantic \
    -O0 -g \
    -fno-pie -no-pie \
    -fcf-protection=none \
    -fno-builtin-puts \
    -Wl,-z,lazy \
    plt_demo.c -o plt_demo
```

* `-no-pie`：固定可执行文件加载基址（代码段基址位于 `0x400000` 附近），便于固定反汇编地址推演；
* `-fcf-protection=none`：关闭 Intel CET 间接分支跟踪，避免生成分离式 `.plt.sec`；
* `-fno-builtin-puts`：严禁编译器将 `puts` 当作内部内联优化处理；
* `-Wl,-z,lazy`：显式开启延迟绑定。

---

## 三、反汇编与底层微操作逐条推演

### 3.1 `main` 函数中的调用点分析

使用 `objdump -d -M intel --disassemble=main plt_demo` 查看：

```asm
0000000000401126 <main>:
  401126: push   rbp
  401127: mov    rbp, rsp
  40112a: mov    edi, 0x402004         # 传递 "first" 字符串常量地址 (RDI)
  40112f: e8 fc fe ff ff               # call 401030 <puts@plt>
  401134: cmp    eax, 0xffffffff       # 比较返回值与 EOF
  401137: jne    401140
  ...
  401140: mov    edi, 0x40200a         # 传递 "second" 字符串常量地址 (RDI)
  401145: e8 e6 fe ff ff               # call 401030 <puts@plt>
  40114a: cmp    eax, 0xffffffff
  ...
```

#### 手工验证 RIP 相对位移计算
观察第一次调用的机器码：`e8 fc fe ff ff`
* 操作码 `e8` 代表 `call rel32`。
* 紧随其后的 4 字节操作数为小端序：`fc fe ff ff` $\rightarrow$ 补码解析为 32 位有符号整数：
  $$0\text{xFFFFFEFC} = -0\text{x104}$$
* **System V ABI / x86-64 规范**：相对寻址的基准永远是**下一条指令的起始地址**（Next RIP）：
  $$\text{Next\_RIP} = 0\text{x401134}$$
  $$\text{Target} = \text{Next\_RIP} + \text{disp32} = 0\text{x401134} + (-0\text{x104}) = 0\text{x401030}$$
目标地址精准锁定在 `0x401030`（即 `puts@plt`）！

---

### 3.2 `puts@plt` 专属入口剖析（16 字节）

使用 `objdump -d -M intel --section=.plt plt_demo` 查看跳板指令：

```asm
0000000000401030 <puts@plt>:
  401030: ff 25 ca 2f 00 00    jmp    QWORD PTR [rip+0x2fca]   # 404000 <puts@got.plt>
  401036: 68 00 00 00 00       push   0x0                      # 压入重定位序号 (reloc_index)
  40103b: e9 e0 ff ff ff       jmp    401020 <.plt>            # 跳转到公共 PLT0
```

#### 指令 1：`401030: jmp QWORD PTR [rip+0x2fca]`
* **寻址计算**：下一条指令地址为 `0x401036`，目标内存单元为：
  $$0\text{x401036} + 0\text{x2fca} = 0\text{x404000}$$
  该地址正是数据段中的 `GOT[puts]` 槽位！
* **间接跳转语义**：`jmp QWORD PTR [...]` 是从该内存单元**读取 8 字节内容**，并将该内容载入 `RIP`！**绝非跳到 `0x404000` 执行数据！**
* **初始状态陷阱**：在程序刚启动尚未解析时，ELF 文件在 `0x404000` 里预填的值恰好就是下一条指令的地址：`0x0000000000401036`！
  因此，这条 `jmp` 实际上直接滑落到紧随其后的 `0x401036`。

#### 指令 2：`401036: push 0x0`
* 向栈中压入 8 字节立即数 `0x0`。
* 这个 `0` 代表该外部调用在 `.rela.plt` 重定位表中的**下标索引（Relocation Offset Index）**。第 0 号条目正是关于 `puts` 的重定位描述。

#### 指令 3：`40103b: jmp 401020`
* 相对跳转至公共跳板 `PLT0`。注意这是 `jmp` 而非 `call`，**不会在栈顶新增返回地址**。

---

### 3.3 `PLT0` 公共入口剖析（16 字节）

所有外部动态函数的 PLT 最终未命中时，都会汇聚到位于 `.plt` 开头的公共代码块 `PLT0`：

```asm
0000000000401020 <.plt>:
  401020: ff 35 ca 2f 00 00    push   QWORD PTR [rip+0x2fca]   # 403ff0 <GOT[1]>
  401026: ff 25 cc 2f 00 00    jmp    QWORD PTR [rip+0x2fcc]   # 403ff8 <GOT[2]>
  40102c: 0f 1f 40 00          nop    DWORD PTR [rax+0x0]      # 4字节对齐填充
```

#### 指令 1：`push QWORD PTR [rip+0x2fca]`
* 下一条指令地址为 `0x401026`，读取内存地址：
  $$0\text{x401026} + 0\text{x2fca} = 0\text{x403ff0} \quad (\text{即 } \text{GOT}[1])$$
* `GOT[1]` 存放的是动态链接器在初始化时写入的本模块标识：`struct link_map *` 指针。

#### 指令 2：`jmp QWORD PTR [rip+0x2fcc]`
* 下一条指令地址为 `0x40102c`，读取内存地址：
  $$0\text{x40102c} + 0\text{x2fcc} = 0\text{x403ff8} \quad (\text{即 } \text{GOT}[2])$$
* `GOT[2]` 存放的是动态链接器的运行时解析入口：`&_dl_runtime_resolve`。
* CPU 控制权正式移交给用户态动态链接器！

---

## 四、栈拓扑全景演变：首次解析 vs 后续直达

设进入 `main` 时，初始栈顶为 `S`（$S \pmod{16} = 8$，`[S]` 保存 `main` 的返回地址）。

```
第一次调用解析过程中，栈顶内存拓扑的四步推演：

步骤 1: main 序言建立帧指针     步骤 2: call puts@plt 压栈       步骤 3: puts@plt 压入重定位号    步骤 4: PLT0 压入 link_map
┌─────────────────────────┐   ┌─────────────────────────┐   ┌─────────────────────────┐   ┌─────────────────────────┐
│ S:   main 返回地址       │   │ S:   main 返回地址       │   │ S:   main 返回地址       │   │ S:   main 返回地址       │
├─────────────────────────┤   ├─────────────────────────┤   ├─────────────────────────┤   ├─────────────────────────┤
│ S-8: 旧 RBP             │   │ S-8: 旧 RBP             │   │ S-8: 旧 RBP             │   │ S-8: 旧 RBP             │
└─────────────────────────┘   ├─────────────────────────┤   ├─────────────────────────┤   ├─────────────────────────┤
       ▲ (RSP = S-8)          │ S-16: 0x401134 (返回点) │   │ S-16: 0x401134 (返回点) │   │ S-16: 0x401134 (返回点) │
                              └─────────────────────────┘   ├─────────────────────────┤   ├─────────────────────────┤
                                     ▲ (RSP = S-16)         │ S-24: 0 (reloc_index)   │   │ S-24: 0 (reloc_index)   │
                                                            └─────────────────────────┘   ├─────────────────────────┤
                                                                   ▲ (RSP = S-24)         │ S-32: link_map 指针     │
                                                                                          └─────────────────────────┘
                                                                                                 ▲ (RSP = S-32)
```

### 4.1 动态链接器内部工作机理
当执行流以 `RSP = S-32` 进入 `_dl_runtime_resolve(link_map, reloc_index)` 时：
1. **寄存器现场保护**：为了不破坏调用方准备好的参数寄存器（`RDI` 保存着 `"first"`，`RSI`、`RDX` 等），动态链接器首先在栈上完整保存所有通用寄存器及 SIMD 寄存器（`XMM0`~`XMM7`）；
2. **定位重定位结构**：根据 `link_map` 找到程序的 `.rela.plt` 基址，利用传入的 `reloc_index = 0`，索引到 `Elf64_Rela` 结构；
3. **符号名与版本查找**：从 `r_info` 提取符号表下标，在 `.dynsym` 和 `.dynstr` 中查到名字 `"puts"` 及版本 `"GLIBC_2.2.5"`；
4. **共享库符号解析**：沿 `link_map` 链表在已加载的 `libc.so.6` 符号表中执行哈希查找，得到 `puts` 在 libc 中的运行时基址偏移加上库加载基址，算出真实地址 $P$；
5. **原地回写 GOT**：执行核心指令 `* (uint64_t *)r_offset = P`，即**覆写 `0x404000` 处的数值为 $P$**；
6. **现场恢复与平栈**：恢复全部易失寄存器，清理掉 `link_map` 和 `reloc_index`（平栈 16 字节），使栈顶重新精确对齐到 `S-16`（即 `main` 的返回地址 `0x401134`）；
7. **无缝交付**：动态链接器执行一条间接跳转指令 `jmp P`，控制权平滑切入真实 `puts` 函数！

### 4.2 第二次调用的光速直达（Fast Path）
在执行第二次 `puts("second")` 时：
1. `main` 执行 `call 0x401030`（压入下一条返回地址 `0x40114a`，`RSP = S-16`）；
2. 执行 `puts@plt` 第一条指令：`jmp QWORD PTR [0x404000]`；
3. 此时 `0x404000` 内存中**已经是 libc 中 `puts` 的真实地址 $P$**；
4. CPU 直接转跳至 $P$ 开始打印！
5. `puts@plt` 后续的 `push 0`、`PLT0`、动态链接器解析过程**全被绕过，一次内存间接寻址即完成调用**！

---

## 五、底层数据结构映射：`.rela.plt` 与 `.got.plt`

### 5.1 `.rela.plt` 重定位项的二进制规范
在 64 位 System V ABI 下，重定位项 `Elf64_Rela` 大小为 **24 字节**：

```c
typedef struct {
    Elf64_Addr   r_offset; // 8 字节: 待修改的目标内存虚拟地址 (0x404000)
    Elf64_Xword  r_info;   // 8 字节: 高 32 位为符号表下标，低 32 位为重定位类型
    Elf64_Sxword r_addend; // 8 字节: 计算偏移加数 (通常为 0)
} Elf64_Rela;
```

使用 `readelf -rW plt_demo` 查看输出：
```text
Relocation section '.rela.plt' at offset 0x5d8 contains 1 entry:
    Offset             Info             Type               Symbol's Value  Symbol's Name + Addend
0000000000404000  0000000200000007 R_X86_64_JUMP_SLOT    0000000000000000 puts@GLIBC_2.2.5 + 0
```
* **`r_offset = 0x404000`**：指向 `.got.plt` 中为 `puts` 分配的地址槽。
* **`r_info = 0x0000000200000007`**：
  * 高 32 位为 `0x2` $\rightarrow$ 符号表中第 2 号符号（`puts`）；
  * 低 32 位为 `0x7` $\rightarrow$ `R_X86_64_JUMP_SLOT`（指令重定位槽类型）。

### 5.2 `.got.plt` 的保留表项拓扑
`.got.plt` 节区固定将前 3 个 8 字节槽（共 24 字节）保留给动态链接系统：

| 槽位索引 | 内存地址 | 槽位名称 | 物理内容与作用 |
| :---: | :---: | :---: | :--- |
| `GOT[0]` | `0x403fe8` | `_DYNAMIC` | 指向 ELF 的 `.dynamic` 动态信息段首地址，供动态链接器自省。 |
| `GOT[1]` | `0x403ff0` | `link_map` | 进程装载后由 `ld.so` 填充，指向主程序的模块链表节点。 |
| `GOT[2]` | `0x403ff8` | `dl_resolver` | 进程装载后由 `ld.so` 填充，指向 `_dl_runtime_resolve` 函数入口。 |
| **`GOT[3]`** | **`0x404000`** | **`GOT[puts]`** | **首个外部函数跳转槽**。初始为 `0x401036`，首次调用后由解析器重写为真实函数基址 $P$。 |

---

## 六、现代工业变体与安全对抗：PIE、RELRO 与 `-fno-plt`

### 6.1 Full RELRO 与立即绑定（`-z now`）
在经典懒绑定下，`GOT[puts]` 必须在运行期被修改，因此 `.got.plt` 所在内存页必须保持可写（`RW-`）。黑客利用堆栈溢出等漏洞篡改 GOT 表项即可轻松劫持控制流（GOT Overwrite 攻击）。

为了封堵该攻击面，工业界引入了 **Full RELRO（Full Relocation Read-Only）**：
```bash
gcc -Wl,-z,relro -Wl,-z,now plt_demo.c -o plt_full_relro
```
* **运行机理**：`-z now` 强制动态链接器在程序刚启动时**一次性解析所有外部动态符号**，并填妥全部 GOT 表；
* **内存加固**：解析完成后，动态链接器调用 `mprotect()` 将包含 GOT 的整个 `PT_GNU_RELRO` 内存段**彻底降权为只读（`R--`）**；
* **安全代价**：消除了懒绑定，但换取了整个 GOT 表免受任何内存写原语篡改的绝对安全。

### 6.2 `-fno-plt`：彻底拔除跳板的现代化调用
GCC 引入了 `-fno-plt` 代码生成选项：
```bash
gcc -O2 -fno-plt plt_demo.c -o plt_noplt
```
反汇编变化：
```asm
# 经典模式
call 401030 <puts@plt>

# -fno-plt 模式
call QWORD PTR [rip+0x2ed2]        # 直接间接 call 读取 GOT 表项
```
* **底层革新**：直接取消了 `.plt` 代码跳板与 `R_X86_64_JUMP_SLOT` 重定位，改为通过 `R_X86_64_GLOB_DAT` 重定位项直接从 GOT 加载函数指针；
* **性能收益**：省去了一次无谓的 `jmp` 跳转，极大减轻了 CPU 分支目标预测器（BTB）的压力，同时指令体积更紧凑。

### 6.3 Intel CET 与 `.plt.sec`
在现代 Ubuntu 开启 IBT（间接分支跟踪）的环境下，编译器会生成双层 PLT：
* `.plt.sec`：精简跳板，首条指令为 `endbr64`，直接间接跳转至 GOT；
* `.plt`：仅保留延迟绑定时的慢速解析流程代码。

---

## 七、GDB 动态单步实验：亲眼见证 GOT 表的质变

### 7.1 实验断点规划与执行
```bash
gdb -q ./plt_demo
```
在 GDB 中依次执行以下工业级调试命令：

```gdb
(gdb) set disassembly-flavor intel
(gdb) break main
(gdb) run
# 停在 main 函数入口

# 1. 检查调用前 GOT[puts] (0x404000) 的初始值
(gdb) x/gx 0x404000
0x404000 <puts@got.plt>:    0x0000000000401036
# 验证发现: 其内容确实精确指向 puts@plt 的第二条指令 push 0x0 !

# 2. 单步跟踪进入 puts@plt
(gdb) break *0x401030
(gdb) continue
(gdb) display/i $pc
(gdb) stepi
# 观察到下一条指令正是 0x401036 (push 0x0)
(gdb) stepi
# 观察到进入 0x40103b (jmp 401020)
(gdb) stepi
# 观察到进入 PLT0 (0x401020: push [GOT[1]])

# 3. 让第一次 puts 调用完整执行并返回
(gdb) finish
# 此时终端输出 "first" 字符串

# 4. 再次检查 GOT[puts] 内存单元！见证质变！
(gdb) x/gx 0x404000
0x404000 <puts@got.plt>:    0x00007ffff7e12340
# 结论: 0x404000 处的指针已被动态链接器成功覆写为 libc 的真实 puts 物理映射地址!

# 5. 单步跟进第二次 puts 调用
(gdb) break *0x401030
(gdb) continue
(gdb) stepi
# 奇迹发生: 第一条 jmp QWORD PTR [0x404000] 直接单步跳入了 0x7ffff7e12340 (libc puts)！
# 完全不再执行后续的 push 0 和 PLT0！
```

---

## 八、硬核课后推演与考核题

### 练习 1：手算相对寻址与跳转目标
已知以下反汇编指令：
```asm
0000000000400500: e8 fb 00 00 00    call 0x400600
```
请根据 Intel x86-64 规范，推导机器码中的 `fb 00 00 00` 是如何得出的？

> **推导解析**：
> 1. `call rel32` 指令全长为 5 字节（1 字节操作码 `e8` + 4 字节位移操作数）；
> 2. 下一条指令基准地址：$\text{Next\_RIP} = 0\text{x400500} + 5 = 0\text{x400505}$；
> 3. 计算相对位移：$\text{disp32} = \text{Target} - \text{Next\_RIP} = 0\text{x400600} - 0\text{x400505} = 0\text{xFB}$；
> 4. 按照小端序排布，低字节在先：`fb 00 00 00`。

### 练习 2：动态可执行文件的首条指令
在动态链接程序中，`main()` 是不是 CPU 进入用户态后执行的第一条机器指令？

> **推导解析**：
> 绝对不是。内核 `execve` 装载后，首先将控制权转移给 `PT_INTERP` 指定的动态链接器 `ld.so`；`ld.so` 完成共享库装载与 GOT 初始化后，跳转到主程序的 `_start`（定义于 `crt1.o`）；`_start` 调用 `__libc_start_main` 完成环境配置与静态构造函数调用后，才通过 `call main` 正式进入用户业务逻辑。

### 练习 3：静态链接为什么完全没有 PLT 和 GOT？
执行 `gcc -static hello.c -o hello` 后，反汇编为什么找不到 `.plt` 节和 `puts@plt`？

> **推导解析**：
> 静态链接在编译链接期就已经将 `libc.a` 中的 `puts` 实现机器码提取并融合进同一个 ELF 二进制文件中。`main` 与 `puts` 之间的相对虚拟偏移在链接完成的那一刻就已经彻底固定，链接器直接将 `call` 指令修正为针对文件内部函数的相对跳转，无需任何运行时动态寻址或间接跳板。

---

## 九、口头答辩验收标准（Checklist）

在进入 Day 23 探索 GOT 劫持与重定位深水区前，必须能脱稿无死角回答以下 8 个核心问题：

- [ ] **1. PLT 与 GOT 的内存属性区别**：PLT 位于哪个节？属于什么内存权限？GOT 位于哪个节？为什么默认是可写的？
- [ ] **2. 首次调用的死循环防范**：`puts@plt` 的第一条指令是 `jmp [GOT[puts]]`，为什么首次执行不会死循环？GOT 初始值是什么？
- [ ] **3. 重定位工单传递**：`puts@plt` 第二条指令 `push 0x0` 压入的立即数有什么物理用途？
- [ ] **4. 动态链接器的寻址入口**：`PLT0` 是如何找到 `_dl_runtime_resolve` 的？`GOT[1]` 和 `GOT[2]` 各存了什么？
- [ ] **5. 运行时覆写点**：动态链接器解析完成后，覆写的具体内存地址是什么？是修改代码段还是修改数据段？
- [ ] **6. 第二次调用的光速执行**：为什么第二次执行 `call puts@plt` 不再进入动态链接器？
- [ ] **7. Full RELRO 的加固逻辑**：`-z now` 如何彻底废除延迟绑定？为什么说 Full RELRO 能防御 GOT 覆写攻击？
- [ ] **8. `-fno-plt` 的优化本质**：不使用 PLT 时，编译器如何生成外部函数调用指令？