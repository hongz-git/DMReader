#include "mainwindow.h"

#include <QDir>
#include <QThread>
#include <QGroupBox>
#include <QGridLayout>
#include <QSettings>
#include <QCoreApplication>
#include <QRandomGenerator>

#include <algorithm>

/**
 * @brief 检查网关是否已经在运行（通过尝试连接 ws 端口）
 * @return true 表示网关已就绪
 */
bool MainWindow::ensureGatewayRunning() {
    QWebSocket probe;
    QEventLoop loop;
    bool ok = false;

    QObject::connect(&probe, &QWebSocket::connected, [&]() {
        ok = true;
        probe.close();
    });
    QObject::connect(&probe, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
                     [&loop]() { loop.quit(); });

    probe.open(QUrl(urlInput->text().trimmed()));
    QTimer::singleShot(1500, &loop, &QEventLoop::quit);
    loop.exec();

    return ok;
}

/**
 * @brief 启动网关子进程（后台常驻）
 */
void MainWindow::startGateway() {
    // 先释放旧的，防止多次 new 泄漏
    stopGateway();

    QString gatewayDir = QCoreApplication::applicationDirPath() + "/gateway";
    QDir dir(gatewayDir);
    QStringList exes = dir.entryList(QStringList() << "*.exe", QDir::Files);

    if (exes.isEmpty()) {
        log("⚠️ gateway 文件夹下没有找到任何 exe 文件");
        log("请确认已将网关程序解压到: " + gatewayDir);
        return;
    }

    QString gatewayExe;
    for (const QString& exe : exes) {
        if (exe.contains("Wss", Qt::CaseInsensitive) ||
            exe.contains("Barrage", Qt::CaseInsensitive) ||
            exe.contains("Grab", Qt::CaseInsensitive)) {
            gatewayExe = gatewayDir + "/" + exe;
            break;
        }
    }
    if (gatewayExe.isEmpty()) gatewayExe = gatewayDir + "/" + exes.first();

    log("找到网关: " + gatewayExe);

    gatewayProcess = new QProcess(this);
    gatewayProcess->setWorkingDirectory(gatewayDir);
    gatewayProcess->start(gatewayExe, QStringList());
    log("正在启动弹幕网关...");
}

/**
 * @brief 停止网关子进程
 */
void MainWindow::stopGateway() {
    if (gatewayProcess) {
        if (gatewayProcess->state() == QProcess::Running) {
            gatewayProcess->kill();
            gatewayProcess->waitForFinished(3000);
        }
        gatewayProcess->deleteLater();
        gatewayProcess = nullptr;
    }
}

/**
 * @brief 入队朗读（防刷屏）
 */
void MainWindow::enqueueSpeak(const QString& text) {
    lastSpeakTime = QDateTime::currentDateTime();
    ttsQueue.enqueue(text);
    if (ttsQueue.size() > 10) ttsQueue.dequeue();
}

void MainWindow::onTtsTimerTimeout() {
    if (ttsQueue.isEmpty()) return;

    if (engineIsQwen()) {
        if (!qwenTts->isBusy()) {
            qwenTts->say(ttsQueue.dequeue());
        }
        return;
    }

    if (tts->state() != QTextToSpeech::Speaking) {
        tts->say(ttsQueue.dequeue());
    }
}

void MainWindow::onRandomTimerTimeout() {
    if (!randomSwitch->isChecked()) return;
    if (!ttsQueue.isEmpty()) return;
    if (engineIsQwen() ? qwenTts->isBusy() : tts->state() == QTextToSpeech::Speaking) return;

    const qint64 idleSec = lastSpeakTime.secsTo(QDateTime::currentDateTime());
    if (idleSec < randomIntervalSpin->value()) return;

    const QStringList lines = randomTextEdit->toPlainText().split('\n', Qt::SkipEmptyParts);
    if (lines.isEmpty()) return;

    const QString line = lines.at(QRandomGenerator::global()->bounded(lines.size())).trimmed();
    if (!line.isEmpty()) {
        log("🎲 随机朗读: " + line);
        enqueueSpeak(line);
    }
}

void MainWindow::onReconnectTimerTimeout() {
    if (connected) { reconnectTimer->stop(); return; }
    reconnectAttempts++;
    if (reconnectAttempts > 10) {
        log("重连次数过多，停止自动重连");
        reconnectTimer->stop();
        return;
    }
    log(QString("尝试第 %1 次重连...").arg(reconnectAttempts));
    webSocket->open(QUrl(urlInput->text().trimmed()));
}

void MainWindow::onGatewayWatchdogTimeout() {
    if (!gatewayProcess) return;
    if (gatewayProcess->state() != QProcess::Running) {
        log("检测到网关进程已退出，正在重新启动...");
        startGateway();
    }
}

