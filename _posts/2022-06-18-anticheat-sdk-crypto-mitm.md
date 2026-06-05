---
title: 某老牌反作弊产品分析：从 VM 加固到一个能被中间人攻击的加密漏洞
categories: Android
tags: [Android, 逆向, 反作弊, 设备指纹, RSA, AES, VMP, 中间人攻击]
keywords: Android 逆向, 反作弊 SDK, 同盾, 设备指纹, blackbox, VMP, RSA 私钥加密, 公钥还原, AES, 中间人攻击, JNI, RegisterNatives, 模拟器检测, 多开检测, Xposed, Magisk
description: 逆向某老牌反作弊 SDK 的设备指纹（blackbox）采集与加密链路：Java/JNI 初始化、自研 VM 引擎、环境检测、设备信息采集，最后还原出它"用 RSA 私钥加密 AES KEY/IV"的设计缺陷——私钥内置且为 PKCS#8，可反推公钥，进而解出全部上报数据，构成可被中间人攻击的加密漏洞。
---

## 一、背景 & 场景

### 1.1 环境

样本：某老牌反作弊 SDK（Android arm64，含 `libtongdun.so` + `libtdvm.so` 双模块）

工具链：IDA Pro 9.2、Frida、JADX、JDK（用标准 `KeyFactory`/`Cipher` 还原算法）

### 1.2 引言

做风控对抗，绕不开"设备指纹"这块硬骨头。厂商在 App 里塞一个 SDK，启动时采集一大堆设备信息、跑一遍环境检测，加密后上报服务器，换回一个 `blackbox` 字符串作为这台设备的唯一身份。我们这边想做的，无非是把这条链路看穿：**采了什么、怎么加密、能不能伪造**。

本文分析的是一款做了多年的老牌反作弊产品。它该有的对抗都有——字符串加密、关键算法塞进自研 VM、AB 双模块拆分逻辑、全套环境检测（多开 / Xposed / Magisk / 模拟器 / 云手机 / 自动点击）。整体看下来加固水平不低。

但最后落到加密协议本身时，它犯了一个**致命的设计错误**：用 **RSA 私钥** 去加密会话用的 AES KEY/IV，而这把私钥**直接内置在 SO 里**，并且是标准 **PKCS#8** 编码。PKCS#8 的私钥结构里同时带着 `n / e / d / p / q`——也就是说，**拿到私钥就等于拿到了公钥**。于是任何人都能从上报报文里把 AES KEY/IV 解出来，再解出全部明文设备数据。传输层的机密性直接归零，构成一个可被中间人攻击的加密漏洞。

下文按"先看清加固、再戳穿加密"的顺序展开：产品框架 → JNI 初始化 → VM 引擎 → 环境检测与信息采集 → 加密流程 → 漏洞还原与 MITM。文中私钥、密钥、指纹明文等敏感数据均做了部分脱敏处理，仅保留结构。

> 本文仅限学习交流与防御研究，请勿用于非法及商业用途。

## 二、产品整体框架

逆向还原出来，整套 SDK 是 **Java 接入层 + 两个 Native 模块** 的结构，职责拆得很清楚：

```
┌──────────────────────── Java 接入层 ────────────────────────┐
│  FMAgent.initWithCallback(ctx, ENV, callback)               │
│     └─ 异步采集 → 加密 → 上报 → 回调 onEvent(blackbox)       │
└────────────────────────────┬───────────────────────────────┘
                             │ JNI
        ┌────────────────────┴────────────────────┐
        ▼                                          ▼
┌─────────────────────┐                ┌───────────────────────┐
│   libtongdun.so      │  调用 VM 解析  │      libtdvm.so        │
│  （业务逻辑 / A 模块）│ ─────────────► │  （VM 引擎 / B 模块）   │
│  环境检测、信息采集   │ ◄───────────── │  解压解密 + 执行字节码  │
│  加密流程编排         │   handle 回调  │  关键算法藏在字节码里   │
└─────────────────────┘                └───────────────────────┘
```

关键的对抗设计在于：**A 模块（`libtongdun.so`）里大量关键方法并不直接实现算法，而是把一段 VM 字节码丢给 B 模块（`libtdvm.so`）解析执行**。想静态看懂某个算法，就得先把 VM 的 handle 一个个还原出来。这是它把逆向成本抬高的主要手段。

逆向还原出的完整产品架构如下：

![产品整体架构](/images/posts/android/anticheat-mitm/architecture.png)

