"use strict";

/*
 * Vanilla, no framework and no build step, in the same shape as the Cix
 * dashboard: one cache object every renderer reads from, DOM API
 * rendering rather than innerHTML, and a single fetch wrapper that
 * blinks the activity LEDs.
 */

const POLL_INTERVAL_MS = 2000;
const PAGE_SIZE = 25;

const cache = {
	status: null,
	artifacts: [],
	importing: false
};

let query = "";
let page = 0;
let logSeq = 0;
let browsing = false;
/* "" means every architecture; otherwise the one being shown. */
let archFilter = "";
/* What the chip strip was last built from, so it is not rebuilt blind. */
let archChipsKey = null;

const el = (id) => document.getElementById(id);
const ledTx = el("led-tx");
const ledRx = el("led-rx");

function ledBlink(led) {
	led.classList.add("lit");
	setTimeout(() => led.classList.remove("lit"), 120);
}

function setText(id, value) {
	el(id).textContent = value;
}

/* ---------- formatting ---------- */

function humanBytes(n) {
	const units = ["B", "KB", "MB", "GB", "TB"];
	let v = Number(n) || 0;
	let i = 0;

	while (v >= 1024 && i < units.length - 1) {
		v /= 1024;
		i++;
	}
	return (i === 0 ? v : v.toFixed(v < 10 ? 2 : 1)) + " " + units[i];
}

function humanDuration(seconds) {
	const s = Number(seconds) || 0;
	const d = Math.floor(s / 86400);
	const h = Math.floor((s % 86400) / 3600);
	const m = Math.floor((s % 3600) / 60);

	if (d > 0)
		return d + "d " + h + "h";
	if (h > 0)
		return h + "h " + m + "m";
	if (m > 0)
		return m + "m";
	return s + "s";
}

/* ---------- transport ---------- */

function token() {
	return el("token").value.trim();
}

async function apiRequest(method, path, opts) {
	const init = { method: method, headers: {} };
	const useToken = opts && opts.auth ? token() : "";

	ledBlink(ledTx);
	if (useToken)
		init.headers["Authorization"] = "Bearer " + useToken;

	const res = await fetch(path, init);

	ledBlink(ledRx);

	let json = null;
	if (res.status !== 204) {
		try {
			json = await res.json();
		} catch (e) {
			json = null;
		}
	}
	if (!res.ok)
		throw new Error(json && json.error ? json.error
		                                   : "request failed (HTTP " + res.status + ")");
	return json;
}

/* ---------- log dock ---------- */

function logLine(text, kind) {
	const log = el("log");
	const line = document.createElement("div");

	if (kind)
		line.className = kind;
	line.textContent = text;
	log.insertBefore(line, log.firstChild);
	while (log.childNodes.length > 400)
		log.removeChild(log.lastChild);
}

/*
 * The server's own activity ring, not this page's view of it. Asks for
 * entries after the last sequence number it saw, so a poll costs one
 * small response instead of the whole ring.
 */
async function refreshLog() {
	const data = await apiRequest("GET", "/api/v1/log?after=" + logSeq);
	const entries = data.entries || [];

	for (const e of entries) {
		const when = new Date(e.time_ms).toTimeString().substring(0, 8);

		logLine(when + "  " + e.text, e.level === "warn" ? "warn" : "");
	}
	logSeq = data.seq;
	/* Collapsed is the default, so say what is happening behind it. */
	setText("log-summary", entries.length > 0 ? entries[entries.length - 1].text : "");
}

/* ---------- results ---------- */

/* The URL is a FORMAT's name, at the root of base_url. An identity is
 * not fetchable -- what you download is one encoding of it. */
function formatUrl(f) {
	return "/" + f.name;
}

/* The encodings of an identity, in the order the server sent them. */
function formatsOf(a) {
	return a.formats || [];
}

/*
 * The encoding a row links to when the format column is not showing.
 * First rather than chosen: the server sorts formats by name, so this
 * is stable, and while only one encoding exists it is the only one.
 */
function primaryFormat(a) {
	return formatsOf(a)[0];
}

