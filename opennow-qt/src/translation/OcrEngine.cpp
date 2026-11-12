#include "translation/OcrEngine.h"
#include "translation/TranslationHttp.h"

#include <QBuffer>
#include <QByteArray>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QProcess>
#include <QStandardPaths>

namespace {
constexpr auto GeminiModel = "gemini-3.5-flash-lite";
// The model must return one block PER LINE, with the block's position in
// the image — the overlay draws one Vietnamese box over each English line,
// so coordinates are the whole point. x/y/w/h are fractions of the image
// (0.0-1.0). If the model ignores the shape and returns the old {"text"}
// form, parseGeminiOcrText wraps it as one whole-image block.
constexpr auto GeminiOcrPrompt =
    "Extract all English text visible in this image (dialogue, subtitles, menus, labels), "
    "one block per line of text, in reading order. "
    "Reply with JSON only, no other text: {\"blocks\": [{\"text\": \"...\", \"x\": 0.0, \"y\": 0.0, "
    "\"w\": 0.0, \"h\": 0.0}], \"confidence\": 0.0}. "
    "x, y, w, h are the block's position and size as fractions of the image (0.0-1.0). "
    "If no English text is visible, reply {\"blocks\": [], \"confidence\": 0}.";

QString geminiUrl()
{
    return QStringLiteral("https://generativelanguage.googleapis.com/v1beta/models/")
        + QString::fromLatin1(GeminiModel) + QStringLiteral(":generateContent");
}

// Parses the model's JSON reply out of the Gemini response envelope.
// Strips ```json fences if the model added them despite the prompt.
// Fills text (blocks joined with "\n") and blocks (one per line, rect
// normalized 0..1 within the image, clamped and empty/zero-area blocks
// dropped). Accepts the old {"text": ...} shape as one whole-image block.
bool parseGeminiOcrText(const QByteArray &body, QString &text, QList<OcrBlock> &blocks,
                        double &confidence, QString &error)
{
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) {
        error = QStringLiteral("Invalid response from OCR service");
        return false;
    }
    const QJsonArray candidates = doc.object().value(QStringLiteral("candidates")).toArray();
    if (candidates.isEmpty()) {
        error = QStringLiteral("OCR service returned no result");
        return false;
    }
    const QJsonArray parts = candidates.at(0).toObject()
        .value(QStringLiteral("content")).toObject()
        .value(QStringLiteral("parts")).toArray();
    if (parts.isEmpty()) {
        error = QStringLiteral("OCR service returned no text");
        return false;
    }
    QString raw = parts.at(0).toObject().value(QStringLiteral("text")).toString().trimmed();
    if (raw.startsWith(QStringLiteral("```"))) {
        const int firstNewline = raw.indexOf(QChar(u'\n'));
        if (firstNewline >= 0)
            raw = raw.mid(firstNewline + 1);
        if (raw.endsWith(QStringLiteral("```")))
            raw.chop(3);
        raw = raw.trimmed();
    }
    const QJsonDocument inner = QJsonDocument::fromJson(raw.toUtf8());
    if (!inner.isObject()) {
        error = QStringLiteral("OCR service returned unparsable text");
        return false;
    }
    const QJsonObject obj = inner.object();
    confidence = obj.value(QStringLiteral("confidence")).toDouble(-1.0);
    blocks.clear();
    const QJsonArray blockArray = obj.value(QStringLiteral("blocks")).toArray();
    for (const QJsonValue &value : blockArray) {
        const QJsonObject block = value.toObject();
        const QString blockText = block.value(QStringLiteral("text")).toString().trimmed();
        if (blockText.isEmpty())
            continue;
        const double x = qBound(0.0, block.value(QStringLiteral("x")).toDouble(0.0), 1.0);
        const double y = qBound(0.0, block.value(QStringLiteral("y")).toDouble(0.0), 1.0);
        const double w = qBound(0.0, block.value(QStringLiteral("w")).toDouble(0.0), 1.0 - x);
        const double h = qBound(0.0, block.value(QStringLiteral("h")).toDouble(0.0), 1.0 - y);
        if (w <= 0.0 || h <= 0.0)
            continue;
        blocks.append(OcrBlock{blockText, QRectF(x, y, w, h)});
    }
    if (blocks.isEmpty()) {
        // Old shape (or a model that ignored the blocks instruction):
        // one block covering the whole image, so the overlay still shows
        // the text instead of dropping the whole OCR result.
        const QString flat = obj.value(QStringLiteral("text")).toString().trimmed();
        if (!flat.isEmpty())
            blocks.append(OcrBlock{flat, QRectF(0, 0, 1, 1)});
    }
    QStringList lines;
    lines.reserve(blocks.size());
    for (const OcrBlock &block : blocks)
        lines.append(block.text);
    text = lines.join(QChar(u'\n'));
    return true;
}
} // namespace

