#include "translation/TranslationEngine.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMetaObject>
#include <QSettings>
#include <QStandardPaths>
#include <QVariant>

namespace {
constexpr int KeepPreviousMs = 2000;
// Full-screen crops never stabilize: HUD pixels, animations and the game
// clock change every frame, so the stability timer would restart forever
// and OCR would never run. Full-screen mode throttles instead: at most
// one OCR job per interval, newest frame wins.
constexpr int FullScreenIntervalMs = 2000;
// Frame pump: one screenshot request per interval while translation is
// enabled. The native runtime fulfills a request on the next presented
// frame, so a 1s pump feeds the engine ~1 frame per second — enough for
// the stability gate (400ms) and the full-screen throttle (2s) — for the
// cost of one texture readback per second.
constexpr int FramePumpIntervalMs = 1000;

bool isFullScreenRect(const QRectF &rect)
{
    return rect.width() >= 0.99 && rect.height() >= 0.99;
}

QRectF clampNormalized(const QRectF &rect)
{
    QRectF clamped(qBound(0.0, rect.x(), 1.0), qBound(0.0, rect.y(), 1.0),
                   qBound(0.0, rect.width(), 1.0), qBound(0.0, rect.height(), 1.0));
    if (clamped.right() > 1.0)
        clamped.setWidth(1.0 - clamped.x());
    if (clamped.bottom() > 1.0)
        clamped.setHeight(1.0 - clamped.y());
    return clamped;
}

// Where the dialogue DB may live, in priority order. The shipped seed is
// src/translation/data/wuwa-dialogue-db.json; a deployed copy next to the
// executable or in AppData wins when present.
QString resolveDialogueDbPath()
{
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/wuwa-dialogue-db.json");
    if (QFile::exists(appData))
        return appData;
    const QString nextToExe = QCoreApplication::applicationDirPath()
        + QStringLiteral("/wuwa-dialogue-db.json");
    if (QFile::exists(nextToExe))
        return nextToExe;
    const QString inDataDir = QCoreApplication::applicationDirPath()
        + QStringLiteral("/data/wuwa-dialogue-db.json");
    if (QFile::exists(inDataDir))
        return inDataDir;
    // Bundled seed (Resources.cmake registers it under :/translation/).
    // Lowest priority: a user DB next to the exe or in AppData wins.
    if (QFile::exists(QStringLiteral(":/translation/wuwa-dialogue-db.json")))
        return QStringLiteral(":/translation/wuwa-dialogue-db.json");
    return appData; // default target: loadDialogueDb() tolerates a missing file
}
// Persist one translation setting under "translation/<key>" in QSettings.
// The Rust core owns ShellStore.settings and rejects unknown keys, so
// translation settings must never go through it (see constructor).
void saveTranslationSetting(const QString &key, const QVariant &value)
{
    QSettings settings;
    settings.setValue(QStringLiteral("translation/") + key, value);
}
} // namespace

