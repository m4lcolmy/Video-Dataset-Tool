#include "mainwindow.h"
#include "./ui_mainwindow.h"

#include <QFileDialog>
#include <QStandardPaths>
#include <QDateTime>
#include <QMessageBox>
#include <QKeyEvent>
#include <QTextStream>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    ui->playPauseBtn->setFocus();

    ui->nextImageLabel->setStyleSheet(
        "QLabel {"
        "  background: #4287f5;"
        "  color: white;"
        "  border-radius: 6px;"
        "  padding: 2px 6px;"
        "}"
        );

    // Remember original style of the "Next image" label & prepare its timer
    nextLabelOrigStyle_ = ui->nextImageLabel->styleSheet();
    flashTimer_.setSingleShot(true);
    connect(&flashTimer_, &QTimer::timeout, this, [this]() {
        ui->nextImageLabel->setStyleSheet(nextLabelOrigStyle_);
    });

    // Build a centered overlay for play/pause glyph
    overlayIcon_ = new QLabel(ui->videoLabel);
    overlayIcon_->setAttribute(Qt::WA_TransparentForMouseEvents);
    overlayIcon_->setAlignment(Qt::AlignCenter);
    overlayIcon_->setText(""); // start hidden

    overlayEffect_ = new QGraphicsOpacityEffect(overlayIcon_);
    overlayIcon_->setGraphicsEffect(overlayEffect_);
    overlayEffect_->setOpacity(0.0);

    overlayFade_ = new QPropertyAnimation(overlayEffect_, "opacity", this);
    overlayFade_->setDuration(600);
    overlayFade_->setStartValue(0.0);
    overlayFade_->setEndValue(1.0);

    // styling for the overlay (big, soft, centered)
    overlayIcon_->setStyleSheet(
        "QLabel {"
        "  color: white;"
        "  font: 700 72px 'Segoe UI', 'Ubuntu', sans-serif;"
        "  text-align: center;"
        "  background: transparent;"   // transparent background
        "}"
        );
    overlayIcon_->hide();

    // Catch keys app-wide and mouse on the video label
    qApp->installEventFilter(this);
    ui->videoLabel->installEventFilter(this);
    ui->videoLabel->setContextMenuPolicy(Qt::NoContextMenu); // optional: no default menu
    // ui->videoLabel->setToolTip("Left click: Play/Pause • Right click: Save frame");
    // Let the video area expand with the window
    ui->videoLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui->videoLabel->setMinimumSize(1, 1);
    ui->videoLabel->setScaledContents(false); // we scale manually in displayMat()

    // kill extra margins around the video
    if (ui->videoGroupLayout) ui->videoGroupLayout->setContentsMargins(0,0,0,0);
    if (ui->verticalLayout_5) ui->verticalLayout_5->setContentsMargins(0,0,0,0);

    // Install event filter if you later want to catch more keys
    this->installEventFilter(this);

    // Where we keep our tiny "database"
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(appData);
    configPath_ = appData + QDir::separator() + "config.txt";

    loadConfig();
    recalcNextImageFromDir();
    updateInfoLabels();

    // Connect timer for playback
    connect(&timer_, &QTimer::timeout, this, &MainWindow::tick);

    // Keyboard shortcut: press 'S' to save current frame
    saveShortcut_ = new QShortcut(QKeySequence(Qt::Key_S), this);
    connect(saveShortcut_, &QShortcut::activated, this, &MainWindow::saveCurrentFrame);

    // Slider behavior
    connect(ui->timeSlider, &QSlider::sliderPressed,  this, &MainWindow::on_timeSlider_sliderPressed);
    connect(ui->timeSlider, &QSlider::sliderReleased, this, &MainWindow::on_timeSlider_sliderReleased);

    // Reflect paths in labels if available
    if (!lastVideoPath_.isEmpty())
        ui->videoPathLabel->setText(lastVideoPath_);
    if (!saveDirPath_.isEmpty())
        ui->saveDirLabel->setText(saveDirPath_);

    // If last video exists, open it (but don't auto-play)
    if (!lastVideoPath_.isEmpty() && QFile::exists(lastVideoPath_))
        openVideo(lastVideoPath_);
}

MainWindow::~MainWindow()
{
    saveConfig();
    delete ui;
}

// ================== UI Slots ==================

