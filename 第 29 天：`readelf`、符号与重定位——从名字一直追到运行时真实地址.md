# 第 29 天：`readelf`、符号与重定位——从名字一直追到运行时真实地址

> **核心目标**：从零构建基于 ELF 规范与 System V AMD64 ABI 的闭环动态链接证据链。透彻理解 `.symtab`、`.strtab`、`.dynsym`、`.dynstr`、`.shstrtab` 五大核心表的职能边界；掌握 24 字节符号结构 `Elf64_Sym` 与 24 字节重定位结构 `Elf64_Rela` 的物理排布与手工位运算解码；打通从 `.dynstr` 字符串偏移、`.dynsym` 符号索引、`.rela.plt` 重定位项到 GOT 槽物理虚拟地址的完整寻址路径；掌握 `PT_LOAD` 坐标变换算法；通过毁伤实验深刻领悟节头表与程序头表在操作系统加载运行维度的本质差异。

---

## 【📌 一句话核心概括】
**`readelf` 是解析 ELF 内部表项与数据结构的基准工具，它通过解析五大符号/字符串表、解构 24 字节 `Elf64_Sym` 与 `Elf64_Rela` 结构体，完整串联起从动态字符串偏移、动态符号索引、重定位项到运行时 GOT 内存槽位的确定性寻址闭环。**

## 【📖 通俗原理解释】
- **五大符号表与字符串表的分工边界**：`.symtab` 与 `.strtab` 存储程序内部所有本地与全局符号，用于开发与离线调试（`strip` 可安全剥离）；`.dynsym` 与 `.dynstr` 专用于运行期动态链接（包含外部未决符号与对外导出接口，必须被 `PT_LOAD` 映射进内存，`strip` 绝不可删除）；`.shstrtab` 则专供各节区自身的名字寻址。
- **符号与重定位条目的解码链条**：重定位结构体（`Elf64_Rela`）的 `r_info` 高 32 位（ARM64/x86-64）指定动态符号表条目下标（`ELF64_R_SYM`），低 32 位指定重定位类型（如 `R_X86_64_JUMP_SLOT`）。解析器先通过索引在 `.dynsym` 中找到 24 字节的 `Elf64_Sym`，再由其 `st_name` 偏移在 `.dynstr` 字符串表中取出符号真名（如 `"puts"`），最后将目标函数加载地址回写至 `r_offset` 所指向的 GOT 槽。
- **程序头表（Phdr）与节头表（Shdr）的执行决定性**：操作系统内核装载器（`execve`）仅依赖程序头表（`PT_LOAD`）分配虚拟内存与映射物理页；若手工将节头表偏移 `e_shoff` 抹零，ELF 依然能正常执行；但若破坏程序头表偏移 `e_phoff`，内核将报格式错误直接拒绝执行。

---

## 一、五大符号与字符串表的正交边界

在逆向与静态分析中，绝不能将各类符号表与字符串表混淆：

```mermaid
flowchart TD
    subgraph 链接与静态调试视角 (Strip 可安全删除)
        S1[".symtab 本地+全局符号表<br/>(含 static 函数、本地变量、文件名)"] -->|引用偏移| S2[".strtab 普通符号字符串表"]
        S3[".shstrtab 节区名字符串表<br/>(专门保存 .text, .rodata 等节的名字)"]
    end
    subgraph 运行期动态链接必需 (Strip 绝不可删除)
        D1[".dynsym 动态符号表<br/>(仅含外部导入 UND 与对外导出符号)"] -->|st_name 偏移| D2[".dynstr 动态字符串表<br/>(保存导入函数名、依赖库名 libc.so.6)"]
        R1[".rela.plt 过程链接重定位表"] -->|r_info 索引| D1
        R1 -->|r_offset 槽位| GOT[".got.plt 全局偏移表槽位"]
    end
```

| 节名称（Section） | 核心职能 | 运行时是否映射入内存 | 执行 `strip --strip-all` 后的命运 |
| :--- | :--- | :---: | :--- |
| **`.symtab`** | 完整符号表（本地 static、全局、对象、文件名） | 否 | **物理抹除**（节省磁盘空间） |
| **`.strtab`** | `.symtab` 中符号名称的字符串存储池 | 否 | **物理抹除** |
| **`.dynsym`** | 动态链接必需的符号表（导入 `UND` 与动态导出） | **是**（`PT_LOAD` 覆盖） | **严格保留**（删除将导致动态链接崩溃） |
| **`.dynstr`** | 动态符号名、共享库名（`libc.so.6`）、版本名 | **是** | **严格保留** |
| **`.shstrtab`** | 存储各个**节自身的名字**（如 `".text"`, `".data"`） | 否 | 保留（若手动清空 `e_shoff` 则失去意义） |

