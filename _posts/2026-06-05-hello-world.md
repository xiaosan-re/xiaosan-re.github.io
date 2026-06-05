---
title: 博客搭建完成
categories: 杂记
tags: [杂记]
description: 博客的第一篇文章，介绍这里以后会写些什么。
---

这是博客的第一篇文章。以后会在这里记录 Android native 逆向相关的内容：

- so 加固脱壳
- OLLVM / 控制流平坦化反混淆
- Frida hook 与 trace 实战
- IDA / capstone 使用笔记

## 怎么写新文章

在 `_posts/` 目录下新建文件，命名格式为 `年-月-日-英文标题.md`，开头带上 front matter：

```markdown
---
title: 文章标题
categories: Android
tags: [Frida, Android]
---

正文用 Markdown 写……
```

提交推送后，GitHub 会自动重新构建并更新网站。

## 代码高亮示例

```javascript
const base = Module.findBaseAddress("libfoo.so");
Interceptor.attach(base.add(0x1234), {
  onEnter(args) {
    console.log("arg0 =", args[0]);
  },
});
```
