#include "qwenttsclient.h"

#include <QNetworkRequest>
#include <QJsonDocument>
#include <QMediaDevices>

static const char* kRealtimeUrl =
    "wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen3-tts-flash-realtime";
static const int kSampleRate = 24000;   // 协议约定的 PCM 输出采样率
static const int kMaxConnectFailures = 3;

QwenTtsClient::QwenTtsClient(QObject* parent)
    : QObject(parent) {
    audioFormat.setSampleRate(kSampleRate);
    audioFormat.setChannelCount(1);
    audioFormat.setSampleFormat(QAudioFormat::Int16);

    connect(&ws, &QWebSocket::connected, this, &QwenTtsClient::onConnected);
    connect(&ws, &QWebSocket::textMessageReceived, this, &QwenTtsClient::onTextMessageReceived);
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
}

void QwenTtsClient::connectToService() {
    if (apiKey.isEmpty()) {
        clearSession(false, QString());
        emit failed(QStringLiteral("未配置 DashScope API Key，无法使用千问 TTS"));
        return;
    }
    if (connecting || ws.state() != QAbstractSocket::UnconnectedState) return;

    connecting = true;
    QNetworkRequest req(QUrl(QString::fromLatin1(kRealtimeUrl)));
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
    session["sample_rate"] = kSampleRate;
    QJsonObject obj;
    obj["type"] = QStringLiteral("session.update");
    obj["session"] = session;
    sendJson(obj);
    configuredVoice = voice;
}

void QwenTtsClient::onConnected() {
    connecting = false;
    connectFailures = 0;
    sessionReady = true;
    sendSessionUpdate();
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
}

void QwenTtsClient::onTextMessageReceived(const QString& message) {
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) return;
    QJsonObject root = doc.object();
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

    // API 固定输出 24kHz/单声道 PCM，本地负责转换。
    // 优先用设备首选格式（统一为 16bit），严格检查不过再按常见组合探测，
    // 避免部分声卡/驱动拒绝 24kHz 或单声道格式导致无法播放。
    QAudioFormat f = dev.preferredFormat();
    f.setSampleFormat(QAudioFormat::Int16);
    if (!dev.isFormatSupported(f)) {
        bool ok = false;
        const int rates[] = { 48000, 44100, 24000, 22050, 16000 };
        const int channelCounts[] = { 2, 1 };
        for (int ch : channelCounts) {
            for (int r : rates) {
                QAudioFormat t;
                t.setSampleRate(r);
                t.setChannelCount(ch);
                t.setSampleFormat(QAudioFormat::Int16);
                if (dev.isFormatSupported(t)) {
                    f = t;
                    ok = true;
                    break;
                }
            }
            if (ok) break;
        }
        if (!ok) {
            emit failed(QStringLiteral("无法找到音频设备支持的输出格式"));
            return;
        }
    }

    audioFormat = f;
    targetRate = f.sampleRate();
    targetChannels = f.channelCount();
    sink = new QAudioSink(dev, audioFormat, this);
    sink->setBufferSize(targetRate * targetChannels * 4 / 5);   // 0.4s 缓冲（16bit）
    audioIo = sink->start();
    if (!audioIo) {
        emit failed(QStringLiteral("音频设备启动失败"));
        closeAudio();
    }
}

/**
 * @brief 将 API 输出的 24kHz/单声道 Int16 PCM 转换为音频设备支持的格式
 * 线性插值重采样 + 单声道复制，语音场景质量足够
 */
QByteArray QwenTtsClient::convertPcm(const QByteArray& pcm) {
    if (pcm.isEmpty()) return pcm;
    if (targetRate == kSampleRate && targetChannels == 1) return pcm;

    const qint16* src = reinterpret_cast<const qint16*>(pcm.constData());
    const int srcN = pcm.size() / 2;
    if (srcN <= 0) return QByteArray();

    // 1) 重采样 24kHz → targetRate（线性插值）
    QByteArray mono;
    const qint16* monoData = src;
    int monoN = srcN;
    if (targetRate != kSampleRate) {
        const int dstN = int((qint64)srcN * targetRate / kSampleRate);
        if (dstN <= 0) return QByteArray();
        mono.resize(dstN * 2);
        qint16* dst = reinterpret_cast<qint16*>(mono.data());
        for (int i = 0; i < dstN; ++i) {
            const double pos = double(i) * kSampleRate / targetRate;
            int i0 = int(pos);
            if (i0 > srcN - 1) i0 = srcN - 1;
            const int i1 = qMin(i0 + 1, srcN - 1);
            const double frac = pos - i0;
            const double s = src[i0] + (src[i1] - src[i0]) * frac;
            dst[i] = static_cast<qint16>(int(qBound(-32768.0, s, 32767.0)));
        }
        monoData = reinterpret_cast<const qint16*>(mono.constData());
        monoN = dstN;
    }

    if (targetChannels == 1) return mono.isEmpty() ? pcm : mono;

    // 2) 单声道 → 多声道复制
    QByteArray out;
    out.resize(monoN * 2 * targetChannels);
    qint16* o = reinterpret_cast<qint16*>(out.data());
    for (int i = 0; i < monoN; ++i) {
        for (int c = 0; c < targetChannels; ++c)
            o[i * targetChannels + c] = monoData[i];
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
    if (!queue.isEmpty() && connectFailures < kMaxConnectFailures) retryTimer.start();
}

void QwenTtsClient::onErrorOccurred() {
    lastSocketError = ws.errorString();
}

void QwenTtsClient::clearSession(bool reportFailure, const QString& reason) {
    sessionReady = false;
    connecting = false;
    configuredVoice.clear();
    currentText.clear();
    if (reportFailure && !reason.isEmpty()) emit failed(reason);
    if (!queue.isEmpty()) retryTimer.start();
}
