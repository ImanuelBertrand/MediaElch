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
    /// \brief Saves every movie set, which is not the same as saving each of them in turn.
    /// \details The movies queued in every set and every set's pending artwork are written,
    ///          but a set's `set.nfo` only where there is one already or something to write.
    ///          See the comment in saveAllSets() for why.
    void saveAllSets();
    QSplitter* splitter();

public:
    /// \brief Enables or disables the actions that need a movie set information folder.
    /// \details Two different questions.  *Add Movie Set* needs a record, because a set with
    ///          no members and no `set.nfo` is dropped by the next reload; the artwork needs
    ///          a resolvable path, which the default "artwork next to movies" layout has.
    ///          Everything else in this tab writes movie NFOs and stays enabled either way.
    ///
    ///          Cheap and safe to call often; it must not reload the model.
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
    /// \brief Whether writeSet() writes a set's `set.nfo` even when nothing asks for it.
    enum class RecordWrite
    {
        /// Write it whatever the set's state, as the per-set *Save* has always done.
        Always,
        /// Write it only for a set that has a record already or that was edited.
        OnlyIfChangedOrBacked
    };
    /// \brief Writes one set: the movies queued under \p setName, its artwork and its record.
    /// \details The single place any of those three is written, so that *Save* and *Save All*
    ///          cannot drift apart; \p recordWrite is the one thing they disagree on.  A set
    ///          the model does not know has no record to write, which is not a failure.
    /// \return Whether everything that was asked for was written.  False leaves a whole
    ///         sentence naming the set in \p failureText, ready to be shown.
    ELCH_NODISCARD bool writeSet(const QString& setName, RecordWrite recordWrite, QString& failureText);
    /// \brief What to call the set \p setName in a message to the user.
    ELCH_NODISCARD QString displayNameOfSet(const QString& setName) const;

    /// \brief The three renames, kept apart because they are three different operations.
    /// \details onSetNameChanged() decides which one a typed name means; each of these
    ///          performs exactly one of them.
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
    void carryQueuedMoviesOver(const QString& oldName, const QString& newName);
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
    /// \details Only the direction matters; see onSettingsSaved().
    bool m_recordsAreConfigured = false;
    DownloadManager* m_downloadManager;
    QMovie* m_loadingMovie;
    /// \brief Whether the list is filtered down to the sets that have no movies.
    /// \details A set with a `set.nfo` survives having no members, so the list can hold sets
    ///          nothing in the library points at, and nothing about a row says so.
    bool m_showOnlyEmptySets = false;

    void loadSet(QString set);
};
