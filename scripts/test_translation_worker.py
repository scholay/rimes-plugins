"""Portable regression checks for the Windows worker and shared Linux core."""
import importlib.util
import io
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[1] / "native/shared-python/local_translation.py"
spec = importlib.util.spec_from_file_location("translation_core", SOURCE)
core = importlib.util.module_from_spec(spec)
spec.loader.exec_module(core)


class TranslationWorkerTests(unittest.TestCase):
    def test_worker_keeps_framing_after_invalid_json(self):
        incoming = io.BytesIO(b'not json\n{"text":"hello","target":"English"}\n')
        outgoing = io.BytesIO()
        with patch.object(sys, "argv", [str(SOURCE), "/tmp/models", "--worker"]), \
             patch.object(sys, "stdin", SimpleNamespace(buffer=incoming)), \
             patch.object(sys, "stdout", SimpleNamespace(buffer=outgoing)):
            core.main()
        replies = [json.loads(line) for line in outgoing.getvalue().splitlines()]
        self.assertIn("error", replies[0])
        self.assertEqual(replies[1], {"text": "hello"})

    def test_worker_rejects_truncated_frame(self):
        outgoing = io.BytesIO()
        with patch.object(sys, "argv", [str(SOURCE), "/tmp/models", "--worker"]), \
             patch.object(sys, "stdin", SimpleNamespace(buffer=io.BytesIO(b'{"text":"hello"}'))), \
             patch.object(sys, "stdout", SimpleNamespace(buffer=outgoing)):
            core.main()
        self.assertEqual(outgoing.getvalue(), b"")

    def test_explicit_direction_is_not_changed_by_mixed_text(self):
        tokenizer = SimpleNamespace(encode=lambda text, **_: [text], decode=lambda _: "translated")
        engine = SimpleNamespace(translate_batch=lambda *a, **k: [SimpleNamespace(hypotheses=[["translated"]])])
        with patch.object(core, "models", return_value=(tokenizer, engine)) as models:
            result = core.translate({"text": "hello 世界", "source": "English", "target": "Chinese"}, Path("/tmp/models"))
            self.assertEqual(result, "translated")
            models.assert_called_once_with(Path("/tmp/models"), "en", "zh")


if __name__ == "__main__":
    unittest.main()