GeminiVisionOcrEngine::GeminiVisionOcrEngine(const QString &apiKey)
    : m_apiKey(apiKey)
{
}

void GeminiVisionOcrEngine::setApiKey(const QString &apiKey)
{
    m_apiKey = apiKey;
}

OcrResult GeminiVisionOcrEngine::recognize(const QImage &image)
{
    if (m_apiKey.isEmpty())
        return OcrResult{OcrResult::Status::NotAvailable, {}, {}, -1.0,
                         QStringLiteral("Gemini API key not set")};
    if (image.isNull() || image.width() <= 0 || image.height() <= 0)
        return OcrResult{OcrResult::Status::Failed, {}, {}, -1.0,
                         QStringLiteral("Empty image passed to OCR")};

    QByteArray jpeg;
    {
        QBuffer buffer(&jpeg);
        buffer.open(QIODevice::WriteOnly);
        // JPEG quality 90: small payload, text stays legible.
        if (!image.convertedTo(QImage::Format_RGB32).save(&buffer, "JPG", 90))
            return OcrResult{OcrResult::Status::Failed, {}, {}, -1.0,
                             QStringLiteral("Could not encode image for OCR")};
    }

    QJsonObject inlineData;
    inlineData.insert(QStringLiteral("mimeType"), QStringLiteral("image/jpeg"));
    inlineData.insert(QStringLiteral("data"), QString::fromLatin1(jpeg.toBase64()));
    QJsonObject imagePart;
    imagePart.insert(QStringLiteral("inlineData"), inlineData);
    QJsonObject textPart;
    textPart.insert(QStringLiteral("text"), QString::fromLatin1(GeminiOcrPrompt));
    QJsonObject content;
    content.insert(QStringLiteral("parts"), QJsonArray{imagePart, textPart});
    QJsonObject requestBody;
    requestBody.insert(QStringLiteral("contents"), QJsonArray{content});
    const QByteArray payload = QJsonDocument(requestBody).toJson(QJsonDocument::Compact);

    // Key travels in the x-goog-api-key header, never in the URL, and is
    // never logged.
    const TranslationHttpResult result = TranslationHttp::postJson(
        geminiUrl(), payload,
        {{QByteArrayLiteral("x-goog-api-key"), m_apiKey.toUtf8()}});
    if (!result.ok())
        return OcrResult{OcrResult::Status::Failed, {}, {}, -1.0, result.error};

    QString text;
    QList<OcrBlock> blocks;
    double confidence = -1.0;
    QString error;
    if (!parseGeminiOcrText(result.body, text, blocks, confidence, error))
        return OcrResult{OcrResult::Status::Failed, {}, {}, -1.0, error};
    return OcrResult{OcrResult::Status::Ok, text, blocks, confidence, {}};
}

bool TesseractOcrEngine::available() const
{
    return !QStandardPaths::findExecutable(QStringLiteral("tesseract")).isEmpty();
}

