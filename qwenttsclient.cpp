#include "qwenttsclient.h"

#include <QNetworkRequest>
#include <QJsonDocument>
#include <QList>
#include <QMediaDevices>
#include <QUuid>
#include <QtEndian>

static const int kRealtimeSampleRate = 24000;   // realtime 系（qwen3-tts）输出采样率
static const int kAudioTtsSampleRate = 48000;   // audio-tts 系（qwen-audio-3.0）输出采样率
static const int kMaxConnectFailures = 3;

static int bytesPerSample(QAudioFormat::SampleFormat sf) {
    switch (sf) {
    case QAudioFormat::UInt8: return 1;
    case QAudioFormat::Int32: return 4;
    case QAudioFormat::Float: return 4;
    case QAudioFormat::Int16:
    default: return 2;
    }
}

/**
 * @brief 解析标准 RIFF/WAVE 头，返回采样率、声道数、位深与 data 区块位置
 * 支持 PCM(1)/IEEE Float(3)/EXTENSIBLE(0xFFFE) 编码
 */
static bool parseWavHeader(const QByteArray& wav, int& rate, int& channels, int& bits,
                           QAudioFormat::SampleFormat& fmt, qint64& dataOffset, qint64& dataLen) {
    if (wav.size() < 44) return false;
    if (qstrncmp(wav.constData(), "RIFF", 4) != 0 ||
        qstrncmp(wav.constData() + 8, "WAVE", 4) != 0) return false;

    const uchar* base = reinterpret_cast<const uchar*>(wav.constData());
    qint64 pos = 12;
    bool haveFmt = false;
    bool haveData = false;
    while (pos + 8 <= wav.size()) {
        const char* id = wav.constData() + pos;
        const quint32 size = qFromLittleEndian<quint32>(base + pos + 4);
        if (qstrncmp(id, "fmt ", 4) == 0 && pos + 8 + 16 <= wav.size()) {
            const quint16 audioFormat = qFromLittleEndian<quint16>(base + pos + 8);
            channels = int(qFromLittleEndian<quint16>(base + pos + 10));
            rate = int(qFromLittleEndian<quint32>(base + pos + 12));
            bits = int(qFromLittleEndian<quint16>(base + pos + 22));
            if (audioFormat != 1 && audioFormat != 3 && audioFormat != 0xFFFE) return false;
            if (bits == 16) fmt = QAudioFormat::Int16;
            else if (bits == 8) fmt = QAudioFormat::UInt8;
            else if (bits == 32) fmt = (audioFormat == 3) ? QAudioFormat::Float : QAudioFormat::Int32;
            else return false;
            if (rate <= 0 || channels <= 0) return false;
            haveFmt = true;
        } else if (qstrncmp(id, "data", 4) == 0) {
            dataOffset = pos + 8;
            const qint64 remain = wav.size() - dataOffset;
            dataLen = size > 0 ? qMin<qint64>(size, remain) : remain;
            haveData = true;
        }
        pos += 8 + qint64(size) + (size & 1);   // 区块按 2 字节对齐
        if (haveFmt && haveData) break;
    }
    return haveFmt && haveData && dataLen > 0;
}

QwenTtsClient::QwenTtsClient(QObject* parent)
    : QObject(parent) {
    audioFormat.setSampleRate(apiSampleRate);
    audioFormat.setChannelCount(1);
    audioFormat.setSampleFormat(QAudioFormat::Int16);

    nam = new QNetworkAccessManager(this);

    connect(&ws, &QWebSocket::connected, this, &QwenTtsClient::onConnected);
    connect(&ws, &QWebSocket::textMessageReceived, this, &QwenTtsClient::onTextMessageReceived);
    connect(&ws, &QWebSocket::binaryMessageReceived,
            this, &QwenTtsClient::onBinaryMessageReceived);
    connect(&ws, &QWebSocket::disconnected, this, &QwenTtsClient::onDisconnected);
    connect(&ws, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &QwenTtsClient::onErrorOccurred);

    pumpTimer.setInterval(40);
    connect(&pumpTimer, &QTimer::timeout, this, &QwenTtsClient::onPumpTimerTimeout);

    retryTimer.setSingleShot(true);
    retryTimer.setInterval(1000);
    connect(&retryTimer, &QTimer::timeout, this, &QwenTtsClient::trySpeakNext);
}

