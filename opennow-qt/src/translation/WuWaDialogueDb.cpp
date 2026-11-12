#include "translation/WuWaDialogueDb.h"
#include "translation/TranslationCache.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QtGlobal>
#include <algorithm>

namespace {
// Minimum Levenshtein similarity for a fuzzy DB hit. OCR noise is mostly
// punctuation/case (handled by normalization) plus one or two wrong
// characters per line; 0.88 accepts those while rejecting a different
// line that merely shares its first two words.
constexpr double MinFuzzyRatio = 0.88;

// Normalized key with every non-alphanumeric stripped, so "Talk to
// Shilang!" and "Talk to Shilang" squash to the same form.
QString squashKey(const QString &normalizedKey)
{
    QString out;
    out.reserve(normalizedKey.size());
    for (const QChar c : normalizedKey) {
        if (c.isLetterOrNumber())
            out.append(c);
    }
    return out;
}
} // namespace

WuWaDialogueDb::WuWaDialogueDb(const QString &filePath)
    : m_filePath(filePath)
{
}

bool WuWaDialogueDb::loadDialogueDb(const QString &path)
{
    m_filePath = path;
    return loadDialogueDb();
}

bool WuWaDialogueDb::loadDialogueDb()
{
    if (m_filePath.isEmpty())
        return true; // No DB configured; empty DB, not an error.
    QFile file(m_filePath);
    if (!file.exists())
        return true; // Seed file not installed yet; DB stays empty.
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject())
        return false;
    const QJsonObject entries = doc.object().value(QStringLiteral("entries")).toObject();
    QMutexLocker locker(&m_mutex);
    m_entries.clear();
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        const QString key = TranslationCache::normalizeKey(it.key());
        if (!key.isEmpty())
            m_entries.insert(key, it.value().toString());
    }
    rebuildIndexLocked();
    return true;
}

bool WuWaDialogueDb::loadChunkFile(const QString &path)
{
    QFile file(path);
    if (!file.exists())
        return true; // A chunk file missing from the cache is skipped.
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject())
        return false;
    const QJsonObject entries = doc.object().value(QStringLiteral("entries")).toObject();
    QMutexLocker locker(&m_mutex);
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        const QString key = TranslationCache::normalizeKey(it.key());
        if (!key.isEmpty())
            m_entries.insert(key, it.value().toString());
    }
    rebuildIndexLocked();
    return true;
}

void WuWaDialogueDb::clear()
{
    QMutexLocker locker(&m_mutex);
    m_entries.clear();
    m_firstTwoWordsIndex.clear();
    m_squashedIndex.clear();
}

bool WuWaDialogueDb::lookup(const QString &englishText, QString &translated) const
{
    const QString key = TranslationCache::normalizeKey(englishText);
    if (key.isEmpty())
        return false;
    QMutexLocker locker(&m_mutex);
    const auto it = m_entries.constFind(key);
    if (it == m_entries.constEnd())
        return false;
    translated = it.value();
    return true;
}

bool WuWaDialogueDb::lookupFuzzy(const QString &englishText, QString &translated) const
{
    const QString key = TranslationCache::normalizeKey(englishText);
    if (key.isEmpty())
        return false;
    QMutexLocker locker(&m_mutex);
    // 1) Exact on the normalized key.
    const auto exact = m_entries.constFind(key);
    if (exact != m_entries.constEnd()) {
        translated = exact.value();
        return true;
    }
    // 2) Exact on the squashed form (OCR punctuation/space noise).
    const QString squashed = squashKey(key);
    if (!squashed.isEmpty()) {
        const auto squashedHit = m_squashedIndex.constFind(squashed);
        if (squashedHit != m_squashedIndex.constEnd()) {
            translated = m_entries.value(squashedHit.value());
            if (!translated.isEmpty())
                return true;
        }
    }
    // 3) Levenshtein ratio among entries sharing the first two words.
    // The index keeps the candidate set tiny even for a 216k-entry DB.
    const auto candidates = m_firstTwoWordsIndex.value(firstTwoWords(key));
    double bestRatio = 0.0;
    QString bestTranslation;
    for (const QString &candidate : candidates) {
        const double ratio = similarityRatio(key, candidate);
        if (ratio > bestRatio) {
            bestRatio = ratio;
            bestTranslation = m_entries.value(candidate);
        }
    }
    if (bestRatio >= MinFuzzyRatio && !bestTranslation.isEmpty()) {
        translated = bestTranslation;
        return true;
    }
    return false;
}

double WuWaDialogueDb::similarityRatio(const QString &a, const QString &b)
{
    const int n = a.size();
    const int m = b.size();
    if (n == 0 && m == 0)
        return 1.0;
    if (n == 0 || m == 0)
        return 0.0;
    // Two-row Levenshtein; strings are dialogue lines (short).
    QList<int> previous(m + 1);
    QList<int> current(m + 1);
    for (int j = 0; j <= m; ++j)
        previous[j] = j;
    for (int i = 1; i <= n; ++i) {
        current[0] = i;
        for (int j = 1; j <= m; ++j) {
            const int cost = a.at(i - 1) == b.at(j - 1) ? 0 : 1;
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1,
                                   previous[j - 1] + cost});
        }
        previous.swap(current);
    }
    const int distance = previous[m];
    return 1.0 - static_cast<double>(distance) / std::max(n, m);
}

QString WuWaDialogueDb::firstTwoWords(const QString &normalizedKey)
{
    const int firstSpace = normalizedKey.indexOf(QChar(u' '));
    if (firstSpace < 0)
        return normalizedKey;
    const int secondSpace = normalizedKey.indexOf(QChar(u' '), firstSpace + 1);
    if (secondSpace < 0)
        return normalizedKey;
    return normalizedKey.left(secondSpace);
}

void WuWaDialogueDb::rebuildIndexLocked()
{
    m_firstTwoWordsIndex.clear();
    m_squashedIndex.clear();
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        m_firstTwoWordsIndex[firstTwoWords(it.key())].append(it.key());
        // First key wins on a squashed collision (duplicate lines with
        // different punctuation normalize to the same entry anyway).
        m_squashedIndex.insert(squashKey(it.key()), it.key());
    }
}

int WuWaDialogueDb::size() const
{
    QMutexLocker locker(&m_mutex);
    return m_entries.size();
}
