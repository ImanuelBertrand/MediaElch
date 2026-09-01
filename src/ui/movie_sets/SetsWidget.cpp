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

    QStringList setNames;
    for (const MovieSet* movieSet : setModel->sets()) {
        if (m_showOnlyEmptySets && !movieSet->movies().isEmpty()) {
            continue;
        }
        setNames.append(movieSet->name());
    }
    setNames.sort();

    for (const QString& setName : asConst(setNames)) {
        m_moviesToSave.insert(setName, QVector<Movie*>());
        m_setPosters.insert(setName, QImage());
        m_setBackdrops.insert(setName, QImage());

        int row = ui->sets->rowCount();
        ui->sets->insertRow(row);
        ui->sets->setItem(row, 0, new QTableWidgetItem(setName));
        ui->sets->item(row, 0)->setData(Qt::UserRole, setName);
    }
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

    QString setName = ui->sets->item(ui->sets->currentRow(), 0)->text();
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
    ui->setName->setText(set);
    ui->buttonPreviewBackdrop->setEnabled(false);
    ui->buttonPreviewPoster->setEnabled(false);
    ui->movies->blockSignals(true);

    const MovieSet* movieSet = Manager::instance()->movieSetModel()->set(set);
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

    QString setName = ui->sets->item(ui->sets->currentRow(), 0)->text();
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
        // Logged at info rather than debug so that a test can see it: removing this
        // guard to watch a test fail would open a modal dialog and hang instead.
        qCInfo(generic) << "[SetsWidget] Not choosing set artwork: it has nowhere to be written.";
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
        qCInfo(generic) << "[SetsWidget] Not choosing set artwork: it has nowhere to be written.";
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

    QStringList setNames;
    setNames << ui->sets->item(ui->sets->currentRow(), 0)->data(Qt::UserRole).toString();
    setNames << ui->sets->item(ui->sets->currentRow(), 0)->text();
    setNames.removeDuplicates();

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

    // The set's own record.  Its overview, collection id and artwork are authoritative
    // in `set.nfo` (D-A), so saving a set means writing that file -- and writing it is
    // also what gives the set an existence apart from its movies, so that an empty one
    // is still there after the next reload.
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
    const QString currentName = ui->sets->item(ui->sets->currentRow(), 0)->text();
    MovieSet* movieSet = Manager::instance()->movieSetModel()->set(currentName);
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
                           << "was not saved: its artwork could not be written, and neither could its movie"
                           << "set file.";
        NotificationBox::instance()->showError(
            tr("<b>\"%1\"</b>: the movies were saved, but the artwork and the movie set file could not be "
               "written.")
                .arg(currentName));
        return;
    }
    if (!recordSaved) {
        // The movies and the artwork above were saved; only the set's own file was not.
        qCWarning(generic) << "[SetsWidget] Movie set" << currentName
                           << "was not saved: its movie set file could not be written.";
        NotificationBox::instance()->showError(
            tr("<b>\"%1\"</b>: the movies were saved, but the movie set file could not be written.").arg(currentName));
        return;
    }
    if (!artworkSaved) {
        qCWarning(generic) << "[SetsWidget] Movie set" << currentName
                           << "was not saved: its artwork could not be written.";
        NotificationBox::instance()->showError(
            tr("<b>\"%1\"</b>: the movies were saved, but the artwork could not be written.").arg(currentName));
        return;
    }

    NotificationBox::instance()->showSuccess(tr("<b>\"%1\"</b> Saved").arg(currentName));
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
    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    const QString baseName = tr("New Movie Set");
    QString setName = baseName;
    int adder = 0;
    while (setModel->set(setName) != nullptr) {
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
    m_setPosters.remove(setName);
    m_setBackdrops.remove(setName);
}