/*
 * Digests are matched by PREFIX, never as a substring.
 *
 * Substring matching them looks reasonable and is useless in practice:
 * every two-character query is a substring of almost every sha256, so
 * searching "bc" once returned 33 artifacts including jumpbox and
 * openssl. A prefix is also how anyone actually refers to a hash, the
 * way git and docker short ids work.
 */
function matches(a) {
	const needle = query.toLowerCase();

	/*
	 * Ahead of the text search and never short-circuited past it: the
	 * chip narrows whatever the query found, rather than competing
	 * with it.
	 */
	if (archFilter !== "" && (a.arch || "") !== archFilter)
		return false;
	if (!needle)
		return true;
	if (a.artifact.toLowerCase().indexOf(needle) >= 0)
		return true;
	if (a.version !== "" && a.version.toLowerCase().indexOf(needle) >= 0)
		return true;
	/*
	 * Also the version as it is written in a name, so searching
	 * "5.2.37-2" finds it even though version and release are
	 * separate columns here.
	 */
	if (a.version !== "" && (a.version + "-" + a.release).indexOf(needle) >= 0)
		return true;
	/*
	 * Every encoding, not just the first: searching "cixpkg" has to
	 * find the artifact published as one, and a digest belongs to a
	 * format rather than to the identity above it.
	 */
	for (const f of formatsOf(a)) {
		if (f.format.toLowerCase().indexOf(needle) >= 0)
			return true;
		if (f.sha256.indexOf(needle) === 0)
			return true;
	}
	return false;
}

function copyDigest(digest) {
	if (navigator.clipboard)
		navigator.clipboard.writeText(digest).then(
			() => logLine("copied " + digest),
			() => logLine("could not copy to clipboard", "err"));
}

/*
 * A cell holding one line per encoding.
 *
 * Every stacked column emits its lines in the same order, so format,
 * signature, size and digest read across as rows within the row. A
 * single-encoding artifact gets exactly one line and looks like it
 * always did.
 */
function stackedCell(a, className, build) {
	const cell = document.createElement("td");

	cell.className = className;
	for (const f of formatsOf(a)) {
		const line = document.createElement("div");

		line.className = "stack-line";
		build(line, f);
		cell.appendChild(line);
	}
	return cell;
}

function resultRow(a) {
	const row = document.createElement("tr");
	const primary = primaryFormat(a);

	/*
	 * The name is the link, so the URL is not repeated in a column of
	 * its own. Hovering shows the full filename, which is what the
	 * store is actually keyed by -- every one of them when the
	 * artifact exists in more than one encoding.
	 */
	const nameCell = document.createElement("td");
	const link = document.createElement("a");
	link.className = "mono";
	link.href = primary ? formatUrl(primary) : "#";
	link.textContent = a.artifact;
	link.title = formatsOf(a).map((f) => f.name).join("\n");
	nameCell.appendChild(link);
	row.appendChild(nameCell);

	const versionCell = document.createElement("td");
	versionCell.className = "mono num";
	versionCell.textContent = a.version || "-";
	row.appendChild(versionCell);

	/*
	 * Its own column, not folded into the version: the version is
	 * upstream's and the release is ours. 5.2.37 is what the bash
	 * authors shipped; -2 is what we did to it.
	 */
	const relCell = document.createElement("td");

	relCell.className = "mono num rel";
	relCell.textContent = a.release;
	row.appendChild(relCell);

	/*
	 * Each encoding links to its own URL. That is the point of the
	 * column: with two of them the name above can only point at one,
	 * and the other would be unreachable from the page.
	 */
	row.appendChild(stackedCell(a, "mono rel format-col", (line, f) => {
		const a2 = document.createElement("a");

		a2.href = formatUrl(f);
		a2.textContent = f.format;
		a2.title = f.name;
		line.appendChild(a2);
	}));

	const archCell = document.createElement("td");

	archCell.className = "mono num rel arch-col";
	archCell.textContent = a.arch || "-";
	row.appendChild(archCell);

	/*
	 * Only what carries one says anything, so a package shows blank
	 * rather than being described as unsigned -- signing is not a
	 * thing it does. Per encoding, because each has its own detached
	 * signature and one can be signed while the other is not.
	 */
	row.appendChild(stackedCell(a, "num rel signed-col", (line, f) => {
		line.textContent = f.signed === undefined ? "" : (f.signed ? "yes" : "no");
		if (f.signed === false)
			line.style.color = "var(--error)";
	}));

	row.appendChild(stackedCell(a, "num", (line, f) => {
		line.textContent = f.bytes < 0 ? "dangling" : humanBytes(f.bytes);
	}));

	row.appendChild(stackedCell(a, "", (line, f) => {
		const digest = document.createElement("span");

		digest.className = "mono digest";
		digest.textContent = f.sha256.substring(0, 16) + "\u2026";
		digest.title = f.sha256 + " (click to copy)";
		digest.addEventListener("click", () => copyDigest(f.sha256));
		line.appendChild(digest);
	}));

	/*
	 * When this artifact was last published here, not when the bytes
	 * were built. One line even with two encodings: the row is what
	 * landed, and the per-format dates are minutes apart at most.
	 */
	const whenCell = document.createElement("td");

	whenCell.className = "muted num";
	if (a.modified > 0) {
		const d = new Date(a.modified * 1000);

		whenCell.textContent = d.toISOString().substring(0, 10);
		whenCell.title = d.toLocaleString();
	} else {
		whenCell.textContent = "-";
	}
	row.appendChild(whenCell);

	return row;
}