void MainWindow::on_selectVideoBtn_clicked()
{
    QString path = QFileDialog::getOpenFileName(this, "Select Video",
                                                lastVideoPath_.isEmpty() ? QDir::homePath() : QFileInfo(lastVideoPath_).absolutePath(),
                                                "Videos (*.mp4 *.avi *.mkv *.mov *.m4v *.webm);;All Files (*)");
    if (path.isEmpty()) return;

    openVideo(path);
    lastVideoPath_ = path;
    ui->videoPathLabel->setText(path);
    saveConfig();
}

void MainWindow::on_selectDirBtn_clicked()
{
    QString dir = QFileDialog::getExistingDirectory(this, "Select Save Directory",
                                                    saveDirPath_.isEmpty() ? QDir::homePath() : saveDirPath_);
    if (dir.isEmpty()) return;

    saveDirPath_ = dir;
    ui->saveDirLabel->setText(dir);

    recalcNextImageFromDir();
    updateInfoLabels();
    saveConfig();
}

void MainWindow::on_playPauseBtn_clicked()
{
    if (!cap_.isOpened()) return;
    setPlaying(!playing_);
}

void MainWindow::on_reloadVideoBtn_clicked()
{
    if (!cap_.isOpened()) return;
    // Restart from the beginning and start playing
    setPlaying(false);
    seekTo(0);
    setPlaying(true);
}

void MainWindow::on_preVideoBtn_clicked()
{
    if (!cap_.isOpened()) return;
    setPlaying(false);
    stepRelative(-1);
}

void MainWindow::on_nextVideoBtn_clicked()
{
    if (!cap_.isOpened()) return;
    setPlaying(false);
    stepRelative(+1);
}

void MainWindow::on_timeSlider_sliderMoved(int value)
{
    if (!cap_.isOpened()) return;
    seekTo(value);
}

void MainWindow::on_timeSlider_sliderPressed()
{
    sliderHeld_ = true;
    if (playing_) setPlaying(false);
}

void MainWindow::on_timeSlider_sliderReleased()
{
    if (!cap_.isOpened()) { sliderHeld_ = false; return; }
    // Finalize position at the released value (cheap, but ensures sync)
    int target = ui->timeSlider->value();
    seekTo(target);
    sliderHeld_ = false;
}

// ================== Playback Loop ==================

void MainWindow::tick()
{
    if (!cap_.isOpened()) return;

    cv::Mat frame;
    if (!cap_.read(frame))
    {
        // End of video => stop
        setPlaying(false);
        return;
    }

    currentFrameIndex_ = static_cast<int>(cap_.get(cv::CAP_PROP_POS_FRAMES)) - 1;
    currentFrameBGR_ = frame.clone();

    displayMat(currentFrameBGR_);
    if (!sliderHeld_)
        ui->timeSlider->setValue(currentFrameIndex_);
    updateInfoLabels();
}

// ================== Helpers ==================

void MainWindow::flashNextImageLabel()
{
    // Green chip style for 2 seconds
    ui->nextImageLabel->setStyleSheet(
        "QLabel {"
        "  background: #2ecc71;"
        "  color: white;"
        "  border-radius: 6px;"
        "  padding: 2px 6px;"
        "}"
        );
    flashTimer_.start(300);
}

void MainWindow::showOverlayGlyph(const QString &glyph)
{
    if (!overlayIcon_) return;

    overlayIcon_->setText(glyph);
    overlayIcon_->adjustSize();

    // Center in videoLabel
    QSize s = overlayIcon_->size();
    QSize parentSize = ui->videoLabel->size();
    int x = (parentSize.width()  - s.width())  / 2;
    int y = (parentSize.height() - s.height()) / 2;
    overlayIcon_->move(x, y);

    overlayIcon_->show();

    overlayFade_->stop();
    overlayFade_->setDuration(120);
    overlayFade_->setStartValue(0.0);
    overlayFade_->setEndValue(1.0);
    overlayFade_->start();

    QTimer::singleShot(450, this, [this]() {
        overlayFade_->stop();
        overlayFade_->setDuration(350);
        overlayFade_->setStartValue(1.0);
        overlayFade_->setEndValue(0.0);
        overlayFade_->start();
        connect(overlayFade_, &QPropertyAnimation::finished, overlayIcon_, [this]() {
            if (overlayEffect_->opacity() == 0.0) overlayIcon_->hide();
        });
    });
}