void MainWindow::onAlwaysOnTopToggled(bool checked) {
    setWindowFlag(Qt::WindowStaysOnTopHint, checked);
    show();
    log(checked ? "窗口已置顶" : "窗口已取消置顶");
}

void MainWindow::onOpacityChanged(int value) {
    setWindowOpacity(value / 100.0);
    opacityLabel->setText(QString::number(value) + "%");
}

void MainWindow::saveConfig() {
    QSettings settings("Byjsmc", "DouyinDanmuTTS");
    settings.setValue("url", urlInput->text());
    settings.setValue("rate", rateSlider->value());
    settings.setValue("volume", volumeSlider->value());
    settings.setValue("voiceIndex", voiceCombo->currentIndex());
    settings.setValue("danmuEnabled", danmuSwitch->isChecked());
    settings.setValue("giftEnabled", giftSwitch->isChecked());
    settings.setValue("enterEnabled", enterSwitch->isChecked());
    settings.setValue("alwaysOnTop", alwaysOnTopCheck->isChecked());
    settings.setValue("opacity", opacitySlider->value());
    settings.setValue("danmuTemplate", danmuTemplateInput->text());
    settings.setValue("giftTemplate", giftTemplateInput->text());
    settings.setValue("engine", engineCombo->currentData().toString());
    settings.setValue("apiKey", apiKeyInput->text());
    settings.setValue("qwenVoiceIndex", qwenVoiceCombo->currentIndex());
    settings.setValue("roomId", selectedRoomId());
    settings.setValue("randomEnabled", randomSwitch->isChecked());
    settings.setValue("randomInterval", randomIntervalSpin->value());
    settings.setValue("randomText", randomTextEdit->toPlainText());
}

void MainWindow::loadConfig() {
    QSettings settings("Byjsmc", "DouyinDanmuTTS");
    urlInput->setText(settings.value("url", "ws://127.0.0.1:8888").toString());
    rateSlider->setValue(settings.value("rate", 50).toInt());
    volumeSlider->setValue(settings.value("volume", 50).toInt());

    int savedVoice = settings.value("voiceIndex", 0).toInt();
    if (savedVoice >= 0 && savedVoice < voices.size())
        voiceCombo->setCurrentIndex(savedVoice);
    else
        voiceCombo->setCurrentIndex(0);

    danmuSwitch->setChecked(settings.value("danmuEnabled", true).toBool());
    giftSwitch->setChecked(settings.value("giftEnabled", true).toBool());
    enterSwitch->setChecked(settings.value("enterEnabled", true).toBool());
    alwaysOnTopCheck->setChecked(settings.value("alwaysOnTop", false).toBool());
    opacitySlider->setValue(settings.value("opacity", 100).toInt());
    danmuTemplateInput->setText(settings.value("danmuTemplate", "%1说：%2").toString());
    giftTemplateInput->setText(settings.value("giftTemplate", "感谢%1送出的%2%3个").toString());

    randomSwitch->setChecked(settings.value("randomEnabled", false).toBool());
    randomIntervalSpin->setValue(settings.value("randomInterval", 60).toInt());
    randomTextEdit->setPlainText(settings.value("randomText", "").toString());
    apiKeyInput->setText(settings.value("apiKey", "").toString());

    int savedQwenVoice = settings.value("qwenVoiceIndex", 0).toInt();
    if (savedQwenVoice >= 0 && savedQwenVoice < qwenVoiceCombo->count())
        qwenVoiceCombo->setCurrentIndex(savedQwenVoice);

    // 房间选择恢复（房间未被发现时先按 ID 占位）
    QString savedRoom = settings.value("roomId", "").toString();
    refreshRoomCombo(savedRoom);

    QString engine = settings.value("engine", "system").toString();
    int engineIdx = engineCombo->findData(engine);
    engineCombo->setCurrentIndex(engineIdx >= 0 ? engineIdx : 0);
    updateEngineUi();

    qwenTts->setApiKey(apiKeyInput->text());
    if (qwenVoiceCombo->currentIndex() >= 0)
        qwenTts->setVoice(qwenVoiceCombo->itemData(qwenVoiceCombo->currentIndex()).toString());

    tts->setRate(rateSlider->value() / 100.0);
    tts->setVolume(volumeSlider->value() / 100.0);
    if (voiceCombo->currentIndex() >= 0 && voiceCombo->currentIndex() < voices.size())
        tts->setVoice(voices[voiceCombo->currentIndex()]);
    setWindowOpacity(opacitySlider->value() / 100.0);

    danmuTpl = danmuTemplateInput->text();
    giftTpl = giftTemplateInput->text();
}

