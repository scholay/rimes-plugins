# Claude channel

The optional Linux plugin exposes the native host's Claude Code CLI channel.
Install the official CLI separately; XSIME checks its declared capabilities and
its own authentication status before sending an explicit Buffer request. The
CLI manages credentials. XSIME does not import environment API keys or display
account metadata.

Text generation uses the same complete-source prompt, model/effort choices,
streaming contract and tool-disabled print arguments as macOS. The Linux host
owns process cancellation, result validation and native input delivery. Plugin
revocation invalidates outstanding results. Downloading this data manifest does
not install a CLI or authorize a network request.

Implementation and acceptance are tracked in scholay/xsime issue #172. A platform
entry is not evidence of authenticated model, desktop or pixel acceptance.
