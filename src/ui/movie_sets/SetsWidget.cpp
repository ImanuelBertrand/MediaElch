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
#include "ui/UiUtils.h"
#include "ui/image/ImageDialog.h"
#include "ui/image/ImagePreviewDialog.h"
#include "ui/main/MainWindow.h"
#include "ui/movie_sets/MovieListDialog.h"
#include "ui/notifications/NotificationBox.h"

#include <QFileDialog>
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
    auto* actionAddSet = new QAction(tr("Add Movie Set"), this);
    auto* actionDeleteSet = new QAction(tr("Delete Movie Set"), this);
    // A set with a `set.nfo` stays in the list after its last movie leaves it, so the
    // list can hold sets nothing in the library points at.  Nothing about a row says so,
    // and a user with a hundred sets cannot find them by opening each one.
    auto* actionOnlyEmptySets = new QAction(tr("Show Only Empty Movie Sets"), this);
    actionOnlyEmptySets->setCheckable(true);
    m_tableContextMenu->addAction(actionAddSet);
    m_tableContextMenu->addAction(actionDeleteSet);
    m_tableContextMenu->addSeparator();
    m_tableContextMenu->addAction(actionOnlyEmptySets);
    connect(actionAddSet, &QAction::triggered, this, &SetsWidget::onAddMovieSet);
    connect(actionDeleteSet, &QAction::triggered, this, &SetsWidget::onRemoveMovieSet);
    connect(actionOnlyEmptySets, &QAction::toggled, this, &SetsWidget::onShowOnlyEmptySets);
    connect(ui->sets, &QWidget::customContextMenuRequested, this, &SetsWidget::showSetsContextMenu);

    clear();

    QPixmap pixmap = QPixmap(":/img/placeholders/poster.png")
                         .scaled(QSize(160, 260) * devicePixelRatioF(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    pixmap.setDevicePixelRatio(devicePixelRatioF());
    ui->poster->setPixmap(pixmap);

    QPixmap pixmap2 = QPixmap(":/img/placeholders/fanart.png")
                          .scaled(QSize(160, 72) * devicePixelRatioF(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    pixmap2.setDevicePixelRatio(devicePixelRatioF());
    ui->backdrop->setPixmap(pixmap2);
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

    // Regrouping here is what drops the sets whose last movie has left, and what a set
    // added by the context menu but never filled does not survive -- both as before.
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

    for (const QString& setName : asConst(setNames)) {
        for (Movie* movie : asConst(m_moviesToSave[setName])) {
            movie->controller()->saveData(Manager::instance()->mediaCenterInterface());
        }
        m_moviesToSave[setName].clear();

        // A set without a name has no path of its own to write artwork to: movieSetFileName()
        // collapses to the artwork directory itself, or to the first movie that has no set.
        if (!setName.isEmpty() && !m_setPosters[setName].isNull()) {
            Manager::instance()->mediaCenterInterface()->saveMovieSetPoster(setName, m_setPosters[setName]);
            m_setPosters[setName] = QImage();
        }
        if (!setName.isEmpty() && !m_setBackdrops[setName].isNull()) {
            Manager::instance()->mediaCenterInterface()->saveMovieSetBackdrop(setName, m_setBackdrops[setName]);
            m_setBackdrops[setName] = QImage();
        }
    }

    // The set's own record.  Its overview, collection id and artwork are authoritative
    // in `set.nfo` (D-A), so saving a set means writing that file -- and writing it is
    // also what gives the set an existence apart from its movies, so that an empty one
    // is still there after the next reload.  Nothing happens when no movie set
    // information folder is configured: there is nowhere to put a record, and sets are
    // read-only.
    //
    // The current name, not the pre-rename one: a record under the old name would be a
    // record for a set that no longer exists.
    MovieSet* movieSet = Manager::instance()->movieSetModel()->set(ui->sets->item(ui->sets->currentRow(), 0)->text());
    if (movieSet != nullptr) {
        Manager::instance()->mediaCenterInterface()->saveMovieSet(*movieSet);
    }

    NotificationBox::instance()->showSuccess(
        tr("<b>\"%1\"</b> Saved").arg(ui->sets->item(ui->sets->currentRow(), 0)->text()));
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
    QString setName = tr("New Movie Set");
    int adder = -1;
    bool setExists = false;
    do {
        adder++;
        setExists = false;
        for (int i = 0, n = ui->sets->rowCount(); i < n; ++i) {
            if ((adder == 0 && ui->sets->item(i, 0)->text() == setName)
                || (adder > 0 && ui->sets->item(i, 0)->text() == QString("%1 %2").arg(setName).arg(adder))) {
                setExists = true;
                break;
            }
        }
    } while (setExists);

    if (adder > 0) {
        setName.append(QString(" %1").arg(adder));
    }

    Manager::instance()->movieSetModel()->addSet(setName);
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
    ui->sets->removeRow(ui->sets->currentRow());

    // removeSet() detaches the movies from the set and marks them changed; the sort
    // title is this tab's own doing and has to be cleared here.
    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    const MovieSet* movieSet = setModel->set(origSetName);
    if (movieSet != nullptr) {
        const QVector<Movie*> members = movieSet->movies();
        for (Movie* movie : members) {
            movie->setSortTitle("");
        }
    }
    setModel->removeSet(origSetName);
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

    if (mergesIntoExistingSet) {
        // Its movies are in the set they were merged into now; the emptied one goes.
        setModel->removeSet(origSetName);
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

void SetsWidget::onJumpToMovie(QTableWidgetItem* item)
{
    if (item->column() != 0) {
        return;
    }

    auto* movie = item->data(Qt::UserRole).value<Movie*>();
    emit sigJumpToMovie(movie);
}
