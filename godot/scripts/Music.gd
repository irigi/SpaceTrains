# Background music: the tracks in assets/music/manifest.csv, played on their own "Music" bus.
# Two shuffled decks (quiet and classical, the manifest's mood column): QUIET_PER_CLASSICAL quiet
# tracks, then one classical, with no repeat until a deck is used up. Silence between tracks,
# a fade-in, a short "now playing" note. N: next track, Shift+N: music off/on, [ / ]: volume
# (by key position: the two keys right of P). The setting is kept in user://settings.cfg.
# Main leaves it out in shot tours, headless runs and with --no-music; --music-log prints each start.
extends Node

const UiTheme := preload("res://scripts/ui/UiTheme.gd")

const MUSIC_DIR := "res://assets/music"
const SETTINGS_PATH := "user://settings.cfg"
const BUS_NAME := "Music"
const QUIET_PER_CLASSICAL := 2
const FIRST_DELAY_S := 4.0
const GAP_MIN_S := 20.0
const GAP_MAX_S := 60.0
const SKIP_GAP_S := 1.0
const FADE_IN_S := 2.0
const FADE_OUT_S := 1.5
const FADE_FLOOR_DB := -40.0
const NOTE_SHOW_S := 8.0
const NOTE_FADE_S := 1.0
const VOLUME_STEP := 0.1
const DEFAULT_VOLUME := 0.6

var tracks: Array[Dictionary] = []  # manifest rows: file, title, composer, performer, mood, gain_db
var decks := {"quiet": [], "classical": []}  # indices into tracks, drawn from the back
var quiet_since_classical := 0
var last_index := -1
var current := -1
var volume := DEFAULT_VOLUME
var enabled := true
var log_starts := false

var player: AudioStreamPlayer
var gap_timer: Timer
var fade: Tween
var note: Label
var note_tween: Tween

func setup(ui_root: Control) -> void:
    note = Label.new()
    note.name = "NowPlaying"
    note.set_anchors_and_offsets_preset(Control.PRESET_CENTER_BOTTOM)
    note.offset_top = -40.0
    note.offset_bottom = -16.0
    note.grow_horizontal = Control.GROW_DIRECTION_BOTH
    note.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
    note.add_theme_color_override("font_color", UiTheme.TEXT_DIM)
    note.mouse_filter = Control.MOUSE_FILTER_IGNORE
    note.visible = false
    ui_root.add_child(note)

func _ready() -> void:
    log_starts = "--music-log" in OS.get_cmdline_user_args()
    _load_settings()
    _load_manifest()
    _ensure_bus()
    player = AudioStreamPlayer.new()
    player.bus = BUS_NAME
    player.finished.connect(_on_track_finished)
    add_child(player)
    gap_timer = Timer.new()
    gap_timer.one_shot = true
    gap_timer.timeout.connect(_play_next)
    add_child(gap_timer)
    _apply_volume()
    if enabled and not tracks.is_empty():
        gap_timer.start(FIRST_DELAY_S)

func _unhandled_input(event: InputEvent) -> void:
    if not (event is InputEventKey and event.pressed and not event.echo):
        return
    if event.keycode == KEY_N and event.shift_pressed:
        _set_enabled(not enabled)
    elif event.keycode == KEY_N:
        if enabled:
            _skip()
    elif event.physical_keycode == KEY_BRACKETLEFT:
        _change_volume(-VOLUME_STEP)
    elif event.physical_keycode == KEY_BRACKETRIGHT:
        _change_volume(VOLUME_STEP)
    else:
        return
    get_viewport().set_input_as_handled()

# --- Playlist ---

func _next_index() -> int:
    var mood := "quiet"
    if quiet_since_classical >= QUIET_PER_CLASSICAL or _mood_count("quiet") == 0:
        mood = "classical"
    if _mood_count(mood) == 0:
        mood = "quiet"
    if decks[mood].is_empty():
        decks[mood] = _shuffled(mood)
    var index: int = decks[mood].pop_back()
    quiet_since_classical = 0 if mood == "classical" else quiet_since_classical + 1
    return index

func _shuffled(mood: String) -> Array:
    var deck := []
    for i in tracks.size():
        if tracks[i]["mood"] == mood:
            deck.append(i)
    deck.shuffle()
    # The deck is drawn from the back: don't start a new deck with the track just played.
    if deck.size() > 1 and deck.back() == last_index:
        deck.push_front(deck.pop_back())
    return deck

func _mood_count(mood: String) -> int:
    var count := 0
    for track in tracks:
        if track["mood"] == mood:
            count += 1
    return count

