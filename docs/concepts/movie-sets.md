# Movie Sets

__Status__: Concept, work in progress  
__Last Updated on__: 2026-08-31

Two invitations stand open in the issue tracker.  bugwelle in #1747,
2024-05-18:

> the "movie set" feature needs an overhaul. It doesn't work properly and
> there is a lot to add to.

And again in #1809, 2024-11-04:

> The movie-set feature needs to be reworked. It was supported only poorly
> when I started and I never got to improve it either.

This document takes up that offer.  It describes what the movie-set code does
today and proposes two changes that the open feature requests all turn out to
depend on: a set becomes an object that exists in memory, and it gets a file
of its own on disk.  Almost every movie-set bug in the tracker is a symptom of
it having neither — #800 and #1848 (the set overview), #1746 and #642 (set
posters won't scrape), #1303, #1001, #421 and #822 (missing set art types) are
not seven independent gaps but one gap seen from seven angles.

**Everything below about Kodi is read from Kodi's source, not observed in a
running Kodi.**  Nothing here has been built, run or tested against a real
library.  Where the text predicts a runtime behaviour — most importantly that
an empty set overview blanks a populated one on Kodi 21 and 22 — that is
inference from the code, and it predicts destruction of a user's data, so it
deserves a manual check before anything is built on it.  Citations are against
this tree, `xbmc` at `22.0b1-Piers-976-ge7b101cf05` (with the `19.5-Matrix`,
`20.5-Nexus` and `21.3-Omega` tags named explicitly where used), and
Ember-MM-Newscraper 1.11.0.

Three small fixes are **open but unmerged** — upstream has neither accepted
nor reviewed them — and this design assumes them: #2011 (write set artwork
where Kodi can read it), #2012 (keep the TMDB collection id in the movie NFO)
and #2013 (renaming a set no longer loses its data).  They are noted below
where they change something described here; everything else describes
`master`.


## Current State

### A Set Has No Existence of Its Own

`MovieSet` is a struct of three fields, `tmdbId`, `name` and `overview`
(`src/data/movie/MovieSet.h:8-17`), stored by value on each `Movie`
(`src/data/movie/Movie.h:272`).  Its translation unit contains a comment and
nothing else (`src/data/movie/MovieSet.cpp:3`).

There is no list of sets anywhere.  It is recomputed, from scratch, in three
unrelated places, each walking the whole movie model and grouping by
`movie->set().name`: the sets tab (`src/ui/movie_sets/SetsWidget.cpp:120-134`),
the set combo box in the movie widget
(`src/ui/movies/MovieWidget.cpp:598-606`), and the set dropdown in the filter
widget (`src/ui/small_widgets/FilterWidget.cpp:318-319`).
`MainWindow::onMenu` runs the first of those on every switch to the Movie Sets
tab (`src/ui/main/MainWindow.cpp:881`), after first discarding the four
`QMap`s that hold everything the tab knows (`SetsWidget.cpp:106-109`).

What that discard costs is worth being precise about, because it is smaller
than it looks and still bad.  Edits to a movie are not lost: `Movie::setSet`
marks the movie dirty (`src/data/movie/Movie.cpp:779-783`) and it stays dirty
in `MovieModel`.  What is lost is the tab's own state — the `m_moviesToSave`
list of what still needs writing, and any set poster or backdrop the user
picked but has not saved (`SetsWidget.cpp:107-109`).  Leave the tab and come
back, and the images are gone with no indication that anything happened.

### The Identity of a Set Is a Table Cell

Because there is no set object, the set the UI is operating on is identified by
the string in the row's `Qt::UserRole` (written at `SetsWidget.cpp:149` and
`:521`).  Four call sites read it back as the set's identity:
`chooseSetPoster()` (`:365`), `chooseSetBackdrop()` (`:400`),
`onRemoveMovieSet()` (`:534`) and `onSetNameChanged()` (`:550`).  A fifth,
`saveSet()`, hedges: it reads the role *and* the displayed text and iterates
both (`:435-440`), which is only necessary because the two are known to
disagree.  Renaming a row never updates the role, so on `master` they diverge
from the first rename onwards — `onRemoveMovieSet()` even detaches the movies
under one name (`:537`) while erasing the map entries under the other
(`:541-544`).  #2013 fixes this within the existing design; it does not remove
the design.