QwenTtsClient::~QwenTtsClient() {
    closeAudio();
    ws.close();
}

void QwenTtsClient::setApiKey(const QString& key) {
    const QString newKey = key.trimmed();
    if (newKey == apiKey) return;
    apiKey = newKey;
    // 换 Key 后强制重建会话
    if (ws.state() != QAbstractSocket::UnconnectedState) ws.close();
}

void QwenTtsClient::setModel(const QString& m) {
    const QString newModel = m.trimmed();
    if (newModel.isEmpty() || newModel == model) return;
    model = newModel;
    apiSampleRate = isRealtimeProtocol() ? kRealtimeSampleRate : kAudioTtsSampleRate;
    abortHttp();
    // 两类 WS 模型的端点与事件协议不同，切换后强制重建连接
    sessionReady = false;
    taskStarted = false;
    taskId.clear();
    configuredVoice.clear();
    currentText.clear();
    if (ws.state() != QAbstractSocket::UnconnectedState) ws.close();
}

void QwenTtsClient::setVoice(const QString& v) {
    voice = v;
}

void QwenTtsClient::setVolume(int percent) {
    volume = qBound(0, percent, 100);
}

bool QwenTtsClient::isBusy() const {
    return connecting || !currentText.isEmpty() || !queue.isEmpty() || !pendingPcm.isEmpty()
           || httpReply != nullptr || downloadReply != nullptr;
}

void QwenTtsClient::say(const QString& text) {
    QString t = text.trimmed();
    if (t.isEmpty()) return;
    queue.enqueue(t);
    trySpeakNext();
}

void QwenTtsClient::stopAll() {
    queue.clear();
    currentText.clear();
    pendingPcm.clear();
    pumpTimer.stop();
    retryTimer.stop();
    closeAudio();
    abortHttp();
    connectFailures = 0;
    if (ws.state() != QAbstractSocket::UnconnectedState) ws.close();
    sessionReady = false;
    connecting = false;
    configuredVoice.clear();
    taskStarted = false;
    taskId.clear();
}

bool QwenTtsClient::isRealtimeProtocol() const {
    return model.contains(QStringLiteral("realtime"));
}

bool QwenTtsClient::isTaskProtocol() const {
    return model.startsWith(QStringLiteral("qwen-audio"));
}

bool QwenTtsClient::isHttpProtocol() const {
    return !isRealtimeProtocol() && !isTaskProtocol();
}

void QwenTtsClient::connectToService() {
    if (apiKey.isEmpty()) {
        clearSession(false, QString());
        emit failed(QStringLiteral("未配置 DashScope API Key，无法使用千问 TTS"));
        return;
    }
    if (connecting || ws.state() != QAbstractSocket::UnconnectedState) return;

    connecting = true;
    const QUrl url = isRealtimeProtocol()
        ? QUrl(QStringLiteral("wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=%1").arg(model))
        : QUrl(QStringLiteral("wss://dashscope.aliyuncs.com/api-ws/v1/inference/"));
    QNetworkRequest req(url);
    req.setRawHeader("Authorization", "Bearer " + apiKey.toUtf8());
    req.setRawHeader("user-agent", "DMReader/0.4");
    ws.open(req);
}