/*
 * Column sorting.
 *
 * The server already sends newest-published first, and that stays the
 * default here: on a search-first page the list answers "what landed",
 * and finding a known name is the search box's job. Sorting is for the
 * other times -- scanning everything for something specific, or finding
 * what is taking up the room.
 *
 * Which direction a column opens in depends on the column. Text wants
 * A-Z first; a size, a release or a date almost always wants the
 * largest or newest first, because that is the reason you clicked it.
 */
const SORT_DIR_FIRST = {
	artifact: 1,
	arch: 1,
	signed: 1,
	version: -1,
	release: -1,
	bytes: -1,
	modified: -1,
};

let sort = loadSort();

function loadSort() {
	try {
		const raw = localStorage.getItem("cixcache-sort");

		if (raw !== null) {
			const s = JSON.parse(raw);

			if (SORT_DIR_FIRST[s.key] !== undefined && (s.dir === 1 || s.dir === -1))
				return s;
		}
	} catch (e) {
		/* private mode, blocked storage, or something we did not write */
	}
	return { key: "modified", dir: -1 };
}

function saveSort() {
	try {
		localStorage.setItem("cixcache-sort", JSON.stringify(sort));
	} catch (e) {
		/* the sort still applies, it just will not survive a reload */
	}
}

function sortCmp(a, b) {
	const k = sort.key;
	let r;

	if (k === "bytes" || k === "release" || k === "modified") {
		r = a[k] - b[k];
	} else if (k === "version") {
		/*
		 * The server's ordering, not ours. version_rank is each
		 * artifact's position under store_version_cmp(), which knows
		 * that a prerelease precedes its release (v2.2.0-rc6 before
		 * v2.2.0) and that 2.1.10 follows 2.1.8 -- neither of which
		 * string collation gets right.
		 *
		 * Comparing a number here rather than reimplementing those
		 * rules is the point: two comparators would eventually
		 * disagree about which artifact is newer, and only one of
		 * them would be the one the server acts on.
		 */
		r = a.version_rank - b.version_rank;
	} else if (k === "arch") {
		r = (a.arch || "").localeCompare(b.arch || "");
	} else if (k === "signed") {
		/*
		 * undefined last: those are artifacts the question does not
		 * apply to. Over every encoding, worst first -- one unsigned
		 * format is what you want to see, whatever the others say.
		 */
		const rank = (x) => {
			const fs = formatsOf(x);

			if (fs.some((f) => f.signed === false))
				return 0;
			if (fs.some((f) => f.signed === true))
				return 1;
			return 2;
		};

		r = rank(a) - rank(b);
	} else {
		r = a.artifact.localeCompare(b.artifact, undefined, { numeric: true });
	}
	if (r !== 0)
		return r * sort.dir;
	/*
	 * Stems are unique, so this makes the order total. Without it,
	 * equal keys leave rows free to swap places on every poll.
	 */
	return a.stem < b.stem ? -1 : a.stem > b.stem ? 1 : 0;
}

