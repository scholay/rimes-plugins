# ChatGPT / Codex CLI

通过本机官方 Codex CLI 使用 ChatGPT 订阅。支持 macOS、Windows 和 Linux；Linux 由宿主编译的 app-server 适配器执行，插件包只声明渠道。

模型、推理深度与 Skill 按本渠道显式选择。Linux 登录保存在宿主私有数据目录的 `ai/codex-home`；不导入普通 `~/.codex` 的 MCP、工具或 Hook。文本任务随 Buffer 生命周期取消；显式 imagegen 任务由 Mailbox 持有，关闭 Buffer 后可继续，图片只保存和预览于 Mailbox。

Linux 候选构建与实际桌面、视觉验收由本体任务 [#171](https://github.com/scholay/xsime/issues/171) 跟踪。此声明不表示已公开发版。卸载宿主保留词库、学习记录、登录和 Mailbox 数据。

维护者：[学术海](https://pm.scholay.com)。Apache-2.0；宿主原生代码适用各自仓库的许可证。