/**
 * @brief 主窗口构造函数
 * 初始化 UI、WebSocket 连接和 TTS 引擎
 */
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), connected(false), gatewayProcess(nullptr),
    waitingForGateway(false), reconnectAttempts(0), manualDisconnect(false) {

    // ========== 窗口基本设置 ==========
    setWindowTitle("抖音弹幕朗读助手");
    resize(560, 900);

    // ========== TTS 引擎初始化 ==========
    tts = new QTextToSpeech(this);
    voices = tts->availableVoices();

    qwenTts = new QwenTtsClient(this);
    connect(qwenTts, &QwenTtsClient::started, this, [this](const QString& t) {
        Q_UNUSED(t);
    });
    connect(qwenTts, &QwenTtsClient::failed, this, [this](const QString& msg) {
        log("⚠️ " + msg);
    });

    // 随机朗读定时器
    randomTimer = new QTimer(this);
    randomTimer->setInterval(5000);
    connect(randomTimer, &QTimer::timeout, this, &MainWindow::onRandomTimerTimeout);
    lastSpeakTime = QDateTime::currentDateTime();

    // ========== WebSocket 初始化 ==========
    webSocket = new QWebSocket();

    // 定时器初始化
    ttsTimer = new QTimer(this);
    ttsTimer->setInterval(2000);
    reconnectTimer = new QTimer(this);
    reconnectTimer->setInterval(5000);
    gatewayWatchdogTimer = new QTimer(this);
    gatewayWatchdogTimer->setInterval(10000);

    // ========== 中央部件和主布局 ==========
    QWidget* central = new QWidget();
    setCentralWidget(central);
    QVBoxLayout* layout = new QVBoxLayout(central);
    layout->setSpacing(8);
    layout->setContentsMargins(12, 10, 12, 10);

    // ========== 标题 ==========
    QLabel* title = new QLabel("🎙️ 抖音弹幕朗读助手");
    title->setStyleSheet("font-size: 20px; font-weight: bold; color: #5b8def;");
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);

    // ========== 连接设置 ==========
    QGroupBox* connGroup = new QGroupBox("连接");
    QGridLayout* connLayout = new QGridLayout(connGroup);
    urlInput = new QLineEdit("ws://127.0.0.1:8888");
    urlInput->setPlaceholderText("弹幕网关地址");
    connectBtn = new QPushButton("连接");
    disconnectBtn = new QPushButton("断开");
    disconnectBtn->setEnabled(false);
    connLayout->addWidget(urlInput, 0, 0);
    connLayout->addWidget(connectBtn, 0, 1);
    connLayout->addWidget(disconnectBtn, 0, 2);
    connLayout->addWidget(new QLabel("房间："), 1, 0);
    roomCombo = new QComboBox();
    roomCombo->addItem("全部房间", QString());
    roomCombo->setToolTip("打开直播间后自动发现房间；选择指定房间只朗读该直播间");
    connLayout->addWidget(roomCombo, 1, 1, 1, 2);
    layout->addWidget(connGroup);

    // ========== 功能开关 ==========
    QGroupBox* switchGroup = new QGroupBox("朗读开关");
    QHBoxLayout* switchLayout = new QHBoxLayout(switchGroup);
    danmuSwitch = new QCheckBox("弹幕朗读");
    giftSwitch = new QCheckBox("礼物朗读");
    enterSwitch = new QCheckBox("进房播报");  // 新增
    danmuSwitch->setChecked(true);
    giftSwitch->setChecked(true);
    enterSwitch->setChecked(true);             // 新增
    switchLayout->addWidget(danmuSwitch);
    switchLayout->addWidget(giftSwitch);
    switchLayout->addWidget(enterSwitch);      // 新增
    switchLayout->addStretch();
    layout->addWidget(switchGroup);

    // 随机朗读（长时间无弹幕时暖场）
    QGroupBox* randomGroup = new QGroupBox("随机朗读");
    QVBoxLayout* randomLayout = new QVBoxLayout(randomGroup);
    QHBoxLayout* randomCfgLayout = new QHBoxLayout();
    randomSwitch = new QCheckBox("启用（无朗读持续");
    randomSwitch->setToolTip("超过设定时长没有任何朗读时，随机播放一行随机模板");
    randomIntervalSpin = new QSpinBox();
    randomIntervalSpin->setRange(10, 3600);
    randomIntervalSpin->setValue(60);
    randomIntervalSpin->setSuffix(" 秒");
    QLabel* randomSuffixLabel = new QLabel("后随机播放一行）");
    randomCfgLayout->addWidget(randomSwitch);
    randomCfgLayout->addWidget(randomIntervalSpin);
    randomCfgLayout->addWidget(randomSuffixLabel);
    randomCfgLayout->addStretch();
    randomLayout->addLayout(randomCfgLayout);
    randomTextEdit = new QPlainTextEdit();
    randomTextEdit->setPlaceholderText("一行一条随机话术，例如：\n欢迎来到直播间，喜欢主播点个关注哦～\n有什么想听的，打在弹幕里告诉我吧～");
    randomTextEdit->setMaximumHeight(96);
    randomLayout->addWidget(randomTextEdit);
    layout->addWidget(randomGroup);

    // ========== TTS 设置 ==========
    QGroupBox* ttsGroup = new QGroupBox("语音设置");
    QGridLayout* ttsLayout = new QGridLayout(ttsGroup);
    ttsLayout->addWidget(new QLabel("引擎："), 0, 0);
    engineCombo = new QComboBox();
    engineCombo->addItem("系统语音（本地）", "system");
    engineCombo->addItem("通义千问 TTS（云端）", "qwen");
    ttsLayout->addWidget(engineCombo, 0, 1, 1, 2);

    systemVoiceLabel = new QLabel("系统音色：");
    ttsLayout->addWidget(systemVoiceLabel, 1, 0);
    voiceCombo = new QComboBox();
    for (const auto& v : voices) voiceCombo->addItem(v.name());
    ttsLayout->addWidget(voiceCombo, 1, 1, 1, 2);

    qwenVoiceLabel = new QLabel("云端音色：");
    ttsLayout->addWidget(qwenVoiceLabel, 2, 0);
    qwenVoiceCombo = new QComboBox();
    struct QwenVoice { const char* name; const char* param; const char* desc; };
    static const QwenVoice qwenVoices[] = {
        {"芊悦", "Cherry", "阳光积极、亲切自然小姐姐（女性）"},
        {"苏瑶", "Serena", "温柔小姐姐（女性）"},
        {"晨煦", "Ethan", "标准普通话，带部分北方口音。阳光、温暖、活力、朝气（男性）"},
        {"千雪", "Chelsie", "二次元虚拟女友（女性）"},
        {"茉兔", "Momo", "撒娇搞怪，逗你开心（女性）"},
        {"十三", "Vivian", "拽拽的、可爱的小暴躁（女性）"},
        {"月白", "Moon", "率性帅气的月白（男性）"},
        {"四月", "Maia", "知性与温柔的碰撞（女性）"},
        {"凯", "Kai", "耳朵的一场SPA（男性）"},
        {"不吃鱼", "Nofish", "不会翘舌音的设计师（男性）"},
        {"萌宝", "Bella", "喝酒不打醉拳的小萝莉（女性）"},
        {"詹妮弗", "Jennifer", "品牌级、电影质感般美语女声（女性）"},
        {"甜茶", "Ryan", "节奏拉满，戏感炸裂，真实与张力共舞（男性）"},
        {"卡捷琳娜", "Katerina", "御姐音色，韵律回味十足（女性）"},
        {"艾登", "Aiden", "精通厨艺的美语大男孩（男性）"},
        {"沧明子", "Eldric Sage", "沉稳睿智的老者，沧桑如松却心明如镜（男性）"},
        {"乖小妹", "Mia", "温顺如春水，乖巧如初雪（女性）"},
        {"沙小弥", "Mochi", "聪明伶俐的小大人，童真未泯却早慧如禅（男性）"},
        {"燕铮莺", "Bellona", "声音洪亮，吐字清晰，人物鲜活（女性）"},
        {"田叔", "Vincent", "一口独特的沙哑烟嗓，道尽千军万马与江湖豪情（男性）"},
        {"萌小姬", "Bunny", "\"萌属性\"爆棚的小萝莉（女性）"},
        {"阿闻", "Neil", "平直的基线语调，字正腔圆的新闻主持人（男性）"},
        {"墨讲师", "Elias", "学科严谨性与叙事技巧兼备（女性）"},
        {"徐大爷", "Arthur", "被岁月和旱烟浸泡过的质朴嗓音（男性）"},
        {"邻家妹妹", "Nini", "糯米糍一样又软又黏的嗓音（女性）"},
        {"小婉", "Seren", "温和舒缓的声线，助你更快入眠（女性）"},
        {"顽屁小孩", "Pip", "调皮捣蛋却充满童真（男性）"},
        {"少女阿月", "Stella", "甜到发腻的迷糊少女音，正义感瞬间拉满（女性）"},
        {"博德加", "Bodega", "热情的西班牙大叔（男性）"},
        {"索尼莎", "Sonrisa", "热情开朗的拉美大姐（女性）"},
        {"阿列克", "Alek", "战斗民族的冷与毛呢大衣下的暖（男性）"},
        {"多尔切", "Dolce", "慵懒的意大利大叔（男性）"},
        {"素熙", "Sohee", "温柔开朗，情绪丰富的韩国欧尼（女性）"},
        {"小野杏", "Ono Anna", "鬼灵精怪的青梅竹马（女性）"},
        {"莱恩", "Lenn", "理性为底色、叛逆藏细节的德国青年（男性）"},
        {"埃米尔安", "Emilien", "浪漫的法国大哥哥（男性）"},
        {"安德雷", "Andre", "声音磁性，自然舒服、沉稳男生（男性）"},
        {"拉迪奥·戈尔", "Radio Gol", "足球诗人，为足球而生的解说（男性）"},
        {"上海-阿珍", "Jada", "风风火火的沪上阿姐（女性）"},
        {"北京-晓东", "Dylan", "北京胡同里长大的少年（男性）"},
        {"南京-老李", "Li", "耐心的瑜伽老师（男性）"},
        {"陕西-秦川", "Marcus", "面宽话短，心实声沉——老陕的味道（男性）"},
        {"闽南-阿杰", "Roy", "诙谐直爽、市井活泼的台湾哥仔形象（男性）"},
        {"天津-李彼得", "Peter", "天津相声，专业捧哏（男性）"},
        {"四川-晴儿", "Sunny", "甜到你心里的川妹子（女性）"},
        {"四川-程川", "Eric", "一个跳脱市井的四川成都男子（男性）"},
        {"粤语-阿强", "Rocky", "幽默风趣的阿强，在线陪聊（男性）"},
        {"粤语-阿清", "Kiki", "甜美的港妹闺蜜（女性）"},
    };
    for (const auto& v : qwenVoices) {
        qwenVoiceCombo->addItem(QString::fromUtf8(v.name), QString::fromUtf8(v.param));
        qwenVoiceCombo->setItemData(qwenVoiceCombo->count() - 1,
                                    QString::fromUtf8(v.desc), Qt::ToolTipRole);
    }
    ttsLayout->addWidget(qwenVoiceCombo, 2, 1, 1, 2);

    qwenKeyLabel = new QLabel("API Key：");
    ttsLayout->addWidget(qwenKeyLabel, 3, 0);
    apiKeyInput = new QLineEdit();
    apiKeyInput->setEchoMode(QLineEdit::Password);
    apiKeyInput->setPlaceholderText("阿里云百炼 DashScope API Key");
    ttsLayout->addWidget(apiKeyInput, 3, 1, 1, 2);

    ttsLayout->addWidget(new QLabel("语速："), 4, 0);
    rateSlider = new QSlider(Qt::Horizontal);
    rateSlider->setRange(0, 100);
    rateSlider->setValue(50);
    rateLabel = new QLabel("50");
    rateLabel->setFixedWidth(30);
    ttsLayout->addWidget(rateSlider, 4, 1);
    ttsLayout->addWidget(rateLabel, 4, 2);
    // 新增：音量
    ttsLayout->addWidget(new QLabel("音量："), 5, 0);
    volumeSlider = new QSlider(Qt::Horizontal);
    volumeSlider->setRange(0, 100);
    volumeSlider->setValue(50);
    volumeLabel = new QLabel("50");
    volumeLabel->setFixedWidth(30);
    ttsLayout->addWidget(volumeSlider, 5, 1);
    ttsLayout->addWidget(volumeLabel, 5, 2);
    layout->addWidget(ttsGroup);

    // 朗读模板
    QGroupBox* tplGroup = new QGroupBox("朗读模板");
    QGridLayout* tplLayout = new QGridLayout(tplGroup);
    tplLayout->addWidget(new QLabel("弹幕模板："), 0, 0);
    danmuTemplateInput = new QLineEdit("%1说：%2");
    danmuTemplateInput->setPlaceholderText("%1=昵称 %2=内容");
    tplLayout->addWidget(danmuTemplateInput, 0, 1);
    tplLayout->addWidget(new QLabel("礼物模板："), 1, 0);
    giftTemplateInput = new QLineEdit("感谢%1送出的%2%3个");
    giftTemplateInput->setPlaceholderText("%1=昵称 %2=礼物名 %3=数量");
    tplLayout->addWidget(giftTemplateInput, 1, 1);
    layout->addWidget(tplGroup);

    // 窗口设置
    QGroupBox* winGroup = new QGroupBox("窗口设置");
    QGridLayout* winLayout = new QGridLayout(winGroup);
    alwaysOnTopCheck = new QCheckBox("窗口置顶");
    winLayout->addWidget(alwaysOnTopCheck, 0, 0);
    winLayout->addWidget(new QLabel("透明度："), 0, 1);
    opacitySlider = new QSlider(Qt::Horizontal);
    opacitySlider->setRange(30, 100);
    opacitySlider->setValue(100);
    opacityLabel = new QLabel("100%");
    opacityLabel->setFixedWidth(40);
    winLayout->addWidget(opacitySlider, 0, 2);
    winLayout->addWidget(opacityLabel, 0, 3);
    layout->addWidget(winGroup);

    // ========== 弹幕列表 ==========
    QGroupBox* listGroup = new QGroupBox("弹幕");
    QVBoxLayout* listLayout = new QVBoxLayout(listGroup);
    danmuList = new QListWidget();
    danmuList->setMaximumHeight(200);
    listLayout->addWidget(danmuList);
    layout->addWidget(listGroup);

    // ========== 日志 ==========
    QGroupBox* logGroup = new QGroupBox("日志");
    QVBoxLayout* logLayout = new QVBoxLayout(logGroup);
    logView = new QTextEdit();
    logView->setReadOnly(true);
    logView->setMaximumHeight(80);
    logLayout->addWidget(logView);
    layout->addWidget(logGroup);

    // ========== 底部版本信息 ==========
    QLabel* footer = new QLabel(
        "本程序仅供个人自用，用于主播本人监听自己直播间弹幕，请遵守相关法律法规，违者后果自负\n\n"
        "版本：v0.4.0\n"
        "开发者：Byjsmc\n"
        "最后更新于：2026/09/05"
        );
    footer->setStyleSheet("color: gray; font-size: 11px;");
    footer->setWordWrap(true);
    layout->addWidget(footer);

    // ========== 信号连接 ==========
    connect(connectBtn, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(disconnectBtn, &QPushButton::clicked, this, &MainWindow::onDisconnectClicked);
    connect(webSocket, &QWebSocket::connected, this, &MainWindow::onWebSocketConnected);
    connect(webSocket, &QWebSocket::disconnected, this, &MainWindow::onWebSocketDisconnected);
    connect(webSocket, &QWebSocket::textMessageReceived, this, &MainWindow::onTextMessageReceived);
    connect(webSocket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error), this, &MainWindow::onWebSocketError);
    connect(tts, &QTextToSpeech::stateChanged, this, &MainWindow::onStateChanged);

    // 语速滑块
    connect(rateSlider, &QSlider::valueChanged, this, [this](int v) {
        rateLabel->setText(QString::number(v));
        tts->setRate(v / 100.0);
    });
    // 音量滑块
    connect(volumeSlider, &QSlider::valueChanged, this, [this](int v) {
        volumeLabel->setText(QString::number(v));
        tts->setVolume(v / 100.0);
    });
    // 音色选择
    connect(voiceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (idx >= 0 && idx < voices.size()) tts->setVoice(voices[idx]);
    });
    // 云端 TTS
    connect(engineCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onEngineChanged);
    connect(qwenVoiceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (idx >= 0) qwenTts->setVoice(qwenVoiceCombo->itemData(idx).toString());
    });
    connect(apiKeyInput, &QLineEdit::editingFinished, this, [this]() {
        qwenTts->setApiKey(apiKeyInput->text());
    });
    // 随机朗读
    randomTimer->start();
    // 窗口设置
    connect(alwaysOnTopCheck, &QCheckBox::toggled, this, &MainWindow::onAlwaysOnTopToggled);
    connect(opacitySlider, &QSlider::valueChanged, this, &MainWindow::onOpacityChanged);

    // 防刷屏定时器
    connect(ttsTimer, &QTimer::timeout, this, &MainWindow::onTtsTimerTimeout);
    ttsTimer->start();
    // 重连定时器
    connect(reconnectTimer, &QTimer::timeout, this, &MainWindow::onReconnectTimerTimeout);
    // 网关守护定时器
    connect(gatewayWatchdogTimer, &QTimer::timeout, this, &MainWindow::onGatewayWatchdogTimeout);
    gatewayWatchdogTimer->start();

    // 加载配置
    loadConfig();
    setWindowFlag(Qt::WindowStaysOnTopHint, alwaysOnTopCheck->isChecked());

    // 模板字符串
    danmuTpl = danmuTemplateInput->text();
    giftTpl = giftTemplateInput->text();
    connect(danmuTemplateInput, &QLineEdit::editingFinished, this, [this]() {
        danmuTpl = danmuTemplateInput->text();
    });
    connect(giftTemplateInput, &QLineEdit::editingFinished, this, [this]() {
        giftTpl = giftTemplateInput->text();
    });

    log("程序已启动，请先启动弹幕网关再点击连接");
    show();
}

