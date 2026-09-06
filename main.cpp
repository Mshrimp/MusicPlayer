#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QTableView>
#include <QStandardItemModel>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>
#include <QSet>
#include <QBrush>
#include <QColor>
#include <QSlider>
#include <QScrollBar>
#include <QStyleOptionSlider>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QShortcut>
#include <QKeyEvent>
#include <QSettings>
#include <QStatusBar>
#include <QMenu>
#include <QComboBox>
#include <QRandomGenerator>
#include <QItemSelectionModel>
#include <QFile>
#include <QStringDecoder>
#include <QMediaPlayer>

#include <iconv.h>
#include <vector>
#include <QAudioOutput>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QUrl>
#include <QImage>
#include <QPixmap>

#include <algorithm>
#include <functional>
#include <utility>

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/mp4coverart.h>

// cover_mac.mm 提供：Qt 图片插件缺失时用 macOS ImageIO 兜底解码（如 webp）
QImage imageFromMac(const QByteArray &data);

namespace {

const QStringList kAudioSuffixes = {
    QStringLiteral("*.mp3"), QStringLiteral("*.flac"), QStringLiteral("*.m4a"),
    QStringLiteral("*.wav"), QStringLiteral("*.ogg"), QStringLiteral("*.aac"),
};
const QString kFileDialogFilter =
    QStringLiteral("音频文件 (*.mp3 *.flac *.m4a *.wav *.ogg *.aac)");

// 毫秒 → "分:秒"
QString formatTime(qint64 ms) {
    const qint64 totalSecs = ms / 1000;
    return QStringLiteral("%1:%2")
        .arg(totalSecs / 60)
        .arg(totalSecs % 60, 2, 10, QLatin1Char('0'));
}

// ---------- LRC 歌词 ----------

struct LyricLine {
    qint64 timeMs = 0;
    QString text;
};

// 解析 LRC 文本：支持一行多时间标签、[offset:±ms] 偏移；[ti:] 等元数据标签忽略
bool parseLrc(const QString &content, QList<LyricLine> &out) {
    qint64 offset = 0;
    for (const QString &raw : content.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.at(0) != QLatin1Char('['))
            continue;
        QList<qint64> times;
        int pos = 0;
        while (pos < line.size() && line.at(pos) == QLatin1Char('[')) {
            const int close = line.indexOf(QLatin1Char(']'), pos);
            if (close < 0)
                break;
            const QString tag = line.mid(pos + 1, close - pos - 1);
            if (tag.startsWith(QLatin1String("offset:"))) {
                offset = tag.mid(7).trimmed().toLongLong(); // 整体时间偏移
            } else {
                const int colon = tag.indexOf(QLatin1Char(':'));
                if (colon > 0) {
                    bool okMin = false, okSec = false;
                    const int minutes = tag.left(colon).toInt(&okMin);
                    const double secs = tag.mid(colon + 1).toDouble(&okSec);
                    if (okMin && okSec)
                        times << qint64((minutes * 60.0 + secs) * 1000);
                }
            }
            pos = close + 1;
        }
        const QString text = line.mid(pos).trimmed();
        for (qint64 t : times)
            out.append({t + offset, text});
    }
    std::sort(out.begin(), out.end(),
              [](const LyricLine &a, const LyricLine &b) { return a.timeMs < b.timeMs; });
    return !out.isEmpty();
}

// 用系统 iconv 把 GBK/GB18030 字节流转成 UTF-8（Qt 6.5 无 GBK 解码支持）
QString decodeGbk(const QByteArray &data) {
    iconv_t cd = iconv_open("UTF-8", "GB18030");
    if (cd == reinterpret_cast<iconv_t>(-1))
        return QString();
    const char *inBuf = data.constData();
    size_t inLeft = size_t(data.size());
    std::vector<char> outBuf(size_t(data.size()) * 2 + 16); // GBK→UTF-8 最长约 2 倍
    char *outPtr = outBuf.data();
    size_t outLeft = outBuf.size();
    const size_t ret =
        iconv(cd, const_cast<char **>(&inBuf), &inLeft, &outPtr, &outLeft);
    iconv_close(cd);
    if (ret == size_t(-1))
        return QString();
    return QString::fromUtf8(outBuf.data(), int(outPtr - outBuf.data()));
}

// 读取 LRC 文件：先按 UTF-8 解码，失败按 GB18030（覆盖 GBK）解码——中文歌词常见编码
bool loadLrcFile(const QString &path, QList<LyricLine> &out) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = file.readAll();

    QString content;
    QStringDecoder utf8(QStringDecoder::Utf8);
    content = utf8.decode(data);
    if (utf8.hasError())
        content = decodeGbk(data);
    return parseLrc(content, out);
}

// 自然排序比较器：数字段按数值比较（1 < 2 < 10），其余字符忽略大小写按码点比较。
// 与 locale 无关（保证 ASCII 在前、中文在后，行为确定）。
// 按完整路径排序时，同一文件夹的文件自然连续，即"按文件夹分组"。
bool pathLessThan(const QString &a, const QString &b) {
    int i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const QChar ca = a.at(i), cb = b.at(j);
        if (ca.isDigit() && cb.isDigit()) {
            // 数字段：跳前导零后先比位数、再比数值
            int si = i, sj = j;
            while (i < a.size() && a.at(i).isDigit())
                ++i;
            while (j < b.size() && b.at(j).isDigit())
                ++j;
            while (si < i - 1 && a.at(si) == QLatin1Char('0'))
                ++si;
            while (sj < j - 1 && b.at(sj) == QLatin1Char('0'))
                ++sj;
            const int li = i - si, lj = j - sj;
            if (li != lj)
                return li < lj;
            const int cmp = QStringView(a).mid(si, li).compare(QStringView(b).mid(sj, lj));
            if (cmp != 0)
                return cmp < 0;
        } else {
            const QChar la = ca.toLower(), lb = cb.toLower();
            if (la != lb)
                return la < lb;
            ++i;
            ++j;
        }
    }
    return a.size() - i < b.size() - j; // 前缀相同则短者在前
}

// ---------- 封面 ----------

