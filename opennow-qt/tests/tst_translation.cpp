// Unit tests for the translation module: crop hash, cache normalization,
// dialogue DB, and the bounded worker queue. No network is used; OCR and
// translation providers are exercised only through their NotAvailable paths
// (no key, no binary) where noted.
#include "translation/TranslationCache.h"
#include "translation/TranslationEngine.h"
#include "translation/TranslationWorker.h"
#include "translation/WuWaDialogueDb.h"
#include "translation/OcrEngine.h"

#include <QColor>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>

class TranslationTest : public QObject
{
    Q_OBJECT
private slots:
    void cropHashChangesWhenPixelsChange()
    {
        QImage first(64, 32, QImage::Format_ARGB32);
        first.fill(Qt::white);
        QImage identical(64, 32, QImage::Format_ARGB32);
        identical.fill(Qt::white);
        QImage changed(64, 32, QImage::Format_ARGB32);
        changed.fill(Qt::white);
        changed.setPixel(10, 10, qRgb(0, 0, 0));

        const QString hashFirst = TranslationEngine::cropHash(first);
        QVERIFY(!hashFirst.isEmpty());
        QCOMPARE(TranslationEngine::cropHash(identical), hashFirst);
        QVERIFY(TranslationEngine::cropHash(changed) != hashFirst);
        // Null / empty images hash to empty.
        QVERIFY(TranslationEngine::cropHash(QImage()).isEmpty());
    }

    void cropDialogueImageRespectsNormalizedRect()
    {
        QImage frame(200, 100, QImage::Format_ARGB32);
        frame.fill(Qt::black);
        // Default dialogue rect: x 5%, y 72%, w 90%, h 24%.
        const QImage crop = TranslationEngine::cropDialogueImage(
            frame, QRectF(0.05, 0.72, 0.90, 0.24));
        QCOMPARE(crop.width(), 180);
        QCOMPARE(crop.height(), 24);
        QCOMPARE(crop.size(), QSize(180, 24));
        // Empty rect / null image -> null crop.
        QVERIFY(TranslationEngine::cropDialogueImage(frame, QRectF()).isNull());
        QVERIFY(TranslationEngine::cropDialogueImage(QImage(), QRectF(0, 0, 1, 1)).isNull());
    }

    void cacheLookupHitAndMiss()
    {
        TranslationCache cache(100);
        QString out;
        QVERIFY(!cache.lookup(QStringLiteral("hello"), out));
        cache.insert(QStringLiteral("hello"), QStringLiteral("xin chào"));
        QVERIFY(cache.lookup(QStringLiteral("hello"), out));
        QCOMPARE(out, QStringLiteral("xin chào"));
        QCOMPARE(cache.size(), 1);
    }

    void cacheNormalizationIsCaseAndWhitespaceInsensitive()
    {
        TranslationCache cache(100);
        cache.insert(QStringLiteral("  Hello   World  "), QStringLiteral("xin chào thế giới"));
        QString out;
        QVERIFY(cache.lookup(QStringLiteral("hello world"), out));
        QCOMPARE(out, QStringLiteral("xin chào thế giới"));
        QVERIFY(cache.lookup(QStringLiteral("HELLO   WORLD"), out));
        QVERIFY(cache.lookup(QStringLiteral("\nhello\tworld\n"), out));
        QCOMPARE(TranslationCache::normalizeKey(QStringLiteral("  A   B ")),
                 QStringLiteral("a b"));
    }

    void cacheClearEmptiesIt()
    {
        TranslationCache cache(100);
        cache.insert(QStringLiteral("a"), QStringLiteral("b"));
        QCOMPARE(cache.size(), 1);
        cache.clear();
        QCOMPARE(cache.size(), 0);
        QString out;
        QVERIFY(!cache.lookup(QStringLiteral("a"), out));
    }

    void cacheEvictsOldestAtMaxEntries()
    {
        TranslationCache cache(3);
        cache.insert(QStringLiteral("one"), QStringLiteral("1"));
        cache.insert(QStringLiteral("two"), QStringLiteral("2"));
        cache.insert(QStringLiteral("three"), QStringLiteral("3"));
        cache.insert(QStringLiteral("four"), QStringLiteral("4"));
        QCOMPARE(cache.size(), 3);
        QString out;
        QVERIFY(!cache.lookup(QStringLiteral("one"), out));
        QVERIFY(cache.lookup(QStringLiteral("four"), out));
    }