MainWindow::~MainWindow() {
    saveConfig();
    reconnectTimer->stop();
    ttsTimer->stop();
    randomTimer->stop();
    gatewayWatchdogTimer->stop();
    if (webSocket->state() == QAbstractSocket::ConnectedState) webSocket->close();
    qwenTts->stopAll();
    stopGateway();
}

// ==================== 连接控制 ====================

void MainWindow::onConnectClicked() {
    QString url = urlInput->text().trimmed();
    if (url.isEmpty()) { QMessageBox::warning(this, "错误", "请输入弹幕网关地址"); return; }

    if (!ensureGatewayRunning()) {
        log("网关未运行，正在尝试启动...");
        startGateway();

        QString gatewayDir = QCoreApplication::applicationDirPath() + "/gateway";
        QDir dir(gatewayDir);
        if (dir.entryList(QStringList() << "*.exe", QDir::Files).isEmpty()) {
            log("❌ gateway 文件夹下未找到任何 exe，请确认已部署网关");
            return;
        }

        log("等待网关初始化（约 3 秒）...");
        waitingForGateway = true;
        // 异步等待，不阻塞 UI
        QTimer::singleShot(3000, this, [this, url]() {
            waitingForGateway = false;
            log(QString("正在连接 %1...").arg(url));
            webSocket->open(QUrl(url));
        });
        return;
    }

    log(QString("正在连接 %1...").arg(url));
    webSocket->open(QUrl(url));
}