void QwenTtsClient::sendJson(const QJsonObject& obj) {
    if (ws.state() != QAbstractSocket::ConnectedState) return;
    ws.sendTextMessage(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
}

void QwenTtsClient::sendSessionUpdate() {
    QJsonObject session;
    session["mode"] = QStringLiteral("commit");
    session["voice"] = voice;
    session["language_type"] = QStringLiteral("Auto");
    session["response_format"] = QStringLiteral("pcm");
    session["sample_rate"] = kRealtimeSampleRate;
    QJsonObject obj;
    obj["type"] = QStringLiteral("session.update");
    obj["session"] = session;
    sendJson(obj);
    configuredVoice = voice;
}

void QwenTtsClient::sendRunTask() {
    taskId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject header;
    header["action"] = QStringLiteral("run-task");
    header["task_id"] = taskId;
    header["streaming"] = QStringLiteral("duplex");
    QJsonObject parameters;
    parameters["text_type"] = QStringLiteral("PlainText");
    parameters["voice"] = voice;
    parameters["format"] = QStringLiteral("pcm");
    parameters["sample_rate"] = apiSampleRate;
    parameters["volume"] = 50;
    parameters["rate"] = 1;
    parameters["pitch"] = 1;
    parameters["enable_ssml"] = false;
    QJsonObject payload;
    payload["task_group"] = QStringLiteral("audio");
    payload["task"] = QStringLiteral("tts");
    payload["function"] = QStringLiteral("SpeechSynthesizer");
    payload["model"] = model;
    payload["parameters"] = parameters;
    payload["input"] = QJsonObject();
    QJsonObject obj;
    obj["header"] = header;
    obj["payload"] = payload;
    sendJson(obj);
}

void QwenTtsClient::sendContinueTask(const QString& text) {
    QJsonObject header;
    header["action"] = QStringLiteral("continue-task");
    header["task_id"] = taskId;
    header["streaming"] = QStringLiteral("duplex");
    QJsonObject input;
    input["text"] = text;
    QJsonObject payload;
    payload["input"] = input;
    QJsonObject obj;
    obj["header"] = header;
    obj["payload"] = payload;
    sendJson(obj);
}

void QwenTtsClient::sendFinishTask() {
    QJsonObject header;
    header["action"] = QStringLiteral("finish-task");
    header["task_id"] = taskId;
    header["streaming"] = QStringLiteral("duplex");
    QJsonObject obj;
    obj["header"] = header;
    obj["payload"] = QJsonObject();
    sendJson(obj);
}

void QwenTtsClient::startHttpRequest(const QString& text) {
    QNetworkRequest req(QUrl(QStringLiteral(
        "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation")));
    req.setRawHeader("Authorization", "Bearer " + apiKey.toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QJsonObject input;
    input["text"] = text;
    input["voice"] = voice;
    input["language_type"] = QStringLiteral("Auto");
    QJsonObject body;
    body["model"] = model;
    body["input"] = input;
    httpReply = nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(httpReply, &QNetworkReply::finished, this, &QwenTtsClient::onHttpReplyFinished);
}

void QwenTtsClient::abortHttp() {
    if (httpReply) {
        QNetworkReply* r = httpReply;
        httpReply = nullptr;
        r->abort();
        r->deleteLater();
    }
    if (downloadReply) {
        QNetworkReply* r = downloadReply;
        downloadReply = nullptr;
        r->abort();
        r->deleteLater();
    }
}

void QwenTtsClient::onHttpReplyFinished() {
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || reply != httpReply) return;   // 已被 abort/过期的回复
    httpReply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit failed(QStringLiteral("千问TTS请求失败: %1").arg(reply->errorString()));
        currentText.clear();
        return;
    }

    QJsonParseError parseErr{};
    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        emit failed(QStringLiteral("千问TTS响应解析失败"));
        currentText.clear();
        return;
    }
    const QJsonObject root = doc.object();
    if (root.value("status_code").toInt() != 200) {
        QString msg = root.value("message").toString();
        if (msg.isEmpty()) msg = root.value("code").toString();
        emit failed(QStringLiteral("千问TTS: %1").arg(msg));
        currentText.clear();
        return;
    }

    const QString url = root.value("output").toObject()
                            .value("audio").toObject().value("url").toString();
    if (url.isEmpty()) {
        emit failed(QStringLiteral("千问TTS: 响应中缺少音频地址"));
        currentText.clear();
        return;
    }

    // 下载音频文件（签名的 OSS 地址，无需鉴权头）
    QNetworkRequest dlReq{QUrl(url)};   // 花括号避免最令人困惑解析
    downloadReply = nam->get(dlReq);
    connect(downloadReply, &QNetworkReply::finished, this, &QwenTtsClient::onAudioDownloaded);
}