接入方使用上很简单：选对应平台 SDK → 前后端联调 → 运行 App 拿到 `blackbox` 设备指纹。整体对接的数据流程时序如下：

![整体对接数据流程时序图](/images/posts/android/anticheat-mitm/seq-diagram.png)

## 三、JAVA 与 JNI 初始化

### 3.1 入口

接入方在首页 Activity 的 `onCreate` 里调用初始化，传环境标志和回调：

```java
// FMAgent.ENV_SANDBOX     沙盒环境
// FMAgent.ENV_PRODUCTION  生产环境
FMAgent.initWithCallback(this, FMAgent.ENV_PRODUCTION, new FMCallback() {
    @Override
    public void onEvent(String s) {
        tdBlackbox = s;                 // 拿到 blackbox 设备指纹
        Log.e(TAG, "blackbox:" + tdBlackbox);
    }
});
```

### 3.2 加载 SO

初始化里按版本加载两个 SO，先 VM 引擎后业务模块：

```java
if (Build.VERSION.SDK_INT >= 17) {
    System.loadLibrary("tdvm");      // VM 引擎
}
System.loadLibrary("tongdun");       // 业务逻辑
```

### 3.3 JNI_OnLoad 动态注册

Native 方法走 `RegisterNatives` 动态注册——这是反作弊 SO 的标配，目的是让你没法直接靠导出符号定位 native 实现。注册逻辑本身被一层 trampoline 包了一下：

```
RegisterNatives_sub_786D163E4C
    ...
    BLR  X8            ; 实际调用 JNIEnv->RegisterNatives
    RET
```

还原出注册表里的 native 方法签名：

```
getData2        (Ljava/lang/String;)[B
tongdun         (Landroid/content/Context;)V
tongdun2        (Landroid/content/Context;)V
XOnEvent        (Landroid/content/Context;)Ljava/lang/String;
onSensorChanged (Landroid/hardware/SensorManager;Lcn/.../s;Landroid/hardware/SensorEvent;)V
```

`XOnEvent` 就是采集 + 加密 + 返回 blackbox 的主入口，是后面所有分析的锚点。整个 Java/JNI 初始化流程如下：

![JAVA 与 JNI 初始化整体流程](/images/posts/android/anticheat-mitm/jni-init-flow.png)

## 四、VM 虚拟机基本逻辑

### 4.1 两个导出函数

VM 逻辑全在 `libtdvm.so`，模块只导出两个方法：

```
td_eea7e05642c04e240c51   // 解压解密 VM 字节码（VMBycode）
td_b13d6928ba611f6a6e37   // 解析执行 VM 字节码
```

`libtongdun.so` 里绝大多数关键方法，最终都会把一段字节码传给这两个函数解析执行。

### 4.2 取指 → 派发 → 执行

进入 VM 后是一个非常典型的 **取指 / 解码 / 派发** 解释器循环。先取出解压后的字节码：

```
EnterVM_sub_709EA954F0
    ...
    LDR   X28, [X19,#0x28]      ; 解压后字节码基址
    LDR   X23, [X19]
    ADRP  X26, jump_table       ; 派发跳转表
    ...
    B     dsp_loc                ; 取下一条 VMBycode
```

派发处把指令字段抽出来当 switch 的 case 索引，跳到对应 handle：

```
dsp_loc:
    LDR    W21, [X8,X28]        ; 取一条 VMBycode
    UBFX   W8, W21, #0x19, #4   ; 抽出 opcode 字段
    SUB    W8, W8, #4           ; 归一化成 switch index
    CMP    W8, #0xB
    B.HI   default
    LDRSW  X8, [X26,X8,LSL#2]   ; 查跳转表
    ADD    X8, X8, X26
    BR     X8                   ; switch 跳转
```

### 4.3 常见 Handle

算术类 handle 都是教科书式的"取两个操作数、算、写回"，几条指令一个：

```
ADD_sub:  ADD W8, W3, W2 ; STR W8, [X1] ; RET
SUB_sub:  SUB W8, W2, W3 ; STR W8, [X1] ; RET
AND_sub:  AND W8, W3, W2 ; STR W8, [X1] ; RET
EOR_sub:  EOR W8, W3, W2 ; STR W8, [X1] ; RET
MUL_sub:  MUL W8, W3, W2 ; STR W8, [X1] ; RET
```

加上一个取 VM 内部数据栈值的 `getdatabas` handle（按编号 `0x1D/0x1E/0x1F...` 取不同槽位的基址），加密时这些算术 + 取值 handle 来回组合，就把算法"摊"进了字节码里。