TranslationEngine::TranslationEngine(QObject *parent)
    : QObject(parent)
    , m_worker(new TranslationWorker(nullptr)) // no parent: it moves to its own thread
{
    m_cache.load();
    m_dialogueDb.loadDialogueDb(resolveDialogueDbPath());

    m_stabilityTimer.setSingleShot(true);
    connect(&m_stabilityTimer, &QTimer::timeout, this, &TranslationEngine::onStabilityTimeout);
    m_keepPreviousTimer.setSingleShot(true);
    connect(&m_keepPreviousTimer, &QTimer::timeout, this, &TranslationEngine::onKeepPreviousExpired);
    m_frameTimer.setInterval(FramePumpIntervalMs);
    connect(&m_frameTimer, &QTimer::timeout, this, &TranslationEngine::onFrameTimerTimeout);

    connect(m_worker, &TranslationWorker::jobCompleted,
            this, &TranslationEngine::onWorkerJobCompleted);
    m_worker->start();
    // Gemini key, persisted locally only (QSettings, never in a file we
    // commit, never logged). Restored here so hasGeminiApiKey() is true
    // after a restart without re-entering the key.
    {
        const QSettings settings;
        m_geminiApiKey = settings.value(QStringLiteral("translation/geminiApiKey")).toString();
        if (!m_geminiApiKey.isEmpty())
            QMetaObject::invokeMethod(m_worker, "setGeminiApiKey", Qt::QueuedConnection,
                                      Q_ARG(QString, m_geminiApiKey));
    }
    // Translation settings are persisted by THIS engine in QSettings
    // (translation/<key>), never through ShellStore.settings: the Rust
    // core rejects unknown setting keys ("Unknown setting:
    // translation.enabled"), so a write through the core is rolled back
    // about a second later and the Settings toggle flips back off. The
    // QML page binds straight to this engine's properties.
    {
        const QSettings settings;
        m_enabled = settings.value(QStringLiteral("translation/enabled"), false).toBool();
        m_sourceLanguage = settings.value(QStringLiteral("translation/sourceLanguage"),
                                          QStringLiteral("en")).toString();
        m_targetLanguage = settings.value(QStringLiteral("translation/targetLanguage"),
                                          QStringLiteral("vi")).toString();
        m_ocrEngine = settings.value(QStringLiteral("translation/ocrEngine"),
                                     QStringLiteral("gemini-vision")).toString();
        m_translationProvider = settings.value(QStringLiteral("translation/translationProvider"),
                                               QStringLiteral("google")).toString();
        m_overlayMode = settings.value(QStringLiteral("translation/overlayMode"),
                                       QStringLiteral("inplace")).toString();
        m_stabilityDelayMs = settings.value(QStringLiteral("translation/stabilityDelayMs"), 400).toInt();
        m_fontSizePx = settings.value(QStringLiteral("translation/fontSizePx"), 22).toInt();
        m_showOriginal = settings.value(QStringLiteral("translation/showOriginal"), false).toBool();
        m_cacheEnabled = settings.value(QStringLiteral("translation/cacheEnabled"), true).toBool();
        m_dialoguePreset = settings.value(QStringLiteral("translation/dialoguePreset"),
                                          QStringLiteral("wuthering-waves")).toString();
        const QRectF presetRect = dialogueRectForPreset(m_dialoguePreset);
        if (!presetRect.isNull())
            m_dialogueRect = presetRect;
        else
            m_dialoguePreset = QStringLiteral("wuthering-waves");
        // Clamp anything a hand-edited config file may have corrupted.
        if (m_sourceLanguage.isEmpty())
            m_sourceLanguage = QStringLiteral("en");
        if (m_targetLanguage.isEmpty())
            m_targetLanguage = QStringLiteral("vi");
        if (!QStringList{QStringLiteral("gemini-vision"), QStringLiteral("tesseract"),
                         QStringLiteral("rapidocr"), QStringLiteral("windows-ocr")}
                 .contains(m_ocrEngine))
            m_ocrEngine = QStringLiteral("gemini-vision");
        if (!QStringList{QStringLiteral("google"), QStringLiteral("gemini")}
                 .contains(m_translationProvider))
            m_translationProvider = QStringLiteral("google");
        if (!QStringList{QStringLiteral("inplace"), QStringLiteral("subtitle"), QStringLiteral("off")}
                 .contains(m_overlayMode))
            m_overlayMode = QStringLiteral("inplace");
        m_stabilityDelayMs = qBound(0, m_stabilityDelayMs, 5000);
        m_fontSizePx = qBound(10, m_fontSizePx, 64);
    }
    // The pump was restored enabled from QSettings: start it. The
    // frameRequested connect in ApplicationStartup is wired after this
    // constructor returns, and the first timeout is 1s away, so no
    // request is ever emitted before the connect exists.
    if (m_enabled)
        m_frameTimer.start();
    setStatus(QStringLiteral("Translation ready"));
}

TranslationEngine::~TranslationEngine()
{
    m_worker->stop();
    delete m_worker;
    m_cache.save();
}

