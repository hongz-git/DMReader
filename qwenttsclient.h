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
 * @brief 通义千问云端语音合成客户端（DashScope 实时 WebSocket）
 *
 * 支持两类模型协议：
 *  1) realtime 系（如 qwen3-tts-flash-realtime）：
 *     wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=<model>
 *     session.update 指定音色，input_text_buffer.append + commit 提交，
 *     response.audio.delta 携带 base64 PCM（24kHz）
 *  2) audio-tts 系（如 qwen-audio-3.0-tts-plus）：
 *     wss://dashscope.aliyuncs.com/api-ws/v1/inference/
 *     run-task → task-started → continue-task(+finish-task) →
 *     二进制 PCM 音频帧（48kHz）→ task-finished
 *
 * 播放：QAudioSink 推送模式，音频统一在本地转换为设备支持的格式；
 * 连接按需建立并跨多次朗读复用，断线后在下一次朗读时自动重建。
 */
class QwenTtsClient : public QObject {
    Q_OBJECT

public:
    explicit QwenTtsClient(QObject* parent = nullptr);
    ~QwenTtsClient() override;

    void setApiKey(const QString& key);
    void setModel(const QString& model);   // 切换模型（会重建连接）
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
    void onBinaryMessageReceived(const QByteArray& message);
    void onDisconnected();
    void onErrorOccurred();
    void onPumpTimerTimeout();             // 定时把积压 PCM 写入音频设备

private:
    bool isRealtimeProtocol() const;
    void connectToService();
    void sendJson(const QJsonObject& obj);
    void sendSessionUpdate();              // realtime 系：会话参数
    void sendRunTask();                    // audio-tts 系：开始任务
    void sendContinueTask(const QString& text);
    void sendFinishTask();
    void trySpeakNext();
    void openAudio();
    void closeAudio();
    void clearSession(bool reportFailure, const QString& reason);

    QWebSocket ws;
    QTimer pumpTimer;
    QTimer retryTimer;

    QString apiKey;
    QString model = QStringLiteral("qwen3-tts-flash-realtime");
    QString voice = "Cherry";
    QString configuredVoice;               // realtime 系当前已下发到服务端的音色
    QString lastSocketError;
    int volume = 100;
    int connectFailures = 0;
    int apiSampleRate = 24000;             // API 输出采样率（按模型协议确定）

    QString taskId;                        // audio-tts 系当前任务 ID
    bool taskStarted = false;

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
    QAudioFormat::SampleFormat targetSampleFormat = QAudioFormat::Int16;

    QByteArray convertPcm(const QByteArray& pcm);   // API 输出格式 → 设备格式
};

#endif // QWENTTSCLIENT_H
