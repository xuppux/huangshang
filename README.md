# 黄山派手表固件

立创·黄山派（SiFli SF32LB52-ULP）手表固件，LVGL v8 UI + BLE 通信。

## 这个仓库里有什么

| 目录 | 内容 |
|------|------|
| `watch/lvgl/watch/` | **主工程**：表盘、GUI 应用、BLE 通信、电源管理 |
| `watch/adc/` `gpio/` `I2C/` `uart/` `ws2812/` | 外设驱动示例与验证代码 |
| `watch/RT-Device/` | 传感器驱动 |
| `watch/watchface-tools/` | 表盘生成工具链（Python） |
| `watch/ui-preview/` | UI 预览页（浏览器直接打开） |
| `docs/` | 联调记录（脱敏版） |
| 根目录 | 工程说明、硬件实现清单、功能可行性分析 |

## ⚠️ 仓库不含 sifli-sdk（重要）

`watch/` 能编译的前提是同级有 `sifli-sdk/` 目录，**它不在本仓库里**（860MB 第三方代码）。

### 灾难恢复 / 换设备时怎么重建

```powershell
# 1. 克隆本仓库
git clone https://github.com/xuppux/huangshang.git
cd huangshang

# 2. 拉取 SDK（必须带 --recursive，它有子模块）
git clone --recursive https://gitee.com/SiFli/sifli-sdk.git

# 3. 把 SDK 放到与 watch/ 同级的位置，最终结构应为：
#    huangshang\
#      sifli-sdk\      ← 步骤 2 得到的
#      watch\          ← 本仓库内容
#
# 4. 编译
cd watch
.\build.ps1
```

**从 Gitee 拉而不是 GitHub**：SDK 托管在 Gitee，国内直连速度快，且这是 SiFli 官方仓库。

## 编译产物不在仓库里

`.gitignore` 排除了 `main.elf` / `main.hex` / `*.o` / `build_*` 等。这些**都能重新编译生成**，不需要备份。

需要历史版本的固件二进制时，用 git tag 找回对应源码再编译：

```powershell
git tag                    # 查看所有版本标签
git checkout v0.2.0        # 切到那个确切版本
.\build.ps1                # 重新编译
```

## 硬件事实备忘

- **RTC 无备份电池**：拔电和 sftool 烧录都会归零，只有 reboot 保持。根治靠 BLE 校时。
- **触摸**：FT6146 电容屏，挂 I2C1，中断 PA41。
- **BLE 校时**是根治 RTC 问题的方向，待实现。

## 敏感信息处理

本仓库为 **Public（公开）**，因此：

- 所有 `_*` 开头的串口调试日志**未纳入**版本管理（实测含 11 处设备 MAC、4 处 IP）
- `docs/` 下是脱敏版联调记录，MAC 后 3 字节已打码
- `watch/蓝牙通道自检结果.txt` 因包含**周围他人设备**的 MAC 地址，未上传，原件仅存本地

需要查看完整调试记录时，本地 `watch\` 目录里保留有原始文件。
