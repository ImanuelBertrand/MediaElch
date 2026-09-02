# Movie Sets

__Status__: Partially implemented; see "What Is Not Built Yet"  
__Last Updated on__: 2026-09-02

Movie sets (Kodi calls them sets, TMDB calls them collections) were supported
only poorly in MediaElch: a set was a string on each movie and nothing else.
Almost every open set issue is a symptom — the missing artwork types of #1303,
#1001, #421 and #822, the overview with no editor, the scraped-and-dropped TMDB
collection id (#2012).

This is a concept, not a walkthrough; the details live in the comments of
`MovieSetModel`, `MovieSet` and `KodiXml`.  Everything I say about Kodi I read
from its source, not from a running Kodi.


## Current State

### Sets in Kodi

A set exists in Kodi because a movie's NFO names it in `<set><name>`.  Kodi
never discovers sets on its own: it derives them while scanning movies, and a
movie belongs to at most one set.

The `<set>` element may also carry an `<overview>`.  Several members can each
carry one, so Kodi has to choose — and an *empty* `<overview>` is a value, not
an absence, so a single empty one blanks the whole set's overview.

Kodi can also be given a "movie set information folder":

| Kodi   | Set artwork in it | `set.nfo` in it | Overview when members disagree |
|--------|-------------------|-----------------|--------------------------------|
| 19, 20 | read              | ignored         | the member scanned *first* wins |
| 21     | read              | ignored         | the member scanned *last* wins  |
| 22     | read              | read            | the member scanned *last* wins  |

A `set.nfo` overrides what the movies said, and the set is then *matched* on
its `<originaltitle>` — which every member's `<set><name>` has to equal.

### Sets in MediaElch Before This Work

MediaElch had no set object.  A set was a `MovieSetInfo` value — name,
collection id, overview — on each movie, and three unrelated places recomputed
the list by walking all movies: the sets tab, the combo box and the set filter.

Renaming a set put a name-only value on every member, so the overview and the
id were lost.  The overview had no editor, and there is still no set scraper.


## Proposal

### A Set Is an Object, and a Model Owns the List

`MovieSet` is a `QObject` with a name, a display title, a collection id, an
overview, its artwork and a list of member movies it does not own.

`MovieSetModel` owns all sets.  It groups the movies once and then follows
them: their change signal, the movie model's row removal, object destruction as
the backstop.  The sets tab, the combo box and the filter all read this one
list, held by `Manager` next to the other models.

### Membership Comes From the Movies

Kodi's rule is also MediaElch's: a movie is in a set because its own NFO says
so, and `<set><name>` is the only thing that can say so.  Membership is never
stored in the set's own file.

A membership change is therefore an edit of the *movie*, and the model has one
entry point for it: it writes the value, moves the movie and marks it changed,
and does nothing when the value has not changed.  Everywhere else the model
follows the library rather than editing it, and dirties nothing.  Get that wrong
and you either lose an edit or rewrite every NFO on every reload.

The per-movie setter stays public: the NFO reader and the scrape merger cannot
tell a library movie from a transient one, so the model reconciles afterwards.

### Attributes Live in `set.nfo`

With a movie set information folder configured, a set has a record of its own
at `<folder>/<legalised name>/set.nfo`, in a folder named after the set with
the characters Kodi cannot use in a path replaced:

```xml
<set>
  <title>Alien Anthology</title>
  <originaltitle>Alien Collection</originaltitle>
  <overview>…</overview>
  <uniqueid type="tmdb">8091</uniqueid>
</set>
```

The record is authoritative for the overview and the collection id (#2012), but
Kodi 19 to 21 never read it, so both have to reach every member NFO too — with
*identical* text in each, since the versions disagree about which member wins
and identical text is the only deterministic answer.  An empty one is never
written.

`<art>` is neither written nor read — the artwork is the image files in the
same folder, which every Kodi reads first, so a record entry would duplicate it.

The record also gives a set *existence*: a set with a `set.nfo` outlives its
last member, a set without one is nothing but the grouping of its movies.
Whether it has one is a filesystem fact, not a guess from unsaved changes.

Saving a set writes the changed members, the pending artwork and the record.
Only the latter two writes are checked, which is why a failure message opens by
saying the movies were saved.  Removing a set removes the record before the
members: one that outlived its set would bring it back at the next reload.

### Two Names: Match Key and Display Title

`MovieSet::name()` is the match key: byte-identical to `<set><name>` in every
member NFO and to `<originaltitle>` in `set.nfo`, it names the set's folder,
and the model keys its sets by it.  That is three copies of one string, and a
rename is correct only if it moves all three together.

`MovieSet::title()` is the display title, `set.nfo`'s `<title>`, empty whenever
it equals the key; the two diverge only after a set-file-only rename (below).
A set with no record cannot have one, because a movie NFO has no element for
it.  Names are untrimmed on both sides: a normalised key would not match.

The collection id is deliberately not the key.  Kodi matches sets by name and
reads no id from any set element, so two sets sharing a name are one set to
Kodi.  The id rides along and survives a rename, but never identifies the set.

### Without a Folder, Sets Are Just Their Movies

The shipping default stores set artwork next to the movies and has no
information folder.  bugwelle's objection in #1243 to changing that still holds:
the alternative makes the user configure a directory before anything works.

Sets are still read from the movie NFOs and shown, and everything stored in
the movies still works, from membership to the sort title.  What needs the
folder is off — `set.nfo`, and with it *Add Movie Set*, the sets tab's button
for a set with no members yet, which would have nowhere to be remembered.  With
that layout chosen but no folder, artwork is off too; earlier code wrote into
the working directory instead.

The sets tab shows a notice for whichever of the three states applies.  I
worded it so the default does not read as a mistake, because it is the default
I ship.  One predicate answers "are records configured", asked live so that a
settings change takes effect at once.

### A Derived Set Is Seeded From Its Members

A derived set is created from a name alone, although its members' NFOs may
carry the overview and the id — so whenever a set without a record gains a
member, the model fills its empty overview and id from that member.  Otherwise
the set's first `set.nfo` would be written from emptiness, and an edited
overview mirrored back would destroy what the members held.  Seeding is
read-side only: nothing is written and no set gains a record by it.

### When a Set Is Dropped

A set is never dropped merely for becoming empty: an edit that removes its
last movie leaves it standing, and a set created by *Add Movie Set* is a set
before it has members.

Sets go when the library is re-derived — a reload, or the movies leaving the
movie model — and nothing is left to derive them from, or when a user removes
them.  Re-derivation spares a set that has a record, and is also where a set
with a record but no members is found, by listing the folder's `set.nfo` files.

A reload refreshes only *whether* a record exists, not its contents, so an
unsaved edit survives it.  The exception is a set that had no record and now
has one, which has to be read or it would write its emptiness over the file.

### Renaming Is a Setting

There are two genuinely different renames, and which one is right depends on
the Kodi version, so `MovieSetRenameMode` has three states:

| Setting | Rewrites | Consequence |
|---|---|---|
| *All movie files* | `<set><name>` in every member, the set's folder and `set.nfo`'s `<originaltitle>` | correct on every Kodi, but on 22 the old set row stays behind with its artwork and id until Clean Library runs |
| *Set file only* | `set.nfo`'s `<title>` alone | Kodi 22 renames the row in place and keeps its artwork and id; Kodi 21 and earlier never see the rename, and it needs a record to live in |
| *Automatic* (default) | set-file-only where Kodi is 22 or later **and** records are configured, all movie files otherwise | — |

Automatic's second condition matters because a fresh install is Kodi 22 with
no folder.  An explicit *set file only* where there are no records I refuse
with a message rather than quietly downgrade: the only fallback is the heavier
rename this user picked the setting to avoid.  `resolveRenameMode()` is the
single derivation, static so it can be tested on its own.

Two invariants govern the all-movie-files rename.  The set's files move on
disk *before* its members are reassigned, because in the next-to-movies layout
the artwork's path resolves through a member whose set name is still the old
one.  And the set object is renamed rather than replaced, so overview, id and
artwork stay with it; the rename also clears the display title.

Kodi 22 charges two small prices after a set-file-only rename, because its art
picker and the movie's `set.*` art lookup both go through the display title's
folder.  I accept them: orphaning the set row costs more.

### Merge Is Not a Rename

Renaming a set to a name another set already has is a merge: every movie moves
into the existing set.  The sets tab asks first, because that cannot be undone.

A merge is always the all-movie-files operation whatever the setting says,
since a display title cannot merge anything.  The source's record is removed
and its artwork is not carried over, because the target has its own.  Two sets
may not share a display title either, or the tab would show indistinguishable
rows — checked where MediaElch picks the name, not where it mirrors the files.

### Artwork

Set artwork is the image files in the set's folder, or beside the movies in
the default layout, named by the movie set data-file templates:
`<setName>-poster.jpg` and `<setName>-fanart.jpg` by default.

Which types a set supports is data rather than code, so Kodi's remaining set
art types (banner, clearart, clearlogo, discart, landscape, keyart) can be
added without changing the class.  Only poster and backdrop exist today, which
is why #1303, #1001, #421 and #822 are still open.

Whatever the user picks is decoded on the way in and re-encoded to JPEG on the
way out.  Writing the downloaded bytes verbatim instead is worth doing when the
artwork work happens; nothing holds them today.

Kodi has never read the `<setName>-poster.jpg` files the next-to-movies layout
writes; it reads `<movie base name>-set.poster.jpg` there, which we never write.


## What Is Not Built Yet

- The further artwork types above, and the short-form file names Kodi's
  information-folder reader needs for them.
- The mirror into the member NFOs.  Nothing pushes a set's overview or id onto
  its members — only the NFO reader and the scrapers write those values there.
- The third copy of the match key in an all-movie-files rename: the record
  moves into the new folder, but its `<originaltitle>` is not rewritten.
- Set scraping.  Nothing fetches a collection's overview, id or artwork into a
  `MovieSet`; the TMDB scraper only fills the *movie's* set value.
- An editor for the overview and the id in the sets tab, and a scrape and
  save-all workflow there.
- One type-keyed pair of artwork methods on the media center interface instead
  of the per-type virtuals, and with them somewhere to keep the downloaded
  bytes so that artwork is written verbatim rather than re-encoded.


## Out of Scope

Ordering movies within a set: Kodi has no element or column for it, so in-set
order is the ordinary movie sort — the per-movie sort title the sets tab
already edits.  Ember's `<sets><set order="0">` variant, which Kodi never
reads.  Pushing artwork to a running Kodi over JSON-RPC, a different feature.


## Open Questions

- **When does a set earn a record?**  Writing one for every set would fill the
  combo box and the filter with sets no movie answers to.  Today only saving a
  set writes one; whether a rename or an edited overview should, I don't know.
- **Where the sets live.**  `Manager` holds the model; a `MovieSetModule` in the
  sense of `module-system.md` would fit where I want MediaElch to go, not where
  it is.
- **File names in the information folder.**  Kodi takes the file stem in a
  set's folder as the art type; I have not confirmed whether the default
  `<setName>-poster.jpg` is accepted there, or only `poster.jpg`.
- **Kodi behaviour is read from source, not observed.**  The empty-overview
  blanking and the two Kodi 22 rename prices deserve a manual check.
