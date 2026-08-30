# Movie Sets

__Status__: Concept, work in progress  
__Last Updated on__: 2026-08-30

Two invitations stand open in the issue tracker.  bugwelle in #1747,
2024-05-18:

> the "movie set" feature needs an overhaul. It doesn't work properly and
> there is a lot to add to.

And again in #1809, 2024-11-04:

> The movie-set feature needs to be reworked. It was supported only poorly
> when I started and I never got to improve it either.

This document takes up that offer.  It describes what the movie-set code does
today, and proposes the one structural change the open feature requests all
turn out to depend on: making a set an object that exists.  Almost every
movie-set bug in the tracker is a symptom of it not existing — #800 and #1848
(no set overview), #1746 and #642 (set posters won't scrape), #1303, #1001,
#421 and #822 (missing set art types) are not seven independent gaps but one
gap seen from seven angles.

Three small fixes are already in flight and are noted below where they change
something described here: #2011 (write set artwork where Kodi can read it),
#2012 (keep the TMDB collection id in the movie NFO) and #2013 (renaming a set
no longer loses its data).  Everything else below describes `master`.


## Current State

### A Set Has No Existence of Its Own

`MovieSet` is a struct of three fields, `tmdbId`, `name` and `overview`
(`src/data/movie/MovieSet.h:8-17`), stored by value on each `Movie`
(`src/data/movie/Movie.h:272`).  Its translation unit contains a comment and
nothing else (`src/data/movie/MovieSet.cpp:3`).

There is no list of sets anywhere.  `SetsWidget::loadSets()` derives one, from
scratch, by walking every movie in the library and grouping by
`movie->set().name` (`src/ui/movie_sets/SetsWidget.cpp:120-134`), first
throwing away the four `QMap`s that hold everything the tab knows
(`SetsWidget.cpp:106-109`).  `MainWindow::onMenu` calls it on every switch to
the Movie Sets tab (`src/ui/main/MainWindow.cpp:881`).  A second, independent
copy of the same grouping fills the set combo box in the movie widget
(`src/ui/movies/MovieWidget.cpp:597-602`).

Consequences the user sees: leaving the tab with unsaved changes discards
them silently, because the pending-write map is cleared along with everything
else; and any per-set state we might want to add has nowhere to live for
longer than one tab visit.

### The Identity of a Set Is a Table Cell

Because there is no set object, the set the UI is operating on is identified by
the string in the row's `Qt::UserRole` (written at `SetsWidget.cpp:149` and
`:521`).  Five call sites read it back: `saveSet()` (`:436`),
`chooseSetPoster()` (`:365`), `chooseSetBackdrop()` (`:400`),
`onRemoveMovieSet()` (`:534`) and `onSetNameChanged()` (`:550`).  Renaming a
row never updates the role, so on `master` those five disagree with the
displayed name from the first rename onwards — `onRemoveMovieSet()` even
detaches the movies under one name (`:537`) while erasing the map entries
under the other (`:541-544`).  #2013 fixes this within the existing design; it
does not remove the design.

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
(`src/ui/movie_sets/SetsWidget.h:61-62`) and two virtual methods on the media
center interface (`src/media_center/MediaCenterInterface.h:34-35`).  Adding a
third type means touching all five places.

The interface is keyed by set *name*, not by a set: `saveMovieSetPoster(QString
setName, QImage)`.  `KodiXml::movieSetFileName` then has to find the set again
— in "artwork next to movies" mode it linear-scans the entire movie model
looking for any member (`src/media_center/KodiXml.cpp:1316-1327`) — and
`DataFile::saveFileName` substitutes the name into the template
(`src/settings/DataFile.cpp:52-57`).  Set art is also the only artwork
MediaElch re-encodes: `poster.save(fileName, "jpg", 100)` at
`KodiXml.cpp:1226` and `:1242`, regardless of the template's extension, where
movie, show and concert art are written as the bytes that were downloaded.
Whether those paths are ones Kodi reads at all is the subject of #2011, #1747,
#1158 and #1809, and is not revisited here.