The same absence forces `onSetNameChanged()` to rebuild a `MovieSet` from the
name alone (`:568-570`), and `MovieWidget::onSetChange` to do the same
(`src/ui/movies/MovieWidget.cpp:1204-1206`).  Both silently drop the set's
`overview` and `tmdbId`, because there is nowhere else those values are kept.

### Artwork: Two Types, Written by Name

Exactly two set art types exist in the whole application:
`ImageType::MovieSetPoster` and `ImageType::MovieSetBackdrop`
(`src/globals/Globals.h:123-124`), mirrored by two `DataFileType` entries
(`:239-240`), two default filename templates
(`src/settings/Settings.cpp:96-97`), two `QMap` members on the widget
(`src/ui/movie_sets/SetsWidget.h:61-62`), four virtual methods on the media
center interface (`src/media_center/MediaCenterInterface.h:32-35`) with their
`KodiXml` overrides (`src/media_center/KodiXml.h:44-47`,
`src/media_center/KodiXml.cpp:1183-1245`), and a hard-wired pair of settings
fields (`src/ui/settings/MovieSettingsWidget.cpp:43-44`, `:55-56`, `:148-154`,
plus its `.ui`).  Adding a third art type means touching all of it.

The interface is keyed by set *name*, not by a set: `saveMovieSetPoster(QString
setName, QImage)`.  `KodiXml::movieSetFileName` then has to find the set again
— in "artwork next to movies" mode it linear-scans the entire movie model
looking for any member (`src/media_center/KodiXml.cpp:1316-1327`) — and
`DataFile::saveFileName` substitutes the name into the template
(`src/settings/DataFile.cpp:52-57`).  Set art is also the only artwork
MediaElch re-encodes: `poster.save(fileName, "jpg", 100)` at
`KodiXml.cpp:1226` and `:1242`, regardless of the template's extension, where
movie, show and concert art is written as the bytes that were downloaded.
Whether those paths are ones Kodi reads at all is the subject of #2011, #1747,
#1158 and #1809, and is not revisited here.

The movie-set information folder is optional today and off by default: the
artwork mode defaults to `ArtworkNextToMovies`
(`src/globals/Globals.h:76-83`, `src/settings/Settings.h:233`,
`src/settings/Settings.cpp:304`), and the separate-folder path is only taken
when the user has switched to it.

Kodi is much more generous than two art types.  In the movie-set information
folder, `poster` and `fanart` are the guaranteed defaults
(`xbmc/video/VideoThumbLoader.cpp:88`) and the whitelist that applies at the
stricter artwork levels offers `clearart`, `discart`, `keyart`, `banner`,
`landscape` and `clearlogo` (`system/settings/settings.xml:1374-1379`); at the
default level any ASCII-alphanumeric name of at most 25 characters is accepted
(`VideoThumbLoader.cpp:143-150`, reached with `addAll` derived from the
artwork level at `xbmc/video/VideoInfoScanner.cpp:2489`, whose setting
defaults to `ALL` — `system/settings/settings.xml:1356-1358`).  So six of the
eight types Kodi will display for a set have no representation in MediaElch at
all.

### There Is No Set Scraper

`SetsWidget::chooseSetPoster()` allocates a throwaway `Movie`, assigns the set
name as that movie's *title*, and opens `ImageDialog` asking for
`ImageType::MoviePoster` (`SetsWidget.cpp:366-373`).  `ImageDialog` seeds its
search box from the movie's title (`src/ui/image/ImageDialog.cpp:118`) and
calls `m_currentProvider->searchMovie(...)` (`:808`).  In other words, asking
for a poster for _Alien Collection_ searches the movie database for a film
called "Alien Collection".  That is the whole of #1746 and #642.

It cannot be fixed inside `SetsWidget`: `ImageProvider` has no collection
entry point to call (`src/scrapers/image/ImageProvider.h:71-117`).

Metadata scraping is closer than it looks.  `TmdbMovieScrapeJob` already reads
`belongs_to_collection` from the movie details
(`src/scrapers/movie/tmdb/TmdbMovieScrapeJob.cpp:179-180`) and follows it to
`/3/collection/{id}` (`:116-148`, `src/scrapers/tmdb/TmdbApi.cpp:294-303`) —
but reads only `id`, `name` and `overview` out of the response (`:143-147`)
and discards `poster_path` and `backdrop_path`, which are in the same JSON
object.

### The Overview Has No UI

