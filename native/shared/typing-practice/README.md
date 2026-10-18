# Article practice core

The Linux host uses this bounded, Unicode-aware scoring model with the same
articles and actual step-by-step observations exported from the native macOS
`TypingTestSession`. Only committed editor text is aligned. Preedit starts the
clock but is not scored. Physical keys, repeats, accepted chord batches,
composition backspace, committed backspace and corrections remain distinct.

The host supplies a monotonic clock, explicit insertion ranges, trusted chord
availability, a frozen input configuration and interruption reasons. Results
contain aggregate scores and configuration identity, never article/editor text.
No key listener, user files, timers or network calls belong in this core.

`Characters` and `InsertionRange` use ICU extended grapheme boundaries and NFC
comparison. The bounded Myers edit traversal follows Swift's tie order so
repeated characters receive the same attempt counts. Its source and license are
recorded in `THIRD_PARTY_NOTICES.md` and `LICENSES/Swift-Apache-2.0.txt`.

The host's contract, storage, GTK editor and real Fcitx tests must pass before
enabling the Linux adapter. Desktop and pixel acceptance remain separate gates.
