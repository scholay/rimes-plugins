"""RIMES's bounded stdin/stdout local translator. Never opens a network connection."""
from functools import lru_cache
import json
import os
from pathlib import Path
import re
import sys

LIMIT = 256 * 1024
LANGUAGES = {"english": "en", "en": "en", "英语": "en", "英文": "en",
             "chinese": "zh", "zh": "zh", "zh-cn": "zh", "zh-hans": "zh", "简体中文": "zh", "中文": "zh"}


@lru_cache(maxsize=2)
def models(root, source, target):
    import ctranslate2
    import sentencepiece
    package = root / "models" / f"translate-{source}_{target}-1_9"
    tokenizer = sentencepiece.SentencePieceProcessor(model_file=str(package / "sentencepiece.model"))
    engine = ctranslate2.Translator(str(package / "model"), device="cpu", compute_type="int8",
                                  inter_threads=1, intra_threads=min(4, os.cpu_count() or 1))
    return tokenizer, engine


def translate(request, root):
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
    tokenizer, engine = models(root, source, target)
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


def respond(raw, root):
    try:
        if len(raw) > LIMIT * 6 + 1024:
            raise ValueError("Request too large")
        return {"text": translate(json.loads(raw.decode("utf-8")), root)}
    except ValueError as error:
        return {"error": str(error)}
    except Exception:
        return {"error": "Local translation resources are unavailable or incompatible."}


def main():
    root = Path(sys.argv[1]).resolve()
    worker = len(sys.argv) == 3 and sys.argv[2] == "--worker"
    while True:
        raw = (sys.stdin.buffer.readline(LIMIT * 6 + 1025) if worker
               else sys.stdin.buffer.read(LIMIT * 6 + 1025))
        if not raw:
            return
        if worker and not raw.endswith(b"\n"):
            return  # Truncated/oversized request must not desynchronize the stream.
        response = respond(raw, root)
        sys.stdout.buffer.write(json.dumps(response, ensure_ascii=False).encode("utf-8") + (b"\n" if worker else b""))
        sys.stdout.buffer.flush()
        raw = response = None  # Keep model weights, never a source/result history.
        if not worker:
            return


if __name__ == "__main__":
    main()
