# RIMES 官方插件

[![CI](https://github.com/scholay/rimes-plugins/actions/workflows/ci.yml/badge.svg)](https://github.com/scholay/rimes-plugins/actions/workflows/ci.yml)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-64748b)](LICENSE)

RIMES 官方免费开源插件仓库。

核心维护者：[学术海](https://pm.scholay.com) · 本体项目：[RIMES](https://github.com/scholay/rimes)

RIMES 的中文输入能力基于 [Rime 输入法引擎（librime）](https://github.com/rime/librime)。插件与引擎的实际依赖关系，以各插件说明为准。

## 当前状态

仓库已完成初始化，包含许可、署名说明、贡献指引和 GitHub Actions 基础校验。插件实现、清单及各平台构建流程将在后续接入；本体中现有插件尚未迁移到这里。

## 目录

- [plugins/](plugins/)：插件代码与各插件说明。
- [CONTRIBUTING.md](CONTRIBUTING.md)：贡献指引。
- [ATTRIBUTION.md](ATTRIBUTION.md)：来源与署名模板。
- [LICENSING.md](LICENSING.md)：许可证适用范围。

CI 当前校验仓库文件、许可证完整性、本地文档链接和提交空白格式，使用标准 GitHub 托管运行器。运行方式：

```sh
python3 scripts/check_repository.py
```

## 许可与署名

本仓库自有代码和文档采用 [Apache License 2.0](LICENSE)。归属信息见 [NOTICE](NOTICE)，第三方组件说明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

我们欢迎复用、改进与再创作，并倡议衍生版本注明 Rime、RIMES、所使用的官方插件和修改者之间的实际关系。具体展示位置与推荐文案属于倡议，不增加 Apache-2.0 之外的许可条件。

Official free and open-source plugins for RIMES, maintained by [学术海](https://pm.scholay.com). This repository is being initialized; plugin implementations and platform builds will be added separately. Original repository material is licensed under Apache-2.0, with third-party terms preserved where applicable.
