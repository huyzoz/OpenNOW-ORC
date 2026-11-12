#include "translation/TranslationWorker.h"
#include "translation/CustomApi.h"
#include "translation/OcrEngine.h"
#include "translation/TranslationHttp.h"
#include "translation/TranslationProvider.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMetaObject>
#include <QMutexLocker>

// ---------------------------------------------------------------------------
// TranslationJobQueue
// ---------------------------------------------------------------------------

bool TranslationJobQueue::submit(const TranslationJob &job)
{
    QMutexLocker locker(&m_mutex);
    if (!m_running.has_value()) {
        m_running = job;
        return true;
    }
    if (m_waiting.has_value())
        ++m_droppedStale;
    m_waiting = job;
    return true;
}

std::optional<TranslationJob> TranslationJobQueue::takeRunning()
{
    QMutexLocker locker(&m_mutex);
    if (m_running.has_value())
        return m_running; // already running; caller is processing it
    if (!m_waiting.has_value())
        return std::nullopt;
    m_running = std::move(m_waiting);
    m_waiting.reset();
    return m_running;
}

void TranslationJobQueue::finishRunning()
{
    QMutexLocker locker(&m_mutex);
    m_running.reset();
}

bool TranslationJobQueue::hasRunning() const
{
    QMutexLocker locker(&m_mutex);
    return m_running.has_value();
}

bool TranslationJobQueue::hasWaiting() const
{
    QMutexLocker locker(&m_mutex);
    return m_waiting.has_value();
}

void TranslationJobQueue::clear()
{
    QMutexLocker locker(&m_mutex);
    m_running.reset();
    m_waiting.reset();
}

void TranslationJobQueue::clearWaiting()
{
    QMutexLocker locker(&m_mutex);
    m_waiting.reset();
}

QRectF mapBlockRectToFrame(const QRectF &blockRect, const QRectF &sourceRect)
{
    const double x = qBound(0.0, sourceRect.x() + blockRect.x() * sourceRect.width(), 1.0);
    const double y = qBound(0.0, sourceRect.y() + blockRect.y() * sourceRect.height(), 1.0);
    const double w = qBound(0.0, blockRect.width() * sourceRect.width(), 1.0 - x);
    const double h = qBound(0.0, blockRect.height() * sourceRect.height(), 1.0 - y);
    return QRectF(x, y, w, h);
}

// ---------------------------------------------------------------------------
// TranslationWorker
// ---------------------------------------------------------------------------

TranslationWorker::TranslationWorker(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<TranslationJob>();
    qRegisterMetaType<TranslationJobResult>();
    qRegisterMetaType<TranslatedBlock>();
    qRegisterMetaType<CustomApiConfig>();
    // Needed for the queued setChunkDb(WuWaDialogueDb*) invoke from the
    // engine (WuWaDialogueDb is not a QObject).
    qRegisterMetaType<WuWaDialogueDb *>();
}

TranslationWorker::~TranslationWorker()
{
    stop();
}

void TranslationWorker::start()
{
    if (m_thread.isRunning())
        return;
    moveToThread(&m_thread);
    m_thread.start();
}

void TranslationWorker::stop()
{
    if (!m_thread.isRunning())
        return;
    // Cancel anything queued, then quit the event loop and wait. A job
    // already running finishes its current synchronous call first (bounded
    // by the 8s network timeout), then the thread exits. After the thread
    // has finished, move back to the calling thread so deletion is safe.
    QMetaObject::invokeMethod(this, "cancelPending", Qt::QueuedConnection);
    m_thread.quit();
    m_thread.wait();
    moveToThread(QThread::currentThread());
}

int TranslationWorker::droppedStaleJobs() const
{
    return m_queue.droppedStale();
}

