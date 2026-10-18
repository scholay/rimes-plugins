# Local aggregate statistics

The portable core mirrors the native macOS KeyFrequencyStore and TypingSpeedStore. It accepts sanitized key identifiers, repeat/shortcut flags, character counts and chord measurements. It has no text, application identity or delivery API. Persistence, focus/protection checks and opt-in plugin lifecycle belong to the host.

The two collectors remain independent. Key frequency includes physical repeats and shortcuts. Activity excludes repeats, shortcuts and pure navigation. Sessions split after a gap greater than 10 seconds, on a backwards clock, at local midnight, on collector changes and on restart. Active time is the sum of eligible within-session intervals. Character rate is count × 60 / max(1, active seconds); a missing day or zero-duration sample remains unknown in the UI.

Native version-1 JSON records, 500 recent sessions, bounded counters, calendar-span limits and deterministic key-frequency ties are preserved. Loaded unknown JSON fields are discarded as with native Codable. Civil-day range generation avoids DST gaps.

Linux replays fixtures exported by the actual native stores in `Sources/RimeBuffer/StatisticsLinuxFixtures.swift`; it does not substitute a handwritten expected result for the macOS behavior. `platforms/linux/statistics/tests/metrics_core_tests.cpp` covers those fixtures, restart/consent/deletion, admission filters, exact inactivity thresholds, DST, limits and corrupt records.
