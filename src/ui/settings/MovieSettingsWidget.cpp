#include "ui/settings/MovieSettingsWidget.h"
#include "ui_MovieSettingsWidget.h"

#include "settings/DataFile.h"
#include "settings/Settings.h"

#include <QFileDialog>

MovieSettingsWidget::MovieSettingsWidget(QWidget* parent) : QWidget(parent), ui(new Ui::MovieSettingsWidget)
{
    ui->setupUi(this);

#ifdef Q_OS_MAC
    QFont smallFont = ui->lblFilenameDescription->font();
    smallFont.setPointSize(smallFont.pointSize() - 1);
    ui->lblFilenameDescription->setFont(smallFont);
    ui->lblPlaceholders->setFont(smallFont);
    ui->lblMovieSetRenameHint->setFont(smallFont);
#endif

    ui->comboMovieSetArtwork->addItem(
        tr("Artwork next to movies (add-on style)"), static_cast<int>(MovieSetArtworkType::ArtworkNextToMovies));
    ui->comboMovieSetArtwork->addItem(tr("Separate artwork directory (Kodi movie set information folder)"),
        static_cast<int>(MovieSetArtworkType::SeparateArtworkFolder));
    ui->comboMovieSetArtwork->setToolTip(
        tr("\"Artwork next to movies\" writes set artwork next to the movie folders, prefixed with the "
           "set name (Movie Set Artwork Automator naming). Kodi does not read this layout; add-ons such "
           "as Artwork Beef do, if their movie set artwork directory points at your movies directory."));

    ui->comboMovieSetRename->addItem(
        tr("Automatic (recommended)"), static_cast<int>(MovieSetRenameMode::Automatic));
    ui->comboMovieSetRename->addItem(
        tr("Rename in the movie set file only"), static_cast<int>(MovieSetRenameMode::SetFileOnly));
    ui->comboMovieSetRename->addItem(
        tr("Rename in all movie files"), static_cast<int>(MovieSetRenameMode::AllMovieFiles));

    connect(ui->comboMovieSetRename,
        elchOverload<int>(&QComboBox::currentIndexChanged),
        this,
        &MovieSettingsWidget::onComboMovieSetRenameChanged);

    connect(ui->comboMovieSetArtwork,
        elchOverload<int>(&QComboBox::currentIndexChanged),
        this,
        &MovieSettingsWidget::onComboMovieSetArtworkChanged);

    connect(ui->btnMovieSetArtworkDir,
        &QAbstractButton::clicked, //
        this,
        &MovieSettingsWidget::onChooseMovieSetArtworkDir);

    // The rename hint's warning turns on whether this field is empty, so it has to be
    // recomputed when the field changes -- by the Choose button, which only calls
    // setText(), and by the user typing a path in directly.  Without this the warning
    // said "renaming a set will be refused" for the rest of the session after the user
    // had just fixed exactly that.
    connect(ui->movieSetArtworkDir, &QLineEdit::textChanged, this, [this] {
        onComboMovieSetRenameChanged(ui->comboMovieSetRename->currentIndex());
    });

    ui->movieNfo->setProperty("dataFileType", static_cast<int>(DataFileType::MovieNfo));
    ui->moviePoster->setProperty("dataFileType", static_cast<int>(DataFileType::MoviePoster));
    ui->movieBackdrop->setProperty("dataFileType", static_cast<int>(DataFileType::MovieBackdrop));
    ui->movieCdArt->setProperty("dataFileType", static_cast<int>(DataFileType::MovieCdArt));
    ui->movieClearArt->setProperty("dataFileType", static_cast<int>(DataFileType::MovieClearArt));
    ui->movieLogo->setProperty("dataFileType", static_cast<int>(DataFileType::MovieLogo));
    ui->movieBanner->setProperty("dataFileType", static_cast<int>(DataFileType::MovieBanner));
    ui->movieThumb->setProperty("dataFileType", static_cast<int>(DataFileType::MovieThumb));
    ui->movieSetPosterFileName->setProperty("dataFileType", static_cast<int>(DataFileType::MovieSetPoster));
    ui->movieSetFanartFileName->setProperty("dataFileType", static_cast<int>(DataFileType::MovieSetBackdrop));

    const QStringList placeholders({"baseFileName"});
    ui->movieNfo->setPlaceholders(placeholders);
    ui->moviePoster->setPlaceholders(placeholders);
    ui->movieBackdrop->setPlaceholders(placeholders);
    ui->movieCdArt->setPlaceholders(placeholders);
    ui->movieClearArt->setPlaceholders(placeholders);
    ui->movieLogo->setPlaceholders(placeholders);
    ui->movieBanner->setPlaceholders(placeholders);
    ui->movieThumb->setPlaceholders(placeholders);
    ui->movieSetPosterFileName->setPlaceholders({"setName"});
    ui->movieSetFanartFileName->setPlaceholders({"setName"});
}

