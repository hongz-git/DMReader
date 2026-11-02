#include "qwenttsclient.h"

#include <QNetworkRequest>
#include <QJsonDocument>
#include <QList>
#include <QMediaDevices>
#include <QUuid>

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

QwenTtsClient::QwenTtsClient(QObject* parent)
    : QObject(parent) {
    audioFormat.setSampleRate(apiSampleRate);
    audioFormat.setChannelCount(1);
    audioFormat.setSampleFormat(QAudioFormat::Int16);

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
    // 两类模型的端点与事件协议不同，切换后强制重建连接
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
    return connecting || !currentText.isEmpty() || !queue.isEmpty() || !pendingPcm.isEmpty();
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

void QwenTtsClient::onConnected() {
    connecting = false;
    connectFailures = 0;
    sessionReady = true;
    if (isRealtimeProtocol()) sendSessionUpdate();
    trySpeakNext();
}

void QwenTtsClient::trySpeakNext() {
    if (!currentText.isEmpty() || queue.isEmpty()) return;

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
        pendingPcm.append(convertPcm(pcm));
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
    pendingPcm.append(convertPcm(pcm));
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
 * @brief 将 API 输出的单声道 Int16 PCM（apiSampleRate）转换为音频设备输出格式
 * 线性插值重采样 + 单声道复制 + 采样格式转换，语音场景质量足够
 */
QByteArray QwenTtsClient::convertPcm(const QByteArray& pcm) {
    if (pcm.isEmpty()) return pcm;

    const qint16* src = reinterpret_cast<const qint16*>(pcm.constData());
    const int srcN = pcm.size() / 2;
    if (srcN <= 0) return QByteArray();

    // 1) 重采样 apiSampleRate → targetRate（线性插值），中间格式保持 Int16
    QByteArray mono;
    const qint16* monoData = src;
    int monoN = srcN;
    if (targetRate != apiSampleRate) {
        monoN = int((qint64)srcN * targetRate / apiSampleRate);
        if (monoN <= 0) return QByteArray();
        mono.resize(monoN * 2);
        qint16* dst = reinterpret_cast<qint16*>(mono.data());
        for (int i = 0; i < monoN; ++i) {
            const double pos = double(i) * apiSampleRate / targetRate;
            int i0 = int(pos);
            if (i0 > srcN - 1) i0 = srcN - 1;
            const int i1 = qMin(i0 + 1, srcN - 1);
            const double frac = pos - i0;
            const double s = src[i0] + (src[i1] - src[i0]) * frac;
            dst[i] = static_cast<qint16>(int(qBound(-32768.0, s, 32767.0)));
        }
        monoData = reinterpret_cast<const qint16*>(mono.constData());
    }

    // 2) 单声道 → 多声道复制 + 采样格式转换（一次遍历完成）
    if (targetChannels == 1 && targetSampleFormat == QAudioFormat::Int16)
        return mono.isEmpty() ? pcm : mono;

    const int bytesPer = bytesPerSample(targetSampleFormat);
    QByteArray out;
    out.resize(monoN * targetChannels * bytesPer);
    char* o = out.data();
    for (int i = 0; i < monoN; ++i) {
        const qint16 v = monoData[i];
        for (int c = 0; c < targetChannels; ++c) {
            char* p = o + (i * targetChannels + c) * bytesPer;
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