void MainWindow::onDisconnectClicked() {
    manualDisconnect = true;
    reconnectTimer->stop();
    webSocket->close();
    log("已断开连接");
}

void MainWindow::onWebSocketConnected() {
    connected = true;
    reconnectAttempts = 0;
    reconnectTimer->stop();
    connectBtn->setEnabled(false);
    disconnectBtn->setEnabled(true);
    log("✅ 已连接到弹幕服务");
    enqueueSpeak("已连接到弹幕服务");
}

void MainWindow::onWebSocketDisconnected() {
    connected = false;
    connectBtn->setEnabled(true);
    disconnectBtn->setEnabled(false);

    if (manualDisconnect) {
        manualDisconnect = false;
        log("已手动断开，不自动重连");
        return;
    }

    log("❌ 连接已断开");
    enqueueSpeak("连接已断开");
    reconnectAttempts = 0;
    reconnectTimer->start();
}

void MainWindow::onWebSocketError(QAbstractSocket::SocketError error) {
    Q_UNUSED(error);
    QString errMsg = QString("连接错误: %1 ，请检查网关是否正常运行以及程序是否为管理员身份运行").arg(webSocket->errorString());
    log(errMsg);
    enqueueSpeak("连接错误，请检查网关是否正常运行以及程序是否为管理员身份运行");
}