void TranslationWorker::submitJob(const TranslationJob &job)
{
    // Runs in the worker thread (queued invocation from the GUI thread).
    TranslationJob effective = job;
    if (effective.geminiApiKey.isEmpty())
        effective.geminiApiKey = m_geminiApiKey;
    if (effective.customApi.baseUrl.isEmpty())
        effective.customApi.baseUrl = m_customApi.baseUrl;
    if (effective.customApi.apiKey.isEmpty())
        effective.customApi.apiKey = m_customApi.apiKey;
    if (effective.customApi.model.isEmpty())
        effective.customApi.model = m_customApi.model;
    m_queue.submit(effective);
    if (!m_processing)
        QMetaObject::invokeMethod(this, "processNext", Qt::QueuedConnection);
}

void TranslationWorker::setGeminiApiKey(const QString &key)
{
    // Runs in the worker thread; the key is never logged.
    m_geminiApiKey = key;
}

void TranslationWorker::setCustomApiConfig(const CustomApiConfig &config)
{
    // Runs in the worker thread; the key is never logged.
    m_customApi = config;
}

void TranslationWorker::setChunkDb(WuWaDialogueDb *db)
{
    // Runs in the worker thread. The DB is owned by the engine; only
    // const, mutex-guarded lookups happen here.
    m_chunkDb = db;
}

void TranslationWorker::setChunkEnabled(bool enabled)
{
    // Runs in the worker thread.
    m_chunkEnabled = enabled;
}

void TranslationWorker::startChunkDownload(const QString &manifestUrl, const QString &cacheDir)
{
    // Runs in the worker thread. Fetches the manifest, then downloads
    // each listed file one per event-loop turn (queued self-invocation)
    // so OCR/translate jobs keep running between downloads.
    m_chunkManifestUrl = manifestUrl;
    m_chunkBaseUrl = manifestUrl;
    if (m_chunkBaseUrl.endsWith(QStringLiteral("manifest.json")))
        m_chunkBaseUrl.chop(QStringLiteral("manifest.json").size());
    m_chunkCacheDir = cacheDir;
    m_chunkFiles.clear();
    m_chunkIndex = 0;
    m_chunkError.clear();
    m_chunkManifestBody.clear();
    m_chunkVersion.clear();
    const TranslationHttpResult result = TranslationHttp::get(manifestUrl);
    if (!result.ok()) {
        emit chunkDownloadFinished(result.error);
        return;
    }
    m_chunkManifestBody = result.body;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(result.body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        emit chunkDownloadFinished(QStringLiteral("Chunk manifest is not valid JSON"));
        return;
    }
    const QJsonObject manifest = doc.object();
    m_chunkVersion = manifest.value(QStringLiteral("version")).toString();
    for (const QJsonValue &value : manifest.value(QStringLiteral("files")).toArray()) {
        const QString file = value.toString();
        if (!file.isEmpty())
            m_chunkFiles.append(file);
    }
    if (m_chunkFiles.isEmpty() || m_chunkVersion.isEmpty()) {
        emit chunkDownloadFinished(QStringLiteral("Chunk manifest lists no files"));
        return;
    }
    emit chunkDownloadProgress(0, m_chunkFiles.size());
    QMetaObject::invokeMethod(this, "downloadNextChunkFile", Qt::QueuedConnection);
}

