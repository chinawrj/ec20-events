# 开发与协议

## 数据通路

```text
EC20 内部 /dev/smd8
    -> C AT 逐行解析器
    -> +CMTI 持久 pending 槽位
    -> CMGF? / CPMS? / CMGR=index
    -> C SMS-DELIVER PDU 解码
    -> outbox 原子提交
    -> 原生 curl (GnuTLS、证书/主机名验证)
    -> https://api.pushover.net/1/messages.json
```

程序静态链接 musl，固件上的 curl 则动态使用其 GnuTLS。没有 shell 执行拼接的短信内容；发送程序通过 `fork` + `execl` 调用 curl，正文和密钥均以文件参数传入。装机脚本使用 USB ADB，只在开发/安装时需要电脑。

## 内部 AT

- 打开 `/dev/smd8`，raw termios、115200；退出时恢复 termios。
- `AT+CNMI=2,1,0,0,0`：到达时发送 `+CMTI`。本机安装前即为此值。
- `AT+CPIN?`：`+CPIN: READY` 等；`AT+CEREG?`：`+CEREG: n,stat`；`AT+CGATT?`：`+CGATT: 0/1`。
- `+CMTI: "SM",7`：将槽位 7 原子写入 `pending/7`，不打印电话号码或正文。
- 读取前查询 `AT+CMGF?`，要求 0；查询 `AT+CPMS?`，要求第一个存储为 SM。
- `AT+CMGR=7`：接收 `+CMGR: ...`、一行十六进制 PDU、终止 `OK`。只有完整成功事务才解码、入队、写去重记录并移除 pending。
- `ERROR`、`+CMS ERROR`、`+CME ERROR` 和 8 秒超时均不删除待处理槽位。SMS 重试至少间隔 30 秒，状态轮询继续进行。
- 不发 `CMGL` 或 `CMGD`，不扫描、删除旧短信，不改变 APN/USB 模式或执行基带重启。
- 当前只处理 SMS-DELIVER（TP-MTI=00）。URC 与查询共用流；CEREG 仅在对应查询期间比较。未来可进一步实现完整 AT 命令仲裁和 CEREG URC 语法区分。

AT 输入行最大 2047 字节，溢出后丢弃到行结束；PDU 最大 512 字节。禁止多个程序同时读取内部 AT。外部 USB AT 修改共享设置也会影响服务。

## SMS PDU

依次读取：SMSC 长度及地址、TPDU 首字节、发送方地址长度/类型及半字节地址、PID、DCS、7 字节 SCTS、UDL、用户数据。SCTS 目前跳过，不作为系统校时依据。

- 国际电话号码加 `+`；数字低半字节在前；字母数字地址采用 GSM7。
- DCS 选择 GSM7、8-bit 或 UCS-2。压缩/保留类型不猜测为文本。
- GSM7 逐 septet 解包，支持 ESC 扩展。UDH 占据的 septet 数为 `ceil((UDHL+1)*8/7)`，包括填充位。
- UCS-2 大端解码，同时兼容成对 UTF-16 surrogate；无效 surrogate 替换为 U+FFFD。
- UDH IEI `00` / `08` 提取 8/16 位拼接引用、总段数和段号。不会自动拼接，以免把不同发送方/批次消息误合并。
- 国家语言 shift table IEI `24` / `25` 不支持，输出编码提示与十六进制数据，避免静默乱码。
- 非法长度/截断输出“无法解码”通知；原 PDU 保存在私有去重文件，供本地诊断。

