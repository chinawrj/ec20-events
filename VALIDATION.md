# 验证记录 — 2026-10-24

## 已通过

- 用户确认手机收到最初 Hello World；该条由主机 HTTPS 提交。
- ARMv7 静态 ELF 在真实 EC20 上运行，原生中文 SMS PDU 解码输出正确。
- 程序约 64 KiB；部署后逐文件 SHA-256 校验。
- SysV 服务安装到持久的 `/etc` 与 `/data`；start、stop、restart 实测。
- 服务启动后 supervisor 与 monitor 两个进程存在；supervisor 实测 RSS 76 KiB（并非整个进程树峰值）。
- 人工结束 monitor 后，supervisor 约 30 秒自动启动新 monitor；既有队列保持。
- 两个原有事件保留在队列；无效 1980 年时钟下发送被拒绝，未关闭 TLS 校验。
- 8 个主机自动化测试通过：中文/emoji、GSM7 扩展、长短信填充/段号、截断 PDU、100 条上限/失败保留、伪 AT 设备待办恢复/重复实例排斥、HTTP/TLS/API 异常保留与成功确认、密钥权限拒绝。发送测试使用本地替身，不触发网络请求。

- Linux `/sbin/reboot` 后 USB 已恢复；boot ID 已变化，uptime 为 1 分钟，supervisor 的 PPid=1，monitor 运行，无需手动 start；队列从 2 条增为 3 条（新增启动通知）。系统重启自启通过，未声称物理断电压力测试通过。

- 20,000 个确定性合成异常 PDU 输入通过 UBSan 检查。macOS 27 的 ASan runtime 在进入 main 前自旋，已停止该测试进程；未将此项记为 ASan 通过，Linux CI 中保留 ASan+UBSan 验证。

## 当前待验收

- 未验证真实运营商下发短信：设备没有天线。
- 未验证网卡独立 HTTPS 投递：没有蜂窝默认路由，时钟还未由可信来源校准。
- 队列掉电容错经过原子 rename/fsync 设计与代码检查，未做随机硬断电压力测试。
- GitHub 目标仓库尚待用户指定；本地源码包已独立于原项目私有证据目录。

不能把“本机单元测试通过”“程序成功运行”或“安装自启链接”写成“运营商短信与脱机推送已经端到端通过”。

设备密钥权限实测为 0600，目录 0700；程序 SHA-256：`9e41585f995c7b156f4b72b922ce20079838bf6b794ff6b7c8f69d0b4e3212af`。重启后 `/data` 约剩余 5864 KiB。
