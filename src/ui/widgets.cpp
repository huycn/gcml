#include "widgets.h"

#include "core/trace2.h"

#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QFile>
#include <QKeyEvent>
#include <QPainter>
#include <QPointer>
#include <QRegularExpressionValidator>
#include <QScreen>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QSvgRenderer>
#include <QTabWidget>
#include <QTimer>

namespace gcm::ui {

// =============================================================================================
// Theme
// =============================================================================================

namespace {

// UI/Assets/Base.axaml + Images.axaml
const QColor LightWindow(0xF6, 0xF6, 0xF6);
const QColor DarkWindow(0x28, 0x28, 0x28);
const QColor LightIcon(0x25, 0x29, 0x2F);
// clazy:exclude-next-line=non-pod-global-static
const QColor DarkIcon(Qt::white);
const QColor LightWarning(0xFF, 0x7F, 0x27);
const QColor DarkWarning(0xFF, 0xD4, 0x0F);

QPalette darkPalette()
{
    QPalette p;
    const QColor text(0xF0, 0xF0, 0xF0);
    const QColor disabledText(0x80, 0x80, 0x80);
    const QColor base(0x1E, 0x1E, 0x1E);
    const QColor button(0x3A, 0x3A, 0x3A);
    const QColor highlight(0x4C, 0xA2, 0xE6);
    p.setColor(QPalette::Window, DarkWindow);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, DarkWindow);
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, QColor(0x9A, 0x9A, 0x9A));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::red);
    p.setColor(QPalette::Light, QColor(0x50, 0x50, 0x50));
    p.setColor(QPalette::Midlight, QColor(0x44, 0x44, 0x44));
    p.setColor(QPalette::Mid, QColor(0x30, 0x30, 0x30));
    p.setColor(QPalette::Dark, QColor(0x18, 0x18, 0x18));
    p.setColor(QPalette::Shadow, Qt::black);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::black);
    p.setColor(QPalette::Link, QColor(0x99, 0xEB, 0xFF));
    p.setColor(QPalette::LinkVisited, QColor(0xC0, 0xA0, 0xFF));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    p.setColor(QPalette::Accent, highlight);
#endif
    for (QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        p.setColor(QPalette::Disabled, role, disabledText);
    }
    p.setColor(QPalette::Disabled, QPalette::Button, QColor(0x30, 0x30, 0x30));
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x50, 0x50, 0x50));
    return p;
}

QPalette lightPalette()
{
    const std::unique_ptr<QStyle> fusion(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette p = fusion ? fusion->standardPalette() : QPalette();
    p.setColor(QPalette::Window, LightWindow);
    p.setColor(QPalette::Highlight, QColor(0x00, 0x5F, 0xB8));
    p.setColor(QPalette::HighlightedText, Qt::white);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    p.setColor(QPalette::Accent, QColor(0x00, 0x5F, 0xB8));
#endif
    p.setColor(QPalette::Link, QColor(0x00, 0x5F, 0xB8));
    return p;
}

bool paletteIsDark(const QPalette &p)
{
    return p.color(QPalette::Window).lightness() < 128;
}

QByteArray readResource(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

void applyTheme(QApplication &app, bool dark)
{
    // Fusion honours the palette fully; the platform style may draw with its own (light) colours.
    if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        app.setStyle(fusion);
    }
    app.setPalette(dark ? darkPalette() : lightPalette());
}

void applySystemTheme(QApplication &app)
{
    const QString forced = qEnvironmentVariable("GCM_UI_THEME").trimmed().toLower();
    if (forced == QLatin1String("dark") || forced == QLatin1String("light")) {
        applyTheme(app, forced == QLatin1String("dark"));
        return;
    }
    const bool systemDark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    if (systemDark != paletteIsDark(app.palette())) {
        applyTheme(app, systemDark);
    }
}

bool isDarkTheme()
{
    return paletteIsDark(QApplication::palette());
}

QPixmap themedPixmap(const QString &name, int height, int width)
{
    const QString path = QStringLiteral(":/gcm/") + name;
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;

    if (!name.endsWith(QLatin1String(".svg"))) {
        QPixmap source(path);
        if (source.isNull()) {
            return source;
        }
        const QSize logical = width > 0 ? QSize(width, height) : QSize(qRound(height * qreal(source.width()) / source.height()), height);
        QPixmap scaled = source.scaled(logical * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);
        return scaled;
    }

    QByteArray svg = readResource(path);
    const bool dark = isDarkTheme();
    svg.replace("#25292F", (dark ? DarkIcon : LightIcon).name().toLatin1());
    svg.replace("#FF7F27", (dark ? DarkWarning : LightWarning).name().toLatin1());
    QSvgRenderer renderer(svg);
    const QSizeF native = renderer.viewBoxF().size();
    const QSize logical = width > 0 ? QSize(width, height)
                                    : QSize(qRound(native.isEmpty() ? height : height * native.width() / native.height()), height);
    QPixmap pixmap(logical * dpr);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        // Stretch="Uniform": keep the aspect ratio, centred
        QSizeF target = native.scaled(QSizeF(pixmap.size()), Qt::KeepAspectRatio);
        const QRectF rect((pixmap.width() - target.width()) / 2, (pixmap.height() - target.height()) / 2, target.width(), target.height());
        renderer.render(&painter, rect);
    }
    pixmap.setDevicePixelRatio(dpr);
    return pixmap;
}