    void cachePersistsToJson()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("cache.json"));
        {
            TranslationCache cache(path, 100);
            cache.insert(QStringLiteral("hello"), QStringLiteral("xin chào"));
            QVERIFY(cache.save());
        }
        TranslationCache reloaded(path, 100);
        QVERIFY(reloaded.load());
        QString out;
        QVERIFY(reloaded.lookup(QStringLiteral("hello"), out));
        QCOMPARE(out, QStringLiteral("xin chào"));
    }

    void dialogueDbLookupMissesOnEmptyDb()
    {
        WuWaDialogueDb db;
        QString out;
        QVERIFY(!db.lookup(QStringLiteral("hello, Rover"), out));
        QCOMPARE(db.size(), 0);
    }

    void dialogueDbLoadsSeedFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("wuwa.json"));
        QJsonObject entries;
        entries.insert(QStringLiteral("Hello, Rover"), QStringLiteral("Chào, Rover"));
        QJsonObject root;
        root.insert(QStringLiteral("version"), 1);
        root.insert(QStringLiteral("entries"), entries);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        file.close();

        WuWaDialogueDb db;
        QVERIFY(db.loadDialogueDb(path));
        QCOMPARE(db.size(), 1);
        QString out;
        QVERIFY(db.lookup(QStringLiteral("hello, rover"), out));
        QCOMPARE(out, QStringLiteral("Chào, Rover"));
        QVERIFY(!db.lookup(QStringLiteral("unknown line"), out));
        // Missing file: empty DB, not an error.
        WuWaDialogueDb missing;
        QVERIFY(missing.loadDialogueDb(dir.filePath(QStringLiteral("nope.json"))));
        QCOMPARE(missing.size(), 0);
    }

    void boundedQueueDropsStaleJob()
    {
        TranslationJobQueue queue;
        TranslationJob first;
        first.directText = QStringLiteral("first");
        TranslationJob second;
        second.directText = QStringLiteral("second");
        TranslationJob third;
        third.directText = QStringLiteral("third");

        QVERIFY(queue.submit(first));  // becomes running
        QVERIFY(queue.hasRunning());
        QVERIFY(!queue.hasWaiting());
        QVERIFY(queue.submit(second)); // becomes waiting
        QVERIFY(queue.hasWaiting());
        QCOMPARE(queue.droppedStale(), 0);
        QVERIFY(queue.submit(third));  // replaces waiting "second"
        QCOMPARE(queue.droppedStale(), 1);
        QVERIFY(queue.hasWaiting());

        const auto running = queue.takeRunning();
        QVERIFY(running.has_value());
        QCOMPARE(running->directText, QStringLiteral("first"));
        queue.finishRunning();
        const auto next = queue.takeRunning();
        QVERIFY(next.has_value());
        QCOMPARE(next->directText, QStringLiteral("third")); // "second" was dropped
        queue.finishRunning();
        QVERIFY(!queue.takeRunning().has_value());
    }

    void stubOcrEnginesReportNotAvailable()
    {
        RapidOcrEngine rapid;
        QVERIFY(!rapid.available());
        QImage image(10, 10, QImage::Format_ARGB32);
        image.fill(Qt::white);
        QCOMPARE(rapid.recognize(image).status, OcrResult::Status::NotAvailable);
        WindowsOcrEngine windows;
        QVERIFY(!windows.available());
        QCOMPARE(windows.recognize(image).status, OcrResult::Status::NotAvailable);
        GeminiVisionOcrEngine gemini;
        QVERIFY(!gemini.available()); // no key
        QCOMPARE(gemini.recognize(image).status, OcrResult::Status::NotAvailable);
        QVERIFY(createOcrEngine(QStringLiteral("bogus")) == nullptr);
    }

    void dialoguePresetRects()
    {
        QCOMPARE(TranslationEngine::dialogueRectForPreset(QStringLiteral("wuthering-waves")),
                 QRectF(0.05, 0.72, 0.90, 0.24));
        QCOMPARE(TranslationEngine::dialogueRectForPreset(QStringLiteral("generic")),
                 QRectF(0.05, 0.60, 0.90, 0.30));
        // Full screen: the whole frame, so every line of text is OCR'd,
        // not just the dialogue box.
        QCOMPARE(TranslationEngine::dialogueRectForPreset(QStringLiteral("full-screen")),
                 QRectF(0, 0, 1, 1));
        QVERIFY(TranslationEngine::dialogueRectForPreset(QStringLiteral("bogus")).isNull());
    }

    void mapBlockRectToFrameCases()
    {
        // QRectF == is exact double equality, but the mapping does
        // floating-point math: compare with a small tolerance.
        auto nearRect = [](const QRectF &a, const QRectF &b) {
            return qAbs(a.x() - b.x()) < 1e-9 && qAbs(a.y() - b.y()) < 1e-9
                && qAbs(a.width() - b.width()) < 1e-9 && qAbs(a.height() - b.height()) < 1e-9;
        };
        // A block in the middle of the dialogue crop maps into the
        // dialogue area of the frame.
        QVERIFY(nearRect(
            ::mapBlockRectToFrame(QRectF(0.1, 0.2, 0.5, 0.3), QRectF(0.05, 0.72, 0.90, 0.24)),
            QRectF(0.14, 0.768, 0.45, 0.072)));
        // Full-screen crop: block coords are already frame coords.
        QVERIFY(nearRect(
            ::mapBlockRectToFrame(QRectF(0.2, 0.1, 0.4, 0.05), QRectF(0, 0, 1, 1)),
            QRectF(0.2, 0.1, 0.4, 0.05)));
        // A block that would run past the frame edge is clamped.
        QVERIFY(nearRect(
            ::mapBlockRectToFrame(QRectF(0.9, 0.9, 0.5, 0.5), QRectF(0, 0, 1, 1)),
            QRectF(0.9, 0.9, 0.1, 0.1)));
    }

    void blocksToVariantListRoundTrip()
    {
        const QList<TranslatedBlock> blocks = {
            TranslatedBlock{QStringLiteral("Hello"), QStringLiteral("Xin chào"),
                            QRectF(0.1, 0.2, 0.5, 0.1)},
            TranslatedBlock{QStringLiteral("World"), QStringLiteral("Thế giới"),
                            QRectF(0.1, 0.35, 0.4, 0.1)},
        };
        const QVariantList list = TranslationEngine::blocksToVariantList(blocks);
        QCOMPARE(list.size(), 2);
        QCOMPARE(list.at(0).toMap().value(QStringLiteral("translated")).toString(),
                 QStringLiteral("Xin chào"));
        QCOMPARE(list.at(1).toMap().value(QStringLiteral("x")).toDouble(), 0.1);
        QCOMPARE(list.at(1).toMap().value(QStringLiteral("h")).toDouble(), 0.1);
        // A flat text with no blocks becomes one block over the whole rect.
        const QVariantList single = TranslationEngine::singleBlockVariantList(
            QStringLiteral("Hi"), QStringLiteral("Chào"), QRectF(0.05, 0.72, 0.90, 0.24));
        QCOMPARE(single.size(), 1);
        QCOMPARE(single.at(0).toMap().value(QStringLiteral("y")).toDouble(), 0.72);
    }

    void testJobDoesNotShowOnStream()
    {
        TranslationEngine engine;
        // A "Dịch thử" result updates the TEST section (last* fields) but
        // must never appear on the live overlay: hasTranslation stays
        // false and no blocks are published.
        TranslationJobResult testResult;
        testResult.ok = true;
        testResult.testJob = true;
        testResult.original = QStringLiteral("Hello world");
        testResult.translated = QStringLiteral("Xin chào thế giới");
        testResult.ocrMs = 10;
        testResult.translateMs = 20;
        QVERIFY(QMetaObject::invokeMethod(&engine, "onWorkerJobCompleted",
                                          Q_ARG(TranslationJobResult, testResult)));
        QCOMPARE(engine.lastTranslatedText(), QStringLiteral("Xin chào thế giới"));
        QVERIFY(!engine.hasTranslation());
        QVERIFY(engine.translationBlocks().isEmpty());

        // A stream result DOES show: hasTranslation true, one block over
        // the line it translates.
        TranslationJobResult streamResult;
        streamResult.ok = true;
        streamResult.testJob = false;
        streamResult.original = QStringLiteral("Hello world");
        streamResult.translated = QStringLiteral("Xin chào thế giới");
        streamResult.blocks = {TranslatedBlock{QStringLiteral("Hello world"),
                                               QStringLiteral("Xin chào thế giới"),
                                               QRectF(0.1, 0.2, 0.5, 0.1)}};
        QVERIFY(QMetaObject::invokeMethod(&engine, "onWorkerJobCompleted",
                                          Q_ARG(TranslationJobResult, streamResult)));
        QVERIFY(engine.hasTranslation());
        QCOMPARE(engine.translationBlocks().size(), 1);
        QCOMPARE(engine.translationBlocks().at(0).toMap()
                     .value(QStringLiteral("translated")).toString(),
                 QStringLiteral("Xin chào thế giới"));
    }

    void framePumpFiresWhenEnabled()
    {
        TranslationEngine engine;
        engine.setEnabled(false); // deterministic start: pump stopped
        QSignalSpy spy(&engine, &TranslationEngine::frameRequested);
        engine.setEnabled(true);
        QVERIFY(spy.wait(3000)); // pump interval is 1s
        QVERIFY(spy.count() >= 1);
        engine.setEnabled(false);
    }
};

QTEST_MAIN(TranslationTest)
#include "tst_translation.moc"