void TranslationWorker::downloadNextChunkFile()
{
    // Runs in the worker thread, one file per event-loop turn.
    if (m_chunkIndex >= m_chunkFiles.size()) {
        // Save the manifest next to the chunks: the engine reads it for
        // the version check and the detail card, without re-downloading.
        const QString manifestPath = m_chunkCacheDir + QChar(u'/') + m_chunkVersion
            + QStringLiteral("/manifest.json");
        QDir().mkpath(QFileInfo(manifestPath).path());
        QFile manifestFile(manifestPath);
        if (manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
            manifestFile.write(m_chunkManifestBody);
        emit chunkDownloadFinished(m_chunkError);
        return;
    }
    const QString relative = m_chunkFiles.at(m_chunkIndex);
    const QString url = m_chunkBaseUrl + relative;
    const QString destPath = m_chunkCacheDir + QChar(u'/') + m_chunkVersion
        + QChar(u'/') + relative;
    QDir().mkpath(QFileInfo(destPath).path());
    const TranslationHttpResult result = TranslationHttp::get(url);
    if (result.ok()) {
        QFile out(destPath);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            out.write(result.body);
        else if (m_chunkError.isEmpty())
            m_chunkError = QStringLiteral("Cannot write chunk file: ") + relative;
    } else if (m_chunkError.isEmpty()) {
        m_chunkError = result.error;
    }
    ++m_chunkIndex;
    emit chunkDownloadProgress(m_chunkIndex, m_chunkFiles.size());
    QMetaObject::invokeMethod(this, "downloadNextChunkFile", Qt::QueuedConnection);
}

void TranslationWorker::fetchCustomApiModels(const QString &base, const QString &apiKey)
{
    // Runs in the worker thread. Lists the base's real models
    // (GET {base}/models, key in the Authorization header); the result
    // comes back via customApiModelsFetched.
    const QString url = customApiModelsUrl(base);
    if (url.isEmpty()) {
        emit customApiModelsFetched({}, QStringLiteral("Custom API base URL is empty"));
        return;
    }
    if (apiKey.isEmpty()) {
        emit customApiModelsFetched({}, QStringLiteral("Custom API key is empty"));
        return;
    }
    const TranslationHttpResult result = TranslationHttp::request(
        QStringLiteral("GET"), url,
        {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + apiKey.toUtf8()}});
    if (!result.ok()) {
        emit customApiModelsFetched({}, result.error);
        return;
    }
    QString error;
    const QStringList models = parseCustomApiModels(result.body, error);
    emit customApiModelsFetched(models, error);
}

void TranslationWorker::cancelPending()
{
    // Runs in the worker thread. Drops the waiting job only; a running job
    // is left to finish (it is bounded by the network/OCR timeouts).
    m_queue.clearWaiting();
}