### 4.4 跳出 VM 调其它模块的 Handle

最关键的是这个**回调 handle**——它负责从 VM 里跳回 `libtongdun.so` 执行 native 逻辑：

```
call_loc_709EA936C0
    SUB   SP, SP, #0x80
    BLR   X9            ; 调用 tongdun.so 中的 native 函数
    ADD   SP, SP, #0x80
    ...
```

> **分析技巧**：如果只想理清整体流程、不打算还原算法，调试时盯死这一个 handle 就够了——所有"VM → 业务模块"的调用都从这里出去，等于拿到了 VM 的对外调用日志。真要还原算法，才需要逐个 handle 啃。

## 五、环境检测与设备信息采集

### 5.1 随机 ID：AES 加密 + 钉子文件

首次运行时如果本地没有随机数 ID，就生成一个，用 AES 加密后**写到三个位置**互为备份（俗称"钉子文件"，删一个还能从另两个恢复），而且**三处用不同的 AES KEY**：

```
SharedPreferences  td-client-id-3
/data/user/0/<包名>/files/.td-3
/storage/emulated/0/.td-3

# 三处分别用的 KEY
bs3ggr0ismnzmdwxkacrq88xs9uj3l06
ykj314o0nd8423k2cimo5fvx0k234sc5
phx7ryl7sjppatga3nfl1caircw6ct79
```

AES 这一步是 native 里反射调用 Java 的 `Cipher` 实现的（`AES/ECB/PKCS5Padding`），native 侧负责把 key 字节逐个 `STRB` 拼出来再 `NewStringUTF` 传给 Java——典型的"算法在 Java、调度在 native，字符串全程不落地"写法。

### 5.2 环境检测一览

检测部分覆盖得很全，手法基本是"特征字符串 + `access`/读 `maps`/`__system_property_get`/反射 `loadClass`"几板斧。整理成表：

| 检测项 | 手法 | 部分特征 |
|---|---|---|
| **多开** | 读 `/proc/self/maps` 找包名特征 | `com.lody.virtual`、`com.lbe.parallel`、`com.qihoo.magic`、`io.va.exposed`、`com.excelliance.dualaid` … |
| **Xposed** | 反射 `getSystemClassLoader().loadClass` + 扫进程 | `de/robv/android/xposed/XposedBridge`、`libxposed_art.so`、`com.saurik.substrate` |
| **Magisk** | `df/mount/ps` + `strstr` 找特征 | `/sbin/.magisk` |
| **自动点击** | `access` 探测路径 | `net.aisence.Touchelper`、`com.cyjh.mobileanjian`、`com.touchsprite.android` … |
| **模拟器** | `access` 文件 + 比包名 + `__system_property_get` | `/dev/qemu_pipe`、`/dev/socket/genyd`、`/system/bin/nox-props`、`init.svc.droid4x`、`qemu.sf.lcd_density`、`bluestacks` 系列包名 … |
| **云手机** | `access` + `getPackageInfo` 比包名 | `com.haimawan.cloudappstore`、`com.baidu.mtc.ysera`、`cn.testin.itestin` … |

值得一提的是多开检测那段被做了**控制流混淆 + 字符串加密**：包名特征是运行时一个字节一个字节 `STRB` 拼出来的，再逐字符做半字节交换（`LSR #4` + `BFI`）解密，最后 `strstr` 去 `maps` 里匹配。静态看是一坨散乱的常量比较，动态下断点才看得清。

### 5.3 设备信息采集

采集走的是一张**"类名 + 方法名"表**，native 里用 `forName / getDeclaredMethod / invoke` 双重反射逐项调用。表的内存布局很规整（每项固定槽位、`类名\0...方法名\0`）：

```
android.telephony.TelephonyManager  ->  getDeviceId
                                         getVoiceMailNumber
                                         getSimSerialNumber
                                         getNetworkCountryIso
                                         getNetworkOperatorName
                                         getSimOperatorName
                                         getPhoneType
                                         getNetworkType
                                         getCellLocation
                                         getDeviceSoftwareVersion
android.net.wifi.WifiInfo            ->  getMacAddress / getIpAddress / getSSID / getBSSID
android.net.wifi.WifiManager         ->  getConnectionInfo / getDhcpInfo / getScanResults
java.net.NetworkInterface            ->  getNetworkInterfaces
android.net.Proxy                    ->  getHost
...
```

双重反射的核心调用链（已脱去 trampoline）：

