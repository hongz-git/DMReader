#ifndef QWENTTSCLIENT_H
#define QWENTTSCLIENT_H

#include <QObject>
#include <QWebSocket>
#include <QAudioSink>
#include <QAudio>
#include <QAudioFormat>
#include <QIODevice>
#include <QTimer>
#include <QQueue>
#include <QJsonObject>

/**
 * @brief 通义千问实时语音合成客户端（DashScope qwen3-tts-flash-realtime）
 *
 * 协议：wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen3-tts-flash-realtime
 *  - 鉴权：握手头 Authorization: Bearer <apiKey>
 *  - 会话：session.update 指定音色 / 输出格式（commit 模式手动提交合成）
 *  - 合成：input_text_buffer.append + input_text_buffer.commit
 *  - 音频：response.audio.delta 携带 base64 编码的 PCM 数据
 *
 * 播放：QAudioSink 推送模式，24kHz/16bit/单声道，软件音量增益；
 * 连接按需建立并跨多次朗读复用，断线后在下一次朗读时自动重建。
 */
class QwenTtsClient : public QObject {
    Q_OBJECT

public:
    explicit QwenTtsClient(QObject* parent = nullptr);
    ~QwenTtsClient() override;

    void setApiKey(const QString& key);
    void setVoice(const QString& voice);   // voice 参数，如 "Cherry"
    void setVolume(int percent);           // 0~100，作用于输出 PCM 的软件增益
    void say(const QString& text);         // 入队待朗读文本
    void stopAll();                        // 清空队列、停止播放并断开连接
    bool isBusy() const;                   // 正在连接 / 合成 / 缓冲中

signals:
    void started(const QString& text);
    void finished();                       // 一段文本的音频已交付设备
    void failed(const QString& message);

private slots:
    void onConnected();
    void onTextMessageReceived(const QString& message);
    void onDisconnected();
    void onErrorOccurred();
    void onPumpTimerTimeout();             // 定时把积压 PCM 写入音频设备

private:
    void connectToService();
    void sendJson(const QJsonObject& obj);
    void sendSessionUpdate();
    void trySpeakNext();
    void openAudio();
    void closeAudio();
    void clearSession(bool reportFailure, const QString& reason);

    QWebSocket ws;
    QTimer pumpTimer;
    QTimer retryTimer;

    QString apiKey;
    QString voice = "Cherry";
    QString configuredVoice;               // 当前已下发到服务端的音色
    QString lastSocketError;
    int volume = 100;
    int connectFailures = 0;

    QQueue<QString> queue;                 // 待朗读队列（由外部串行调用，深度通常为 1）
    QString currentText;                   // 正在合成的文本
    bool connecting = false;
    bool sessionReady = false;

    QByteArray pendingPcm;                 // 尚未写入音频设备的 PCM（已转换为目标格式）
    QAudioSink* sink = nullptr;
    QIODevice* audioIo = nullptr;          // 推送模式写入端
    QAudioFormat audioFormat;
    int targetRate = 24000;                // 设备实际支持的输出采样率（openAudio 时确定）
    int targetChannels = 1;                // 设备实际支持的输出声道数

    QByteArray convertPcm(const QByteArray& pcm);   // 24kHz/单声道 → 设备格式
};

#endif // QWENTTSCLIENT_H
