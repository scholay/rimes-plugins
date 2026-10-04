# RIMES 官方插件

[![CI](https://github.com/scholay/rimes-plugins/actions/workflows/ci.yml/badge.svg)](https://github.com/scholay/rimes-plugins/actions/workflows/ci.yml)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-64748b)](LICENSE)

RIMES 官方免费开源插件仓库。

核心维护者：[学术海](https://pm.scholay.com) · 本体项目：[RIMES](https://github.com/scholay/rimes)

RIMES 的中文输入能力基于 [Rime 输入法引擎（librime）](https://github.com/rime/librime)。插件与引擎的实际依赖关系，以各插件说明为准。

## 当前状态

正在进行 RIMES 1.1.0 的插件拆分。Polisher / LaTeX 已有处理内容包、原生源码导入记录和确定性打包工具，宿主接入与四平台验收仍在进行。候选包尚不是正式发行版。

原生适配在本体构建时固定版本导入，随宿主签名；运行时下载包提供宿主解释的处理内容。完整约定见 [插件协议](SPECIFICATION.md)。

## 目录

- [plugins/](plugins/)：插件代码与各插件说明。
- [source-map.json](source-map.json)：插件原生源码与宿主编译路径的对应关系。
- [SPECIFICATION.md](SPECIFICATION.md)：宿主边界、分发及生命周期。
- [CONTRIBUTING.md](CONTRIBUTING.md)：贡献指引。
- [ATTRIBUTION.md](ATTRIBUTION.md)：来源与署名模板。
- [LICENSING.md](LICENSING.md)：许可证适用范围。

CI 校验仓库、许可与插件包，并生成带 SHA-256 的候选包、目录和原生源码压缩包。运行方式：

```sh
python3 scripts/check_repository.py
python3 -m unittest discover -s scripts -p 'test_*.py' -v
python3 scripts/packages.py
```

## 许可与署名

本仓库自有代码和文档采用 [Apache License 2.0](LICENSE)。归属信息见 [NOTICE](NOTICE)，第三方组件说明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

我们欢迎复用、改进与再创作，并倡议衍生版本注明 Rime、RIMES、所使用的官方插件和修改者之间的实际关系。具体展示位置与推荐文案属于倡议，不增加 Apache-2.0 之外的许可条件。

Official free and open-source plugins for RIMES, maintained by [学术海](https://pm.scholay.com). The 1.1.0 migration is in progress; candidate packages are not a production release. Original repository material is licensed under Apache-2.0, with third-party terms preserved where applicable.
