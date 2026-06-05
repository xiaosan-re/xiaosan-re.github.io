---
title: 汽车 App 加固分析：网易易盾壳的自定义 linker、SMC 与脱壳实战
categories: Android
tags: [Android, 逆向, 脱壳, 加固, 易盾, 自定义linker, SMC, DEX, ELF]
keywords: Android 脱壳, 网易易盾, libnesec, 自定义 linker, init_array, SMC 自修改代码, 抹除 ELF 头, JNI_OnLoad 定位, RegisterNatives, DexFile OpenCommon, 反调试, 二次打包, 小鹏汽车
description: 以某汽车 App（网易易盾加固）为样本，完整走一遍 dex 壳的逆向脱壳：从壳的 Java 层 attachBaseContext，到 SO 的入口隐藏、自定义 linker 加载、运行时 SMC 加解密、抹除 ELF 头防 dump，再到 JNI_OnLoad 定位、反调试、DEX 解密与 DexFile::OpenCommon 脱壳点，最后 dump + 二次打包跑起来。
---

## 一、背景 & 场景

### 1.1 环境

样本：某汽车 App（包名 `com.xiaopeng.mycarinfo`），网易易盾（`libnesec.so` + `com.netease.nis.wrapper`）加固，Android arm64

工具链：IDA Pro 9.2、调试器（linker / art 下断）、JADX

### 1.2 加固的本质，与不同视角

聊脱壳之前，先说点务虚的——加固到底在保护什么。

**代码安全只是表面**，核心是帮客户满足"业务不被阻断、关键数据资产不被窃取"的安全需求。加固自身不创造价值，它的价值必须挂在业务上间接体现：通过安全体系为业务保驾护航，增加收益、降低资损率。

换个立场看 App 加固，每一面看到的东西都不一样：

- **用户视角**：只关注功能和交互，对 C 端用户而言加固几乎无感；
- **产品视角**：关注需求、方案、价值；
- **技术视角**：关注架构、实现、可扩展性与安全性，能感知加固的重要性，但更多从技术实现出发；
- **业务视角**：保障业务正常运营，看到的是成本和收益。

视角不同，决策不同，安全程度也不同。有了这层铺垫，下面这套"看起来有点绕"的加固方案，你大概就不会觉得奇怪了——**它是计划的一部分**。

本文以一个汽车 App 的易盾壳为样本，完整走一遍脱壳：壳的 Java 层 → SO 静态分析（入口怎么藏的）→ SO 动态分析（自定义 linker、SMC、抹 ELF 头）→ DEX 解密与脱壳点 → dump 二次打包。文中涉及的包名、密钥等均为逆向实测结果，仅供安全研究学习。

> 本文仅限学习交流与防御研究，请勿用于非法及商业用途。

## 二、加固整体架构

整套壳是经典的 **dex 整体加固 + Native 自定义 linker 保护** 结构：

![加固整体架构](/images/posts/android/yidun-unpack/architecture.jpg)

分三层看：

- **Java 层**：替换 App 入口、读取壳 SO、判断目标平台、加载壳 SO；
- **Native 层（`libnesec.so`）**：壳入口（`init_array` / 解密代码 / 自定义 linker 加载 SO）、初始化（`JNI_OnLoad` / SMC 执行代码 / 注册 native）、反调试（IDA 端口 / 脱壳工具 / 进程状态 / root·模拟器）；
- **Load Dex**：解密 dex、加载 dex、还原原 App 入口。

### 解壳过程

壳跑起来后，干的就是"解密原始 dex + 启动加载原 App Application"这一串活：

1. **解密原 apk 的 dex 集合**——用加密时对应的算法逐个解密 dex；
2. **把解密后的 dex 加进 `dexElements` 数组**——通过反射塞进去；
3. **动态加载原 apk 的 Application**——加密时原 Application 被替换成了壳的 Application，加载过程中要把这步还原回去。

## 三、壳 Java 层分析

### 3.1 attachBaseContext

SO 的释放与加载发生在进程创建、加载 Application **之前**：在 `attachBaseContext` 里解压释放 SO 到 lib 目录，`System.loadLibrary("nesec")` 加载进内存，并在 `MyJni` 类里注册了几个 native 方法：

```java
public static native void cp();
public static native void d(String arg0);
public static native void e(String arg0);
public static native boolean load(Application arg0, String arg1);
public static native boolean load2(Application arg0, String arg1);
public static native boolean run(Context arg0, Application arg1);
```

