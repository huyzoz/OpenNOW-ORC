#include "translation/TranslationEngine.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
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
// Chunk database (v4): local cache root for downloaded chunk files.
// Layout: <base>/<game>/<version>/manifest.json +
// <base>/<game>/<version>/chunks/vi_chunk_NNN.json.
QString chunkCacheBase()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/wuwa-chunks");
}

// Shipped-known chunk catalog (one game today). The downloaded manifest
// refreshes version/counts in loadCachedChunks; these values are what
// the Settings detail card shows before the first download.
struct ChunkGameCatalogEntry
{
    QString id;
    QString title;
    QString version;
    QString repoUrl;
    QString manifestUrl;
    int words = 0;
    int strings = 0;
};

ChunkGameCatalogEntry chunkCatalogEntry(const QString &id)
{
    if (id == QStringLiteral("wuthering-waves"))
        return {id, QStringLiteral("Wuthering Waves"), QStringLiteral("3.8"),
                QStringLiteral("https://github.com/huyzoz/wuwa-vi-chunks"),
                QStringLiteral("https://raw.githubusercontent.com/huyzoz/wuwa-vi-chunks/main/manifest.json"),
                3133145, 216384};
    return {};
}

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
    connect(m_worker, &TranslationWorker::jobProgress,
            this, &TranslationEngine::onWorkerJobProgress);
    connect(m_worker, &TranslationWorker::customApiModelsFetched,
            this, &TranslationEngine::onWorkerModelsFetched);
    connect(m_worker, &TranslationWorker::chunkDownloadProgress,
            this, &TranslationEngine::onChunkDownloadProgress);
    connect(m_worker, &TranslationWorker::chunkDownloadFinished,
            this, &TranslationEngine::onChunkDownloadFinished);
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
        // Custom API config, same rules: local only, never logged.
        // Synced to the worker after the validation block below.
        m_customApiBase = settings.value(QStringLiteral("translation/customApiBase")).toString();
        m_customApiKey = settings.value(QStringLiteral("translation/customApiKey")).toString();
        m_customApiModel = settings.value(QStringLiteral("translation/customApiModel")).toString();
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
        m_chunkEnabled = settings.value(QStringLiteral("translation/chunkEnabled"), false).toBool();
        m_chunkGame = settings.value(QStringLiteral("translation/chunkGame"),
                                     QStringLiteral("wuthering-waves")).toString();
        m_dialoguePreset = settings.value(QStringLiteral("translation/dialoguePreset"),
                                          QStringLiteral("full-screen")).toString();
        // v4.1 migration: the old shipped default was "wuthering-waves"
        // (dialogue strip only). A saved value of it is the old default,
        // not a deliberate choice — migrate it to full-screen once and
        // persist, so existing installs stop missing all non-dialogue
        // text. "generic" is a deliberate choice and stays.
        m_dialoguePreset = migrateDialoguePreset(m_dialoguePreset);
        saveTranslationSetting(QStringLiteral("dialoguePreset"), m_dialoguePreset);
        const QRectF presetRect = dialogueRectForPreset(m_dialoguePreset);
        if (!presetRect.isNull())
            m_dialogueRect = presetRect;
        else
            m_dialoguePreset = QStringLiteral("full-screen");
        // Clamp anything a hand-edited config file may have corrupted.
        if (m_sourceLanguage.isEmpty())
            m_sourceLanguage = QStringLiteral("en");
        if (m_targetLanguage.isEmpty())
            m_targetLanguage = QStringLiteral("vi");
        m_customApiBase = normalizeCustomApiBase(m_customApiBase);
        if (!QStringList{QStringLiteral("gemini-vision"), QStringLiteral("tesseract"),
                         QStringLiteral("rapidocr"), QStringLiteral("windows-ocr"),
                         QStringLiteral("custom-vision")}
                 .contains(m_ocrEngine))
            m_ocrEngine = QStringLiteral("gemini-vision");
        if (!QStringList{QStringLiteral("google"), QStringLiteral("gemini"),
                         QStringLiteral("custom")}
                 .contains(m_translationProvider))
            m_translationProvider = QStringLiteral("google");
        if (m_chunkGame != QStringLiteral("wuthering-waves"))
            m_chunkGame = QStringLiteral("wuthering-waves");
        if (!QStringList{QStringLiteral("inplace"), QStringLiteral("subtitle"), QStringLiteral("off")}
                 .contains(m_overlayMode))
            m_overlayMode = QStringLiteral("inplace");
        m_stabilityDelayMs = qBound(0, m_stabilityDelayMs, 5000);
        m_fontSizePx = qBound(10, m_fontSizePx, 64);
    }
    // Sync the (now normalized) custom-API config to the worker.
    if (!m_customApiBase.isEmpty() || !m_customApiKey.isEmpty() || !m_customApiModel.isEmpty())
        syncCustomApiToWorker();
    // Bundled overlay font (Be Vietnam Pro, SIL OFL 1.1): full Vietnamese
    // diacritics, game-fitting geometric sans. Loaded here so the
    // overlay's font.family resolves even on machines without the font
    // installed; everything else keeps Theme.bodyFont.
    for (const QString &font : {QStringLiteral(":/fonts/BeVietnamPro-Regular.ttf"),
                                QStringLiteral(":/fonts/BeVietnamPro-Medium.ttf"),
                                QStringLiteral(":/fonts/BeVietnamPro-SemiBold.ttf"),
                                QStringLiteral(":/fonts/BeVietnamPro-Bold.ttf")}) {
        if (QFontDatabase::addApplicationFont(font) < 0)
            qWarning() << "TranslationEngine: failed to load bundled font" << font;
    }
    // Chunk mode (v4): catalog strings from the shipped-known values,
    // worker pointed at the chunk DB, cached chunks loaded. The cache is
    // loaded ALWAYS (v4.1), not only when chunk mode is on: the Settings
    // detail card and the download state must reflect what is on disk
    // even when chunk mode is off — otherwise a downloaded DB still
    // reads "Chưa tải chunk" and the download button stays live.
    applyChunkCatalog();
    QMetaObject::invokeMethod(m_worker, "setChunkDb", Qt::QueuedConnection,
                              Q_ARG(WuWaDialogueDb *, &m_chunkDb));
    QMetaObject::invokeMethod(m_worker, "setChunkEnabled", Qt::QueuedConnection,
                              Q_ARG(bool, m_chunkEnabled));
    loadCachedChunks();
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
                                      QStringLiteral("rapidocr"), QStringLiteral("windows-ocr"),
                                      QStringLiteral("custom-vision")};
    if (!valid.contains(engine) || m_ocrEngine == engine)
        return;
    m_ocrEngine = engine;
    saveTranslationSetting(QStringLiteral("ocrEngine"), engine);
    emit ocrEngineChanged();
}