// 提取内嵌封面：MP3 ID3v2 APIC / FLAC Picture / MP4 cover；失败返回空
QImage embeddedCover(const TagLib::FileRef &ref) {
    const auto toImage = [](const TagLib::ByteVector &data) {
        const QByteArray bytes(data.data(), int(data.size()));
        QImage img = QImage::fromData(bytes);
        if (img.isNull())
            img = imageFromMac(bytes); // Qt 官方包无 webp 插件时走 ImageIO 兜底
        return img;
    };
    if (auto *f = dynamic_cast<TagLib::MPEG::File *>(ref.file())) {
        if (f->ID3v2Tag()) {
            const auto frames = f->ID3v2Tag()->frameList("APIC");
            for (const auto *frame : frames) {
                const auto *pic =
                    dynamic_cast<const TagLib::ID3v2::AttachedPictureFrame *>(frame);
                if (pic)
                    return toImage(pic->picture());
            }
        }
    } else if (auto *f = dynamic_cast<TagLib::FLAC::File *>(ref.file())) {
        for (const auto *pic : f->pictureList())
            if (pic)
                return toImage(pic->data());
    } else if (auto *f = dynamic_cast<TagLib::MP4::File *>(ref.file())) {
        if (f->tag()) {
            // TagLib 2 中封面存于 covr 原子，从 ItemMap 取 CoverArtList
            const TagLib::MP4::ItemMap map = f->tag()->itemMap();
            if (map.contains("covr")) {
                const auto covers = map["covr"].toCoverArtList();
                if (!covers.isEmpty())
                    return toImage(covers.front().data());
            }
        }
    }
    return QImage();
}

// 同目录常见命名的封面文件：同名词 / cover / folder / front / album，jpg/png/webp
QImage folderCover(const QString &audioPath) {
    const QFileInfo info(audioPath);
    const QStringList bases = {info.completeBaseName(), QStringLiteral("cover"),
                               QStringLiteral("folder"), QStringLiteral("front"),
                               QStringLiteral("album")};
    const QStringList exts = {QStringLiteral(".jpg"), QStringLiteral(".jpeg"),
                              QStringLiteral(".png"), QStringLiteral(".webp")};
    for (const QString &base : bases)
        for (const QString &ext : exts) {
            const QImage img(info.dir().filePath(base + ext));
            if (!img.isNull())
                return img;
        }
    return QImage();
}

} // namespace

// 列表视图：窗口缩放时各列按基础宽度等比缩放；
// 用户拖动边界后以当前布局为新的比例基准。
class PlaylistView : public QTableView {
public:
    using QTableView::QTableView;

    void setBaseColumnWidths(std::initializer_list<int> widths) {
        m_base = widths;
        applyProportions();
        connect(horizontalHeader(), &QHeaderView::sectionResized, this, [this]() {
            if (m_applying)
                return; // 忽略等比分配自身触发的信号
            m_base.clear();
            for (int c = 0; c < model()->columnCount(); ++c)
                m_base << columnWidth(c);
        });
    }

protected:
    void mousePressEvent(QMouseEvent *event) override {
        // 右键按下未选中的行：先选中该行再交给基类。
        // 基类 extendedSelectionCommand 对「右键未选中的行」会执行 ClearAndSelect
        // （发生在右键菜单事件之前，直接清掉多选）；先选中可让其返回 NoUpdate，
        // 保证：右键已选中行不动多选、右键未选中行只选该行。
        if (event->button() == Qt::RightButton) {
            const QModelIndex index = indexAt(event->position().toPoint());
            if (index.isValid() && !selectionModel()->isSelected(index))
                selectRow(index.row());
        }
        QTableView::mousePressEvent(event);
    }

    void resizeEvent(QResizeEvent *event) override {
        QTableView::resizeEvent(event);
        applyProportions();
    }

private:
    void applyProportions() {
        if (m_applying || m_base.isEmpty())
            return;
        const int total = viewport()->width();
        if (total <= 0)
            return;
        int baseSum = 0;
        for (int w : m_base)
            baseSum += w;
        if (baseSum <= 0)
            return;

        m_applying = true;
        int used = 0;
        for (int c = 0; c < m_base.size(); ++c) {
            const int w = m_base[c] * total / baseSum;
            setColumnWidth(c, w);
            used += w;
        }
        setColumnWidth(0, columnWidth(0) + (total - used)); // 余数给第一列，避免右侧留缝
        m_applying = false;
    }

    QList<int> m_base;
    bool m_applying = false;
};

// 带当前位置标记的滚动条：在凹槽内用蓝色短线标出当前播放歌曲在列表中的位置
class MarkerScrollBar : public QScrollBar {
public:
    using QScrollBar::QScrollBar;

    void setMarkerFraction(qreal fraction) {
        m_fraction = fraction;
        update();
    }

protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            QStyleOptionSlider opt;
            initStyleOption(&opt);
            const QRect groove = style()->subControlRect(
                QStyle::CC_ScrollBar, &opt, QStyle::SC_ScrollBarGroove, this);
            const QRect handle = style()->subControlRect(
                QStyle::CC_ScrollBar, &opt, QStyle::SC_ScrollBarSlider, this);
            // 点击凹槽（非滑块）时直接跳到对应位置，而不是翻页
            if (groove.isValid() && handle.isValid() && !handle.contains(event->pos())
                && opt.maximum > opt.minimum) {
                const bool vertical = orientation() == Qt::Vertical;
                const int clickPos = vertical ? event->pos().y() - groove.y()
                                              : event->pos().x() - groove.x();
                const int handleLen = vertical ? handle.height() : handle.width();
                const int span = (vertical ? groove.height() : groove.width()) - handleLen;
                if (span > 0) {
                    setValue(QStyle::sliderValueFromPosition(
                        opt.minimum, opt.maximum, clickPos - handleLen / 2, span,
                        opt.upsideDown));
                    event->accept();
                    return;
                }
            }
        }
        QScrollBar::mousePressEvent(event); // 点击滑块等其余情况走默认行为
    }

    void paintEvent(QPaintEvent *event) override {
        QScrollBar::paintEvent(event);
        if (m_fraction < 0)
            return;
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        const QRect groove = style()->subControlRect(
            QStyle::CC_ScrollBar, &opt, QStyle::SC_ScrollBarGroove, this);
        if (!groove.isValid() || opt.maximum <= opt.minimum)
            return; // 无滚动内容时不画
        QPainter painter(this);
        const int y = groove.y() + int(m_fraction * (groove.height() - 2)) + 1;
        painter.fillRect(groove.x() + 2, y, groove.width() - 4, 2, QColor(0, 102, 204));
    }

private:
    qreal m_fraction = -1.0;
};