function applySortIndicators() {
	const heads = document.querySelectorAll("th.sortable");
	let i;

	for (i = 0; i < heads.length; i++) {
		const on = heads[i].dataset.sort === sort.key;

		heads[i].setAttribute("aria-sort",
			on ? (sort.dir === 1 ? "ascending" : "descending") : "none");
	}
}

function setSort(key) {
	if (sort.key === key)
		sort.dir = -sort.dir;
	else
		sort = { key: key, dir: SORT_DIR_FIRST[key] };
	saveSort();
	/* A re-sorted list makes the page you were on meaningless. */
	page = 0;
	applySortIndicators();
	renderResults();
}

function renderResults() {
	const results = el("results");
	const body = el("rows");

	/*
	 * Ahead of the early return: the chips sit in the strip under the
	 * search box, which is on screen whether or not there is a list.
	 * Rendering them only alongside results would mean the one control
	 * that starts an architecture browse is missing until you have
	 * already started one.
	 */
	renderArchChips();

	const show = query !== "" || browsing || archFilter !== "";

	results.hidden = !show;
	/* Lets the layout stop vertically centring once there is a list. */
	el("main-view").classList.toggle("has-results", show);
	el("hint").hidden = show;
	el("q-clear").hidden = query === "";
	if (!show)
		return;

	const shown = cache.artifacts.filter(matches);

	shown.sort(sortCmp);

	/*
	 * The architecture column earns its place only when it tells rows
	 * apart. While the whole store is one architecture it is the same
	 * word 125 times, and while none is stamped it is empty -- either
	 * way it is a column of noise. It appears by itself the moment two
	 * artifacts disagree, which is exactly when reading a row without
	 * it would be a mistake.
	 */
	{
		const seen = {};
		let distinct = 0;
		let i;

		for (i = 0; i < shown.length; i++) {
			const v = shown[i].arch || "";

			if (seen[v] === undefined) {
				seen[v] = 1;
				distinct++;
			}
		}
		el("results").classList.toggle("show-arch", distinct > 1);

		/*
		 * Same rule again: the column appears only once something in
		 * view actually carries a signature. While the store holds
		 * only packages it is a column of blanks.
		 */
		let signable = 0;

		for (i = 0; i < shown.length; i++) {
			if (formatsOf(shown[i]).some((f) => f.signed !== undefined))
				signable++;
		}
		el("results").classList.toggle("show-signed", signable > 0);

		/*
		 * And again for the format. While every artifact is a .tar.gz
		 * the column repeats one word down the page; it earns its
		 * place the moment an artifact exists in two encodings, which
		 * is exactly when a row is ambiguous without it.
		 */
		let multi = 0;

		for (i = 0; i < shown.length; i++) {
			if (formatsOf(shown[i]).length > 1)
				multi++;
		}
		el("results").classList.toggle("show-format", multi > 0);
	}

	const pages = Math.max(1, Math.ceil(shown.length / PAGE_SIZE));

	if (page >= pages)
		page = pages - 1;

	body.textContent = "";
	if (shown.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 9;
		cell.className = "empty";
		/*
		 * Name the architecture too. Otherwise a filter left on reads
		 * as "that package is not here", which is a different and
		 * much more alarming statement.
		 */
		cell.textContent = query !== ""
			? "Nothing matches “" + query + "”" +
			  (archFilter !== "" ? " on " + archFilter : "") + "."
			: archFilter !== ""
				? "Nothing published for " + archFilter + "."
				: "Nothing published yet.";
		row.appendChild(cell);
		body.appendChild(row);
	}
	for (const a of shown.slice(page * PAGE_SIZE, (page + 1) * PAGE_SIZE))
		body.appendChild(resultRow(a));

	el("page-prev").disabled = page === 0;
	el("page-next").disabled = page >= pages - 1;
	setText("page-label", shown.length === 0
		? ""
		: (page * PAGE_SIZE + 1) + "–" + Math.min((page + 1) * PAGE_SIZE, shown.length) +
		  " of " + shown.length);
}