void QwenTtsClient::onAudioDownloaded() {
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || reply != downloadReply) return;
    downloadReply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit failed(QStringLiteral("千问TTS音频下载失败: %1").arg(reply->errorString()));
        currentText.clear();
        return;
    }

    int rate = 0;
    int channels = 0;
    int bits = 0;
    QAudioFormat::SampleFormat srcFormat = QAudioFormat::Int16;
    qint64 dataOffset = -1;
    qint64 dataLen = 0;
    const QByteArray wav = reply->readAll();
    if (!parseWavHeader(wav, rate, channels, bits, srcFormat, dataOffset, dataLen)) {
        emit failed(QStringLiteral("千问TTS: 音频解析失败（非标准 WAV）"));
        currentText.clear();
        return;
    }

    QByteArray pcm = wav.mid(dataOffset, dataLen);
    if (volume < 100 && srcFormat == QAudioFormat::Int16) {
        qint16* samples = reinterpret_cast<qint16*>(pcm.data());
        const int n = pcm.size() / 2;
        for (int i = 0; i < n; ++i) {
            const qint32 v = (samples[i] * volume) / 100;
            samples[i] = static_cast<qint16>(qBound<qint32>(-32768, v, 32767));
        }
    }
    pendingPcm.append(convertPcm(pcm, rate, channels, srcFormat));
    openAudio();
    if (!pumpTimer.isActive()) pumpTimer.start();
    currentText.clear();
}

void QwenTtsClient::onConnected() {
    connecting = false;
    connectFailures = 0;
    sessionReady = true;
    if (isRealtimeProtocol()) sendSessionUpdate();
    trySpeakNext();
}

void QwenTtsClient::trySpeakNext() {
    if (!currentText.isEmpty() || queue.isEmpty()) return;

    // http 系（qwen3-tts-flash 等非实时模型）：不走 WebSocket，直接提交合成
    if (isHttpProtocol()) {
        currentText = queue.dequeue();
        startHttpRequest(currentText);
        emit started(currentText);
        return;
    }

    if (!sessionReady) {
        if (!connecting && ws.state() == QAbstractSocket::UnconnectedState
                && !retryTimer.isActive() && connectFailures < kMaxConnectFailures) {
            connectToService();
        }
        return;   // 会话就绪后由 onConnected 再次调度
    }

    if (isRealtimeProtocol()) {
        if (configuredVoice != voice) sendSessionUpdate();
        currentText = queue.dequeue();
        QJsonObject append;
        append["type"] = QStringLiteral("input_text_buffer.append");
        append["text"] = currentText;
        sendJson(append);
        QJsonObject commit;
        commit["type"] = QStringLiteral("input_text_buffer.commit");
        sendJson(commit);
        emit started(currentText);
    } else {
        // audio-tts 系（run-task 协议）：每个utterance一个任务
        if (!taskStarted) {
            currentText = queue.dequeue();
            sendRunTask();
            return;   // 收到 task-started 后继续
        }
        currentText = queue.dequeue();
        sendContinueTask(currentText);
        sendFinishTask();
        emit started(currentText);
    }
}