class PlayerWindow : public QMainWindow {
public:
    PlayerWindow() {
        setWindowTitle(QStringLiteral("Music Player"));

        auto *central = new QWidget(this);
        auto *layout = new QVBoxLayout(central);

        // 顶部：列表操作（移除选中 / 清空列表已移到列表右键菜单）
        auto *toolbar = new QHBoxLayout;
        auto *addFiles = new QPushButton(QStringLiteral("添加文件"), central);
        auto *addDir = new QPushButton(QStringLiteral("添加文件夹"), central);
        toolbar->addWidget(addFiles);
        toolbar->addWidget(addDir);
        layout->addLayout(toolbar);

        // 中部：播放列表（文件名 | 专辑 | 序号 | 歌曲 | 大小 | 格式 | 音质 | 时长，
        // 支持 Shift/Ctrl 多选）
        m_playlistModel = new QStandardItemModel(this);
        m_playlistModel->setHorizontalHeaderLabels(
            {QStringLiteral("文件名"), QStringLiteral("专辑"),
             QStringLiteral("序号"), QStringLiteral("歌曲"),
             QStringLiteral("大小"), QStringLiteral("格式"),
             QStringLiteral("音质"), QStringLiteral("时长")});
        m_playlist = new PlaylistView(central);
        m_playlist->setModel(m_playlistModel);
        m_playlist->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_playlist->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_playlist->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_playlist->verticalHeader()->setVisible(false);
        // 各列按基础宽度等比缩放（拖动窗口边框时同步缩放）
        m_playlist->horizontalHeader()->setStretchLastSection(false);
        m_playlist->setBaseColumnWidths({190, 150, 50, 160, 75, 55, 90, 60});
        // 竖向滚动条带当前播放位置标记
        m_scrollBar = new MarkerScrollBar(m_playlist);
        m_playlist->setVerticalScrollBar(m_scrollBar);
        // 右键菜单：移除选中 / 清空列表
        m_playlist->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_playlist, &QWidget::customContextMenuRequested, this,
                &PlayerWindow::showPlaylistMenu);
        layout->addWidget(m_playlist, /*stretch=*/1);

        // 中部两列：左侧当前曲目名，右侧歌词三行
        // （上一句 / 当前句加粗大字 / 下一句暗色弱化）
        const auto makeDimLyric = [](QLabel *label) {
            label->setAlignment(Qt::AlignCenter);
            label->setWordWrap(true);
            label->setForegroundRole(QPalette::Mid);
        };
        m_lyricPrev = new QLabel(central);
        makeDimLyric(m_lyricPrev);
        m_lyricCurrent = new QLabel(central);
        m_lyricCurrent->setAlignment(Qt::AlignCenter);
        m_lyricCurrent->setWordWrap(true);
        QFont lyricFont = m_lyricCurrent->font();
        lyricFont.setPointSize(lyricFont.pointSize() + 7); // 当前句显著放大
        lyricFont.setBold(true);
        m_lyricCurrent->setFont(lyricFont);
        m_lyricNext = new QLabel(central);
        makeDimLyric(m_lyricNext);
        // 前后句略放大，与当前句拉开层次
        QFont dimFont = m_lyricPrev->font();
        dimFont.setPointSize(dimFont.pointSize() + 2);
        m_lyricPrev->setFont(dimFont);
        m_lyricNext->setFont(dimFont);

        auto *lyricColumn = new QVBoxLayout;
        lyricColumn->addWidget(m_lyricPrev);
        lyricColumn->addWidget(m_lyricCurrent);
        lyricColumn->addWidget(m_lyricNext);

        // 左侧列：封面 | （歌手 / 曲目名 两行）
        m_cover = new QLabel(central);
        m_cover->setFixedSize(84, 84);
        m_cover->setAlignment(Qt::AlignCenter);
        m_cover->setStyleSheet(QStringLiteral(
            "QLabel { border: 1px solid palette(mid); border-radius: 4px;"
            " color: palette(mid); }"));
        setCoverPlaceholder();

        m_artist = new QLabel(central);
        m_artist->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_artist->setWordWrap(true);
        // 歌手用白色偏灰显示：弱化但保持清晰
        m_artist->setStyleSheet(QStringLiteral("color: #C8C8C8;"));

        m_title = new QLabel(QStringLiteral("未加载文件"), central);
        m_title->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_title->setWordWrap(true);
        QFont titleFont = m_title->font();
        titleFont.setPointSize(titleFont.pointSize() + 2); // 曲目名再大一点
        titleFont.setBold(true);
        m_title->setFont(titleFont);

        auto *infoColumn = new QVBoxLayout;
        infoColumn->setSpacing(2);
        // 上下 stretch 撑满：wordWrap 标签才能按实际宽度换行，
        // 直接用 AlignVCenter 会卡在单行 sizeHint 高度导致长歌名被裁
        infoColumn->addStretch();
        infoColumn->addWidget(m_artist);
        infoColumn->addWidget(m_title);
        infoColumn->addStretch();

        auto *leftColumn = new QHBoxLayout;
        leftColumn->setSpacing(8);
        leftColumn->addWidget(m_cover, 0, Qt::AlignVCenter);
        leftColumn->addLayout(infoColumn, 1);

        auto *middleLayout = new QHBoxLayout;
        middleLayout->addLayout(leftColumn, 1); // 左列 20%
        middleLayout->addLayout(lyricColumn, 4); // 右列歌词 80%
        layout->addLayout(middleLayout);

        // 进度条行：当前时间 | 进度 | 总时长
        auto *progressLayout = new QHBoxLayout;
        m_timeLabel = new QLabel(QStringLiteral("0:00"), central);
        m_progress = new QSlider(Qt::Horizontal, central);
        m_progress->setRange(0, 1);
        m_progress->setValue(0);
        m_totalLabel = new QLabel(QStringLiteral("0:00"), central);
        progressLayout->addWidget(m_timeLabel);
        progressLayout->addWidget(m_progress, /*stretch=*/1);
        progressLayout->addWidget(m_totalLabel);
        layout->addLayout(progressLayout);

        // 控制行：上一首 | 播放/暂停 | 下一首 | 音量
        auto *controls = new QHBoxLayout;
        m_prev = new QPushButton(QStringLiteral("◀◀"), central);
        m_prev->setToolTip(QStringLiteral("上一首"));
        m_prev->setEnabled(false);
        m_play = new QPushButton(QStringLiteral("播放"), central);
        m_play->setEnabled(false);
        m_next = new QPushButton(QStringLiteral("▶▶"), central);
        m_next->setToolTip(QStringLiteral("下一首"));
        m_next->setEnabled(false);
        auto *volumeLabel = new QLabel(QStringLiteral("音量"), central);
        m_volume = new QSlider(Qt::Horizontal, central);
        m_volume->setRange(0, 100);
        m_volume->setValue(50); // 首次启动默认音量 50%
        m_volume->setFixedWidth(110);
        m_modeBox = new QComboBox(central);
        m_modeBox->addItems({QStringLiteral("顺序播放"), QStringLiteral("列表循环"),
                             QStringLiteral("单曲循环"), QStringLiteral("随机播放"),
                             QStringLiteral("专辑循环"), QStringLiteral("专辑随机")});
        m_modeBox->setToolTip(QStringLiteral("播放模式"));
        connect(m_modeBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
                &PlayerWindow::onModeChanged);
        controls->addWidget(m_prev);
        controls->addWidget(m_play);
        controls->addWidget(m_next);
        controls->addStretch();
        controls->addWidget(m_modeBox);
        controls->addWidget(volumeLabel);
        controls->addWidget(m_volume);
        layout->addLayout(controls);

        setCentralWidget(central);
        resize(840, 640);