/* ---------- bars ---------- */

function renderStatus() {
	const s = cache.status;

	if (!s)
		return;
	/* Menu bar: what the registry holds. */
	/*
	 * Four zlibs and three greps: distinct packages and published
	 * files are different numbers. Both come from /api/v1/status,
	 * which counts FILES and is deliberately not grouped the way the
	 * listing is -- it is what the store holds, and two encodings of
	 * one artifact really are two files on disk.
	 */
	setText("m-unique", String(s.unique_packages));
	setText("m-packages", String(s.packages));
	/*
	 * Counted apart from packages, mirroring MANIFEST.json's two
	 * sections, and shown only when there are any: an ISO is not a
	 * package, so folding it into that number would make it mean two
	 * kinds of thing at once.
	 */
	const inst = s.installers || 0;

	el("m-installers-wrap").hidden = inst === 0;
	setText("m-installers", String(inst));
	/* The size is the whole store, both kinds and their signatures. */
	setText("m-bytes", humanBytes(s.package_bytes + (s.installer_bytes || 0)));
	/*
	 * Only shown when there are any, and coloured. These serve fine
	 * today and stop being safe the moment another machine's build
	 * shares one of their names -- so this is the only place anyone
	 * would find out before that happens.
	 */
	const un = el("m-unstamped");

	un.hidden = !(s.unstamped > 0);
	un.textContent = s.unstamped + " unstamped";

	/* Status bar: what the server is doing. */
	setText("s-requests", s.requests + " req");
	setText("s-served", humanBytes(s.served_bytes) + " served");
	setText("s-uptime", "up " + humanDuration(s.uptime_seconds));
	/*
	 * The build identity, which is a canonical artifact name -- the
	 * same grammar the store enforces on what it holds. A build from a
	 * modified tree is marked rather than folded into that name: it is
	 * not part of an identity, and it is worth seeing at a glance that
	 * what is serving is not the release it claims to be.
	 */
	{
		const b = s.build || {};
		const build = el("s-build");

		/*
		 * Every field as the SERVER split it. The identity is a
		 * canonical artifact name, and splitting it here would be a
		 * second implementation of a grammar there is one of -- the
		 * hyphen inside "cix-cache" being exactly what a naive split
		 * gets wrong.
		 */
		setText("s-build-artifact", b.artifact || "—");
		setText("s-build-version", b.version || "—");
		setText("s-build-release", "rel " + (b.release === undefined ? "—" : b.release));
		setText("s-build-arch", b.arch || "—");
		/*
		 * The whole name on hover: split for reading, but the
		 * unsplit form is the authoritative one and is what somebody
		 * would paste into a bug report.
		 */
		build.title = b.identity + " — built " + b.time +
			(b.dirty ? " from a modified working tree" : "");
		build.classList.toggle("dirty-build", b.dirty === true);
	}

	/*
	 * Misses alone with no hits at all is the signature of a registry
	 * nobody can reach, and is otherwise indistinguishable from an idle
	 * one -- so it is coloured, and nothing else here is.
	 */
	const hits = el("s-hits");
	hits.textContent = s.artifact_hits + " hit / " + s.artifact_misses + " miss";
	hits.style.color = (s.artifact_misses > 0 && s.artifact_hits === 0) ? "var(--warn)" : "";

	document.title = "cix-cache — " + s.unique_packages + " packages";
}

function renderReach(up) {
	const led = el("led-up");

	led.classList.toggle("up", up);
	led.classList.toggle("down", !up);
	setText("reach", up ? "reachable" : "unreachable");
}

function renderImport() {
	setText("import-state", cache.importing ? "import running…" : "");
}

/* ---------- polling ---------- */

async function poll() {
	if (document.hidden)
		return;
	try {
		cache.status = await apiRequest("GET", "/api/v1/status");
		renderStatus();

		const data = await apiRequest("GET", "/api/v1/artifacts");

		cache.artifacts = data.artifacts || [];
		renderResults();

		const imp = await apiRequest("GET", "/api/v1/import-status");

		cache.importing = !!imp.running;
		renderImport();

		await refreshLog();
		renderReach(true);
	} catch (e) {
		renderReach(false);
	}
}