void QwenTtsClient::onTextMessageReceived(const QString& message) {
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) return;
    QJsonObject root = doc.object();

    if (!isRealtimeProtocol()) {
        // ===== audio-tts 系（run-task 协议）=====
        const QJsonObject header = root.value("header").toObject();
        const QString event = header.value("event").toString();

        if (event == "task-started") {
            taskStarted = true;
            if (!currentText.isEmpty()) {
                sendContinueTask(currentText);
                sendFinishTask();
                emit started(currentText);
            }
            return;
        }

        if (event == "task-finished") {
            // 本任务音频二进制帧已全部送达；排空交给 pump 定时器
            taskStarted = false;
            currentText.clear();
            return;
        }

        if (event == "task-failed") {
            QString msg = header.value("error_message").toString();
            if (msg.isEmpty()) msg = header.value("error_code").toString();
            if (msg.isEmpty()) msg = message.left(200);
            emit failed(QStringLiteral("千问TTS: %1").arg(msg));
            clearSession(false, QString());
            // 任务失败后重建连接，避免会话状态残留
            if (ws.state() != QAbstractSocket::UnconnectedState) ws.close();
            return;
        }
        // result-generated 等其他事件忽略
        return;
    }

    // ===== realtime 系（qwen3-tts session 协议）=====
    QString type = root.value("type").toString();

    if (type == "response.audio.delta") {
        QByteArray pcm = QByteArray::fromBase64(root.value("delta").toString().toUtf8());
        if (pcm.isEmpty()) return;
        if (volume < 100) {
            qint16* samples = reinterpret_cast<qint16*>(pcm.data());
            const int n = pcm.size() / 2;
            for (int i = 0; i < n; ++i) {
                const qint32 v = (samples[i] * volume) / 100;
                samples[i] = static_cast<qint16>(qBound<qint32>(-32768, v, 32767));
            }
        }
        pendingPcm.append(convertPcm(pcm, apiSampleRate, 1, QAudioFormat::Int16));
        openAudio();
        if (!pumpTimer.isActive()) pumpTimer.start();
        return;
    }

    if (type == "response.done") {
        // 音频可能仍在设备缓冲中排队播放；排空交给 pump 定时器处理
        currentText.clear();
        return;
    }

    if (type == "error") {
        QJsonObject err = root.value("error").toObject();
        QString msg = err.value("message").toString();
        if (msg.isEmpty()) msg = message.left(200);
        emit failed(QStringLiteral("千问TTS: %1").arg(msg));
        clearSession(false, QString());
        // 服务端可能保留坏会话，主动断开，下次朗读时重建
        if (ws.state() != QAbstractSocket::UnconnectedState) ws.close();
        return;
    }

    if (type == "session.finished") {
        // 服务端结束会话（如空闲超时），下一次朗读时重新建连
        sessionReady = false;
        configuredVoice.clear();
    }
}

void QwenTtsClient::onBinaryMessageReceived(const QByteArray& message) {
    // audio-tts 系的二进制帧即原始 PCM（Int16 单声道，apiSampleRate）
    if (message.isEmpty()) return;
    QByteArray pcm = message;
    if (volume < 100) {
        qint16* samples = reinterpret_cast<qint16*>(pcm.data());
        const int n = pcm.size() / 2;
        for (int i = 0; i < n; ++i) {
            const qint32 v = (samples[i] * volume) / 100;
            samples[i] = static_cast<qint16>(qBound<qint32>(-32768, v, 32767));
        }
    }
    pendingPcm.append(convertPcm(pcm, apiSampleRate, 1, QAudioFormat::Int16));
    openAudio();
    if (!pumpTimer.isActive()) pumpTimer.start();
}

void QwenTtsClient::onPumpTimerTimeout() {
    if (!sink || !audioIo) {
        if (pendingPcm.isEmpty()) pumpTimer.stop();
        else pendingPcm.clear();
        return;
    }
    while (!pendingPcm.isEmpty()) {
        const qint64 free = sink->bytesFree();
        if (free <= 0) break;
        const qint64 chunk = qMin<qint64>(free, pendingPcm.size());
        const qint64 written = audioIo->write(pendingPcm.constData(), chunk);
        if (written <= 0) break;
        pendingPcm.remove(0, written);
    }

    if (pendingPcm.isEmpty() && currentText.isEmpty()
            && sink->state() != QAudio::ActiveState) {
        // 设备缓冲已排空且没有新的合成任务
        pumpTimer.stop();
        closeAudio();
        emit finished();
    }
}

