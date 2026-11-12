// TranslationEngine: the QML-facing translation controller.
//
// Exposed to QML as the context property "translationEngine". QML reads the
// properties below and calls onStreamFrame() indirectly: ApplicationStartup
// connects NativeStreamRuntime::screenshotReady to onStreamFrame() (in
// addition to the existing saveStreamScreenshot connect, which is untouched).
//
// Pipeline (onStreamFrame, GUI thread only):
//   1. Ignore when disabled or the image is null/empty.
//   2. Crop dialogueRect (normalized 0..1) and SHA-256-hash the crop.
//   3. Dialogue presets only proceed once the hash is unchanged for
//      stabilityDelayMs (QTimer restarts on every hash change). The
//      full-screen preset never stabilizes (HUD/animation pixels change
//      every frame), so it submits at most once per 2s instead.
//   4. Cache lookup first: WuWa dialogue DB, then the memory cache.
//      A hit emits translationReady immediately (lastCacheHit=true) with
//      no network at all.
//   5. Otherwise a job goes to TranslationWorker (own thread): OCR with
//      the selected engine, then translate with the selected provider,
//      then insert into the cache and emit translationReady. Streaming
//      providers (custom API) also report partial text through
//      jobProgress while generating, so the overlay fills in live; the
//      final jobCompleted result always replaces the partial.
// All network/OCR work happens in the worker thread; the GUI thread never
// blocks. On failure lastError is set, the previous translation is kept
// for 2s, and no garbage text is ever emitted.
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPair>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QVariant>

#include "translation/CustomApi.h"
#include "translation/TranslationCache.h"
#include "translation/TranslationWorker.h"
#include "translation/WuWaDialogueDb.h"