MovieSettingsWidget::~MovieSettingsWidget()
{
    delete ui;
}

void MovieSettingsWidget::setSettings(Settings& settings)
{
    m_settings = &settings;
}

void MovieSettingsWidget::loadSettings()
{
    ui->usePlotForOutline->setChecked(m_settings->usePlotForOutline());
    ui->ignoreDuplicateOriginalTitle->setChecked(m_settings->ignoreDuplicateOriginalTitle());

    // Movie set artwork
    for (int i = 0, n = ui->comboMovieSetArtwork->count(); i < n; ++i) {
        if (MovieSetArtworkType(ui->comboMovieSetArtwork->itemData(i).toInt()) == m_settings->movieSetArtworkType()) {
            ui->comboMovieSetArtwork->setCurrentIndex(i);
            break;
        }
    }
    // An invalid DirectoryPath must show as empty, not as a path.  toNativePathString()
    // goes through QDir::absolutePath(), and an invalid path wraps a *default* QDir whose
    // absolutePath() is the process's current working directory -- so "no directory
    // chosen" was displayed as whatever directory MediaElch was started from, and
    // saveSettings() below read that back as a real, valid choice.  One press of Save in
    // this window, for any unrelated reason, then made movieSetRecordsEnabled() answer
    // true forever and pointed the movie set information folder at the working directory:
    // exactly the exposure KodiXml::movieSetFileName() refuses, laundered into a setting
    // where it can no longer refuse it.
    const mediaelch::DirectoryPath movieSetDir = m_settings->movieSetArtworkDirectory();
    ui->movieSetArtworkDir->setText(movieSetDir.isValid() ? movieSetDir.toNativePathString() : QString());
    for (int i = 0, n = ui->comboMovieSetRename->count(); i < n; ++i) {
        if (MovieSetRenameMode(ui->comboMovieSetRename->itemData(i).toInt()) == m_settings->movieSetRenameMode()) {
            ui->comboMovieSetRename->setCurrentIndex(i);
            break;
        }
    }
    onComboMovieSetArtworkChanged(ui->comboMovieSetArtwork->currentIndex());

    const auto loadLineEdit = [this](auto* lineEdit) {
        if (lineEdit->property("dataFileType").isNull()) {
            return;
        }
        bool ok = false;
        DataFileType dataFileType = DataFileType(lineEdit->property("dataFileType").toInt(&ok));
        if (!ok) {
            return;
        }
        const QVector<DataFile> dataFiles = m_settings->dataFiles(dataFileType);
        QStringList filenames;
        for (const DataFile& dataFile : dataFiles) {
            filenames << dataFile.fileName();
        }
        lineEdit->setText(filenames.join(","));
    };
    for (auto* lineEdit : findChildren<QLineEdit*>()) {
        loadLineEdit(lineEdit);
    }
    for (auto* lineEdit : findChildren<PlaceholderLineEdit*>()) {
        loadLineEdit(lineEdit);
    }
}

void MovieSettingsWidget::saveSettings()
{
    QVector<DataFile> dataFiles;
    const auto storeLineEdit = [&dataFiles](auto* lineEdit) {
        if (lineEdit->property("dataFileType").isNull()) {
            return;
        }
        int pos = 0;
        DataFileType dataFileType = DataFileType(lineEdit->property("dataFileType").toInt());
        const QStringList filenames = lineEdit->text().split(",", ElchSplitBehavior::SkipEmptyParts);
        for (const QString& filename : filenames) {
            DataFile df(dataFileType, filename.trimmed(), pos++);
            dataFiles << df;
        }
    };

    for (auto* lineEdit : findChildren<QLineEdit*>()) {
        storeLineEdit(lineEdit);
    }
    for (auto* lineEdit : findChildren<PlaceholderLineEdit*>()) {
        storeLineEdit(lineEdit);
    }

    m_settings->setUsePlotForOutline(ui->usePlotForOutline->isChecked());
    m_settings->setIgnoreDuplicateOriginalTitle(ui->ignoreDuplicateOriginalTitle->isChecked());

    // Movie set artwork
    m_settings->setMovieSetArtworkType(static_cast<MovieSetArtworkType>(
        ui->comboMovieSetArtwork->itemData(ui->comboMovieSetArtwork->currentIndex()).toInt()));
    // Written whichever layout is selected, and that is deliberate: the field is disabled
    // in the artwork-next-to-movies layout but keeps its text, so a user who switches
    // away and back finds their directory still there.  An empty field is an invalid
    // DirectoryPath, which is what "no directory chosen" has to stay -- see loadSettings().
    m_settings->setMovieSetArtworkDirectory(mediaelch::DirectoryPath(ui->movieSetArtworkDir->text()));
    m_settings->setMovieSetRenameMode(static_cast<MovieSetRenameMode>(
        ui->comboMovieSetRename->itemData(ui->comboMovieSetRename->currentIndex()).toInt()));
}