/* ---------- actions ---------- */

async function runGc(dryRun) {
	try {
		await apiRequest(dryRun ? "GET" : "POST", "/api/v1/gc", { auth: !dryRun });
		await poll();
	} catch (e) {
		logLine("gc failed: " + e.message, "err");
		openLog();
	}
}

async function runImport() {
	try {
		await apiRequest("POST", "/api/v1/import", { auth: true });
		await poll();
	} catch (e) {
		logLine("import failed: " + e.message, "err");
		openLog();
	}
}

/*
 * The dock remembers whether it was open. Someone who opened it is
 * watching something, and having to reopen it on every reload is
 * exactly the friction that stops people watching at all.
 */
function setLogOpen(open, persist) {
	el("logdock").classList.toggle("collapsed", !open);
	el("log-toggle").setAttribute("aria-expanded", open ? "true" : "false");
	if (!persist)
		return;
	try {
		localStorage.setItem("cixcache-logdock", open ? "open" : "closed");
	} catch (e) {
		/* nothing persistent available -- the choice lasts this page only */
	}
}

const LOG_MIN_PX = 64;

function logMaxPx() {
	return Math.round(window.innerHeight * 0.7);
}

function setLogHeight(px, persist) {
	const clamped = Math.max(LOG_MIN_PX, Math.min(px, logMaxPx()));

	el("log").style.height = clamped + "px";
	if (!persist)
		return;
	try {
		localStorage.setItem("cixcache-logheight", String(clamped));
	} catch (e) {
		/* nothing persistent available -- the size lasts this page only */
	}
}

function restoreLogHeight() {
	let saved = null;

	try {
		saved = localStorage.getItem("cixcache-logheight");
	} catch (e) {
		saved = null;
	}
	if (saved !== null && !isNaN(parseInt(saved, 10)))
		setLogHeight(parseInt(saved, 10), 0);
}

/*
 * Drag the dock's top edge to resize. Pointer events rather than mouse
 * events so a trackpad or touchscreen works too, with capture so the
 * drag survives the pointer leaving the 7px strip -- which it will, on
 * the very first movement.
 */
function wireResize() {
	const grip = el("log-resize");
	let startY = 0;
	let startH = 0;

	grip.addEventListener("pointerdown", (e) => {
		startY = e.clientY;
		startH = el("log").getBoundingClientRect().height;
		grip.setPointerCapture(e.pointerId);
		grip.classList.add("dragging");
		document.body.classList.add("resizing");
		e.preventDefault();
	});

	grip.addEventListener("pointermove", (e) => {
		if (!grip.hasPointerCapture(e.pointerId))
			return;
		/* Dragging the top edge upward makes the dock taller. */
		setLogHeight(startH + (startY - e.clientY), 0);
	});

	const end = (e) => {
		if (!grip.hasPointerCapture(e.pointerId))
			return;
		grip.releasePointerCapture(e.pointerId);
		grip.classList.remove("dragging");
		document.body.classList.remove("resizing");
		setLogHeight(el("log").getBoundingClientRect().height, 1);
	};

	grip.addEventListener("pointerup", end);
	grip.addEventListener("pointercancel", end);
}

/* A dock sized on a tall window must not swallow a short one. */
window.addEventListener("resize", () => {
	setLogHeight(el("log").getBoundingClientRect().height, 0);
});

function restoreLogState() {
	let saved = null;

	try {
		saved = localStorage.getItem("cixcache-logdock");
	} catch (e) {
		saved = null;
	}
	setLogOpen(saved === "open", 0);
}

function openLog() {
	setLogOpen(1, 1);
}

/* ---------- wiring ---------- */

/*
 * The query lives in the URL fragment, so a search can be linked,
 * bookmarked and reloaded. Fragment rather than a query string: it
 * never reaches the server, which has no business knowing what an
 * operator was looking for.
 */
function syncHash() {
	if (helpOpen())
		return;

	const parts = [];

	if (query !== "")
		parts.push("q=" + encodeURIComponent(query));
	if (archFilter !== "")
		parts.push("arch=" + encodeURIComponent(archFilter));

	const want = parts.length !== 0 ? "#" + parts.join("&") : "";

	if (window.location.hash !== want)
		history.replaceState(null, "", window.location.pathname + want);
}

