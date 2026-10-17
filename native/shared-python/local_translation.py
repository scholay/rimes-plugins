"""RIMES's bounded stdin/stdout local translator. Never opens a network connection."""
import json
import os
from pathlib import Path
import re
import sys

LIMIT = 256 * 1024
LANGUAGES = {"english": "en", "en": "en", "英语": "en", "英文": "en",
             "chinese": "zh", "zh": "zh", "zh-cn": "zh", "zh-hans": "zh", "简体中文": "zh", "中文": "zh"}


def translate(request, root):
    import ctranslate2
    import sentencepiece

    text = request["text"]
    if not isinstance(text, str) or not text or len(text.encode("utf-8")) > LIMIT or "\0" in text:
        raise ValueError("Invalid source text")
    target = LANGUAGES.get(request["target"].strip().lower())
    if not target:
        raise ValueError("Local translation supports English and Chinese. Select AI for other languages.")
    configured_source = request.get("source", "auto").strip().lower()
    source = ("zh" if re.search(r"[\u3400-\u9fff\U00020000-\U0002fa1f]", text) else "en") if configured_source == "auto" else LANGUAGES.get(configured_source)
    if not source:
        raise ValueError("Unsupported source language")
    if source == target:
        return text
    package = root / "models" / f"translate-{source}_{target}-1_9"
    tokenizer = sentencepiece.SentencePieceProcessor(model_file=str(package / "sentencepiece.model"))
    engine = ctranslate2.Translator(str(package / "model"), device="cpu", compute_type="int8",
                                  inter_threads=1, intra_threads=min(4, os.cpu_count() or 1))
    # Preserve paragraph boundaries; never let CT2 silently truncate long input.
    output = []
    for paragraph in re.split(r"(\r?\n)", text):
        if not paragraph.strip():
            output.append(paragraph)
            continue
        sentences = re.split(r"(?<=[。！？.!?])\s*", paragraph)
        translated = []
        for sentence in sentences:
            if not sentence:
                continue
            tokens = tokenizer.encode(sentence, out_type=str)
            if len(tokens) > 512:
                raise ValueError("Sentence too long for local translation. Add sentence breaks or select AI.")
            result = engine.translate_batch([tokens], beam_size=4, max_input_length=0,
                                            max_decoding_length=1024)[0]
            # These OPUS packages retain the SentencePiece whitespace marker in
            # decoded pieces; follow Argos' detokenization without altering '_'.
            decoded = tokenizer.decode(result.hypotheses[0]).replace("▁", " ").strip()
            if target == "zh":
                decoded = re.sub(r"\s+([。，！？；：])", r"\1", decoded)
            if not decoded:
                raise ValueError("Local model returned no translation")
            translated.append(decoded)
        output.append((" " if target == "en" else "").join(translated))
    result = "".join(output)
    if len(result.encode("utf-8")) > LIMIT:
        raise ValueError("Translation is too large")
    return result


if __name__ == "__main__":
    try:
        raw = sys.stdin.buffer.read(LIMIT * 6 + 1025)
        if len(raw) > LIMIT * 6 + 1024:
            raise ValueError("Request too large")
        response = {"text": translate(json.loads(raw.decode("utf-8")), Path(sys.argv[1]).resolve())}
    except ValueError as error:
        response = {"error": str(error)}
    except Exception:
        response = {"error": "Local translation resources are unavailable or incompatible."}
    sys.stdout.buffer.write(json.dumps(response, ensure_ascii=False).encode("utf-8"))