> [!IMPORTANT] **字符串复用机制（String Tail Merging）**
> 在 `.dynstr` 中，编译器和链接器为了节省空间，会采用后缀复用。例如：
> - 偏移 `0x26` 处保存 `"fprintf\0"`
> - 此时若需要存储 `"printf\0"`，链接器**不需要额外开辟空间**，直接将 `printf` 的 `st_name` 指向 `0x27`（跳过首字母 `'f'`）！
> 因此，`st_name` 绝非以单词为单位的索引，而是**严格的字节物理偏移量**！

---

## 二、底层数据结构物理布局与手工位运算

### 1. 符号表项 `Elf64_Sym`（标准 24 字节布局）
```c
typedef struct {
    Elf64_Word    st_name;   // +0x00 (4B): 在对应字符串表中的字节偏移
    unsigned char st_info;   // +0x04 (1B): 符号类型与绑定属性 (Type & Binding)
    unsigned char st_other;  // +0x05 (1B): 符号可见性 (Visibility)
    Elf64_Half    st_shndx;  // +0x06 (2B): 所在节索引 (SHN_UNDEF=0 表示外部未定义)
    Elf64_Addr    st_value;  // +0x08 (8B): 符号物理地址/数值 (外部导入符号此处恒为 0)
    Elf64_Xword   st_size;   // +0x10 (8B): 符号大小 (函数指令字节数或变量大小)
} Elf64_Sym; // 共计 24 字节 (0x18)，8 字节自然对齐，无内嵌填充
```

### 2. 重定位表项 `Elf64_Rela`（标准 24 字节布局）
```c
typedef struct {
    Elf64_Addr   r_offset; // +0x00 (8B): 待修补的运行时虚拟内存地址 (即 GOT 槽地址)
    Elf64_Xword  r_info;   // +0x08 (8B): 高 32 位为符号索引，低 32 位为重定位类型编号
    Elf64_Sxword r_addend; // +0x10 (8B): 显式附加加数 (Explicit Addend)
} Elf64_Rela; // 共计 24 字节 (0x18)
```

### 3. `r_info` 64 位整型的手工位运算解码
在 x86-64 架构下，`r_info` 采用复合编码：
$$\text{Symbol Index} = \text{r\_info} \gg 32$$
$$\text{Relocation Type} = \text{r\_info} \ \& \ \text{0xFFFFFFFF}$$

**实战案例推演**：若重定位表项读出 `r_info = 0x0000000200000007`：
- **符号索引**：`0x0000000200000007 >> 32 = 2` $\rightarrow$ 引用 `.dynsym` 中的第 2 号条目。
- **重定位类型**：`0x0000000200000007 & 0xFFFFFFFF = 7` $\rightarrow$ 查询 x86-64 ABI 手册，编号 7 严格对应 **`R_X86_64_JUMP_SLOT`**！

---

## 三、从名字到运行时地址：端到端六步证据链

以动态调用标准库函数 `puts("ELF day 29")` 为例，建立严丝合缝的静态解析路径：

```text
[第 1 步: 动态字符串名]  .dynstr + 0x01 = "puts\0"
                              │
[第 2 步: 动态符号条目]  .dynsym[2] (偏移: dynsym_base + 2 * 24)
                              ├── st_name = 0x01 ──> 关联 "puts"
                              └── st_shndx = SHN_UNDEF (外部未定义，标记 UND)
                              │
[第 3 步: 关联重定位项]  .rela.plt 表中检索引用符号索引 2 的条目:
                              ├── r_info   = 0x0000000200000007 (JUMP_SLOT)
                              └── r_offset = 0x404000 (锁定待修补 GOT 物理地址)
                              │
[第 4 步: 汇编跳板绑定]  puts@plt (0x401070):
                              └── jmp QWORD PTR [rip + 0x2f86]
                                  (0x40107a + 0x2f86 = 0x404000！精准命中 r_offset)
                              │
[第 5 步: 运行期延迟绑定] 首次调用时 GOT[0x404000] 填入动态链接器解析入口;
                              动态链接器扫描 libc.so.6 导出表，计算 puts 真实虚拟地址 S;
                              │
[第 6 步: 原地覆写生效]  动态链接器执行 *(uint64_t*)0x404000 = S;
                              后续所有 puts@plt 间接跳转直达 libc 内部！
```

---

## 四、三大核心动态重定位类型对比