Kodi is much more generous than two types.  At the default artwork level any
ASCII-alphanumeric name of at most 25 characters found in the movie-set
information folder is imported (`xbmc/video/VideoThumbLoader.cpp:143-150`,
reached with `addAll = true` from
`xbmc/video/VideoInfoScanner.cpp:866`); `poster` and `fanart` are the
guaranteed defaults (`VideoThumbLoader.cpp:88`) and the whitelist that applies
at the stricter levels offers `clearart`, `discart`, `keyart`, `banner`,
`landscape` and `clearlogo` (`system/settings/settings.xml:1374-1379`).  So
six of the eight types Kodi will happily display for a set have no
representation in MediaElch at all.

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

Metadata scraping is closer than it looks.  `TmdbMovieScrapeJob` already
follows `belongs_to_collection` and fetches `/3/collection/{id}`
(`src/scrapers/movie/tmdb/TmdbMovieScrapeJob.cpp:116-148`,
`src/scrapers/tmdb/TmdbApi.cpp:294-303`) — but reads only `id`, `name` and
`overview` out of the response (`:143-147`) and discards `poster_path` and
`backdrop_path`, which are in the same JSON object.

### The Overview Is Write-Only

A set overview can be read from an NFO (`MovieXmlReader.cpp:180-182`), carried
on the `MovieSet` value, and written back (`MovieXmlWriter.cpp:109-115`), and
TMDB fills it in during a movie scrape (`TmdbMovieScrapeJob.cpp:146`).  There
is no widget anywhere that displays or edits it, and the two places that
rebuild a `MovieSet` from a name throw it away.  In practice it survives only
a round trip that touches nothing — which is why #800 reports it as never
populated and #1848 asks for it as a missing feature.  The TMDB collection id
has the same shape of problem: scraped, held in memory, never written
(#2012).


## Proposal

### D-A: A Set Is a Projection Over Its Member Movies

The source of truth for a set is the `<set><name>` and `<set><overview>`
elements in its members' NFOs.  A set with no members cannot exist, because
there is nowhere on disk for it to be recorded.  This is not a design
preference; it is what Kodi does — membership is a single nullable foreign key
on the movie row and the `sets` table is populated as a side effect of
scanning movies (`xbmc/video/VideoDatabaseDDL.cpp:162-163`,
`xbmc/video/VideoDatabase.cpp:2423-2428`).

Three consequences are worth stating explicitly, because they constrain the
code:

Editing a set fans out.  Changing an overview is N NFO writes, one per member,
and renaming a set is a rewrite of every member's `<set><name>`.  The set
object is where the fan-out is decided; it is not something a widget should be
doing in a loop.

The same overview must go into *every* member.  Kodi 19 and 20 keep the
first-scanned member's copy and ignore the rest — `AddSet` returns the
existing row's id and runs no `UPDATE` at all
(`19.5-Matrix:xbmc/video/VideoDatabase.cpp:1664-1668`) — while Kodi 21 and 22
let the last-scanned member win (`VideoDatabase.cpp:1554-1556`).  Identical
text in every member is the only way to get a deterministic result on all
four.

And an *empty* overview must never be written.  MediaElch currently emits
`<overview></overview>` unconditionally whenever a set name exists
(`MovieXmlWriter.cpp:109-115`).  `XMLUtils::GetString` returns `true` for an
existing-but-empty element (`xbmc/utils/XMLUtils.cpp:261-262`), which reaches
`CSetInfoTag::SetOverview` and sets `m_updateSetOverview` unconditionally
(`xbmc/video/SetInfoTag.cpp:58-63`, via
`xbmc/video/VideoInfoTag.cpp:1895-1897`), which is passed straight to `AddSet`
(`VideoDatabase.cpp:2427-2428`) and runs `UPDATE sets SET strOverview = ''`.
So on Kodi 21 and 22 one member with an empty set overview, scanned last,
blanks the whole set's overview.  Writing the same text everywhere is not
sufficient; the write also has to be skipped when the text is empty.

Kodi 22's `set.nfo` and the movie-set information folder are **additional
sinks**, never the source of truth.  `set.nfo` first shipped in Kodi 22 —
`git tag --contains fbee563fc4` lists only the four `22.0*-Piers` tags — and
is inert on the Kodi 19-21 releases MediaElch also supports.  Anything that
lives only there is invisible to most of our users.

### D-B: The Identity Key Is the Name, Not the TMDB Id

Kodi matches a set by name.  On Kodi 22 the match is byte-exact: `SELECT idSet
FROM sets WHERE strOriginalSet='...'` on a plain `text` column with no
collation (`VideoDatabase.cpp:1535-1536`, `VideoDatabaseDDL.cpp:162-163`).  On
19-21 it is `SELECT idSet FROM sets WHERE strSet LIKE '...'`
(`19.5-Matrix:xbmc/video/VideoDatabase.cpp:1654`), which is looser in one
direction — case-insensitive for ASCII — and treats `%` and `_` as
wildcards.
Byte-identical names in every member satisfy all four releases; nothing else
does.  Two sets with the same name *are* one set to Kodi, and a set has no id
in any NFO format Kodi reads — `CSetInfoTag` has no id field that comes from
XML at all (`xbmc/video/SetInfoTag.h:68-74`, parser at
`xbmc/video/SetInfoTag.cpp:35-56`).

So the name is the primary key.  The TMDB collection id is a scraping and
re-identification attribute
carried alongside the name — it tells us which collection to re-fetch, and it
survives a rename — but it never identifies the set.  #2012 gives it somewhere
to be persisted; it does not promote it.

Corollary for the UI: renaming a set is a real operation on N files, and
renaming a set onto an existing name is a merge.  The current code already
does the merge (`SetsWidget.cpp:555-560`) without saying so.

### D-C: Promote `MovieSet` to an Entity, and Give the List a Model

The value stored on `Movie` and the set itself are two different things, and
should be two types.  What a movie's NFO says about its set is a value; the
set is an aggregate over the movies that say it.

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

signals:
    void sigChanged(MovieSet* set);
};
```

`MovieSetImages` follows `MovieImages` (`src/data/movie/MovieImages.h:67-71`):
a `QMap<ImageType, QByteArray>` of downloaded bytes plus the has-changed flags,
so set art stops being re-encoded to JPEG and starts behaving like every other
image MediaElch writes.  The mutators are the fan-out point from D-A: they
update the members' `MovieSetInfo` and mark them changed, and that is the only
place that knows an edit means N writes.

Above it, a `MovieSetModel` in `src/model/` alongside `MovieModel` and
`TvShowModel`, owning its `MovieSet*`s, exposing them the way `MovieModel`
exposes movies (a `MovieSetPointerRole`, `sets()`, `set(name)`, `addSet`,
`removeSet`), and owned by `Manager` next to the other models
(`src/globals/Manager.h:62-65`, `:91-94`).  `SetsWidget` then binds to a model
instead of rebuilding four `QMap`s per tab visit, and `MovieWidget`'s set combo
reads the same model instead of grouping the library a second time.

Keeping the model in sync with `MovieModel` is the part worth reviewing
carefully.  The recommendation is that `MovieSetModel` holds a reference to
`MovieModel`, rebuilds on reset, and maintains itself incrementally on
`Movie::sigChanged` — which `MovieModel` already connects to for every movie
it adopts (`src/model/MovieModel.cpp:27-42`).  Because that signal is emitted
for any change at all (`Movie::setSet` at `src/data/movie/Movie.cpp:779-783`
is one of dozens of setters that call `setChanged(true)`), the model needs a
`QHash<Movie*, QString>` of last-known membership to tell a set change from
any other change.  That is cheap, but it is a real design choice; the
alternative is recorded under Open questions.

Two things this buys immediately, beyond the tab: `movieSetFileName` stops
scanning the library because a set knows its members, and the media-center
interface can take a set instead of a name.

### D-D: Extend Set Artwork to Kodi's Full Complement

Add the six art types Kodi accepts for a set that MediaElch has no
representation for, as `ImageType` and `DataFileType` entries, using the names
MediaElch already uses for the same Kodi art type on movies
(`src/settings/Settings.cpp:91-95`):

| New type            | Kodi art type / filename |
|---------------------|--------------------------|
| `MovieSetBanner`    | `banner`                 |
| `MovieSetClearArt`  | `clearart`               |
| `MovieSetLogo`      | `clearlogo`              |
| `MovieSetCdArt`     | `discart`                |
| `MovieSetThumb`     | `landscape`              |
| `MovieSetKeyArt`    | `keyart`                 |

together with the existing poster and backdrop, that is Kodi's whitelist plus
its two defaults.  `keyart` is the only one with no movie-level counterpart in
MediaElch; it is a textless poster.

This is what unblocks a set image scraper rather than being blocked by it:
today there is nowhere to put a set clearlogo even if one were fetched, which
is why #1303, #1001, #421 and #822 cannot be closed by scraping alone.  It is
also the point at which the per-type virtual methods on
`MediaCenterInterface` stop scaling — eight types is sixteen methods — and
should become one type-keyed pair taking a `MovieSet`.

### D-E: Out of Scope

**Ordering movies within a set.**  Kodi has no index column and no NFO element
for a movie's position in a set, in any version: the `sets` table has four
columns and none of them is an order (`VideoDatabaseDDL.cpp:162-163`),
membership is a plain foreign key with no ordinal, and no `setorder` /
`iSetOrder` / `setindex` identifier exists anywhere in the tree.  In-set order
is the ordinary movie sort, i.e. the per-movie `<sorttitle>` MediaElch already
edits in the sets tab.  Any ordering UI would be a promise the target cannot
keep.

**Ember's YAMJ `<sets><set order="0">` variant**
(`Ember-MM-Newscraper/EmberAPI/clsAPIMediaContainers.vb:1729-1731`,
`:2224-2249`).  Kodi never reads it.

**Pushing artwork to a running Kodi over JSON-RPC.**  Writing files that Kodi
picks up on its next scan is the contract; a live push is a different feature
with a different failure mode.


## Open questions

1. **How does `MovieSetModel` learn about membership changes?**  The
   recommendation above diffs `movie->set().name` against a
   `QHash<Movie*, QString>` on every `Movie::sigChanged`.  The alternative is
   a dedicated `Movie::sigSetChanged`, emitted only from `Movie::setSet` —
   cheaper and self-documenting, but it adds a second change signal to `Movie`
   and every future setter has to remember which one it belongs to.  A third
   option is to make the model the only writer, i.e. remove `Movie::setSet`
   from the public API, which is the cleanest and by far the largest diff.
2. **Who owns the sets?**  The recommendation is `Manager`, next to the other
   models, because `SetsWidget`, `MovieWidget` and `KodiXml` all need them and
   `Manager` is where the other cross-cutting models already live.  That does
   add to a class the module-system concept wants to shrink; the alternative
   is a `MovieSetModule` in the sense of `docs/concepts/module-system.md`,
   which is a better fit for where MediaElch is going and a worse fit for
   where it is.
3. **Should a set be creatable while empty?**  D-A says no set exists without
   members, but the sets tab today lets the user create an empty set and hold
   it in `m_addedSets` until a movie is added (`SetsWidget.cpp:508`,
   `:135-142`).  Keeping that as an explicitly unsaved, in-memory draft seems
   right, but it means the model has rows that are not projections of
   anything, and the distinction has to be visible in the UI or users will
   lose work.
4. **What happens to a set that only exists in the artwork folder?**  Once
   the last member movie loses its `<set>`, the set's images stay on disk
   under a name nothing references.  Deleting them is destructive, keeping
   them accumulates orphans.  Ember tracks an `OldTitle` and deletes on
   rename; that is a decision to make deliberately rather than inherit.
5. **Is the set overview edited per set, or per movie?**  D-A makes it a set
   property, but the movie NFO is where it lives, and a user who edits a
   movie's NFO by hand can make members disagree.  Presumably the sets tab
   owns it and the movie widget shows it read-only — but that is a UI
   decision that affects where the fan-out lives.
