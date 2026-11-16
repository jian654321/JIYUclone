# 手动验证脚本 (PowerShell)

这些脚本用于在 Windows 上对已编译的程序做**端到端验证**。它们会启动 `dist\` 下的真实程序、
模拟教师/学生报文、投递窗口消息, 结束时自动清理进程。**运行时桌面会短暂出现程序窗口, 属正常现象。**

> 运行方式 (PowerShell):
> ```powershell
> powershell -ExecutionPolicy Bypass -File tests\test_xxx.ps1
> ```
> 所有脚本相互独立, 可单独运行; 每个脚本结尾都会自动结束 Teacher/Student 进程。

| 脚本 | 验证内容 |
|---|---|
| `test_connect_screenshot.ps1` | 发现+连接链路: 启动两端 → 截图教师端确认学生"在线" |
| `test_dial_on_demand.ps1` | 按需拨号: 假学生(UDP 心跳+15670 监听) → 教师反向拨号 → RDP/终端命令收发 |
| `test_lock_taskmgr.ps1` | 锁屏看门狗: 锁屏期间启动任务管理器 → 300ms 内被结束; 解锁后恢复 |
| `test_password_flow.ps1` | 紧急退出: 锁屏 → 5×F8 → 密码框 → 654321 → 解锁 |
| `test_exit_flow.ps1` | 退出流程: "退出程序"密码确认 → 进程退出、无残留窗口 |
| `极域安装包静态扫描.ps1` | 辅助工具: 对极域安装包 PE 做只读字符串扫描(docs/极域运行逻辑解析.md 的取证脚本) |

注意:
- 脚本通过 `PostMessage` 模拟点击/按键, 属灰盒验证; 若界面改动请同步更新控件 ID。
- 锁屏类脚本会在真实桌面弹出全屏窗口并短暂屏蔽快捷键, 测试机请勿处理重要事务。