void TranslationEngine::setTranslationProvider(const QString &provider)
{
    static const QStringList valid = {QStringLiteral("google"), QStringLiteral("gemini"),
                                      QStringLiteral("custom")};
    if (!valid.contains(provider) || m_translationProvider == provider)
        return;
    m_translationProvider = provider;
    saveTranslationSetting(QStringLiteral("translationProvider"), provider);
    emit translationProviderChanged();
}

CustomApiConfig TranslationEngine::customApiConfig() const
{
    return CustomApiConfig{m_customApiBase, m_customApiKey, m_customApiModel};
}

void TranslationEngine::syncCustomApiToWorker()
{
    QMetaObject::invokeMethod(m_worker, "setCustomApiConfig", Qt::QueuedConnection,
                              Q_ARG(CustomApiConfig, customApiConfig()));
}

void TranslationEngine::setCustomApiBase(const QString &base)
{
    const QString normalized = normalizeCustomApiBase(base);
    if (m_customApiBase == normalized)
        return;
    m_customApiBase = normalized;
    saveTranslationSetting(QStringLiteral("customApiBase"), normalized);
    syncCustomApiToWorker();
    emit customApiBaseChanged();
}

void TranslationEngine::setCustomApiModel(const QString &model)
{
    const QString trimmed = model.trimmed();
    if (m_customApiModel == trimmed)
        return;
    m_customApiModel = trimmed;
    saveTranslationSetting(QStringLiteral("customApiModel"), trimmed);
    syncCustomApiToWorker();
    emit customApiModelChanged();
}