参考：[3GPP TS 23.040](https://www.3gpp.org/ftp/Specs/archive/23_series/23.040/)（SMS TPDU）、[3GPP TS 23.038](https://www.3gpp.org/ftp/Specs/archive/23_series/23.038/)（字符集）。测试号码、中文正文和所有测试 PDU 都是合成数据。

## 队列与持久化

先在 `outbox/.stage.XXXXXX` 内写 `title`、`message`，fsync 后 rename 为 `event.XXXXXX`。通过 `flock` 分别保护生产/确认与发送，进程死亡自动释放，不依赖删除陈旧目录锁。

API 接受后先将事件目录 rename 为 `.acked.XXXXXX` 并同步父目录，再删除其中的文件。这样中途掉电不会留下只有半个通知、永久阻塞后续队列的可见事件。掉电可能留下隐藏 staging/acked 残片，可停服务后检查清理；正常成功路径会删除。

队列上限 100，标题上限 150 UTF-8 字节，正文上限 1024 UTF-8 字节（比 Pushover 字符上限保守）。不会截断 UTF-8；超限拒绝入队。排序按事件目录名，不承诺严格时间顺序；本机无可靠 RTC 时仍能排队。

`state/sms-INDEX.pdu` 是最后成功入队的完整 PDU，用于同一槽位重放去重。SMS 解码与队列写入在采集进程中完成；HTTPS 在子进程执行，慢网络不会占住 AT 监听。

## Pushover HTTPS

官方协议：[Pushover Message API](https://pushover.net/api)。固定 POST 地址 `https://api.pushover.net/1/messages.json`；字段 `token`、`user`、`title`、`message`、`priority=0`。使用 `application/x-www-form-urlencoded`。

curl 的参数是 `token@/private/path`、`user@/private/path` 等文件引用，进程参数列表不包含凭据。禁用 `.curlrc`，不跟随重定向；启用 HTTPS 协议限制、默认 TLS 校验、HTTP 错误失败、10 秒连接和 30 秒总超时。API body 顶层整数 `status=1` 才确认。返回 1 表示服务器接受，不是设备已展示/用户已读。

时钟早于 2025 年拒绝发送；没有可信校时源时不伪造时间或使用 `-k`。首次失败退避 120 秒，此后指数增长到 3600 秒；成功后恢复 60 秒。HTTP 429 / 凭据错误目前采用同样退避，未区分 Retry-After、永久错误或额度耗尽。状态文件只记录通用原因；不保存包含私密响应字段的 API body，临时响应处理完即删。

## 启动及故障模型

固件 SysV init 默认级别 5；S99 启动本项目，K01 停止。`start-stop-daemon` 启动 C 的 supervise 模式；supervisor fork monitor，monitor 再 fork 短命 HTTPS sender。主进程和监控进程分别有 flock 锁。收到 TERM 时停止新事务、等待当前 curl 最多 30 秒结束；监督进程收到 TERM 后同步停止 monitor。

启动通知以 Linux boot ID 去重，服务 restart 不重复产生开机通知。状态变化基线在内存，重启服务重新建立基线，不把首次查询误判为变化。要扩展硬件看门狗、掉电前通知、存储水位、ME 存储、长短信合并、MQTT、历史未读补扫，应在保留这些故障边界的基础上增加独立协议和测试。

## USB ADB 安装协议

工具针对接口 6，OUT `0x06`、IN `0x8A`，bulk max packet 512。ADB 消息包含 24 字节小端头：command、arg0、arg1、payload length、字节和 checksum、command XOR FFFFFFFF。发送头和 payload 分开；输入累计整头和 payload，验证长度、校验和与 magic。

握手 `CNXN(version=0x01000000,maxdata=4096,"host::")`，使用 `OPEN(local-id=1,"shell:...\0")`，每条 `WRTE` 回 `OKAY`，`CLSE` 结束。该固件未要求 AUTH；其他需要鉴权的设备会拒绝操作，工具不绕过认证。上传经 base64 分块在远端重组为 `.new`，chmod 后 rename，再比较 SHA-256。操作失败只输出通用错误，避免传输正文/密钥泄漏。

这里不使用 TinyUSB：此程序运行在网卡自己的 Linux 上。TinyUSB 是先前电脑侧 USB host/固件开发项目的组件，与网卡内事件服务分属不同层。