### 3.2 native load 加载 dex

加载完 SO 后调用 `load`：解密 dex 并加载到内存——dex 文件解密 + 把解密后的 dex 集合塞进 `dexElements` 数组。

### 3.3 还原原 Application

dex 加载好后，要从**原 App 的 Application** 跑起来。壳里存了原 Application 的类名，反射创建并执行它：

```java
static {
    MyApplication.strAppName = "com.netease.nis.wrapper.MyApplication";
    ...
}
private static Application a(Context ctx) {
    ClassLoader cl = ctx.getClassLoader();
    Class c = cl.loadClass(MyApplication.strAppName);   // 加载原 Application
    MyApplication.newApp = (Application) c.newInstance(); // 创建实例
    return MyApplication.newApp;
}
```

具体流程：

1. `Class.newInstance()` 创建一个 Application 实例；
2. 反射拿到 `Application.attach(Context)` 并 `invoke` 执行：

```java
Application app = MyApplication.a(ctx);
Method attach = Application.class.getDeclaredMethod("attach", Context.class);
attach.setAccessible(true);
attach.invoke(app, ctx);   // 让原 App 真正"接管"
```

到这里壳就把控制权交还给了原 App，对上层完全透明。

## 四、壳 SO 静态分析

按惯例，定位到壳 SO 后先用 IDA 静态收集信息（字符串、入口、导出方法）。但这个壳一上来就给你三连击：

**① 加载直接报错**——节信息被处理过，专门用来反静态反编译：

![IDA 加载报错](/images/posts/android/yidun-unpack/ida-warning.jpg)

**② 导出函数一堆乱码**——导出表被加密了：

![导出函数乱码](/images/posts/android/yidun-unpack/export-garbled.jpg)

**③ 找不到 `init_array` 节**——连入口节都被抹了：

![缺失 init_array 节](/images/posts/android/yidun-unpack/no-initarray.jpg)

静态这条路基本被堵死。结论很简单：**静态不好定位就动态定位**——内存里再怎么藏，也跳不出系统的加载机制。

## 五、壳 SO 动态分析

### 5.1 壳入口定位

按 linker 加载 SO 的流程，有两个点可以当壳入口：`init` 和 `init_array` 是 SO 代码能执行的最早时机，之后才轮到 `JNI_OnLoad`。只要在 **linker 执行 `init` 的地方下断**，就能逮到壳入口：

![在 linker init 处下断](/images/posts/android/yidun-unpack/linker-init-bp.jpg)

定位到的 `init_array` 里有几个关键点，其中 `init_array[3]` 负责**解密 `JNI_OnLoad` 的代码**：

![init_array 解密 JNI_OnLoad](/images/posts/android/yidun-unpack/decrypt-jnionload.jpg)

解密例程是个标准的 **RC4 风格 KSA/PRGA 变体**（256 字节 S 盒交换 + 取流 + 一点魔改）：

```
dec_sub:
loop:
    ADD   W6, W6, #1 ; AND W6, W6, #0xFF      ; i = (i+1) & 0xFF
    LDRB  W3, [X0,X4]                          ; S[i]
    ADD   W5, W3, W5 ; AND W5, W5, #0xFF       ; j = (j+S[i]) & 0xFF
    LDRB  W8, [X0,X7]                          ; S[j]
    STRB  W8, [X0,X4] ; STRB W3, [X0,X7]       ; swap S[i],S[j]
    ADD   W4, W3, W4 ; UXTB W4, W4             ; t = (S[i]+S[j]) & 0xFF
    LDRB  W3, [X0,X4]                          ; k = S[t]
    LSR   W4, W3, #2 ; ORR W3, W4, W3,LSL#6    ; 魔改：循环右移
    ADD   W3, W3, #0x3A                        ; +0x3A
    EOR   W3, W3, W7                            ; 与明文异或
    STRB  W3, [X1],#1
    CMP   X1, X2 ; B.NE loop
```

### 5.2 运行时 SMC：执行时解密、执行后加密

壳大量使用 **SMC（自修改代码）**：一段代码只在**即将执行时**才解密，**执行完立刻加密回去**。这样无论你静态分析还是内存 dump，看到的都是密文。解密用 NEON 一次处理 16 字节，单字节 key 异或：