class TranslationEngine : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    // v5: live OCR needs a vision model key (Gemini key or custom-API
    // key). Without one there is no OCR engine that can read the screen,
    // so the feature refuses to run blind — the Settings toggle stays
    // locked until a key is saved.
    Q_PROPERTY(bool visionKeyConfigured READ visionKeyConfigured NOTIFY visionKeyChanged)
    Q_PROPERTY(QString sourceLanguage READ sourceLanguage WRITE setSourceLanguage NOTIFY sourceLanguageChanged)
    Q_PROPERTY(QString targetLanguage READ targetLanguage WRITE setTargetLanguage NOTIFY targetLanguageChanged)
    Q_PROPERTY(QString ocrEngine READ ocrEngine WRITE setOcrEngine NOTIFY ocrEngineChanged)
    Q_PROPERTY(QString translationProvider READ translationProvider WRITE setTranslationProvider NOTIFY translationProviderChanged)
    Q_PROPERTY(QString overlayMode READ overlayMode WRITE setOverlayMode NOTIFY overlayModeChanged)
    Q_PROPERTY(int stabilityDelayMs READ stabilityDelayMs WRITE setStabilityDelayMs NOTIFY stabilityDelayMsChanged)
    Q_PROPERTY(int fontSizePx READ fontSizePx WRITE setFontSizePx NOTIFY fontSizePxChanged)
    Q_PROPERTY(bool showOriginal READ showOriginal WRITE setShowOriginal NOTIFY showOriginalChanged)
    Q_PROPERTY(bool cacheEnabled READ cacheEnabled WRITE setCacheEnabled NOTIFY cacheEnabledChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString lastTranslatedText READ lastTranslatedText NOTIFY translationTextChanged)
    Q_PROPERTY(QString lastOriginalText READ lastOriginalText NOTIFY translationTextChanged)
    Q_PROPERTY(QRectF dialogueRect READ dialogueRect WRITE setDialogueRect NOTIFY dialogueRectChanged)
    // "full-screen" (whole frame — every line of text: dialogue, quests,
    // chat, menus) is the default since v4.1: the old default
    // "wuthering-waves" (dialogue bottom bar) missed almost all game text,
    // so users saw no translation at all on quest/voiceline/chat screens.
    // "generic" is the lower third. migrateDialoguePreset() moves a saved
    // old default to full-screen (see the .cpp).
    Q_PROPERTY(QString dialoguePreset READ dialoguePreset WRITE setDialoguePreset NOTIFY dialoguePresetChanged)
    Q_PROPERTY(qint64 lastOcrMs READ lastOcrMs NOTIFY translationTextChanged)
    Q_PROPERTY(qint64 lastTranslateMs READ lastTranslateMs NOTIFY translationTextChanged)
    Q_PROPERTY(bool lastCacheHit READ lastCacheHit NOTIFY translationTextChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY statusChanged)
    Q_PROPERTY(bool hasTranslation READ hasTranslation NOTIFY hasTranslationChanged)
    // One entry per translated line: {original, translated, x, y, w, h}
    // with x/y/w/h normalized 0..1 in full-frame coordinates. The overlay
    // draws one box per entry, over the English line it replaces.
    Q_PROPERTY(QVariantList translationBlocks READ translationBlocks NOTIFY translationBlocksChanged)
    // Custom OpenAI-compatible API (any base + key + model the user
    // pastes). The key itself is write-only through setCustomApiKey();
    // QML only ever reads whether one is saved.
    Q_PROPERTY(QString customApiBase READ customApiBase WRITE setCustomApiBase NOTIFY customApiBaseChanged)
    Q_PROPERTY(QString customApiModel READ customApiModel WRITE setCustomApiModel NOTIFY customApiModelChanged)
    Q_PROPERTY(QStringList customApiModels READ customApiModels NOTIFY customApiModelsChanged)
    // Chunk mode (v4): translate from the pre-translated chunk database
    // (repo huyzoz/wuwa-vi-chunks) instead of an AI provider. The DB is
    // downloaded on demand and cached locally; chunk mode OFF is exactly
    // the v3 behavior.
    Q_PROPERTY(bool chunkEnabled READ chunkEnabled WRITE setChunkEnabled NOTIFY chunkEnabledChanged)
    Q_PROPERTY(QString chunkGame READ chunkGame WRITE setChunkGame NOTIFY chunkGameChanged)
    Q_PROPERTY(QStringList chunkGames READ chunkGames NOTIFY chunkGameChanged)
    Q_PROPERTY(QString chunkGameTitle READ chunkGameTitle NOTIFY chunkGameInfoChanged)
    Q_PROPERTY(QString chunkGameVersion READ chunkGameVersion NOTIFY chunkGameInfoChanged)
    Q_PROPERTY(QString chunkGameWordsText READ chunkGameWordsText NOTIFY chunkGameInfoChanged)
    Q_PROPERTY(QString chunkGameStringsText READ chunkGameStringsText NOTIFY chunkGameInfoChanged)
    Q_PROPERTY(QString chunkTranslatedText READ chunkTranslatedText NOTIFY chunkGameInfoChanged)
    Q_PROPERTY(QString chunkRepoUrl READ chunkRepoUrl NOTIFY chunkGameInfoChanged)
    Q_PROPERTY(QString chunkStatus READ chunkStatus NOTIFY chunkStatusChanged)
    Q_PROPERTY(double chunkProgress READ chunkProgress NOTIFY chunkProgressChanged)
    Q_PROPERTY(int chunkDbSize READ chunkDbSize NOTIFY chunkDbSizeChanged)
    // True once the selected game's chunk files are on disk (downloaded),
    // regardless of whether chunk mode is currently on. The Settings page
    // uses it to show "Đã tải" and lock the download button.
    Q_PROPERTY(bool chunkDownloaded READ chunkDownloaded NOTIFY chunkDbSizeChanged)
    Q_PROPERTY(bool lastChunkHit READ lastChunkHit NOTIFY translationTextChanged)

