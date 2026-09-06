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
#include <QHash>
#include <QBrush>
#include <QColor>
#include <QSlider>
#include <QScrollBar>
#include <QStyleOptionSlider>
#include <QMouseEvent>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QShortcut>
#include <QKeyEvent>
#include <QSettings>
#include <QStatusBar>
#include <QMenu>
#include <QListWidget>
#include <QComboBox>
#include <QRandomGenerator>
#include <QItemSelectionModel>
#include <QFile>
#include <QSaveFile>
#include <QElapsedTimer>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLineEdit>
#include <QMessageBox>
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

// 播放列表 JSON 持久化路径（~/Library/Application Support/MusicPlayer/playlists.json）
QString playlistsFilePath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/playlists.json");
}

// 元数据缓存持久化路径（同一目录）
QString metaCacheFilePath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/meta_cache.json");
}

// 按扩展名判断是否为支持的音频文件（拖拽添加用）
bool isAudioFile(const QString &path) {
    static const QSet<QString> suffixes = {
        QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("m4a"),
        QStringLiteral("wav"), QStringLiteral("ogg"), QStringLiteral("aac"),
    };
    return suffixes.contains(QFileInfo(path).suffix().toLower());
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
        connect(horizontalHeader(), &QHeaderView::sectionResized, this,
                [this](int logicalIndex) {
            if (m_applying || logicalIndex < 0 || logicalIndex >= m_base.size())
                return; // 忽略等比分配自身触发的信号
            // 用户拖动边界：该列固定为新宽度，其余可见列等比填充剩余空间
            m_base[logicalIndex] = columnWidth(logicalIndex);
            m_fixedColumn = logicalIndex;
            applyProportions();
        });
    }

    // 列显隐变化后重新分配可见列宽度：
    // setColumnHidden 不触发 resizeEvent，必须显式重排，否则右侧留空
    void relayoutColumns() {
        applyProportions();
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
        m_fixedColumn = -1; // 窗口缩放：所有列恢复等比
        applyProportions();
    }

private:
    void applyProportions() {
        if (m_applying || m_base.isEmpty())
            return;
        const int total = viewport()->width();
        if (total <= 0)
            return;
        // 刚被用户拖过的列保持宽度不动，其余可见列分配剩余空间
        int fixedW = 0;
        if (m_fixedColumn >= 0 && m_fixedColumn < m_base.size()
            && !isColumnHidden(m_fixedColumn)) {
            fixedW = columnWidth(m_fixedColumn);
        } else {
            m_fixedColumn = -1;
        }
        const int rest = total - fixedW;
        if (rest <= 0)
            return; // 固定列占满视口（极端拖动），交给横向滚动条

        int baseSum = 0;
        for (int c = 0; c < m_base.size(); ++c)
            if (!isColumnHidden(c) && c != m_fixedColumn) // 隐藏列与固定列不参与
                baseSum += m_base[c];
        if (baseSum <= 0)
            return;

        m_applying = true;
        int used = 0;
        int firstVisible = -1;
        for (int c = 0; c < m_base.size(); ++c) {
            if (isColumnHidden(c) || c == m_fixedColumn)
                continue;
            const int w = m_base[c] * rest / baseSum;
            setColumnWidth(c, w);
            used += w;
            if (firstVisible < 0)
                firstVisible = c;
        }
        if (firstVisible >= 0) // 余数给第一可见列，避免右侧留缝
            setColumnWidth(firstVisible, columnWidth(firstVisible) + (rest - used));
        m_applying = false;
    }

    QList<int> m_base;
    int m_fixedColumn = -1; // 用户刚拖动的列：保持其宽度，其余列填充剩余空间
    bool m_applying = false;
};

// 单行省略标签：长文本按宽度用「…」截断显示在一行，悬停可见全文
class ElidedLabel : public QLabel {
public:
    using QLabel::QLabel;

    // 注意：QLabel::setText 非虚函数，通过 ElidedLabel* 调用时静态绑定到此版本
    void setText(const QString &text) {
        m_fullText = text;
        updateElide();
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QLabel::resizeEvent(event);
        updateElide();
    }

private:
    void updateElide() {
        if (m_fullText.isEmpty() || width() <= 0) {
            QLabel::setText(m_fullText);
            setToolTip(QString());
            return;
        }
        const QFontMetrics fm(font());
        const QString elided = fm.elidedText(m_fullText, Qt::ElideRight, width());
        QLabel::setText(elided);
        setToolTip(elided != m_fullText ? m_fullText : QString());
    }

    QString m_fullText;
};

// 侧栏 delegate：抑制选中态背景绘制——侧栏不可聚焦，选中高亮会画成灰色；
// 激活列表改用自定义蓝色背景 + 白字标记，不受焦点状态影响。
// 右侧额外绘制歌曲数（Qt::UserRole）：激活项白色，其余灰色。
class SidebarDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        QStyleOptionViewItem opt = option;
        opt.state &= ~QStyle::State_Selected;
        QStyledItemDelegate::paint(painter, opt, index);

        const int count = index.data(Qt::UserRole).toInt();
        if (count <= 0)
            return;
        const bool active = opt.backgroundBrush.style() != Qt::NoBrush;
        painter->save();
        painter->setPen(active ? Qt::white : QColor(0x8E, 0x8E, 0x8E));
        painter->drawText(opt.rect.adjusted(0, 0, -6, 0),
                          Qt::AlignRight | Qt::AlignVCenter,
                          QString::number(count));
        painter->restore();
    }
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
    // 播放列表：name + 规范顺序 paths + 去重镜像 pathSet（两者同步维护）。
    // 声明在类体最前：成员函数的返回类型/参数不是完整类上下文，看不到后部声明。
    struct Playlist {
        QString name;
        QStringList paths;
        QSet<QString> pathSet;
        int lastRow = -1; // 离开该列表时的位置（内存记忆，切回时恢复选中/滚动）
    };

    // 表格元数据缓存：切换大列表时避免全量重读 TagLib（mtime 变化时重读）
    struct TrackMeta {
        QString title;
        QString album;
        unsigned int track = 0;
        int seconds = 0;
        int bitrate = 0;
        int sampleRate = 0;
        qint64 mtime = 0;
    };