QFont fontWithPixelSize(const QFont &base, int pixelSize, QFont::Weight weight)
{
    QFont f(base);
    if (pixelSize > 0) {
        f.setPixelSize(pixelSize);
    }
    f.setWeight(weight);
    return f;
}

// =============================================================================================
// Widgets
// =============================================================================================

ImageLabel::ImageLabel(QString name, int height, int width, QWidget *parent)
    : QLabel(parent), m_name(std::move(name)), m_height(height), m_width(width)
{
    setAlignment(Qt::AlignCenter);
    render();
}

void ImageLabel::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange) {
        render();
    }
    QLabel::changeEvent(event);
}

void ImageLabel::render()
{
    const QPixmap pixmap = themedPixmap(m_name, m_height, m_width);
    setPixmap(pixmap);
    setFixedSize(pixmap.deviceIndependentSize().toSize());
}

LinkButton::LinkButton(const QString &text, QWidget *parent) : QPushButton(text, parent)
{
    setFlat(true);
    setAutoDefault(false);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setStyleSheet(QStringLiteral("QPushButton { border: none; background: transparent; padding: 0px; color: palette(link); }"
                                 "QPushButton:disabled { color: palette(mid); }"));
}

void LinkButton::enterEvent(QEnterEvent *event)
{
    QFont f = font();
    f.setUnderline(true);
    setFont(f);
    QPushButton::enterEvent(event);
}

void LinkButton::leaveEvent(QEvent *event)
{
    QFont f = font();
    f.setUnderline(false);
    setFont(f);
    QPushButton::leaveEvent(event);
}

QPushButton *makeAccentButton(const QString &text, QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    button->setProperty("gcmAccent", true);
    button->setProperty("gcmDefault", true);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    // Fluent "accent" buttons: accent background, contrasting text. Disabled buttons keep the normal look.
    QPalette p = button->palette();
    for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive}) {
        p.setColor(group, QPalette::Button, p.color(group, QPalette::Highlight));
        p.setColor(group, QPalette::ButtonText, p.color(group, QPalette::HighlightedText));
    }
    button->setPalette(p);
    return button;
}

QWidget *makeProductHeader(QWidget *parent)
{
    auto *header = new QWidget(parent);
    header->setObjectName(QStringLiteral("productHeader"));
    auto *layout = new QHBoxLayout(header);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    layout->addStretch();
    layout->addWidget(new ImageLabel(QStringLiteral("gcm-logo.svg"), 32, 32, header));
    auto *text = new QLabel(QStringLiteral("Git Credential Manager"), header);
    text->setFont(fontWithPixelSize(text->font(), 18, QFont::Light));
    layout->addWidget(text);
    layout->addStretch();
    return header;
}

QLabel *makeTextBlock(const QString &text, int pixelSize, QFont::Weight weight, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setFont(fontWithPixelSize(label->font(), pixelSize, weight));
    label->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    return label;
}