void TranslationEngine::setCustomApiKey(const QString &key)
{
    if (m_customApiKey == key)
        return;
    m_customApiKey = key;
    // Persist locally only (QSettings: registry on Windows, ini elsewhere).
    // Never written to the repo, never logged, never sent anywhere except
    // to the user's own base in the Authorization header.
    QSettings settings;
    if (key.isEmpty())
        settings.remove(QStringLiteral("translation/customApiKey"));
    else
        settings.setValue(QStringLiteral("translation/customApiKey"), key);
    syncCustomApiToWorker();
}

bool TranslationEngine::hasCustomApiKey() const
{
    return !m_customApiKey.isEmpty();
}

void TranslationEngine::fetchCustomApiModels()
{
    if (m_customApiBase.isEmpty()) {
        setError(QStringLiteral("Custom API base URL is empty"));
        return;
    }
    if (m_customApiKey.isEmpty()) {
        setError(QStringLiteral("Custom API key is empty"));
        return;
    }
    setStatus(QStringLiteral("Fetching models…"));
    QMetaObject::invokeMethod(m_worker, "fetchCustomApiModels", Qt::QueuedConnection,
                              Q_ARG(QString, m_customApiBase), Q_ARG(QString, m_customApiKey));
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

QStringList TranslationEngine::chunkGames() const
{
    return {QStringLiteral("wuthering-waves")};
}

void TranslationEngine::applyChunkCatalog()
{
    const ChunkGameCatalogEntry entry = chunkCatalogEntry(m_chunkGame);
    m_chunkGameTitle = entry.title;
    m_chunkVersion = entry.version;
    m_chunkRepoUrl = entry.repoUrl;
    const QLocale vi(QStringLiteral("vi_VN"));
    m_chunkGameWordsText = vi.toString(entry.words) + QStringLiteral(" từ");
    m_chunkGameStringsText = vi.toString(entry.strings) + QStringLiteral(" câu");
    emit chunkGameInfoChanged();
}

QString TranslationEngine::chunkGameCacheBase() const
{
    return chunkCacheBase() + QChar(u'/') + m_chunkGame;
}

void TranslationEngine::setChunkEnabled(bool enabled)
{
    if (m_chunkEnabled == enabled)
        return;
    m_chunkEnabled = enabled;
    saveTranslationSetting(QStringLiteral("chunkEnabled"), enabled);
    QMetaObject::invokeMethod(m_worker, "setChunkEnabled", Qt::QueuedConnection,
                              Q_ARG(bool, enabled));
    if (enabled) {
        if (m_chunkDb.size() == 0)
            loadCachedChunks();
    } else {
        setChunkStatus(QStringLiteral("Chunk mode off"));
    }
    emit chunkEnabledChanged();
}

void TranslationEngine::setChunkGame(const QString &game)
{
    if (game != QStringLiteral("wuthering-waves") || m_chunkGame == game)
        return;
    m_chunkGame = game;
    saveTranslationSetting(QStringLiteral("chunkGame"), game);
    applyChunkCatalog();
    if (m_chunkEnabled)
        loadCachedChunks();
    emit chunkGameChanged();
}

void TranslationEngine::downloadChunks()
{
    const ChunkGameCatalogEntry entry = chunkCatalogEntry(m_chunkGame);
    if (entry.manifestUrl.isEmpty()) {
        setError(QStringLiteral("No chunks for this game"));
        return;
    }
    m_chunkProgress = 0.0;
    emit chunkProgressChanged();
    setChunkStatus(QStringLiteral("Downloading chunks…"));
    QMetaObject::invokeMethod(m_worker, "startChunkDownload", Qt::QueuedConnection,
                              Q_ARG(QString, entry.manifestUrl),
                              Q_ARG(QString, chunkGameCacheBase()));
}

void TranslationEngine::setChunkStatus(const QString &status)
{
    if (m_chunkStatus == status)
        return;
    m_chunkStatus = status;
    emit chunkStatusChanged();
}

void TranslationEngine::loadCachedChunks()
{
    const QString base = chunkGameCacheBase();
    QDir dir(base);
    const QStringList versions = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    if (versions.isEmpty()) {
        setChunkStatus(QStringLiteral("Chunk not downloaded — press Download chunks"));
        return;
    }
    const QString version = versions.last();
    const QString versionDir = base + QChar(u'/') + version;
    QStringList files;
    QFile manifestFile(versionDir + QStringLiteral("/manifest.json"));
    if (manifestFile.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(manifestFile.readAll());
        manifestFile.close();
        if (doc.isObject()) {
            const QJsonObject manifest = doc.object();
            const int translated = manifest.value(QStringLiteral("translatedStrings")).toInt();
            const int total = manifest.value(QStringLiteral("uniqueStrings")).toInt();
            const QLocale vi(QStringLiteral("vi_VN"));
            if (translated > 0 && total > 0)
                m_chunkTranslatedText = QStringLiteral("Đã dịch %1 / %2 câu")
                    .arg(vi.toString(translated), vi.toString(total));
            for (const QJsonValue &value : manifest.value(QStringLiteral("files")).toArray()) {
                const QString file = value.toString();
                if (!file.isEmpty())
                    files.append(file);
            }
        }
    }
    if (files.isEmpty()) {
        // No (readable) manifest: load every chunk file in the dir.
        QDir chunksDir(versionDir + QStringLiteral("/chunks"));
        for (const QString &name : chunksDir.entryList({QStringLiteral("*.json")},
                                                       QDir::Files, QDir::Name))
            files.append(QStringLiteral("chunks/") + name);
    }
    m_chunkDb.clear();
    for (const QString &file : files)
        m_chunkDb.loadChunkFile(versionDir + QChar(u'/') + file);
    m_chunkVersion = version;
    const QLocale vi(QStringLiteral("vi_VN"));
    if (m_chunkDb.size() > 0)
        setChunkStatus(QStringLiteral("Downloaded %1 lines").arg(vi.toString(m_chunkDb.size())));
    else
        setChunkStatus(QStringLiteral("Chunk not downloaded — press Download chunks"));
    emit chunkGameInfoChanged();
    emit chunkDbSizeChanged();
}

void TranslationEngine::onChunkDownloadProgress(int done, int total)
{
    m_chunkProgress = total > 0 ? static_cast<double>(done) / total : 0.0;
    setChunkStatus(QStringLiteral("Downloading chunks… %1/%2").arg(done).arg(total));
    emit chunkProgressChanged();
}

void TranslationEngine::onChunkDownloadFinished(const QString &error)
{
    m_chunkProgress = error.isEmpty() ? 1.0 : 0.0;
    emit chunkProgressChanged();
    if (!error.isEmpty()) {
        setError(error);
        return;
    }
    loadCachedChunks();
}

QVariantList TranslationEngine::applyStableRects(const QVariantList &blocks)
{
    QVariantList out = blocks;
    for (QVariant &entry : out) {
        QVariantMap map = entry.toMap();
        const QString key = TranslationCache::normalizeKey(
            map.value(QStringLiteral("original")).toString());
        if (key.isEmpty())
            continue;
        const QRectF rect(map.value(QStringLiteral("x")).toDouble(),
                          map.value(QStringLiteral("y")).toDouble(),
                          map.value(QStringLiteral("w")).toDouble(),
                          map.value(QStringLiteral("h")).toDouble());
        auto it = m_stableRects.find(key);
        if (it == m_stableRects.end()) {
            if (m_stableRects.size() >= MaxStableRects && !m_stableRectOrder.isEmpty())
                m_stableRects.remove(m_stableRectOrder.takeFirst());
            m_stableRects.insert(key, StableRect{rect, 1});
            m_stableRectOrder.append(key);
            continue;
        }
        const QRectF stored = it->rect;
        const bool close = qAbs(rect.x() - stored.x()) < 0.02
            && qAbs(rect.y() - stored.y()) < 0.02
            && qAbs(rect.width() - stored.width()) < 0.02
            && qAbs(rect.height() - stored.height()) < 0.02;
        if (close) {
            it->hits += 1;
            if (it->hits >= 2) {
                map.insert(QStringLiteral("x"), stored.x());
                map.insert(QStringLiteral("y"), stored.y());
                map.insert(QStringLiteral("w"), stored.width());
                map.insert(QStringLiteral("h"), stored.height());
                entry = map;
            }
        } else {
            it->rect = rect;
            it->hits = 1;
        }
    }
    return out;
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

QString TranslationEngine::migrateDialoguePreset(const QString &saved)
{
    if (saved == QStringLiteral("generic") || saved == QStringLiteral("full-screen"))
        return saved;
    // "wuthering-waves" (the pre-v4.1 shipped default), empty, and
    // anything unknown all become the current default, full-screen.
    return QStringLiteral("full-screen");
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
    job.customApi = customApiConfig();
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
    // Chunk DB first (v4): a pre-translated line needs no provider call.
    if (m_chunkEnabled) {
        QString chunked;
        if (m_chunkDb.lookup(text, chunked) || m_chunkDb.lookupFuzzy(text, chunked)) {
            m_lastChunkHit = true;
            setTestResult(text, chunked, 0, 0, true);
            return;
        }
    }
    m_lastChunkHit = false;
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
    job.customApi = customApiConfig();
    setStatus(QStringLiteral("Translating…"));
    QMetaObject::invokeMethod(m_worker, "submitJob", Qt::QueuedConnection,
                              Q_ARG(TranslationJob, job));
}

void TranslationEngine::onWorkerJobProgress(const QString &original,
                                            const QString &partialTranslated,
                                            const QRectF &sourceRect, bool testJob)
{
    setPartialTranslation(original, partialTranslated, sourceRect, testJob);
}

void TranslationEngine::onWorkerModelsFetched(const QStringList &models, const QString &error)
{
    if (!error.isEmpty()) {
        setError(error);
        return;
    }
    m_customApiModels = models;
    emit customApiModelsChanged();
    if (m_customApiModel.isEmpty() && !models.isEmpty())
        setCustomApiModel(models.first());
    setStatus(QStringLiteral("Fetched %1 models").arg(models.size()));
}

void TranslationEngine::setPartialTranslation(const QString &original, const QString &partial,
                                              const QRectF &sourceRect, bool testJob)
{
    // Streaming partial: updates the visible text as it arrives, but
    // never the cache — only the final jobCompleted result is cached.
    // A TEST partial stays in the TEST section, like setTestResult.
    m_lastOriginalText = original;
    m_lastTranslatedText = partial;
    m_lastError.clear();
    setStatus(QStringLiteral("Translating…"));
    if (testJob) {
        emit translationTextChanged();
        return;
    }
    // Live overlay: one block over the source rect for now; the final
    // result replaces it with one box per English line.
    m_translationBlocks = singleBlockVariantList(original, partial, sourceRect);
    if (!m_hasTranslation) {
        m_hasTranslation = true;
        emit hasTranslationChanged();
    }
    emit translationBlocksChanged();
    emit translationTextChanged();
}

void TranslationEngine::onWorkerJobCompleted(const TranslationJobResult &result)
{
    m_lastChunkHit = result.chunkHit;
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
        // not an error. Leave the current translation alone, but say so
        // in the status (v4.1): a silent no-op is indistinguishable from
        // a dead pipeline, and the status is the only place the user can
        // tell the pump is running and the crop is simply empty.
        setStatus(QStringLiteral("No text in scanned area"));
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
                    blocksToVariantList(result.blocks), result.chunkHit);
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
                                        bool cacheHit, const QVariantList &blocks, bool chunkHit)
{
    m_lastOriginalText = original;
    m_lastTranslatedText = translated;
    m_lastOcrMs = ocrMs;
    m_lastTranslateMs = translateMs;
    m_lastCacheHit = cacheHit;
    m_lastChunkHit = chunkHit;
    m_lastError.clear();
    m_translationBlocks = applyStableRects(blocks.isEmpty()
        ? singleBlockVariantList(original, translated, sourceRect)
        : blocks);
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
