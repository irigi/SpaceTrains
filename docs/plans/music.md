# Plan: music (2026-11-02)

**Wish (user, 2026-11-02):** music in the game, free to use, with no licence to buy or negotiate.
Downloaded or generated. Guides for the mood: the Frontier: Elite II playlist (classical pieces such as
Wagner's *Ride of the Valkyries*) and the *First Man* score (Justin Hurwitz: quiet and minimal). That score is under copyright and is not an option; it only sets the mood.

## What "free to use" means here

The game is on a public GitHub repo and ships as downloadable archives, so every track must be safe to
redistribute worldwide, bundled inside a game:

- **Accepted:** public domain (worldwide, or by dedication: CC0 / PD Mark on a recording whose maker dedicated
  it) and **CC BY** (credit only; the credits ship with the game).
- **Not accepted:** NC (non-commercial), ND (no derivatives: cutting a loop or normalising may count), SA
  (share-alike pulls obligations onto the bundle), "free for personal use", or a page with no clear licence.
- **Old composition ≠ free recording.** Wagner, Strauss, Grieg, Mussorgsky and Holst (died 1934, public domain
  in the EU since 2005) wrote public-domain music, but a modern orchestra's recording has its own copyright.
  We need recordings whose own rights are given away.
- **Old recordings** (Edison and Victor discs before 1926) are public domain in the US, and in the EU, where a
  recording's term (then 50 years) ran out long ago; they sound like 78s, so use them only if the user likes that.
- Every track gets a row in a manifest: title, composer, performer, licence, source URL, download date. The
  manifest makes the credits screen and `CREDITS.txt` in the package; a track with a doubtful row stays out.

## Where to look

- **Classical, "Frontier" mood:** Musopen (recordings commissioned and released without rights, e.g. the
  2010 Kickstarter symphonies; check each file's licence), Wikimedia Commons (filter by PD/CC0/CC BY; US
  military band recordings are US government works, so public domain), the Internet Archive's Musopen uploads,
  OpenGameArt (a "Holst – The Planets" upload to check). Targets: *Ride of the Valkyries*, *The Blue Danube*,
  *In the Hall of the Mountain King*, *Night on Bald Mountain*, Holst's *Mars* and *Neptune*.
- **Quiet, "First Man" mood:** CC0 ambient on OpenGameArt (e.g. Yoiyami's "Blue Series", cynicmusic),
  FreePD.com (CC0), Kevin MacLeod / incompetech (CC BY 4.0). Long, slow, pad and piano pieces.
- **Generated:** our own pieces (rendered offline from code, e.g. slow pads and drones) are ours by
  construction. A fallback for gaps, likely below the quality of the best free recordings. AI music services
  are out: their terms restrict use and ownership is unclear.

**I cannot listen to audio.** I can find, check licences, download and measure (length, loudness, loop
points), but the user has to choose by ear. So the first step makes a shortlist the user listens to.

## Design in the game

- `godot/assets/music/` with OGG Vorbis files (~1 MB a minute at 128 kbps: an hour is ~60 MB on top of the
  ~230 MB package) and `manifest.csv`.
- A `Music` node: `AudioStreamPlayer` on its own `Music` bus; shuffled playlist without repeats; a few
  seconds' fade in and out; a pause between tracks (silence suits space and keeps it from wearing thin).
- Possibly two moods: quiet pieces most of the time, the classical set now and then (or on fast time warp).
  To be decided after listening.
- Controls: volume and mute, next track; a short "now playing" note (title, composer, performer) in a
  corner when a track starts. Keys to be chosen (M is the markets table; the key-binding test lists them);
  the setting kept in `user://`.
- Off in shot tours and headless runs. Loudness evened out across tracks (ReplayGain-style gain per file,
  in the manifest).
- `build_release.sh` and the export already carry `godot/assets/`; add `CREDITS.txt` from the manifest to
  the package.

## Steps

1. **Shortlist** (Claude) — done 2026-11-02: 24 recordings with licences checked on each file page (Wikimedia
   Commons API, Philip Milman's and Scott Buckley's pages), published as a private page for the user to listen and pick.
   Strongest sources: US Marine Band / Air Force Band (public domain), Musopen Kickstarter (Czech National SO, public
   domain), Peabody Symphony Mahler (CC0), Philip Milman / Lud and Schlatt (CC BY via YouTube), Scott Buckley (CC BY 4.0
   plus his own terms).
   Original step:: 15–25 candidate recordings with verified licences, both moods, as a page of links
   and notes (length, performer, licence, source). The user listens and picks ~8–15.
2. **Fetch and prepare** the picks: download into a fresh folder, convert to OGG, trim silences, even out
   loudness, record each in the manifest.
3. **Player in Godot:** Music node, bus, playlist, fades, controls, settings, now-playing note, key-binding
   table and test.
4. **Credits:** in-game credits (help screen or a small panel) and `CREDITS.txt` in the package.
5. **Checks:** parse check; run the game and confirm playback and volume (the user listens: Claude can see
   playback state and logs only); package size noted; the Windows package still builds.
6. Docs: `docs/modules/ui_observer.md` (music) and `docs/modules/deployment.md` (credits, size).

## Open for the user

- Mix: mostly quiet with classical now and then (proposed), or classical first?
- How much music: ~45–60 minutes proposed (50–60 MB).
- Generated pieces: wanted at all, or only if the free recordings leave gaps?