void SetsWidget::onSetNameChanged(QTableWidgetItem* item)
{
    QString newName = item->text();
    QString origSetName = item->data(Qt::UserRole).toString();
    if (newName == origSetName) {
        return;
    }

    // Renaming a set to the name of an existing one merges the two.  The movies then
    // end up in a collection that is not the one their overview and id describe.
    //
    // Whether this is a rename or a merge is the model's answer, not the table's.  The
    // table is the snapshot the last loadSets() took, and the model can hold a set it
    // does not show; deciding from the table would then rename A to B while a second
    // set called B is still in the model, and a set's name is its primary key (D-B).
    // MovieSet::setName() is public and does not check, so the model cannot catch it
    // afterwards either.
    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    MovieSet* origSet = setModel->set(origSetName);
    MovieSet* targetSet = setModel->set(newName);
    const bool mergesIntoExistingSet = targetSet != nullptr && targetSet != origSet;

    // A second row showing the new name goes either way: it is the row being merged
    // into, or a row left over from a set the model no longer has.
    for (int i = 0, n = ui->sets->rowCount(); i < n; ++i) {
        if (i != item->row() && ui->sets->item(i, 0)->text() == newName) {
            ui->sets->removeRow(i);
            break;
        }
    }

    // Artwork is stored under the set's name, so carry it over; saveSet() writes it under the
    // new name.  Must be read before the movies are renamed: KodiXml::movieSetFileName() finds
    // "artwork next to movies" through a movie of the set -- and for an empty name, through an
    // arbitrary movie that has no set at all, which is why empty names carry nothing.
    QImage poster;
    QImage backdrop;
    if (!mergesIntoExistingSet && !origSetName.isEmpty() && !newName.isEmpty()) {
        auto* mediaCenter = Manager::instance()->mediaCenterInterface();
        poster = m_setPosters.value(origSetName);
        if (poster.isNull()) {
            poster = mediaCenter->movieSetPoster(origSetName);
        }
        backdrop = m_setBackdrops.value(origSetName);
        if (backdrop.isNull()) {
            backdrop = mediaCenter->movieSetBackdrop(origSetName);
        }
    }

    if (!m_moviesToSave.contains(newName)) {
        m_moviesToSave.insert(newName, QVector<Movie*>());
    }

    const QVector<Movie*> members = (origSet != nullptr) ? origSet->movies() : QVector<Movie*>();

    // Rename the set object before its movies, so that a plain rename keeps the object
    // -- and with it the set's overview, id and artwork -- instead of emptying it and
    // creating a second one under the new name.  A merge cannot: the object it merges
    // into already has that name.
    if (origSet != nullptr && !mergesIntoExistingSet && !newName.isEmpty()) {
        origSet->setName(newName);
    }

    for (Movie* movie : members) {
        m_moviesToSave[newName].append(movie);
        if (mergesIntoExistingSet) {
            MovieSetInfo set;
            set.name = newName;
            setModel->assign(movie, set);
        } else {
            setModel->assign(movie, movie->set().renamedTo(newName));
        }
    }

    m_moviesToSave[origSetName].clear();

    if (mergesIntoExistingSet && !setModel->removeSet(origSetName)) {
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
        ui->sets->setItem(leftoverRow, 0, new QTableWidgetItem(origSetName));
        ui->sets->item(leftoverRow, 0)->setData(Qt::UserRole, origSetName);
        ui->sets->blockSignals(false);

        NotificationBox::instance()->showWarning(
            tr("The movies were merged into <b>\"%1\"</b>, but the old set's movie set file could not be "
               "removed, so <b>\"%2\"</b> is still there with no movies.")
                .arg(newName, origSetName));
    }

    m_setPosters.remove(origSetName);
    m_setBackdrops.remove(origSetName);
    if (!m_setPosters.contains(newName)) {
        m_setPosters.insert(newName, poster);
    }
    if (!m_setBackdrops.contains(newName)) {
        m_setBackdrops.insert(newName, backdrop);
    }

    // The row is the renamed set now; saveSet(), the artwork dialogs and a second rename of the
    // same row look it up by this role.
    ui->sets->blockSignals(true);
    item->setData(Qt::UserRole, newName);
    ui->sets->blockSignals(false);

    loadSet(newName);
}

void SetsWidget::onDownloadFinished(DownloadManagerElement elem)
{
    QString setName = elem.movie->title();
    if (elem.imageType == ImageType::MovieSetPoster) {
        if (m_setPosters.contains(setName)) {
            m_setPosters[setName] = QImage::fromData(elem.data);
        }
        if (ui->sets->currentRow() >= 0 && ui->sets->currentRow() < ui->sets->rowCount()
            && ui->sets->item(ui->sets->currentRow(), 0)->text() == setName) {
            loadSet(setName);
        }
    } else if (elem.imageType == ImageType::MovieSetBackdrop) {
        if (m_setBackdrops.contains(setName)) {
            m_setBackdrops[setName] = QImage::fromData(elem.data);
        }
        if (ui->sets->currentRow() >= 0 && ui->sets->currentRow() < ui->sets->rowCount()
            && ui->sets->item(ui->sets->currentRow(), 0)->text() == setName) {
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
