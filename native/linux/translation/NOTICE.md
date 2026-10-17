# Linux local translation resources

This host interpreter reuses the Windows translation protocol and the exact
SHA-256-pinned Argos zh_en/en_zh 1.9 model packages. Linux runs CTranslate2 4.6.0
on CPU through private, pinned CPython 3.12 wheels. No text leaves the machine.
The host requires distro-provided Python 3.12. Resource installation is a separate
explicit operation; inference never downloads resources.

Initial downloads are 179,927,877 bytes on arm64 and 203,839,312 bytes on amd64.
Weights and wheels are checked by exact length and SHA-256 before extraction.
Original model metadata, README and wheel license files are retained. Plugin
uninstallation does not remove the private language resource directory.

- CTranslate2 4.6.0: MIT, https://github.com/OpenNMT/CTranslate2
- SentencePiece 0.2.0: Apache-2.0, https://github.com/google/sentencepiece
- NumPy 2.2.6: BSD-3-Clause and bundled notices, https://numpy.org/
- PyYAML 6.0.2: MIT, https://pyyaml.org/
- setuptools 80.9.0: MIT, https://github.com/pypa/setuptools
- System Python: Python Software Foundation License, https://docs.python.org/3/license.html
- Argos zh_en/en_zh 1.9, derived from OPUS-MT: CC-BY 4.0.
  Authors: Jörg Tiedemann and Santhosh Thottingal. “OPUS-MT — Building open
  translation services for the World”, EAMT 2020, Lisbon, Portugal.
  Source index: https://github.com/argosopentech/argospm-index
  License: https://creativecommons.org/licenses/by/4.0/

The lock records the SHA-256 of the Windows resource lock used for the shared
model selection. Update both platforms' model identity deliberately; never replace
an artifact behind an existing digest. Runtime Python files are reviewed source
imports, not downloaded plugin code.