QImage TranslationEngine::cropDialogueImage(const QImage &image, const QRectF &normalizedRect)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0)
        return QImage();
    const QRectF rect = clampNormalized(normalizedRect);
    if (rect.width() <= 0.0 || rect.height() <= 0.0)
        return QImage();
    const int x = static_cast<int>(rect.x() * image.width());
    const int y = static_cast<int>(rect.y() * image.height());
    const int w = static_cast<int>(rect.width() * image.width());
    const int h = static_cast<int>(rect.height() * image.height());
    const QRect pixelRect = QRect(x, y, w, h).intersected(image.rect());
    if (pixelRect.isEmpty())
        return QImage();
    return image.copy(pixelRect); // detached copy: safe to hand across threads
}

QString TranslationEngine::cropHash(const QImage &image)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0)
        return QString();
    const QImage argb = image.convertedTo(QImage::Format_ARGB32);
    const QByteArrayView pixels(reinterpret_cast<const char *>(argb.constBits()),
                                static_cast<qsizetype>(argb.sizeInBytes()));
    return QString::fromLatin1(
        QCryptographicHash::hash(pixels, QCryptographicHash::Sha256).toHex());
}

void TranslationEngine::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    saveTranslationSetting(QStringLiteral("enabled"), enabled);
    if (!m_enabled) {
        m_stabilityTimer.stop();
        m_frameTimer.stop();
        m_pendingHash.clear();
        m_pendingCrop = QImage();
        m_worker->cancelPending();
    } else {
        m_frameTimer.start();
    }
    setStatus(m_enabled ? QStringLiteral("Translation enabled")
                        : QStringLiteral("Translation disabled"));
    emit enabledChanged();
}

void TranslationEngine::setSourceLanguage(const QString &language)
{
    if (language.isEmpty() || m_sourceLanguage == language)
        return;
    m_sourceLanguage = language;
    saveTranslationSetting(QStringLiteral("sourceLanguage"), language);
    emit sourceLanguageChanged();
}

void TranslationEngine::setTargetLanguage(const QString &language)
{
    if (language.isEmpty() || m_targetLanguage == language)
        return;
    m_targetLanguage = language;
    saveTranslationSetting(QStringLiteral("targetLanguage"), language);
    emit targetLanguageChanged();
}

void TranslationEngine::setOcrEngine(const QString &engine)
{
    static const QStringList valid = {QStringLiteral("gemini-vision"), QStringLiteral("tesseract"),
                                      QStringLiteral("rapidocr"), QStringLiteral("windows-ocr")};
    if (!valid.contains(engine) || m_ocrEngine == engine)
        return;
    m_ocrEngine = engine;
    saveTranslationSetting(QStringLiteral("ocrEngine"), engine);
    emit ocrEngineChanged();
}

void TranslationEngine::setTranslationProvider(const QString &provider)
{
    static const QStringList valid = {QStringLiteral("google"), QStringLiteral("gemini")};
    if (!valid.contains(provider) || m_translationProvider == provider)
        return;
    m_translationProvider = provider;
    saveTranslationSetting(QStringLiteral("translationProvider"), provider);
    emit translationProviderChanged();
}

void TranslationEngine::setOverlayMode(const QString &mode)
{
    static const QStringList valid = {QStringLiteral("inplace"), QStringLiteral("subtitle"),
                                      QStringLiteral("off")};
    if (!valid.contains(mode) || m_overlayMode == mode)
        return;
    m_overlayMode = mode;
    saveTranslationSetting(QStringLiteral("overlayMode"), mode);
    emit overlayModeChanged();
}

void TranslationEngine::setStabilityDelayMs(int ms)
{
    ms = qBound(0, ms, 5000);
    if (m_stabilityDelayMs == ms)
        return;
    m_stabilityDelayMs = ms;
    saveTranslationSetting(QStringLiteral("stabilityDelayMs"), ms);
    emit stabilityDelayMsChanged();
}

void TranslationEngine::setFontSizePx(int px)
{
    px = qBound(10, px, 64);
    if (m_fontSizePx == px)
        return;
    m_fontSizePx = px;
    saveTranslationSetting(QStringLiteral("fontSizePx"), px);
    emit fontSizePxChanged();
}

