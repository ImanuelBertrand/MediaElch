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
    // Without this, the tooltip that says why *Add Movie Set* is disabled is never shown.
    m_tableContextMenu->setToolTipsVisible(true);
    m_actionAddSet = new QAction(tr("Add Movie Set"), this);
    m_actionAddSet->setObjectName("actionAddMovieSet");
    auto* actionDeleteSet = new QAction(tr("Delete Movie Set"), this);
    actionDeleteSet->setObjectName("actionDeleteMovieSet");
    // A set with a `set.nfo` stays in the list after its last movie leaves it, and nothing
    // about its row says so.
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
    // The settings can be saved while this tab is on screen; there is no signal for the
    // movie set directory in particular.
    connect(Settings::instance(), &Settings::sigSettingsSaved, this, &SetsWidget::onSettingsSaved);
    connect(ui->folderNotice, &QLabel::linkActivated, this, [this](const QString& /*link*/) {
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
    // The model's answer rather than the media center's, so that this guard and the model's
    // own drop rule cannot come apart.
    const bool recordsEnabled = Manager::instance()->movieSetModel()->recordsAreConfigured();
    // Not the same question: artwork resolves in both layouts, a record only in one.
    const bool artworkEnabled = Manager::instance()->mediaCenterInterface()->movieSetArtworkEnabled();
    m_recordsAreConfigured = recordsEnabled;

    m_actionAddSet->setEnabled(recordsEnabled);
    m_actionAddSet->setToolTip(recordsEnabled
                                   ? QString()
                                   : tr("A movie set needs a movie set directory to be remembered in. Choose one "
                                        "under Settings, Movies, Movie Set Artwork."));

    // Deliberately no tooltip: a disabled widget gets no hover, so the notice below carries
    // the explanation.  chooseSetPoster() and chooseSetBackdrop() refuse a second time.
    ui->poster->setEnabled(artworkEnabled);
    ui->backdrop->setEnabled(artworkEnabled);

    // Three states, derived from the two answers above rather than from a third look at the
    // settings.
    if (recordsEnabled) {
        ui->folderNoticeFrame->hide();

    } else if (!artworkEnabled) {
        // A real misconfiguration: the user asked for a directory and never named one.  The
        // notice says what still works too, since membership and names live in the movies.
        ui->folderNoticeFrame->setFrameShape(QFrame::StyledPanel);
        ui->folderNotice->setText(
            tr("<b>No movie set directory is configured.</b> Set artwork cannot be saved, movie sets get no file "
               "of their own, and a movie set with no movies cannot be created. Renaming a set, adding and "
               "removing movies and the sort title still work: those are stored in the movies themselves. "
               "<a href=\"settings\">Choose a directory</a> under Settings, Movies, Movie Set Artwork, or "
               "switch back to \"Artwork next to movies\"."));
        ui->folderNoticeFrame->show();

    } else {
        // Not a warning, and it must not read like one: "artwork next to movies" is the
        // default, so every user who has never opened the settings sees this line.
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
 * \details The set list belongs to MovieSetModel; this only reads it.  The widget's own maps
 *          hold raw Movie*, so they are cleared before ui->sets is.
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

    // Also the moment at which a set with neither movies nor a `set.nfo` is dropped, so a
    // set added by the context menu survives only once saving has written its record.
    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    setModel->reload();

    // Qt::UserRole carries the set's match key and the cell text the display title, which
    // differ after a set-file-only rename.  Every lookup and all three maps go by the key.
    QVector<QPair<QString, QString>> setRows; // (match key, display title)
    for (const MovieSet* movieSet : setModel->sets()) {
        if (m_showOnlyEmptySets && !movieSet->movies().isEmpty()) {
            continue;
        }
        setRows.append({movieSet->name(), movieSet->displayName()});
    }
    // Sorted by what the user reads, with the key breaking a tie so that rows do not reorder
    // under the user while one set is being renamed onto another's title.
    std::sort(setRows.begin(), setRows.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.second != rhs.second ? lhs.second < rhs.second : lhs.first < rhs.first;
    });

    // Signals blocked for the whole population: setItem() and setToolTip() are both an
    // itemChanged, and the row has no Qt::UserRole yet when the first of them fires.
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
    // \p set is the match key; the heading is read by a person, so it shows the display title.
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

    // The match key: this value is written onto the movie's own NFO as `<set><name>`.
    QString setName = ui->sets->item(ui->sets->currentRow(), 0)->data(Qt::UserRole).toString();
    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    for (Movie* movie : asConst(movies)) {
        if (movie->set().name == setName) {
            continue;
        }
        // Name only: the movie joins a different collection, so its previous set's overview
        // and id must not travel with it.  That is why the name check above, and not
        // assign()'s whole-value guard, is what leaves an existing member alone.
        MovieSetInfo set;
        set.name = setName;
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
        // applyWriteAccess() has already disabled the label this is reached from; refusing
        // here too keeps a download from starting for an image that could not be written.
        // Info, not debug: the log line is all the test can hold this refusal by.
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
        // See chooseSetPoster(), including why this one refusal logs at info and not debug.
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

    // The match key alone: all three of this widget's maps are keyed by it, and the cell's
    // text is the display title, a string they have never held.
    const QStringList setNames{ui->sets->item(ui->sets->currentRow(), 0)->data(Qt::UserRole).toString()};

    // A set's poster and backdrop live nowhere but the two maps below until they are
    // written, so an entry is only cleared for a write that actually happened.
    bool artworkSaved = true;
    MediaCenterInterface* mediaCenter = Manager::instance()->mediaCenterInterface();

    for (const QString& setName : asConst(setNames)) {
        for (Movie* movie : asConst(m_moviesToSave[setName])) {
            movie->controller()->saveData(mediaCenter);
        }
        m_moviesToSave[setName].clear();

        // A set without a name has no path of its own: movieSetFileName() collapses to the
        // artwork directory itself, or to the first movie that has no set.
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

    // `set.nfo` holds the authoritative overview and collection id, so saving a set writes it
    // too.  The lookup goes through the match key: by the displayed title it would miss a renamed
    // set, leaving movieSet null and short-circuiting recordSaved to true.
    const QString currentName = ui->sets->item(ui->sets->currentRow(), 0)->data(Qt::UserRole).toString();
    MovieSet* movieSet = Manager::instance()->movieSetModel()->set(currentName);
    // The messages below name a set the user has to recognise, so they use the title.
    const QString displayedName = movieSet != nullptr ? movieSet->displayName() : currentName;
    // saveMovieSet() answers false both for a failed write and for "there are no records
    // here", so without this every Save in the default layout would report an error.
    const bool recordsEnabled = Manager::instance()->movieSetModel()->recordsAreConfigured();
    const bool recordSaved = !recordsEnabled || movieSet == nullptr || mediaCenter->saveMovieSet(*movieSet);

    // Three whole sentences rather than one assembled from fragments, for the translators.
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
    // Behind the disabled action rather than instead of it: a set created here has no members
    // and, without a movie set directory, can never get a `set.nfo`, so the next reload()
    // would drop it in front of the user.  Naming a new set on a movie stays allowed.
    if (!Manager::instance()->movieSetModel()->recordsAreConfigured()) {
        qCDebug(generic) << "[SetsWidget] Not adding a movie set: no movie set directory is configured, so the"
                         << "set could not be remembered and would go at the next reload.";
        return;
    }
    // Asked of the model, not of the table, which is a filtered snapshot: a name absent from
    // the rows can still be taken.  By either name, like the rename refusal.
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

    // The model goes first and the row only follows if it agreed: removeSet() refuses to
    // destroy a set whose `set.nfo` it could not remove.
    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    const MovieSet* movieSet = setModel->set(origSetName);
    const QVector<Movie*> members = (movieSet != nullptr) ? movieSet->movies() : QVector<Movie*>();

    if (!setModel->removeSet(origSetName)) {
        NotificationBox::instance()->showError(
            tr("<b>\"%1\"</b> could not be deleted: its movie set file could not be removed.").arg(setName));
        return;
    }

    // removeSet() marks the detached movies changed; the sort title is this tab's own doing.
    // Read before the removal, which empties the set, and applied after it, so that a refusal
    // leaves the movies untouched too.
    for (Movie* movie : members) {
        movie->setSortTitle("");
    }
    ui->sets->removeRow(ui->sets->currentRow());
    // By the match key: `setName` above is what the cell displays, which after a
    // set-file-only rename is a name these maps have never held.
    m_setPosters.remove(origSetName);
    m_setBackdrops.remove(origSetName);
}

/// \brief Puts the row's text back to what the set is actually called; used when a rename
///        is refused.
void SetsWidget::revertSetName(QTableWidgetItem* item, const QString& name)
{
    // Blocked because setText() is an itemChanged, which would re-enter onSetNameChanged();
    // saved and restored for the reason given in applyDivergenceTooltip().
    const bool wasBlocked = ui->sets->blockSignals(true);
    item->setText(name);
    ui->sets->blockSignals(wasBlocked);
}

void SetsWidget::onSetNameChanged(QTableWidgetItem* item)
{
    // What the user typed is a display name; whether it also becomes the set's match key is
    // what the rename mode decides.  See docs/concepts/movie-sets.md.
    const QString newName = item->text();
    const QString origSetName = item->data(Qt::UserRole).toString();

    // Rename or merge is the model's answer, not the table's: the table is the snapshot the
    // last loadSets() took and the model can hold a set it does not show.
    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    MovieSet* origSet = setModel->set(origSetName);
    const QString origDisplayName = origSet != nullptr ? origSet->displayName() : origSetName;
    if (newName == origDisplayName) {
        return;
    }

    // Both preconditions are checked here rather than inside each of the three operations
    // below.  The empty-name one is safe above the merge check: addSet() refuses the empty
    // name, so no set is keyed by it and it can never be a merge target.
    if (origSet == nullptr) {
        // A row for a set the model no longer has: not the user's doing, so revert quietly.
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
        // Asked, not assumed: a merge moves movies between sets, is not undoable and is one
        // typo in a table cell away.
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
        // Always the all-movie-files rename, whatever the setting says: membership lives in
        // the member movies' NFOs, so a "set file only" merge would not merge at all.
        performMerge(item, origSet, targetSet, origSetName, newName);
        return;
    }

    switch (setModel->renameMode()) {
    case MovieSetModel::RenameMode::Unavailable:
        // A set-file-only rename with no `set.nfo` for the display title to live in;
        // refused rather than quietly performed the other way.
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
/// \details Used by refuseIfNameIsTaken() below and by *Add Movie Set*'s uniquifier.
///          Deliberately not applied to the paths that merely reflect what the files say:
///          the model has to be able to hold whatever the library asserts.
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
/// \details Needed by both renames.  The merge check in onSetNameChanged() matches on the
///          key alone, so a name that is another set's display title sails past it as "not
///          a merge" and would leave two rows the user cannot tell apart.
bool SetsWidget::refuseIfNameIsTaken(QTableWidgetItem* item, MovieSet* origSet, const QString& newName)
{
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
/// \details Called wherever the divergence can appear or disappear, not only in loadSets():
///          a set-file-only rename creates it, and the user needs the explanation then.
void SetsWidget::applyDivergenceTooltip(QTableWidgetItem* item, const MovieSet* movieSet)
{
    const bool diverged = movieSet != nullptr && movieSet->displayName() != movieSet->name();
    // Saved and restored rather than blocked and unblocked: blockSignals() is a flag, not a
    // counter, and loadSets() calls this from inside its own blocked region.
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
    if (refuseIfNameIsTaken(item, origSet, newName)) {
        return;
    }

    // The whole rename: no movie is touched, and neither is the set's folder, which Kodi
    // derives from the unchanged key.  Only `set.nfo`'s `<title>` moves, on the next save.
    origSet->setTitle(newName);

    applyDivergenceTooltip(item, origSet);

    // The row keeps its key; only the text moved, and it already has.
    loadSet(origSetName);
}

void SetsWidget::performAllMovieFilesRename(
    QTableWidgetItem* item, MovieSet* origSet, const QString& origSetName, const QString& newName)
{
    if (refuseIfNameIsTaken(item, origSet, newName)) {
        return;
    }

    // A second row showing the new name is one left over from a set the model no longer has;
    // the merge case never reaches here.
    for (int i = 0, n = ui->sets->rowCount(); i < n; ++i) {
        if (i != item->row() && ui->sets->item(i, 0)->data(Qt::UserRole).toString() == newName) {
            ui->sets->removeRow(i);
            break;
        }
    }

    // Before the movies are reassigned: in the artwork-next-to-movies layout the artwork's
    // path is resolved through a movie whose `set().name` is still the old one.
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

    // Through the model, which owns the uniqueness of the key.  Before its movies, so that the
    // object -- and with it the set's overview and id -- is kept instead of being emptied and a
    // second one created under the new name.  It cannot refuse here: refuseIfNameIsTaken()
    // above turned a taken name away, before anything was moved on disk.
    const bool renamed = setModel->renameSet(origSet, newName);
    Q_UNUSED(renamed)

    for (Movie* movie : members) {
        m_moviesToSave[newName].append(movie);
        setModel->assign(movie, movie->set().renamedTo(newName));
    }
    m_moviesToSave[origSetName].clear();

    // Only the images pending in this widget; what is already on disk was moved above and is
    // deliberately not read back in, which would re-encode it and orphan the original.
    carrySetArtworkOver(origSetName, newName);

    ui->sets->blockSignals(true);
    item->setData(Qt::UserRole, newName);
    ui->sets->blockSignals(false);
    // setName() re-unified the two names, so whatever divergence this row showed is gone.
    applyDivergenceTooltip(item, origSet);

    // The rename happened either way -- undoing it would mean rewriting every member again --
    // so a failed file move is reported rather than reverted.
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
    // Read before removeSet(), which destroys origSet when it succeeds.  Display titles,
    // because the confirmation dialog and the rows name the sets the same way.
    const QString origDisplayName = origSet->displayName();
    const QString targetDisplayName = targetSet->displayName();

    const QVector<Movie*> members = origSet->movies();
    for (Movie* movie : members) {
        m_moviesToSave[newName].append(movie);
        // Name only: the movies end up in a collection that is not the one their old
        // overview and id describe.
        MovieSetInfo set;
        set.name = targetSet->name();
        setModel->assign(movie, set);
    }
    m_moviesToSave[origSetName].clear();

    // Removed rather than moved: the target set already has its own record and folder.
    if (!setModel->removeSet(origSetName)) {
        // The merge itself happened; only the emptied source set could not go.  It gets a row
        // back, or the warning below names a set the user cannot see.  Deliberately not
        // loadSets(), which would clear the just-reassigned movies out of m_moviesToSave.
        ui->sets->blockSignals(true);
        const int leftoverRow = ui->sets->rowCount();
        ui->sets->insertRow(leftoverRow);
        ui->sets->setItem(leftoverRow, 0, new QTableWidgetItem(origDisplayName));
        ui->sets->item(leftoverRow, 0)->setData(Qt::UserRole, origSetName);
        ui->sets->blockSignals(false);
        applyDivergenceTooltip(ui->sets->item(leftoverRow, 0), setModel->set(origSetName));

        qCWarning(generic) << "[SetsWidget] Movies were merged into" << targetSet->name() << "but the source set"
                           << origSetName << "could not be removed; it is still there with no movies.";
        NotificationBox::instance()->showWarning(
            tr("The movies were merged into <b>\"%1\"</b>, but the old set's movie set file could not be "
               "removed, so <b>\"%2\"</b> is still there with no movies.")
                .arg(targetDisplayName, origDisplayName));
    }

    // A merge carries no artwork: the set the movies joined has its own.
    m_setPosters.remove(origSetName);
    m_setBackdrops.remove(origSetName);

    ui->sets->blockSignals(true);
    item->setData(Qt::UserRole, targetSet->name());
    item->setText(targetSet->displayName());
    ui->sets->blockSignals(false);
    // This row is the target set now, so it carries the target's divergence, not the source's.
    applyDivergenceTooltip(item, targetSet);

    loadSet(targetSet->name());
}

/// \brief Moves images this widget is holding unsaved from \p oldName's key to \p newName's.
/// \details Only what is pending here; anything already on disk is moved by
///          MediaCenterInterface::renameMovieSetFiles().
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
    // Filtering happens while the list is built, so the list is simply built again.
    loadSets();
}

void SetsWidget::onSettingsSaved()
{
    // sigSettingsSaved means "settings were written", not "the movie set directory changed",
    // so this has to be cheap and has to compare before it acts.
    const bool wereConfigured = m_recordsAreConfigured;
    applyWriteAccess();

    if (!wereConfigured && m_recordsAreConfigured && isVisible()) {
        // Off to on: only reload() discovers the records, and only while this tab is on
        // screen, because entering it reloads anyway and reload() costs a parse per record.
        loadSets();
    }

    // On to off deliberately does not reload: reload() ends in dropEmptySets(), which with
    // records off would destroy every set standing on its `set.nfo` alone.
}

void SetsWidget::onJumpToMovie(QTableWidgetItem* item)
{
    if (item->column() != 0) {
        return;
    }

    auto* movie = item->data(Qt::UserRole).value<Movie*>();
    emit sigJumpToMovie(movie);
}
