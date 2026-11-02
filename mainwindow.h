#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QWebSocket>
#include <QTextToSpeech>
#include <QListWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QComboBox>
#include <QSlider>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QGroupBox>
#include <QCheckBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QTabWidget>
#include <QSplitter>
#include <QScrollBar>
#include <QMessageBox>
#include <QDateTime>
#include <QProcess>
#include <QQueue>
#include <QTimer>
#include <QSettings>
#include <QMap>

#include "qwenttsclient.h"

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    void onTextMessageReceived(const QString& message);
    void onWebSocketConnected();
    void onWebSocketDisconnected();
    void onWebSocketError(QAbstractSocket::SocketError error);
    void onStateChanged(QTextToSpeech::State state);
    void onTtsTimerTimeout();
    void onRandomTimerTimeout();
    void onReconnectTimerTimeout();
    void onGatewayWatchdogTimeout();
    void onAlwaysOnTopToggled(bool checked);
    void onOpacityChanged(int value);
    void onEngineChanged(int index);
    void saveConfig();
    void loadConfig();

private:
    // UI
    QLineEdit* urlInput;
    QPushButton* connectBtn;
    QPushButton* disconnectBtn;
    QListWidget* danmuList;
    QComboBox* roomCombo;
    QComboBox* engineCombo;
    QComboBox* voiceCombo;
    QComboBox* qwenVoiceCombo;
    QLineEdit* apiKeyInput;
    QLabel* systemVoiceLabel;
    QLabel* qwenVoiceLabel;
    QLabel* qwenKeyLabel;
    QSlider* rateSlider;
    QLabel* rateLabel;
    QTextEdit* logView;
    QCheckBox* danmuSwitch;
    QCheckBox* giftSwitch;
    QCheckBox* enterSwitch;
    QCheckBox* randomSwitch;
    QSpinBox* randomIntervalSpin;
    QPlainTextEdit* randomTextEdit;
    QSlider* volumeSlider;
    QLabel* volumeLabel;
    QCheckBox* alwaysOnTopCheck;
    QSlider* opacitySlider;
    QLabel* opacityLabel;
    QLineEdit* danmuTemplateInput;
    QLineEdit* giftTemplateInput;
    QLineEdit* enterTemplateInput;

    // 弹幕
    QWebSocket* webSocket;
    bool connected;
    QMap<QString, QString> roomNames;   // WebRoomId -> 展示名（主播昵称/房间标题）

    // TTS
    QTextToSpeech* tts;
    QVector<QVoice> voices;
    QwenTtsClient* qwenTts;
    QDateTime lastSpeakTime;
    QTimer* randomTimer;

    QProcess* gatewayProcess;
    QQueue<QString> ttsQueue;
    QTimer* ttsTimer;
    QTimer* reconnectTimer;
    QTimer* gatewayWatchdogTimer;
    int reconnectAttempts;
    bool waitingForGateway;
    bool manualDisconnect;
    QString danmuTpl;
    QString giftTpl;
    QString enterTpl;

    void log(const QString& msg);
    void speak(const QString& text);
    void enqueueSpeak(const QString& text);
    void parseDanmu(const QString& json);
    QString roomLabelFor(const QString& webRoomId) const;
    void refreshRoomCombo(const QString& keepId);
    QString selectedRoomId() const;
    bool engineIsQwen() const;
    void updateEngineUi();

    bool ensureGatewayRunning();
    void startGateway();
    void stopGateway();
};

#endif // MAINWINDOW_H
