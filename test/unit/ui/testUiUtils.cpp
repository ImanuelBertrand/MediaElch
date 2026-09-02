#include "test/test_helpers.h"

#include "ui/UiUtils.h"

#include <QApplication>
#include <QComboBox>
#include <QFocusEvent>
#include <QLineEdit>
#include <QMenu>
#include <QVBoxLayout>
#include <QWidget>

using namespace mediaelch;

/// \brief Builds an editable combo box in a shown, active window.
/// \details The window has to be shown and active or the combo never becomes the focus
///          widget and showPopup() has nothing to take focus away from.
namespace {

struct ComboFixture
{
    QWidget host;
    QComboBox* combo;
    QComboBox* otherCombo;
    QLineEdit* otherEdit;

    ComboFixture()
    {
        auto* layout = new QVBoxLayout(&host);
        combo = new QComboBox(&host);
        otherCombo = new QComboBox(&host);
        otherEdit = new QLineEdit(&host);
        layout->addWidget(combo);
        layout->addWidget(otherCombo);
        layout->addWidget(otherEdit);
        combo->setEditable(true);
        combo->addItems({"Alien Collection", "Predator Collection"});
        otherCombo->addItems({"Kodi", "Emby"});
        host.show();
        host.activateWindow();
        qApp->processEvents();
    }

    ~ComboFixture()
    {
        combo->hidePopup();
        otherCombo->hidePopup();
        qApp->processEvents();
    }
};

} // namespace

TEST_CASE("isOwnPopupOpen tells a combo box's own drop-down from anything else", "[ui][movie][set]")
{
    // MovieWidget commits the set name on the set combo's focus-out, and opening that combo's
    // drop-down causes one too.  Qt's focus reason cannot separate them; this predicate does.
    ComboFixture f;

    SECTION("no popup at all")
    {
        CHECK_FALSE(ui::isOwnPopupOpen(f.combo));
    }

    SECTION("the combo's own drop-down is open")
    {
        f.combo->setFocus();
        qApp->processEvents();
        f.combo->showPopup();
        qApp->processEvents();

        CHECK(ui::isOwnPopupOpen(f.combo));
    }

    SECTION("the drop-down is closed again")
    {
        f.combo->setFocus();
        f.combo->showPopup();
        qApp->processEvents();
        REQUIRE(ui::isOwnPopupOpen(f.combo));

        f.combo->hidePopup();
        qApp->processEvents();

        CHECK_FALSE(ui::isOwnPopupOpen(f.combo));
    }

    SECTION("another widget's drop-down is open")
    {
        // A different popup must not suppress the commit: the user really is leaving the box.
        f.combo->setFocus();
        qApp->processEvents();
        f.otherCombo->showPopup();
        qApp->processEvents();
        REQUIRE(QApplication::activePopupWidget() != nullptr);

        CHECK_FALSE(ui::isOwnPopupOpen(f.combo));
        CHECK(ui::isOwnPopupOpen(f.otherCombo));
    }

    SECTION("a null combo box has no popup")
    {
        CHECK_FALSE(ui::isOwnPopupOpen(nullptr));
    }

    SECTION("a null combo box has no popup while an unrelated one is open")
    {
        // Without the null-combo guard this degenerates to "popup->parentWidget() == nullptr",
        // which any parentless popup satisfies.
        QMenu menu;
        menu.addAction("unrelated");
        menu.popup(QPoint(0, 0));
        qApp->processEvents();
        REQUIRE(QApplication::activePopupWidget() == &menu);
        REQUIRE(menu.parentWidget() == nullptr);

        CHECK_FALSE(ui::isOwnPopupOpen(nullptr));

        menu.close();
        qApp->processEvents();
    }
}

