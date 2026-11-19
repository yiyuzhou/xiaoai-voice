# xiaoai-voice · LX06 Windows 安装指南

此版本面向小爱音箱 Pro（LX06）。Windows 负责构建和部署，音箱负责本地识别「豆包豆包」和连接火山引擎 O2。原厂小爱进程继续处理「小爱同学」。应用看到原厂的唤醒事件时会退出豆包会话并让出播放。默认音频接口和唤醒日志路径依据 Open‑XiaoAI 的 LX06 1.94.13 补丁固件核对。现场仍需确认录音共享、原厂唤醒事件和两个助手的切换。

## 准备

- 在米家 App 记下 LX06 的实际固件版本，在路由器确认音箱的局域网 IPv4。电脑和音箱连同一个局域网。
- 原厂 LX06 1.94.13 先准备 Micro‑USB 数据线，并用匹配型号、版本的 Open‑XiaoAI 补丁固件启用共享录音和 SSH。保留当前系统备份。这个项目的 `setup/02` 到 `07` 是 OH2P 的工具。 [Open‑XiaoAI 的 LX06 1.94.13 发布页](https://github.com/idootop/open-xiaoai/releases/tag/LX06_1.94.13) · [Open‑XiaoAI 刷机和 SSH 文档](https://github.com/idootop/open-xiaoai/blob/main/docs/flash.md)
- Windows 部署需要 Python 3、OpenSSH 客户端及 curl.exe。从源码构建时另需 Git for Windows 和 Docker Desktop（Linux 容器模式）。在火山引擎开通豆包端到端实时语音 O2，并创建语音 API Key。 [火山引擎开通指南](火山引擎开通.md)

## 在 Windows 构建

```powershell
Set-Location D:\workspace_ai\xiaoai-voice
powershell -NoProfile -ExecutionPolicy Bypass -File .\setup\build-lx06.ps1
```

首次会下载编译依赖。产物是 `xiaoai-plus/build-armv7/xiaoai_plus_speaker`，在 ARM 音箱上运行。

## 检查和安装

先用 Windows OpenSSH 验证密钥登录，并核对音箱主机指纹，将它录入当前 Windows 用户的 SSH 主机记录。对只提供旧 RSA 算法的 Dropbear，单次 SSH 使用 `-o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa`，对应安装脚本参数是 `--legacy-rsa`。

下面用示例 IP 和私钥路径。把它们改成你的值，固件版本默认是 `1.94.13`。

```powershell
$speakerArgs = @('--host', '192.168.1.80',
                 '--key', "$env:USERPROFILE\.ssh\xiaoai_lx06")
python .\setup\lx06.py probe @speakerArgs
python .\setup\lx06.py probe-wake @speakerArgs
```

`probe` 核对型号、版本、日志可读性及录放音接口。它把一秒录音在音箱上丢弃，仅统计字节数，播放测试输入为静音。运行 `probe-wake` 时看到 `READY` 后喊一声「小爱同学」。命令应返回 `Native wake event detected`。若固件将原厂唤醒日志写到别处，可在参数中加入 `--native-log /实际日志路径`，检查和安装使用同一参数。如果收不到原厂事件，先从真机确认日志格式和路径。

```powershell
python .\setup\lx06.py install @speakerArgs
python .\setup\lx06.py start @speakerArgs
python .\setup\lx06.py status @speakerArgs
```

安装时在终端无回显输入 API Key，部署在音箱的 `/data/xiaoai-plus`。安装器用于首次安装，发现同名目录就停止。安装完通过 `start` 启动，随后便可关闭 Windows 电脑。音箱重启后可以再次运行 `start`。Open‑XiaoAI 的 LX06 1.94.13 固件会在开机时调用已有的 `/data/init.sh`。需要开机自启时，由设备管理员保留该文件中的原有命令，并加入 `/bin/sh /data/xiaoai-plus/boot.sh`。

## 双唤醒验收

1. 待机时喊「小爱同学」，试一条原来能用的指令。
2. 待机时喊「豆包豆包」，至少聊两轮，确认音箱能播出豆包的语音回复。
3. 豆包会话中喊「小爱同学」，确认豆包退出会话，小爱接管。小爱应答结束后，再次喊「豆包豆包」检查能否进入新会话。
4. 分别说「再见」和等待约 30 秒，确认两种方式都能退出豆包；用 `status` 查看进程是否还在。

LX06 默认以普通话问候并聊天。可以在同一次会话中说「接下来用英语」，聊几句后再说「切回普通话」。结束会话并重新唤醒后，默认语言恢复为普通话。建议把这两次切换和重新唤醒一起纳入真机验收。

上述语言行为适用于 Windows 安装器部署的独立对话模式。单独启用上游记忆后台时，使用该后台配套的英语陪练设定。
