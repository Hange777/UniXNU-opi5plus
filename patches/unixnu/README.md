# patches/unixnu — 我们对上游 UniXNU 的改动

这些补丁**不是**设备驱动或板级适配，而是对 UniXNU 自身缺陷的修正。
每个补丁都应当可以直接 `git am` 到对应 pin 的上游 UniXNU 树上。

**上游 pin**：见 `../unixnu.lock` 的 `xnu`/`unixnu` 条目与 `repos/PINS.md`。
本目录的补丁基于 `04b4e558bcded5213c2d802984862dc062f28257`。

## 0001-unixnu-connect-storage-and-probe-config-volumes.patch

### 修的问题

从 UEFI **Boot Manager 手工选择 UniXNU** 启动时，`Host` 判定为
`UniXNUHostApplication`（因为 `LoadOptions` 为空，而 `UniXNU.c:31-37` 要求
它精确等于 `"unixnu:firmware"`）。而 `Main.c` 原本写的是

```c
if (Host == UniXNUHostFirmware) {
  UniXNUConnectAll ();
}
UniXNUConfigLoad (...);
```

⇒ 手工启动路径**永远不会调用 `ConnectAll()`** ⇒ `RkSdmmcDxe` / `DwMmcHcDxe`
这些驱动模型驱动从未被 `ConnectController` 触发 ⇒ 没有
`EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` ⇒ 配置卷扫描扫不到任何普通磁盘 ⇒
回落内置配置（4 项：Return/Restart/Power off/Tools），且**屏幕上没有任何线索
说明为什么**。

在只有 `esp.img` 内嵌配置的镜像上，这个 bug 被 RAM disk 掩盖了（RAM disk 自带
SimpleFileSystem，不依赖 ConnectAll）；一旦按 M1-02 的设计删掉内嵌配置、
把 `unixnu.toml` 放到 SD 卡 ext4 上，问题立刻暴露。

### 怎么修

1. **`Main.c`**：无条件调用 `UniXNUConnectAll()`。
   `ConnectController` 对已连接的 handle 是幂等的，代价可忽略。
2. **`Config.c`**：加 `UNIXNU_CONFIG_PROBE`（默认 1）编译期开关，把
   *扫到几个 SimpleFileSystem 卷* 和 *每个卷打开配置路径的 `EFI_STATUS`*
   写进 `Config->Notice`，由 `Ui.c:532`/`Ui.c:803` 显示在菜单状态行上。
   没有串口的板子只能靠这一行定位。
   同时修掉一处覆盖：`AddFallbackLoaders() == 0` 时原本会用
   `AsciiStrCpyS(..., "no boot entry found")` **整段覆盖** `Notice`，
   导致探测行永远看不到；现在改为追加。

### 上游跟进

- 症状可复现于任何"从 Boot Manager 手选、配置只在普通磁盘上"的 UEFI 板。
- 建议把 1（ConnectAll 无条件）提给上游；2（探测输出）可作为可选的
  debug 选项，或改成受 PCD 控制。

### 验证

- QEMU：`tools/wsl/121-ext4-qemu-test.sh`（AAVMF + 同一份 `Ext4Dxe.efi` +
  64 MiB ext4 盘 + `startup.nsh` 里 `load Ext4Dxe.efi` 再跑 `UniXNU.efi`）。
- 真板：M1-03/05 刷 `…-connectconfig-spifirst.img`，HDMI 上应看到
  `config: \EFI\UniXNU\unixnu.toml` 而不是 `built-in configuration`，
  菜单出现 `xnu` 项。

### 回滚

```bash
cd repos/UniXNU && git checkout -- package/UniXNUPkg/Library/UniXNULib/{Main.c,Config.c}
```
固件回滚见 `docs/tasks/M1-04-maskrom-recovery.md`。