        // 播放器
        m_player = new QMediaPlayer(this);
        m_audio = new QAudioOutput(this);
        m_player->setAudioOutput(m_audio);

        connect(addFiles, &QPushButton::clicked, this, &PlayerWindow::addFiles);
        connect(addDir, &QPushButton::clicked, this, &PlayerWindow::addDirectory);
        connect(m_playlist, &QTableView::doubleClicked, this, &PlayerWindow::onRowDoubleClicked);
        connect(m_play, &QPushButton::clicked, this, &PlayerWindow::togglePlay);
        connect(m_prev, &QPushButton::clicked, this, &PlayerWindow::playPrevious);
        connect(m_next, &QPushButton::clicked, this, &PlayerWindow::playNext);
        connect(m_player, &QMediaPlayer::playbackStateChanged, this, &PlayerWindow::syncPlayButton);
        connect(m_player, &QMediaPlayer::mediaStatusChanged, this, &PlayerWindow::onMediaStatusChanged);
        connect(m_player, &QMediaPlayer::errorOccurred, this, &PlayerWindow::onPlayerError);
        connect(m_player, &QMediaPlayer::positionChanged, this, &PlayerWindow::onPositionChanged);
        connect(m_player, &QMediaPlayer::durationChanged, this, &PlayerWindow::onDurationChanged);
        connect(m_progress, &QSlider::sliderPressed, this, [this] { m_seeking = true; });
        connect(m_progress, &QSlider::sliderMoved, this, [this](int value) {
            m_timeLabel->setText(formatTime(value));
            m_player->setPosition(value); // 拖动时实时试听
        });
        connect(m_progress, &QSlider::sliderReleased, this, [this] {
            m_player->setPosition(m_progress->value());
            m_seeking = false;
        });
        connect(m_volume, &QSlider::valueChanged, this, [this](int value) {
            m_audio->setVolume(value / 100.0);
        });

        // 选中/滚动后空闲 30 秒：视图回到当前播放歌曲位置
        m_idleTimer = new QTimer(this);
        m_idleTimer->setSingleShot(true);
        m_idleTimer->setInterval(30000);
        connect(m_idleTimer, &QTimer::timeout, this, &PlayerWindow::onSelectionIdle);
        const auto restartIdleTimer = [this] { m_idleTimer->start(); };
        connect(m_playlist->selectionModel(), &QItemSelectionModel::selectionChanged,
                this, restartIdleTimer);
        connect(m_playlist->verticalScrollBar(), &QScrollBar::valueChanged,
                this, restartIdleTimer);
        connect(m_playlist->horizontalScrollBar(), &QScrollBar::valueChanged,
                this, restartIdleTimer);

        // 键盘操作统一走事件过滤器（nativeVirtualKey 识别，见 eventFilter）：
        // macOS 中文输入法会导致 Cocoa/Carbon 键映射不一致，QShortcut 可能失效
        qApp->installEventFilter(this);
        // 仅保留媒体音量键走 QShortcut（虚拟键码在各机型上不统一）
        const auto addShortcut = [this](int key, const std::function<void()> &fn) {
            auto *shortcut = new QShortcut(QKeySequence(key), this);
            connect(shortcut, &QShortcut::activated, this, fn);
        };
        addShortcut(Qt::Key_VolumeUp, [this] { changeVolume(+5); });
        addShortcut(Qt::Key_VolumeDown, [this] { changeVolume(-5); });

        // 状态记忆：退出时保存，启动时恢复
        connect(qApp, &QApplication::aboutToQuit, this, &PlayerWindow::saveState);
        restoreState();
    }

