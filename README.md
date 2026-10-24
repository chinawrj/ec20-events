# EC20 Events

在 Quectel EC20 OpenLinux 网卡内运行的轻量 C 程序：监听新短信、SIM 和蜂窝注册/附着状态变化，持久排队，通过 Pushover HTTPS API 通知手机。无需常驻电脑、浏览器、Python 或 Node.js。设备端使用一个静态 ARM ELF 和固件已有的原生 `/usr/bin/curl`；SysV init 的短 shell 脚本只负责启动和停止。

## 支持范围

- 实测平台：EC20F / MDM9607，ARMv7 Linux 3.18.20，USB VID:PID `2c7c:0125`，内部 AT `/dev/smd8`。**不是 Huawei E8372h-155 的安装包。**
- 新短信：监听 `+CMTI: "SM",index`，检查 PDU 模式及当前读存储，然后 `AT+CMGR=index`。支持 GSM 7-bit 默认/扩展字符、UCS-2、字母数字发送方和 UDH 长短信标识。长短信各段分别通知，不合并；不支持的编码保留为十六进制提示。
- 事件：每次系统启动通知；运行期间 SIM PIN 状态、LTE 注册、分组网络附着状态变化。每个状态约 30 秒查询一次。
- 100 条持久通知队列；HTTPS 成功且 API `status=1` 才确认消费。错误按 120 秒至 3600 秒退避；正常发送间隔通常不超过 60 秒。断网重连后可能仍需等待已有退避。
- 进程锁防重复实例；监督进程在采集进程退出后约 30 秒重启它。
- 不扫描历史短信、不删除 SIM 短信。读取新短信会把它标为已读。SIM 存储满时需要人工清理。

## 实机验证状态

详见 [验证记录](VALIDATION.md)。手机收到的最初 Hello World 经电脑网络发送，用户已确认收到。原生 ARM 程序已在网卡执行，安装到持久分区并通过服务启动/停止、采集进程恢复及 Linux 系统重启后自动运行测试。

当前设备没有天线，尚无蜂窝数据路由，启动时钟为 1980 年。因此**尚未验证脱离电脑后的真实短信接收及网卡直连 Pushover 投递**。程序遇到无效时钟会保留队列，绝不关闭 TLS 证书验证。接上天线、正确 APN/联网、校准时间是独立运行的前提；程序不负责拨号或重写 APN。

## 编译与测试

```sh
# 主机功能/伪终端测试；网络调用被替身替代，不接触设备或真实密钥
python3 tools/run_tests.py

# 主机版（可用于离线 decode 和测试）
make

# Zig 0.13.0，静态 ARMv7 / musl soft-float
make arm ZIG=/absolute/path/to/zig
```

使用支持当前 macOS SDK 的编译器；如果 SDK 与 linker 不匹配，可以通过 `CC` / `CFLAGS` 显式指定匹配的 SDK。测试包含中文/扩展字符、长短信填充位、截断 PDU、100 条队列上限、失败保留、API 成功确认、密钥权限及 AT 伪终端恢复。

## 安装

安装工具仅适用于上面的 EC20 USB 配置。关闭占用 ADB 的其他工具；需要主机 Python 3、PyUSB、libusb。它通过 USB ADB 接口 6 上传，校验 SHA-256。它不会打印上传内容或密钥。

```sh
python3 -m pip install pyusb
# 在仓库以外创建私有目录，目录 0700，文件 0600。
# app-token 和 user-key 各为 30 字节，不带结尾换行。
python3 tools/deploy.py --key-dir /absolute/private/directory --install-service
# 后续升级保持设备密钥
python3 tools/deploy.py --install-service
```

设备路径：

| 路径 | 用途 |
|---|---|
| `/data/ec20-events/ec20-events` | ARM 可执行文件，0700 |
| `/data/ec20-events/app-token`、`user-key` | 私有凭据，0600 |
| `/data/ec20-events/outbox` | 待发通知，含短信正文；不得公开 |
| `/data/ec20-events/pending` | 已接收但尚未成功入队的短信槽位 |
| `/data/ec20-events/state` | 去重 PDU、boot ID、受限状态摘要；不得公开 |
| `/etc/init.d/ec20-events` | SysV 管理脚本 |
| `/etc/rc[2-5].d/S99ec20-events` | 多用户级别启动 |
| `/etc/rc[016].d/K01ec20-events` | 停机/单用户级别停止 |
| `/run/ec20-events.pid` | 监督进程 PID |

```sh
# 以下命令在网卡 shell 执行
/etc/init.d/ec20-events status
/etc/init.d/ec20-events stop
/etc/init.d/ec20-events start
/etc/init.d/ec20-events restart
/data/ec20-events/ec20-events enqueue 'TEST' 'Synthetic notification'
/data/ec20-events/ec20-events flush
```

`flush` 只投递现有队列。不要用 `microcom /dev/smd8` 与服务并发读写同一 AT 通道。网页使用 USB AT 时也不要同时修改 CMGF、CPMS、CNMI 或 URC 路由；服务依赖 `QURCCFG="urcport"` 为 `smd`，当前实机已经如此，安装器不改它。

## 卸载/回滚

```sh
/etc/init.d/ec20-events stop
rm /etc/rc2.d/S99ec20-events /etc/rc3.d/S99ec20-events \
   /etc/rc4.d/S99ec20-events /etc/rc5.d/S99ec20-events \
   /etc/rc0.d/K01ec20-events /etc/rc1.d/K01ec20-events /etc/rc6.d/K01ec20-events
rm /etc/init.d/ec20-events
```

此操作保留 `/data/ec20-events` 中的程序、密钥和队列，便于回滚或另行备份。不要将整个设备目录提交到 Git。安装器会覆盖本项目程序和本项目管理的 init 文件；不会覆盖同名但无项目标识的第三方 init 文件。

## 已知边界

- AT 通道未监听期间的新短信通知可能丢失；不扫描历史存储意味着不能保证停机期间的短信补收。已落盘的 pending 槽位会在重启后续处理。
- `SM` 外存储只推送到达提示；PDU 模式或 SM 读存储不符合时暂停正文读取，不擅自改存储。
- 同一 SIM 槽位与完全相同 PDU 用于去重；通知的远端投递是至少一次语义，服务器接受后、本地确认前掉电可能重复。
- 监督进程自身若被杀死，需要服务重启或系统重启；它只监督采集进程，不是整机 watchdog。
- 队列满时不丢弃旧事件；新的 SMS pending 保留，但快速发生的中间状态变化可能合并或遗漏。启动/队列不可写等失败需要检查本地 status。
- 只保证本次实测固件；其他 EC20 固件可能没有 Linux shell、curl、内部 AT 或相同的启动系统。

协议、状态机和扩展方式见 [DEVELOPMENT.md](DEVELOPMENT.md)。凭据处理见 [SECURITY.md](SECURITY.md)。