```
GetStaticMethodID(... "forName" ...)        ; Class.forName(类名)
NewStringUTF(方法名)
GetMethodID(... "getDeclaredMethod" ...)     ; clazz.getDeclaredMethod(方法名)
CallObjectMethod(...)                        ; method.invoke() 取设备信息
```

> 要补全"还有哪些设备信息"，不用逐行读汇编——在 4.4 那个"跳出 VM"的 handle 上下断点，把每次回调的类名/方法名打出来即可，几次运行就能拉全清单。

### 5.4 每采一项，VM 加密一次

采集不是攒齐再加密，而是**每取到一项设备信息就进 VM 跑一次对应 handle 加密**。这也是为什么前面要先把 VM 引擎看明白——加密散落在字节码里，逐项进行。

## 六、加密流程分析

把采集到的设备数据变成最终上报的报文，整条加密链是这样：

```
设备信息(JSON) ──► ① MurmurHash2 校验 + 组合 ──► ② 压缩
                                                      │
        ┌─────────────────────────────────────────────┘
        ▼
   ③ 随机生成 AES KEY/IV ──► ④ AES 加密压缩后的设备数据 ─┐
        │                                               │
        └─► ⑤ 用内置 RSA【私钥】加密 KEY/IV ─┐           │
                                            ▼           ▼
                              ⑥ 组合 [RSA(KEY/IV)] + [AES(设备数据)] ──► POST 上报
```

### 6.1 压缩前的校验与组合

压缩前先算一道校验值跟设备数据组合。文中标的是"CRC"，但看常量 `0x5BD1E995` 就知道这其实是 **MurmurHash2**（这个乘子是 MurmurHash2 的标志性 `m`）：

```
EncData_sub:
    MOV   W8, #0x5BD1E995     ; MurmurHash2 magic
    EOR   W9, W2, W1
    ...
    MUL   W9, W9, W8
    EOR   W14, W14, W14,LSR#24
    ...
```

### 6.2 随机生成 AES KEY/IV

KEY/IV 来自 `gettimeofday` 播种的 `srand` + `sprintf` 拼接，是**每次会话动态生成**的：

```
gettimeofday → srand → sprintf

// 本次抓到的明文 KEY/IV（脱敏保留结构）
KEY = fda958f6-07e5-47
IV  = e4ae2f7b-96b5-4a
```

### 6.3 用 RSA 私钥加密 AES KEY/IV（问题就在这）

把 KEY 和 IV 拼成一个字符串 `fda958f6-07e5-47e4ae2f7b-96b5-4a`，然后——**用 RSA 私钥对它加密**。这把私钥**硬编码在 SO 里**，PKCS#8 base64（已隐去部分）：

```
MIICeAIBADANBgkqhkiG9w0BAQEFAASCAmIwggJeAgEAAoGBALE2OfQ8BYg9Lq4n
KGTamXyia6raCc1adzCOsFrnk/VN0s2W8yQJfYdq+QUNRHv0zANW0Uafh7nHCWeBn
... （中间略） ...
8POECQQDPcNOysMNbrLh1mGe6ydUsojSbheAIOZPQ/lhUbhzPXAXTYaPkTq7uty6S
YZOMtWLxIFZ1eA9HHm3tJOCgC888
```

加密同样是反射调 Java：`getInstance` → `generatePrivate` → `doFinal`。加密后的 KEY/IV 密文（128 字节，RSA-1024）：

```
98 93 1B 85 66 82 76 26  88 2B 09 13 AA 22 4E 76
9B 3F 47 93 8B A7 CD D7  A6 48 3D C9 70 55 29 6A
57 B7 65 AE F4 3E 2C CB  5C E1 CD 6B 57 B5 86 2F
1D 81 FC A3 56 27 64 13  27 42 A0 84 C3 23 CD 0D
05 D1 0D B0 22 36 FE 36  B5 17 61 6F 19 14 1D B1
67 A0 1F F4 F2 09 83 CA  C1 9A C4 64 14 F4 54 7D
DA ...
```

> 这里就埋下了第七节要戳穿的雷：**"用私钥加密"在密码学上等价于签名**，它保证的是"来源可信"，而**不是机密性**——因为对应的公钥按定义是公开可得的。把会话密钥用私钥加密，等于用一把人人能拿到的钥匙去锁机密数据。

### 6.4 AES 加密压缩后的设备数据

