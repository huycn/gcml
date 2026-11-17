// Shared building blocks of the graphical helper: the dialog window (port of UI/Controls/DialogWindow), the theme
// (UI/Assets/Base.axaml + Images.axaml), the product header, hyperlink buttons, images, and the GitHub controls
// HorizontalShadowDivider and SixDigitInput.
#pragma once

#include "core/common.h"

#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include <functional>

class QApplication;
class QBoxLayout;
class QTabWidget;

namespace gcm::ui {

// --- theme ----------------------------------------------------------------------------------
/// Follows the system: when the platform reports a dark colour scheme but the style still uses a light palette
/// (e.g. the Fusion style on GNOME), a dark Fusion palette is installed. GCM_UI_THEME=light|dark forces one.
void applySystemTheme(QApplication &app);
/// Installs the light or dark palette explicitly (also used by the tests for screenshots).
void applyTheme(QApplication &app, bool dark);
/// True when the current application palette is dark (window colour darker than mid grey).
bool isDarkTheme();

/// Renders a bundled image (`:/gcm/<name>`) at the given logical height (width from the aspect ratio, or `width`
/// if > 0), for the device pixel ratio of the screen. Monochrome icons (person, info, help, warning) take the
/// theme's icon/warning colour, as the Avalonia resources do.
QPixmap themedPixmap(const QString &name, int height, int width = 0);

/// Pixel font size as in the Avalonia views (FontSize is in device-independent pixels).
QFont fontWithPixelSize(const QFont &base, int pixelSize, QFont::Weight weight = QFont::Normal);

// --- widgets --------------------------------------------------------------------------------
/// An image that re-renders itself when the palette changes (light/dark).
class ImageLabel : public QLabel
{
    Q_OBJECT
public:
    ImageLabel(QString name, int height, int width = 0, QWidget *parent = nullptr);

protected:
    void changeEvent(QEvent *event) override;

private:
    void render();
    QString m_name;
    int m_height;
    int m_width;
};

/// Button styled as a hyperlink (the "hyperlink" class of ButtonHyperlink.axaml).
class LinkButton : public QPushButton
{
    Q_OBJECT
public:
    explicit LinkButton(const QString &text, QWidget *parent = nullptr);

protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
};

/// A push button with the "accent" class: highlighted colours, and the dialog's default (Enter) button while it
/// is visible on the current page.
QPushButton *makeAccentButton(const QString &text, QWidget *parent = nullptr);

/// Logo plus "Git Credential Manager" (the product header of the generic views).
QWidget *makeProductHeader(QWidget *parent = nullptr);

/// Centred text with the given pixel size and weight; word wrapped.
QLabel *makeTextBlock(const QString &text, int pixelSize = 0, QFont::Weight weight = QFont::Normal, QWidget *parent = nullptr);

/// Adds `widget` to `layout` horizontally centred.
void addCentered(QBoxLayout *layout, QWidget *widget);

/// A line edit for a secret (PasswordChar="●").
QLineEdit *makeSecretEdit(const QString &placeholder, QWidget *parent = nullptr);
QLineEdit *makeTextEdit(const QString &placeholder, QWidget *parent = nullptr);

/// Port of GitHub/UI/Controls/HorizontalShadowDivider: a 1 px line with a soft shadow below.
class ShadowDivider : public QWidget
{
    Q_OBJECT
public:
    explicit ShadowDivider(QWidget *parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
};

/// Port of GitHub/UI/Controls/SixDigitInput: six one-digit boxes; typing moves forward, Backspace/Left/Right
/// move between boxes, paste fills all boxes from the digits of the clipboard text.
class SixDigitInput : public QWidget
{
    Q_OBJECT
public:
    explicit SixDigitInput(QWidget *parent = nullptr);

    /// Concatenation of the boxes (only filled boxes contribute).
    QString text() const;
    /// Distributes the digits of `text` over the boxes (non-digits are dropped; empty clears all).
    void setText(const QString &text);
    void setFocusToFirst();
    QLineEdit *box(int index) const { return m_boxes.value(index); }

Q_SIGNALS:
    void textChanged(const QString &text);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void moveFocus(bool next);
    QList<QLineEdit *> m_boxes;
    bool m_ignoreUpdate = false;
};

// --- views and the window -------------------------------------------------------------------
/// Base of every view (the Avalonia UserControl + its WindowViewModel): emits accepted()/canceled() to close
/// the window, and focuses the most useful control when shown (IFocusable.SetFocus).
class View : public QWidget
{
    Q_OBJECT
public:
    explicit View(QWidget *parent = nullptr) : QWidget(parent) {}
    QString title() const { return m_title; }
    void setTitle(const QString &title) { m_title = title; }
    virtual void setInitialFocus() {}

Q_SIGNALS:
    void accepted();
    void canceled();

protected:
    QString m_title = QStringLiteral("Git Credential Manager");
};

/// Port of DialogWindow + AvaloniaUi.ShowViewAsync: a fixed-width (420) window sized to its content
/// (280..520 px high), centred on the screen, with the view inside 30/25/30/30 margins. Escape or closing the
/// window cancels.
class DialogWindow : public QDialog
{
    Q_OBJECT
public:
    explicit DialogWindow(View *view);
    View *view() const { return m_view; }

    /// Shows the window and waits until it is closed; true when the view accepted. Writes the TRACE2 region
    /// ui/show_window, and ui/parent when a parent window id is given (parenting itself is Windows only upstream).
    bool showAndWait(const OptString &parentWindowId = std::nullopt);

    /// Test seam: called (from the window's event loop) right after the window is shown. The hook drives the view
    /// and must eventually accept or cancel it.
    using ShowHook = std::function<void(DialogWindow &)>;
    static void setShowHook(ShowHook hook);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void fitToContent();
    View *m_view;
    bool m_accepted = false;
};

/// Makes the first visible, enabled accent button inside `root` the dialog's default button. Called by the
/// window when shown and by tabbed views when the page changes.
void updateDefaultButton(QWidget *root);

/// Helper for tabbed views: adds a page with the given margins (StackPanel Margin="0,10") to `tabs`.
QWidget *addTabPage(QTabWidget *tabs, const QString &title, QBoxLayout *&layout, int topBottomMargin);

} // namespace gcm::ui
