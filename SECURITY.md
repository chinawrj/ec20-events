# 密钥与个人数据

源码不包含真实 Pushover User Key、API Token、电话号码、IMEI、IMSI、ICCID、短信或原始实机日志。所有测试数据均为合成数据。

- 凭据置于仓库外的私有目录，或设备 `/data/ec20-events`（0700）中两个独立文件（0600）。程序拒绝权限过宽的凭据。
- 不把密钥写到源码、命令行参数、环境变量、示例、CI secrets 之外的工作流、README 或 Git 提交。
- `.gitignore` 仅是辅助，不是防泄漏保证。发布前仍应扫描 Git 索引和拟推送历史；本仓库提供 `tools/check_secrets.py`，可在本地用私有凭据目录精确匹配，输出只报告文件名。
- `outbox`、`pending`、`state` 包含个人通信数据，应保持私有。日志和二进制也要检查，不只查源码。
- 短信发送到用户指定的 Pushover 账户，HTTPS 保护传输；本实现不提供 Pushover 端到端加密。持久队列也未加密，具有网卡 root/ADB 访问权限的人可以读取。
- 不使用 `curl -k`，不通过 HTTP 发送 token，不把第三方公共应用 token 写进程序。
- 泄漏时在 Pushover 撤销/更换对应凭据，再独立更新设备文件；删除最新提交中的字符串不能清除 Git 历史。

不要在公开 issue 中贴凭据、完整短信、私有设备身份或整个 `/data/ec20-events` 的压缩包。
