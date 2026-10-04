# 第三方组件与来源

`native/macos/ScholayAcademicPlugins.swift` 从 RIMES 提交
`5daa62b87c008028c42581713aaa85131683697d` 导入。该文件包含 Polisher / LaTeX
原生适配，导入时保留原有行为；后续修改记录在 Git 历史中。RIMES 现行许可为 Apache-2.0，
历史 MIT 材料的授权与版权声明保留在 [LICENSES/MIT-legacy.txt](LICENSES/MIT-legacy.txt)。
本仓库不分发 Rime 引擎二进制。

宿主项目为 [RIMES](https://github.com/scholay/rimes)，其中文输入能力基于 [Rime / librime](https://github.com/rime/librime)。这项来源说明不表示当前仓库直接分发了引擎。

后续引入第三方代码、方案、词库、图片或其他资源时，请在相应插件目录记录组件名称、原始来源、版本或提交、许可证、版权声明及修改情况，并随发行包保留所需的完整许可材料。