TEST_CASE("shouldCommitOnFocusOut is the whole commit rule for an editable combo box", "[ui][movie][set]")
{
    // MovieWidget::eventFilter() is this predicate plus a call to onSetChange(), and no test
    // can build a MovieWidget, so the rule lives here where each term can be falsified alone.
    ComboFixture f;
    QFocusEvent focusOut(QEvent::FocusOut, Qt::MouseFocusReason);
    QFocusEvent focusIn(QEvent::FocusIn, Qt::MouseFocusReason);

    SECTION("a focus-out on the combo box commits")
    {
        CHECK(ui::shouldCommitOnFocusOut(f.combo, f.combo, &focusOut));
    }

    SECTION("another object's focus-out does not")
    {
        // The filter is installed on one widget today, so this term is reachable only here.
        CHECK_FALSE(ui::shouldCommitOnFocusOut(f.combo, f.otherCombo, &focusOut));
        CHECK_FALSE(ui::shouldCommitOnFocusOut(f.combo, f.otherEdit, &focusOut));
    }

    SECTION("gaining focus does not commit")
    {
        CHECK_FALSE(ui::shouldCommitOnFocusOut(f.combo, f.combo, &focusIn));
    }

    SECTION("a blocked combo box is being repopulated, not edited")
    {
        // Events are delivered whatever blockSignals() says, so the bracket that
        // updateMovieInfo() holds while it refills the box is read explicitly.
        f.combo->blockSignals(true);
        CHECK_FALSE(ui::shouldCommitOnFocusOut(f.combo, f.combo, &focusOut));
        f.combo->blockSignals(false);
        CHECK(ui::shouldCommitOnFocusOut(f.combo, f.combo, &focusOut));
    }

    SECTION("the combo box's own drop-down opening does not commit")
    {
        f.combo->setFocus();
        qApp->processEvents();
        f.combo->showPopup();
        qApp->processEvents();
        REQUIRE(ui::isOwnPopupOpen(f.combo));

        CHECK_FALSE(ui::shouldCommitOnFocusOut(f.combo, f.combo, &focusOut));
    }

    SECTION("another widget's drop-down opening still commits")
    {
        f.combo->setFocus();
        qApp->processEvents();
        f.otherCombo->showPopup();
        qApp->processEvents();
        REQUIRE(QApplication::activePopupWidget() != nullptr);

        CHECK(ui::shouldCommitOnFocusOut(f.combo, f.combo, &focusOut));
    }

    SECTION("a null combo box or event commits nothing")
    {
        CHECK_FALSE(ui::shouldCommitOnFocusOut(nullptr, nullptr, &focusOut));
        CHECK_FALSE(ui::shouldCommitOnFocusOut(f.combo, f.combo, nullptr));
    }
}

TEST_CASE("withCurrentValue keeps an editable combo box able to show its value", "[ui][movie][set]")
{
    SECTION("a value the list already has leaves the list untouched")
    {
        const QStringList sets{"", "Alien Collection", "Predator Collection"};

        const QStringList result = ui::withCurrentValue(sets, "Alien Collection");

        CHECK(result == sets);
    }

    SECTION("a value the list is missing is added, so indexOf() cannot return -1")
    {
        // What updateMovieInfo() faces when MovieSetModel has not seen a movie's set yet:
        // without this the box lands on index -1 and the next focus loss commits "".
        const QStringList sets{"", "Alien Collection"};

        const QStringList result = ui::withCurrentValue(sets, "Alien Anthology");

        CHECK(result.contains("Alien Anthology"));
        CHECK(result.indexOf("Alien Anthology") >= 0);
        CHECK(result.mid(0, sets.size()) == sets);
    }

    SECTION("the empty name is a value like any other")
    {
        // MovieWidget's list starts with an empty entry, but a list without one must still be
        // able to show it rather than fall back to -1.
        CHECK(ui::withCurrentValue({"", "Alien Collection"}, "") == QStringList{"", "Alien Collection"});
        CHECK(ui::withCurrentValue({"Alien Collection"}, "").indexOf("") >= 0);
    }

    SECTION("an empty list can still show a value")
    {
        CHECK(ui::withCurrentValue({}, "Alien Collection") == QStringList{"Alien Collection"});
    }
}

TEST_CASE("hasUncommittedEdit tells a typed set name from a stale display", "[ui][movie][set]")
{
    // MovieWidget's save commits the set combo, because the navbar's buttons never end its
    // edit; the rule that keeps that from writing a name the user never touched lives here.
    ComboFixture f;
    f.combo->setCurrentText("Alien Collection");

    SECTION("a box still showing what it was filled with holds no edit")
    {
        CHECK_FALSE(ui::hasUncommittedEdit(f.combo, "Alien Collection"));
    }

    SECTION("a name typed into the box is an edit")
    {
        f.combo->setCurrentText("Alien Anthology");

        CHECK(ui::hasUncommittedEdit(f.combo, "Alien Collection"));
    }

    SECTION("an emptied box is an edit, which is how a movie leaves its set")
    {
        f.combo->setCurrentText("");

        CHECK(ui::hasUncommittedEdit(f.combo, "Alien Collection"));
    }

    SECTION("a set renamed elsewhere leaves the untouched box holding no edit")
    {
        // The blocker: the sets tab renamed the set under the widget, so the box shows a name
        // the movie no longer carries -- and committing it would drag the movie back.
        const QString renamedElsewhere = "Alien Anthology";
        REQUIRE(f.combo->currentText() != renamedElsewhere);

        CHECK_FALSE(ui::hasUncommittedEdit(f.combo, "Alien Collection"));
    }

    SECTION("a name picked from the drop-down is an edit until it is committed")
    {
        f.combo->setCurrentIndex(1);
        REQUIRE(f.combo->currentText() == "Predator Collection");

        CHECK(ui::hasUncommittedEdit(f.combo, "Alien Collection"));
        CHECK_FALSE(ui::hasUncommittedEdit(f.combo, "Predator Collection"));
    }

    SECTION("a null combo box holds nothing to commit")
    {
        CHECK_FALSE(ui::hasUncommittedEdit(nullptr, "Alien Collection"));
    }
}
