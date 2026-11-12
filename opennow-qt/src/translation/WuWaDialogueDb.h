// WuWaDialogueDb: pre-translated Wuthering Waves dialogue lines.
//
// Same JSON format as TranslationCache ({"version":1,"entries":{en: vi}})
// with keys normalized the same way, so a DB hit and a cache hit are
// interchangeable. The DB is consulted BEFORE the memory cache: a curated
// line always wins over a machine-translated one.
//
// The shipped seed file (data/wuwa-dialogue-db.json) intentionally contains
// ZERO datamined lines: no game dialogue translations are invented here.
// It starts empty and fills only from lines actually seen (and verified)
// during live play. Do not hand-write dialogue into the seed file.
//
// Chunk mode (v4): the downloaded chunk database (repo
// huyzoz/wuwa-vi-chunks, one {"version":1,"entries":{...}} file per
// 4,000-string chunk) is loaded with loadChunkFile(), which MERGES
// entries instead of replacing them, so 55 files accumulate into one
// map. lookupFuzzy() tolerates OCR noise: exact on the squashed form
// (all non-alphanumerics stripped), then a Levenshtein-ratio match among
// entries sharing the first two words.
#pragma once

#include <QHash>
#include <QList>
#include <QMutex>
#include <QString>

class WuWaDialogueDb
{
public:
    WuWaDialogueDb() = default;
    explicit WuWaDialogueDb(const QString &filePath);

    // Loads entries from `path` (JSON, same shape as the translation cache).
    // Returns false if the file exists but cannot be parsed; a missing file
    // leaves the DB empty and returns true.
    bool loadDialogueDb(const QString &path);
    bool loadDialogueDb(); // reloads from the path given at construction

    // Merges entries from a chunk file (same JSON shape) WITHOUT clearing
    // what is already loaded. Returns false if the file exists but cannot
    // be parsed; a missing file is a no-op that returns true.
    bool loadChunkFile(const QString &path);

    void clear(); // empties the map (e.g. before a re-download)

    // Normalized lookup. Returns true on hit and fills `translated`.
    bool lookup(const QString &englishText, QString &translated) const;
    // OCR-tolerant lookup: squashed-form exact, then Levenshtein ratio
    // >= 0.88 among entries sharing the first two words. Returns true on
    // hit and fills `translated`.
    bool lookupFuzzy(const QString &englishText, QString &translated) const;

    // Levenshtein similarity ratio in [0,1]: 1 - dist/max(lenA, lenB).
    // Pure and unit-testable.
    static double similarityRatio(const QString &a, const QString &b);

    int size() const;
    QString filePath() const { return m_filePath; }

private:
    // Rebuilds m_firstTwoWordsIndex from m_entries. Caller holds m_mutex.
    void rebuildIndexLocked();
    // First two words of a normalized key (or the whole key if shorter).
    static QString firstTwoWords(const QString &normalizedKey);

    QString m_filePath;
    mutable QMutex m_mutex;
    QHash<QString, QString> m_entries;
    // First-two-words -> normalized keys, for bounding fuzzy candidates.
    QHash<QString, QList<QString>> m_firstTwoWordsIndex;
    // Squashed key (non-alphanumerics stripped) -> normalized key.
    QHash<QString, QString> m_squashedIndex;
};