```
decCode:
    ...
    DUP   V1.16B, W23           ; 用 W23 这个 key 字节铺满 16 字节
loop16:
    LDR   Q0, [X3]
    EOR   V0.16B, V0.16B, V1.16B ; 16 字节一把 XOR 解密
    STR   Q0, [X3],#0x10
    ...
tail:                          ; 不足 16 字节的尾部逐字节
    LDRB  W1, [X19,X2]
    EOR   W1, W23, W1
    STRB  W1, [X19,X2]
    ...
```

> SMC 是这个壳最硬的一块：你在 IDA 里看到的永远是加密态，必须**动态跑到那条指令解密的瞬间**才能看清，且看完它马上又加密回去。

### 5.3 自定义 linker：解密 + 填充 + 抹 ELF 头

壳没用系统 linker，而是**自己实现了一套 SO 加载**，全程在 `init_array` 里完成。三步走：

**① 解密字符串表**（简单异或，带递增 key）：

![解密后的字符串表](/images/posts/android/yidun-unpack/decrypt-strtab.jpg)

```
DecSecString:
    MOV   W7, #0x2342 ; MOVK W7, #0x5631,LSL#16   ; key = 0x56312342
loop:
    LDR   W6, [X3,X5]
    EOR   W6, W6, W7                               ; 异或解密
    ADD   W7, W7, W5                               ; key 随偏移递增
    STR   W6, [X3,X5]
    ADD   X5, X5, #4 ; CMP W4,W5 ; B.HI loop
```

**② 解密指令**（前面 5.1 那个 RC4 变体）。

**③ 填充指令 + 抹 ELF 头**：解析 ELF 头拿到各 LOAD 段，`mmap` 映射进内存，`mprotect` 改读写权限，最后**用 `0xBB` 把 ELF 头整个 `memset` 掉**：

```
mmap_so_memcpy_code:
    ...
    BLR   X8            ; mprotect(对齐后的段, RW)
    MOV   W1, #0xBB
    BL    memset_0      ; 用 0xBB 填充/抹掉 ELF 头
    ...
    BLR   X4            ; mprotect(段, 按 p_flags 设置最终权限)
```

> 抹 ELF 头这一手专治"从内存直接反 ELF / dump SO"——你 dump 出来的是一坨没有头、没有节表的残体，想完整还原 SO，得**按 ELF 格式手工重组**一个出来。这是这个壳防 dump 的核心价值所在。

至此 `init_array` 阶段结束，接着才执行 `JNI_OnLoad`。

### 5.4 JNI_OnLoad 定位

代码虽然解密、填充好了，但导出函数还是看不到，所以仍要借系统机制定位 `JNI_OnLoad`。跟 `LoadNativeLibrary` 流程：

```
art::JavaVMExt::LoadNativeLibrary(...)
  └─ art::SharedLibrary::FindSymbolWithoutNativeBridge("JNI_OnLoad")
         → 直接返回 JNI_OnLoad 地址
```

在 `FindSymbolWithoutNativeBridge` 返回处即可拿到真实 `JNI_OnLoad` 地址。

### 5.5 反调试

`JNI_OnLoad` 里铺了几道反调试，常规手法：

- **扫调试器**：`opendir("/proc/")` + `readdir` 遍历进程，查是否有 `android_server`（IDA 远程调试服务）在跑；
- **`fork` 子进程反调试**：`fork` 出子进程，`pthread_create` 起反调试线程（典型的 `ptrace` 互锁 / 自附加思路）。

### 5.6 RegisterNatives 与 DEX 解密

`JNI_OnLoad` 里 `RegisterNatives` 动态注册 native 方法（`X2` 寄存器即 `JNINativeMethod` 数组）：

```c
typedef struct {
    const char* name;       // Java 层 native 方法名
    const char* signature;  // 函数签名
    void*       fnPtr;      // native 实现地址
} JNINativeMethod;
```

**解密 dex 前先查脱壳器**——`access` 探测一票已知脱壳工具 / 模拟器特征：

```
/data/dexname
/data/local/tmp/unpacker.config
/data/fart
/sdcard/fart
/data/local/tmp/libFupk3.so
libFupk3.so
libblackdex.so
libhoudini.so      ; 模拟器（houdini = x86 上跑 ARM）
```

然后从壳 dex 里拷出一段**配置密文**解密，里面是包名、原 Application、各种 key 和开关：

