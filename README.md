# ABK First-Stage Orange Bootconfig

ABK（AnyBase Kernel）外部模块。在 `after_patch` 阶段把「首阶段 orange 视图」补进内核源码，
让你用 ABK 直接编出一个能放行 `verification-disabled vbmeta` 的 `boot.img`。

补丁来源与行为对齐 AOSP 上游提交
[82d06ef](https://github.com/Content-Creation-Club/android_kernel_xiaomi_dash/commit/82d06eff8f8a5a1fdd74ec4d559f5ee02b331297)。

## 它解决什么问题

伪回锁 LK 报 `verifiedbootstate=green`（锁定），而第三方 ROM 的 vbmeta 是「校验关闭」的。
first-stage init 看到

```
green + HashtreeDisabled  →  自相矛盾，拒绝挂载
```

于是卡开机。把这一处读取改成 `orange`：

```
orange + HashtreeDisabled →  合理，放行
```

然后 PID 1 第一次 `exec /system/bin/init` 时立刻关闭，之后所有读取仍然是真实的 `green`。

## 改了哪些文件

| 文件 | 改动 |
|---|---|
| `fs/proc/bootconfig.c` | 在 `boot_config_proc_show()` 开头插入 orange 输出 |
| `drivers/android/dsu_avb.c` | **新增**，PID 1 + exec 门控 |
| `include/linux/android_dsu_avb.h` | **新增**，`CONFIG_ANDROID_DSU_AVB_BYPASS` 空实现回退 |
| `drivers/android/Kconfig` | **新增** `config ANDROID_DSU_AVB_BYPASS` |
| `drivers/android/Makefile` | **新增** `obj-$(CONFIG_ANDROID_DSU_AVB_BYPASS) += dsu_avb.o` |
| `DEFCONFIG` | 写入 `CONFIG_ANDROID_DSU_AVB_BYPASS=y` |

**不改**：vbmeta、SELinux、dm-verity 表、DSU 检测逻辑。

## ABK 用法

GitHub Actions 外部模块字符串（两个阶段都要）：

```text
https://github.com/<you>/abk-dsu-avb-bypass.git;after_patch|https://github.com/<you>/abk-dsu-avb-bypass.git;before_build
```

ABK App：添加一次选 `after_patch`，再添加一次选 `before_build`。

## 验证

构建完成后：

```bash
# 1) 内核里应该有这条启动日志
adb shell dmesg | grep android-dsu-avb
# android-dsu-avb: first-stage orange view enabled
# android-dsu-avb: first-stage orange view closed

# 2) 属性层必须仍然是 green（这是「之后保持严格」的关键）
adb shell getprop ro.boot.verifiedbootstate
```

如果第 2 步出来是 `orange`，说明 orange 泄漏到了属性层，需要改成只在
`fs_mgr` 那次读取时替换——把 `dmesg` 和 `getprop` 结果发出来再调整。

## 本地自检

不需要内核也能验证补丁逻辑：

```bash
KERNEL_ROOT=/path/to/kernel DEFCONFIG=/path/to/gki_defconfig \
  python3 scripts/abk_dsu_avb.py --stage after_patch

KERNEL_ROOT=/path/to/kernel DEFCONFIG=/path/to/gki_defconfig \
  python3 scripts/abk_dsu_avb.py --stage before_build
```

`after_patch` 幂等：重复执行不会重复插入。

## 依赖

- `python3`（ABK/CI 环境自带）
- 内核需开启 `BOOT_CONFIG`、`PROC_FS`、`TRACEPOINTS`（GKI 默认满足）

## 与 DSU-Permissive 的区别

| | DSU-Permissive (KO) | 本模块 |
|---|---|---|
| 载体 | ramdisk 里的 `.ko` | 编进内核 |
| 是否需要改 init_boot | 是 | **否** |
| 是否改 vbmeta flags | 是 | **否** |
| 是否强制 SELinux permissive | 可选 | **否** |
| 是否只在 DSU 生效 | 是 | 否（普通启动也生效） |

只改「看到的开机状态」，不动任何证据，所以不会出现状态自相矛盾导致的卡死。