void MainWindow::togglePlayPause()
{
    if (!cap_.isOpened()) return;   // no video loaded → ignore
    setPlaying(!playing_);
}

void MainWindow::openVideo(const QString &path)
{
    if (cap_.isOpened()) cap_.release();

    cap_.open(path.toStdString());
    if (!cap_.isOpened())
    {
        QMessageBox::warning(this, "Error", "Failed to open video.");
        return;
    }

    fps_ = cap_.get(cv::CAP_PROP_FPS);
    if (fps_ <= 0.0) fps_ = 30.0;

    frameCount_ = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_COUNT));
    currentFrameIndex_ = 0;

    ensureSliderRange();
    updateTimerFromFPS();

    // Show first frame
    seekTo(0);
    // setPlaying(false);
}

void MainWindow::updateTimerFromFPS()
{
    int intervalMs = static_cast<int>(1000.0 / std::max(1.0, fps_));
    timer_.setInterval(intervalMs);
}

void MainWindow::ensureSliderRange()
{
    ui->timeSlider->setMinimum(0);
    ui->timeSlider->setMaximum(std::max(0, frameCount_ - 1));
    ui->timeSlider->setSingleStep(1);
    ui->timeSlider->setPageStep(std::max(1, frameCount_ / 20));
}

void MainWindow::seekTo(int frameIndex)
{
    if (!cap_.isOpened()) return;

    frameIndex = std::clamp(frameIndex, 0, std::max(0, frameCount_ - 1));
    cap_.set(cv::CAP_PROP_POS_FRAMES, frameIndex);

    cv::Mat frame;
    if (cap_.read(frame))
    {
        currentFrameIndex_ = static_cast<int>(cap_.get(cv::CAP_PROP_POS_FRAMES)) - 1;
        currentFrameBGR_ = frame.clone();
        displayMat(currentFrameBGR_);

        // IMPORTANT: don't fight the user while scrubbing
        if (!sliderHeld_)
            ui->timeSlider->setValue(currentFrameIndex_);
    }
    updateInfoLabels();
}

void MainWindow::stepRelative(int deltaFrames)
{
    int target = currentFrameIndex_ + deltaFrames;
    seekTo(target);
}

void MainWindow::setPlaying(bool on)
{
    playing_ = on;
    if (playing_) timer_.start();
    else          timer_.stop();

    ui->playPauseBtn->setToolTip(playing_ ? "Pause" : "Play");

    // NEW: visual feedback overlay
    showOverlayGlyph(playing_ ? "▶" : "⏸");
}

void MainWindow::displayMat(const cv::Mat &bgr)
{
    if (bgr.empty()) return;
    QImage img = matToQImage(bgr);
    // Keep aspect fit inside the QLabel
    QPixmap pix = QPixmap::fromImage(img).scaled(ui->videoLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    ui->videoLabel->setPixmap(pix);
}

QImage MainWindow::matToQImage(const cv::Mat &bgr)
{
    cv::Mat rgb;
    if (bgr.channels() == 3)
        cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    else if (bgr.channels() == 4)
        cv::cvtColor(bgr, rgb, cv::COLOR_BGRA2RGBA);
    else
        cv::cvtColor(bgr, rgb, cv::COLOR_GRAY2RGB);

    return QImage(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step), QImage::Format_RGB888).copy();
}

void MainWindow::updateInfoLabels()
{
    ui->frameInfoLabel->setText(QString("Frame: %1 / %2").arg(currentFrameIndex_).arg(frameCount_));
    ui->nextImageLabel->setText(QString("Next image: %1").arg(nextImageIndex_));
}

void MainWindow::saveCurrentFrame()
{
    if (currentFrameBGR_.empty())
        return;

    if (saveDirPath_.isEmpty())
    {
        QMessageBox::information(this, "Save directory required", "Please select a save directory first.");
        return;
    }

    QDir dir(saveDirPath_);
    if (!dir.exists())
        dir.mkpath(".");

    // Ensure numbering continues from largest numeric filename
    recalcNextImageFromDir();

    // Format: image_XXXX.png (zero-padded to 4 digits)
    QString filename = QString("image_%1.png")
                           .arg(nextImageIndex_, 4, 10, QLatin1Char('0'));

    QString fullPath = dir.filePath(filename);

    // Save as PNG
    bool ok = cv::imwrite(fullPath.toStdString(), currentFrameBGR_);
    if (!ok)
    {
        QMessageBox::warning(this, "Save failed", "Could not save image.");
        return;
    }

    ++nextImageIndex_;
    updateInfoLabels();
    saveConfig();

    flashNextImageLabel();
    statusBar()->showMessage(QString("Saved: %1").arg(filename), 3000);  // shows for 4 seconds
}