void MovieSettingsWidget::onComboMovieSetArtworkChanged(int comboIndex)
{
    MovieSetArtworkType value = MovieSetArtworkType(ui->comboMovieSetArtwork->itemData(comboIndex).toInt());
    ui->btnMovieSetArtworkDir->setEnabled(value == MovieSetArtworkType::SeparateArtworkFolder);
    ui->movieSetArtworkDir->setEnabled(value == MovieSetArtworkType::SeparateArtworkFolder);

    switch (value) {
    case MovieSetArtworkType::ArtworkNextToMovies: {
        ui->movieSetPosterFileName->setText("<setName>-folder.jpg");
        ui->movieSetFanartFileName->setText("<setName>-fanart.jpg");
        break;
    }
    case MovieSetArtworkType::SeparateArtworkFolder: {
        ui->movieSetPosterFileName->setText("folder.jpg");
        ui->movieSetFanartFileName->setText("fanart.jpg");
        break;
    }
    }

    // The rename hint depends on this combo as well as on its own: a set-file-only
    // rename needs a `set.nfo`, which exists only in the separate-folder layout.
    onComboMovieSetRenameChanged(ui->comboMovieSetRename->currentIndex());
}

void MovieSettingsWidget::onComboMovieSetRenameChanged(int comboIndex)
{
    const auto mode = MovieSetRenameMode(ui->comboMovieSetRename->itemData(comboIndex).toInt());
    // Asked of the two widgets rather than of Settings, because the user may have
    // changed the layout in this dialog and not pressed Save yet -- the hint has to
    // describe what they are about to get, not what is still on disk.
    const bool recordsWouldExist =
        MovieSetArtworkType(ui->comboMovieSetArtwork->itemData(ui->comboMovieSetArtwork->currentIndex()).toInt())
            == MovieSetArtworkType::SeparateArtworkFolder
        && !ui->movieSetArtworkDir->text().isEmpty();

    QString hint;
    switch (mode) {
    case MovieSetRenameMode::Automatic:
        hint = tr("MediaElch picks the rename your Kodi version understands. Kodi 22 and later rename only the "
                  "movie set file; earlier versions rewrite every movie file. Change the Kodi version under "
                  "Kodi settings.");
        break;

    case MovieSetRenameMode::SetFileOnly:
        hint = tr("Only set.nfo is rewritten. Kodi 22 keeps the set's artwork and its place in your library. "
                  "Your movie files keep the old name inside them, so Kodi 21 and earlier -- and other tools "
                  "that read movie NFOs -- will keep showing the old name.");
        if (!recordsWouldExist) {
            // Said before the rename rather than after it.  A refusal the user only
            // meets once they have typed a new name is a worse refusal than one they
            // were warned about while choosing the setting that causes it.
            hint += "\n"
                    + tr("There is no movie set information folder, and set.nfo lives nowhere else, so renaming a "
                         "set will be refused. Choose \"Separate artwork directory\" above and pick a folder.");
        }
        break;

    case MovieSetRenameMode::AllMovieFiles:
        hint = tr("Every movie's NFO is rewritten, so every version of Kodi shows the new name. On Kodi 22 the "
                  "renamed set is treated as a new set: its artwork moves with it, but the old, now-empty set "
                  "stays in Kodi's database until you run Clean Library.");
        break;
    }
    ui->lblMovieSetRenameHint->setText(hint);
}

void MovieSettingsWidget::onChooseMovieSetArtworkDir()
{
    QString dir = QFileDialog::getExistingDirectory(
        this, tr("Choose a directory where your movie set artwork is stored"), QDir::homePath());
    if (!dir.isEmpty()) {
        ui->movieSetArtworkDir->setText(dir);
    }
}