void TranslationWorker::processNext()
{
    // Runs in the worker thread. m_processing guards re-entrancy; the
    // queue itself decides running vs waiting.
    if (m_processing)
        return;
    std::optional<TranslationJob> job = m_queue.takeRunning();
    if (!job.has_value())
        return;
    m_processing = true;

    TranslationJobResult result;
    result.sourceRect = job->sourceRect;
    result.cropHash = job->cropHash;
    result.testJob = job->testJob; // set once here: every return below keeps it

    QString original;
    QList<OcrBlock> ocrBlocks;
    qint64 ocrMs = 0;
    if (!job->directText.isEmpty()) {
        original = job->directText;
    } else {
        QElapsedTimer ocrTimer;
        ocrTimer.start();
        // Fallback chain: try the selected engine first; if it is
        // unavailable (no Gemini key, no tesseract binary, stub engine),
        // try the next engine that reports available(). This is what makes
        // the default (gemini-vision, no key yet) actually work on a fresh
        // install when Tesseract is present, instead of failing outright.
        // A Failed (network/process) result is NOT retried on another
        // engine — it is a real error, not a missing engine.
        const QStringList candidates = {job->ocrEngineId, QStringLiteral("tesseract"),
                                        QStringLiteral("gemini-vision")};
        QStringList tried;
        OcrResult ocr;
        bool haveOcrResult = false;
        for (const QString &candidateId : candidates) {
            if (tried.contains(candidateId))
                continue;
            tried.append(candidateId);
            auto engine = createOcrEngine(candidateId, job->geminiApiKey, job->customApi);
            if (!engine)
                continue;
            if (!engine->available())
                continue;
            ocr = engine->recognize(job->crop);
            haveOcrResult = true;
            break;
        }
        ocrMs = ocrTimer.elapsed();
        if (!haveOcrResult) {
            result.error = QStringLiteral("No OCR engine available (tried: %1). "
                                          "Set a Gemini key or install Tesseract.")
                               .arg(tried.join(QStringLiteral(", ")));
            result.ocrMs = ocrMs;
            m_queue.finishRunning();
            m_processing = false;
            emit jobCompleted(result);
            QMetaObject::invokeMethod(this, "processNext", Qt::QueuedConnection);
            return;
        }
        if (ocr.status != OcrResult::Status::Ok) {
            result.error = ocr.error.isEmpty() ? QStringLiteral("OCR failed") : ocr.error;
            result.ocrMs = ocrMs;
            m_queue.finishRunning();
            m_processing = false;
            emit jobCompleted(result);
            QMetaObject::invokeMethod(this, "processNext", Qt::QueuedConnection);
            return;
        }
        original = ocr.text.trimmed();
        ocrBlocks = ocr.blocks;
        if (original.isEmpty()) {
            // No dialogue text in this crop: not an error, just nothing to
            // translate. Report success with empty text so the engine can
            // keep the previous translation instead of showing an error.
            result.ok = true;
            result.original.clear();
            result.translated.clear();
            result.ocrMs = ocrMs;
            m_queue.finishRunning();
            m_processing = false;
            emit jobCompleted(result);
            QMetaObject::invokeMethod(this, "processNext", Qt::QueuedConnection);
            return;
        }
    }
    result.original = original;

    // Chunk DB first (v4): every OCR'd line is looked up (exact, then
    // fuzzy) before any provider is called. A hit never reaches the
    // provider; only misses are translated. When every line hits, no
    // provider is created at all (translateMs = 0, chunkHit = true).
    QHash<int, QString> dbHits; // ocr block index -> Vietnamese
    QList<int> missIndices;
    QStringList missTexts;
    for (int i = 0; i < ocrBlocks.size(); ++i) {
        QString vi;
        if (m_chunkEnabled && m_chunkDb
            && (m_chunkDb->lookup(ocrBlocks.at(i).text, vi)
                || m_chunkDb->lookupFuzzy(ocrBlocks.at(i).text, vi)))
            dbHits.insert(i, vi);
        else {
            missIndices.append(i);
            missTexts.append(ocrBlocks.at(i).text);
        }
    }

    QElapsedTimer translateTimer;
    translateTimer.start();
    if (!ocrBlocks.isEmpty() && missIndices.isEmpty()) {
        // All lines came from the chunk DB: no provider call at all.
        result.ok = true;
        result.translateMs = 0;
        result.ocrMs = ocrMs;
        result.chunkHit = true;
        result.cacheHit = true;
        QStringList allTranslated;
        for (int i = 0; i < ocrBlocks.size(); ++i) {
            const QString vi = dbHits.value(i);
            if (vi.isEmpty())
                continue; // cannot happen: missIndices is empty
            allTranslated.append(vi);
            result.blocks.append(TranslatedBlock{
                ocrBlocks.at(i).text, vi,
                mapBlockRectToFrame(ocrBlocks.at(i).rect, job->sourceRect)});
        }
        result.translated = allTranslated.join(QChar(u'\n'));
    } else {
        auto provider = createTranslationProvider(job->providerId, job->geminiApiKey, job->customApi);
        if (!provider) {
            result.error = QStringLiteral("Unknown translation provider");
            result.ocrMs = ocrMs;
            m_queue.finishRunning();
            m_processing = false;
            emit jobCompleted(result);
            QMetaObject::invokeMethod(this, "processNext", Qt::QueuedConnection);
            return;
        }
        // One network call for the missed lines joined with "\n" (the
        // free Google endpoint preserves line breaks), then split the
        // reply back onto the missed blocks. If the reply has a different
        // number of lines, fall back to one call per block — the position
        // mapping must never be guessed from a misaligned split.
        // Streaming providers report partial text through the callback as
        // it arrives (emitted as jobProgress); the final text still comes
        // back here and replaces the partial. Partials are suppressed when
        // some lines already came from the DB: a partial only covers the
        // misses and must not be shown over the whole crop.
        const QString providerInput = ocrBlocks.isEmpty()
            ? original
            : missTexts.join(QChar(u'\n'));
        const TranslateResult translated = provider->translateStreaming(
            providerInput, job->sourceLanguage, job->targetLanguage,
            [&](const QString &partial) {
                if (dbHits.isEmpty())
                    emit jobProgress(original, partial, job->sourceRect, job->testJob);
            });
        result.translateMs = translateTimer.elapsed();
        result.ocrMs = ocrMs;
        if (!translated.ok) {
            result.error = translated.error;
        } else if (ocrBlocks.isEmpty()) {
            // Direct-text job (or an OCR engine that gave flat text only):
            // one block covering the whole crop.
            result.ok = true;
            result.translated = translated.text;
            result.blocks.append(TranslatedBlock{original, translated.text,
                                                 mapBlockRectToFrame(QRectF(0, 0, 1, 1),
                                                                     job->sourceRect)});
        } else {
            QStringList lines = translated.text.split(QChar(u'\n'));
            while (!lines.isEmpty() && lines.last().isEmpty())
                lines.removeLast();
            for (QString &line : lines)
                line = line.trimmed();
            if (lines.size() == missIndices.size()) {
                // Lines align with the missed blocks: merge DB hits and
                // provider lines back into reading order.
                result.ok = true;
                QStringList allTranslated;
                int missPos = 0;
                for (int i = 0; i < ocrBlocks.size(); ++i) {
                    if (dbHits.contains(i)) {
                        allTranslated.append(dbHits.value(i));
                        result.blocks.append(TranslatedBlock{
                            ocrBlocks.at(i).text, dbHits.value(i),
                            mapBlockRectToFrame(ocrBlocks.at(i).rect, job->sourceRect)});
                    } else {
                        const QString line = lines.at(missPos++);
                        if (line.isEmpty())
                            continue; // never cover a line with an empty box
                        allTranslated.append(line);
                        result.blocks.append(TranslatedBlock{
                            ocrBlocks.at(i).text, line,
                            mapBlockRectToFrame(ocrBlocks.at(i).rect, job->sourceRect)});
                    }
                }
                result.translated = allTranslated.join(QChar(u'\n'));
                if (result.blocks.isEmpty()) {
                    // Every line translated to nothing: report "no text"
                    // so the engine leaves the current translation alone.
                    result.original.clear();
                    result.translated.clear();
                }
            } else {
                // Line count mismatch: translate each missed block on its
                // own; DB-hit blocks are carried through unchanged.
                QStringList perBlock;
                QString lastError;
                for (int i = 0; i < ocrBlocks.size(); ++i) {
                    if (dbHits.contains(i)) {
                        perBlock.append(dbHits.value(i));
                        result.blocks.append(TranslatedBlock{
                            ocrBlocks.at(i).text, dbHits.value(i),
                            mapBlockRectToFrame(ocrBlocks.at(i).rect, job->sourceRect)});
                        continue;
                    }
                    const OcrBlock &block = ocrBlocks.at(i);
                    const TranslateResult one =
                        provider->translate(block.text, job->sourceLanguage, job->targetLanguage);
                    if (!one.ok) {
                        lastError = one.error;
                        continue;
                    }
                    const QString oneText = one.text.trimmed();
                    if (oneText.isEmpty())
                        continue;
                    perBlock.append(oneText);
                    result.blocks.append(TranslatedBlock{
                        block.text, oneText,
                        mapBlockRectToFrame(block.rect, job->sourceRect)});
                }
                if (result.blocks.isEmpty()) {
                    result.error = lastError.isEmpty() ? QStringLiteral("Translation failed")
                                                       : lastError;
                } else {
                    result.ok = true;
                    result.translated = perBlock.join(QChar(u'\n'));
                }
            }
        }
    }

    m_queue.finishRunning();
    m_processing = false;
    emit jobCompleted(result);
    QMetaObject::invokeMethod(this, "processNext", Qt::QueuedConnection);
}