void MainWindow::recalcNextImageFromDir()
{
    if (saveDirPath_.isEmpty())
    {
        nextImageIndex_ = 1;
        return;
    }
    nextImageIndex_ = extractLargestNumberInDir(saveDirPath_) + 1;
}

int MainWindow::extractLargestNumberInDir(const QString &dirPath)
{
    QDir dir(dirPath);
    if (!dir.exists()) return 0;

    QStringList filters;
    filters << "*.jpg" << "*.jpeg" << "*.png" << "*.bmp";
    QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks | QDir::Readable);

    int maxNum = 0;
    QRegularExpression re("(\\d+)");
    for (const QFileInfo &fi : list)
    {
        const QString base = fi.completeBaseName(); // without extension
        QRegularExpressionMatchIterator it = re.globalMatch(base);
        while (it.hasNext())
        {
            QRegularExpressionMatch m = it.next();
            bool ok = false;
            int num = m.captured(1).toInt(&ok);
            if (ok) maxNum = std::max(maxNum, num);
        }
    }
    return maxNum;
}

// ================== Config TXT ==================

void MainWindow::loadConfig()
{
    QFile f(configPath_);
    if (!f.exists()) return;
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;

    QTextStream in(&f);
    while (!in.atEnd())
    {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const int eq = line.indexOf('=');
        if (eq <= 0) continue;

        const QString key = line.left(eq).trimmed();
        const QString val = line.mid(eq + 1).trimmed();

        if (key == "last_video") lastVideoPath_ = val;
        else if (key == "save_dir") saveDirPath_ = val;
        else if (key == "next_image") nextImageIndex_ = val.toInt();
    }
    f.close();
}

void MainWindow::saveConfig() const
{
    QFile f(configPath_);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;

    QTextStream out(&f);
    out << "# Simple config for Video Dataset Preparation Tool\n";
    out << "last_video=" << lastVideoPath_ << "\n";
    out << "save_dir="   << saveDirPath_   << "\n";
    out << "next_image=" << nextImageIndex_ << "\n";
    f.close();
}

// ================== Events ==================

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    // Handle mouse click on the video label to toggle play/pause
    if (obj == ui->videoLabel && event->type() == QEvent::MouseButtonPress)
    {
        auto *me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            togglePlayPause();
            return true; // consume
        }

        if (me->button() == Qt::RightButton) {
            saveCurrentFrame();      // <-- capture frame on right-click
            return true;             // consume
        }
    }

    // Handle keys globally (installed on qApp)
    if (event->type() == QEvent::KeyPress)
    {
        auto *ke = static_cast<QKeyEvent*>(event);

        // SPACE => play/pause, and consume so it doesn't click focused buttons
        if (ke->key() == Qt::Key_Space) {
            togglePlayPause();
            return true; // consume
        }

        // 'S' => save current frame (consume to avoid button triggering)
        if (ke->key() == Qt::Key_S) {
            saveCurrentFrame();
            return true; // consume
        }

        // NEW: Arrow keys step one frame
        if (ke->key() == Qt::Key_Left) {
            if (cap_.isOpened()) {
                setPlaying(false);      // ensure paused
                stepRelative(-1);       // go back one frame
            }
            return true;
        }
        if (ke->key() == Qt::Key_Right) {
            if (cap_.isOpened()) {
                setPlaying(false);      // ensure paused
                stepRelative(+1);       // forward one frame
            }
            return true;
        }
    }

    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::resizeEvent(QResizeEvent *e)
{
    QMainWindow::resizeEvent(e);

    if (overlayIcon_) {
        // Let the label compute its preferred size for the current text & style
        overlayIcon_->adjustSize();

        QSize s = overlayIcon_->size();
        QSize parentSize = ui->videoLabel->size();

        int x = (parentSize.width()  - s.width())  / 2;
        int y = (parentSize.height() - s.height()) / 2;

        overlayIcon_->move(x, y);
    }

    //rescale the currently shown frame to fit the new size
    if (!currentFrameBGR_.empty())
        displayMat(currentFrameBGR_);
}