```
c : 5FD6EB80B07CB412704C5A46D899C63B
p : com.xiaopeng.mycarinfo
a : com.xiaopeng.mycarinfo.application.tinker.CarApplication   // 原 App 入口
r : 047AEA142BC032C4
d : 4A7651EA24600BAA                                           // 解密 dex 的 key
m : 1   z : 0   u : 1   ...（一长串行为开关）
```

拿到 dex key 后，解析壳 dex 格式取出加密存放的 DEX，**循环解密 6 个 DEX** 还原出明文：

```
DecDex_sub:
    ...
    BL  memcpy_2              ; 拷贝 DEX 密文
    BL  DecKEY_sub           ; 解密 key（4A7651EA24600BAA）
    BL  initkey_sub          ; 初始化解密器
    BL  Dec_DexData_sub      ; 解出明文 DEX
    ...
```

> **脱壳点 1**：这里解密完，内存里就是**原始明文 DEX**，直接 dump 即可。

### 5.7 加载 DEX

Android 10 是个分水岭：10 之前 `DexClassLoader` 加载的 dex 默认会走 `dex2oat` 优化；10 之后系统**不再**对 `DexClassLoader` 加载的 dex 做 `dex2oat`，运行时只接受系统生成的 OAT。所以壳直接调 art 内部接口加载明文 DEX：

```cpp
std::unique_ptr<DexFile> DexFile::OpenCommon(
        const uint8_t* base, size_t size,
        const std::string& location, uint32_t location_checksum, ...);
```

> **脱壳点 2**：`DexFile::OpenCommon` 是个非常舒服的脱壳点——hook 它，`base`/`size` 就是完整 DEX，直接拿。之后壳构造 `PathDexList`，用其成员 `Element[] dexElements` 指向这些 DEX，DEX 就被挂进了 ClassLoader。

## 六、脱壳二次打包

经过上面的分析，**dump 点其实很多**：内存里解密完那一刻、`DexFile::OpenCommon` 加载时，都能拿到明文 DEX。

把 dump 出来的 6 个 DEX 重新打包进 apk，**入口类改成上面解出来的原 App 入口** `com.xiaopeng.mycarinfo.application.tinker.CarApplication`，加载完所有 DEX 后反射调用原始入口，App 就能正常跑起来：

![重打包反射调用原始入口](/images/posts/android/yidun-unpack/repack-entry.jpg)

## 七、总结

**亮点**：Native 层的 SO 保护比"整体压缩加密"那种壳上了一个台阶——**SMC（运行时自修改）+ 自定义 linker 加载 SO + 抹掉 ELF 头/节信息**，能很好地防止从内存直接 dump ELF。想完整脱出 SO，得按 ELF 格式重新拼一个完整文件出来，防脱壳的安全度确实是有的。

**不足**：Native 层堆了那么多反调试、反脱壳，但**最终 DEX 还是要在内存里以完整明文出现**，这一步绕不过去——只要 DEX 落地成明文，各种脱壳机就很难真正防住。这也是 dex 整体加固的通病：壳保护得再狠，被保护的 DEX 总有"现明文"的一刻。

最后用一张表对比一下这个壳"防得住"和"防不住"的：

| 防护手段 | 目的 | 实际效果 |
|---|---|---|
| 节信息处理 / 导出加密 / 抹 init_array | 反静态分析 | ✅ 静态基本看不了 |
| SMC 运行时加解密 | 反内存 dump 代码 | ✅ 看到的永远是密文 |
| 自定义 linker + 抹 ELF 头 | 防 dump 完整 SO | ✅ dump 出来是残体，要手工重组 |
| 多重反调试（android_server / fork ptrace） | 拦调试器 | ⚠️ 常规手法，可对抗 |
| 脱壳器特征检测（fart/blackdex/Fupk3） | 拦已知脱壳机 | ⚠️ 黑名单式，能绕 |
| DEX 整体加密 | 保护业务代码 | ❌ 内存必现明文 → 可 dump |

一句话收尾：**这个壳真正的功夫全在 SO 的自我保护上，DEX 本身反而是软肋**。对 dex 壳来说，SO 加固做得再厚，也挡不住"内存里那份明文 DEX"——找准 `DexFile::OpenCommon` 这类系统加载点，脱壳就是水到渠成的事。

借这次分析，也顺带把高版本系统的 DEX 加载流程和自定义 linker 的玩法摸了一遍。

> 本文涉及的逆向均用于安全研究与防御对抗学习，请勿用于非法用途。