再用 6.2 那把随机 AES KEY 加密压缩后的设备数据。AES 实现是 native 自带的标准实现（key 扩展、查表都在），反编译出来就是教科书 AES：

```c
// X0:key, X1:位长(128/192/256), X2:输出轮密钥
__int64 AES_initkey(unsigned int *key, int bits, unsigned int *rk) {
    if (bits != 128 && bits != 256 && bits != 192) return -2;
    rk[60] = (bits==128)?10 : (bits==192)?12 : 14;   // 轮数
    rk[0] = __builtin_bswap32(key[0]);
    rk[1] = __builtin_bswap32(key[1]);
    rk[2] = __builtin_bswap32(key[2]);
    rk[3] = __builtin_bswap32(key[3]);
    // ... 标准 Rijndael key expansion（Te/Rcon 查表）...
}
```

### 6.5 组合上报

最后把两段拼起来——`[RSA 私钥加密的 AES KEY/IV] + [AES 加密的设备数据]`——POST 给服务器。组合后的报文格式如下（前 128 字节高亮部分是 RSA 加密的 AES KEY/IV，其后是 AES 加密的设备数据）：

![上报报文组合格式](/images/posts/android/anticheat-mitm/report-format.jpg)



```
url: https://fp.fraudmetrix.cn/android3_5/profile.json?partner=xxx&version=3.6.7&clientSeqId=1654331726915998700
```

上报走反射调用 `cn/tongdun/android/shell/common/HttpHelper`，可选 `trustSSL` / 自定义 `HostnameVerifier`，并从响应 `Set-Cookie` 里抠出 `XXID` 存为会话 id：

```java
private static String connect(URL url, byte[] body, ...) {
    HttpsURLConnection conn = (HttpsURLConnection) url.openConnection(Proxy.NO_PROXY);
    // arg12==1: trustSSL  arg12==2: 自定义 HostnameVerifier
    conn.setRequestMethod("POST");
    conn.getOutputStream().write(body);
    // ... 从 Set-Cookie 解析 XXID → FMAgent.xxid ...
    // 读取响应体（blackbox）
}
```

服务器成功后返回 blackbox：

```json
{"code":"000","desc":"k9OCtUBncUi1/r3N84z30FFW3AwxnmZnJfuKa2bhCcS/s9mKZAuBFnJ6BYRDDpUkz+fxJhWvD+bbun3eUbCyiw=="}
```

这个值由硬件 ID、OAID、文件 ID 等生成，就是这台设备最终的指纹。

## 七、加密漏洞还原与中间人攻击

铺垫完整条加密链，现在戳穿它。目标很明确：**不依赖任何运行时 hook，仅凭 SO 里内置的那把私钥 + 一份抓到的上报报文，把全部明文设备数据解出来**。能做到，就证明这套传输加密形同虚设、可被中间人完全解读乃至伪造。

### 7.1 从私钥反推公钥

直觉上"只有私钥（d, n）很难推出公钥"——但前提是私钥只含 `d` 和 `n`。本 SDK 的私钥是 **PKCS#8 / PKCS#1 `RSAPrivateKey`** 结构，它**完整携带了密钥对的全部参数**：

```
RSAPrivateKey ::= SEQUENCE {
    version           Version,
    modulus           INTEGER,  -- n
    publicExponent    INTEGER,  -- e   ← 公钥指数就在里面！
    privateExponent   INTEGER,  -- d
    prime1            INTEGER,  -- p
    prime2            INTEGER,  -- q
    exponent1         INTEGER,  -- d mod (p-1)
    exponent2         INTEGER,  -- d mod (q-1)
    coefficient       INTEGER   -- (q^-1) mod p
}
```

公钥就是 `(n, e)`，而 `n` 和 `e`(通常 `0x10001`) 都明明白白躺在私钥结构里。所以**只要私钥泄露，公钥就是免费送的**。用标准 JCE 加载私钥、取 `modulus`、配上 `e=010001` 重建公钥：

```java
// 1. 加载内置私钥（PKCS#8）
byte[] buffer = Base64.getDecoder().decode(strPrivateKey);
PKCS8EncodedKeySpec keySpec = new PKCS8EncodedKeySpec(buffer);
RSAPrivateKey privateKey = (RSAPrivateKey)
        KeyFactory.getInstance("RSA").generatePrivate(keySpec);

// 2. 从私钥里取 modulus，配 e=0x10001 重建公钥
BigInteger modulus = privateKey.getModulus();
String modulusHex = bytesToHex(modulus.toByteArray());
RSAPublicKeySpec pubSpec = new RSAPublicKeySpec(
        new BigInteger(modulusHex, 16),
        new BigInteger("010001", 16));
RSAPublicKey publicKey = (RSAPublicKey)
        KeyFactory.getInstance("RSA").generatePublic(pubSpec);
String publicKeyString = Base64.encode(publicKey.getEncoded());
```

