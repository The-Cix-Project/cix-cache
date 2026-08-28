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

/* The URL is the name, at the root of base_url. */
function artifactUrl(a) {
	return "/" + a.name;
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
	return a.sha256.indexOf(needle) === 0;
}

function copyDigest(digest) {
	if (navigator.clipboard)
		navigator.clipboard.writeText(digest).then(
			() => logLine("copied " + digest),
			() => logLine("could not copy to clipboard", "err"));
}

function resultRow(a) {
	const row = document.createElement("tr");

	/*
	 * The name is the link, so the URL is not repeated in a column of
	 * its own. Hovering shows the full filename, which is what the
	 * store is actually keyed by.
	 */
	const nameCell = document.createElement("td");
	const link = document.createElement("a");
	link.className = "mono";
	link.href = artifactUrl(a);
	link.textContent = a.artifact;
	link.title = a.name;
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

	const sizeCell = document.createElement("td");
	sizeCell.className = "num";
	sizeCell.textContent = a.bytes < 0 ? "dangling" : humanBytes(a.bytes);
	row.appendChild(sizeCell);

	const digestCell = document.createElement("td");
	const digest = document.createElement("span");
	digest.className = "mono digest";
	digest.textContent = a.sha256.substring(0, 16) + "…";
	digest.title = a.sha256 + " (click to copy)";
	digest.addEventListener("click", () => copyDigest(a.sha256));
	digestCell.appendChild(digest);
	row.appendChild(digestCell);

	/* When this NAME was published here, not when the bytes were built. */
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

function renderResults() {
	const results = el("results");
	const body = el("rows");
	const show = query !== "" || browsing;

	results.hidden = !show;
	/* Lets the layout stop vertically centring once there is a list. */
	el("main-view").classList.toggle("has-results", show);
	el("hint").hidden = show;
	el("q-clear").hidden = query === "";
	if (!show)
		return;

	const shown = cache.artifacts.filter(matches);
	const pages = Math.max(1, Math.ceil(shown.length / PAGE_SIZE));

	if (page >= pages)
		page = pages - 1;

	body.textContent = "";
	if (shown.length === 0) {
		const row = document.createElement("tr");
		const cell = document.createElement("td");

		cell.colSpan = 6;
		cell.className = "empty";
		cell.textContent = "Nothing matches “" + query + "”.";
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
	/* Four zlibs and three greps: packages and artifacts differ. */
	setText("m-unique", String(s.unique_packages));
	setText("m-packages", String(s.packages));
	setText("m-bytes", humanBytes(s.package_bytes));

	/* Status bar: what the server is doing. */
	setText("s-requests", s.requests + " req");
	setText("s-served", humanBytes(s.served_bytes) + " served");
	setText("s-uptime", "up " + humanDuration(s.uptime_seconds));
	setText("s-build", s.build_version);

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
	const want = query ? "#q=" + encodeURIComponent(query) : "";

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

	const m = /^#q=(.*)$/.exec(h);

	if (m === null)
		return;
	try {
		query = decodeURIComponent(m[1]);
	} catch (e) {
		query = "";
	}
	el("q").value = query;
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
	page = 0;
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
