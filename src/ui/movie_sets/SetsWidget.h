#pragma once

#include <QImage>
#include <QMap>
#include <QMovie>
#include <QSplitter>
#include <QStringList>
#include <QTableWidgetItem>
#include <QWidget>

#include "network/DownloadManagerElement.h"

class DownloadManager;
class Movie;

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

signals:
    void setActionSaveEnabled(bool, MainWidgets);
    void sigJumpToMovie(Movie* movie);

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

private:
    Ui::SetsWidget* ui;
    QMap<QString, QVector<Movie*>> m_moviesToSave;
    QMap<QString, QImage> m_setPosters;
    QMap<QString, QImage> m_setBackdrops;
    QImage m_currentPoster;
    QImage m_currentBackdrop;
    QMenu* m_tableContextMenu;
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