function helpOpen() {
	return !el("help").hidden;
}

/*
 * Help is a route, not a dialog: it can be linked, and Back closes it.
 * Deep links to a section (#help-publish) open it scrolled there.
 */
function setHelp(open, anchor) {
	el("help").hidden = !open;
	el("main-view").hidden = open;
	if (!open)
		return;
	if (anchor) {
		const target = document.getElementById(anchor);

		if (target !== null)
			target.scrollIntoView();
	} else {
		el("help").querySelector(".help-body").scrollTop = 0;
	}
}

function readHash() {
	const h = window.location.hash;

	if (h === "#help" || h.indexOf("#help-") === 0) {
		setHelp(1, h.substring(1));
		return;
	}
	setHelp(0, null);

	const params = h.substring(1).split("&");
	let i;

	query = "";
	archFilter = "";
	for (i = 0; i < params.length; i++) {
		const eq = params[i].indexOf("=");
		const key = eq < 0 ? params[i] : params[i].substring(0, eq);
		let value = "";

		if (eq < 0)
			continue;
		try {
			value = decodeURIComponent(params[i].substring(eq + 1));
		} catch (e) {
			continue; /* a hand-mangled hash is not worth failing over */
		}
		if (key === "q")
			query = value;
		else if (key === "arch")
			archFilter = value;
	}
	el("q").value = query;
}

/*
 * The chips are built from every artifact, not from the rows currently
 * shown -- a filter that removed its own way back would be a trap.
 * They appear only when the store holds more than one architecture,
 * for the same reason the Arch column does: with one value there is
 * nothing to choose between, and the control would only ever be able
 * to hide things.
 */
function renderArchChips() {
	const host = el("arch-chips");
	const seen = {};
	const list = [];
	let i;

	for (i = 0; i < cache.artifacts.length; i++) {
		const v = cache.artifacts[i].arch || "";

		if (v !== "" && seen[v] === undefined) {
			seen[v] = 1;
			list.push(v);
		}
	}
	list.sort();

	/* Never leave a filter on that the user can no longer see. */
	if (archFilter !== "" && seen[archFilter] === undefined) {
		archFilter = "";
		syncHash();
	}

	/*
	 * Rebuilt only when the set of architectures or the selection has
	 * actually changed. renderResults() runs on every poll, and
	 * replacing these buttons every two seconds would take the
	 * keyboard focus off one while it was being used, and undo a
	 * :hover under the pointer.
	 */
	const key = list.join(",") + "|" + archFilter;

	if (key === archChipsKey)
		return;
	archChipsKey = key;

	host.textContent = "";
	if (list.length < 2) {
		host.hidden = true;
		return;
	}

	host.hidden = false;
	list.unshift("");
	for (i = 0; i < list.length; i++) {
		const value = list[i];
		const b = document.createElement("button");

		b.className = "chip" + (archFilter === value ? " on" : "");
		b.textContent = value === "" ? "all" : value;
		b.title = value === ""
			? "Every architecture"
			: "Only artifacts built for " + value;
		b.addEventListener("click", () => {
			archFilter = value;
			page = 0;
			/*
			 * Clicking a chip is a browse, "all" included -- otherwise
			 * the way back from a filtered list is the empty home
			 * screen, and the control that got you there cannot undo
			 * itself.
			 */
			browsing = true;
			syncHash();
			renderResults();
		});
		host.appendChild(b);
	}
}

{
	const heads = document.querySelectorAll("th.sortable");
	let i;

	for (i = 0; i < heads.length; i++) {
		const th = heads[i];

		th.addEventListener("click", () => setSort(th.dataset.sort));
		/* Reachable without a mouse: they are focusable, so honour keys. */
		th.addEventListener("keydown", (e) => {
			if (e.key === "Enter" || e.key === " ") {
				e.preventDefault();
				setSort(th.dataset.sort);
			}
		});
	}
	applySortIndicators();
}

el("q").addEventListener("input", (e) => {
	query = e.target.value.trim();
	page = 0;
	syncHash();
	renderResults();
});

