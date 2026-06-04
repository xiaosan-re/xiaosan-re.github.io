---
layout: page
title: About
description: 关于小三
keywords: 小三, xiaosan-re, Android 逆向
comments: false
menu: 关于
permalink: /about/
---

我是小三，一名 Android 逆向工程师。

主要方向：native so 脱壳、反混淆，常用 Frida、IDA、capstone。

这个博客用来记录逆向实战中的笔记与踩坑。

## 联系

<ul>
{% for website in site.data.social %}
<li>{{website.sitename }}：<a href="{{ website.url }}" target="_blank">@{{ website.name }}</a></li>
{% endfor %}
</ul>

## 公众号

欢迎关注我的微信公众号「矛和盾的故事」：

<img style="height:192px;width:192px;border:1px solid lightgrey;" src="{{ site.url }}/assets/images/qrcode.jpg" alt="矛和盾的故事" />

## Skill Keywords

{% for skill in site.data.skills %}
### {{ skill.name }}
<div class="btn-inline">
{% for keyword in skill.keywords %}
<button class="btn btn-outline" type="button">{{ keyword }}</button>
{% endfor %}
</div>
{% endfor %}
