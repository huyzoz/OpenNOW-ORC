// TranslationWorker: OCR + translation off the GUI thread.
//
// The engine submits jobs from the GUI thread; the worker owns a QThread
// and does all network/OCR work there. Results come back via queued
// signals only. The worker never touches QML or any GUI object, and the
// QImage in a job is a detached copy owned by the job.
//
// Bounded queue (TranslationJobQueue, unit-testable without threads or
// network): at most 1 job running + 1 job waiting. A new submit while a
// job is waiting REPLACES the waiting job (the stale one is dropped and
// counted) — dialogue moves on, so an old crop must never be translated
// after a newer one.
#pragma once

#include <QImage>
#include <QMetaType>
#include <QMutex>
#include <QObject>
#include <QRectF>
#include <QString>
#include <QThread>
#include <optional>

// One translated line and where it sits on the frame: rect is normalized
// 0..1 in FULL-FRAME coordinates (the worker maps the OCR block's
// crop-relative rect through the job's sourceRect), so the overlay can draw
// the Vietnamese box exactly over the English line it replaces.
struct TranslatedBlock
{
    QString original;
    QString translated;
    QRectF rect;
};
Q_DECLARE_METATYPE(TranslatedBlock)

struct TranslationJob
{
    QImage crop;            // detached dialogue crop; empty for direct-text jobs
    QString cropHash;       // SHA-256 of the crop; lets the engine cache by pixels
    QRectF sourceRect;      // normalized dialogue rect the crop came from
    QString directText;     // when non-empty, OCR is skipped and this is translated
    bool testJob = false;   // true for the Settings "Dịch thử" button: the
                            // result updates the TEST section only and must
                            // never show on the live stream overlay
    QString ocrEngineId = QStringLiteral("gemini-vision");
    QString providerId = QStringLiteral("google");
    QString sourceLanguage = QStringLiteral("en");
    QString targetLanguage = QStringLiteral("vi");
    QString geminiApiKey;   // forwarded to Gemini engines; never logged
};
Q_DECLARE_METATYPE(TranslationJob)

struct TranslationJobResult
{
    bool ok = false;
    QString original;   // OCR'd (or direct) English text
    QString translated; // Vietnamese text on success
    // One translated block per OCR'd line, in reading order. Empty for
    // direct-text jobs that carry no OCR coordinates.
    QList<TranslatedBlock> blocks;
    bool testJob = false; // mirrors the job: TEST results stay off the overlay
    QRectF sourceRect;
    qint64 ocrMs = 0;
    qint64 translateMs = 0;
    QString cropHash;
    bool cacheHit = false; // true when the translation came from cache/DB
    QString error;      // safe to show; never contains a key
};
Q_DECLARE_METATYPE(TranslationJobResult)

// Maps an OCR block's rect (normalized 0..1 within the crop) into
// full-frame normalized coordinates through the crop's sourceRect.
// Pure and unit-testable; results are clamped to the frame.
QRectF mapBlockRectToFrame(const QRectF &blockRect, const QRectF &sourceRect);

// Pure queue logic: 1 running + 1 waiting, newest waiting job wins.
// No threads, no network — this is what the unit test exercises.
class TranslationJobQueue
{
public:
    // Returns true if the job was accepted (as running or waiting).
    // When a waiting job is replaced, the replaced job is counted in
    // droppedStale().
    bool submit(const TranslationJob &job);
    std::optional<TranslationJob> takeRunning(); // moves waiting -> running
    void finishRunning();
    bool hasRunning() const;
    bool hasWaiting() const;
    int droppedStale() const { return m_droppedStale; }
    void clear();
    void clearWaiting(); // drops the waiting job only; running job untouched

private:
    mutable QMutex m_mutex;
    std::optional<TranslationJob> m_running;
    std::optional<TranslationJob> m_waiting;
    int m_droppedStale = 0;
};

class TranslationWorker : public QObject
{
    Q_OBJECT
public:
    explicit TranslationWorker(QObject *parent = nullptr);
    ~TranslationWorker() override;

    void start(); // starts the worker thread
    void stop();  // quits the thread and waits for it (call before destruction)

    int droppedStaleJobs() const;

public slots:
    // Called via QueuedConnection from the GUI thread. Accepts into the
    // bounded queue; a stale waiting job is dropped, never processed late.
    void submitJob(const TranslationJob &job);
    void setGeminiApiKey(const QString &key);
    void cancelPending();

signals:
    void jobCompleted(const TranslationJobResult &result);

private slots:
    void processNext();

private:
    TranslationJobQueue m_queue;
    QThread m_thread;
    QString m_geminiApiKey; // worker-thread copy; set via queued slot
    bool m_processing = false; // worker-thread only
};