void addCentered(QBoxLayout *layout, QWidget *widget)
{
    layout->addWidget(widget, 0, Qt::AlignHCenter);
}

QLineEdit *makeTextEdit(const QString &placeholder, QWidget *parent)
{
    auto *edit = new QLineEdit(parent);
    edit->setPlaceholderText(placeholder);
    edit->setMinimumHeight(32);
    return edit;
}

QLineEdit *makeSecretEdit(const QString &placeholder, QWidget *parent)
{
    QLineEdit *edit = makeTextEdit(placeholder, parent);
    edit->setEchoMode(QLineEdit::Password);
    return edit;
}

ShadowDivider::ShadowDivider(QWidget *parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(4);
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

QSize ShadowDivider::sizeHint() const
{
    return {100, 4};
}

void ShadowDivider::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    // A soft shadow under the line: radial gradient black -> transparent, 25 % opacity, clipped to the lower half
    QRadialGradient gradient(QPointF(width() / 2.0, 0), width() / 2.0);
    gradient.setColorAt(0, QColor(0, 0, 0, 64));
    gradient.setColorAt(1, Qt::transparent);
    painter.fillRect(QRect(0, 1, width(), 3), gradient);
    painter.fillRect(QRect(0, 0, width(), 1), QColor(Qt::darkGray));
}

SixDigitInput::SixDigitInput(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    const auto *validator = new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9]")), this);
    for (int i = 0; i < 6; ++i) {
        auto *box = new QLineEdit(this);
        box->setObjectName(QStringLiteral("digit%1").arg(i + 1));
        box->setFixedSize(36, 46);
        box->setMaxLength(1);
        box->setAlignment(Qt::AlignCenter);
        box->setFont(fontWithPixelSize(box->font(), 20));
        box->setValidator(validator);
        box->installEventFilter(this);
        connect(box, &QLineEdit::textChanged, this, [this] {
            if (!m_ignoreUpdate) {
                Q_EMIT textChanged(text());
            }
        });
        connect(box, &QLineEdit::textEdited, this, [this](const QString &value) {
            if (!value.isEmpty()) {
                moveFocus(true);
            }
        });
        layout->addWidget(box);
        m_boxes.append(box);
    }
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

QString SixDigitInput::text() const
{
    QString result;
    for (const QLineEdit *box : m_boxes) {
        result += box->text();
    }
    return result;
}

void SixDigitInput::setText(const QString &text)
{
    // Upstream pads with spaces (so a short paste would count as six characters) and throws on more than six
    // digits; here only the first six digits are used and missing ones leave the box empty.
    QString digits;
    for (const QChar c : text) {
        if (c.isDigit() && digits.size() < 6) {
            digits += c;
        }
    }
    m_ignoreUpdate = true;
    for (int i = 0; i < m_boxes.size(); ++i) {
        m_boxes[i]->setText(i < digits.size() ? QString(digits[i]) : QString());
    }
    m_ignoreUpdate = false;
    Q_EMIT textChanged(this->text());
}

void SixDigitInput::setFocusToFirst()
{
    m_boxes.constFirst()->setFocus(Qt::TabFocusReason);
}

void SixDigitInput::moveFocus(bool next)
{
    const auto *focused = qobject_cast<QLineEdit *>(QApplication::focusWidget());
    const qsizetype index = m_boxes.indexOf(const_cast<QLineEdit *>(focused));
    if (index < 0) {
        return;
    }
    const qsizetype target = next ? std::min<qsizetype>(m_boxes.size() - 1, index + 1) : std::max<qsizetype>(0, index - 1);
    m_boxes[target]->setFocus(Qt::TabFocusReason);
}

bool SixDigitInput::eventFilter(QObject *watched, QEvent *event)
{
    auto *box = qobject_cast<QLineEdit *>(watched);
    if (!box || event->type() != QEvent::KeyPress) {
        return QWidget::eventFilter(watched, event);
    }
    auto *key = static_cast<QKeyEvent *>(event);
    if (key->matches(QKeySequence::Paste)) {
        setText(QGuiApplication::clipboard()->text());
        return true;
    }
    switch (key->key()) {
    case Qt::Key_Right:
        moveFocus(true);
        return true;
    case Qt::Key_Left:
        moveFocus(false);
        return true;
    case Qt::Key_Backspace:
        moveFocus(false);
        box->setText(QString());
        return true;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
    case Qt::Key_Escape:
    case Qt::Key_Delete:
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return false;
    default:
        break;
    }
    // Only allow 0-9 (plus the navigation keys above)
    const QString t = key->text();
    if (t.size() == 1 && t[0] >= QLatin1Char('0') && t[0] <= QLatin1Char('9')) {
        return false;
    }
    return true;
}