// ==================== 消息处理 ====================

void MainWindow::onTextMessageReceived(const QString& message) {
    parseDanmu(message);
}

void MainWindow::onStateChanged(QTextToSpeech::State state) {
    Q_UNUSED(state);
}

/**
 * @brief 解析弹幕网关推送的 JSON 消息
 * Data 字段是嵌套的 JSON 字符串，需要二次解析
 * 内层消息公共字段（DouyinBarrageGrab Msg 基类）：
 *   WebRoomId(web直播间ID，稳定) / RoomId(场次号，每次开播变化)
 *   RoomTitle / Owner{Nickname,SecUid,...}
 */
void MainWindow::parseDanmu(const QString& json) {
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (doc.isNull() || !doc.isObject()) return;
    QJsonObject root = doc.object();

    int type = root.value("Type").toInt();

    // Data 是 JSON 字符串，需要再解析一层
    QJsonObject data;
    QJsonValue dataVal = root.value("Data");
    if (dataVal.isString()) {
        QJsonDocument dataDoc = QJsonDocument::fromJson(dataVal.toString().toUtf8());
        if (dataDoc.isObject()) data = dataDoc.object();
    } else if (dataVal.isObject()) {
        data = dataVal.toObject();
    }

    // ========== 房间识别：新房间加入下拉框 ==========
    QString webRoomId = data.value("WebRoomId").toString();
    if (webRoomId.isEmpty()) webRoomId = data.value("WebRoomid").toString();
    if (!webRoomId.isEmpty()) {
        QString label;
        QJsonObject owner = data.value("Owner").toObject();
        if (owner.isEmpty()) owner = data.value("Onwer").toObject();   // 网关历史拼写
        label = owner.value("Nickname").toString();
        if (label.isEmpty()) label = data.value("RoomTitle").toString();
        if (label.isEmpty()) label = webRoomId;
        if (!roomNames.contains(webRoomId) || roomNames.value(webRoomId).isEmpty()) {
            roomNames.insert(webRoomId, label);
            log(QString("发现直播间: %1 (%2)").arg(label, webRoomId));
            refreshRoomCombo(selectedRoomId());
        }
    }

    // ========== 房间过滤 ==========
    const QString selected = selectedRoomId();
    if (!selected.isEmpty() && webRoomId != selected) return;

    // 来源标注：发现了多个房间时才加前缀，避免单房间噪音
    const QString sourcePrefix =
        (roomNames.size() > 1 && !webRoomId.isEmpty())
            ? QString("[%1] ").arg(roomNames.value(webRoomId, webRoomId))
            : QString();

    // 提取用户昵称
    auto getNick = [&](const QJsonObject& d) -> QString {
        QJsonObject u = d.value("User").toObject();
        QString nick = u.value("Nickname").toString();
        if (nick.isEmpty()) nick = u.value("DisplayName").toString();
        if (nick.isEmpty()) nick = u.value("DisplayId").toString();
        return nick.isEmpty() ? "某人" : nick;
    };

    if (type == 1) {  // 弹幕
        QString nickname = getNick(data);
        QString content = data.value("Content").toString().trimmed();
        if (content.isEmpty()) return;

        danmuList->addItem(sourcePrefix + QString("%1: %2").arg(nickname).arg(content));
        if (danmuList->count() > 80) danmuList->takeItem(0);
        danmuList->scrollToBottom();

        if (danmuSwitch->isChecked()) {
            QString text = danmuTpl;
            text.replace("%1", nickname);
            text.replace("%2", content);
            enqueueSpeak(text);
        }

    } else if (type == 5) {  // 礼物
        QString nickname = getNick(data);
        QString gift = data.value("GiftName").toString();
        if (gift.isEmpty()) gift = "礼物";
        int count = data.value("GiftCount").toInt();
        if (count == 0) count = data.value("RepeatCount").toInt();
        if (count == 0) count = 1;

        QString text = giftTpl;
        text.replace("%1", nickname);
        text.replace("%2", gift);
        text.replace("%3", QString::number(count));
        danmuList->addItem(sourcePrefix + QString("[礼物] %1").arg(text));

        if (giftSwitch->isChecked()) {
            enqueueSpeak(text);
        }

    } else if (type == 3) {  // 进入直播间
        QString nickname = getNick(data);
        if (nickname.isEmpty() || nickname == "某人") return;

        danmuList->addItem(sourcePrefix + QString("[进房] %1 进入了直播间").arg(nickname));
        if (danmuList->count() > 80) danmuList->takeItem(0);
        danmuList->scrollToBottom();

        if (enterSwitch->isChecked()) {
            enqueueSpeak(QString("%1 进入了直播间").arg(nickname));
        }
    }
}

