(() => {
  "use strict";

  const AUDIO_EXTENSIONS = new Set([
    "mp3", "ogg", "oga", "wav", "flac", "m4a", "aac", "opus", "webm", "wma", "mp4"
  ]);
  const TEXT_FRAME_KEYS = {
    TIT2: "title",
    TT2: "title",
    TPE1: "artist",
    TP1: "artist",
    TALB: "album",
    TAL: "album",
    TYER: "year",
    TDRC: "year"
  };
  const REPEAT_MODES = ["off", "all", "one"];
  const STORAGE_KEYS = {
    theme: "sound-memoirs-theme",
    volume: "sound-memoirs-volume",
    repeat: "sound-memoirs-repeat",
    shuffle: "sound-memoirs-shuffle"
  };
  const THEME_COPY = {
    original: {
      modeLabel: "LOCAL AUDIO TERMINAL / 640×480 LINEAGE",
      displayCode: "MONITOR / 43 BAND",
      displayTitle: "SIGNAL FIELD",
      libraryCode: "STORAGE / LOCAL",
      libraryTitle: "LOCAL INDEX",
      frameMessage: "DROP LOCAL AUDIO ANYWHERE / TOUCH & DRAG TO SEEK",
      themeFootLabel: "ORIGINAL INSTRUMENT LAYOUT",
      themeColor: "#d9e3e0"
    },
    paper: {
      modeLabel: "FIELD RECORD / PRIVATE LISTENING ROOM",
      displayCode: "SIGNAL DOCUMENT / 43 BAND",
      displayTitle: "AUDIO FIELD NOTES",
      libraryCode: "CATALOGUE / PRIVATE",
      libraryTitle: "LISTENING FILE",
      frameMessage: "LOCAL RECORD ONLY / TOUCH & DRAG TO SEEK",
      themeFootLabel: "PAPER ARCHIVE LAYOUT",
      themeColor: "#d8cfbd"
    }
  };

  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
  const clamp = (value, min, max) => Math.min(max, Math.max(min, value));

  const els = {
    appFrame: $("#drop-zone"),
    displayModule: $(".display-module"),
    fileInput: $("#file-input"),
    addFilesButton: $("#add-files-button"),
    emptyAddButton: $("#empty-add-button"),
    trackSearch: $("#track-search"),
    queueList: $("#queue-list"),
    queueScroll: $("#queue-scroll"),
    emptyState: $("#empty-state"),
    libraryCount: $("#library-count"),
    currentIndexLabel: $("#current-index-label"),
    clearButton: $("#clear-button"),
    previousButton: $("#previous-button"),
    nextButton: $("#next-button"),
    playButton: $("#play-button"),
    shuffleButton: $("#shuffle-button"),
    repeatButton: $("#repeat-button"),
    repeatLabel: $("#repeat-label"),
    muteButton: $("#mute-button"),
    seekTrack: $("#seek-track"),
    volumeTrack: $("#volume-track"),
    volumeFill: $("#volume-fill"),
    volumeThumb: $("#volume-thumb"),
    volumeValue: $("#volume-value"),
    elapsedTime: $("#elapsed-time"),
    durationTime: $("#duration-time"),
    playerStatus: $("#player-status"),
    busState: $("#bus-state"),
    signalLabel: $("#signal-label"),
    trackTitle: $("#track-title"),
    trackArtist: $("#track-artist"),
    playingKicker: $("#playing-kicker"),
    trackFormat: $("#track-format"),
    albumCell: $("#album-cell"),
    coverImage: $("#cover-image"),
    visualizer: $("#visualizer"),
    meterLeft: $("#meter-left"),
    meterRight: $("#meter-right"),
    dropOverlay: $("#drop-overlay"),
    toast: $("#toast"),
    sessionClock: $("#session-clock"),
    themeButtons: $$("[data-theme-choice]")
  };

  const readStored = (key, fallback) => {
    try {
      return localStorage.getItem(key) ?? fallback;
    } catch {
      return fallback;
    }
  };

  const writeStored = (key, value) => {
    try {
      localStorage.setItem(key, String(value));
    } catch {
      // Storage can be unavailable for local files or privacy modes.
    }
  };

  const storedVolume = Number.parseFloat(readStored(STORAGE_KEYS.volume, "80"));
  const storedRepeat = readStored(STORAGE_KEYS.repeat, "off");
  const storedTheme = readStored(STORAGE_KEYS.theme, "original");
  const storedShuffle = readStored(STORAGE_KEYS.shuffle, "false");

  const state = {
    tracks: [],
    currentIndex: -1,
    isPlaying: false,
    isSeeking: false,
    isMuted: false,
    volume: Number.isFinite(storedVolume) ? clamp(storedVolume / 100, 0, 1) : 0.8,
    repeat: REPEAT_MODES.includes(storedRepeat) ? storedRepeat : "off",
    shuffle: storedShuffle === "true",
    search: "",
    theme: THEME_COPY[storedTheme] ? storedTheme : "original",
    audioContext: null,
    analyser: null,
    sourceNode: null,
    frequencyData: null,
    waveformData: null,
    audioGraphFailed: false,
    toastTimer: 0,
    dragDepth: 0,
    peaks: new Float32Array(43)
  };

  const audio = new Audio();
  audio.preload = "metadata";
  audio.volume = state.volume;

  function formatTime(seconds) {
    if (!Number.isFinite(seconds) || seconds < 0) return "00:00";
    const total = Math.floor(seconds);
    const hours = Math.floor(total / 3600);
    const minutes = Math.floor((total % 3600) / 60);
    const secs = total % 60;
    if (hours > 0) {
      return `${String(hours).padStart(2, "0")}:${String(minutes).padStart(2, "0")}:${String(secs).padStart(2, "0")}`;
    }
    return `${String(minutes).padStart(2, "0")}:${String(secs).padStart(2, "0")}`;
  }

  function formatBytes(bytes) {
    if (!Number.isFinite(bytes)) return "LOCAL";
    if (bytes < 1024 * 1024) return `${Math.max(1, Math.round(bytes / 1024))} KB`;
    return `${(bytes / (1024 * 1024)).toFixed(bytes >= 10 * 1024 * 1024 ? 0 : 1)} MB`;
  }

  function fileExtension(name) {
    return name.split(".").pop()?.toLowerCase() || "";
  }

  function isAudioFile(file) {
    if (!file) return false;
    if (file.type?.startsWith("audio/")) return true;
    return AUDIO_EXTENSIONS.has(fileExtension(file.name));
  }

  function cleanDisplayName(name) {
    return name
      .replace(/\.[^.]+$/, "")
      .replace(/[_]+/g, " ")
      .replace(/\s+/g, " ")
      .trim();
  }

  function parseFileName(name) {
    const base = cleanDisplayName(name);
    const match = base.match(/^(.+?)\s+-\s+(.+)$/);
    if (!match) return { title: base || name, artist: "LOCAL FILE" };
    return { artist: match[1].trim(), title: match[2].trim() };
  }

  function syncSafe(bytes, offset) {
    return (
      ((bytes[offset] & 0x7f) << 21) |
      ((bytes[offset + 1] & 0x7f) << 14) |
      ((bytes[offset + 2] & 0x7f) << 7) |
      (bytes[offset + 3] & 0x7f)
    ) >>> 0;
  }

  function readUint24BE(bytes, offset) {
    return (bytes[offset] << 16) | (bytes[offset + 1] << 8) | bytes[offset + 2];
  }

  function readUint32BE(bytes, offset) {
    return (
      (bytes[offset] * 0x1000000) +
      (bytes[offset + 1] << 16) +
      (bytes[offset + 2] << 8) +
      bytes[offset + 3]
    ) >>> 0;
  }

  function readUint32LE(bytes, offset) {
    return (
      bytes[offset] +
      (bytes[offset + 1] << 8) +
      (bytes[offset + 2] << 16) +
      (bytes[offset + 3] * 0x1000000)
    ) >>> 0;
  }

  function decodeId3Text(payload) {
    if (!payload?.length) return "";
    const encoding = payload[0];
    let label = "windows-1252";
    let data = payload.slice(1);

    if (encoding === 1) {
      label = "utf-16le";
      if (data[0] === 0xfe && data[1] === 0xff) {
        label = "utf-16be";
        data = data.slice(2);
      } else if (data[0] === 0xff && data[1] === 0xfe) {
        data = data.slice(2);
      }
    } else if (encoding === 2) {
      label = "utf-16be";
    } else if (encoding === 3) {
      label = "utf-8";
    }

    try {
      return new TextDecoder(label)
        .decode(data)
        .replace(/\u0000[\s\S]*$/, "")
        .trim();
    } catch {
      return new TextDecoder()
        .decode(data)
        .replace(/\u0000[\s\S]*$/, "")
        .trim();
    }
  }

  function findNullTerminator(bytes, start, wide = false) {
    if (!wide) {
      for (let index = start; index < bytes.length; index += 1) {
        if (bytes[index] === 0) return index;
      }
      return bytes.length;
    }
    for (let index = start; index < bytes.length - 1; index += 2) {
      if (bytes[index] === 0 && bytes[index + 1] === 0) return index;
    }
    return bytes.length;
  }

  function parseApic(payload) {
    if (!payload || payload.length < 7) return null;
    const encoding = payload[0];
    let cursor = 1;
    const mimeEnd = findNullTerminator(payload, cursor, false);
    let mime = new TextDecoder("windows-1252")
      .decode(payload.slice(cursor, mimeEnd))
      .trim()
      .toLowerCase();
    cursor = Math.min(payload.length, mimeEnd + 1);
    if (cursor >= payload.length) return null;
    cursor += 1; // Picture type.
    const descriptionEnd = findNullTerminator(payload, cursor, encoding === 1 || encoding === 2);
    cursor = Math.min(payload.length, descriptionEnd + (encoding === 1 || encoding === 2 ? 2 : 1));
    const image = payload.slice(cursor);
    if (image.length < 16 || image.length > 6 * 1024 * 1024) return null;
    if (!/^image\//.test(mime)) {
      if (image[0] === 0xff && image[1] === 0xd8) mime = "image/jpeg";
      else if (image[0] === 0x89 && image[1] === 0x50) mime = "image/png";
      else return null;
    }
    return URL.createObjectURL(new Blob([image], { type: mime }));
  }

  function parsePic(payload) {
    if (!payload || payload.length < 6) return null;
    const format = new TextDecoder("windows-1252")
      .decode(payload.slice(1, 4))
      .trim()
      .toUpperCase();
    const mime = format === "PNG" ? "image/png" : format === "JPG" ? "image/jpeg" : "";
    if (!mime) return null;
    const mimeBytes = new TextEncoder().encode(mime);
    const adjusted = new Uint8Array(1 + mimeBytes.length + 1 + payload.length - 4);
    adjusted[0] = payload[0];
    adjusted.set(mimeBytes, 1);
    adjusted[1 + mimeBytes.length] = 0;
    adjusted.set(payload.slice(4), 2 + mimeBytes.length);
    return parseApic(adjusted);
  }

  async function parseId3(file, preview) {
    const version = preview[3];
    const flags = preview[5];
    const tagSize = syncSafe(preview, 6);
    const available = Math.min(file.size, tagSize + 10, 8 * 1024 * 1024);
    const bytes = new Uint8Array(await file.slice(0, available).arrayBuffer());
    const headerSize = version === 2 ? 6 : 10;
    const result = {};
    let position = 10;

    if (flags & 0x40 && version >= 3) {
      if (position + 4 > bytes.length) return result;
      const extendedSize = version === 4 ? syncSafe(bytes, position) : readUint32BE(bytes, position) + 4;
      position += Math.max(4, extendedSize);
    }

    while (position + headerSize <= bytes.length) {
      const idLength = version === 2 ? 3 : 4;
      const idBytes = bytes.slice(position, position + idLength);
      if (idBytes.every((byte) => byte === 0)) break;
      const id = new TextDecoder("windows-1252").decode(idBytes);
      const frameSize = version === 2
        ? readUint24BE(bytes, position + 3)
        : version === 4
          ? syncSafe(bytes, position + 4)
          : readUint32BE(bytes, position + 4);
      position += headerSize;
      if (!frameSize || position + frameSize > bytes.length) break;
      const payload = bytes.slice(position, position + frameSize);
      position += frameSize;

      if (TEXT_FRAME_KEYS[id]) {
        const value = decodeId3Text(payload);
        if (value && !result[TEXT_FRAME_KEYS[id]]) result[TEXT_FRAME_KEYS[id]] = value;
      } else if ((id === "APIC" || id === "PIC") && !result.coverUrl) {
        result.coverUrl = id === "APIC" ? parseApic(payload) : parsePic(payload);
      }
    }
    return result;
  }

  function parseVorbisComments(bytes, start, length) {
    const result = {};
    const end = Math.min(bytes.length, start + length);
    let position = start;
    if (position + 4 > end) return result;
    const vendorLength = readUint32LE(bytes, position);
    position += 4;
    if (vendorLength > length || position + vendorLength + 4 > end) return result;
    position += vendorLength;
    const count = readUint32LE(bytes, position);
    position += 4;
    const decoder = new TextDecoder("utf-8");

    for (let index = 0; index < count && position + 4 <= end; index += 1) {
      const itemLength = readUint32LE(bytes, position);
      position += 4;
      if (itemLength > end - position) break;
      const comment = decoder.decode(bytes.slice(position, position + itemLength));
      position += itemLength;
      const separator = comment.indexOf("=");
      if (separator < 1) continue;
      const key = comment.slice(0, separator).toUpperCase();
      const value = comment.slice(separator + 1).trim();
      if (key === "TITLE") result.title = value;
      if (key === "ARTIST") result.artist = value;
      if (key === "ALBUM") result.album = value;
      if (key === "DATE") result.year = value;
    }
    return result;
  }

  async function parseFlac(file, preview) {
    const bytes = new Uint8Array(
      await file.slice(0, Math.min(file.size, 4 * 1024 * 1024)).arrayBuffer()
    );
    let position = 4;
    let last = false;
    while (!last && position + 4 <= bytes.length) {
      const header = bytes[position];
      last = Boolean(header & 0x80);
      const type = header & 0x7f;
      const length = readUint24BE(bytes, position + 1);
      position += 4;
      if (position + length > bytes.length) break;
      if (type === 4) return parseVorbisComments(bytes, position, length);
      position += length;
    }
    return {};
  }

  async function readEmbeddedMetadata(file) {
    try {
      const preview = new Uint8Array(await file.slice(0, 12).arrayBuffer());
      if (preview[0] === 0x49 && preview[1] === 0x44 && preview[2] === 0x33) {
        return await parseId3(file, preview);
      }
      if (
        preview[0] === 0x66 && preview[1] === 0x4c &&
        preview[2] === 0x61 && preview[3] === 0x43
      ) {
        return await parseFlac(file, preview);
      }
    } catch {
      // Metadata is optional; file name remains the reliable fallback.
    }
    return {};
  }

  async function createTrack(file) {
    const fallback = parseFileName(file.name);
    const tags = await readEmbeddedMetadata(file);
    return {
      id: crypto.randomUUID?.() || `${Date.now()}-${Math.random().toString(16).slice(2)}`,
      file,
      name: file.name,
      url: URL.createObjectURL(file),
      title: tags.title || fallback.title,
      artist: tags.artist || fallback.artist,
      album: tags.album || "",
      year: tags.year || "",
      coverUrl: tags.coverUrl || "",
      duration: 0,
      size: file.size,
      extension: fileExtension(file.name).toUpperCase() || "AUDIO",
      key: `${file.name}|${file.size}|${file.lastModified}`
    };
  }

  async function readFileEntry(entry) {
    return new Promise((resolve) => {
      entry.file(resolve, () => resolve(null));
    });
  }

  async function readDirectoryEntries(entry) {
    const reader = entry.createReader();
    const entries = [];
    let batch = [];
    do {
      batch = await new Promise((resolve) => {
        reader.readEntries(resolve, () => resolve([]));
      });
      entries.push(...batch);
    } while (batch.length);
    return entries;
  }

  async function collectDroppedFiles(dataTransfer) {
    const entries = [];
    for (const item of [...dataTransfer.items]) {
      if (item.kind !== "file") continue;
      const entry = item.webkitGetAsEntry?.();
      if (entry) entries.push(entry);
    }

    if (!entries.length) return [...dataTransfer.files];
    const collected = [];
    const visit = async (entry) => {
      if (entry.isFile) {
        const file = await readFileEntry(entry);
        if (file) collected.push(file);
      } else if (entry.isDirectory) {
        const children = await readDirectoryEntries(entry);
        for (const child of children) await visit(child);
      }
    };
    for (const entry of entries) await visit(entry);
    return collected;
  }

  function showToast(message, isError = false, duration = 3200) {
    window.clearTimeout(state.toastTimer);
    els.toast.textContent = message;
    els.toast.classList.toggle("is-error", isError);
    els.toast.classList.add("is-visible");
    state.toastTimer = window.setTimeout(() => {
      els.toast.classList.remove("is-visible");
    }, duration);
  }

  async function mapWithConcurrency(items, limit, mapper) {
    const results = new Array(items.length);
    let nextIndex = 0;
    const worker = async () => {
      while (nextIndex < items.length) {
        const index = nextIndex;
        nextIndex += 1;
        results[index] = await mapper(items[index]);
      }
    };
    const workers = Array.from(
      { length: Math.min(limit, items.length) },
      () => worker()
    );
    await Promise.all(workers);
    return results;
  }

  async function addFiles(files, { autoplay = true } = {}) {
    const candidates = [...files].filter(isAudioFile);
    if (!candidates.length) {
      showToast("没有识别到可播放的本地音频文件。", true);
      return;
    }

    const existingKeys = new Set(state.tracks.map((track) => track.key));
    const uniqueFiles = [];
    for (const file of candidates) {
      const key = `${file.name}|${file.size}|${file.lastModified}`;
      if (!existingKeys.has(key)) {
        existingKeys.add(key);
        uniqueFiles.push(file);
      }
    }

    if (!uniqueFiles.length) {
      showToast("这些文件已经在当前曲库中。");
      return;
    }

    els.busState.textContent = "READING LOCAL TAGS";
    const created = await mapWithConcurrency(uniqueFiles, 4, createTrack);
    const firstIndex = state.tracks.length;
    state.tracks.push(...created);

    if (state.currentIndex < 0) {
      state.currentIndex = firstIndex;
      prepareTrack(firstIndex);
    }

    renderLibrary();
    els.busState.textContent = "READY / LOCAL INPUT";
    const skipped = candidates.length - uniqueFiles.length;
    showToast(
      `已载入 ${created.length} 个本地文件${skipped ? `，跳过 ${skipped} 个重复项` : ""}。`
    );

    if (autoplay && created.length) {
      await loadTrack(firstIndex, true);
    }
  }

  function filteredTracks() {
    const query = state.search.trim().toLocaleLowerCase();
    if (!query) return state.tracks.map((track, index) => ({ track, index }));
    return state.tracks
      .map((track, index) => ({ track, index }))
      .filter(({ track }) => {
        const haystack = `${track.title} ${track.artist} ${track.album} ${track.name}`.toLocaleLowerCase();
        return haystack.includes(query);
      });
  }

  function createSvgIcon(symbolId, className = "icon") {
    const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("class", className);
    svg.setAttribute("aria-hidden", "true");
    const use = document.createElementNS("http://www.w3.org/2000/svg", "use");
    use.setAttribute("href", symbolId);
    svg.append(use);
    return svg;
  }

  function renderLibrary() {
    const visible = filteredTracks();
    const fragment = document.createDocumentFragment();

    for (const { track, index } of visible) {
      const item = document.createElement("li");
      item.className = "queue-item";
      item.dataset.trackIndex = String(index);
      if (index === state.currentIndex) item.classList.add("is-current");

      const select = document.createElement("button");
      select.type = "button";
      select.className = "track-select";
      select.setAttribute("aria-label", `播放 ${track.title}`);
      select.dataset.action = "play";

      const number = document.createElement("span");
      number.className = "track-number";
      number.textContent = String(index + 1).padStart(2, "0");

      const copy = document.createElement("span");
      copy.className = "track-copy";
      const title = document.createElement("strong");
      title.textContent = track.title;
      const artist = document.createElement("small");
      artist.textContent = [track.artist, track.album].filter(Boolean).join(" / ") || track.name;
      copy.append(title, artist);

      const duration = document.createElement("span");
      duration.className = "track-duration";
      duration.textContent = track.duration ? formatTime(track.duration) : track.extension;

      const remove = document.createElement("button");
      remove.type = "button";
      remove.className = "track-remove";
      remove.title = "从当前曲库移除";
      remove.setAttribute("aria-label", `移除 ${track.title}`);
      remove.dataset.action = "remove";
      remove.append(createSvgIcon("#i-x"));

      select.append(number, copy, duration);
      item.append(select, remove);
      fragment.append(item);
    }

    els.queueList.replaceChildren(fragment);
    els.emptyState.hidden = state.tracks.length > 0;
    els.libraryCount.textContent =
      state.search.trim()
        ? `${String(visible.length).padStart(3, "0")} / ${String(state.tracks.length).padStart(3, "0")} ITEMS`
        : `${String(state.tracks.length).padStart(3, "0")} ITEMS`;
    els.currentIndexLabel.textContent = state.currentIndex >= 0
      ? `SELECTED ${String(state.currentIndex + 1).padStart(2, "0")}`
      : "NO SELECTION";
    els.clearButton.disabled = state.tracks.length === 0;

    if (state.tracks.length === 0 && state.currentIndex < 0) {
      els.currentIndexLabel.textContent = "NO SELECTION";
    }
  }

  function setCover(track) {
    if (track?.coverUrl) {
      els.coverImage.src = track.coverUrl;
      els.coverImage.alt = `${track.album || track.title} 封面`;
      els.albumCell.classList.add("has-cover");
    } else {
      els.coverImage.removeAttribute("src");
      els.coverImage.alt = "";
      els.albumCell.classList.remove("has-cover");
    }
  }

  function updateCurrentUI() {
    const track = state.tracks[state.currentIndex];
    if (!track) {
      els.trackTitle.textContent = "NO LOCAL FILE LOADED";
      els.trackArtist.textContent = "等待本地音频输入";
      els.playingKicker.textContent = "NOW PLAYING / CH 00";
      els.trackFormat.textContent = "—";
      els.elapsedTime.textContent = "00:00";
      els.durationTime.textContent = "00:00";
      els.playerStatus.textContent = "NO SIGNAL";
      els.signalLabel.textContent = "STANDBY";
      els.playingKicker.textContent = "NOW PLAYING / CH 00";
      els.seekTrack.style.setProperty("--seek", "0%");
      els.seekTrack.style.setProperty("--buffered", "0%");
      els.seekTrack.setAttribute("aria-valuenow", "0");
      els.seekTrack.setAttribute("aria-valuetext", "00:00 / 00:00");
      setCover(null);
      document.title = "SOUND MEMOIRS / Local Archive";
      setPlaybackUI(false);
      return;
    }

    els.trackTitle.textContent = track.title;
    els.trackArtist.textContent = [track.artist, track.album, track.year].filter(Boolean).join(" / ");
    els.playingKicker.textContent = `NOW PLAYING / CH ${String(state.currentIndex + 1).padStart(2, "0")}`;
    els.trackFormat.textContent = track.extension;
    els.durationTime.textContent = track.duration ? formatTime(track.duration) : "00:00";
    setCover(track);
    document.title = `${track.title} / SOUND MEMOIRS`;
  }

  function prepareTrack(index) {
    const track = state.tracks[index];
    if (!track) return;
    state.currentIndex = index;
    els.seekTrack.style.setProperty("--seek", "0%");
    els.seekTrack.style.setProperty("--buffered", "0%");
    els.elapsedTime.textContent = "00:00";
    els.durationTime.textContent = track.duration ? formatTime(track.duration) : "00:00";
    updateCurrentUI();
    renderLibrary();
  }

  function ensureAudioGraph() {
    if (state.audioContext) {
      if (state.audioContext.state === "suspended") {
        state.audioContext.resume().catch(() => {});
      }
      return;
    }
    if (state.audioGraphFailed) return;

    const AudioContextClass = window.AudioContext || window.webkitAudioContext;
    if (!AudioContextClass) {
      state.audioGraphFailed = true;
      return;
    }

    try {
      const context = new AudioContextClass();
      const source = context.createMediaElementSource(audio);
      const analyser = context.createAnalyser();
      analyser.fftSize = 2048;
      analyser.smoothingTimeConstant = 0.82;
      analyser.minDecibels = -92;
      analyser.maxDecibels = -12;
      source.connect(analyser);
      analyser.connect(context.destination);
      state.audioContext = context;
      state.sourceNode = source;
      state.analyser = analyser;
      state.frequencyData = new Uint8Array(analyser.frequencyBinCount);
      state.waveformData = new Uint8Array(analyser.fftSize);
    } catch {
      state.audioGraphFailed = true;
    }
  }

  function setPlaybackUI(isPlaying) {
    state.isPlaying = isPlaying;
    els.appFrame.classList.toggle("is-playing", isPlaying);
    els.playButton.setAttribute("aria-label", isPlaying ? "暂停" : "播放");
    els.playButton.title = isPlaying ? "暂停" : "播放";
    if (state.currentIndex >= 0) {
      els.signalLabel.textContent = isPlaying ? "SIGNAL LIVE" : "HOLD";
      els.playerStatus.textContent = isPlaying ? "LOCAL PLAY" : "PAUSED";
      els.busState.textContent = `${isPlaying ? "PLAY" : "HOLD"} / LOCAL INPUT`;
    } else {
      els.signalLabel.textContent = "STANDBY";
      els.playerStatus.textContent = "NO SIGNAL";
      els.busState.textContent = "READY / LOCAL INPUT";
    }
  }

  async function playCurrent() {
    if (!state.tracks.length) {
      els.fileInput.click();
      return;
    }
    if (state.currentIndex < 0) {
      prepareTrack(0);
    }
    if (!audio.src) {
      await loadTrack(state.currentIndex, true);
      return;
    }
    ensureAudioGraph();
    try {
      await audio.play();
    } catch (error) {
      setPlaybackUI(false);
      showToast(
        error?.name === "NotAllowedError"
          ? "浏览器阻止了自动播放，请再按一次播放。"
          : "当前浏览器无法播放这个音频格式。",
        true
      );
    }
  }

  async function loadTrack(index, shouldPlay = false) {
    const track = state.tracks[index];
    if (!track) return;
    audio.pause();
    state.currentIndex = index;
    audio.src = track.url;
    audio.load();
    updateCurrentUI();
    renderLibrary();
    if (shouldPlay) await playCurrent();
  }

  function visibleSequence() {
    const tracks = filteredTracks();
    return tracks.length ? tracks : state.tracks.map((track, index) => ({ track, index }));
  }

  async function moveTrack(direction, manual = true) {
    if (!state.tracks.length) return;
    if (state.shuffle) {
      if (state.tracks.length === 1) {
        await loadTrack(0, true);
        return;
      }
      let next = state.currentIndex;
      while (next === state.currentIndex) next = Math.floor(Math.random() * state.tracks.length);
      await loadTrack(next, true);
      return;
    }

    const sequence = visibleSequence();
    const visiblePosition = sequence.findIndex(({ index }) => index === state.currentIndex);
    const startPosition = visiblePosition >= 0 ? visiblePosition : 0;
    let nextPosition = startPosition + direction;
    if (nextPosition >= sequence.length) {
      if (!manual && state.repeat === "off") {
        audio.pause();
        setPlaybackUI(false);
        return;
      }
      nextPosition = 0;
    }
    if (nextPosition < 0) nextPosition = sequence.length - 1;
    await loadTrack(sequence[nextPosition].index, true);
  }

  async function previousTrack() {
    if (state.currentIndex < 0) return;
    if (audio.currentTime > 3) {
      audio.currentTime = 0;
      updateProgress();
      return;
    }
    await moveTrack(-1, true);
  }

  function cycleRepeat() {
    const current = REPEAT_MODES.indexOf(state.repeat);
    state.repeat = REPEAT_MODES[(current + 1) % REPEAT_MODES.length];
    writeStored(STORAGE_KEYS.repeat, state.repeat);
    const labels = { off: "OFF", all: "ALL", one: "ONE" };
    els.repeatLabel.textContent = labels[state.repeat];
    els.repeatButton.classList.toggle("is-active", state.repeat !== "off");
    els.repeatButton.title = `循环模式：${labels[state.repeat]}`;
    els.repeatButton.setAttribute("aria-label", `循环模式：${labels[state.repeat]}`);
  }

  function toggleShuffle() {
    state.shuffle = !state.shuffle;
    els.shuffleButton.classList.toggle("is-active", state.shuffle);
    els.shuffleButton.setAttribute("aria-pressed", String(state.shuffle));
    els.shuffleButton.title = state.shuffle ? "随机播放：开" : "随机播放：关";
    writeStored(STORAGE_KEYS.shuffle, state.shuffle);
    showToast(state.shuffle ? "随机播放已开启。" : "随机播放已关闭。");
  }

  function updateVolumeUI() {
    const percentage = Math.round(state.volume * 100);
    els.volumeTrack.style.setProperty("--volume", `${percentage}%`);
    els.volumeFill.style.width = `${percentage}%`;
    els.volumeThumb.style.left = `${percentage}%`;
    els.volumeValue.value = String(percentage);
    els.volumeValue.textContent = String(percentage);
    els.volumeTrack.setAttribute("aria-valuenow", String(percentage));
    els.volumeTrack.setAttribute("aria-valuetext", `${percentage}%`);
    els.appFrame.classList.toggle("is-muted", state.isMuted);
    els.muteButton.setAttribute("aria-label", state.isMuted ? "取消静音" : "静音");
    els.muteButton.title = state.isMuted ? "取消静音" : "静音";
  }

  function setVolumeFromRatio(ratio) {
    state.volume = clamp(ratio, 0, 1);
    audio.volume = state.volume;
    if (state.volume > 0) state.isMuted = false;
    audio.muted = state.isMuted;
    writeStored(STORAGE_KEYS.volume, Math.round(state.volume * 100));
    updateVolumeUI();
  }

  function updateProgress() {
    if (state.isSeeking) return;
    const duration = Number.isFinite(audio.duration) ? audio.duration : 0;
    const position = Number.isFinite(audio.currentTime) ? audio.currentTime : 0;
    const ratio = duration > 0 ? clamp(position / duration, 0, 1) : 0;
    els.seekTrack.style.setProperty("--seek", `${ratio * 100}%`);
    els.elapsedTime.textContent = formatTime(position);
    els.durationTime.textContent = formatTime(duration);
    els.seekTrack.setAttribute("aria-valuenow", String(Math.round(ratio * 100)));
    els.seekTrack.setAttribute("aria-valuetext", `${formatTime(position)} / ${formatTime(duration)}`);
  }

  function seekToRatio(ratio) {
    const duration = Number.isFinite(audio.duration) ? audio.duration : 0;
    if (!duration) return;
    const safeRatio = clamp(ratio, 0, 1);
    const position = duration * safeRatio;
    audio.currentTime = position;
    els.seekTrack.style.setProperty("--seek", `${safeRatio * 100}%`);
    els.elapsedTime.textContent = formatTime(position);
    els.durationTime.textContent = formatTime(duration);
    els.seekTrack.setAttribute("aria-valuenow", String(Math.round(safeRatio * 100)));
    els.seekTrack.setAttribute("aria-valuetext", `${formatTime(position)} / ${formatTime(duration)}`);
  }

  function bindPointerRange(element, getRatio, setRatio, onStart, onEnd) {
    let activePointer = null;
    const calculate = (event) => {
      const rect = element.getBoundingClientRect();
      return clamp((event.clientX - rect.left) / rect.width, 0, 1);
    };
    const finish = (event) => {
      if (activePointer !== event.pointerId) return;
      element.classList.remove("is-dragging");
      activePointer = null;
      onEnd?.();
      if (element.hasPointerCapture?.(event.pointerId)) {
        element.releasePointerCapture(event.pointerId);
      }
    };

    element.addEventListener("pointerdown", (event) => {
      if (event.button !== 0) return;
      activePointer = event.pointerId;
      try {
        element.setPointerCapture?.(event.pointerId);
      } catch {
        // Synthetic test events and older browsers may not have an active pointer.
      }
      element.classList.add("is-dragging");
      onStart?.();
      setRatio(calculate(event));
      event.preventDefault();
    });

    element.addEventListener("pointermove", (event) => {
      if (event.pointerId !== activePointer) return;
      setRatio(calculate(event));
      event.preventDefault();
    });

    element.addEventListener("pointerup", finish);
    element.addEventListener("pointercancel", finish);

    element.addEventListener("keydown", (event) => {
      const current = getRatio();
      let next = current;
      if (event.key === "ArrowLeft" || event.key === "ArrowDown") next -= 0.05;
      else if (event.key === "ArrowRight" || event.key === "ArrowUp") next += 0.05;
      else if (event.key === "Home") next = 0;
      else if (event.key === "End") next = 1;
      else return;
      setRatio(clamp(next, 0, 1));
      event.preventDefault();
    });
  }

  function removeTrack(index) {
    const track = state.tracks[index];
    if (!track) return;
    const wasCurrent = index === state.currentIndex;
    if (wasCurrent) audio.pause();

    state.tracks.splice(index, 1);
    URL.revokeObjectURL(track.url);
    if (track.coverUrl) URL.revokeObjectURL(track.coverUrl);

    if (!state.tracks.length) {
      audio.removeAttribute("src");
      audio.load();
      state.currentIndex = -1;
      updateCurrentUI();
    } else if (wasCurrent) {
      state.currentIndex = Math.min(index, state.tracks.length - 1);
      loadTrack(state.currentIndex, false);
    } else if (index < state.currentIndex) {
      state.currentIndex -= 1;
      renderLibrary();
    }

    if (state.tracks.length) renderLibrary();
    showToast("已从当前曲库移除；磁盘上的原文件未改动。");
  }

  function clearLibrary() {
    if (!state.tracks.length) return;
    const confirmed = window.confirm("从当前列表移除全部曲目？本地文件不会被删除。");
    if (!confirmed) return;
    audio.pause();
    audio.removeAttribute("src");
    audio.load();
    for (const track of state.tracks) {
      URL.revokeObjectURL(track.url);
      if (track.coverUrl) URL.revokeObjectURL(track.coverUrl);
    }
    state.tracks = [];
    state.currentIndex = -1;
    state.search = "";
    els.trackSearch.value = "";
    updateCurrentUI();
    renderLibrary();
    showToast("当前曲库已清空。");
  }

  function setTheme(theme, persist = true) {
    if (!THEME_COPY[theme]) return;
    state.theme = theme;
    document.body.dataset.theme = theme;
    const copy = THEME_COPY[theme];
    for (const node of $$("[data-copy]")) {
      const key = node.dataset.copy.replace(/-([a-z])/g, (_, letter) => letter.toUpperCase());
      if (copy[key]) node.textContent = copy[key];
    }
    for (const button of els.themeButtons) {
      const isCurrent = button.dataset.themeChoice === theme;
      button.classList.toggle("is-active", isCurrent);
      button.setAttribute("aria-pressed", String(isCurrent));
    }
    const footLabel = $("#theme-foot-label");
    if (footLabel) footLabel.textContent = copy.themeFootLabel;
    const themeMeta = $('meta[name="theme-color"]');
    if (themeMeta) themeMeta.content = copy.themeColor;
    if (persist) writeStored(STORAGE_KEYS.theme, theme);
  }

  function updateClock() {
    const now = new Date();
    const parts = new Intl.DateTimeFormat("zh-CN", {
      year: "numeric", month: "2-digit", day: "2-digit"
    }).formatToParts(now);
    const value = Object.fromEntries(parts.map((part) => [part.type, part.value]));
    const text = `${value.year}.${value.month}.${value.day}`;
    els.sessionClock.textContent = text;
    els.sessionClock.dateTime = now.toISOString().slice(0, 10);
  }

  function initializeVisualizer() {
    const canvas = els.visualizer;
    const context = canvas.getContext("2d", { alpha: false });
    if (!context) return;
    const barCount = 43;
    let width = 0;
    let height = 0;
    let dpr = 1;

    const resize = () => {
      const rect = canvas.getBoundingClientRect();
      dpr = Math.min(window.devicePixelRatio || 1, 2);
      width = Math.max(1, Math.round(rect.width));
      height = Math.max(1, Math.round(rect.height));
      const pixelWidth = Math.round(width * dpr);
      const pixelHeight = Math.round(height * dpr);
      if (canvas.width !== pixelWidth || canvas.height !== pixelHeight) {
        canvas.width = pixelWidth;
        canvas.height = pixelHeight;
      }
    };

    const colors = () => {
      const style = getComputedStyle(document.body);
      return {
        screen: style.getPropertyValue("--screen").trim(),
        accent: style.getPropertyValue("--accent").trim(),
        accentSoft: style.getPropertyValue("--accent-soft").trim(),
        amber: style.getPropertyValue("--amber").trim(),
        blue: style.getPropertyValue("--blue").trim(),
        frame: style.getPropertyValue("--frame").trim(),
        line: style.getPropertyValue("--line").trim(),
        paper: document.body.dataset.theme === "paper"
      };
    };

    const drawIdleLevel = (index, time) => {
      const a = Math.sin(time * 0.00038 + index * 0.71);
      const b = Math.sin(time * 0.00017 - index * 0.29);
      const envelope = Math.pow(1 - index / barCount, 1.15);
      return 0.018 + Math.max(0, a * b) * 0.055 * envelope;
    };

    const draw = (time) => {
      resize();
      context.setTransform(dpr, 0, 0, dpr, 0, 0);
      context.clearRect(0, 0, width, height);

      const palette = colors();
      context.fillStyle = palette.screen;
      context.fillRect(0, 0, width, height);

      const leftPad = 10;
      const rightPad = 10;
      const plotWidth = Math.max(1, width - leftPad - rightPad);
      const waveformBottom = Math.max(72, height * 0.285);
      const barsTop = waveformBottom + (palette.paper ? 40 : 27);
      const barsBottom = height - 35;
      const maxBarHeight = Math.max(18, barsBottom - barsTop);
      const step = plotWidth / barCount;
      const barWidth = Math.max(2, Math.min(9, step * (palette.paper ? 0.34 : 0.62)));
      const isLive = Boolean(state.analyser && state.audioContext?.state === "running" && !audio.paused);

      if (isLive) {
        state.analyser.getByteFrequencyData(state.frequencyData);
        state.analyser.getByteTimeDomainData(state.waveformData);
      }

      context.save();
      context.beginPath();
      context.rect(leftPad, 15, plotWidth, waveformBottom - 15);
      context.clip();
      context.strokeStyle = palette.blue;
      context.globalAlpha = palette.paper ? 0.64 : 0.74;
      context.lineWidth = palette.paper ? 1 : 1.2;
      context.beginPath();

      const waveformSamples = isLive ? state.waveformData.length : 180;
      for (let index = 0; index < waveformSamples; index += 1) {
        const x = leftPad + (index / (waveformSamples - 1)) * plotWidth;
        let normalized;
        if (isLive) {
          normalized = (state.waveformData[index] - 128) / 128;
        } else {
          normalized =
            Math.sin(time * 0.00125 + index * 0.18) * 0.025 +
            Math.sin(time * 0.00045 - index * 0.067) * 0.018;
        }
        const amplitude = (waveformBottom - 28) * 0.46;
        const y = (waveformBottom + 15) / 2 - normalized * amplitude;
        if (index === 0) context.moveTo(x, y);
        else context.lineTo(x, y);
      }
      context.stroke();
      context.restore();

      context.strokeStyle = palette.line;
      context.globalAlpha = 0.65;
      context.lineWidth = 1;
      context.beginPath();
      context.moveTo(leftPad, waveformBottom);
      context.lineTo(width - rightPad, waveformBottom);
      context.stroke();
      context.globalAlpha = 1;

      let leftEnergy = 0;
      let rightEnergy = 0;
      const peakDecay = palette.paper ? 0.009 : 0.012;

      for (let index = 0; index < barCount; index += 1) {
        let level;
        if (isLive) {
          const normalized = index / (barCount - 1);
          const frequencyLength = state.frequencyData.length;
          const startBin = Math.floor(Math.pow(normalized, 1.78) * frequencyLength * 0.72);
          const endBin = Math.max(startBin + 1, Math.floor(Math.pow((index + 1) / barCount, 1.78) * frequencyLength * 0.72));
          let sum = 0;
          for (let bin = startBin; bin < Math.min(endBin, frequencyLength); bin += 1) {
            sum += state.frequencyData[bin];
          }
          const average = sum / Math.max(1, Math.min(endBin, frequencyLength) - startBin);
          level = clamp((average - 24) / 205, 0, 1);
          if (index < barCount / 2) leftEnergy += level;
          else rightEnergy += level;
        } else {
          level = drawIdleLevel(index, time);
        }

        const peak = Math.max(level, (state.peaks[index] || 0) - peakDecay);
        state.peaks[index] = Math.max(level, peak - peakDecay);
        const x = leftPad + index * step + (step - barWidth) / 2;
        const segments = Math.max(1, Math.floor((level * maxBarHeight) / 4));

        context.fillStyle = palette.line;
        context.globalAlpha = 0.17;
        context.fillRect(x, barsTop, barWidth, maxBarHeight);
        context.globalAlpha = 1;

        for (let segment = 0; segment < segments; segment += 1) {
          const y = barsBottom - segment * 4;
          context.fillStyle = segment > segments * 0.82 ? palette.amber : palette.accent;
          context.fillRect(x, y - 2, barWidth, 2);
        }

        const peakY = barsBottom - Math.floor((state.peaks[index] || 0) * maxBarHeight);
        context.fillStyle = palette.accentSoft;
        context.globalAlpha = isLive ? 0.68 : 0.28;
        context.fillRect(x, clamp(peakY - 3, barsTop, barsBottom - 2), barWidth, 2);
        context.globalAlpha = 1;
      }

      const meterScale = isLive ? 1 : 0.15;
      const leftLevel = Math.round(clamp(leftEnergy / (barCount / 2) * meterScale * 1.4, 0.03, 1) * 100);
      const rightLevel = Math.round(clamp(rightEnergy / (barCount / 2) * meterScale * 1.4, 0.03, 1) * 100);
      els.meterLeft.style.setProperty("--meter", `${leftLevel}%`);
      els.meterRight.style.setProperty("--meter", `${rightLevel}%`);

      context.fillStyle = palette.frame;
      context.globalAlpha = 0.82;
      context.fillRect(leftPad, barsBottom + 3, plotWidth, 1);
      context.globalAlpha = 1;

      requestAnimationFrame(draw);
    };

    requestAnimationFrame(draw);
    new ResizeObserver(resize).observe(canvas);
  }

  function initializeEvents() {
    const openPicker = () => els.fileInput.click();
    els.addFilesButton.addEventListener("click", openPicker);
    els.emptyAddButton.addEventListener("click", openPicker);

    els.fileInput.addEventListener("change", async () => {
      const files = [...(els.fileInput.files || [])];
      els.fileInput.value = "";
      if (files.length) await addFiles(files, { autoplay: true });
    });

    els.playButton.addEventListener("click", () => {
      if (!state.tracks.length) {
        openPicker();
      } else if (audio.paused) {
        playCurrent();
      } else {
        audio.pause();
      }
    });

    els.previousButton.addEventListener("click", previousTrack);
    els.nextButton.addEventListener("click", () => moveTrack(1, true));
    els.repeatButton.addEventListener("click", cycleRepeat);
    els.shuffleButton.addEventListener("click", toggleShuffle);
    els.clearButton.addEventListener("click", clearLibrary);

    els.muteButton.addEventListener("click", () => {
      state.isMuted = !state.isMuted;
      audio.muted = state.isMuted;
      updateVolumeUI();
    });

    els.queueList.addEventListener("click", (event) => {
      const item = event.target.closest(".queue-item");
      if (!item) return;
      const index = Number(item.dataset.trackIndex);
      const action = event.target.closest("[data-action]")?.dataset.action;
      if (action === "remove") removeTrack(index);
      else if (action === "play") loadTrack(index, true);
    });

    els.trackSearch.addEventListener("input", () => {
      state.search = els.trackSearch.value;
      renderLibrary();
    });

    for (const button of els.themeButtons) {
      button.addEventListener("click", () => setTheme(button.dataset.themeChoice));
    }

    bindPointerRange(
      els.seekTrack,
      () => {
        const duration = Number.isFinite(audio.duration) ? audio.duration : 0;
        return duration ? audio.currentTime / duration : 0;
      },
      seekToRatio,
      () => {
        state.isSeeking = true;
      },
      () => {
        state.isSeeking = false;
        updateProgress();
      }
    );

    bindPointerRange(
      els.volumeTrack,
      () => state.volume,
      setVolumeFromRatio
    );

    audio.addEventListener("play", () => setPlaybackUI(true));
    audio.addEventListener("pause", () => setPlaybackUI(false));
    audio.addEventListener("timeupdate", updateProgress);
    audio.addEventListener("durationchange", () => {
      const track = state.tracks[state.currentIndex];
      if (track && Number.isFinite(audio.duration) && audio.duration > 0) {
        track.duration = audio.duration;
      }
      updateCurrentUI();
      updateProgress();
      renderLibrary();
    });
    audio.addEventListener("progress", () => {
      if (!audio.buffered.length || !Number.isFinite(audio.duration) || !audio.duration) return;
      const end = audio.buffered.end(audio.buffered.length - 1);
      els.seekTrack.style.setProperty("--buffered", `${clamp(end / audio.duration, 0, 1) * 100}%`);
    });
    audio.addEventListener("ended", () => {
      if (state.repeat === "one") {
        audio.currentTime = 0;
        playCurrent();
      } else {
        moveTrack(1, false);
      }
    });
    audio.addEventListener("error", () => {
      if (!audio.src) return;
      setPlaybackUI(false);
      els.signalLabel.textContent = "READ ERROR";
      els.playerStatus.textContent = "FORMAT ERROR";
      showToast("这个文件无法在当前浏览器中解码，请尝试 MP3、OGG 或 WAV。", true, 5000);
    });

    document.addEventListener("keydown", (event) => {
      const target = event.target;
      const isTyping =
        target instanceof HTMLInputElement ||
        target instanceof HTMLTextAreaElement ||
        target?.isContentEditable;
      if (event.key === "/" && !isTyping) {
        event.preventDefault();
        els.trackSearch.focus();
        return;
      }
      if (event.key.toLocaleLowerCase() === "t" && !isTyping && !event.ctrlKey && !event.metaKey) {
        setTheme(state.theme === "original" ? "paper" : "original");
        return;
      }
      if (event.code === "Space" && !isTyping && !target?.closest('[role="slider"]')) {
        event.preventDefault();
        if (audio.paused) playCurrent();
        else audio.pause();
        return;
      }
      if (event.altKey && event.key === "ArrowRight") {
        event.preventDefault();
        moveTrack(1, true);
      } else if (event.altKey && event.key === "ArrowLeft") {
        event.preventDefault();
        previousTrack();
      }
    });

    document.addEventListener("dragenter", (event) => {
      if (!event.dataTransfer?.types?.includes("Files")) return;
      event.preventDefault();
      state.dragDepth += 1;
      els.dropOverlay.classList.add("is-visible");
    });

    document.addEventListener("dragover", (event) => {
      if (!event.dataTransfer?.types?.includes("Files")) return;
      event.preventDefault();
      event.dataTransfer.dropEffect = "copy";
    });

    document.addEventListener("dragleave", (event) => {
      if (!event.dataTransfer?.types?.includes("Files")) return;
      state.dragDepth = Math.max(0, state.dragDepth - 1);
      if (!state.dragDepth) els.dropOverlay.classList.remove("is-visible");
    });

    document.addEventListener("drop", async (event) => {
      if (!event.dataTransfer?.types?.includes("Files")) return;
      event.preventDefault();
      state.dragDepth = 0;
      els.dropOverlay.classList.remove("is-visible");
      const files = await collectDroppedFiles(event.dataTransfer);
      await addFiles(files, { autoplay: true });
    });
  }

  function initialize() {
    setTheme(state.theme, false);
    setVolumeFromRatio(state.volume);
    updateVolumeUI();
    const labels = { off: "OFF", all: "ALL", one: "ONE" };
    els.repeatLabel.textContent = labels[state.repeat];
    els.repeatButton.classList.toggle("is-active", state.repeat !== "off");
    els.repeatButton.title = `循环模式：${labels[state.repeat]}`;
    els.shuffleButton.classList.toggle("is-active", state.shuffle);
    els.shuffleButton.setAttribute("aria-pressed", String(state.shuffle));
    updateCurrentUI();
    renderLibrary();
    updateClock();
    window.setInterval(updateClock, 60_000);
    initializeVisualizer();
    initializeEvents();
  }

  initialize();
})();