| 重定位类型 | 计算公式 | 典型出现位置 | 语义与触发时机 |
| :--- | :--- | :--- | :--- |
| **`R_X86_64_JUMP_SLOT`** | $\text{GOT[slot]} = S$ | `.rela.plt` | **函数调用重定位**。用于 PLT 延迟绑定跳板，首次调用或启动时填入外部函数真实地址。 |
| **`R_X86_64_GLOB_DAT`** | $\text{GOT[slot]} = S$ | `.rela.dyn` | **全局变量/指针重定位**。如外部数据 `stderr`、`__libc_start_main`，装载阶段必须立即解析。 |
| **`R_X86_64_RELATIVE`** | $\text{Target} = B + A$ | `.rela.dyn` | **基址重定位（PIE/共享库专用）**。不查符号名！直接将加载基址 $B$ 加上代码附加值 $A$，执行速度极快。 |

---

## 五、虚拟地址与文件偏移换算（PT_LOAD 坐标变换）

在无调试器协助的静态反汇编逆向中，将内存虚拟地址（VAddr）反查回磁盘文件原始十六进制偏移（File Offset）是基本功：

```text
               文件偏移轴 (File Offset)                       内存虚拟地址轴 (Virtual Memory Address)
       0x0000 ┌──────────────────────┐               0x000000 ┌──────────────────────┐
              │ ELF Header & Headers │                        │ (未映射区域)         │
     p_offset ├──────────────────────┤               p_vaddr  ├──────────────────────┤
              │                      │                        │                      │
              │  PT_LOAD 文件有效数据 │ 映射到虚拟内存空间        │  PT_LOAD 内存代码/数据│
              │  (长度: p_filesz)     │ ─────────────────────> │  (长度: p_filesz)    │
              │                      │                        │                      │
p_offset+filesz├──────────────────────┤        p_vaddr+filesz ├──────────────────────┤
              │ 下一个段或节头元数据 │                        │ 零填充区 (.bss)      │
              │                      │                        │ (长度: memsz-filesz) │
              └──────────────────────┘         p_vaddr+memsz  ├──────────────────────┤
                                                              │ (页对齐 Padding)     │
                                                              └──────────────────────┘
```

### 1. 换算黄金法则
若目标虚拟地址 $\text{VAddr}$ 落在某个 `PT_LOAD` 段范围内（即 $p\_vaddr \le \text{VAddr} < p\_vaddr + p\_filesz$）：
$$\text{File\_Offset} = \text{VAddr} - p\_vaddr + p\_offset$$

### 2. 实战推演：换算 `puts` GOT 槽（0x404000）的文件偏移
在 `readelf -lW day29_demo` 中找到对应的数据可读写段：
```text
Type   Offset   VirtAddr   PhysAddr   FileSiz  MemSiz   Flg Align
LOAD   0x002df8 0x00403df8 0x00403df8 0x000238 0x000258 RW  0x1000
```
- **验证范围**：文件有效上限为 $0x403df8 + 0x238 = 0x404030$。目标 $0x404000$ 严格落在此区间内。
- **数学代入**：
  $$\text{File\_Offset} = 0x404000 - 0x403df8 + 0x002df8 = 0x208 + 0x2df8 = 0x3000$$
- **十六进制验证**：执行 `xxd -s 0x3000 -l 8 day29_demo`，打印出的 8 字节即为磁盘中 `puts` GOT 槽的初始值（未绑定前指向 `0x401030` PLT 解析入口）。

> [!CAUTION] **零填充区（.bss）的致命边界**
> 若地址落在 $[p\_vaddr + p\_filesz, \ p\_vaddr + p\_memsz)$ 区间内，说明该地址属于运行时内核 `mmap` 分配并清零的内存区（如未初始化的全局变量）。**该地址在磁盘 ELF 文件中不存在任何对应的物理字节**！强行换算将读取到无关的节头或文件外部数据。

---

## 六、节头表 vs 程序头表毁伤实验与加载机理

通过对 ELF 头关键字段的主动故障注入（Fault Injection），揭示 Linux 内核加载与分析工具的底线逻辑：