### 7.2 用公钥解出 AES KEY/IV

私钥加密 ↔ 公钥解密。既然公钥已经到手，就拿它去解上报报文里那 128 字节的 RSA 密文（6.3 抓到的那段）：

```java
// keydata = 6.3 抓到的 128 字节 RSA 密文
String aeskey = new String(RSAUtils.publicKeyDecrypt(publicKeyString, keydata));
System.out.println("aeskey: " + aeskey);

// 解出：前 16 字节是 AES KEY，后 16 字节是 IV
// => fda958f6-07e5-47e4ae2f7b-96b5-4a
```

解出来的正是 6.2 那把随机 KEY/IV——**整个会话密钥协商被攻破**。

### 7.3 用 AES 解出设备数据

有了 KEY/IV，最后一步就是标准 AES 解密上报报文里的设备数据段：

```java
// 前 16 字节是 KEY，后 16 字节是 IV
String key = "fda958f6-07e5-47";
String iv  = "e4ae2f7b-96b5-4a";

public static byte[] aesDecrypt(String filePath, String key, String iv) {
    byte[] enc = FileUtils.getContent(filePath);          // 上报报文中的 AES 段
    Cipher cipher = Cipher.getInstance(ALGORITHMS);        // AES/CBC/...
    cipher.init(Cipher.DECRYPT_MODE,
            new SecretKeySpec(key.getBytes(), "AES"),
            new IvParameterSpec(iv.getBytes(StandardCharsets.UTF_8)));
    return cipher.doFinal(enc);                            // 解出压缩后的设备数据
}
```

到这里，**仅凭一把内置私钥 + 一份抓包，无需在目标进程里 hook 任何东西，就把上报的设备数据完整解了出来**。这意味着位于网络中间的攻击者可以读取、甚至篡改/伪造任意设备的指纹上报——这就是标题里说的"可被中间人攻击的加密漏洞"。

> AES 解出来的还是**压缩后**的数据，再往里还有一层 VM 内的处理需要还原大部分 handle。好在 VM 字节码没做强混淆，分析起来不难——这部分留个坑，给想深入的同学。

## 八、总结

把这款产品拆完，几点观感：

**业务**：老牌厂商，近几年从营销 / 渠道反作弊转向金融安全。产品包体偏大、模块偏多，体验、稳定性、易用性上能感觉到历史包袱。

**代码**：架构不够精简，代码冗余明显；很多空数据加密时不做判空，空数据也照样进 VM 引擎跑一遍，白白吃性能。把部分关键算法塞进 VM 这一手，对抗逆向确实有效。

**安全**：整体加固能力是在线的——字符串加密、AB 双模块拆分、关键逻辑下沉到 VM，逆向成本被实实在在抬高了。但**一个致命短板抵消了大半努力：用内置的 RSA 私钥去加密会话密钥**。私钥本就不该出现在客户端，更何况它是带全参数的 PKCS#8、公钥可直接反推。结果就是采集、检测、VM 加密一整套重武器堆下来，传输层却被一行 `getModulus()` 撕开口子。

最后用一张表对比一下"它以为的防护"和"实际的防护"：

| 环节 | 设计意图 | 实际效果 |
|---|---|---|
| 关键算法塞进 VM | 抬高静态逆向成本 | ✅ 有效，确实费劲 |
| 字符串加密 / 多开 / 模拟器检测 | 拦截改机、群控、自动化 | ✅ 覆盖全面 |
| AB 双模块拆分 | 打断分析链路 | ✅ 增加跳转复杂度 |
| RSA【私钥】加密 AES KEY/IV | 保护传输机密性 | ❌ 私钥内置 + PKCS#8 → 公钥可推 → 全量可解 |

一句话收尾：**加固做得再厚，密钥学基础错了就是地基塌方**。对客户端来说，凡是"想保密的东西却把解密能力一并打包进 App"，本质上都等于没加密——RSA 用私钥加密、对称密钥硬编码、都是同一类错误的不同写法。

> 本文涉及的逆向均用于安全研究与防御对抗学习，相关私钥 / 密钥 / 指纹明文均已脱敏，请勿用于非法用途。