public:
    explicit TranslationEngine(QObject *parent = nullptr);
    ~TranslationEngine() override;

    // Pure, unit-testable helpers (no instance state).
    // Crops the normalized rect (clamped to the image) or returns a null
    // image when the rect is empty/out of bounds.
    static QImage cropDialogueImage(const QImage &image, const QRectF &normalizedRect);
    // SHA-256 hex of the crop's ARGB32 pixels; "" for a null/empty image.
    static QString cropHash(const QImage &image);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);
    QString sourceLanguage() const { return m_sourceLanguage; }
    void setSourceLanguage(const QString &language);
    QString targetLanguage() const { return m_targetLanguage; }
    void setTargetLanguage(const QString &language);
    QString ocrEngine() const { return m_ocrEngine; }
    void setOcrEngine(const QString &engine);
    QString translationProvider() const { return m_translationProvider; }
    void setTranslationProvider(const QString &provider);
    QString overlayMode() const { return m_overlayMode; }
    void setOverlayMode(const QString &mode);
    int stabilityDelayMs() const { return m_stabilityDelayMs; }
    void setStabilityDelayMs(int ms);
    int fontSizePx() const { return m_fontSizePx; }
    void setFontSizePx(int px);
    bool showOriginal() const { return m_showOriginal; }
    void setShowOriginal(bool show);
    bool cacheEnabled() const { return m_cacheEnabled; }
    void setCacheEnabled(bool enabled);
    QString statusText() const { return m_statusText; }
    QString lastTranslatedText() const { return m_lastTranslatedText; }
    QString lastOriginalText() const { return m_lastOriginalText; }
    QRectF dialogueRect() const { return m_dialogueRect; }
    void setDialogueRect(const QRectF &rect);
    QString dialoguePreset() const { return m_dialoguePreset; }
    // Normalized rect for a preset, or a null rect for an unknown preset.
    static QRectF dialogueRectForPreset(const QString &preset);
    // Migrates a saved preset to the current default: the old shipped
    // default "wuthering-waves" becomes "full-screen" (it was never a
    // deliberate user choice, just the code default); "generic" and
    // "full-screen" pass through; empty/unknown become "full-screen".
    // Pure, unit-testable.
    static QString migrateDialoguePreset(const QString &saved);
    // Converts worker blocks to the QML-facing QVariantList of maps
    // {original, translated, x, y, w, h}. Pure, unit-testable.
    static QVariantList blocksToVariantList(const QList<TranslatedBlock> &blocks);
    // One block covering the whole rect (used when only flat text exists,
    // e.g. a cache hit with no stored blocks). Pure, unit-testable.
    static QVariantList singleBlockVariantList(const QString &original, const QString &translated,
                                               const QRectF &rect);
    qint64 lastOcrMs() const { return m_lastOcrMs; }
    qint64 lastTranslateMs() const { return m_lastTranslateMs; }
    bool lastCacheHit() const { return m_lastCacheHit; }
    QString lastError() const { return m_lastError; }
    bool hasTranslation() const { return m_hasTranslation; }
    QVariantList translationBlocks() const { return m_translationBlocks; }
    QString customApiBase() const { return m_customApiBase; }
    void setCustomApiBase(const QString &base);
    QString customApiModel() const { return m_customApiModel; }
    void setCustomApiModel(const QString &model);
    QStringList customApiModels() const { return m_customApiModels; }

    // Chunk mode (v4). chunkGames() is the list of games with a chunk
    // database; today only Wuthering Waves, built as a list so future
    // games slot in. The detail strings come from the shipped catalog
    // until the downloaded manifest refreshes them.
    bool chunkEnabled() const { return m_chunkEnabled; }
    void setChunkEnabled(bool enabled);
    QString chunkGame() const { return m_chunkGame; }
    void setChunkGame(const QString &game);
    QStringList chunkGames() const;
    QString chunkGameTitle() const { return m_chunkGameTitle; }
    QString chunkGameVersion() const { return m_chunkVersion; }
    QString chunkGameWordsText() const { return m_chunkGameWordsText; }
    QString chunkGameStringsText() const { return m_chunkGameStringsText; }
    QString chunkTranslatedText() const { return m_chunkTranslatedText; }
    QString chunkRepoUrl() const { return m_chunkRepoUrl; }
    QString chunkStatus() const { return m_chunkStatus; }
    double chunkProgress() const { return m_chunkProgress; }
    int chunkDbSize() const { return m_chunkDb.size(); }
    // True only when the chunk DB on disk holds the full set the
    // manifest promises (translatedStrings). A partial download is NOT
    // "downloaded" — the button stays live so it can be re-downloaded.
    bool chunkDownloaded() const { return isChunkDownloadComplete(m_chunkDb.size(), m_chunkExpectedStrings); }
    // Pure, unit-testable: complete iff the manifest promises at least
    // one string and the DB holds at least that many.
    static bool isChunkDownloadComplete(int dbSize, int expectedStrings);
    bool lastChunkHit() const { return m_lastChunkHit; }

    Q_INVOKABLE void onStreamFrame(const QImage &image);
    Q_INVOKABLE void requestTestTranslation(const QString &englishText);
    Q_INVOKABLE void clearCache();
    // Downloads the chunk database for the selected game (manifest +
    // chunk files) on the worker thread. Progress arrives via
    // chunkProgressChanged / chunkStatusChanged; completion reloads the
    // DB from the local cache.
    Q_INVOKABLE void downloadChunks();
    Q_INVOKABLE void setGeminiApiKey(const QString &key);
    Q_INVOKABLE bool hasGeminiApiKey() const;
    Q_INVOKABLE void setCustomApiKey(const QString &key);
    Q_INVOKABLE bool hasCustomApiKey() const;
    // v5: true when a vision model can actually read the screen — a
    // Gemini key OR a custom-API key is saved. Either one satisfies the
    // mandatory-key requirement.
    bool visionKeyConfigured() const;
    // Lists the models of the configured custom base (GET {base}/models
    // with the saved key). Result arrives via customApiModelsChanged /
    // statusText; the model list itself is never persisted.
    Q_INVOKABLE void fetchCustomApiModels();
    // "wuthering-waves" (dialogue bottom bar) or "generic" (lower third).
    Q_INVOKABLE void setDialoguePreset(const QString &preset);