void QwenTtsClient::openAudio() {
    if (sink) return;
    QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull()) {
        emit failed(QStringLiteral("未找到可用的音频输出设备"));
        return;
    }

    // 设备首选格式（与系统混音格式一致）必然可打开。
    // 注意 isFormatSupported 的严格检查在 WASAPI 共享模式下只认 Float 混音格式，
    // 会把 Int16 等全部拒绝，因此不以其作为硬性依据；直接尝试打开，
    // 首选格式失败再退回 Float/Int16 的常见组合。
    QList<QAudioFormat> candidates;
    const QAudioFormat preferred = dev.preferredFormat();
    if (preferred.sampleRate() > 0 && preferred.channelCount() > 0
            && preferred.sampleFormat() != QAudioFormat::Unknown) {
        candidates.append(preferred);
    }
    for (int r : { 48000, 44100, 24000 }) {
        for (int ch : { 2, 1 }) {
            QAudioFormat f;
            f.setSampleRate(r);
            f.setChannelCount(ch);
            f.setSampleFormat(QAudioFormat::Float);
            candidates.append(f);
            f.setSampleFormat(QAudioFormat::Int16);
            candidates.append(f);
        }
    }

    for (const QAudioFormat& f : candidates) {
        audioFormat = f;
        targetRate = f.sampleRate();
        targetChannels = f.channelCount();
        targetSampleFormat = f.sampleFormat();
        sink = new QAudioSink(dev, audioFormat, this);
        sink->setBufferSize(targetRate * targetChannels
                            * bytesPerSample(targetSampleFormat) * 2 / 5);   // 0.4s 缓冲
        audioIo = sink->start();
        if (audioIo) return;   // 打开成功
        closeAudio();          // 失败则尝试下一个候选格式
    }

    emit failed(QStringLiteral("无法打开音频输出设备（已尝试多种输出格式）"));
}

/**
 * @brief 将任意源格式的 PCM 转换为音频设备输出格式
 * 步骤：格式归一化为 Int16 → 多声道下混为单声道 → 线性插值重采样 → 扩声道 + 目标格式转换
 */