func _play_next() -> void:
    if not enabled or tracks.is_empty():
        return
    var index := _next_index()
    var track: Dictionary = tracks[index]
    var stream := _load_stream(track["file"])
    if stream == null:
        # A missing or unreadable file (e.g. a clone without Git LFS): drop it and go on.
        push_warning("Music: cannot load %s, skipped" % track["file"])
        tracks.remove_at(index)
        decks = {"quiet": [], "classical": []}
        last_index = -1
        if not tracks.is_empty():
            _play_next.call_deferred()
        return
    current = index
    last_index = index
    player.stream = stream
    var gain := float(track["gain_db"])
    player.volume_db = gain + FADE_FLOOR_DB
    player.play()
    _start_fade(gain, FADE_IN_S)
    _show_note("♪  %s — %s  ·  %s" % [track["title"], track["composer"], track["performer"]])
    if log_starts:
        print("[Music] %s (%s, gain %.1f dB, %.0f s)" % [track["file"], track["mood"], gain, stream.get_length()])

func _on_track_finished() -> void:
    current = -1
    if enabled:
        gap_timer.start(randf_range(GAP_MIN_S, GAP_MAX_S))

func _skip() -> void:
    gap_timer.stop()
    if not player.playing:
        _play_next()
        return
    _start_fade(player.volume_db + FADE_FLOOR_DB, FADE_OUT_S)
    fade.tween_callback(func() -> void:
        player.stop()
        current = -1
        gap_timer.start(SKIP_GAP_S))

func _set_enabled(on: bool) -> void:
    enabled = on
    _save_settings()
    gap_timer.stop()
    if on:
        _show_note("♪  Music on")
        gap_timer.start(SKIP_GAP_S)
    else:
        _show_note("♪  Music off   (Shift+N to turn on)")
        if player.playing:
            _start_fade(player.volume_db + FADE_FLOOR_DB, FADE_OUT_S)
            fade.tween_callback(func() -> void:
                player.stop()
                current = -1)

func _start_fade(target_db: float, seconds: float) -> void:
    if fade != null and fade.is_valid():
        fade.kill()
    fade = create_tween()
    fade.tween_property(player, "volume_db", target_db, seconds)

# --- Volume and settings ---

func _change_volume(delta: float) -> void:
    volume = clampf(snappedf(volume + delta, VOLUME_STEP), 0.0, 1.0)
    _apply_volume()
    _save_settings()
    _show_note("♪  Music volume %d %%" % roundi(volume * 100.0))

func _apply_volume() -> void:
    var bus := AudioServer.get_bus_index(BUS_NAME)
    AudioServer.set_bus_mute(bus, volume <= 0.0)
    AudioServer.set_bus_volume_db(bus, linear_to_db(maxf(volume, 0.001)))

func _ensure_bus() -> void:
    if AudioServer.get_bus_index(BUS_NAME) >= 0:
        return
    AudioServer.add_bus()
    var bus := AudioServer.bus_count - 1
    AudioServer.set_bus_name(bus, BUS_NAME)
    AudioServer.set_bus_send(bus, "Master")

func _load_settings() -> void:
    var config := ConfigFile.new()
    if config.load(SETTINGS_PATH) == OK:
        volume = clampf(float(config.get_value("music", "volume", DEFAULT_VOLUME)), 0.0, 1.0)
        enabled = bool(config.get_value("music", "enabled", true))

func _save_settings() -> void:
    var config := ConfigFile.new()
    config.load(SETTINGS_PATH)  # keep other sections
    config.set_value("music", "volume", volume)
    config.set_value("music", "enabled", enabled)
    config.save(SETTINGS_PATH)

# --- Files ---

func _load_manifest() -> void:
    var file := FileAccess.open(MUSIC_DIR.path_join("manifest.csv"), FileAccess.READ)
    if file == null:
        push_warning("Music: no manifest at %s" % MUSIC_DIR)
        return
    var header := file.get_csv_line()
    while not file.eof_reached():
        var line := file.get_csv_line()
        if line.size() != header.size():
            continue
        var row := {}
        for i in header.size():
            row[header[i]] = line[i]
        if row.get("mood", "") not in ["quiet", "classical"]:
            row["mood"] = "quiet"
        tracks.append(row)

func _load_stream(file_name: String) -> AudioStream:
    var path := MUSIC_DIR.path_join(file_name)
    if ResourceLoader.exists(path):
        return load(path)
    # Not imported (the project was not opened in the editor since the file came): read it directly.
    if FileAccess.file_exists(path):
        return AudioStreamOggVorbis.load_from_file(ProjectSettings.globalize_path(path))
    return null

# --- Now-playing note ---

func _show_note(text: String) -> void:
    if note == null:
        return
    if note_tween != null and note_tween.is_valid():
        note_tween.kill()
    note.text = text
    note.modulate.a = 1.0
    note.visible = true
    note_tween = create_tween()
    note_tween.tween_interval(NOTE_SHOW_S)
    note_tween.tween_property(note, "modulate:a", 0.0, NOTE_FADE_S)
    note_tween.tween_callback(note.hide)