signals:
    void translationReady(const QString &original, const QString &translated, const QRectF &sourceRect);
    void translationCleared();
    void translationTextChanged(); // parameterless NOTIFY for last* properties
    void statusChanged();
    void enabledChanged();
    void sourceLanguageChanged();
    void targetLanguageChanged();
    void ocrEngineChanged();
    void translationProviderChanged();
    void overlayModeChanged();
    void stabilityDelayMsChanged();
    void fontSizePxChanged();
    void showOriginalChanged();
    void cacheEnabledChanged();
    void dialogueRectChanged();
    void dialoguePresetChanged();
    void hasTranslationChanged();
    void translationBlocksChanged();
    void customApiBaseChanged();
    void customApiModelChanged();
    void customApiModelsChanged();
    void chunkEnabledChanged();
    void chunkGameChanged();
    void chunkGameInfoChanged();
    void chunkStatusChanged();
    void chunkProgressChanged();
    void chunkDbSizeChanged();
    void visionKeyChanged();
    // Emitted by the frame pump while translation is enabled. Application
    // Startup connects it to NativeStreamRuntime::requestScreenshot(): the
    // screenshotReady signal only fires for an explicit screenshot request,
    // so without this pump the engine receives NO frames during gameplay
    // and live OCR never runs (the v1 bug).
    void frameRequested();

private slots:
    void onStabilityTimeout();
    void onWorkerJobCompleted(const TranslationJobResult &result);
    void onWorkerJobProgress(const QString &original, const QString &partialTranslated,
                             const QRectF &sourceRect, bool testJob);
    void onWorkerModelsFetched(const QStringList &models, const QString &error);
    void onChunkDownloadProgress(int done, int total);
    void onChunkDownloadFinished(const QString &error);
    void onKeepPreviousExpired();
    void onFrameTimerTimeout();