el("q").addEventListener("keydown", (e) => {
	if (e.key === "Escape") {
		el("q").value = "";
		query = "";
		browsing = false;
		page = 0;
		syncHash();
		renderResults();
	}
});

el("q-clear").addEventListener("click", () => {
	el("q").value = "";
	query = "";
	browsing = false;
	page = 0;
	syncHash();
	renderResults();
	el("q").focus();
});

el("btn-browse").addEventListener("click", () => {
	browsing = !browsing;
	/*
	 * The label says everything, so it has to mean it. Leaving an
	 * architecture filter on would show a subset under a control that
	 * promises the opposite -- and the chip that set it is the only
	 * thing on screen saying otherwise.
	 */
	archFilter = "";
	page = 0;
	syncHash();
	renderResults();
});

el("page-prev").addEventListener("click", () => {
	if (page > 0) {
		page--;
		renderResults();
	}
});

el("page-next").addEventListener("click", () => {
	page++;
	renderResults();
});

el("log-toggle").addEventListener("click", () => {
	setLogOpen(el("logdock").classList.contains("collapsed"), 1);
});

el("log-clear").addEventListener("click", () => {
	el("log").textContent = "";
});

el("btn-help").addEventListener("click", () => {
	if (helpOpen()) {
		history.replaceState(null, "", window.location.pathname);
		readHash();
	} else {
		window.location.hash = "help";
	}
});

el("help-close").addEventListener("click", () => {
	history.replaceState(null, "", window.location.pathname);
	setHelp(0, null);
});

window.addEventListener("hashchange", readHash);

el("btn-maint").addEventListener("click", (e) => {
	e.stopPropagation();
	el("maint").hidden = !el("maint").hidden;
});

el("maint").addEventListener("click", (e) => e.stopPropagation());
document.addEventListener("click", () => {
	el("maint").hidden = true;
});

el("btn-gc-dry").addEventListener("click", () => runGc(true));
el("btn-gc").addEventListener("click", () => runGc(false));
el("btn-import").addEventListener("click", runImport);

/*
 * The icon shows what clicking will DO, not what is currently on: a
 * moon means "go dark". Two subpaths in one <path>, so there is nothing
 * to swap but the d attribute.
 */
const ICON_MOON = "M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z";
const ICON_SUN = "M12 17a5 5 0 1 1 0-10 5 5 0 0 1 0 10zM12 1v2M12 21v2M4.2 4.2l1.4 1.4" +
                 "M18.4 18.4l1.4 1.4M1 12h2M21 12h2M4.2 19.8l1.4-1.4M18.4 5.6l1.4-1.4";

function isDark() {
	const set = document.documentElement.getAttribute("data-theme");

	if (set)
		return set === "dark";
	return window.matchMedia("(prefers-color-scheme: dark)").matches;
}

function renderThemeIcon() {
	el("icon-theme").setAttribute("d", isDark() ? ICON_SUN : ICON_MOON);
}

el("btn-theme").addEventListener("click", () => {
	const next = isDark() ? "light" : "dark";

	document.documentElement.setAttribute("data-theme", next);
	try {
		localStorage.setItem("cixcache-theme", next);
	} catch (e) {
		/* nothing persistent available -- the choice lasts this page only */
	}
	renderThemeIcon();
});

/* Follow the system where the viewer has expressed no preference. */
window.matchMedia("(prefers-color-scheme: dark)")
	.addEventListener("change", renderThemeIcon);

/* "/" focuses the search from anywhere, as a search-first page should. */
document.addEventListener("keydown", (e) => {
	if (e.key === "/" && document.activeElement !== el("q") && !helpOpen()) {
		e.preventDefault();
		el("q").focus();
	}
	if (e.key === "Escape" && helpOpen()) {
		history.replaceState(null, "", window.location.pathname);
		setHelp(0, null);
	}
	if (e.key === "?" && document.activeElement !== el("q"))
		window.location.hash = "help";
});

renderThemeIcon();
wireResize();
restoreLogHeight();
restoreLogState();
readHash();
el("q").focus();
poll();
setInterval(poll, POLL_INTERVAL_MS);