void TranslationEngine::setShowOriginal(bool show)
{
    if (m_showOriginal == show)
        return;
    m_showOriginal = show;
    saveTranslationSetting(QStringLiteral("showOriginal"), show);
    emit showOriginalChanged();
}

void TranslationEngine::setCacheEnabled(bool enabled)
{
    if (m_cacheEnabled == enabled)
        return;
    m_cacheEnabled = enabled;
    saveTranslationSetting(QStringLiteral("cacheEnabled"), enabled);
    emit cacheEnabledChanged();
}

void TranslationEngine::setDialogueRect(const QRectF &rect)
{
    const QRectF clamped = clampNormalized(rect);
    if (m_dialogueRect == clamped)
        return;
    m_dialogueRect = clamped;
    emit dialogueRectChanged();
}

QRectF TranslationEngine::dialogueRectForPreset(const QString &preset)
{
    if (preset == QStringLiteral("wuthering-waves"))
        return QRectF(0.05, 0.72, 0.90, 0.24);
    if (preset == QStringLiteral("generic"))
        return QRectF(0.05, 0.60, 0.90, 0.30);
    if (preset == QStringLiteral("full-screen"))
        return QRectF(0, 0, 1, 1);
    return QRectF();
}

void TranslationEngine::setDialoguePreset(const QString &preset)
{
    const QRectF rect = dialogueRectForPreset(preset);
    if (rect.isNull()) {
        setError(QStringLiteral("Unknown dialogue preset"));
        return;
    }
    const bool changed = m_dialoguePreset != preset;
    m_dialoguePreset = preset;
    if (changed) {
        saveTranslationSetting(QStringLiteral("dialoguePreset"), preset);
        emit dialoguePresetChanged();
    }
    setDialogueRect(rect);
    // NOTE: full-screen used to force overlayMode to "subtitle" here
    // because one in-place box over the whole frame would hide the game.
    // With per-line blocks the in-place mode draws one small box per
    // English line instead, so full-screen + inplace now works and the
    // forced switch is gone (v2).
}

void TranslationEngine::setGeminiApiKey(const QString &key)
{
    if (m_geminiApiKey == key)
        return;
    m_geminiApiKey = key;
    // Persist locally only (QSettings: registry on Windows, ini elsewhere).
    // Never written to the repo, never logged, never sent anywhere except
    // to Google in the x-goog-api-key header.
    QSettings settings;
    if (key.isEmpty())
        settings.remove(QStringLiteral("translation/geminiApiKey"));
    else
        settings.setValue(QStringLiteral("translation/geminiApiKey"), key);
    QMetaObject::invokeMethod(m_worker, "setGeminiApiKey", Qt::QueuedConnection,
                              Q_ARG(QString, key));
}

bool TranslationEngine::hasGeminiApiKey() const
{
    return !m_geminiApiKey.isEmpty();
}

void TranslationEngine::clearCache()
{
    m_cache.clear();
    m_cache.save();
    setStatus(QStringLiteral("Translation cache cleared"));
}

void TranslationEngine::onStreamFrame(const QImage &image)
{
    if (!m_enabled)
        return;
    if (image.isNull() || image.width() <= 0 || image.height() <= 0)
        return;
    const QImage crop = cropDialogueImage(image, m_dialogueRect);
    if (crop.isNull())
        return;
    const QString hash = cropHash(crop);
    if (hash.isEmpty())
        return;
    if (hash == m_lastProcessedHash)
        return; // already translated this exact crop
    if (isFullScreenRect(m_dialogueRect)) {
        // Full-screen crops never stabilize (see FullScreenIntervalMs):
        // throttle instead of waiting for a still frame.
        if (m_fullScreenThrottle.isValid() && m_fullScreenThrottle.elapsed() < FullScreenIntervalMs)
            return;
        m_fullScreenThrottle.restart();
        submitCrop(crop, hash, m_dialogueRect);
        return;
    }
    if (hash != m_pendingHash) {
        // New content: (re)start the stability wait.
        m_pendingHash = hash;
        m_pendingCrop = crop;
        m_pendingRect = m_dialogueRect;
        m_stabilityTimer.start(m_stabilityDelayMs);
    }
    // Same hash as pending: the running timer already measures stability;
    // do not restart it.
}

