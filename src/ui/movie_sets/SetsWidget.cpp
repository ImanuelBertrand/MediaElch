#include "SetsWidget.h"
#include "ui_SetsWidget.h"

#include "data/movie/Movie.h"
#include "data/movie/MovieSet.h"
#include "globals/Globals.h"
#include "globals/Helper.h"
#include "globals/Manager.h"
#include "log/Log.h"
#include "media_center/MediaCenterInterface.h"
#include "model/MovieSetModel.h"
#include "network/DownloadManager.h"
#include "settings/Settings.h"
#include "ui/UiUtils.h"
#include "ui/image/ImageDialog.h"
#include "ui/image/ImagePreviewDialog.h"
#include "ui/main/MainWindow.h"
#include "ui/movie_sets/MovieListDialog.h"
#include "ui/notifications/NotificationBox.h"

#include <QFileDialog>
#include <QFrame>
#include <QLabel>
#include <QMessageBox>

SetsWidget::SetsWidget(QWidget* parent) : QWidget(parent), ui(new Ui::SetsWidget)
{
    ui->setupUi(this);

    ui->sets->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui->movies->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui->movies->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ui->buttonPreviewBackdrop->setEnabled(false);
    ui->buttonPreviewPoster->setEnabled(false);

#ifndef Q_OS_MAC
    QFont nameFont = ui->setName->font();
    nameFont.setPointSize(nameFont.pointSize() - 4);
    ui->setName->setFont(nameFont);
#endif

    m_loadingMovie = new QMovie(":/img/spinner.gif", QByteArray(), this);
    m_loadingMovie->start();
    m_downloadManager = new DownloadManager(this);

    // clang-format off
    connect(ui->sets,                  &QTableWidget::itemSelectionChanged,   this, &SetsWidget::onSetSelected);
    connect(ui->sets,                  &QTableWidget::itemChanged,            this, &SetsWidget::onSetNameChanged);
    connect(ui->movies,                &QTableWidget::itemChanged,            this, &SetsWidget::onSortTitleChanged);
    connect(ui->movies,                &QTableWidget::itemDoubleClicked,      this, &SetsWidget::onJumpToMovie);
    connect(ui->buttonAddMovie,        &QAbstractButton::clicked,             this, &SetsWidget::onAddMovie);
    connect(ui->buttonRemoveMovie,     &QAbstractButton::clicked,             this, &SetsWidget::onRemoveMovie);
    connect(ui->poster,                &MyLabel::clicked,                     this, &SetsWidget::chooseSetPoster);
    connect(ui->backdrop,              &MyLabel::clicked,                     this, &SetsWidget::chooseSetBackdrop);
    connect(ui->buttonPreviewPoster,   &QAbstractButton::clicked,             this, &SetsWidget::onPreviewPoster);
    connect(ui->buttonPreviewBackdrop, &QAbstractButton::clicked,             this, &SetsWidget::onPreviewBackdrop);
    connect(m_downloadManager,         &DownloadManager::sigDownloadFinished, this, &SetsWidget::onDownloadFinished, static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection));
    // clang-format on

    ui->sets->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tableContextMenu = new QMenu(ui->sets);
    // A disabled action's tooltip is only shown if the menu is asked to show tooltips at
    // all, and *Add Movie Set* is disabled for a reason the user cannot otherwise see.
    m_tableContextMenu->setToolTipsVisible(true);
    m_actionAddSet = new QAction(tr("Add Movie Set"), this);
    m_actionAddSet->setObjectName("actionAddMovieSet");
    auto* actionDeleteSet = new QAction(tr("Delete Movie Set"), this);
    actionDeleteSet->setObjectName("actionDeleteMovieSet");
    // A set with a `set.nfo` stays in the list after its last movie leaves it, so the
    // list can hold sets nothing in the library points at.  Nothing about a row says so,
    // and a user with a hundred sets cannot find them by opening each one.
    auto* actionOnlyEmptySets = new QAction(tr("Show Only Empty Movie Sets"), this);
    actionOnlyEmptySets->setCheckable(true);
    m_tableContextMenu->addAction(m_actionAddSet);
    m_tableContextMenu->addAction(actionDeleteSet);
    m_tableContextMenu->addSeparator();
    m_tableContextMenu->addAction(actionOnlyEmptySets);
    connect(m_actionAddSet, &QAction::triggered, this, &SetsWidget::onAddMovieSet);
    connect(actionDeleteSet, &QAction::triggered, this, &SetsWidget::onRemoveMovieSet);
    connect(actionOnlyEmptySets, &QAction::toggled, this, &SetsWidget::onShowOnlyEmptySets);
    connect(ui->sets, &QWidget::customContextMenuRequested, this, &SetsWidget::showSetsContextMenu);
    // The settings window can be opened and saved while this tab is on screen, so the
    // answer to "may I write?" changes under it.  There is no signal for the movie set
    // directory in particular; this is the one the application has.
    connect(Settings::instance(), &Settings::sigSettingsSaved, this, &SetsWidget::onSettingsSaved);
    connect(ui->folderNotice, &QLabel::linkActivated, this, [this](const QString& /*link*/) {
        // The notice names the settings page in words as well, so the link is a
        // shortcut and not the only way there.
        emit sigOpenSettings();
    });

    clear();

    QPixmap pixmap = QPixmap(":/img/placeholders/poster.png")
                         .scaled(QSize(160, 260) * devicePixelRatioF(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    pixmap.setDevicePixelRatio(devicePixelRatioF());
    ui->poster->setPixmap(pixmap);

    QPixmap pixmap2 = QPixmap(":/img/placeholders/fanart.png")
                          .scaled(QSize(160, 72) * devicePixelRatioF(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    pixmap2.setDevicePixelRatio(devicePixelRatioF());
    ui->backdrop->setPixmap(pixmap2);

    applyWriteAccess();
}

void SetsWidget::applyWriteAccess()
{
    // Asked of the model, not of the media center directly.  The model's answer also
    // covers "no media center at all", which is what its own drop rule decides by, and
    // a guard that asked a second, subtly different question would come apart from the
    // rule it is guarding.
    const bool recordsEnabled = Manager::instance()->movieSetModel()->recordsAreConfigured();
    // Asked of the media center, which is the object that would have to write the file.
    // Not the same question: artwork resolves in both layouts, a record in one.
    const bool artworkEnabled = Manager::instance()->mediaCenterInterface()->movieSetArtworkEnabled();
    m_recordsAreConfigured = recordsEnabled;

    m_actionAddSet->setEnabled(recordsEnabled);
    m_actionAddSet->setToolTip(recordsEnabled
                                   ? QString()
                                   : tr("A movie set needs a movie set directory to be remembered in. Choose one "
                                        "under Settings, Movies, Movie Set Artwork."));

    // A disabled widget receives no mouse events, so this is what stops the artwork
    // dialogs from opening.  chooseSetPoster() and chooseSetBackdrop() refuse as well,
    // so that a future rearrangement of this tab cannot reopen the path -- but only the
    // enabled state below is held by a test.  The slots' own refusal is pinned by the
    // line they log and nothing more, because a test that removed the guard to watch it
    // fail would open a modal ImageDialog and hang instead.
    //
    // Deliberately no tooltip on these two.  A disabled widget gets no hover either, so
    // a tooltip here could never be shown; the notice below is on screen in exactly the
    // state that disables them and is where the explanation belongs.  (The disabled
    // *action* above is a different mechanism: QMenu draws and hovers its own action
    // rects, so setToolTipsVisible() does show that one.)
    ui->poster->setEnabled(artworkEnabled);
    ui->backdrop->setEnabled(artworkEnabled);

    // Three states, derived from the two answers above rather than from a third look at
    // the settings -- which is deliberate, because a third look would be a third
    // predicate.  Records off *and* artwork off can only be the separate artwork
    // directory selected without one having been chosen; records off with artwork on can
    // only be the other layout.
    if (recordsEnabled) {
        ui->folderNoticeFrame->hide();

    } else if (!artworkEnabled) {
        // A real misconfiguration: the user asked for a directory and never named one.
        // Until this step that silently wrote into the process's working directory.
        //
        // It names exactly the three things that are off and then says what still works,
        // and the second half is not politeness.  Calling this tab "read-only" would be
        // untrue -- renaming, membership, the sort title and deleting a set all still
        // write -- and it would push the user into renaming a set by retyping the name on
        // each movie, which is the D3 fork the sets tab's own rename exists to prevent.
        // Nor does it claim the overview and the TMDB id cannot be saved: neither has an
        // editor anywhere yet, so that would be describing a loss the user cannot feel.
        ui->folderNoticeFrame->setFrameShape(QFrame::StyledPanel);
        ui->folderNotice->setText(
            tr("<b>No movie set directory is configured.</b> Set artwork cannot be saved, movie sets get no file "
               "of their own, and a movie set with no movies cannot be created. Renaming a set, adding and "
               "removing movies and the sort title still work: those are stored in the movies themselves. "
               "<a href=\"settings\">Choose a directory</a> under Settings, Movies, Movie Set Artwork, or "
               "switch back to \"Artwork next to movies\"."));
        ui->folderNoticeFrame->show();

    } else {
        // Not a warning, and it must not read like one.  "Artwork next to movies" is the
        // default precisely so that nobody has to configure a directory before using
        // MediaElch (bugwelle, #1243, 2021-04-06), so every user who has never opened
        // the settings sees this line and none of them has done anything wrong.
        ui->folderNoticeFrame->setFrameShape(QFrame::NoFrame);
        ui->folderNotice->setText(
            tr("Movie sets have no file of their own in this layout, so a movie set with no movies cannot be "
               "created. Set artwork is written next to your movies. "
               "<a href=\"settings\">Settings, Movies, Movie Set Artwork</a>"));
        ui->folderNoticeFrame->show();
    }
}

/**
 * \brief SetsWidget::~SetsWidget
 */
SetsWidget::~SetsWidget()
{
    delete ui;
}

/**
 * \brief Returns the splitter
 * \return The splitter
 */
QSplitter* SetsWidget::splitter()
{
    return ui->splitter;
}

void SetsWidget::showSetsContextMenu(QPoint point)
{
    m_tableContextMenu->exec(ui->sets->mapToGlobal(point));
}

/**
 * \brief Shows the library's movie sets
 * \details The set list itself belongs to MovieSetModel, which groups the library once
 *          and keeps the result; this only reads it.  The widget's own maps still hold
 *          raw Movie*, so they are cleared before ui->sets is -- otherwise MediaElch may
 *          access an invalidated Movie* in an event handler of ui->sets.
 */
void SetsWidget::loadSets()
{
    applyWriteAccess();
    m_moviesToSave.clear();
    m_setPosters.clear();
    m_setBackdrops.clear();

    emit setActionSaveEnabled(false, MainWidgets::MovieSets);
    clear();
    ui->buttonPreviewBackdrop->setEnabled(false);
    ui->buttonPreviewPoster->setEnabled(false);
    int currentRow =
        (ui->sets->currentRow() >= 0 && ui->sets->currentRow() < ui->sets->rowCount()) ? ui->sets->currentRow() : 0;
    ui->sets->clear();
    ui->sets->setRowCount(0);

    // Regrouping here is what re-derives the library's sets, and the moment at which a
    // set that has nothing left to exist by is dropped -- no movies *and* no `set.nfo`
    // (D-A).  So a set whose last movie left survives if it has a record and goes if it
    // has not, and a set added by the context menu survives only once it has been saved,
    // which is what writes its record.
    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    setModel->reload();

    // Two strings per row, and which is which decides whether this tab works at all.
    // Qt::UserRole carries the set's **match key** -- what the member NFOs say, what
    // names the folder on disk, what MovieSetModel is keyed by -- and every lookup in
    // this widget goes through it.  The cell *text* is the display title, which is the
    // same string until a set-file-only rename and a different one after (D-B).  The
    // widget's three maps are keyed by the match key, like the model.
    QVector<QPair<QString, QString>> setRows; // (match key, display title)
    for (const MovieSet* movieSet : setModel->sets()) {
        if (m_showOnlyEmptySets && !movieSet->movies().isEmpty()) {
            continue;
        }
        setRows.append({movieSet->name(), movieSet->displayName()});
    }
    // Sorted by what the user reads, with the key breaking a tie so that the order is
    // total: two sets may not share a display title, but they may while one is being
    // renamed onto the other's, and an unstable sort of equal keys would reorder rows
    // under the user mid-edit.
    std::sort(setRows.begin(), setRows.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.second != rhs.second ? lhs.second < rhs.second : lhs.first < rhs.first;
    });

    // Signals blocked for the whole population, because setItem() and setToolTip() are
    // both an itemChanged -- the signal onSetNameChanged() is connected to -- and the row
    // does not have its Qt::UserRole yet when the first of them fires.  So every row
    // inserted here used to re-enter the rename slot as "a set with no name, renamed to
    // this one", which found a set already called that and took the merge branch: a
    // no-op merge of an empty set, run once per row on every load.  It was invisible
    // while a merge was silent.  It is not invisible now that a merge asks first.
    ui->sets->blockSignals(true);
    for (const auto& setRow : asConst(setRows)) {
        const QString& setName = setRow.first;
        m_moviesToSave.insert(setName, QVector<Movie*>());
        m_setPosters.insert(setName, QImage());
        m_setBackdrops.insert(setName, QImage());

        int row = ui->sets->rowCount();
        ui->sets->insertRow(row);
        ui->sets->setItem(row, 0, new QTableWidgetItem(setRow.second));
        ui->sets->item(row, 0)->setData(Qt::UserRole, setName);
        // The divergence is never hidden.  A set whose display title is not what its
        // movie files say is exactly the state a user needs to be able to see, or "why
        // does Kodi 21 show the old name?" has no answer anywhere in the UI.  The same
        // helper runs at every rename, so this is not the only place it can appear.
        applyDivergenceTooltip(ui->sets->item(row, 0), setModel->set(setName));
    }
    ui->sets->blockSignals(false);

    if (ui->sets->rowCount() > 0 && currentRow < ui->sets->rowCount()) {
        ui->sets->setCurrentItem(ui->sets->item(currentRow, 0));
    }
    emit setActionSaveEnabled(true, MainWidgets::MovieSets);
}

/**
 * \brief Called when set table selection changes
 * \see SetsWidget::loadSets
 */
void SetsWidget::onSetSelected()
{
    int row = ui->sets->currentRow();
    qCDebug(generic) << "row=" << row << "rowCount=" << ui->sets->rowCount();
    if (row < 0 || row >= ui->sets->rowCount()) {
        clear();
        return;
    }

    QString setName = ui->sets->item(ui->sets->currentRow(), 0)->data(Qt::UserRole).toString();
    loadSet(setName);
}

/**
 * \brief Clears contents
 */
void SetsWidget::clear()
{
    ui->setName->clear();
    ui->movies->clearContents();
    ui->movies->setRowCount(0);
    ui->backdropResolution->clear();
    ui->posterResolution->clear();
    m_currentBackdrop = QImage();
    m_currentPoster = QImage();
}

/**
 * \brief Fills the widget with set data
 * \param set Name of the set
 */
void SetsWidget::loadSet(QString set)
{
    qCDebug(generic) << "Entered, set=" << set;
    clear();
    const MovieSet* movieSet = Manager::instance()->movieSetModel()->set(set);
    // \p set is the match key, because that is what every caller has to look the set up
    // by.  The heading is read by a person, so it shows the display title -- the same
    // split saveSet() makes for its messages.  Leaving the key here put the old name in
    // the big label while the row above it showed the new one.
    ui->setName->setText(movieSet != nullptr ? movieSet->displayName() : set);
    ui->buttonPreviewBackdrop->setEnabled(false);
    ui->buttonPreviewPoster->setEnabled(false);
    ui->movies->blockSignals(true);
    const QVector<Movie*> movies = (movieSet != nullptr) ? movieSet->movies() : QVector<Movie*>();
    for (Movie* movie : movies) {
        int row = ui->movies->rowCount();
        ui->movies->insertRow(row);
        ui->movies->setItem(row, 0, new QTableWidgetItem(movie->title()));
        ui->movies->item(row, 0)->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        ui->movies->item(row, 0)->setData(Qt::UserRole, QVariant::fromValue(movie));
        ui->movies->setItem(row, 1, new QTableWidgetItem(movie->sortTitle()));
    }
    ui->movies->sortByColumn(1, Qt::AscendingOrder);

    if (!m_setPosters[set].isNull()) {
        QImage poster = m_setPosters[set];
        QPixmap pixmap = QPixmap::fromImage(poster).scaled(
            QSize(200, 300) * devicePixelRatioF(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pixmap.setDevicePixelRatio(devicePixelRatioF());
        ui->poster->setPixmap(pixmap);
        ui->posterResolution->setText(QString("%1x%2").arg(poster.width()).arg(poster.height()));
        ui->buttonPreviewPoster->setEnabled(true);
        m_currentPoster = poster;
    } else if (!Manager::instance()->mediaCenterInterface()->movieSetPoster(set).isNull()) {
        QImage poster = Manager::instance()->mediaCenterInterface()->movieSetPoster(set);
        QPixmap pixmap = QPixmap::fromImage(poster).scaled(
            QSize(200, 300) * devicePixelRatioF(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pixmap.setDevicePixelRatio(devicePixelRatioF());
        ui->poster->setPixmap(pixmap);
        ui->posterResolution->setText(QString("%1x%2").arg(poster.width()).arg(poster.height()));
        ui->buttonPreviewPoster->setEnabled(true);
        m_currentPoster = poster;
    } else {
        QPixmap pixmap =
            QPixmap(":/img/placeholders/poster.png")
                .scaled(QSize(120, 120) * devicePixelRatioF(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pixmap.setDevicePixelRatio(devicePixelRatioF());
        ui->poster->setPixmap(pixmap);
        ui->buttonPreviewPoster->setEnabled(false);
    }

    if (!m_setBackdrops[set].isNull()) {
        QImage backdrop = m_setBackdrops[set];
        QPixmap pixmap = QPixmap::fromImage(backdrop).scaled(
            QSize(200, 112) * devicePixelRatioF(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pixmap.setDevicePixelRatio(devicePixelRatioF());
        ui->backdrop->setPixmap(pixmap);
        ui->backdropResolution->setText(QString("%1x%2").arg(backdrop.width()).arg(backdrop.height()));
        ui->buttonPreviewBackdrop->setEnabled(true);
        m_currentBackdrop = backdrop;
    } else if (!Manager::instance()->mediaCenterInterface()->movieSetBackdrop(set).isNull()) {
        QImage backdrop = Manager::instance()->mediaCenterInterface()->movieSetBackdrop(set);
        QPixmap pixmap = QPixmap::fromImage(backdrop).scaled(
            QSize(200, 112) * devicePixelRatioF(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pixmap.setDevicePixelRatio(devicePixelRatioF());
        ui->backdrop->setPixmap(pixmap);
        ui->backdropResolution->setText(QString("%1x%2").arg(backdrop.width()).arg(backdrop.height()));
        ui->buttonPreviewBackdrop->setEnabled(true);
        m_currentBackdrop = backdrop;
    } else {
        QPixmap pixmap =
            QPixmap(":/img/placeholders/fanart.png")
                .scaled(QSize(96, 96) * devicePixelRatioF(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pixmap.setDevicePixelRatio(devicePixelRatioF());
        ui->backdrop->setPixmap(pixmap);
        ui->buttonPreviewBackdrop->setEnabled(false);
    }
    ui->movies->blockSignals(false);
}

/**
 * \brief Called when an item in the movies table was changed
 *        Updates movies sorttitle and reorders the table
 * \param item changed item
 */
void SetsWidget::onSortTitleChanged(QTableWidgetItem* item)
{
    qCDebug(generic) << "Entered, item->row=" << item->row() << "rowCount=" << ui->movies->rowCount();
    if (item->row() < 0 || item->row() >= ui->movies->rowCount() || item->column() != 1) {
        qCDebug(generic) << "Invalid row";
        return;
    }
    auto* movie = ui->movies->item(item->row(), 0)->data(Qt::UserRole).value<Movie*>();
    movie->setSortTitle(item->text());
    ui->movies->sortByColumn(1, Qt::AscendingOrder);
    if (!m_moviesToSave[movie->set().name].contains(movie)) {
        m_moviesToSave[movie->set().name].append(movie);
    }
}

/**
 * \brief Execs the MovieListDialog and (if accepted) adds a movie to the movies table
 *        and sets the setname in the movie, which is what makes it a member of the set
 */
void SetsWidget::onAddMovie()
{
    if (ui->sets->currentRow() < 0 || ui->sets->currentRow() >= ui->sets->rowCount()) {
        qCDebug(generic) << "[SetsWidget] Invalid current row";
        return;
    }

    // TODO: Don't use "this", because we don't want to inherit the stylesheet,
    // but we can't pass "nullptr", because otherwise there won't be a modal.
    auto* listDialog = new MovieListDialog(MainWindow::instance());
    const int exitCode = listDialog->exec();
    QVector<Movie*> movies = listDialog->selectedMovies();
    listDialog->deleteLater();

    if (exitCode != QDialog::Accepted || movies.isEmpty()) {
        return;
    }

    const int row = ui->sets->currentRow();
    if (row < 0 || row >= ui->sets->rowCount()) {
        return;
    }

    // The match key, because this value is written onto the movie's own NFO as
    // `<set><name>`; the display title is not a name any movie file may carry.
    QString setName = ui->sets->item(ui->sets->currentRow(), 0)->data(Qt::UserRole).toString();
    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    for (Movie* movie : asConst(movies)) {
        if (movie->set().name == setName) {
            continue;
        }
        // The movie joins a different collection, so its previous set's overview and
        // id must not travel with it.  The name check above, not assign()'s whole-value
        // guard, is what leaves a movie already in this set alone: a name-only value
        // never compares equal to a set that carries an id or an overview.
        MovieSetInfo set;
        set.name = setName;
        // The model owns membership, so it is what puts the movie into the set.
        setModel->assign(movie, set);
        if (!m_moviesToSave[setName].contains(movie)) {
            m_moviesToSave[setName].append(movie);
        }
    }
    loadSet(setName);
}

/**
 * \brief Removes a movie from the movies table and sets an empty sorttitle and set for
 *        the movie, which is what removes it from the set
 */
void SetsWidget::onRemoveMovie()
{
    if (ui->sets->currentRow() < 0 || ui->sets->currentRow() >= ui->sets->rowCount()) {
        qCDebug(generic) << "Invalid current row in sets";
        return;
    }
    if (ui->movies->currentRow() < 0 || ui->movies->currentRow() >= ui->movies->rowCount()) {
        qCDebug(generic) << "Invalid current row in movies";
        return;
    }
    auto* movie = ui->movies->item(ui->movies->currentRow(), 0)->data(Qt::UserRole).value<Movie*>();
    if (!m_moviesToSave[movie->set().name].contains(movie)) {
        m_moviesToSave[movie->set().name].append(movie);
    }
    movie->setSortTitle("");
    Manager::instance()->movieSetModel()->assign(movie, MovieSetInfo{});
    ui->movies->removeRow(ui->movies->currentRow());
}

/**
 * \brief Shows QFileDialog to choose an image, if successful sets the poster
 */
void SetsWidget::chooseSetPoster()
{
    if (ui->sets->currentRow() < 0 || ui->sets->currentRow() >= ui->sets->rowCount()) {
        qCDebug(generic) << "[SetsWidget] Invalid current row in sets";
        return;
    }
    if (!Manager::instance()->mediaCenterInterface()->movieSetArtworkEnabled()) {
        // applyWriteAccess() has already disabled the label this is reached from; this
        // is the same refusal at the action rather than at the affordance, so that a
        // download is never started for an image that could not be written afterwards.
        // Logged at info rather than debug, because the log line is all a test can hold
        // this refusal by: removing the guard sends the slot into ImageDialog, which no
        // test can answer.  See testSetsWidget.cpp for what that costs.
        qCInfo(generic) << "[SetsWidget] Not choosing a set poster: set artwork has nowhere to be written.";
        return;
    }

    QString setName = ui->sets->item(ui->sets->currentRow(), 0)->data(Qt::UserRole).toString();
    auto* movie = new Movie(QStringList());
    movie->setTitle(setName);

    // TODO: Don't use "this", because we don't want to inherit the stylesheet,
    // but we can't pass "nullptr", because otherwise there won't be a modal.
    auto* imageDialog = new ImageDialog(MainWindow::instance());
    imageDialog->setMovie(movie);
    imageDialog->execWithType(ImageType::MoviePoster);
    const int exitCode = imageDialog->result();
    const QUrl imageUrl = imageDialog->imageUrl();
    imageDialog->deleteLater();

    if (exitCode == QDialog::Accepted) {
        DownloadManagerElement d;
        d.movie = movie;
        d.imageType = ImageType::MovieSetPoster;
        d.url = imageUrl;
        m_downloadManager->addDownload(d);
        ui->poster->setPixmap(QPixmap());
        ui->poster->setMovie(m_loadingMovie);
        ui->buttonPreviewPoster->setEnabled(false);
    }
}

/**
 * \brief Shows QFileDialog to choose an image, if successful sets the backdrop
 */
void SetsWidget::chooseSetBackdrop()
{
    if (ui->sets->currentRow() < 0 || ui->sets->currentRow() >= ui->sets->rowCount()) {
        qCDebug(generic) << "Invalid current row in sets";
        return;
    }
    if (!Manager::instance()->mediaCenterInterface()->movieSetArtworkEnabled()) {
        // See chooseSetPoster().
        qCInfo(generic) << "[SetsWidget] Not choosing a set backdrop: set artwork has nowhere to be written.";
        return;
    }

    QString setName = ui->sets->item(ui->sets->currentRow(), 0)->data(Qt::UserRole).toString();
    auto* movie = new Movie(QStringList());
    movie->setTitle(setName);

    // TODO: Don't use "this", because we don't want to inherit the stylesheet,
    // but we can't pass "nullptr", because otherwise there won't be a modal.
    auto* imageDialog = new ImageDialog(MainWindow::instance());
    imageDialog->setMovie(movie);
    imageDialog->execWithType(ImageType::MovieBackdrop);
    const int exitCode = imageDialog->result();
    const QUrl imageUrl = imageDialog->imageUrl();
    imageDialog->deleteLater();

    if (exitCode == QDialog::Accepted) {
        DownloadManagerElement d;
        d.movie = movie;
        d.imageType = ImageType::MovieSetBackdrop;
        d.url = imageUrl;
        m_downloadManager->addDownload(d);
        ui->backdrop->setPixmap(QPixmap());
        ui->backdrop->setMovie(m_loadingMovie);
        ui->buttonPreviewBackdrop->setEnabled(false);
    }
}

/**
 * \brief Saves changed movies in this set
 */
void SetsWidget::saveSet()
{
    if (ui->sets->currentRow() < 0 || ui->sets->currentRow() >= ui->sets->rowCount()) {
        qCDebug(generic) << "Invalid current row in sets";
        return;
    }

    // The match key alone.  All three of this widget's maps are keyed by it, and the
    // cell's text is now the *display* title, which after a set-file-only rename is a
    // string no map here has ever held -- so adding it would index three maps into
    // existence with nothing in them and flush an empty list of movies.  It used to be
    // added because the row's two strings could disagree only transiently, mid-rename;
    // they can disagree permanently now, and the key is the half that identifies.
    const QStringList setNames{ui->sets->item(ui->sets->currentRow(), 0)->data(Qt::UserRole).toString()};

    // The artwork writes can refuse, and the refusal decides whether the image may be
    // let go of.  A set's poster and backdrop live nowhere but the two maps below until
    // they are written, so clearing an entry for a write that did not happen destroys
    // the image -- and this used to clear them unconditionally, under a void return,
    // and then tell the user "Saved".  Kept, so the next Save can write them once the
    // user has fixed what refused them.
    bool artworkSaved = true;
    MediaCenterInterface* mediaCenter = Manager::instance()->mediaCenterInterface();

    for (const QString& setName : asConst(setNames)) {
        for (Movie* movie : asConst(m_moviesToSave[setName])) {
            movie->controller()->saveData(mediaCenter);
        }
        m_moviesToSave[setName].clear();

        // A set without a name has no path of its own to write artwork to: movieSetFileName()
        // collapses to the artwork directory itself, or to the first movie that has no set.
        if (!setName.isEmpty() && !m_setPosters[setName].isNull()) {
            if (mediaCenter->saveMovieSetPoster(setName, m_setPosters[setName])) {
                m_setPosters[setName] = QImage();
            } else {
                artworkSaved = false;
            }
        }
        if (!setName.isEmpty() && !m_setBackdrops[setName].isNull()) {
            if (mediaCenter->saveMovieSetBackdrop(setName, m_setBackdrops[setName])) {
                m_setBackdrops[setName] = QImage();
            } else {
                artworkSaved = false;
            }
        }
    }

    // The set's own record.  `set.nfo` is where the overview and the collection id are
    // authoritative (D-A) -- every member NFO carries a mirror of both, and the artwork
    // is the image files written above, which the record never holds -- so saving a set
    // means writing that file as well.  Writing it is also what gives the set an
    // existence apart from its movies, so that an empty one is still there after the next
    // reload.
    //
    // The write can refuse, in more ways than it once could: no movie set information
    // folder configured, a name that legalises away to nothing, a file already there
    // that belongs to another set, or one that cannot be read to find out.  Reporting
    // success anyway tells the user the opposite of what happened twice over -- nothing
    // was written, and the set did not gain the record that would let it survive losing
    // its movies.
    //
    // The current name, not the pre-rename one: a record under the old name would be a
    // record for a set that no longer exists.
    //
    // Looked up by the **match key**, which is what the model is keyed by.  Asking for
    // the displayed title instead finds nothing after a set-file-only rename, and a
    // null set here makes recordSaved true by short circuit -- so the record would
    // never be written and the rename would be silently lost at the next reload, which
    // is the one failure this whole feature exists to avoid.
    const QString currentName = ui->sets->item(ui->sets->currentRow(), 0)->data(Qt::UserRole).toString();
    MovieSet* movieSet = Manager::instance()->movieSetModel()->set(currentName);
    // What the user is shown.  The messages below name a set the user has to recognise,
    // and after a set-file-only rename that is the title, not the key.
    const QString displayedName = movieSet != nullptr ? movieSet->displayName() : currentName;
    // "There are no records here" is not a failed write, and the media center cannot
    // tell the two apart: saveMovieSet() answers false for both, because that is what
    // its own callers in the model need to hear.  Asked without this, every Save in the
    // artwork-next-to-movies layout -- the shipping default -- put a red error box on
    // screen complaining that a file the notice above has just explained does not exist
    // could not be written.
    //
    // The same predicate applyWriteAccess() asks, and for the same reason: two answers
    // to one question is how a guard and the rule it guards come apart.
    const bool recordsEnabled = Manager::instance()->movieSetModel()->recordsAreConfigured();
    const bool recordSaved = !recordsEnabled || movieSet == nullptr || mediaCenter->saveMovieSet(*movieSet);

    // Three whole sentences rather than one assembled from fragments: which of the two
    // failed is what tells the user where to look, and a translator needs the sentence.
    if (!recordSaved && !artworkSaved) {
        qCWarning(generic) << "[SetsWidget] Movie set" << currentName
                           << "was saved only in part: its artwork could not be written, and neither could its"
                           << "movie set file.";
        NotificationBox::instance()->showError(
            tr("<b>\"%1\"</b>: the movies were saved, but the artwork and the movie set file could not be "
               "written.")
                .arg(displayedName));
        return;
    }
    if (!recordSaved) {
        // The movies and the artwork above were saved; only the set's own file was not.
        qCWarning(generic) << "[SetsWidget] Movie set" << currentName
                           << "was saved only in part: its movie set file could not be written.";
        NotificationBox::instance()->showError(
            tr("<b>\"%1\"</b>: the movies were saved, but the movie set file could not be written.")
                .arg(displayedName));
        return;
    }
    if (!artworkSaved) {
        qCWarning(generic) << "[SetsWidget] Movie set" << currentName
                           << "was saved only in part: its artwork could not be written.";
        NotificationBox::instance()->showError(
            tr("<b>\"%1\"</b>: the movies were saved, but the artwork could not be written.").arg(displayedName));
        return;
    }

    NotificationBox::instance()->showSuccess(tr("<b>\"%1\"</b> Saved").arg(displayedName));
}

/**
 * \brief Shows a full preview of the current backdrop
 */
void SetsWidget::onPreviewBackdrop()
{
    // TODO: Don't use "this", because we don't want to inherit the stylesheet,
    //       but we can't pass "nullptr", because otherwise there won't be a modal.
    auto* dialog = new ImagePreviewDialog(MainWindow::instance());
    dialog->setImage(QPixmap::fromImage(m_currentBackdrop));
    dialog->exec();
    dialog->deleteLater();
}

/**
 * \brief Shows a full preview of the current poster
 */
void SetsWidget::onPreviewPoster()
{
    // TODO: Don't use "this", because we don't want to inherit the stylesheet,
    // but we can't pass "nullptr", because otherwise there won't be a modal.
    auto* dialog = new ImagePreviewDialog(MainWindow::instance());
    dialog->setImage(QPixmap::fromImage(m_currentPoster));
    dialog->exec();
    dialog->deleteLater();
}

void SetsWidget::onAddMovieSet()
{
    m_tableContextMenu->close();
    // Behind the disabled action, not instead of it.  A set created here has no members
    // and no `set.nfo`, and with no movie set information folder it can never get one,
    // so the next reload() drops it (MovieSetModel::dropEmptySets()) and the user
    // watches a set they just made disappear.  **This is also what keeps read-only mode
    // from accumulating memberless sets**, which is the property the whole design rests
    // on -- so this guard is load-bearing rather than defensive, and both halves of it
    // are pinned by a test in test/unit/ui/testSetsWidget.cpp.
    //
    // Naming a *new* set on a movie is a different thing and stays allowed: that set has
    // a member from the moment it exists, so nothing drops it, and membership is
    // authoritative in the movie files with or without a folder (D1a).
    if (!Manager::instance()->movieSetModel()->recordsAreConfigured()) {
        qCDebug(generic) << "[SetsWidget] Not adding a movie set: no movie set directory is configured, so the"
                         << "set could not be remembered and would go at the next reload.";
        return;
    }
    // Asked of the model, not of the table.  The table is a filtered view of the model --
    // "Show Only Empty Movie Sets" hides every set that has movies, and a set can also be
    // missing from it because the list has not been rebuilt since -- so a name absent
    // from the rows can still be taken.  Adding it then hands back the existing set and
    // inserts a second row for it, and a set's name is its primary key (D-B).
    // onSetNameChanged() already asks the model for the same reason.
    //
    // By *either* name, through the same predicate the rename refusal uses.  Asking
    // MovieSetModel::set() alone matches keys only, so a set whose display title is
    // already "New Movie Set" -- one that has had a set-file-only rename -- would let
    // this create a second row the user cannot tell apart from it.  That is the same
    // defect the rename guard exists to prevent, one path over, which is exactly how it
    // got here: a guard added where it was noticed and not to its siblings.
    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    const QString baseName = tr("New Movie Set");
    QString setName = baseName;
    int adder = 0;
    while (setNameIsTaken(setName, nullptr)) {
        ++adder;
        setName = QString("%1 %2").arg(baseName).arg(adder);
    }

    setModel->addSet(setName);
    m_moviesToSave.insert(setName, QVector<Movie*>());
    m_setPosters.insert(setName, QImage());
    m_setBackdrops.insert(setName, QImage());

    ui->sets->blockSignals(true);
    int row = ui->sets->rowCount();
    ui->sets->insertRow(row);
    ui->sets->setItem(row, 0, new QTableWidgetItem(setName));
    ui->sets->item(row, 0)->setData(Qt::UserRole, setName);
    ui->sets->blockSignals(false);
}

void SetsWidget::onRemoveMovieSet()
{
    m_tableContextMenu->close();
    if (ui->sets->currentRow() < 0 || ui->sets->currentRow() >= ui->sets->rowCount()) {
        qCWarning(generic) << "Invalid row" << ui->sets->currentRow();
        return;
    }

    QString setName = ui->sets->item(ui->sets->currentRow(), 0)->text();
    QString origSetName = ui->sets->item(ui->sets->currentRow(), 0)->data(Qt::UserRole).toString();

    // The model goes first, and the row only follows if it agreed.  removeSet() can
    // refuse -- it will not destroy a set whose `set.nfo` it could not remove, because
    // such a set comes back at the next reload -- and taking the row away first would
    // show the user a deletion that had not happened.
    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    const MovieSet* movieSet = setModel->set(origSetName);
    const QVector<Movie*> members = (movieSet != nullptr) ? movieSet->movies() : QVector<Movie*>();

    if (!setModel->removeSet(origSetName)) {
        NotificationBox::instance()->showError(
            tr("<b>\"%1\"</b> could not be deleted: its movie set file could not be removed.").arg(setName));
        return;
    }

    // removeSet() detaches the movies from the set and marks them changed; the sort
    // title is this tab's own doing and has to be cleared here.  Read before the
    // removal, which empties the set, and applied after it, so that a refusal leaves the
    // movies untouched too.
    for (Movie* movie : members) {
        movie->setSortTitle("");
    }
    ui->sets->removeRow(ui->sets->currentRow());
    // By the match key: `setName` above is what the cell displays, and the two are
    // different strings after a set-file-only rename, so removing by it would leave
    // this set's artwork in the maps under a name nothing looks up again.
    m_setPosters.remove(origSetName);
    m_setBackdrops.remove(origSetName);
}

/// \brief Puts the row's text back to what the set is actually called.
/// \details Used wherever a rename is refused.  Signals are blocked because writing an
///          item's text is an itemChanged, which is the signal onSetNameChanged() is
///          connected to -- without this a refusal would re-enter it and refuse again.
void SetsWidget::revertSetName(QTableWidgetItem* item, const QString& name)
{
    // Saved and restored, for the reason spelled out in applyDivergenceTooltip().
    const bool wasBlocked = ui->sets->blockSignals(true);
    item->setText(name);
    ui->sets->blockSignals(wasBlocked);
}

void SetsWidget::onSetNameChanged(QTableWidgetItem* item)
{
    // What the user typed is a *display* name.  Whether it reaches the member movies'
    // NFOs -- whether it becomes the set's match key at all -- is what the rename mode
    // decides; see docs/concepts/movie-sets.md, D-B.
    const QString newName = item->text();
    const QString origSetName = item->data(Qt::UserRole).toString();

    // Whether this is a rename or a merge is the model's answer, not the table's.  The
    // table is the snapshot the last loadSets() took, and the model can hold a set it
    // does not show; deciding from the table would then rename A to B while a second
    // set called B is still in the model, and a set's name is its primary key (D-B).
    // MovieSet::setName() is public and does not check, so the model cannot catch it
    // afterwards either.
    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    MovieSet* origSet = setModel->set(origSetName);
    const QString origDisplayName = origSet != nullptr ? origSet->displayName() : origSetName;
    if (newName == origDisplayName) {
        return;
    }

    // One precondition for all three operations, asked here rather than inside each of
    // them.  "Added to one path and not its siblings" is how this slot has gone wrong
    // repeatedly, and the empty name is the case where getting it wrong destroys data:
    // the all-movie-files rename skipped its collision check and its setName() for an
    // empty name and then reassigned every member to it anyway, detaching the whole set
    // and marking each movie changed -- so the next save wrote an empty `<set>` into
    // every member NFO, with the MovieSet still in the model and its row no longer
    // findable by anything.  Silently.
    //
    // Above the merge check, and safe there: `MovieSetModel::addSet()` refuses the empty
    // name, so no set is ever keyed by it and an empty name can never be a merge target.
    // Nothing below is skipped by moving it up, which is why it can sit where it covers
    // all three operations at once.
    if (origSet == nullptr) {
        // A row for a set the model no longer has.  Not the user's doing and there is
        // nothing to rename, so this reverts quietly.
        qCWarning(generic) << "[SetsWidget] Ignoring a rename of" << origSetName
                           << "-- the model no longer holds that set.";
        revertSetName(item, origDisplayName);
        return;
    }
    if (newName.isEmpty()) {
        qCWarning(generic) << "[SetsWidget] Movie set" << origDisplayName
                           << "was not renamed: a movie set cannot have an empty name.";
        NotificationBox::instance()->showError(
            tr("<b>\"%1\"</b> was not renamed: a movie set cannot have an empty name. To take its movies out "
               "of it, remove the set instead.")
                .arg(origDisplayName));
        revertSetName(item, origDisplayName);
        return;
    }

    MovieSet* targetSet = setModel->set(newName);
    const bool mergesIntoExistingSet = targetSet != nullptr && targetSet != origSet;

    if (mergesIntoExistingSet) {
        // Asked, not assumed.  A merge moves movies between sets and is not undoable,
        // and it is one typo in a table cell away -- the doc has called this out as a
        // gap since the behaviour was first written.
        const QMessageBox::StandardButton answer = QMessageBox::question(this,
            tr("Merge movie sets?"),
            tr("<b>\"%1\"</b> already exists. Renaming <b>\"%2\"</b> to it merges the two: every movie in "
               "<b>\"%2\"</b> is moved into <b>\"%1\"</b>, and <b>\"%2\"</b> is deleted.<br><br>This cannot be "
               "undone. Merge them?")
                .arg(targetSet->displayName(), origDisplayName),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            revertSetName(item, origDisplayName);
            return;
        }
        // A merge is always the all-movie-files rename and the setting does not govern
        // it, because there is no other way to perform one.  Membership lives in the
        // member movies' NFOs (D-A), so moving a movie into another set *is* rewriting
        // its `<set><name>`; a `set.nfo` cannot say which movies belong to a set, so a
        // "set file only" merge would write a display title and quietly not merge.
        performMerge(item, origSet, targetSet, origSetName, newName);
        return;
    }

    switch (setModel->renameMode()) {
    case MovieSetModel::RenameMode::Unavailable:
        // The user asked for a set-file-only rename and there is no `set.nfo` for the
        // display title to live in.  Refused rather than quietly performed the other
        // way: see MovieSetModel::resolveRenameMode().
        qCWarning(generic) << "[SetsWidget] Movie set" << origDisplayName
                           << "was not renamed: a set-file-only rename needs a movie set information folder and"
                           << "none is configured.";
        NotificationBox::instance()->showError(
            tr("<b>\"%1\"</b> was not renamed: renaming only the movie set file needs a movie set information "
               "folder, and none is configured. Choose one in the settings, or set renaming to \"all movie "
               "files\".")
                .arg(origDisplayName));
        revertSetName(item, origDisplayName);
        return;

    case MovieSetModel::RenameMode::SetFileOnly: performSetFileOnlyRename(item, origSet, origSetName, newName); return;

    case MovieSetModel::RenameMode::AllMovieFiles: performAllMovieFilesRename(item, origSet, origSetName, newName);
        return;
    }
}

/// \brief Whether any set other than \p except answers to \p name, by either of its names.
/// \details One derivation, used by refuseIfNameIsTaken() below and by *Add Movie Set*'s
///          uniquifier.  Two of them is how they came apart in the first place: the
///          uniquifier asked MovieSetModel::set(), which matches keys only, so a set whose
///          *display title* was already "New Movie Set" let it create the very pair of
///          indistinguishable rows the rename refusal exists to prevent.
///
///          Deliberately **not** applied to the paths that merely reflect what the files
///          already say -- MovieSetXmlReader, MovieSetModel::reload() and
///          MovieSetModel::assign().  Those represent a state on disk or a value a user
///          typed onto a movie's own `<set><name>`, and a set MediaElch refuses to show
///          is worse than two rows it explains: the model has to be able to hold whatever
///          the library asserts (D-A).  The guard belongs where MediaElch is the one
///          *choosing* a name -- a rename, and a set created from nothing.
bool SetsWidget::setNameIsTaken(const QString& name, const MovieSet* except) const
{
    for (const MovieSet* other : Manager::instance()->movieSetModel()->sets()) {
        if (other == except) {
            continue;
        }
        if (other->displayName() == name || other->name() == name) {
            return true;
        }
    }
    return false;
}

/// \brief Refuses \p newName if another set already answers to it, by either of its names.
/// \return Whether the rename was refused; the caller returns without doing anything.
/// \details The merge check in onSetNameChanged() asks MovieSetModel::set(), which
///          matches on the **key** alone -- so it catches only a name that is another
///          set's key, and a name that is another set's *display title* sails past it as
///          "not a merge".  That is right about Kodi (two sets sharing a display title
///          are still two sets) and wrong about this tab, which would then show two rows
///          the user cannot tell apart and could not rename unambiguously.
///
///          Both renames need it, not just the set-file-only one.  Under all movie files
///          the typed name becomes this set's key, and colliding with another set's
///          display title produces the same two identical rows -- permanently, since the
///          key is what the next reload rebuilds from.
bool SetsWidget::refuseIfNameIsTaken(QTableWidgetItem* item, MovieSet* origSet, const QString& newName)
{
    // origSet is never null: onSetNameChanged() has established that before dispatching.
    // It used to be, on the all-movie-files path, and the refusal then read
    // `"" was not renamed` and wiped the cell.
    if (!setNameIsTaken(newName, origSet)) {
        return false;
    }
    const QString origDisplayName = origSet->displayName();
    qCWarning(generic) << "[SetsWidget] Movie set" << origDisplayName << "was not renamed: another"
                       << "movie set is already called" << newName;
    NotificationBox::instance()->showError(
        tr("<b>\"%1\"</b> was not renamed: another movie set is already called <b>\"%2\"</b>.")
            .arg(origDisplayName, newName));
    revertSetName(item, origDisplayName);
    return true;
}

/// \brief Gives \p item the tooltip naming \p movieSet's key, or clears it.
/// \details Set wherever the divergence can appear or disappear, not only in loadSets():
///          a set-file-only rename *creates* the divergence, and a tooltip that only
///          arrives at the next reload is one the user does not have at the moment they
///          would ask what just happened.  D-B promises it is never hidden.
void SetsWidget::applyDivergenceTooltip(QTableWidgetItem* item, const MovieSet* movieSet)
{
    const bool diverged = movieSet != nullptr && movieSet->displayName() != movieSet->name();
    // Save and restore rather than block-then-unblock.  QObject::blockSignals() is a
    // plain flag, not a counter, so an unconditional unblock here re-enables signals for
    // whoever was already blocking them -- and loadSets() calls this from inside its own
    // blocked region, where every following setItem() would then re-enter
    // onSetNameChanged() and open a merge dialog per row.
    const bool wasBlocked = ui->sets->blockSignals(true);
    item->setToolTip(diverged
                         ? tr("The movie files say: %1\nOnly the movie set file carries the name above.")
                               .arg(movieSet->name())
                         : QString());
    ui->sets->blockSignals(wasBlocked);
}

void SetsWidget::performSetFileOnlyRename(
    QTableWidgetItem* item, MovieSet* origSet, const QString& origSetName, const QString& newName)
{
    // origSet is not null and newName is not empty: onSetNameChanged() establishes both
    // for all three operations before it dispatches.
    if (refuseIfNameIsTaken(item, origSet, newName)) {
        return;
    }

    // The whole rename.  No movie is touched -- their `<set><name>` is the join key and
    // stays exactly where it is -- and neither is the set's folder, which Kodi derives
    // from that same key.  What changes is `set.nfo`'s `<title>`, on the next save.
    origSet->setTitle(newName);

    // The divergence exists as of the line above, so the tooltip that explains it does
    // too, rather than waiting for the next loadSets().
    applyDivergenceTooltip(item, origSet);

    // The row keeps its key.  Only the text moved, and it already has.
    loadSet(origSetName);
}

void SetsWidget::performAllMovieFilesRename(
    QTableWidgetItem* item, MovieSet* origSet, const QString& origSetName, const QString& newName)
{
    // Same precondition as the other two; see onSetNameChanged().
    if (refuseIfNameIsTaken(item, origSet, newName)) {
        return;
    }

    // A second row showing the new name is one left over from a set the model no longer
    // has; the merge case never reaches here.
    for (int i = 0, n = ui->sets->rowCount(); i < n; ++i) {
        if (i != item->row() && ui->sets->item(i, 0)->data(Qt::UserRole).toString() == newName) {
            ui->sets->removeRow(i);
            break;
        }
    }

    // Before the movies are reassigned, and that ordering is required rather than
    // tidy: in the artwork-next-to-movies layout the artwork's path is found through a
    // movie whose `set().name` is still the old one, so afterwards there is nothing to
    // resolve it from.  This moves the set's `set.nfo` and every file in its folder --
    // not the poster and backdrop this widget happens to know about, and not through a
    // decode and re-encode, which is what carrying them in memory used to cost.
    auto* mediaCenter = Manager::instance()->mediaCenterInterface();
    using MovieSetFileMove = MediaCenterInterface::MovieSetFileMove;
    const MovieSetFileMove filesMoved = origSetName.isEmpty()
                                            ? MovieSetFileMove::Moved
                                            : mediaCenter->renameMovieSetFiles(origSetName, newName);

    if (!m_moviesToSave.contains(newName)) {
        m_moviesToSave.insert(newName, QVector<Movie*>());
    }

    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    const QVector<Movie*> members = origSet->movies();

    // Rename the set object before its movies, so that the object -- and with it the
    // set's overview, id and artwork -- is kept instead of being emptied and a second
    // one created under the new name.
    origSet->setName(newName);

    for (Movie* movie : members) {
        m_moviesToSave[newName].append(movie);
        setModel->assign(movie, movie->set().renamedTo(newName));
    }
    m_moviesToSave[origSetName].clear();

    // Images that are pending in this widget and nowhere else -- downloaded and not yet
    // saved -- move with the set.  What is already on disk was moved above, so it is
    // deliberately *not* read back in here: doing that re-encoded a PNG to JPEG and
    // left the original behind as an orphan.
    carrySetArtworkOver(origSetName, newName);

    // The row is the renamed set now; saveSet(), the artwork dialogs and a second rename
    // of the same row look it up by this role.
    ui->sets->blockSignals(true);
    item->setData(Qt::UserRole, newName);
    ui->sets->blockSignals(false);
    // setName() re-unified the two names, so whatever divergence this row showed is gone.
    applyDivergenceTooltip(item, origSet);

    // The rename happened either way.  The movie NFOs are the set's identity, so a rename
    // whose files could not follow is a rename plus a findable leftover -- the same shape
    // as the merge leftover below, and for the same reason: undoing it would mean
    // rewriting every member again, which is larger and riskier than saying so.
    //
    // Which leftover, though, is not one sentence.  In the separate-folder layout the
    // directory rename can succeed and the artwork rename inside it fail, and then the
    // set's record *is* under the new name -- telling that user their files are "still
    // stored under the old name" sends them to a folder that no longer exists.
    switch (filesMoved) {
    case MovieSetFileMove::Moved: break;

    case MovieSetFileMove::NotMoved:
        qCWarning(generic) << "[SetsWidget] Movie set" << origSetName << "was renamed to" << newName
                           << "but its files could not be moved at all; they are still under the old name.";
        NotificationBox::instance()->showWarning(
            tr("<b>\"%1\"</b> was renamed, but its movie set file and artwork could not be moved. They are still "
               "stored under <b>\"%2\"</b>.")
                .arg(newName, origSetName));
        break;

    case MovieSetFileMove::PartlyMoved:
        qCWarning(generic) << "[SetsWidget] Movie set" << origSetName << "was renamed to" << newName
                           << "but only some of its files moved; part of its artwork still carries the old name.";
        NotificationBox::instance()->showWarning(
            tr("<b>\"%1\"</b> was renamed, but only some of its files could be moved. Part of its artwork still "
               "carries the old name, so MediaElch will not find it. Check the log for which files.")
                .arg(newName));
        break;
    }

    loadSet(newName);
}

void SetsWidget::performMerge(
    QTableWidgetItem* item, MovieSet* origSet, MovieSet* targetSet, const QString& origSetName, const QString& newName)
{
    // The row being merged into goes; this row becomes it.
    for (int i = 0, n = ui->sets->rowCount(); i < n; ++i) {
        if (i != item->row() && ui->sets->item(i, 0)->data(Qt::UserRole).toString() == targetSet->name()) {
            ui->sets->removeRow(i);
            break;
        }
    }

    if (!m_moviesToSave.contains(newName)) {
        m_moviesToSave.insert(newName, QVector<Movie*>());
    }

    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    // Read before removeSet(), which destroys origSet when it succeeds.  Both messages
    // below name sets the user has to recognise, and after a set-file-only rename that
    // is the display title -- naming them by key would print two different strings for
    // the same set in one merge, since the confirmation dialog and the row both use the
    // display title.  `newName` is the target's key by construction, because the merge
    // check matched on it.
    const QString origDisplayName = origSet->displayName();
    const QString targetDisplayName = targetSet->displayName();

    const QVector<Movie*> members = origSet->movies();
    for (Movie* movie : members) {
        m_moviesToSave[newName].append(movie);
        // Deliberately name-only: the movies end up in a collection that is not the one
        // their old overview and id describe, so those must not travel with them.
        MovieSetInfo set;
        set.name = targetSet->name();
        setModel->assign(movie, set);
    }
    m_moviesToSave[origSetName].clear();

    // The source set's files go through the deliberate removal path rather than being
    // moved: there is nothing for them to be moved to, because the target set already
    // has its own record and its own folder.
    if (!setModel->removeSet(origSetName)) {
        // Its movies are in the set they were merged into now, so the merge itself
        // happened; what could not go is the emptied source set, because its `set.nfo`
        // could not be removed.  It stays, backed and memberless, and the sets tab's
        // "Show Only Empty Movie Sets" is how the user finds it.  Undoing the merge
        // instead would mean putting every movie back, which is a larger and riskier
        // operation than leaving a findable leftover.  This is a merge that finished
        // plus a leftover, not a half-done one.
        //
        // The table has to be told, or the warning names a set the user cannot see.  The
        // dedupe loop above took the target's row away and this row now carries the
        // target's name, so nothing here shows the source set at all while the model
        // still holds it.  Signals are blocked because inserting an item is an
        // itemChanged, which is the signal this very slot is connected to.
        //
        // Deliberately not loadSets(): that clears m_moviesToSave, and the movies this
        // merge just reassigned are sitting in it waiting to be written.
        ui->sets->blockSignals(true);
        const int leftoverRow = ui->sets->rowCount();
        ui->sets->insertRow(leftoverRow);
        ui->sets->setItem(leftoverRow, 0, new QTableWidgetItem(origDisplayName));
        ui->sets->item(leftoverRow, 0)->setData(Qt::UserRole, origSetName);
        ui->sets->blockSignals(false);
        // The leftover is a real row for a set that still exists, so it needs the same
        // explanation of a divergence as any other -- it is a third place a row's
        // identity is decided, alongside loadSets() and the two renames.
        applyDivergenceTooltip(ui->sets->item(leftoverRow, 0), setModel->set(origSetName));

        qCWarning(generic) << "[SetsWidget] Movies were merged into" << targetSet->name() << "but the source set"
                           << origSetName << "could not be removed; it is still there with no movies.";
        NotificationBox::instance()->showWarning(
            tr("The movies were merged into <b>\"%1\"</b>, but the old set's movie set file could not be "
               "removed, so <b>\"%2\"</b> is still there with no movies.")
                .arg(targetDisplayName, origDisplayName));
    }

    // A merge carries no artwork: the set the movies joined has its own, and the source
    // set's images belong to a set that is being deleted.
    m_setPosters.remove(origSetName);
    m_setBackdrops.remove(origSetName);

    ui->sets->blockSignals(true);
    item->setData(Qt::UserRole, targetSet->name());
    item->setText(targetSet->displayName());
    ui->sets->blockSignals(false);
    // This row is the target set now, so it carries the target's divergence and not the
    // source's.  Without this it kept the tooltip of a set that no longer exists --
    // actively false -- or hid a real divergence the target has.
    applyDivergenceTooltip(item, targetSet);

    loadSet(targetSet->name());
}

/// \brief Moves images this widget is holding unsaved from \p oldName's key to \p newName's.
/// \details Only what is pending here.  Anything already on disk is moved on disk by
///          MediaCenterInterface::renameMovieSetFiles(), losslessly and for every file in
///          the set's folder rather than for the two types this widget knows about.
void SetsWidget::carrySetArtworkOver(const QString& oldName, const QString& newName)
{
    const QImage poster = m_setPosters.value(oldName);
    const QImage backdrop = m_setBackdrops.value(oldName);
    m_setPosters.remove(oldName);
    m_setBackdrops.remove(oldName);
    if (!poster.isNull() || !m_setPosters.contains(newName)) {
        m_setPosters.insert(newName, poster);
    }
    if (!backdrop.isNull() || !m_setBackdrops.contains(newName)) {
        m_setBackdrops.insert(newName, backdrop);
    }
}

void SetsWidget::onDownloadFinished(DownloadManagerElement elem)
{
    QString setName = elem.movie->title();
    if (elem.imageType == ImageType::MovieSetPoster) {
        if (m_setPosters.contains(setName)) {
            m_setPosters[setName] = QImage::fromData(elem.data);
        }
        if (ui->sets->currentRow() >= 0 && ui->sets->currentRow() < ui->sets->rowCount()
            && ui->sets->item(ui->sets->currentRow(), 0)->data(Qt::UserRole).toString() == setName) {
            loadSet(setName);
        }
    } else if (elem.imageType == ImageType::MovieSetBackdrop) {
        if (m_setBackdrops.contains(setName)) {
            m_setBackdrops[setName] = QImage::fromData(elem.data);
        }
        if (ui->sets->currentRow() >= 0 && ui->sets->currentRow() < ui->sets->rowCount()
            && ui->sets->item(ui->sets->currentRow(), 0)->data(Qt::UserRole).toString() == setName) {
            loadSet(setName);
        }
    }
    delete elem.movie;
}

void SetsWidget::onShowOnlyEmptySets(bool onlyEmpty)
{
    m_showOnlyEmptySets = onlyEmpty;
    // Filtering is done while the list is built, so the list is simply built again.  It
    // also re-derives the library, which is the point: the sets that are empty *and*
    // have no record go at that moment, so what is left under the filter is exactly the
    // sets that survived on their own record.
    loadSets();
}

void SetsWidget::onSettingsSaved()
{
    // Settings::sigSettingsSaved means "settings were written", not "the movie set
    // directory changed" -- it also fires for the season order, the import dialogs and
    // the Kodi sync -- so this has to be cheap and has to compare before it acts.
    const bool wereConfigured = m_recordsAreConfigured;
    applyWriteAccess();

    if (!wereConfigured && m_recordsAreConfigured && isVisible()) {
        // Off to on.  The records have to be discovered: a set that has a `set.nfo` and
        // no member movie exists only once the folder has been listed, and the sets that
        // are already here need their record read rather than only counted.  Only
        // reload() does that.  Only when this tab is on screen, because entering it
        // reloads anyway (MainWindow::onMenu) and reload() costs a parse per record.
        loadSets();
    }

    // On to off does **not** reload, and that is the point rather than an omission.
    // reload() ends in dropEmptySets(), and with records off isBacked() is false for
    // every set, so every set that is standing on its `set.nfo` alone would be destroyed
    // at that moment -- while the user watches.  It happens at the next visit to this
    // tab either way, by design (a set is its movies again when there is no folder), but
    // nothing is gained by bringing it forward.  The sets' record flags are untouched
    // here as well, which is what lets turning the folder back on restore every set's
    // answer at once; see MovieSetModel::isBacked().
}

void SetsWidget::onJumpToMovie(QTableWidgetItem* item)
{
    if (item->column() != 0) {
        return;
    }

    auto* movie = item->data(Qt::UserRole).value<Movie*>();
    emit sigJumpToMovie(movie);
}