| 故障实验项目 | 修改位置与注入数据 | 工具表现（`readelf` / `file`） | Linux 内核 `execve` 运行表现 | 底层原理剖析 |
| :--- | :--- | :--- | :--- | :--- |
| **破坏 ELF 魔数** | `offset 0x00`: 写入 `\x00` 破坏 `\x7fELF` | `file` 降级为 `data`；`readelf` 报错退出 | **立即拒绝执行**<br/>Shell 提示：`cannot execute binary file` | 魔数是内核识别 ELF 格式的绝对硬性第一道关卡。 |
| **入口地址归零** | `offset 0x18`: `e_entry` 写入 8 字节 `0x0` | `readelf -h` 正常显示，入口点显示为 `0x0` | **段错误崩溃**<br/>内核成功映射，跳转 `0x0` 引发 `SIGSEGV` | 内核不校验入口点语义，盲目跳转未映射的零地址。 |
| **清空节头表偏移** | `offset 0x28`: `e_shoff` 写入 8 字节 `0x0` | `readelf -S` 报错找不到节头；`objdump` 丢失节名 | **完全不受影响，完美执行！** | **核心精髓**：内核与动态链接器只依赖程序头（`PT_LOAD` 与 `PT_DYNAMIC`），完全不看节头！ |
| **清空程序头表偏移** | `offset 0x20`: `e_phoff` 写入 8 字节 `0x0` | `readelf -S` 仍可读节；`readelf -l` 报错瘫痪 | **立即拒绝执行**<br/>内核无法获知如何映射内存，报格式错误 | 内核无法构建进程虚拟内存空间映射（VMA）。 |

---

## 七、`readelf` 核心实战检查命令集

```bash
# 1. 查看 ELF 头元数据 (入口点、程序头/节头表物理偏移与数量)
readelf -hW <elf_file>

# 2. 检查程序头表 (Segment 加载段划分与权限映射)
readelf -lW <elf_file>

# 3. 检查节头表 (Section 逻辑划分、大小、权限与 Lk/Inf 引用链)
readelf -SW <elf_file>

# 4. 仅提取动态符号表 (查看外部导入函数 UND 及符号版本)
readelf --dyn-syms -W <elf_file>

# 5. 查看动态重定位项 (重点排查 .rela.plt 与 .rela.dyn)
readelf -rW <elf_file>

# 6. 转储动态字符串表内容 (排查函数名与依赖库名物理偏移)
readelf --string-dump=.dynstr <elf_file>

# 7. 动态观察运行时符号绑定全过程 (排查实际加载库与符号覆盖)
LD_DEBUG=bindings ./<elf_file> <args>
```

---

## 八、口头验收与面试高频题

1. **`.dynsym` 和 `.symtab` 的核心区别是什么？为什么执行 `strip` 命令后程序依然能够调用外部 `puts`？**
   - `.symtab` 包含编译期和静态链接所需的所有符号（包括本地静态函数和变量），属于调试与辅助元数据，`strip` 会将其彻底抹除；而 `.dynsym` 仅包含运行时动态链接必需的导入与导出符号，被包含在只读 `PT_LOAD` 段中随程序载入内存，动态链接器必须依据它解析外部函数地址，因此常规 `strip` 绝对不会也不能删除 `.dynsym`。
2. **在 64 位 ELF 中，重定位表项的 `r_info` 字段是如何编码的？若 `r_info = 0x0000000200000007`，其具体含义是什么？**
   - `r_info` 是 64 位整型，采用复合编码：高 32 位表示引用的动态符号表索引（`r_info >> 32`），低 32 位表示重定位类型编号（`r_info & 0xFFFFFFFF`）。该数值表示：引用 `.dynsym` 中索引为 2 的符号，执行编号为 7 的 `R_X86_64_JUMP_SLOT`（PLT 槽位跳转重定位）。
3. **为什么一个 ELF 文件的节头表（Section Header Table）被恶意擦除后，程序依然能够被 Linux 内核正常执行？**
   - Linux 内核的 `execve` 系统调用加载器只读取和解析**程序头表（Program Header Table）**中的 `PT_LOAD` 段来映射虚拟内存空间，并通过 `PT_INTERP` 和 `PT_DYNAMIC` 段唤醒动态链接器；内核和动态链接器在运行期完全不依赖节头表。节头表仅仅是提供给编译器、链接器和静态分析工具（如 GDB/objdump）使用的逻辑视图。
4. **重定位项中的 `r_offset` 是目标函数的实际入口地址吗？如果不是，它到底代表什么？**
   - 绝对不是！`r_offset` 是待修补内存槽位的虚拟地址（通常是 GOT 表中的某个 8 字节条目）。它是动态链接器写入函数真实地址的“容器地址”，而不是函数本身的执行入口。
5. **在利用 `PT_LOAD` 将虚拟内存地址换算为文件偏移时，什么情况下会出现“该地址有效，但在文件中无对应字节”？**
   - 当该虚拟地址落在该段的未初始化数据区（`.bss`）时。该区域的物理特征是 `p_memsz > p_filesz`，超出 `p_filesz` 的部分在磁盘文件中不占用实体字节存储，仅在内核装载映射时分配全零物理页。