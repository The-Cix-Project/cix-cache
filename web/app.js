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

let tierFilter = "";
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

/* The URL is the name -- images one level down, packages at the root. */
function artifactUrl(a) {
	return (a.tier === "images" ? "/images/" : "/") + a.name;
}

const HEX64 = /^[0-9a-f]{64}$/;

/*
 * Digests are matched by PREFIX, never as a substring, and so are image
 * versions -- which are digests too.
 *
 * Substring matching them looks reasonable and is useless in practice:
 * every two-character query is a substring of almost every sha256, so
 * searching "bc" returned 33 artifacts including jumpbox and openssl.
 * A prefix is also how anyone actually refers to a hash, the way git
 * and docker short ids work.
 */
function matches(a) {
	const needle = query.toLowerCase();

	if (tierFilter && a.tier !== tierFilter)
		return false;
	if (!needle)
		return true;
	if (a.artifact.toLowerCase().indexOf(needle) >= 0)
		return true;
	if (a.version !== "" && !HEX64.test(a.version) &&
	    a.version.toLowerCase().indexOf(needle) >= 0)
		return true;
	if (HEX64.test(a.version) && a.version.indexOf(needle) === 0)
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

	const tierCell = document.createElement("td");
	const badge = document.createElement("span");
	badge.className = "badge badge-" + a.tier;
	badge.textContent = a.tier === "images" ? "image" : "pkg";
	tierCell.appendChild(badge);
	row.appendChild(tierCell);

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
	versionCell.className = "mono";
	if (a.tier === "images") {
		/* A manifest hash, not a label -- truncated, full on hover. */
		versionCell.textContent = a.version.substring(0, 16) + "…";
		versionCell.title = a.version;
	} else {
		versionCell.textContent = a.version || "-";
	}
	row.appendChild(versionCell);

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

	return row;
}

function renderResults() {
	const results = el("results");
	const body = el("rows");
	const show = query !== "" || browsing;

	results.hidden = !show;
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

		cell.colSpan = 5;
		cell.className = "empty";
		cell.textContent = "Nothing matches " + (query ? "“" + query + "”" : "that filter") + ".";
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
	setText("m-packages", String(s.packages));
	setText("m-images", String(s.images));
	setText("m-bytes", humanBytes(s.package_bytes + s.image_bytes));

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

	document.title = "cix-cache — " + (s.packages + s.images) + " artifacts";
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

function openLog() {
	el("logdock").classList.remove("collapsed");
	el("log-toggle").setAttribute("aria-expanded", "true");
}

/* ---------- wiring ---------- */

/*
 * The query lives in the URL fragment, so a search can be linked,
 * bookmarked and reloaded. Fragment rather than a query string: it
 * never reaches the server, which has no business knowing what an
 * operator was looking for.
 */
function syncHash() {
	const want = query ? "#q=" + encodeURIComponent(query) : "";

	if (window.location.hash !== want)
		history.replaceState(null, "", window.location.pathname + want);
}

function readHash() {
	const m = /^#q=(.*)$/.exec(window.location.hash);

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

for (const btn of document.querySelectorAll(".tier-btn"))
	btn.addEventListener("click", () => {
		tierFilter = btn.getAttribute("data-tier");
		page = 0;
		for (const other of document.querySelectorAll(".tier-btn"))
			other.classList.toggle("is-on", other === btn);
		renderResults();
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
	const dock = el("logdock");
	const open = dock.classList.toggle("collapsed") === false;

	el("log-toggle").setAttribute("aria-expanded", open ? "true" : "false");
});

el("log-clear").addEventListener("click", () => {
	el("log").textContent = "";
});

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
	if (e.key === "/" && document.activeElement !== el("q")) {
		e.preventDefault();
		el("q").focus();
	}
});

renderThemeIcon();
readHash();
el("q").focus();
poll();
setInterval(poll, POLL_INTERVAL_MS);