QByteArray QwenTtsClient::convertPcm(const QByteArray& pcm, int srcRate, int srcChannels,
                                     QAudioFormat::SampleFormat srcFormat) {
    if (pcm.isEmpty() || srcRate <= 0 || srcChannels <= 0) return QByteArray();
    const int srcBytes = bytesPerSample(srcFormat);
    const int totalFrames = pcm.size() / (srcBytes * srcChannels);
    if (totalFrames <= 0) return QByteArray();
    const char* raw = pcm.constData();

    // 1) 归一化为单声道 Int16（采样格式转换 + 下混）
    QByteArray mono1;
    mono1.resize(totalFrames * 2);
    qint16* mono = reinterpret_cast<qint16*>(mono1.data());
    for (int i = 0; i < totalFrames; ++i) {
        qint32 acc = 0;
        for (int c = 0; c < srcChannels; ++c) {
            const char* p = raw + (i * srcChannels + c) * srcBytes;
            qint32 v = 0;
            switch (srcFormat) {
            case QAudioFormat::UInt8:
                v = (qint32(*reinterpret_cast<const uchar*>(p)) - 128) << 8;
                break;
            case QAudioFormat::Int32:
                v = (*reinterpret_cast<const qint32*>(p)) >> 16;
                break;
            case QAudioFormat::Float: {
                const float f = *reinterpret_cast<const float*>(p);
                v = int(qBound(-1.0f, f, 1.0f) * 32767.0f);
                break;
            }
            case QAudioFormat::Int16:
            default:
                v = *reinterpret_cast<const qint16*>(p);
                break;
            }
            acc += v;
        }
        mono[i] = static_cast<qint16>(qBound<qint32>(-32768, acc / srcChannels, 32767));
    }

    // 2) 重采样 srcRate → targetRate（线性插值）
    QByteArray mono2;
    const qint16* monoData = reinterpret_cast<const qint16*>(mono1.constData());
    int monoN = totalFrames;
    if (targetRate != srcRate) {
        monoN = int((qint64)totalFrames * targetRate / srcRate);
        if (monoN <= 0) return QByteArray();
        mono2.resize(monoN * 2);
        qint16* dst = reinterpret_cast<qint16*>(mono2.data());
        for (int i = 0; i < monoN; ++i) {
            const double pos = double(i) * srcRate / targetRate;
            int i0 = int(pos);
            if (i0 > totalFrames - 1) i0 = totalFrames - 1;
            const int i1 = qMin(i0 + 1, totalFrames - 1);
            const double frac = pos - i0;
            const double s = mono[i0] + (mono[i1] - mono[i0]) * frac;
            dst[i] = static_cast<qint16>(int(qBound(-32768.0, s, 32767.0)));
        }
        monoData = reinterpret_cast<const qint16*>(mono2.constData());
    }

    // 3) 单声道 → 多声道复制 + 目标采样格式转换（一次遍历完成）
    if (targetChannels == 1 && targetSampleFormat == QAudioFormat::Int16)
        return mono2.isEmpty() ? mono1 : mono2;

    const int dstBytes = bytesPerSample(targetSampleFormat);
    QByteArray out;
    out.resize(monoN * targetChannels * dstBytes);
    char* o = out.data();
    for (int i = 0; i < monoN; ++i) {
        const qint16 v = monoData[i];
        for (int c = 0; c < targetChannels; ++c) {
            char* p = o + (i * targetChannels + c) * dstBytes;
            switch (targetSampleFormat) {
            case QAudioFormat::UInt8:
                *reinterpret_cast<uchar*>(p) = static_cast<uchar>((v >> 8) + 128);
                break;
            case QAudioFormat::Int32:
                *reinterpret_cast<qint32*>(p) = qint32(v) << 16;
                break;
            case QAudioFormat::Float:
                *reinterpret_cast<float*>(p) = float(v) / 32768.0f;
                break;
            case QAudioFormat::Int16:
            default:
                *reinterpret_cast<qint16*>(p) = v;
                break;
            }
        }
    }
    return out;
}

void QwenTtsClient::closeAudio() {
    if (!sink) return;
    sink->stop();
    sink->deleteLater();
    sink = nullptr;
    audioIo = nullptr;
}

void QwenTtsClient::onDisconnected() {
    const bool handshakeFailed = connecting;
    connecting = false;
    if (handshakeFailed) {
        ++connectFailures;
        if (connectFailures >= kMaxConnectFailures) {
            emit failed(QStringLiteral("千问TTS连续连接失败，已放弃当前朗读: %1")
                            .arg(lastSocketError.isEmpty() ? QStringLiteral("连接关闭") : lastSocketError));
            queue.clear();
            connectFailures = 0;
        }
    } else if (!currentText.isEmpty()) {
        emit failed(QStringLiteral("千问TTS连接中断: %1")
                        .arg(lastSocketError.isEmpty() ? QStringLiteral("连接关闭") : lastSocketError));
        currentText.clear();
    }
    sessionReady = false;
    configuredVoice.clear();
    taskStarted = false;
    taskId.clear();
    if (!queue.isEmpty() && connectFailures < kMaxConnectFailures) retryTimer.start();
}

void QwenTtsClient::onErrorOccurred() {
    lastSocketError = ws.errorString();
}

void QwenTtsClient::clearSession(bool reportFailure, const QString& reason) {
    sessionReady = false;
    connecting = false;
    configuredVoice.clear();
    taskStarted = false;
    taskId.clear();
    currentText.clear();
    if (reportFailure && !reason.isEmpty()) emit failed(reason);
    if (!queue.isEmpty()) retryTimer.start();
}