void TranslationEngine::submitCrop(const QImage &crop, const QString &hash, const QRectF &rect)
{
    m_lastProcessedHash = hash;
    if (m_overlayMode == QStringLiteral("off"))
        return;

    // Cache first, keyed by crop hash: an identical dialogue line seen
    // before emits immediately with no OCR and no network at all.
    // (The text-keyed cache/DB lookup happens for direct-text jobs in
    // requestTestTranslation, and every worker result is inserted into
    // both caches, so the two stay in sync.)
    if (m_cacheEnabled) {
        const auto hit = m_hashCache.constFind(hash);
        if (hit != m_hashCache.constEnd()) {
            emitTranslation(hit->original, hit->translated, rect, 0, 0, true, hit->blocks);
            return;
        }
    }

    TranslationJob job;
    job.crop = crop;
    job.cropHash = hash;
    job.sourceRect = rect;
    job.ocrEngineId = m_ocrEngine;
    job.providerId = m_translationProvider;
    job.sourceLanguage = m_sourceLanguage;
    job.targetLanguage = m_targetLanguage;
    job.geminiApiKey = m_geminiApiKey;
    setStatus(QStringLiteral("Reading dialogue…"));
    QMetaObject::invokeMethod(m_worker, "submitJob", Qt::QueuedConnection,
                              Q_ARG(TranslationJob, job));
}

void TranslationEngine::onStabilityTimeout()
{
    if (!m_enabled || m_pendingHash.isEmpty() || m_pendingCrop.isNull())
        return;
    if (m_pendingHash == m_lastProcessedHash)
        return;
    const QImage crop = m_pendingCrop;
    const QString hash = m_pendingHash;
    const QRectF rect = m_pendingRect;
    m_pendingHash.clear();
    m_pendingCrop = QImage();
    submitCrop(crop, hash, rect);
}

void TranslationEngine::requestTestTranslation(const QString &englishText)
{
    const QString text = englishText.trimmed();
    if (text.isEmpty()) {
        setError(QStringLiteral("Nothing to translate"));
        return;
    }
    if (m_cacheEnabled) {
        QString cached;
        if (m_dialogueDb.lookup(text, cached) || m_cache.lookup(text, cached)) {
            // TEST result only: it must never reach the live overlay.
            setTestResult(text, cached, 0, 0, true);
            return;
        }
    }
    TranslationJob job;
    job.directText = text;
    job.testJob = true;
    job.sourceRect = m_dialogueRect;
    job.providerId = m_translationProvider;
    job.sourceLanguage = m_sourceLanguage;
    job.targetLanguage = m_targetLanguage;
    job.geminiApiKey = m_geminiApiKey;
    setStatus(QStringLiteral("Translating…"));
    QMetaObject::invokeMethod(m_worker, "submitJob", Qt::QueuedConnection,
                              Q_ARG(TranslationJob, job));
}

void TranslationEngine::onWorkerJobCompleted(const TranslationJobResult &result)
{
    if (!result.ok) {
        setError(result.error.isEmpty() ? QStringLiteral("Translation failed") : result.error);
        // A failed TEST job only shows in the TEST section: never touch
        // the live overlay for it.
        if (result.testJob)
            return;
        // Keep the previous translation visible for 2s, then clear it.
        // Never emit garbage text.
        if (m_hasTranslation)
            m_keepPreviousTimer.start(KeepPreviousMs);
        return;
    }
    if (result.testJob) {
        // TEST-section only: the Settings page shows it, the stream
        // overlay never sees it.
        setTestResult(result.original, result.translated,
                      result.ocrMs, result.translateMs, result.cacheHit);
        return;
    }
    if (result.original.isEmpty() && result.translated.isEmpty()) {
        // OCR found no dialogue text in the crop: nothing to show, and
        // not an error. Leave the current translation alone.
        return;
    }
    m_keepPreviousTimer.stop();
    if (m_cacheEnabled && !result.translated.isEmpty()) {
        m_cache.insert(result.original, result.translated);
        // Remember by crop hash too, so the exact same pixels next time
        // hit the cache before any OCR/network (see onStabilityTimeout).
        if (!result.cropHash.isEmpty()) {
            if (!m_hashCache.contains(result.cropHash))
                m_hashOrder.append(result.cropHash);
            m_hashCache.insert(result.cropHash,
                               HashCacheEntry{result.original, result.translated,
                                              blocksToVariantList(result.blocks)});
            while (m_hashCache.size() > MaxHashCacheEntries && !m_hashOrder.isEmpty())
                m_hashCache.remove(m_hashOrder.takeFirst());
        }
    }
    m_lastCacheHit = false;
    emitTranslation(result.original, result.translated, result.sourceRect,
                    result.ocrMs, result.translateMs, false,
                    blocksToVariantList(result.blocks));
}