void MainWindow::speak(const QString& text) {
    tts->say(text);
}

// ==================== 引擎与房间辅助 ====================

bool MainWindow::engineIsQwen() const {
    return engineCombo && engineCombo->currentData().toString() == "qwen";
}

void MainWindow::updateEngineUi() {
    const bool qwen = engineIsQwen();
    voiceCombo->setVisible(!qwen);
    systemVoiceLabel->setVisible(!qwen);
    qwenVoiceCombo->setVisible(qwen);
    qwenVoiceLabel->setVisible(qwen);
    apiKeyInput->setVisible(qwen);
    qwenKeyLabel->setVisible(qwen);
}

void MainWindow::onEngineChanged(int index) {
    Q_UNUSED(index);
    if (engineIsQwen()) {
        if (tts->state() == QTextToSpeech::Speaking) tts->stop();
        qwenTts->setApiKey(apiKeyInput->text());
        if (qwenVoiceCombo->currentIndex() >= 0)
            qwenTts->setVoice(qwenVoiceCombo->itemData(qwenVoiceCombo->currentIndex()).toString());
        if (apiKeyInput->text().trimmed().isEmpty())
            log("⚠️ 使用千问 TTS 需要先填写阿里云百炼 DashScope API Key");
    } else {
        qwenTts->stopAll();
    }
    updateEngineUi();
}