private:
    // ---------- 列表操作 ----------

    void addFiles() {
        QStringList paths = QFileDialog::getOpenFileNames(
            this, QStringLiteral("选择音频文件"), QDir::homePath(), kFileDialogFilter);
        std::sort(paths.begin(), paths.end(), pathLessThan);
        addPaths(paths, QString());
    }

    void addDirectory() {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择文件夹"), QDir::homePath());
        if (dir.isEmpty())
            return;

        QStringList paths;
        QDirIterator it(dir, kAudioSuffixes,
                        QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
        while (it.hasNext())
            paths << it.next();
        std::sort(paths.begin(), paths.end(), pathLessThan); // 按路径自然排序 → 按文件夹分组
        addPaths(paths, dir);
    }

    void addPaths(const QStringList &paths, const QString &rootDir) {
        for (const QString &path : paths) {
            if (m_paths.contains(path))
                continue;
            m_paths.insert(path);

            // 读一次标签，同时取歌曲名 / 专辑名 / 曲目编号 / 时长 / 码率 / 采样率
            QString title;
            QString album;
            unsigned int track = 0;
            int seconds = 0;
            int bitrate = 0;
            int sampleRate = 0;
            const TagLib::FileRef ref(path.toUtf8().constData());
            if (!ref.isNull()) {
                if (ref.tag()) {
                    title = QString::fromStdWString(ref.tag()->title().toWString()).trimmed();
                    album = QString::fromStdWString(ref.tag()->album().toWString()).trimmed();
                    track = ref.tag()->track();
                }
                if (ref.audioProperties()) {
                    const auto *ap = ref.audioProperties();
                    seconds = ap->lengthInSeconds();
                    bitrate = ap->bitrate();
                    sampleRate = ap->sampleRate();
                }
            }
            // 无专辑标签时退回所在子文件夹名（rootDir 为空即逐文件添加时总是显示；
            // 添加文件夹时仅当文件不在根目录下才显示）。
            if (album.isEmpty()) {
                const QString parent = QFileInfo(path).dir().path();
                if (rootDir.isEmpty() || parent != rootDir)
                    album = QFileInfo(parent).fileName();
            }

            auto *nameItem = new QStandardItem(QFileInfo(path).fileName());
            nameItem->setData(path, Qt::UserRole);
            nameItem->setToolTip(path);

            auto *albumItem = new QStandardItem(album);
            albumItem->setToolTip(path);
            albumItem->setForeground(QBrush(Qt::gray)); // 弱化显示，突出文件名

            auto *trackItem = new QStandardItem(
                track ? QStringLiteral("%1").arg(track, 2, 10, QLatin1Char('0'))
                      : QString());
            trackItem->setToolTip(path);
            trackItem->setTextAlignment(Qt::AlignCenter);
            trackItem->setForeground(QBrush(Qt::gray));

            auto *titleItem = new QStandardItem(title); // 标签中的准确歌曲名
            titleItem->setToolTip(path);

            auto *sizeItem = new QStandardItem(
                QStringLiteral("%1 MB").arg(QFileInfo(path).size() / 1024.0 / 1024.0, 0, 'f', 1));
            sizeItem->setToolTip(path);
            sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            sizeItem->setForeground(QBrush(Qt::gray));

            auto *formatItem = new QStandardItem(QFileInfo(path).suffix().toUpper());
            formatItem->setToolTip(path);
            formatItem->setTextAlignment(Qt::AlignCenter);
            formatItem->setForeground(QBrush(Qt::gray));

            // 音质列：如 "320k/44.1kHz"；采样率为整千时省略小数（48kHz）
            QString quality;
            if (bitrate > 0) {
                const QString kHz = sampleRate % 1000 == 0
                                        ? QString::number(sampleRate / 1000)
                                        : QString::number(sampleRate / 1000.0, 'f', 1);
                quality = QStringLiteral("%1k/%2kHz").arg(bitrate).arg(kHz);
            }
            auto *qualityItem = new QStandardItem(quality);
            qualityItem->setToolTip(path);
            qualityItem->setTextAlignment(Qt::AlignCenter);
            qualityItem->setForeground(QBrush(Qt::gray));

            auto *durationItem = new QStandardItem(
                seconds > 0 ? QStringLiteral("%1:%2")
                                  .arg(seconds / 60)
                                  .arg(seconds % 60, 2, 10, QLatin1Char('0'))
                            : QString());
            durationItem->setToolTip(path);
            durationItem->setTextAlignment(Qt::AlignCenter);
            durationItem->setForeground(QBrush(Qt::gray));

            m_playlistModel->appendRow({nameItem, albumItem, trackItem, titleItem, sizeItem,
                                        formatItem, qualityItem, durationItem});
        }
        updateScrollMarker(); // 列表变长，当前播放的相对位置随之变化
    }

    void removeSelected() {
        QList<int> rows;
        const QModelIndexList sel = m_playlist->selectionModel()->selectedRows();
        for (const QModelIndex &idx : sel)
            rows << idx.row();
        std::sort(rows.begin(), rows.end(), std::greater<int>());

        int removedBelow = 0;
        bool removedCurrent = false;
        for (int row : rows) {
            m_paths.remove(m_playlistModel->item(row, 0)->data(Qt::UserRole).toString());
            if (row < m_currentRow)
                ++removedBelow;
            else if (row == m_currentRow)
                removedCurrent = true;
            m_playlistModel->removeRow(row);
        }

        if (removedCurrent) {
            m_player->stop();
            m_currentRow = -1;
            resetLabels(); // 内部会把标记行重置
        } else {
            m_currentRow -= removedBelow;
            m_markedRow -= removedBelow; // 标记行随当前行同步上移
        }
        updateScrollMarker();
        // 行号整体移动：洗牌队列与回退历史中的行号失效，作废待重建
        m_shuffleQueue.clear();
        m_history.clear();
    }

    void clearPlaylist() {
        m_player->stop();
        m_playlistModel->setRowCount(0);
        m_paths.clear();
        m_currentRow = -1;
        resetLabels();
        updateScrollMarker();
        m_shuffleQueue.clear();
        m_history.clear();
    }

    // 列表右键菜单：全选 / 移除 / 清空列表。
    // 清空列表容易误操作：仅当列表已全选时才出现在菜单里（先全选、再右键清空）。
    // 右键点击的行不在选中集合时先选中该行，明确「移除」的作用对象。
    void showPlaylistMenu(const QPoint &pos) {
        // pos 是视图坐标（含表头偏移），indexAt 需要视口坐标
        const QModelIndex index =
            m_playlist->indexAt(m_playlist->viewport()->mapFrom(m_playlist, pos));
        if (index.isValid() && !m_playlist->selectionModel()->isSelected(index))
            m_playlist->selectRow(index.row());

        const int n = m_playlistModel->rowCount();
        const bool allSelected =
            n > 0 && m_playlist->selectionModel()->selectedRows().size() == n;

        QMenu menu(this);
        QAction *removeAct = menu.addAction(QStringLiteral("移除"));
        removeAct->setEnabled(m_playlist->selectionModel()->hasSelection());
        QAction *selectAllAct = menu.addAction(QStringLiteral("全选"));
        selectAllAct->setEnabled(n > 0 && !allSelected);
        menu.addSeparator();
        QAction *clearAct = menu.addAction(QStringLiteral("清空列表"));
        clearAct->setVisible(allSelected); // 未全选时不显示，防止误点

        const QAction *chosen = menu.exec(m_playlist->mapToGlobal(pos));
        // 菜单关闭后把焦点还给列表视图：macOS 上选中色随焦点状态渲染，
        // 焦点不在视图时蓝色框会退成灰色
        m_playlist->setFocus();
        if (chosen == selectAllAct)
            m_playlist->selectAll();
        else if (chosen == removeAct)
            removeSelected();
        else if (chosen == clearAct)
            clearPlaylist();
    }

    // ---------- 播放 ----------

    void onRowDoubleClicked(const QModelIndex &index) {
        playRow(index.row());
    }

    void playRow(int row) {
        if (row < 0 || row >= m_playlistModel->rowCount())
            return;
        m_currentRow = row;
        markCurrentRow(row); // 颜色标注当前播放行
        updateScrollMarker();
        // 选中跟随当前播放行；视图仅在该行不可见时才做最小滚动，
        // 不打断用户正在浏览的列表
        m_playlist->selectRow(row);
        m_playlist->scrollTo(m_playlistModel->index(row, 0),
                             QAbstractItemView::EnsureVisible);
        loadTrack(m_playlistModel->item(row, 0)->data(Qt::UserRole).toString());
        m_player->play();
    }

    // 当前播放行标注：文件名、歌曲两列用深蓝色加粗显示。
    // 不用背景色——与选中高亮、灰色小字列都不冲突，选中时文字依然清晰。
    void markCurrentRow(int row) {
        const auto setMark = [this](int r, bool on) {
            if (r < 0 || r >= m_playlistModel->rowCount())
                return;
            for (int c : {0, 3}) { // 文件名列、歌曲列
                auto *it = m_playlistModel->item(r, c);
                if (on) {
                    QFont font = m_playlist->font();
                    font.setBold(true);
                    it->setFont(font);
                    it->setForeground(QBrush(QColor(0, 102, 204)));
                } else {
                    // 恢复默认字体与前景色。注意不能用 setForeground(QBrush())：
                    // 空画刷（NoBrush）会使文字不可见。
                    it->setData(QVariant(), Qt::FontRole);
                    it->setData(QVariant(), Qt::ForegroundRole);
                }
            }
        };
        setMark(m_markedRow, false);
        m_markedRow = row;
        setMark(row, true);
    }

    // 更新滚动条上的当前播放位置标记
    void updateScrollMarker() {
        const int n = m_playlistModel->rowCount();
        m_scrollBar->setMarkerFraction(n > 0 && m_currentRow >= 0
                                           ? (m_currentRow + 0.5) / n
                                           : -1.0);
    }

    void loadTrack(const QString &path) {
        // 用 TagLib 读取元数据
        const TagLib::FileRef ref(path.toUtf8().constData());
        const QString fileName = QFileInfo(path).fileName();

        QString title = fileName;
        QString artist;
        int seconds = 0;
        if (!ref.isNull() && ref.tag()) {
            const QString tagTitle =
                QString::fromStdWString(ref.tag()->title().toWString());
            if (!tagTitle.isEmpty())
                title = tagTitle;
            artist =
                QString::fromStdWString(ref.tag()->artist().toWString()).trimmed();
            if (artist.isEmpty()) // 无歌手时退回专辑名，保持该行不空
                artist = QString::fromStdWString(ref.tag()->album().toWString())
                             .trimmed();
        }
        if (!ref.isNull() && ref.audioProperties())
            seconds = ref.audioProperties()->lengthInSeconds();

        m_artist->setText(artist);
        m_title->setText(title);
        updateCover(ref, path);
        m_play->setEnabled(true);
        m_prev->setEnabled(true);
        m_next->setEnabled(true);
        // 加载同名 .lrc 歌词
        m_lyrics.clear();
        m_lyricIndex = -1;
        const QString lrcPath = QFileInfo(path).absolutePath() + QLatin1Char('/')
            + QFileInfo(path).completeBaseName() + QStringLiteral(".lrc");
        m_lyricPrev->clear();
        m_lyricCurrent->clear();
        if (loadLrcFile(lrcPath, m_lyrics))
            m_lyricNext->clear();
        else
            m_lyricNext->setText(QStringLiteral("（暂无歌词）"));
        // 进度条复位：先用 TagLib 时长兜底，媒体加载后 durationChanged 会刷新
        m_seeking = false;
        m_progress->setRange(0, seconds * 1000);
        m_progress->setValue(0);
        m_timeLabel->setText(QStringLiteral("0:00"));
        m_totalLabel->setText(formatTime(seconds * 1000LL));
        m_player->setSource(QUrl::fromLocalFile(path));
    }

    // 更新封面显示：优先内嵌图片，其次同目录常见封面文件；无则占位符
    void updateCover(const TagLib::FileRef &ref, const QString &path) {
        QImage img = embeddedCover(ref);
        if (img.isNull())
            img = folderCover(path);
        if (img.isNull()) {
            setCoverPlaceholder();
            return;
        }
        m_cover->setPixmap(QPixmap::fromImage(img).scaled(
            m_cover->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    void setCoverPlaceholder() {
        m_cover->setPixmap(QPixmap());
        m_cover->setText(QStringLiteral("♪"));
    }

    void togglePlay() {
        if (m_currentRow < 0)
            return;
        m_playlist->selectRow(m_currentRow); // 播放/暂停时选中态也跟随当前歌曲
        if (m_player->playbackState() == QMediaPlayer::PlayingState)
            m_player->pause();
        else
            m_player->play();
    }

    void playPrevious() {
        if (m_currentRow < 0)
            return;
        // 播放超过 3 秒时先回到本曲开头（常见播放器行为）
        if (m_player->position() > 3000) {
            m_player->setPosition(0);
            return;
        }
        const int prev = previousRowFor();
        if (prev >= 0)
            playRow(prev);
    }

    void playNext() {
        if (m_currentRow < 0)
            return;
        const int next = nextRowFor();
        if (next >= 0)
            playRow(next);
    }

    void onPositionChanged(qint64 position) {
        m_timeLabel->setText(formatTime(position));
        if (!m_seeking) { // 拖动进度条期间滑块不跟随媒体位置，避免互相拉扯
            const QSignalBlocker blocker(m_progress);
            m_progress->setValue(int(position));
        }
        updateLyric(position);
    }

    // 二分查找当前播放位置对应的歌词行，仅变化时刷新显示
    void updateLyric(qint64 positionMs) {
        if (m_lyrics.isEmpty())
            return;
        int lo = 0, hi = m_lyrics.size() - 1, current = -1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            if (m_lyrics[mid].timeMs <= positionMs) {
                current = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        if (current == m_lyricIndex)
            return;
        m_lyricIndex = current;
        // 三行显示：上一句 / 当前句 / 下一句
        m_lyricPrev->setText(current > 0 ? m_lyrics[current - 1].text : QString());
        m_lyricCurrent->setText(current >= 0 ? m_lyrics[current].text : QString());
        m_lyricNext->setText(current + 1 < m_lyrics.size() ? m_lyrics[current + 1].text
                                                           : QString());
    }

    void onDurationChanged(qint64 duration) {
        m_totalLabel->setText(formatTime(duration));
        m_progress->setRange(0, int(duration));
    }

    // 选中/滚动空闲 30 秒后触发：选中态与视图一起回到当前播放行
    void onSelectionIdle() {
        if (m_currentRow < 0 || m_currentRow >= m_playlistModel->rowCount())
            return;
        const QModelIndex idx = m_playlistModel->index(m_currentRow, 0);
        m_playlist->selectRow(m_currentRow);
        m_playlist->setCurrentIndex(idx);
        if (!m_playlist->viewport()->rect().contains(m_playlist->visualRect(idx)))
            m_playlist->scrollTo(idx, QAbstractItemView::EnsureVisible);
    }

    // ---------- 键盘操作 ----------

    // 全局按键过滤：用 nativeVirtualKey（macOS 原生虚拟键码）识别按键，
    // 绕过中文输入法导致的 Cocoa/Carbon 键映射不一致问题。
    // 带修饰键的按键（如 Shift+↑ 扩展选择）不拦截，交给控件默认处理。
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (event->type() != QEvent::KeyPress)
            return QMainWindow::eventFilter(obj, event);
        if (QApplication::activeModalWidget()) // 文件选择对话框等模态窗口打开时不拦截
            return false;
        auto *ke = static_cast<QKeyEvent *>(event);
        const auto mods = ke->modifiers();
        const int vk = ke->nativeVirtualKey();

        // , 和 .（即 < >）键控制上/下一首：允许带 Shift（打 < > 本身就需要 Shift），
        // 按虚拟键码识别，任何输入法下都生效
        if (vk == 43 && (mods == Qt::NoModifier || mods == Qt::ShiftModifier)) {
            playPrevious(); // < 上一首
            return true;
        }
        if (vk == 47 && (mods == Qt::NoModifier || mods == Qt::ShiftModifier)) {
            playNext(); // > 下一首
            return true;
        }

        if (mods != Qt::NoModifier && mods != Qt::KeypadModifier)
            return false;
        switch (vk) {
        case 123: changeVolume(-5); return true;  // ← 音量减小
        case 124: changeVolume(+5); return true;  // → 音量增大
        case 125: moveSelection(+1); return true; // ↓ 选中下移
        case 126: moveSelection(-1); return true; // ↑ 选中上移
        case 13: moveSelection(-1); return true;  // W：选中上移
        case 1: moveSelection(+1); return true;   // S：选中下移
        case 36:                                // 主回车
        case 76:                                // 小键盘回车
            playSelectedRow(); return true;
        case 49: togglePlay(); return true;     // 空格
        case 116: changeVolume(+5); return true;  // Fn+↑ / PageUp
        case 121: changeVolume(-5); return true;  // Fn+↓ / PageDown
        default: break;
        }
        return false;
    }

    // 上下键移动选中行，越界时停在首/末行，并保持选中行可见
    void moveSelection(int delta) {
        const int n = m_playlistModel->rowCount();
        if (n == 0)
            return;
        const QModelIndex cur = m_playlist->currentIndex();
        const int row = cur.isValid() ? cur.row() : (delta > 0 ? -1 : 0);
        const int target = qBound(0, row + delta, n - 1);
        m_playlist->selectRow(target);
        m_playlist->setCurrentIndex(m_playlistModel->index(target, 0));
        m_playlist->scrollTo(m_playlistModel->index(target, 0),
                             QAbstractItemView::EnsureVisible);
    }

    // 回车：播放当前选中的歌曲
    void playSelectedRow() {
        const QModelIndex cur = m_playlist->currentIndex();
        if (cur.isValid())
            playRow(cur.row());
    }

    // 音量增减（Fn+上下键 / 媒体音量键）
    void changeVolume(int delta) {
        m_volume->setValue(qBound(0, m_volume->value() + delta, 100));
    }

    void syncPlayButton(QMediaPlayer::PlaybackState state) {
        m_play->setText(state == QMediaPlayer::PlayingState
                            ? QStringLiteral("暂停")
                            : QStringLiteral("播放"));
    }

    void onMediaStatusChanged(QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia && m_pendingPosition > 0) {
            // 恢复上次播放位置（需等媒体加载完成）
            m_player->setPosition(m_pendingPosition);
            m_pendingPosition = 0;
        }
        if (status == QMediaPlayer::EndOfMedia) {
            // 按播放模式决定下一首；返回 -1 表示停止
            const int next = nextRowFor();
            if (next >= 0)
                playRow(next);
            else
                m_player->stop();
        }
    }

    // 播放出错时在状态栏提示文件名与原因，避免点击后毫无反应
    void onPlayerError(QMediaPlayer::Error error, const QString &errorString) {
        if (error == QMediaPlayer::NoError)
            return;
        QString name = QStringLiteral("当前文件");
        if (m_currentRow >= 0 && m_currentRow < m_playlistModel->rowCount())
            name = m_playlistModel->item(m_currentRow, 0)->text();
        const QString detail = errorString.isEmpty()
                                   ? QStringLiteral("格式不受支持或文件已损坏")
                                   : errorString;
        statusBar()->showMessage(
            QStringLiteral("无法播放「%1」：%2").arg(name, detail), 8000);
    }

    // 播放模式（枚举值与下拉框顺序一致，随 QSettings 持久化）
    enum PlayMode {
        kModeSequential = 0, // 顺序播放：到列表末尾停止
        kModeListLoop,       // 列表循环
        kModeSingleLoop,     // 单曲循环
        kModeShuffle,        // 随机播放（全列表）
        kModeAlbumLoop,      // 专辑循环
        kModeAlbumShuffle,   // 专辑随机
    };

    // 专辑 = 同一父文件夹的连续行（列表按路径排序，同专辑天然连续）
    std::pair<int, int> albumBounds(int row) const {
        const auto dirOf = [this](int r) {
            return QFileInfo(m_playlistModel->item(r, 0)->data(Qt::UserRole).toString())
                .dir()
                .path();
        };
        const QString dir = dirOf(row);
        int start = row;
        while (start > 0 && dirOf(start - 1) == dir)
            --start;
        int end = row;
        while (end + 1 < m_playlistModel->rowCount() && dirOf(end + 1) == dir)
            ++end;
        return {start, end};
    }

    // 播放模式切换：随机模式立即洗牌；切换后旧的洗牌队列与回退历史作废
    void onModeChanged(int index) {
        m_mode = index;
        if (m_mode == kModeShuffle)
            rebuildShuffleQueue();
        else
            m_shuffleQueue.clear();
        m_history.clear();
    }

    // 按播放模式计算自动切歌的下一行；返回 -1 表示停止。
    // 非 const：洗牌模式需要推进洗牌队列并记录回退历史。
    int nextRowFor() {
        const int n = m_playlistModel->rowCount();
        if (n == 0 || m_currentRow < 0)
            return -1;
        switch (m_mode) {
        case kModeSingleLoop:
            return m_currentRow;
        case kModeListLoop:
            return (m_currentRow + 1) % n;
        case kModeShuffle: {
            if (n <= 1)
                return -1;
            // 记录离开的曲目，供 ◀◀ 按历史回退
            if (m_history.size() >= 100)
                m_history.removeFirst();
            m_history << m_currentRow;
            return shuffleNextRow();
        }
        case kModeAlbumLoop: {
            const auto bounds = albumBounds(m_currentRow);
            return m_currentRow + 1 <= bounds.second ? m_currentRow + 1 : bounds.first;
        }
        case kModeAlbumShuffle: {
            const auto bounds = albumBounds(m_currentRow);
            if (bounds.first == bounds.second)
                return -1; // 单曲专辑无从随机，停止
            int r = m_currentRow;
            while (r == m_currentRow)
                r = int(QRandomGenerator::global()->bounded(bounds.first, bounds.second + 1));
            return r;
        }
        case kModeSequential:
        default:
            return m_currentRow + 1 < n ? m_currentRow + 1 : -1;
        }
    }

    // 与 nextRowFor 对称的上一行；返回 -1 表示无操作
    int previousRowFor() {
        const int n = m_playlistModel->rowCount();
        if (n == 0 || m_currentRow < 0)
            return -1;
        switch (m_mode) {
        case kModeSingleLoop:
            return m_currentRow; // 单曲循环：重播本曲
        case kModeListLoop:
            return (m_currentRow - 1 + n) % n;
        case kModeShuffle:
            return m_history.isEmpty() ? -1 : m_history.takeLast();
        case kModeAlbumLoop: {
            const auto bounds = albumBounds(m_currentRow);
            return m_currentRow - 1 >= bounds.first ? m_currentRow - 1 : bounds.second;
        }
        case kModeAlbumShuffle: {
            const auto bounds = albumBounds(m_currentRow);
            if (bounds.first == bounds.second)
                return -1;
            int r = m_currentRow;
            while (r == m_currentRow)
                r = int(QRandomGenerator::global()->bounded(bounds.first, bounds.second + 1));
            return r;
        }
        case kModeSequential:
        default:
            return m_currentRow > 0 ? m_currentRow - 1 : -1;
        }
    }

    // 重建洗牌队列：除当前行外全部行随机排列，保证一轮内不重复
    void rebuildShuffleQueue() {
        m_shuffleQueue.clear();
        const int n = m_playlistModel->rowCount();
        for (int i = 0; i < n; ++i)
            if (i != m_currentRow)
                m_shuffleQueue << i;
        for (int i = m_shuffleQueue.size() - 1; i > 0; --i) { // Fisher-Yates
            const int j = int(QRandomGenerator::global()->bounded(i + 1));
            std::swap(m_shuffleQueue[i], m_shuffleQueue[j]);
        }
    }

    // 从洗牌队列取下一行：队列耗尽时重建；跳过失效行与当前行
    int shuffleNextRow() {
        const int n = m_playlistModel->rowCount();
        while (!m_shuffleQueue.isEmpty()) {
            const int r = m_shuffleQueue.takeFirst();
            if (r >= 0 && r < n && r != m_currentRow)
                return r;
        }
        rebuildShuffleQueue();
        return m_shuffleQueue.isEmpty() ? -1 : m_shuffleQueue.takeFirst();
    }

    // ---------- 状态记忆 ----------

    void saveState() {
        QSettings settings;
        settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
        settings.setValue(QStringLiteral("volume"), m_volume->value());

        QStringList paths;
        paths.reserve(m_playlistModel->rowCount());
        for (int r = 0; r < m_playlistModel->rowCount(); ++r)
            paths << m_playlistModel->item(r, 0)->data(Qt::UserRole).toString();
        settings.setValue(QStringLiteral("playlist/paths"), paths);

        settings.setValue(QStringLiteral("playback/row"), m_currentRow);
        settings.setValue(QStringLiteral("playback/path"),
                          m_currentRow >= 0
                              ? m_playlistModel->item(m_currentRow, 0)
                                    ->data(Qt::UserRole)
                                    .toString()
                              : QString());
        settings.setValue(QStringLiteral("playback/position"),
                          m_currentRow >= 0 ? m_player->position() : 0);
        settings.setValue(QStringLiteral("playback/mode"), m_mode);
    }

    void restoreState() {
        QSettings settings;

        const QByteArray geometry =
            settings.value(QStringLiteral("window/geometry")).toByteArray();
        if (!geometry.isEmpty())
            restoreGeometry(geometry);

        m_volume->setValue(settings.value(QStringLiteral("volume"), 50).toInt());

        const int mode = settings.value(QStringLiteral("playback/mode"), kModeSequential).toInt();
        if (mode >= kModeSequential && mode <= kModeAlbumShuffle) {
            m_mode = mode;
            m_modeBox->setCurrentIndex(mode);
        }

        // 已不存在的路径直接过滤，避免恢复出播放即报错的空行
        QStringList existingPaths;
        const QStringList paths =
            settings.value(QStringLiteral("playlist/paths")).toStringList();
        for (const QString &p : paths)
            if (QFileInfo::exists(p))
                existingPaths << p;
        if (!existingPaths.isEmpty())
            addPaths(existingPaths, QString()); // 无根目录语义：专辑回退显示所在文件夹名

        // 优先按上次播放的文件路径定位行（行号会因失效路径被过滤而偏移）
        int row = settings.value(QStringLiteral("playback/row"), -1).toInt();
        const QString rowPath =
            settings.value(QStringLiteral("playback/path")).toString();
        if (!rowPath.isEmpty()) {
            for (int r = 0; r < m_playlistModel->rowCount(); ++r) {
                if (m_playlistModel->item(r, 0)->data(Qt::UserRole).toString()
                    == rowPath) {
                    row = r;
                    break;
                }
            }
        }
        const qint64 position =
            settings.value(QStringLiteral("playback/position"), 0).toLongLong();
        if (row >= 0 && row < m_playlistModel->rowCount()) {
            m_currentRow = row;
            markCurrentRow(row);
            updateScrollMarker();
            m_playlist->selectRow(row);
            m_playlist->scrollTo(m_playlistModel->index(row, 0),
                                 QAbstractItemView::EnsureVisible);
            loadTrack(m_playlistModel->item(row, 0)->data(Qt::UserRole).toString());
            m_pendingPosition = position; // 媒体加载完成后定位（见 onMediaStatusChanged）
        }
    }

    void resetLabels() {
        m_artist->clear();
        m_title->setText(QStringLiteral("未加载文件"));
        setCoverPlaceholder();
        m_play->setEnabled(false);
        m_prev->setEnabled(false);
        m_next->setEnabled(false);
        m_markedRow = -1;
        m_seeking = false;
        m_progress->setRange(0, 1);
        m_progress->setValue(0);
        m_timeLabel->setText(QStringLiteral("0:00"));
        m_totalLabel->setText(QStringLiteral("0:00"));
        m_lyrics.clear();
        m_lyricIndex = -1;
        m_lyricPrev->clear();
        m_lyricCurrent->clear();
        m_lyricNext->clear();
    }

    PlaylistView *m_playlist = nullptr;
    QStandardItemModel *m_playlistModel = nullptr;
    MarkerScrollBar *m_scrollBar = nullptr;
    QLabel *m_cover = nullptr;
    QLabel *m_artist = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_timeLabel = nullptr;
    QLabel *m_totalLabel = nullptr;
    QLabel *m_lyricPrev = nullptr;
    QLabel *m_lyricCurrent = nullptr;
    QLabel *m_lyricNext = nullptr;
    QPushButton *m_play = nullptr;
    QPushButton *m_prev = nullptr;
    QPushButton *m_next = nullptr;
    QSlider *m_progress = nullptr;
    QSlider *m_volume = nullptr;
    QComboBox *m_modeBox = nullptr;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audio = nullptr;
    QSet<QString> m_paths; // 列表内文件路径，用于去重
    QTimer *m_idleTimer = nullptr; // 选中/滚动空闲回位计时器
    QList<int> m_shuffleQueue; // 随机模式洗牌队列：待播行（一轮内不重复）
    QList<int> m_history;      // 随机模式回退历史：◀◀ 按此顺序回退
    int m_currentRow = -1; // 当前播放行
    int m_markedRow = -1;  // 当前有颜色标记的行（与 m_currentRow 同步维护）
    bool m_seeking = false; // 正在拖动进度条
    qint64 m_pendingPosition = 0; // 启动恢复时待定位的播放位置（毫秒）
    int m_mode = kModeSequential; // 当前播放模式
    QList<LyricLine> m_lyrics; // 当前曲目的歌词
    int m_lyricIndex = -1;     // 当前显示的歌词行
};

int main(int argc, char *argv[]) {
    QCoreApplication::setOrganizationName(QStringLiteral("MusicPlayer"));
    QCoreApplication::setApplicationName(QStringLiteral("MusicPlayer"));
    QApplication app(argc, argv);
    PlayerWindow w;
    w.show();
    return app.exec();
}