A set overview is read from an NFO (`MovieXmlReader.cpp:180-182`), carried on
the `MovieSet` value, written back (`MovieXmlWriter.cpp:109-115`), and filled
in from TMDB during a movie scrape (`TmdbMovieScrapeJob.cpp:146`).  What is
missing is any widget that displays or edits it — there is none — and the
two places that rebuild a `MovieSet` from a name alone throw it away on the next
rename or reassignment.  So it survives a round trip that touches nothing and
is lost the moment the user edits the set, which is the shape of what #800 and
#1848 describe.  The TMDB collection id has the same problem one step further
along: scraped, held in memory, never written at all (#2012).


## Proposal

### D-A: Membership Comes From the Movies; Attributes Live in `set.nfo`

The current code and the earlier draft of this document both treat "the set"
as one thing.  Kodi does not, and the distinction decides where every write
goes.

**Membership is only ever the movies.**  On every Kodi version, a set exists
because a movie's NFO names it.  `CVideoInfoScanner::UpdateSetInTag` returns
immediately unless the movie's tag already has a set title
(`xbmc/video/VideoInfoScanner.cpp:834-835`); it is reached only from the movie
scan path (`:1472`, `:1559`); and the movie-set information folder is looked
up by the title that came from the movie (`GetMovieSetInfoFolder`,
`:2442-2458`).  Kodi never enumerates the MSIF to discover sets.  Membership
in the database is a single nullable `idSet integer` column on the movie row
(`xbmc/video/VideoDatabaseDDL.cpp:85`) — one movie, at most one set, no
ordinal.  This half is non-negotiable and unchanged by everything below.

**Attributes are a different story, and on Kodi 22 the movie NFO already
loses.**  When an MSIF folder exists and contains a `set.nfo`,
`UpdateSetInTag` *demotes* the name that came from the movie and lets the file
override the rest (`VideoInfoScanner.cpp:847-855`):

```cpp
tag.m_set.SetOriginalTitle(tag.m_set.GetTitle());   // movie NFO's <set><name>
if (!setTag.GetTitle().empty())    tag.m_set.SetTitle(setTag.GetTitle());
if (!setTag.GetOverview().empty()) tag.m_set.SetOverview(setTag.GetOverview());
```

`strOriginalSet` — the column Kodi 22 matches on — keeps the movie's name;
`strSet`, the displayed one, becomes `set.nfo`'s.  Artwork has the same
ordering: files found in the MSIF folder first (`:866`), `set.nfo`'s `<art>`
block only `if (movieSetArt.empty() && ...)` (`:869`), and the scraper's
`set.*` URLs only in the `else` branch where there is no MSIF at all (`:876`).

So on Kodi 22 an overview edit written only into member NFOs is silently
overridden on the next scan wherever a `set.nfo` exists.  Treating that file
as an optional extra stops being an option the moment it exists.

**The decision is therefore to make `set.nfo` MediaElch's own authoritative
record too.**  The set's overview, its artwork and its TMDB collection id live
in `<MSIF>/<legal set name>/`, and the member NFOs receive a mirrored copy so
that Kodi 19-21 — which cannot see `set.nfo` at all — and any other tool
reading movie NFOs still get one.  The fan-out to N member NFOs is still real,
but it is now a *projection out of* the authoritative record rather than the
thing that constitutes the set.

That mirror is written **regardless of the target Kodi version**.  On Kodi 22
it is redundant, because `set.nfo` wins for title and overview, but a user who
changes Kodi version or points another tool at the library should not silently
lose their set data, and the redundancy costs a few lines of XML.

What each member NFO carries is therefore no longer simply "the set's name":

- `<set><name>` is the **join key**.  It equals the displayed name until a
  set-file-only rename, after which it stays put while `set.nfo`'s `<title>`
  moves (see D-B).
- `<set><overview>` is a mirror of the authoritative overview, subject to the
  two rules below: identical text in every member, and never written empty.
- `<set><uniqueid type="tmdb">` is a mirror of the authoritative id (#2012).

That gives a clean division:

| | Authoritative copy | Mirror | Read by |
|---|---|---|---|
| Membership | member `<set><name>` | — | 19-22, MediaElch |
| Overview | `set.nfo` | every member | 22: file; 19-21: mirror |
| Artwork | MSIF folder files | — | Kodi 19-22 |
| TMDB id | `set.nfo` | every member | MediaElch only (#2012) |

The join between the two halves is the name, and it has to be exact: the
member NFOs' `<set><name>` and `set.nfo`'s `<originaltitle>` must be the same
string, because that string is what lands in `strOriginalSet`.  Point them at
different values and Kodi keys the set's row off something no movie will ever
mention again.

`set.nfo` first shipped in Kodi 22 — `git tag --contains fbee563fc4` lists
only the four `22.0*-Piers` tags — so writing it can never make 19-21 worse;
there it is simply an unread file next to the artwork.
`CSetInfoTag::ParseNative` (`xbmc/video/SetInfoTag.cpp:35-56`) reads exactly
`<title>`, `<originaltitle>`, `<overview>` and `<art>` and ignores every other
child, so the TMDB id can ride along in a child of our own choosing and Kodi
will skip it — the same free ride #2012 relies on inside the movie NFO.

**A set with no member movies is a valid MediaElch set** under this model: it
has a `set.nfo`, so it has a record.  It is simply invisible to Kodi until a
movie joins it, because Kodi discovers sets only from movies, and the UI
should say so rather than pretending otherwise.  The artwork folder likewise
stops being a pile of images with nothing to explain what they are for.

#### The Cost: the Folder Becomes Mandatory

This design does not work without a configured movie-set information folder,
so **set functionality is guarded behind a warning that tells the user to
configure one**, and MediaElch offers no sets at all until they do.  That is a
deliberate reversal of today's situation, where the folder is optional and off
by default, and it is exactly the objection bugwelle raised in #1243 on
2021-04-06 against changing the artwork default:

> The default is "Artwork next to movies" simply because otherwise the user
> would have to configure a directory first. And MediaElch does not yet have a
> "Quick Start" dialog or similar.

The objection is right, and the answer here is the explicit warning rather
than a silent fallback — because the silent fallback is worse than refusing.
With no folder configured, `Settings::movieSetArtworkDirectory()` returns a
`DirectoryPath` built from an empty string, which is `isValid() == false`
wrapping `QDir("")` (`src/media/Path.h:26`,
`src/settings/Settings.cpp:305-306`), and `KodiXml::movieSetFileName` never
checks `isValid()` before calling
`dir.absolutePath()` (`src/media_center/KodiXml.cpp:1311-1315`).
`QDir("").absolutePath()` resolves to the *process's current working
directory* (verified against Qt 6.8), so today a user who selects the
separate-folder mode without setting a folder scatters set artwork next to
wherever MediaElch happened to be launched from.  Refusing to operate is
strictly better than that.

#### Migration Is Not Optional

Today every set comes from movie NFOs.  An existing user who upgrades and
configures a folder must find their sets already there, not gone.  So the
first use of a configured folder runs a one-shot pass that materialises every
set MediaElch can see in the movie NFOs into a `set.nfo`, carrying whatever
overview and id those NFOs already hold.  Without that pass this design
silently empties the sets tab for every existing user, which is a worse first
impression than the bug it fixes.

#### Two Rules About the Overview That Still Bind

Both are about Kodi 19-21, which only ever sees the mirror, and both survive
the decision above unchanged.

The same overview must go into *every* member.  Kodi 19 and 20 keep the
first-scanned member's copy and ignore the rest — `AddSet` returns the
existing row's id and runs no `UPDATE` at all
(`19.5-Matrix:xbmc/video/VideoDatabase.cpp:1664-1668`,
`20.5-Nexus:…:1696-1701`) — while Kodi 21 and 22 let the last-scanned member
win.  Identical text everywhere is the only way to get a deterministic result
on all four.

And an *empty* overview must never be written.  MediaElch currently emits
`<overview></overview>` unconditionally whenever a set name exists
(`MovieXmlWriter.cpp:109-115`), and `XMLUtils::GetString` returns `true` for
an existing-but-empty element (`xbmc/utils/XMLUtils.cpp:261-262`).  On Kodi 21
that reaches `m_updateSetOverview = true`
(`21.3-Omega:xbmc/video/VideoInfoTag.cpp:1278-1281`) and then
`UPDATE sets SET strOverview = '' WHERE idSet = …`
(`21.3-Omega:xbmc/video/VideoDatabase.cpp:1776-1781`, caller at `:2658`).  On
Kodi 22 the same flag is set inside `CSetInfoTag::SetOverview`
(`xbmc/video/SetInfoTag.cpp:58-63`, via
`xbmc/video/VideoInfoTag.cpp:1895-1897`), is passed to `AddSet`
(`xbmc/video/VideoDatabase.cpp:2427-2428`), and runs
`UPDATE sets SET strSet = '…', strOverview = '' WHERE idSet = …`
(`:1554-1556`).  Either way, one member with an empty set overview, scanned
last, blanks the whole set's overview.  Writing the same text everywhere is
not sufficient; the write also has to be skipped when the text is empty.
This is the one prediction in this document worth checking by hand before
relying on it.

### D-B: The Identity Key Is the Name, Not the TMDB Id

Kodi matches a set by name.  On Kodi 22 the match is byte-exact: `SELECT idSet
FROM sets WHERE strOriginalSet='...'` on a plain `text` column with no
collation (`VideoDatabase.cpp:1535-1536`, `VideoDatabaseDDL.cpp:162-163`).  On
19-21 it is `SELECT idSet FROM sets WHERE strSet LIKE '...'`
(`19.5-Matrix:xbmc/video/VideoDatabase.cpp:1654`), which is looser in one
direction — case-insensitive for ASCII — and treats `%` and `_` as
wildcards.  Byte-identical names in every member satisfy all four releases;
nothing else does.  Two sets with the same name *are* one set to Kodi, and a
set has no id in any NFO format Kodi reads: `CSetInfoTag` has no id field that
comes from XML at all (`xbmc/video/SetInfoTag.h:68-74`, parser at
`SetInfoTag.cpp:35-56`).

So the name is the primary key.  The TMDB collection id is a scraping and
re-identification attribute carried alongside it — it tells us which
collection to re-fetch, and it survives a rename — but it never identifies the
set.  #2012 gives it somewhere to be persisted; it does not promote it.

#### Renaming Is a Setting, Not an Inference

The consequence for renaming is worse than "the UI must keep up".  Kodi 22
matches on `GetOriginalTitle()` falling back to the title
(`VideoDatabase.cpp:2427-2428`), and `strOriginalSet` is written **only on
INSERT** (`:1541-1544`) — the UPDATE branch touches `strSet` and `strOverview`
and nothing else (`:1555`, `:1558`).  So there are two genuinely different
renames, and which one is correct depends entirely on the Kodi version the
library is for:

**Set file only.**  Write `set.nfo`'s `<title>`; leave every member's
`<set><name>` and `set.nfo`'s `<originaltitle>` alone, since that string is
the join key.  On Kodi 22 the row matches, `UPDATE sets SET strSet = …` runs
in place, and the set keeps its identity and its artwork.  On Kodi 19-21 the
rename is simply **invisible** — those versions never read `set.nfo` and take
the display name from the member NFOs.

**All movie files.**  Rewrite every member's `<set><name>`, plus `set.nfo`'s
`<title>` and `<originaltitle>`.  Correct on Kodi 19-21.  On Kodi 22 the match
key has changed, so `AddSet` finds nothing, INSERTs a new row and leaves the
old one orphaned with its artwork rows until the user runs Clean Library.
Bounded rather than catastrophic — the new row picks up art from the new
folder — but real, and users should be told.

This is exposed as a **three-state setting defaulting to "Automatic"**, which
follows `KodiSettings::kodiVersion()`: set-file-only at v22, all-movie-files
at v17-v21, with the two explicit modes available as overrides.  The correct
choice is fully determined by the target version, so most users should never
have to understand any of the above; the overrides exist for mixed setups and
for users who want their movie NFOs to stay human-readable.  Coupling
set behaviour to the Kodi version setting is what psonnosp proposed in #1243
on 2021-04-08 and bugwelle agreed to ("Errr... yes of course. :)"), which is
the closest thing to prior maintainer assent this design has.

The wart, stated honestly: under set-file-only a member NFO's `<set><name>` no
longer matches the set's displayed name.  That is correct — it is the key, not
the label — but it looks wrong to anyone opening the file, and any other tool
reading movie NFOs will show the old name.  That cost is the reason this is a
setting rather than unconditional behaviour.

**`KodiVersion` needs a two-line fix first, and it is part of this work.**
`KodiVersion::latest()` returns the default-constructed value
(`src/media_center/KodiVersion.cpp:9-12`) and the constructor's default
argument is `v20` (`src/media_center/KodiVersion.h:24`), while `isValid()`
accepts up to 22 (`KodiVersion.cpp:14-17`) and `all()` lists v22 (`:19-22`) —
a missed bump.  The member initialiser at `KodiVersion.h:37` says `v19`,
disagreeing with the constructor's default even before v22 enters it.
"Automatic" reads `KodiSettings::kodiVersion()`, so left alone a fresh install
would sit at v20 and never select the set-file-only path: the feature's
default behaviour would be wrong out of the box.  That makes it a dependency
of this design rather than an unrelated tidy-up, and it is fixed here.

Renaming a set onto an existing name is a merge, on both sides.  The current
code already performs it (`SetsWidget.cpp:555-560`) without telling the user.

### D-C: Promote `MovieSet` to an Entity, and Give the List a Model

The value stored on `Movie` and the set itself are two different things, and
should be two types.  What a movie's NFO says about its set is a value; the
set is an aggregate over the movies that say it, plus the record in
`set.nfo`.

Keep the current three-field struct as the per-movie value — it is what
`Movie::set()` returns, what the NFO reader and writer exchange, and what the
scrapers fill in — and rename it to something that says what it is, e.g.
`MovieSetInfo`.  Then `MovieSet` becomes the entity:

```cpp
class MovieSet : public QObject
{
    Q_OBJECT
public:
    explicit MovieSet(QString name, QObject* parent = nullptr);

    ELCH_NODISCARD QString name() const;
    ELCH_NODISCARD TmdbId tmdbId() const;
    ELCH_NODISCARD QString overview() const;
    ELCH_NODISCARD const QVector<Movie*>& movies() const;   // not owned
    ELCH_NODISCARD MovieSetImages& images();

    void setName(QString name);          // rewrites every member
    void setTmdbId(TmdbId id);
    void setOverview(QString overview);
    void addMovie(Movie* movie);
    void removeMovie(Movie* movie);
    void clearMovies();

signals:
    void sigChanged(MovieSet* set);
};
```

**Membership heals itself.**  A set holds `Movie*` it does not own, and nothing in
MediaElch tells it when one of them dies: `Movie::sigChanged` is not emitted from
`~Movie`, and `MovieModel` neither resets nor names what it removed — `clear()`
(`src/model/MovieModel.cpp:278-289`) only calls `deleteLater()` on every movie.
`QObject::destroyed` is the one notification that does arrive, including for that
`deleteLater()`, so `addMovie()` connects to it and the member drops out of `m_movies`
on its own.  That is five lines, it needs no cooperation from `MovieModel`, and it is
what makes it safe for a set to outlive a library reload at all — see Open question 1,
which this answers for the object; the model's own bookkeeping is answered below.
The connection is deliberately never taken down: `removeMovie()` leaves it in place,
because re-adding the same movie would only have to make it again, and the handler is a
no-op for a movie that is not a member.

`MovieSetImages` follows `MovieImages` (`src/data/movie/MovieImages.h:67-71`):
a `QMap<ImageType, QByteArray>` of downloaded bytes plus the has-changed flags,
so set art stops being re-encoded to JPEG and starts behaving like every other
image MediaElch writes.  The mutators are the fan-out point from D-A: they
update the record and mark the members changed, and that is the only place
that knows an edit means a `set.nfo` write plus N member NFO writes.

The constructor takes only a name, which is the honest signature: the entity
is loaded from `set.nfo` where one exists and reconstructed from the members
where one does not.  When the two disagree — a hand-edited member NFO, a
partially completed scrape, or #2012 writing an id into some members and not
others — **`set.nfo` wins and the members are overwritten with the mirror**.
That is the whole benefit of having an authoritative record, and it removes
the "which member's copy is right" question that has no good answer.

Above the entity, a `MovieSetModel` in `src/model/` alongside `MovieModel` and
`TvShowModel`, owning its `MovieSet*`s, exposing them the way `MovieModel`
exposes movies (a `MovieSetPointerRole`, `sets()`, `set(name)`, `addSet`,
`removeSet`), and owned by `Manager` next to the other models
(`src/globals/Manager.h:62-65`, `:91-94`).  All three recompute sites then
read one model instead of grouping the library three times.

The model does that grouping once, when `Manager` hands it the movie model, and then
keeps the result rather than recomputing it — the three sites read `sets()` and nothing
else.  Keeping it current takes two things, because `MovieModel` offers no reset signal
to rebuild on:

- **A movie joining the library** arrives as `rowsInserted`, which is emitted by both
  `addMovie()` and `addMovies()`.
- **A movie changing its set** is found by comparing the movie's current
  `set().name` against the one it was last seen with, on every `Movie::sigChanged`.
  That signal fires for every kind of edit, so most of the comparisons find nothing;
  it costs one hash lookup and one string compare.  This is the interim arrangement
  for as long as `Movie::setSet` is in the public API — it is option 1 of Open
  question 2 — and it goes away with the setter when the model becomes the only
  writer.

**A set exists while it has members.**  One that loses its last movie is dropped,
because until `set.nfo` is written a set has no record apart from the movies that name
it, which is exactly what the three grouping sites assumed.  It is also what keeps the
model from collecting a set per keystroke: the movie widget's set combo box is
`editable` and `editTextChanged` rewrites the movie's set name on every one of them
(`src/ui/movies/MovieWidget.cpp:194`).  A set created empty by `addSet()` — what the
sets tab's *Add Movie Set* does — is the one exception, and it survives only until the
next `reload()`.  That regroup is also where a set whose object is kept but whose
members are gone is collected; it preserves the objects, so a set's own record survives
it.

**A membership change dirties nothing on its own** — not the set, since membership is
not in `set.nfo` (D-A), and not the movie.  So wherever the model makes a membership
edit that has to reach disk, the model marks the member movies changed itself.
`removeSet()` is the case that exists today: it detaches its movies, and the detach has
to be saved.  Miss that and the edit is lost with no dirty flag anywhere, while
`MovieSet::sigChanged` has already claimed the set changed.

**The recommended shape is that the model is the only writer**: `MovieSet` and
`MovieSetModel` own set membership, and `Movie::setSet` leaves the public API.
This is by a wide margin the largest diff of the options — it touches the NFO
reader, five scrapers and both widgets — and it should be planned as its own
step rather than smuggled in.  It is recommended anyway, for two reasons that
are independent of each other:

First, **lifetime**.  A `MovieSet` holding `QVector<Movie*>` has to survive a
library reload, and nothing today makes that safe.  `MovieModel` has no
`beginResetModel`/`endResetModel` at all — only `beginInsertRows` (`:29`,
`:37`) and `beginRemoveRows` (`:283`) — so there is no reset to rebuild on,
and `MovieModel::clear()` (`:278-289`) calls `movie->deleteLater()` on every
element while `Movie::sigChanged` never fires on destruction.  The existing
code already knows this hazard and works around it by having no long-lived
state at all: the comment at `SetsWidget.cpp:104-105` says the maps must be
cleared before the table is, "otherwise MediaElch may access invalidated
`Movie*`".  Any design that keeps set objects across a reload takes that
hazard on, and the version where the model is the only writer is the one where
there is a single place to solve it.

Second, **duplicated state**.  The value/entity split alone does not remove
it.  `Movie::set()` returns by value (`src/data/movie/Movie.h:95`,
`src/data/movie/Movie.cpp:469`) and the value stays a plain member
(`Movie.h:272`), so every member movie keeps a mutable copy of the set's name,
overview and id alongside the entity's.  Two writers, no reconciliation.  Only
removing the movie-side setter closes that.

The alternatives are recorded under Open questions.

Two things this buys immediately, beyond the tab: `movieSetFileName` stops
scanning the library because a set knows its members, and the media-center
interface can take a set instead of a name.

### D-D: Extend Set Artwork to Kodi's Full Complement

Add the six art types Kodi accepts for a set that MediaElch has no
representation for, as `ImageType` and `DataFileType` entries, using the names
MediaElch already uses for the same Kodi art type on movies
(`src/settings/Settings.cpp:91-95`):

| New type            | Kodi art type / stem |
|---------------------|----------------------|
| `MovieSetBanner`    | `banner`             |
| `MovieSetClearArt`  | `clearart`           |
| `MovieSetLogo`      | `clearlogo`          |
| `MovieSetCdArt`     | `discart`            |
| `MovieSetThumb`     | `landscape`          |
| `MovieSetKeyArt`    | `keyart`             |

Together with the existing poster and backdrop, that is Kodi's whitelist plus
its two defaults, and it is byte-identical across 19.5, 20.5, 21.3 and 22, so
nothing here is Kodi-22-only.  `keyart` is the only one with no movie-level
counterpart in MediaElch; it is a textless poster.

**This is coupled to the filename defaults and does not work without them.**
MediaElch's templates substitute `<setName>`
(`src/settings/Settings.cpp:96-97`, `src/settings/DataFile.cpp:52-57`), so a
new banner type would be written as `Alien Collection-banner.jpg`.
`GetMovieSetInfoFolder` returns a path with a trailing slash
(`VideoInfoScanner.cpp:2450-2452`), which leaves `baseFilename` empty in
`AddLocalItemArtwork` and makes the *whole* stem the candidate art type
(`:172-196`) — so `alien collection-banner` fails `IsValidArtType` (it has
spaces and a hyphen) and is in no whitelist, and Kodi drops the file.  Six new
types with hyphenated defaults would be six new ways to write files nothing
reads.  Short-form defaults (`banner.jpg`, `clearlogo.png`, …) are part of
D-D, not a follow-up.

With that, this is what unblocks a set image scraper rather than being blocked
by it: today there is nowhere to put a set clearlogo even if one were fetched,
which is why #1303, #1001, #421 and #822 cannot be closed by scraping alone.
It is also the point at which the per-type virtual methods on
`MediaCenterInterface` stop scaling — eight types would be sixteen methods —
and they should become one type-keyed pair taking a `MovieSet`.

One convention worth knowing about, because it is not the one MediaElch
implements and it is the only way Kodi will read set art that sits next to the
movies.  When a movie belongs to a set, Kodi adds `set.<type>` entries to the
movie's own wanted-art list (`VideoInfoScanner.cpp:2486-2487`) and then scans
the movie's own folder with that list (`:2527`).  A file named
`<movie base name>-set.poster.jpg` therefore yields the candidate `set.poster`
which, while it fails `IsValidArtType` because of the dot, still matches the
whitelist branch of the same condition (`:212-214`) — the two tests are
`||`-ed, so the second is still evaluated when the first fails.  Those entries
are then written to the set by `SetDetailsForMovie`
(`VideoDatabase.cpp:2429-2436`).  This is *not* MediaElch's
`ArtworkNextToMovies` layout, which writes `<setName>-poster.jpg` beside the
movie folders and which Kodi has never read.  If that mode is worth keeping,
this is the spelling that would make it work.  Read from the source and not
yet observed running.

### D-E: Out of Scope

**Ordering movies within a set.**  Kodi has no index column and no NFO element
for a movie's position in a set, in any version.  Membership is the bare
`idSet integer` column on the movie row with no ordinal beside it
(`xbmc/video/VideoDatabaseDDL.cpp:85`), the `sets` table has four columns and
none of them is an order (`:162-163`), and the movie-NFO `<set>` parser reads
only `name` and `overview` (`xbmc/video/VideoInfoTag.cpp:1488-1504`).  In-set
order is the ordinary movie sort, i.e. the per-movie `<sorttitle>` MediaElch
already edits in the sets tab.  Any ordering UI would be a promise the target
cannot keep.

**Ember's YAMJ `<sets><set order="0">` variant**
(`Ember-MM-Newscraper/EmberAPI/clsAPIMediaContainers.vb:1729-1731`,
`:2224-2249`).  Kodi never reads it.

**Pushing artwork to a running Kodi over JSON-RPC.**  Writing files that Kodi
picks up on its next scan is the contract; a live push is a different feature
with a different failure mode.


## Open questions

1. ~~**How does a `MovieSet` survive `MovieModel::clear()`?**~~  **Answered.**  The
   premise that there is no per-movie destruction notification was wrong: there is
   `QObject::destroyed`, and it is emitted for the `deleteLater()` that `clear()`
   runs (`src/model/MovieModel.cpp:278-289`).  A set connects to it in `addMovie()`
   and drops the member itself, so no reset signal has to be added to `MovieModel`,
   no `QPointer` holes have to be tolerated, and `MovieModel` does not have to own
   the set model.  See D-C.
2. ~~**How does the model learn about membership changes, if `Movie::setSet`
   stays?**~~  **Answered, for as long as it stays.**  Option 1: the model compares
   `movie->set().name` against the name it last saw on every `Movie::sigChanged`.
   The signal does fire for every change of any kind, but the comparison is a hash
   lookup and a string compare, which is cheaper than a second change signal on
   `Movie` that every future setter would have to choose between.  It goes away with
   the setter when the model becomes the only writer.
3. **Who owns the sets?**  The recommendation is `Manager`, next to the other
   models, because all three recompute sites and `KodiXml` need them and
   `Manager` is where the other cross-cutting models already live.  That adds
   to a class the module-system concept wants to shrink; the alternative is a
   `MovieSetModule` in the sense of `docs/concepts/module-system.md`, a better
   fit for where MediaElch is going and a worse fit for where it is.
4. **How hard is the folder requirement?**  D-A guards the sets tab behind a
   configured MSIF.  Whether that also means refusing to *read* sets from
   movie NFOs — i.e. showing an empty tab plus a warning rather than a
   read-only list — is a UX call with a migration consequence, since the
   one-shot materialisation pass has to run from somewhere.