// =============================================================================================
// DialogWindow
// =============================================================================================

namespace {
DialogWindow::ShowHook &showHook()
{
    static DialogWindow::ShowHook hook;
    return hook;
}
} // namespace

void updateDefaultButton(QWidget *root)
{
    QPushButton *chosen = nullptr;
    const QList<QPushButton *> buttons = root->findChildren<QPushButton *>();
    for (QPushButton *button : buttons) {
        if (!chosen && button->property("gcmDefault").toBool() && button->isVisibleTo(root->window())) {
            // A button on a hidden tab page is not visible to the window
            bool onHiddenPage = false;
            for (QWidget *w = button->parentWidget(); w && w != root->window(); w = w->parentWidget()) {
                if (w->isHidden()) {
                    onHiddenPage = true;
                    break;
                }
            }
            if (!onHiddenPage) {
                chosen = button;
            }
        }
    }
    for (QPushButton *button : buttons) {
        button->setDefault(button == chosen);
    }
}

QWidget *addTabPage(QTabWidget *tabs, const QString &title, QBoxLayout *&layout, int topBottomMargin)
{
    auto *page = new QWidget(tabs);
    layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, topBottomMargin, 0, topBottomMargin);
    layout->setSpacing(0);
    tabs->addTab(page, title);
    return page;
}

DialogWindow::DialogWindow(View *view) : m_view(view)
{
    setWindowTitle(view->title());
    setWindowIcon(QIcon(themedPixmap(QStringLiteral("gcm-logo.svg"), 64, 64)));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(30, 25, 30, 30);
    layout->addWidget(view);
    connect(view, &View::accepted, this, [this] {
        m_accepted = true;
        accept();
    });
    connect(view, &View::canceled, this, [this] {
        m_accepted = false;
        reject();
    });
}

void DialogWindow::setShowHook(ShowHook hook)
{
    showHook() = std::move(hook);
}

void DialogWindow::fitToContent()
{
    // Width="420" SizeToContent="Height" MinHeight="280" MaxHeight="520" CanResize="False"
    constexpr int Width = 420;
    QLayout *l = layout();
    l->activate();
    const int height = l->hasHeightForWidth() ? l->totalHeightForWidth(Width) : l->totalSizeHint().height();
    setFixedSize(Width, std::clamp(height, 280, 520));
}

void DialogWindow::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    updateDefaultButton(this);
    // Send a focus request to the view once the window is up
    QPointer<View> view = m_view;
    QTimer::singleShot(0, this, [view] {
        if (view) {
            view->setInitialFocus();
        }
    });
}

bool DialogWindow::showAndWait(const OptString &parentWindowId)
{
    Trace2::Scope region = Trace2::startRegion(QStringLiteral("ui"), QStringLiteral("show_window"));

    fitToContent();
    // WindowStartupLocation="CenterScreen"
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect area = screen->availableGeometry();
        move(area.center() - QPoint(width() / 2, height() / 2));
    }

    // Parenting to an external window is only implemented for Windows upstream; the id is traced as there.
    bool ok = false;
    const qlonglong parentId = parentWindowId ? parentWindowId->toLongLong(&ok) : 0;
    if (ok && parentId != 0) {
        Trace2::writeData(QStringLiteral("ui"), QStringLiteral("parent"), QStringLiteral("0x%1").arg(parentId, 0, 16));
    }

    if (const ShowHook &hook = showHook()) {
        QTimer::singleShot(0, this, [this, hook] { hook(*this); });
    }
    m_accepted = false;
    show();
    // Bring the window to the front and focus it
    if (QGuiApplication::platformName() != QLatin1String("offscreen")) {
        raise();
    }
    activateWindow();
    exec();
    return m_accepted;
}

} // namespace gcm::ui