void TranslationEngine::onKeepPreviousExpired()
{
    clearTranslation();
}

void TranslationEngine::onFrameTimerTimeout()
{
    if (m_enabled)
        emit frameRequested();
}

QVariantList TranslationEngine::blocksToVariantList(const QList<TranslatedBlock> &blocks)
{
    QVariantList list;
    list.reserve(blocks.size());
    for (const TranslatedBlock &block : blocks) {
        QVariantMap map;
        map.insert(QStringLiteral("original"), block.original);
        map.insert(QStringLiteral("translated"), block.translated);
        map.insert(QStringLiteral("x"), block.rect.x());
        map.insert(QStringLiteral("y"), block.rect.y());
        map.insert(QStringLiteral("w"), block.rect.width());
        map.insert(QStringLiteral("h"), block.rect.height());
        list.append(map);
    }
    return list;
}

QVariantList TranslationEngine::singleBlockVariantList(const QString &original,
                                                      const QString &translated,
                                                      const QRectF &rect)
{
    return blocksToVariantList({TranslatedBlock{original, translated, rect}});
}

void TranslationEngine::emitTranslation(const QString &original, const QString &translated,
                                        const QRectF &sourceRect, qint64 ocrMs, qint64 translateMs,
                                        bool cacheHit, const QVariantList &blocks)
{
    m_lastOriginalText = original;
    m_lastTranslatedText = translated;
    m_lastOcrMs = ocrMs;
    m_lastTranslateMs = translateMs;
    m_lastCacheHit = cacheHit;
    m_lastError.clear();
    m_translationBlocks = blocks.isEmpty()
        ? singleBlockVariantList(original, translated, sourceRect)
        : blocks;
    if (!m_hasTranslation) {
        m_hasTranslation = true;
        emit hasTranslationChanged();
    }
    setStatus(QStringLiteral("Translation ready"));
    emit translationBlocksChanged();
    emit translationTextChanged();
    emit translationReady(original, translated, sourceRect);
}

void TranslationEngine::setTestResult(const QString &original, const QString &translated,
                                      qint64 ocrMs, qint64 translateMs, bool cacheHit)
{
    m_lastOriginalText = original;
    m_lastTranslatedText = translated;
    m_lastOcrMs = ocrMs;
    m_lastTranslateMs = translateMs;
    m_lastCacheHit = cacheHit;
    m_lastError.clear();
    setStatus(QStringLiteral("Translation ready"));
    emit translationTextChanged();
}

void TranslationEngine::clearTranslation()
{
    if (!m_hasTranslation && m_lastTranslatedText.isEmpty())
        return;
    m_lastTranslatedText.clear();
    m_lastOriginalText.clear();
    m_translationBlocks.clear();
    m_hasTranslation = false;
    emit hasTranslationChanged();
    emit translationBlocksChanged();
    emit translationTextChanged();
    emit translationCleared();
}

void TranslationEngine::setStatus(const QString &status)
{
    if (m_statusText == status)
        return;
    m_statusText = status;
    emit statusChanged();
}

void TranslationEngine::setError(const QString &error)
{
    m_lastError = error;
    setStatus(error);
}
