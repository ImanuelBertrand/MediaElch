#pragma once

#include <QImage>
#include <QMap>
#include <QMovie>
#include <QSplitter>
#include <QStringList>
#include <QTableWidgetItem>
#include <QWidget>

#include "network/DownloadManagerElement.h"
#include "utils/Meta.h"

class MovieSet;

class DownloadManager;
class Movie;
class QAction;

namespace Ui {
class SetsWidget;
}

/**
 * \brief The SetsWidget class
 */
class SetsWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SetsWidget(QWidget* parent = nullptr);
    ~SetsWidget() override;

public slots:
    void loadSets();
    void saveSet();
    QSplitter* splitter();

public:
    /// \brief Enables or disables the actions that need a movie set information folder.
    /// \details Two questions, and they are deliberately different ones.  *Add Movie
    ///          Set* needs a **record**: a set with no members and no `set.nfo` is
    ///          dropped by the next reload (MovieSetModel::dropEmptySets()), so offering
    ///          to create one with no folder configured is offering something that
    ///          silently disappears -- and it is the only path that can create such a
    ///          set, which is why disabling it is what keeps read-only mode from
    ///          accumulating them.  The artwork needs a resolvable **path**, which the
    ///          default "artwork next to movies" layout has and a separate folder that
    ///          was never chosen has not.
    ///
    ///          Everything else in this tab writes movie NFOs -- renaming a set, moving
    ///          movies in and out of it, the sort title, deleting it -- and stays
    ///          enabled with no folder, because membership and the set's name are
    ///          authoritative in the member movies (D-A/D1a).  Disabling the rename in
    ///          particular would leave retyping the name on each movie as the only way
    ///          to rename a set, which is exactly how a set forks in two (D3).
    ///
    ///          Cheap, and safe to call often: it reads two settings and touches a
    ///          handful of widgets.  It must *not* reload the model.
    void applyWriteAccess();

signals:
    void setActionSaveEnabled(bool, MainWidgets);
    void sigJumpToMovie(Movie* movie);
    /// \brief The user followed the link in the notice; open the settings window.
    void sigOpenSettings();

private slots:
    void onSetSelected();
    void clear();
    void onSortTitleChanged(QTableWidgetItem* item);
    void onAddMovieSet();
    void onRemoveMovieSet();
    void onAddMovie();
    void onRemoveMovie();
    void chooseSetPoster();
    void chooseSetBackdrop();
    void onPreviewPoster();
    void onPreviewBackdrop();
    void showSetsContextMenu(QPoint point);
    void onSetNameChanged(QTableWidgetItem* item);
    void onDownloadFinished(DownloadManagerElement elem);
    void onJumpToMovie(QTableWidgetItem* item);
    void onShowOnlyEmptySets(bool onlyEmpty);
    void onSettingsSaved();

private:
    /// \brief The three renames, kept apart because they are three different operations.
    /// \details onSetNameChanged() decides which one a typed name means -- merge, or the
    ///          rename mode of D-B -- and each of these performs exactly one of them.
    void performSetFileOnlyRename(
        QTableWidgetItem* item, MovieSet* origSet, const QString& origSetName, const QString& newName);
    void performAllMovieFilesRename(
        QTableWidgetItem* item, MovieSet* origSet, const QString& origSetName, const QString& newName);
    void performMerge(QTableWidgetItem* item,
        MovieSet* origSet,
        MovieSet* targetSet,
        const QString& origSetName,
        const QString& newName);
    void revertSetName(QTableWidgetItem* item, const QString& name);
    ELCH_NODISCARD bool setNameIsTaken(const QString& name, const MovieSet* except) const;
    ELCH_NODISCARD bool refuseIfNameIsTaken(QTableWidgetItem* item, MovieSet* origSet, const QString& newName);
    void applyDivergenceTooltip(QTableWidgetItem* item, const MovieSet* movieSet);
    void carrySetArtworkOver(const QString& oldName, const QString& newName);

    Ui::SetsWidget* ui;
    QMap<QString, QVector<Movie*>> m_moviesToSave;
    QMap<QString, QImage> m_setPosters;
    QMap<QString, QImage> m_setBackdrops;
    QImage m_currentPoster;
    QImage m_currentBackdrop;
    QMenu* m_tableContextMenu;
    /// \brief Kept so that it can be disabled; see applyWriteAccess().
    QAction* m_actionAddSet = nullptr;
    /// \brief What applyWriteAccess() last found, to notice the setting being changed.
    /// \details Only the direction matters, and only one of the two directions does
    ///          anything beyond re-enabling controls; see onSettingsSaved().
    bool m_recordsAreConfigured = false;
    DownloadManager* m_downloadManager;
    QMovie* m_loadingMovie;
    /// \brief Whether the list is filtered down to the sets that have no movies.
    /// \details A set with a `set.nfo` survives having no members (D-A), so the list can
    ///          hold sets that nothing in the library points at.  They are not
    ///          distinguishable from the others by looking, and in a long list they are
    ///          not findable at all, so the tab needs a way to ask for them.
    bool m_showOnlyEmptySets = false;

    void loadSet(QString set);
};
