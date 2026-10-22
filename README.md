# ABK First-Stage AVB Bypass

ABK（AnyBase Kernel）外部模块。在 `after_patch` 阶段把「首阶段 AVB 放行」补进内核源码，
让你用 ABK 编译出的内核能接受**保留原版 vbmeta（Flags=0，校验开启）**的第三方 ROM。

**只做 AVB，不碰 SELinux。**

## 两个独立开关

| Kconfig | 作用 | 默认 |
|---|---|---|
| `CONFIG_ANDROID_DSU_AVB_BYPASS` | 让 PID 1 从 `/proc/bootconfig` 读到 `verifiedbootstate="orange"` | 由模块打开 |
| `CONFIG_ANDROID_DSU_AVB_VBMETA_VIEW` | 把 PID 1 读到的 AVB 头强行标成 `VERIFICATION_DISABLED` | 由模块打开 |

两者可以单独关闭。如果第二个在你的内核上编译不过，把它设成 `n`，第一个仍然有效。

## 为什么需要第二个开关

```
磁盘上的 vbmeta（原版）  Flags=0  →  libavb 会验签、验哈希
本地编译的 boot.img               →  签名对不上 → 校验失败 → 不开机
```

`CONFIG_ANDROID_DSU_AVB_VBMETA_VIEW` 让内核把 PID 1 **读到的** AVB 头改成
`VERIFICATION_DISABLED`，libavb 于是跳过全部校验。**磁盘上的 vbmeta 一个字节不改。**

配合第一个开关（orange），fs_mgr 不会因为「green + 校验关闭」自相矛盾而拒绝。

## 实现方式

```
1. kprobe 挂在 vfs_read 上
2. PID 1 且处于 first-stage 时，若在读块设备 → 把该 file 的 f_op 换成代理
3. 代理在 read / read_iter 返回后检查缓冲区：
     开头是 "AVB0" → 把 offset 120 处的 big-endian u32 flags |= 2
4. PID 1 exec /system/bin/init 时：注销 kprobe、把所有 f_op 还原
```

AVB 头的 flags 在 **偏移 120、大端 u32**（不是很多人以为的 104）。

## 改了哪些文件

| 文件 | 改动 |
|---|---|
| `fs/proc/bootconfig.c` | 插入 orange 输出 |
| `drivers/android/dsu_avb.c` | 新增：PID 1 判定 + exec 门控 |
| `drivers/android/dsu_avb_view.c` | 新增：vbmeta 读取视图改写 |
| `include/linux/android_dsu_avb.h` | 新增 |
| `drivers/android/Kconfig` | 新增两个 config |
| `drivers/android/Makefile` | 新增两条 obj |
| `DEFCONFIG` | 写入两个 `=y` |

## ABK 用法

```text
https://github.com/<you>/abk-dsu-avb-bypass.git;after_patch|https://github.com/<you>/abk-dsu-avb-bypass.git;before_build
```

## 构建后验证

```bash
adb shell dmesg | grep android-dsu-avb
# android-dsu-avb: first-stage orange view enabled
# android-dsu-avb: first-stage vbmeta read view armed
# android-dsu-avb: first-stage PID 1 vbmeta read view marked verification-disabled
# android-dsu-avb: first-stage AVB read-view window closed (N header(s) patched)
# android-dsu-avb: first-stage orange view closed

adb shell getprop ro.boot.verifiedbootstate    # 期望 green
```

判定标准：

- 出现 `marked verification-disabled` → 读取视图确实生效
- `N header(s) patched` 里 N ≥ 1 → 至少改到过一个 AVB 头
- `getprop` 仍是 `green` → 属性层没被污染

## 已知风险

- `dsu_avb_view.c` **没有在本机编译验证过**（没有 ARM64 工具链）。它是按 6.6 的
  `struct iov_iter` / `file_operations` 写的，5.10/5.15 可能需要微调。
- 若编译报错，把 `CONFIG_ANDROID_DSU_AVB_VBMETA_VIEW` 设为 `n`，只保留 orange。
- f_op 代理只在 PID 1 的 first-stage 窗口内存在，second-stage 会全部还原。

## 与 CyberKernel 的差别

| | CyberKernel | 本模块 |
|---|---|---|
| orange 注入 | ✅ | ✅ |
| vbmeta 读取视图改写 | ✅ | ✅ |
| SELinux permissive | ✅ | ❌ |
| DSU 标记门控 | ✅ | ❌（每次启动都生效） |