QString MainWindow::selectedRoomId() const {
    if (!roomCombo || roomCombo->currentIndex() < 0) return QString();
    return roomCombo->currentData().toString();
}

QString MainWindow::roomLabelFor(const QString& webRoomId) const {
    return roomNames.value(webRoomId, webRoomId);
}

void MainWindow::refreshRoomCombo(const QString& keepId) {
    QString want = keepId;
    if (want.isEmpty()) want = selectedRoomId();

    const QSignalBlocker blocker(roomCombo);
    roomCombo->clear();
    roomCombo->addItem("全部房间", QString());

    // 按名称排序展示
    QList<QPair<QString, QString>> rooms;   // label, id
    for (auto it = roomNames.constBegin(); it != roomNames.constEnd(); ++it)
        rooms.append({it.value(), it.key()});
    std::sort(rooms.begin(), rooms.end(), [](const QPair<QString, QString>& a,
                                             const QPair<QString, QString>& b) {
        return a.first < b.first;
    });
    for (const auto& r : rooms)
        roomCombo->addItem(r.first, r.second);

    if (!want.isEmpty() && roomCombo->findData(want) < 0)
        roomCombo->addItem(want, want);   // 已保存但尚未发现的房间先按 ID 占位

    int idx = roomCombo->findData(want);
    roomCombo->setCurrentIndex(idx >= 0 ? idx : 0);
}

void MainWindow::log(const QString& msg) {
    QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
    logView->append(QString("[%1] %2").arg(ts).arg(msg));
    logView->verticalScrollBar()->setValue(logView->verticalScrollBar()->maximum());
}