public:
    PlayerWindow() {
        setWindowTitle(QStringLiteral("Music Player"));

        auto *central = new QWidget(this);
        auto *rootLayout = new QVBoxLayout(central);
        // 上半区：左侧播放列表栏 + （工具栏 + 歌曲表格）；下方"正在播放"信息区整行从左开始
        auto *topRow = new QHBoxLayout;
        auto *topContent = new QWidget(central);
        auto *layout = new QVBoxLayout(topContent);
        layout->setContentsMargins(0, 0, 0, 0);


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
        // 点击表头排序 + 右键表头勾选列显隐
        connect(m_playlist->horizontalHeader(), &QHeaderView::sectionClicked,
                this, &PlayerWindow::onHeaderSectionClicked);
        m_playlist->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_playlist->horizontalHeader(), &QWidget::customContextMenuRequested,
                this, &PlayerWindow::onHeaderMenu);
        // 竖向滚动条带当前播放位置标记
        m_scrollBar = new MarkerScrollBar(m_playlist);
        m_playlist->setVerticalScrollBar(m_scrollBar);
        // 右键菜单：移除选中 / 清空列表
        m_playlist->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_playlist, &QWidget::customContextMenuRequested, this,
                &PlayerWindow::showPlaylistMenu);
        // 列表标题行：左侧播放列表名，右侧歌曲总数与当前第几首
        auto *listHeader = new QHBoxLayout;
        m_listTitleLabel = new QLabel(central);
        QFont listTitleFont = m_listTitleLabel->font();
        listTitleFont.setBold(true);
        m_listTitleLabel->setFont(listTitleFont);
        m_listInfoLabel = new QLabel(central);
        m_listInfoLabel->setForegroundRole(QPalette::Mid);
        listHeader->addWidget(m_listTitleLabel);
        listHeader->addStretch();
        listHeader->addWidget(m_listInfoLabel);
        layout->addLayout(listHeader);

        // 搜索框：⌘F 唤起，输入即过滤（文件名/歌曲/专辑），清空自动隐藏
        m_searchEdit = new QLineEdit(central);
        m_searchEdit->setPlaceholderText(QStringLiteral("搜索：文件名 / 歌曲 / 专辑（⌘F）"));
        m_searchEdit->setClearButtonEnabled(true);
        m_searchEdit->setAcceptDrops(false); // 拖拽落到搜索框时冒泡给主窗口添加歌曲
        m_searchEdit->hide();
        connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
            const QString needle = text.trimmed();
            if (needle == m_filter)
                return;
            m_filter = needle;
            showActivePlaylist(); // 过滤后重建表格，行号语义与激活列表一致
            if (needle.isEmpty())
                m_searchEdit->hide(); // 清空（Esc/清除按钮）后自动收起
        });
        layout->addWidget(m_searchEdit);

        layout->addWidget(m_playlist, /*stretch=*/1);

        // 左侧栏：播放列表管理（右键菜单：新建列表 / 重命名 / 删除）。
        // 侧栏只与上方工具栏/表格同高，不再贯穿整窗
        m_sidebar = new QListWidget(central);
        m_sidebar->setFixedWidth(150);
        m_sidebar->setSelectionMode(QAbstractItemView::SingleSelection);
        m_sidebar->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_sidebar->setItemDelegate(new SidebarDelegate(m_sidebar)); // 选中态不画灰底
        // 不可聚焦：全局键盘过滤按表格语义拦截按键，侧栏聚焦会抢走按键
        m_sidebar->setFocusPolicy(Qt::NoFocus);
        m_sidebar->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_sidebar, &QWidget::customContextMenuRequested, this,
                &PlayerWindow::onSidebarMenu);
        // 用 itemClicked 而非 currentItemChanged：程序性重建侧栏不会误触发切换
        connect(m_sidebar, &QListWidget::itemClicked, this,
                &PlayerWindow::onSidebarItemClicked);
        // 行内编辑提交后同步列表名（程序性修改用守卫标志跳过）
        connect(m_sidebar, &QListWidget::itemChanged, this,
                &PlayerWindow::onSidebarItemChanged);

        topRow->addWidget(m_sidebar);
        topRow->addWidget(topContent, 1);
        rootLayout->addLayout(topRow, /*stretch=*/1);

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
        m_cover->setFixedSize(68, 68);
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

        m_title = new ElidedLabel(QStringLiteral("未加载文件"), central);
        m_title->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        // 单行显示：长歌名由 ElidedLabel 按宽度用「…」截断
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
        rootLayout->addLayout(middleLayout); // 整行：封面/歌手/歌词从最左侧开始

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
        rootLayout->addLayout(progressLayout);

        // 控制行：上一首 | 播放/暂停 | 下一首 | 音量
        auto *controls = new QHBoxLayout;
        m_prev = new QPushButton(QStringLiteral("◀◀"), central);
        m_prev->setToolTip(QStringLiteral("上一首"));
        m_prev->setEnabled(false);
        m_play = new QPushButton(QStringLiteral("播放"), central);
        m_play->setEnabled(false);
        // 播放/暂停主按钮：保持原生圆角与高度，仅左右宽度加宽到 150%
        m_play->setMinimumWidth(int(m_play->sizeHint().width() * 1.5));
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
        m_speedBox = new QComboBox(central);
        m_speedBox->addItems({QStringLiteral("0.5x"), QStringLiteral("0.75x"),
                              QStringLiteral("1.0x"), QStringLiteral("1.25x"),
                              QStringLiteral("1.5x"), QStringLiteral("2.0x")});
        m_speedBox->setToolTip(QStringLiteral("播放速度"));
        connect(m_speedBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int index) {
                    static const double kRates[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
                    m_player->setPlaybackRate(kRates[index]);
                });
        controls->addWidget(m_prev);
        controls->addWidget(m_play);
        controls->addWidget(m_next);
        controls->addStretch();
        controls->addWidget(m_speedBox);
        controls->addWidget(m_modeBox);
        controls->addWidget(volumeLabel);
        controls->addWidget(m_volume);
        rootLayout->addLayout(controls);

        setCentralWidget(central);
        setAcceptDrops(true); // 拖拽文件/文件夹到窗口添加歌曲
        resize(840, 640);

        // 播放器
        m_player = new QMediaPlayer(this);
        m_audio = new QAudioOutput(this);
        m_player->setAudioOutput(m_audio);

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

        // 睡眠定时：到时渐弱音量后暂停
        m_sleepTimer = new QTimer(this);
        m_sleepTimer->setSingleShot(true);
        connect(m_sleepTimer, &QTimer::timeout, this, &PlayerWindow::onSleepTimeout);
        m_fadeTimer = new QTimer(this);
        m_fadeTimer->setInterval(100); // 渐弱步进：每秒降 10%
        connect(m_fadeTimer, &QTimer::timeout, this, &PlayerWindow::onFadeTick);

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
        addToActivePlaylist(paths, QString());
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
        addToActivePlaylist(paths, dir);
    }

    // 添加到激活播放列表：按列表去重 + 记账 + 即时保存。
    // 若歌曲已在其他列表，状态栏提示（跨列表整理时避免重复加错）
    void addToActivePlaylist(const QStringList &paths, const QString &rootDir) {
        Playlist &pl = activePlaylist();
        QStringList crossList;
        int added = 0;
        for (const QString &path : paths) {
            if (pl.pathSet.contains(path))
                continue;
            bool inOther = false;
            for (int i = 0; i < m_playlists.size(); ++i) {
                if (i != m_activePlaylist && m_playlists[i].pathSet.contains(path)) {
                    inOther = true;
                    break;
                }
            }
            if (inOther)
                crossList << QFileInfo(path).fileName();
            appendRowForPath(path, rootDir);
            pl.paths << path;
            pl.pathSet.insert(path);
            ++added;
        }
        updateScrollMarker(); // 列表变长，当前播放的相对位置随之变化
        updateListHeader();
        refreshSidebar(); // 同步侧栏歌曲数
        savePlaylistsJson();
        saveMetaCache(); // 新解析的标签即时落盘，下次启动免重读
        if (!crossList.isEmpty())
            statusBar()->showMessage(
                QStringLiteral("已添加 %1 首；其中 %2 首已在其他列表：%3…")
                    .arg(added)
                    .arg(crossList.size())
                    .arg(crossList.mid(0, 5).join(QStringLiteral("、"))),
                8000);
    }

    // 路径 → 缓存元数据（缺失或 mtime 变化时解析标签）
    const TrackMeta &metaForPath(const QString &path) {
        const qint64 mtime = QFileInfo(path).lastModified().toSecsSinceEpoch();
        auto it = m_metaCache.find(path);
        if (it == m_metaCache.end() || it->mtime != mtime) {
            TrackMeta meta;
            meta.mtime = mtime;
            // 读一次标签，同时取歌曲名 / 专辑名 / 曲目编号 / 时长 / 码率 / 采样率
            const TagLib::FileRef ref(path.toUtf8().constData());
            if (!ref.isNull()) {
                if (ref.tag()) {
                    meta.title =
                        QString::fromStdWString(ref.tag()->title().toWString()).trimmed();
                    meta.album =
                        QString::fromStdWString(ref.tag()->album().toWString()).trimmed();
                    meta.track = ref.tag()->track();
                }
                if (ref.audioProperties()) {
                    const auto *ap = ref.audioProperties();
                    meta.seconds = ap->lengthInSeconds();
                    meta.bitrate = ap->bitrate();
                    meta.sampleRate = ap->sampleRate();
                }
            }
            it = m_metaCache.insert(path, meta);
        }
        return it.value();
    }

    // 搜索匹配：文件名 / 歌曲名 / 专辑（大小写不敏感）。
    // 文件名先匹配（无需解析标签），命中即返回，降低冷缓存首次搜索成本
    bool matchesFilter(const QString &path) {
        if (QFileInfo(path).fileName().contains(m_filter, Qt::CaseInsensitive))
            return true;
        const TrackMeta &meta = metaForPath(path);
        return meta.title.contains(m_filter, Qt::CaseInsensitive)
            || meta.album.contains(m_filter, Qt::CaseInsensitive);
    }

    // 纯行工厂：读一次标签生成 8 列行并追加到模型（不做去重与列表记账）。
    // 元数据按路径缓存（mtime 变化时重读），切换大列表时不再全量解析标签
    void appendRowForPath(const QString &path, const QString &rootDir) {
        const TrackMeta &meta = metaForPath(path);
        const QString &title = meta.title;
        QString album = meta.album;
        const unsigned int track = meta.track;
        const int seconds = meta.seconds;
        const int bitrate = meta.bitrate;
        const int sampleRate = meta.sampleRate;

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

    void removeSelected() {
        QList<int> rows;
        const QModelIndexList sel = m_playlist->selectionModel()->selectedRows();
        for (const QModelIndex &idx : sel)
            rows << idx.row();
        if (rows.isEmpty())
            return;
        std::sort(rows.begin(), rows.end(), std::greater<int>());
        snapshotForUndo(); // 支持 ⌘Z 撤销

        Playlist &pl = activePlaylist();
        int removedBelow = 0;
        bool removedCurrent = false;
        for (int row : rows) { // 行号降序；按路径移除（过滤态下表格行 ≠ 列表下标）
            const QString path =
                m_playlistModel->item(row, 0)->data(Qt::UserRole).toString();
            pl.paths.removeAll(path);
            pl.pathSet.remove(path);
            if (row < m_currentRow)
                ++removedBelow;
            else if (row == m_currentRow)
                removedCurrent = true;
            m_playlistModel->removeRow(row);
        }

        afterRowsRemoved(removedBelow, removedCurrent);
    }

    // 行移除后的公共收尾：当前行移位/停止、队列失效、侧栏与持久化
    void afterRowsRemoved(int removedBelow, bool removedCurrent) {
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
        updateListHeader();
        refreshSidebar(); // 同步侧栏歌曲数
        savePlaylistsJson();
    }

    // 把选中的歌曲从激活列表移动到目标列表（目标已有则跳过）
    void moveSelectedTo(int targetIndex) {
        if (targetIndex < 0 || targetIndex >= m_playlists.size()
            || targetIndex == m_activePlaylist)
            return;
        QList<int> rows;
        const QModelIndexList sel = m_playlist->selectionModel()->selectedRows();
        for (const QModelIndex &idx : sel)
            rows << idx.row();
        if (rows.isEmpty())
            return;
        std::sort(rows.begin(), rows.end(), std::greater<int>());
        snapshotForUndo(); // 支持 ⌘Z 撤销（恢复源列表）

        Playlist &src = activePlaylist();
        Playlist &dst = m_playlists[targetIndex];
        int moved = 0;
        int removedBelow = 0;
        bool removedCurrent = false;
        for (int row : rows) { // 行号降序；按路径移动
            const QString path =
                m_playlistModel->item(row, 0)->data(Qt::UserRole).toString();
            src.paths.removeAll(path);
            src.pathSet.remove(path);
            if (!dst.pathSet.contains(path)) {
                dst.paths << path;
                dst.pathSet.insert(path);
                ++moved;
            }
            if (row < m_currentRow)
                ++removedBelow;
            else if (row == m_currentRow)
                removedCurrent = true;
            m_playlistModel->removeRow(row);
        }
        afterRowsRemoved(removedBelow, removedCurrent);
        statusBar()->showMessage(
            QStringLiteral("已移动 %1 首到「%2」").arg(moved).arg(dst.name), 5000);
    }

    void clearPlaylist() {
        if (m_playlistModel->rowCount() > 0)
            snapshotForUndo(); // 支持 ⌘Z 撤销
        m_player->stop();
        m_playlistModel->setRowCount(0);
        activePlaylist().paths.clear();
        activePlaylist().pathSet.clear();
        m_currentRow = -1;
        resetLabels();
        updateScrollMarker();
        m_shuffleQueue.clear();
        m_history.clear();
        updateListHeader();
        refreshSidebar(); // 同步侧栏歌曲数
        savePlaylistsJson();
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
        QAction *searchAct = menu.addAction(QStringLiteral("查找（⌘F）"));
        menu.addSeparator();
        QAction *addFilesAct = menu.addAction(QStringLiteral("添加文件"));
        QAction *addDirAct = menu.addAction(QStringLiteral("添加文件夹"));
        QAction *importM3uAct = menu.addAction(QStringLiteral("导入 m3u"));
        menu.addSeparator();
        QAction *removeAct = menu.addAction(QStringLiteral("移除"));
        removeAct->setEnabled(m_playlist->selectionModel()->hasSelection());
        QMenu *moveMenu = menu.addMenu(QStringLiteral("移动到列表"));
        moveMenu->setEnabled(m_playlist->selectionModel()->hasSelection()
                             && m_playlists.size() > 1);
        for (int i = 0; i < m_playlists.size(); ++i)
            if (i != m_activePlaylist) // 不显示当前列表
                moveMenu->addAction(m_playlists[i].name)->setData(i);
        QAction *selectAllAct = menu.addAction(QStringLiteral("全选"));
        selectAllAct->setEnabled(n > 0 && !allSelected);
        QAction *removeMissingAct = menu.addAction(QStringLiteral("清理失效文件"));
        removeMissingAct->setEnabled(n > 0);
        QAction *undoAct = menu.addAction(QStringLiteral("撤销（⌘Z）"));
        undoAct->setEnabled(m_undoSnapshot.valid); // 移除/清空/排序/移动均可撤销
        QMenu *sleepMenu = menu.addMenu(QStringLiteral("睡眠定时"));
        QAction *sleepOffAct = sleepMenu->addAction(QStringLiteral("关闭"));
        QAction *sleep15Act = sleepMenu->addAction(QStringLiteral("15 分钟"));
        QAction *sleep30Act = sleepMenu->addAction(QStringLiteral("30 分钟"));
        QAction *sleep45Act = sleepMenu->addAction(QStringLiteral("45 分钟"));
        QAction *sleep60Act = sleepMenu->addAction(QStringLiteral("60 分钟"));
        QAction *sleep90Act = sleepMenu->addAction(QStringLiteral("90 分钟"));
        menu.addSeparator();
        QAction *exportM3uAct = menu.addAction(QStringLiteral("导出为 m3u"));
        exportM3uAct->setEnabled(n > 0);
        QAction *clearAct = menu.addAction(QStringLiteral("清空列表"));
        clearAct->setVisible(allSelected); // 未全选时不显示，防止误点

        const QAction *chosen = menu.exec(m_playlist->mapToGlobal(pos));
        // 菜单关闭后把焦点还给列表视图：macOS 上选中色随焦点状态渲染，
        // 焦点不在视图时蓝色框会退成灰色
        m_playlist->setFocus();
        if (chosen == addFilesAct)
            addFiles();
        else if (chosen == addDirAct)
            addDirectory();
        else if (chosen == searchAct)
            toggleSearch();
        else if (chosen == importM3uAct)
            importM3u();
        else if (chosen == exportM3uAct)
            exportM3u();
        else if (chosen && chosen->data().isValid()) // 移动到列表子菜单
            moveSelectedTo(chosen->data().toInt());
        else if (chosen == selectAllAct)
            m_playlist->selectAll();
        else if (chosen == removeAct)
            removeSelected();
        else if (chosen == removeMissingAct)
            removeMissingFiles();
        else if (chosen == undoAct)
            undoRemove();
        else if (chosen == sleepOffAct)
            cancelSleepTimer();
        else if (chosen == sleep15Act)
            startSleepTimer(15);
        else if (chosen == sleep30Act)
            startSleepTimer(30);
        else if (chosen == sleep45Act)
            startSleepTimer(45);
        else if (chosen == sleep60Act)
            startSleepTimer(60);
        else if (chosen == sleep90Act)
            startSleepTimer(90);
        else if (chosen == clearAct) {
            // 过滤态清空会移除未显示的歌曲，需确认防止误操作
            if (!m_filter.isEmpty()) {
                const int total = activePlaylist().paths.size();
                const auto ret = QMessageBox::question(
                    this, QStringLiteral("清空列表"),
                    QStringLiteral("当前处于过滤状态（显示 %1 / 共 %2 首）。\n"
                                   "清空列表将移除全部歌曲（含未显示），是否继续？")
                        .arg(n)
                        .arg(total));
                if (ret != QMessageBox::Yes)
                    return;
            }
            clearPlaylist();
        }
    }

    // ---------- 播放列表管理 ----------

    // 列表标题行：播放列表名 + 右侧「当前/总数」（如 107/303）。
    // 当前取正在播放行；跨列表播放无标记行时取当前选中行
    void updateListHeader() {
        const Playlist &pl = activePlaylist();
        m_listTitleLabel->setText(pl.name);
        const int n = m_playlistModel->rowCount();
        int shown = m_currentRow;
        if (shown < 0 && m_playlist->currentIndex().isValid())
            shown = m_playlist->currentIndex().row();
        m_listInfoLabel->setText(
            shown >= 0 ? QStringLiteral("%1/%2").arg(shown + 1).arg(n)
                       : QStringLiteral("共 %1 首").arg(n));
    }

    Playlist &activePlaylist() {
        Q_ASSERT(m_activePlaylist >= 0 && m_activePlaylist < m_playlists.size());
        return m_playlists[m_activePlaylist];
    }

    // 在表格中按路径定位行（首列 UserRole 存路径）
    int findRowForPath(const QString &path) const {
        if (path.isEmpty())
            return -1;
        for (int r = 0; r < m_playlistModel->rowCount(); ++r)
            if (m_playlistModel->item(r, 0)->data(Qt::UserRole).toString() == path)
                return r;
        return -1;
    }

    // 重建侧栏条目并选中激活列表（blockSignals：程序性操作不触发切换）
    void refreshSidebar() {
        m_updatingSidebar = true; // 程序性重建期间的 itemChanged 不处理
        m_sidebar->clear();
        for (const Playlist &pl : m_playlists) {
            QListWidgetItem *item = new QListWidgetItem(pl.name);
            item->setFlags(item->flags() | Qt::ItemIsEditable); // 行内编辑必需
            item->setData(Qt::UserRole, pl.paths.size()); // delegate 右侧显示歌曲数
            m_sidebar->addItem(item);
        }
        m_updatingSidebar = false;
        syncSidebarSelection();
        syncSidebarColors();
    }

    // 激活列表：蓝色背景 + 白色加粗文字（选中态由 delegate 抑制，蓝底不受焦点影响）
    void syncSidebarColors() {
        m_updatingSidebar = true;
        for (int i = 0; i < m_sidebar->count(); ++i) {
            QListWidgetItem *item = m_sidebar->item(i);
            if (i == m_activePlaylist) {
                QFont font = item->font();
                font.setBold(true);
                item->setFont(font);
                item->setForeground(QBrush(Qt::white));
                item->setBackground(QBrush(QColor(0, 102, 204)));
            } else {
                item->setData(Qt::FontRole, QVariant());
                item->setData(Qt::ForegroundRole, QVariant());
                item->setData(Qt::BackgroundRole, QVariant());
            }
        }
        m_updatingSidebar = false;
    }

    // 行内编辑提交：同步列表名（空名回退为原名）
    void onSidebarItemChanged(QListWidgetItem *item) {
        if (m_updatingSidebar || !item)
            return;
        const int i = m_sidebar->row(item);
        if (i < 0 || i >= m_playlists.size())
            return;
        const QString name = item->text().trimmed();
        if (name.isEmpty()) {
            m_updatingSidebar = true;
            item->setText(m_playlists[i].name);
            m_updatingSidebar = false;
            return;
        }
        m_playlists[i].name = name;
        if (i == m_activePlaylist)
            updateListHeader(); // 标题行同步新列表名
        savePlaylistsJson();
    }

    void syncSidebarSelection() {
        const QSignalBlocker blocker(m_sidebar);
        m_sidebar->setCurrentRow(m_activePlaylist);
    }

    // 用激活列表重建表格。不打断播放：正在播的歌在列表中则标记，
    // 不在则无标记（标题/封面/歌词保持），选中第一行便于回车直接播放。
    void showActivePlaylist() {
        m_playlistModel->setRowCount(0);
        m_markedRow = -1;
        m_currentRow = -1;
        m_shuffleQueue.clear(); // 行号在新的表格中已无意义
        m_history.clear();

        const Playlist &pl = activePlaylist();
        for (const QString &path : pl.paths) {
            if (!m_filter.isEmpty() && !matchesFilter(path))
                continue; // 搜索过滤态：只建匹配行
            appendRowForPath(path, QString()); // 无根目录语义：专辑回退显示所在文件夹名
        }
        updateScrollMarker();

        const int n = m_playlistModel->rowCount();
        if (n == 0) {
            m_playlist->clearSelection();
            updateListHeader();
            return;
        }
        const int row = findRowForPath(m_playingPath);
        if (row >= 0) {
            m_currentRow = row;
            markCurrentRow(row);
            updateScrollMarker();
            m_playlist->selectRow(row);
            m_playlist->scrollTo(m_playlistModel->index(row, 0),
                                 QAbstractItemView::EnsureVisible);
        } else {
            // 无播放行时回到上次离开时的位置（该列表记忆的行）
            const int last = activePlaylist().lastRow;
            const int target = (last >= 0 && last < n) ? last : 0;
            m_playlist->selectRow(target);
            m_playlist->setCurrentIndex(m_playlistModel->index(target, 0));
            m_playlist->scrollTo(m_playlistModel->index(target, 0),
                                 QAbstractItemView::EnsureVisible);
        }
        updateListHeader();
    }

    // 切换激活播放列表（点击当前列表时跳过重建，避免无谓的 TagLib 重读）
    void activatePlaylist(int i) {
        if (i < 0 || i >= m_playlists.size())
            return;
        const bool changed = (i != m_activePlaylist);
        if (changed) {
            // 离开当前列表前记住位置（正在播放行；无则选中行），切回时恢复
            Playlist &leaving = activePlaylist();
            leaving.lastRow = m_currentRow >= 0
                                  ? m_currentRow
                                  : (m_playlist->currentIndex().isValid()
                                         ? m_playlist->currentIndex().row()
                                         : -1);
        }
        m_activePlaylist = i;
        syncSidebarSelection();
        syncSidebarColors();
        if (changed)
            showActivePlaylist();
        savePlaylistsJson();
    }

    void onSidebarItemClicked(QListWidgetItem *item) {
        if (item)
            activatePlaylist(m_sidebar->row(item));
    }

    // 侧栏右键菜单：新建列表 / 重命名 / 删除（至少保留一个列表）
    void onSidebarMenu(const QPoint &pos) {
        // pos 是视图坐标，itemAt 需要视口坐标（与歌曲列表右键同样的转换）
        QListWidgetItem *item =
            m_sidebar->itemAt(m_sidebar->viewport()->mapFrom(m_sidebar, pos));
        if (item)
            m_sidebar->setCurrentItem(item); // 先选中右键目标（仅左键点击才切换列表）

        QMenu menu(this);
        QAction *addFilesAct = menu.addAction(QStringLiteral("添加文件"));
        QAction *addDirAct = menu.addAction(QStringLiteral("添加文件夹"));
        menu.addSeparator();
        QAction *newAct = menu.addAction(QStringLiteral("新建列表"));
        QAction *renameAct = menu.addAction(QStringLiteral("重命名"));
        renameAct->setEnabled(item != nullptr);
        QAction *deleteAct = menu.addAction(QStringLiteral("删除"));
        deleteAct->setEnabled(item != nullptr && m_playlists.size() > 1);

        const int index = m_sidebar->currentRow();
        const QAction *chosen = menu.exec(m_sidebar->mapToGlobal(pos));
        if (chosen == addFilesAct || chosen == addDirAct) {
            // 加到右键指向的列表：若指向的不是激活列表，先切换过去，
            // 添加结果立即可见（右键空白处则加到当前激活列表）
            if (index >= 0 && index != m_activePlaylist)
                activatePlaylist(index);
            if (chosen == addFilesAct)
                addFiles();
            else
                addDirectory();
        } else if (chosen == newAct)
            newPlaylist();
        else if (chosen == renameAct && index >= 0)
            renamePlaylist(index);
        else if (chosen == deleteAct && index >= 0)
            deletePlaylist(index);
    }

    // 「新建列表 N」：取第一个未占用的编号
    QString suggestNewPlaylistName() const {
        QSet<QString> names;
        for (const Playlist &pl : m_playlists)
            names.insert(pl.name);
        for (int i = 1;; ++i) {
            const QString name = QStringLiteral("新建列表 %1").arg(i);
            if (!names.contains(name))
                return name;
        }
    }

    // 新建列表：直接追加条目并进入行内编辑（不弹窗），名称在编辑提交时生效
    void newPlaylist() {
        const QString name = suggestNewPlaylistName();
        m_playlists.append({name, {}, {}});
        refreshSidebar(); // 追加侧栏条目
        activatePlaylist(m_playlists.size() - 1); // 新列表立即成为激活列表
        m_sidebar->editItem(m_sidebar->item(m_activePlaylist)); // 直接行内编辑
    }

    // 重命名：行内编辑，提交后经 onSidebarItemChanged 同步
    void renamePlaylist(int index) {
        if (index < 0 || index >= m_playlists.size())
            return;
        m_sidebar->editItem(m_sidebar->item(index));
    }

    void deletePlaylist(int index) {
        if (index < 0 || index >= m_playlists.size() || m_playlists.size() <= 1)
            return; // 至少保留一个列表
        const Playlist &pl = m_playlists[index];
        if (!pl.paths.isEmpty()) {
            const auto ret = QMessageBox::question(
                this, QStringLiteral("删除列表"),
                QStringLiteral("删除列表「%1」？其中 %2 首歌曲将从该列表移除（不影响文件）。")
                    .arg(pl.name)
                    .arg(pl.paths.size()));
            if (ret != QMessageBox::Yes)
                return;
        }
        const int wasActive = m_activePlaylist;
        m_playlists.removeAt(index);
        if (index == wasActive) {
            // 删除的是激活列表：回退到相邻列表并重建表格；播放不打断
            m_activePlaylist = qBound(0, index, int(m_playlists.size()) - 1);
            showActivePlaylist();
        } else {
            m_activePlaylist = index < wasActive ? wasActive - 1 : wasActive;
        }
        refreshSidebar();
        savePlaylistsJson();
    }

    // ---------- m3u 导入 / 导出 ----------

    // 导出激活列表为 m3u（本地绝对路径，foobar2000 等可直接读取）
    void exportM3u() {
        const Playlist &pl = activePlaylist();
        if (pl.paths.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("当前列表为空"), 3000);
            return;
        }
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("导出播放列表"),
            QDir::homePath() + QLatin1Char('/') + pl.name + QStringLiteral(".m3u"),
            QStringLiteral("M3U 播放列表 (*.m3u)"));
        if (path.isEmpty())
            return;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            statusBar()->showMessage(QStringLiteral("无法写入文件"), 3000);
            return;
        }
        QString content = QStringLiteral("#EXTM3U\n");
        for (const QString &p : pl.paths) {
            content += p;
            content += QLatin1Char('\n');
        }
        file.write(content.toUtf8());
        statusBar()->showMessage(
            QStringLiteral("已导出 %1 首到 %2").arg(pl.paths.size()).arg(path), 5000);
    }

    // 导入 m3u 到激活列表（过滤不存在的路径与非音频行）
    void importM3u() {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("导入播放列表"), QDir::homePath(),
            QStringLiteral("M3U 播放列表 (*.m3u *.m3u8)"));
        if (path.isEmpty())
            return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            statusBar()->showMessage(QStringLiteral("无法读取文件"), 3000);
            return;
        }
        QStringList paths;
        while (!file.atEnd()) {
            const QString line = QString::fromUtf8(file.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;
            if (QFileInfo::exists(line) && isAudioFile(line))
                paths << line;
        }
        if (paths.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("未找到有效的歌曲条目"), 3000);
            return;
        }
        std::sort(paths.begin(), paths.end(), pathLessThan);
        addToActivePlaylist(paths, QString());
    }

    // ---------- 搜索 / 排序 / 清理 / 撤销 / 睡眠定时 ----------

    // ⌘F：唤起/收起搜索框
    void toggleSearch() {
        if (m_searchEdit->isVisible()) {
            cancelSearch();
        } else {
            m_searchEdit->show();
            m_searchEdit->setFocus();
            m_searchEdit->selectAll();
        }
    }

    // Esc 取消搜索：清空过滤、收起搜索框、焦点回到列表继续键盘操作
    void cancelSearch() {
        m_searchEdit->clear(); // 触发清过滤（若还有内容）
        m_searchEdit->hide();
        m_playlist->setFocus();
    }

    void onHeaderSectionClicked(int column) {
        sortPlaylistByColumn(column);
    }

    // 点击表头排序：按列重排激活列表（同列再点反向）。
    // 注意：专辑播放模式依赖「同文件夹连续行」，任意重排后该语义会减弱，属预期取舍
    void sortPlaylistByColumn(int column) {
        Playlist &pl = activePlaylist();
        if (pl.paths.size() < 2)
            return;
        if (column == m_lastSortColumn)
            m_sortAscending = !m_sortAscending;
        else {
            m_lastSortColumn = column;
            m_sortAscending = true;
        }
        const bool asc = m_sortAscending;
        snapshotForUndo(); // 排序可撤销（⌘Z 恢复原顺序）
        const auto fileName = [](const QString &p) { return QFileInfo(p).fileName(); };
        std::sort(pl.paths.begin(), pl.paths.end(),
                  [&](const QString &a, const QString &b) {
            if (a == b)
                return false;
            bool less = false;
            switch (column) {
            case 0: // 文件名：自然排序
                less = pathLessThan(fileName(a), fileName(b));
                break;
            case 1: { // 专辑 → 序号 → 路径
                const TrackMeta &ma = metaForPath(a);
                const TrackMeta &mb = metaForPath(b);
                if (ma.album != mb.album) less = ma.album < mb.album;
                else if (ma.track != mb.track) less = ma.track < mb.track;
                else less = pathLessThan(a, b);
                break;
            }
            case 2: { // 序号 → 文件名
                const TrackMeta &ma = metaForPath(a);
                const TrackMeta &mb = metaForPath(b);
                if (ma.track != mb.track) less = ma.track < mb.track;
                else less = pathLessThan(fileName(a), fileName(b));
                break;
            }
            case 3: { // 歌曲名 → 路径
                const TrackMeta &ma = metaForPath(a);
                const TrackMeta &mb = metaForPath(b);
                if (ma.title != mb.title) less = ma.title < mb.title;
                else less = pathLessThan(a, b);
                break;
            }
            case 4: { // 文件大小 → 路径
                const qint64 sa = QFileInfo(a).size();
                const qint64 sb = QFileInfo(b).size();
                if (sa != sb) less = sa < sb;
                else less = pathLessThan(a, b);
                break;
            }
            case 5: { // 格式 → 路径
                const QString ea = QFileInfo(a).suffix();
                const QString eb = QFileInfo(b).suffix();
                if (ea != eb) less = ea < eb;
                else less = pathLessThan(a, b);
                break;
            }
            case 6: { // 音质（码率/采样率） → 路径
                const TrackMeta &ma = metaForPath(a);
                const TrackMeta &mb = metaForPath(b);
                const int qa = ma.bitrate * 1000 + ma.sampleRate;
                const int qb = mb.bitrate * 1000 + mb.sampleRate;
                if (qa != qb) less = qa < qb;
                else less = pathLessThan(a, b);
                break;
            }
            default: { // 时长 → 路径
                const TrackMeta &ma = metaForPath(a);
                const TrackMeta &mb = metaForPath(b);
                if (ma.seconds != mb.seconds) less = ma.seconds < mb.seconds;
                else less = pathLessThan(a, b);
                break;
            }
            }
            return asc ? less : !less;
        });
        m_shuffleQueue.clear();
        m_history.clear();
        showActivePlaylist();
        savePlaylistsJson();
        statusBar()->showMessage(
            QStringLiteral("已按「%1」%2排序")
                .arg(m_playlistModel->headerData(column, Qt::Horizontal).toString(),
                     asc ? QStringLiteral("升序") : QStringLiteral("降序")),
            3000);
    }

    // 表头右键菜单：勾选显示/隐藏列（持久化到 QSettings）
    void onHeaderMenu(const QPoint &pos) {
        QMenu menu(this);
        QVector<QAction *> acts;
        for (int c = 0; c < m_playlistModel->columnCount(); ++c) {
            QAction *act = menu.addAction(
                m_playlistModel->headerData(c, Qt::Horizontal).toString());
            act->setCheckable(true);
            act->setChecked(!m_playlist->isColumnHidden(c));
            acts << act;
        }
        const QAction *chosen =
            menu.exec(m_playlist->horizontalHeader()->mapToGlobal(pos));
        for (int c = 0; c < acts.size(); ++c) {
            if (chosen != acts[c])
                continue;
            m_playlist->setColumnHidden(c, !m_playlist->isColumnHidden(c));
            m_playlist->relayoutColumns(); // 立即重排可见列，填满整行
            QVariantList hiddenCols;
            for (int i = 0; i < m_playlistModel->columnCount(); ++i)
                if (m_playlist->isColumnHidden(i))
                    hiddenCols << i;
            QSettings().setValue(QStringLiteral("columns/hidden"), hiddenCols);
            break;
        }
    }

    // 一键移除文件已不存在的路径（清理失效文件）
    void removeMissingFiles() {
        Playlist &pl = activePlaylist();
        QStringList missing;
        for (const QString &p : pl.paths)
            if (!QFileInfo::exists(p))
                missing << p;
        if (missing.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("没有失效文件"), 3000);
            return;
        }
        snapshotForUndo(); // 支持 ⌘Z 撤销
        for (const QString &p : missing) {
            pl.paths.removeAll(p);
            pl.pathSet.remove(p);
        }
        showActivePlaylist();
        refreshSidebar();
        savePlaylistsJson();
        statusBar()->showMessage(
            QStringLiteral("已移除 %1 个失效文件").arg(missing.size()), 5000);
    }

    // 移除/清空前快照激活列表，⌘Z 可撤销恢复
    void snapshotForUndo() {
        m_undoSnapshot.playlist = m_activePlaylist;
        m_undoSnapshot.paths = activePlaylist().paths;
        m_undoSnapshot.valid = true;
    }

    void undoRemove() {
        if (!m_undoSnapshot.valid || m_undoSnapshot.playlist < 0
            || m_undoSnapshot.playlist >= m_playlists.size())
            return;
        Playlist &pl = m_playlists[m_undoSnapshot.playlist];
        pl.paths = m_undoSnapshot.paths;
        pl.pathSet = QSet<QString>(pl.paths.begin(), pl.paths.end());
        m_undoSnapshot.valid = false;
        if (m_undoSnapshot.playlist == m_activePlaylist)
            showActivePlaylist();
        refreshSidebar();
        savePlaylistsJson();
        statusBar()->showMessage(QStringLiteral("已撤销"), 3000);
    }

    // 睡眠定时：到时 10 秒内渐弱音量后暂停，随后恢复原音量
    void startSleepTimer(int minutes) {
        m_fadeTimer->stop();
        m_sleepTimer->start(minutes * 60 * 1000);
        statusBar()->showMessage(
            QStringLiteral("睡眠定时：%1 分钟后渐弱停止").arg(minutes), 5000);
    }

    void cancelSleepTimer() {
        m_sleepTimer->stop();
        if (m_fadeTimer->isActive()) {
            m_fadeTimer->stop();
            m_volume->setValue(m_sleepVolumeBefore); // 渐弱中途取消则恢复音量
        }
        statusBar()->showMessage(QStringLiteral("睡眠定时已取消"), 3000);
    }

    void onSleepTimeout() {
        m_sleepVolumeBefore = m_volume->value();
        if (m_sleepVolumeBefore <= 0) {
            m_player->pause();
            statusBar()->showMessage(QStringLiteral("睡眠定时：已暂停播放"), 5000);
            return;
        }
        m_fadeTimer->start();
    }

    void onFadeTick() {
        if (m_volume->value() > 0) {
            m_volume->setValue(m_volume->value() - 1);
            return;
        }
        m_fadeTimer->stop();
        m_player->pause();
        m_volume->setValue(m_sleepVolumeBefore);
        statusBar()->showMessage(QStringLiteral("睡眠定时：已暂停播放"), 5000);
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
        updateListHeader();
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
        m_playingPath = path; // 跨列表播放锚点：切换列表后据此重定位标记行
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
        // 播放中的歌不在当前列表时仍可暂停/继续；
        // 仅在完全停止且无当前行时才忽略（避免复活已清空的歌曲）
        const QMediaPlayer::PlaybackState state = m_player->playbackState();
        if (state == QMediaPlayer::StoppedState && m_currentRow < 0)
            return;
        if (m_currentRow >= 0)
            m_playlist->selectRow(m_currentRow); // 播放/暂停时选中态也跟随当前歌曲
        if (state == QMediaPlayer::PlayingState)
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

    // 关窗时兜底保存状态：macOS 上关窗可能不触发 aboutToQuit（应用驻留 Dock），
    // 在 closeEvent 里保存，确保下次启动能恢复当前播放列表
    void closeEvent(QCloseEvent *event) override {
        saveState();
        QMainWindow::closeEvent(event);
    }

    // 拖拽添加：接受 Finder 拖入的文件与文件夹（递归收集音频文件）
    void dragEnterEvent(QDragEnterEvent *event) override {
        if (event->mimeData()->hasUrls())
            event->acceptProposedAction();
        else
            QMainWindow::dragEnterEvent(event);
    }

    void dropEvent(QDropEvent *event) override {
        QStringList paths;
        const QList<QUrl> urls = event->mimeData()->urls();
        for (const QUrl &url : urls) {
            const QString path = url.toLocalFile();
            if (path.isEmpty())
                continue;
            const QFileInfo info(path);
            if (info.isDir()) {
                QDirIterator it(path, kAudioSuffixes,
                                QDir::Files | QDir::Readable,
                                QDirIterator::Subdirectories);
                while (it.hasNext())
                    paths << it.next();
            } else if (isAudioFile(path)) {
                paths << path;
            }
        }
        if (paths.isEmpty())
            return;
        std::sort(paths.begin(), paths.end(), pathLessThan);
        addToActivePlaylist(paths, QString());
        event->acceptProposedAction();
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
        // ⌘F 搜索、⌘Z 撤销、⌘1-9 切列表：即使焦点在行内编辑框也优先处理
        if (mods == Qt::ControlModifier) {
            if (ke->key() == Qt::Key_F) {
                toggleSearch();
                return true;
            }
            if (ke->key() == Qt::Key_Z) {
                undoRemove();
                return true;
            }
            if (ke->key() >= Qt::Key_1 && ke->key() <= Qt::Key_9) {
                activatePlaylist(ke->key() - Qt::Key_1); // 越界时函数内忽略
                return true;
            }
        }
        // 搜索框聚焦时 Esc 取消搜索；其余行内编辑（侧栏列表名/搜索框输入）
        // 放行按键：保证文本输入与中文输入法正常
        if (qobject_cast<QLineEdit *>(QApplication::focusWidget())) {
            if (ke->key() == Qt::Key_Escape
                && QApplication::focusWidget() == m_searchEdit) {
                cancelSearch();
                return true;
            }
            return false;
        }
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
        if (status == QMediaPlayer::LoadedMedia) {
            m_errorSkipCount = 0; // 成功加载后清零自动跳过计数
            if (m_pendingPosition > 0) {
                // 恢复上次播放位置（需等媒体加载完成）
                m_player->setPosition(m_pendingPosition);
                m_pendingPosition = 0;
            }
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
            QStringLiteral("无法播放「%1」：%2，自动跳过").arg(name, detail), 5000);
        // 自动跳过损坏文件；连续失败达上限则停止（防止整列表坏文件死循环）
        if (m_errorSkipCount < 5) {
            const int next = nextRowFor();
            if (next >= 0) {
                ++m_errorSkipCount;
                playRow(next);
                return;
            }
        }
        m_errorSkipCount = 0;
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

    // 专辑 = 同一父文件夹的所有行（按表格顺序返回行号列表）。
    // 不依赖「同文件夹连续行」：任意排序/过滤状态下专辑语义均成立
    QList<int> albumRows(int row) const {
        const auto dirOf = [this](int r) {
            return QFileInfo(m_playlistModel->item(r, 0)->data(Qt::UserRole).toString())
                .dir()
                .path();
        };
        QList<int> rows;
        const QString dir = dirOf(row);
        for (int r = 0; r < m_playlistModel->rowCount(); ++r)
            if (dirOf(r) == dir)
                rows << r;
        return rows;
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
            // 表中当前行之后的下一个同目录行；没有则回该目录第一行
            const QList<int> rows = albumRows(m_currentRow);
            for (int r : rows)
                if (r > m_currentRow)
                    return r;
            return rows.isEmpty() ? -1 : rows.first();
        }
        case kModeAlbumShuffle: {
            const QList<int> rows = albumRows(m_currentRow);
            if (rows.size() <= 1)
                return -1; // 单曲专辑无从随机，停止
            int r = m_currentRow;
            while (r == m_currentRow)
                r = rows[int(QRandomGenerator::global()->bounded(rows.size()))];
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
            // 表中当前行之前最近的同目录行；没有则回该目录最后一行
            const QList<int> rows = albumRows(m_currentRow);
            int prev = -1;
            for (int r : rows)
                if (r < m_currentRow)
                    prev = r;
            return prev >= 0 ? prev : (rows.isEmpty() ? -1 : rows.last());
        }
        case kModeAlbumShuffle: {
            const QList<int> rows = albumRows(m_currentRow);
            if (rows.size() <= 1)
                return -1;
            int r = m_currentRow;
            while (r == m_currentRow)
                r = rows[int(QRandomGenerator::global()->bounded(rows.size()))];
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

    // ---------- 元数据缓存持久化 ----------

    // 持久化表格元数据缓存：启动时免于全量重读 TagLib。
    // 只序列化仍在某个播放列表中的路径，保持文件有界
    void saveMetaCache() {
        QSet<QString> live;
        for (const Playlist &pl : m_playlists)
            live.unite(pl.pathSet);

        QJsonArray arr;
        for (auto it = m_metaCache.constBegin(); it != m_metaCache.constEnd(); ++it) {
            if (!live.contains(it.key()))
                continue;
            const TrackMeta &m = it.value();
            QJsonObject obj;
            obj.insert(QStringLiteral("path"), it.key());
            obj.insert(QStringLiteral("mtime"), double(m.mtime));
            obj.insert(QStringLiteral("title"), m.title);
            obj.insert(QStringLiteral("album"), m.album);
            obj.insert(QStringLiteral("track"), int(m.track));
            obj.insert(QStringLiteral("seconds"), m.seconds);
            obj.insert(QStringLiteral("bitrate"), m.bitrate);
            obj.insert(QStringLiteral("sampleRate"), m.sampleRate);
            arr.append(obj);
        }
        QJsonObject root;
        root.insert(QStringLiteral("tracks"), arr);

        QSaveFile file(metaCacheFilePath());
        if (!file.open(QIODevice::WriteOnly))
            return;
        file.write(QJsonDocument(root).toJson());
        file.commit();
    }

    void loadMetaCache() {
        QFile file(metaCacheFilePath());
        if (!file.open(QIODevice::ReadOnly))
            return;
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            return;
        for (const QJsonValue &v : doc.object().value(QStringLiteral("tracks")).toArray()) {
            const QJsonObject obj = v.toObject();
            TrackMeta m;
            m.mtime = qint64(obj.value(QStringLiteral("mtime")).toDouble());
            m.title = obj.value(QStringLiteral("title")).toString();
            m.album = obj.value(QStringLiteral("album")).toString();
            m.track = unsigned(obj.value(QStringLiteral("track")).toInt());
            m.seconds = obj.value(QStringLiteral("seconds")).toInt();
            m.bitrate = obj.value(QStringLiteral("bitrate")).toInt();
            m.sampleRate = obj.value(QStringLiteral("sampleRate")).toInt();
            const QString path = obj.value(QStringLiteral("path")).toString();
            if (!path.isEmpty())
                m_metaCache.insert(path, m);
        }
    }

    // ---------- 播放列表持久化（JSON） ----------

    // 每次列表/激活索引变更立即保存（原子写）；退出时 saveState 兜底再存一次
    void savePlaylistsJson() {
        QJsonObject root;
        root.insert(QStringLiteral("active"), m_activePlaylist);
        QJsonArray arr;
        for (const Playlist &pl : m_playlists) {
            QJsonObject obj;
            obj.insert(QStringLiteral("name"), pl.name);
            obj.insert(QStringLiteral("paths"), QJsonArray::fromStringList(pl.paths));
            arr.append(obj);
        }
        root.insert(QStringLiteral("playlists"), arr);

        const QString filePath = playlistsFilePath();
        QDir().mkpath(QFileInfo(filePath).absolutePath());
        QSaveFile file(filePath);
        if (!file.open(QIODevice::WriteOnly))
            return;
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.commit();
    }

    // 载入失败（文件不存在/损坏）返回 false，由调用方走迁移或兜底
    bool loadPlaylistsJson() {
        QFile file(playlistsFilePath());
        if (!file.open(QIODevice::ReadOnly))
            return false;
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            return false;

        const QJsonObject root = doc.object();
        const QJsonArray arr = root.value(QStringLiteral("playlists")).toArray();
        if (arr.isEmpty())
            return false;
        m_playlists.clear();
        for (const QJsonValue &v : arr) {
            const QJsonObject obj = v.toObject();
            Playlist pl;
            pl.name = obj.value(QStringLiteral("name")).toString().trimmed();
            if (pl.name.isEmpty())
                pl.name = QStringLiteral("默认列表");
            for (const QJsonValue &pv :
                 obj.value(QStringLiteral("paths")).toArray()) {
                const QString path = pv.toString();
                if (path.isEmpty() || !QFileInfo::exists(path))
                    continue; // 已失效路径直接过滤
                pl.paths << path;
                pl.pathSet.insert(path);
            }
            m_playlists.append(pl);
        }
        if (m_playlists.isEmpty())
            return false;
        m_activePlaylist = qBound(
            0, root.value(QStringLiteral("active")).toInt(-1),
            int(m_playlists.size()) - 1);
        return true;
    }

    // 首次运行：迁移旧版 QSettings 播放列表到「默认列表」；否则新建空默认列表
    void migrateOrSeedPlaylists() {
        if (loadPlaylistsJson())
            return;
        QSettings settings;
        QStringList existing;
        for (const QString &p :
             settings.value(QStringLiteral("playlist/paths")).toStringList())
            if (QFileInfo::exists(p))
                existing << p;
        Playlist pl{QStringLiteral("默认列表"), existing,
                    QSet<QString>(existing.begin(), existing.end())};
        m_playlists.clear();
        m_playlists.append(pl);
        m_activePlaylist = 0;
        settings.remove(QStringLiteral("playlist/paths")); // 迁移后删除旧 key
        savePlaylistsJson();
    }

    // ---------- 状态记忆 ----------

    void saveState() {
        QSettings settings;
        settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
        settings.setValue(QStringLiteral("volume"), m_volume->value());

        // 保存 m_playingPath 而非当前行路径：播放中的歌可能在非激活列表
        // （切换列表后 currentRow == -1），按行保存会丢失
        settings.setValue(QStringLiteral("playback/path"), m_playingPath);
        settings.setValue(QStringLiteral("playback/position"),
                          m_playingPath.isEmpty() ? 0 : m_player->position());
        settings.setValue(QStringLiteral("playback/mode"), m_mode);
        settings.setValue(QStringLiteral("playback/speed"), m_speedBox->currentIndex());
        savePlaylistsJson(); // 退出兜底保存（平时每次变更即时保存）
        saveMetaCache();     // 元数据缓存兜底保存
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

        const int speed = settings.value(QStringLiteral("playback/speed"), 2).toInt();
        m_speedBox->setCurrentIndex(qBound(0, speed, m_speedBox->count() - 1));

        // 恢复列显隐
        const QVariantList hiddenCols =
            settings.value(QStringLiteral("columns/hidden")).toList();
        for (const QVariant &v : hiddenCols) {
            const int c = v.toInt();
            if (c >= 0 && c < m_playlistModel->columnCount())
                m_playlist->setColumnHidden(c, true);
        }

        // 播放列表：JSON 持久化，旧版 QSettings 列表一次性迁移
        migrateOrSeedPlaylists();
        loadMetaCache(); // 先加载元数据缓存，避免启动时全量重读标签
        refreshSidebar();
        showActivePlaylist();

        // 恢复上次播放的歌曲：先切到它所在的播放列表（若不在激活列表），
        // 再定位行与播放位置——上次的列表内容与歌曲一起恢复
        const QString rowPath =
            settings.value(QStringLiteral("playback/path")).toString();
        const qint64 position =
            settings.value(QStringLiteral("playback/position"), 0).toLongLong();
        if (!rowPath.isEmpty()) {
            for (int i = 0; i < m_playlists.size(); ++i) {
                if (m_playlists[i].pathSet.contains(rowPath)) {
                    if (i != m_activePlaylist)
                        activatePlaylist(i); // 切到歌曲所在列表（重建表格并保存）
                    break;
                }
            }
        }
        const int row = findRowForPath(rowPath);
        if (row >= 0) {
            m_currentRow = row;
            markCurrentRow(row);
            updateScrollMarker();
            m_playlist->selectRow(row);
            m_playlist->scrollTo(m_playlistModel->index(row, 0),
                                 QAbstractItemView::EnsureVisible);
        }
        if (!rowPath.isEmpty() && QFileInfo::exists(rowPath)) {
            // 无论歌曲在不在当前列表都恢复上次曲目：
            // 不在任何列表时无标记行，与跨列表播放状态一致
            loadTrack(rowPath);
            m_pendingPosition = position; // 媒体加载完成后定位（见 onMediaStatusChanged）
        }
        updateListHeader();
        saveMetaCache(); // 启动期新解析的标签立即落盘
    }

    void resetLabels() {
        m_playingPath.clear();
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
    QLabel *m_listTitleLabel = nullptr; // 列表标题行：播放列表名
    QLabel *m_listInfoLabel = nullptr;  // 列表标题行右侧：歌曲总数与当前第几首
    MarkerScrollBar *m_scrollBar = nullptr;
    QListWidget *m_sidebar = nullptr; // 左侧栏：播放列表条目
    bool m_updatingSidebar = false;   // 程序性修改侧栏期间跳过 itemChanged 处理
    QLabel *m_cover = nullptr;
    QLabel *m_artist = nullptr;
    ElidedLabel *m_title = nullptr; // 单行省略显示长歌名
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
    QComboBox *m_speedBox = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audio = nullptr;
    QVector<Playlist> m_playlists; // 全部播放列表，恒 >= 1 个
    QHash<QString, TrackMeta> m_metaCache; // 路径 → 表格元数据缓存
    int m_activePlaylist = 0;      // 当前激活列表索引
    QString m_playingPath; // 已加载媒体的路径（跨列表播放锚点）；loadTrack 设置、resetLabels 清除
    QTimer *m_idleTimer = nullptr; // 选中/滚动空闲回位计时器
    QTimer *m_sleepTimer = nullptr; // 睡眠定时（单次）
    QTimer *m_fadeTimer = nullptr;  // 睡眠渐弱步进
    int m_sleepVolumeBefore = 0;    // 渐弱前的音量，用于恢复
    QString m_filter;               // 搜索过滤词（空 = 不过滤）
    int m_lastSortColumn = -1;      // 上次排序的列（同列再点反向）
    bool m_sortAscending = true;
    int m_errorSkipCount = 0;       // 连续播放失败计数（自动跳过防死循环）
    struct UndoSnapshot {           // 移除/清空前的列表快照，⌘Z 撤销
        int playlist = -1;
        QStringList paths;
        bool valid = false;
    } m_undoSnapshot;
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
    app.setQuitOnLastWindowClosed(true); // 关窗即退出（closeEvent 已保存状态），避免驻留 Dock 产生双实例
    PlayerWindow w;
    w.show();
    return app.exec();
}
