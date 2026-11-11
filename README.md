# 闪动校园传感器仿真

纯 native Zygisk 模块，伪造加速度传感器数据，供闪动校园自研计步算法使用。

> 关键词：`闪动校园` `Zygisk` `传感器` `步频` `hook` `易盾绕过`

## 参考

波形参数来自 [FUCK-RUN-](https://github.com/BieFan1029/FUCK-RUN-)。Zygisk API 来自 [zygisk-module-sample](https://github.com/topjohnwu/zygisk-module-sample)。

## 说明

闪动校园新版集成易盾加固，旧 LSPosed 方案会被检测退出。改用 native Zygisk 注入，hook `libsensor.so` 的 `read` 方法改写加速度数据。

## 要求

- KernelSU / KSU Ultra + Zygisk
- Android 14+
- arm64-v8a

## 安装

从 [Releases](https://github.com/gt-hgh/stepspoof_joyrun/releases) 下载 zip，在 KernelSU Manager 中本地安装，重启。

## 使用

KernelSU Manager → 模块 → 打开 WebUI。

设置步频区间（如 172–182 spm），点击开始仿真，随后正常跑步即可。

## 日志

模块运行日志同时写入 logcat 和文件，默认路径：

```
/sdcard/Download/stepspoof_joyrun.log
```

手机文件管理器打开「下载」目录即可取到，不需要电脑。超过 2MB 自动轮转为 `.old`。`/sdcard` 不可写时降级到 `/data/data/com.huachenjie.shandong_school/files/stepspoof.log`。

WebUI 底部「运行日志」卡片显示实际路径与大小，提供三个按钮：打标记（在日志里插一行方便定位）、复制日志（最近 200KB）、清空。

排障流程：点「打标记」→ 复现问题 → 点「复制日志」或直接取文件。

需要电脑时：

```bash
adb logcat -s stepspoof              # 实时看
adb pull /sdcard/Download/stepspoof_joyrun.log
```

## 兼容

| App | 包名 | 状态 |
|---|---|---|
| 闪动校园 | `com.huachenjie.shandong_school` | ✅ |

## 更新

- **v1.2.0** 新增运行日志落盘（默认下载目录）；WebUI 加日志卡片
- **v1.1.8** 整理代码
- **v1.1.7** 波形替换；新增步长与配速
- **v1.1.5** 多槽位 hook；修复 WebUI 混合内容拦截
- **v1.1.0** 独立 WebUI；修复 BOM 目录名
- **v1.0.0** 初版