OcrResult TesseractOcrEngine::recognize(const QImage &image)
{
    if (!available())
        return OcrResult{OcrResult::Status::NotAvailable, {}, {}, -1.0,
                         QStringLiteral("tesseract binary not found")};
    if (image.isNull() || image.width() <= 0 || image.height() <= 0)
        return OcrResult{OcrResult::Status::Failed, {}, {}, -1.0,
                         QStringLiteral("Empty image passed to OCR")};

    QByteArray png;
    {
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG"))
            return OcrResult{OcrResult::Status::Failed, {}, {}, -1.0,
                             QStringLiteral("Could not encode image for OCR")};
    }
    // TSV first: one block per text line, with real coordinates — the
    // overlay needs positions, not just flat text. If TSV yields nothing
    // (empty image, parse failure), fall back to plain text as one
    // whole-image block so the result is still usable.
    auto runTesseract = [&png](const QString &mode, QByteArray *out, QString *err) {
        QProcess process;
        process.start(QStringLiteral("tesseract"),
                      {QStringLiteral("stdin"), QStringLiteral("stdout"), mode,
                       QStringLiteral("--psm"), QStringLiteral("6"),
                       QStringLiteral("-l"), QStringLiteral("eng")});
        if (!process.waitForStarted(3000)) {
            *err = QStringLiteral("Could not start tesseract");
            return false;
        }
        process.write(png);
        process.closeWriteChannel();
        if (!process.waitForFinished(8000)) {
            process.kill();
            *err = QStringLiteral("tesseract timed out");
            return false;
        }
        if (process.exitCode() != 0) {
            *err = QString::fromUtf8(process.readAllStandardError()).trimmed();
            return false;
        }
        *out = process.readAllStandardOutput();
        return true;
    };
    QByteArray tsv;
    QString tsvError;
    if (runTesseract(QStringLiteral("tsv"), &tsv, &tsvError)) {
        QList<OcrBlock> blocks;
        // TSV columns: level page block par line word left top width height
        // conf text. Level 5 rows are words; group by (block, par, line).
        QMap<QPair<int, QPair<int, int>>, QList<QPair<QString, QRectF>>> lines;
        const QList<QByteArray> rows = tsv.split('\n');
        for (qsizetype i = 1; i < rows.size(); ++i) { // row 0 is the header
            const QList<QByteArray> cols = rows.at(i).split('\t');
            if (cols.size() < 12)
                continue;
            bool ok = false;
            const int level = cols.at(0).toInt(&ok);
            if (!ok || level != 5)
                continue;
            const QString word = QString::fromUtf8(cols.at(11)).trimmed();
            if (word.isEmpty())
                continue;
            const int blockNum = cols.at(2).toInt();
            const int parNum = cols.at(3).toInt();
            const int lineNum = cols.at(4).toInt();
            const int left = cols.at(6).toInt();
            const int top = cols.at(7).toInt();
            const int width = cols.at(8).toInt();
            const int height = cols.at(9).toInt();
            if (width <= 0 || height <= 0)
                continue;
            const QRectF rect(qBound(0.0, double(left) / image.width(), 1.0),
                              qBound(0.0, double(top) / image.height(), 1.0),
                              qBound(0.0, double(width) / image.width(), 1.0),
                              qBound(0.0, double(height) / image.height(), 1.0));
            lines[qMakePair(blockNum, qMakePair(parNum, lineNum))]
                .append(qMakePair(word, rect));
        }
        // QMap iterates keys in order: block, then paragraph, then line —
        // the reading order for a single-column game UI.
        for (auto it = lines.constBegin(); it != lines.constEnd(); ++it) {
            QStringList words;
            QRectF united;
            for (const auto &wordRect : it.value()) {
                words.append(wordRect.first);
                united = united.united(wordRect.second);
            }
            if (!words.isEmpty() && united.isValid())
                blocks.append(OcrBlock{words.join(QChar(u' ')), united});
        }
        if (!blocks.isEmpty()) {
            QStringList texts;
            texts.reserve(blocks.size());
            for (const OcrBlock &block : blocks)
                texts.append(block.text);
            // Tesseract reports no per-call confidence on stdout; -1 = unknown.
            return OcrResult{OcrResult::Status::Ok, texts.join(QChar(u'\n')), blocks, -1.0, {}};
        }
    }
    QByteArray plain;
    QString plainError;
    if (!runTesseract(QStringLiteral("txt"), &plain, &plainError))
        return OcrResult{OcrResult::Status::Failed, {}, {}, -1.0, plainError};
    const QString text = QString::fromUtf8(plain).trimmed();
    QList<OcrBlock> blocks;
    if (!text.isEmpty())
        blocks.append(OcrBlock{text, QRectF(0, 0, 1, 1)});
    return OcrResult{OcrResult::Status::Ok, text, blocks, -1.0, {}};
}

OcrResult RapidOcrEngine::recognize(const QImage &)
{
    return OcrResult{OcrResult::Status::NotAvailable, {}, {}, -1.0,
                     QStringLiteral("RapidOCR is not available in this build: "
                                    "ONNX Runtime is not vendored in v1")};
}

OcrResult WindowsOcrEngine::recognize(const QImage &)
{
    return OcrResult{OcrResult::Status::NotAvailable, {}, {}, -1.0,
                     QStringLiteral("Windows OCR is not available: WinRT OCR requires "
                                    "package identity, which this Win32 build does not have")};
}

std::unique_ptr<OcrEngine> createOcrEngine(const QString &id, const QString &geminiApiKey)
{
    if (id == QStringLiteral("gemini-vision"))
        return std::make_unique<GeminiVisionOcrEngine>(geminiApiKey);
    if (id == QStringLiteral("tesseract"))
        return std::make_unique<TesseractOcrEngine>();
    if (id == QStringLiteral("rapidocr"))
        return std::make_unique<RapidOcrEngine>();
    if (id == QStringLiteral("windows-ocr"))
        return std::make_unique<WindowsOcrEngine>();
    return nullptr;
}