private:
    void setStatus(const QString &status);
    void setError(const QString &error);
    // Current custom-API config for jobs (key never leaves this class
    // except into a job / the worker, and is never logged).
    CustomApiConfig customApiConfig() const;
    // Pushes the current custom-API config to the worker thread (queued).
    void syncCustomApiToWorker();
    // Loads the downloaded chunk files for the newest cached version
    // into m_chunkDb (no-op when nothing is cached). Also refreshes the
    // detail strings from the cached manifest.
    void loadCachedChunks();
    // v5: starts the full chunk download when chunk mode is on and the
    // DB is not complete (and no download is already running). Called
    // from the ctor, setEnabled, and setChunkEnabled — the download is
    // mandatory, not something the user has to remember to press.
    void ensureChunksDownloaded();
    void setChunkStatus(const QString &status);
    // Refreshes the detail-card strings from the shipped-known catalog
    // for the selected game.
    void applyChunkCatalog();
    // Local cache dir for the selected game's chunks:
    // <CacheLocation>/wuwa-chunks/<game>.
    QString chunkGameCacheBase() const;
    // Snaps jittering OCR rects to their previously stable position
    // (see m_stableRects). Pure on the given blocks except for the
    // stability bookkeeping.
    QVariantList applyStableRects(const QVariantList &blocks);
    // Cache-first submit of one stable crop to the worker (shared by the
    // stability-timeout path and the full-screen throttle path).
    void submitCrop(const QImage &crop, const QString &hash, const QRectF &rect);
    void emitTranslation(const QString &original, const QString &translated,
                         const QRectF &sourceRect, qint64 ocrMs, qint64 translateMs,
                         bool cacheHit, const QVariantList &blocks = {},
                         bool chunkHit = false);
    // Streaming partial: updates last* fields (+ the live overlay, one
    // block over the source rect) or the TEST section only, but never the
    // cache. The final jobCompleted result always replaces it.
    void setPartialTranslation(const QString &original, const QString &partial,
                               const QRectF &sourceRect, bool testJob);
    // TEST-section only: updates last* fields + status, but never touches
    // the live overlay (hasTranslation / translationBlocks /
    // translationReady). A "Dịch thử" result must never stick on the
    // stream as a permanent subtitle (the v1 bug).
    void setTestResult(const QString &original, const QString &translated,
                       qint64 ocrMs, qint64 translateMs, bool cacheHit);
    void clearTranslation();

    bool m_enabled = false; // OFF by default: translation is opt-in
    QString m_sourceLanguage = QStringLiteral("en");
    QString m_targetLanguage = QStringLiteral("vi");
    QString m_ocrEngine = QStringLiteral("gemini-vision");
    QString m_translationProvider = QStringLiteral("google");
    QString m_overlayMode = QStringLiteral("inplace");
    int m_stabilityDelayMs = 400;
    int m_fontSizePx = 22;
    bool m_showOriginal = false;
    bool m_cacheEnabled = true;
    QString m_statusText;
    QString m_lastTranslatedText;
    QString m_lastOriginalText;
    QRectF m_dialogueRect = QRectF(0.05, 0.72, 0.90, 0.24);
    QString m_dialoguePreset = QStringLiteral("wuthering-waves");
    qint64 m_lastOcrMs = 0;
    qint64 m_lastTranslateMs = 0;
    bool m_lastCacheHit = false;
    QString m_lastError;
    bool m_hasTranslation = false;
    QString m_geminiApiKey; // persisted locally via QSettings only; never logged, never committed
    QString m_customApiBase; // persisted locally via QSettings only
    QString m_customApiKey;  // persisted locally via QSettings only; never logged, never committed
    QString m_customApiModel; // persisted locally via QSettings only
    QStringList m_customApiModels; // fetched live from {base}/models; never persisted

    TranslationCache m_cache;
    WuWaDialogueDb m_dialogueDb;
    // Chunk database (v4): downloaded chunk files, loaded lazily when
    // chunk mode is on. The worker looks lines up in this instance.
    WuWaDialogueDb m_chunkDb;
    bool m_chunkEnabled = false;
    QString m_chunkGame = QStringLiteral("wuthering-waves");
    QString m_chunkVersion;
    QString m_chunkGameTitle = QStringLiteral("Wuthering Waves");
    QString m_chunkGameWordsText;
    QString m_chunkGameStringsText;
    QString m_chunkTranslatedText;
    QString m_chunkRepoUrl = QStringLiteral("https://github.com/huyzoz/wuwa-vi-chunks");
    QString m_chunkStatus;
    double m_chunkProgress = 0.0;
    bool m_lastChunkHit = false;
    // Expected unique strings, from the cached manifest's
    // translatedStrings (0 = unknown: no manifest on disk). Together
    // with m_chunkDb.size() it decides chunkDownloaded().
    int m_chunkExpectedStrings = 0;
    // True while a chunk download is running on the worker thread, so a
    // second ensure/download call cannot start a duplicate download.
    bool m_chunkDownloading = false;
    // Coordinate memory (v4): normalized English line -> last stable
    // rect + how many consecutive frames agreed with it. A rect that
    // agrees twice is "stable" and later jitter snaps back to it, so
    // boxes stop shaking. Placement only; never touches the stream.
    struct StableRect
    {
        QRectF rect;
        int hits = 1;
    };
    static constexpr int MaxStableRects = 200;
    QHash<QString, StableRect> m_stableRects;
    QList<QString> m_stableRectOrder; // oldest first; drives eviction
    TranslationWorker *m_worker = nullptr; // no parent: lives in its own thread

    QTimer m_stabilityTimer;
    QTimer m_keepPreviousTimer;
    // Frame pump: while translation is enabled, request one screenshot
    // per interval so onStreamFrame actually receives frames during
    // gameplay. requestScreenshot() is a no-op when the stream is not
    // presenting, and returns false while one is already pending, so the
    // pump can never pile up requests or save files (saving is gated on
    // the user's own screenshot action in AppController).
    QTimer m_frameTimer;
    QVariantList m_translationBlocks;
    // Full-screen throttle: a full frame never sits still long enough for
    // the stability timer, so full-screen frames submit at most once per
    // FullScreenIntervalMs instead. Invalid until the first full-screen
    // frame, so the first frame submits immediately.
    QElapsedTimer m_fullScreenThrottle;
    QString m_pendingHash;
    QImage m_pendingCrop; // detached copy of the stable crop
    QRectF m_pendingRect;
    QString m_lastProcessedHash; // hash of the crop last sent to cache/worker
    // Crop-hash -> (original, translated, blocks). Lets a repeated
    // identical dialogue line hit the cache BEFORE any OCR/network, which
    // the text-keyed cache cannot do (it needs the English text first).
    // In-memory only; the text-keyed TranslationCache is the persisted one.
    struct HashCacheEntry
    {
        QString original;
        QString translated;
        QVariantList blocks;
    };
    static constexpr int MaxHashCacheEntries = 500;
    QHash<QString, HashCacheEntry> m_hashCache;
    QList<QString> m_hashOrder; // oldest first; drives hash-cache eviction
};
