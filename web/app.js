"use strict";

/*
 * Vanilla, no framework and no build step, in the same shape as the
 * Cix dashboard: one cache object every renderer reads from, DOM API
 * rendering rather than innerHTML, and a single fetch wrapper that
 * blinks the activity LEDs.
 */

const POLL_INTERVAL_MS = 2000;

const cache = {
	status: null,
	artifacts: [],
	importing: false
};

/*
 * 85 artifacts already overflow a screen and the store only grows, so
 * the table pages rather than scrolling forever. Filtering applies
 * before paging, and any filter change resets to the first page --
 * otherwise you can filter down to three rows and still be looking at
 * page four of nothing.
 */
const PAGE_SIZE = 50;

let tierFilter = "";
let textFilter = "";
let page = 0;
let logSeq = 0;

const ledTx = document.getElementById("led-tx");
const ledRx = document.getElementById("led-rx");

function ledBlink(led) {
	led.classList.add("lit");
	setTimeout(() => led.classList.remove("lit"), 120);
}

function logLine(text, kind) {
	const log = document.getElementById("log");
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

	for (const e of data.entries || []) {
		const when = new Date(e.time_ms).toTimeString().substring(0, 8);

		logLine(when + "  " + e.text, e.level === "warn" ? "err" : "");
	}
	logSeq = data.seq;
}

function token() {
	return document.getElementById("token").value.trim();
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
	if (!res.ok) {
		const message = json && json.error ? json.error : "request failed (HTTP " + res.status + ")";

		throw new Error(message);
	}
	return json;
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

function setText(id, value) {
	document.getElementById(id).textContent = value;
}

/* ---------- renderers ---------- */

function renderStatus() {
	const s = cache.status;

	if (!s)
		return;
	setText("stat-packages", String(s.packages));
	setText("stat-package-bytes", humanBytes(s.package_bytes));
	setText("stat-images", String(s.images));
	setText("stat-image-bytes", humanBytes(s.image_bytes));
	setText("stat-served", humanBytes(s.served_bytes));
	setText("stat-requests", s.requests + " requests");
	/*
	 * Misses are shown next to hits, and in the warning colour once
	 * there are misses but no hits at all -- that combination is what
	 * a registry serving the wrong paths looks like, and it is
	 * otherwise indistinguishable from an idle one.
	 */
	const hitmiss = document.getElementById("stat-hitmiss");
	hitmiss.textContent = s.artifact_hits + " hit / " + s.artifact_misses + " missed";
	hitmiss.style.color = (s.artifact_misses > 0 && s.artifact_hits === 0)
		? "var(--warn)" : "";
	setText("stat-uptime", humanDuration(s.uptime_seconds));
	setText("stat-build", s.build_version);
	setText("foot-root", "root: " + s.root);
	setText("foot-version", "cixcached " + s.build_version + " built " + s.build_time);

	const pull = document.getElementById("pill-pull");
	pull.className = "pill ok";
	pull.textContent = "";
	pull.appendChild(document.createTextNode("pull "));
	const pullVal = document.createElement("strong");
	pullVal.textContent = s.pull_open ? "open" : "token";
	pull.appendChild(pullVal);

	/*
	 * An unauthenticated push is the one configuration worth shouting
	 * about: pull being open is by design, because consumers verify
	 * every byte themselves, but an open push lets anyone fill the disk.
	 */
	const push = document.getElementById("pill-push");
	push.className = s.push_configured ? "pill ok" : "pill warn";
	push.textContent = "";
	push.appendChild(document.createTextNode("push "));
	const pushVal = document.createElement("strong");
	pushVal.textContent = s.push_configured ? "token" : "OPEN";
	push.appendChild(pushVal);
}

function copyDigest(digest) {
	if (navigator.clipboard)
		navigator.clipboard.writeText(digest).then(
			() => logLine("copied " + digest, "ok"),
			() => logLine("could not copy to clipboard", "err"));
}

/* The URL is the name -- images one level down, packages at the root. */
function artifactUrl(a) {
	return (a.tier === "images" ? "/images/" : "/") + a.name;
}

function matches(a) {
	const needle = textFilter.toLowerCase();

	if (tierFilter && a.tier !== tierFilter)
		return false;
	if (!needle)
		return true;
	return a.name.toLowerCase().indexOf(needle) >= 0 || a.sha256.indexOf(needle) >= 0;
}

function renderArtifacts() {
	const body = document.getElementById("artifact-rows");
	const shown = cache.artifacts.filter(matches);
	const pages = Math.max(1, Math.ceil(shown.length / PAGE_SIZE));
	let bytes = 0;

	if (page >= pages)
		page = pages - 1;

	for (const a of shown)
		if (a.bytes > 0)
			bytes += a.bytes;

	body.textContent = "";
	for (const a of shown.slice(page * PAGE_SIZE, (page + 1) * PAGE_SIZE)) {
		const row = document.createElement("tr");

		const tierCell = document.createElement("td");
		const badge = document.createElement("span");
		badge.className = "badge badge-" + a.tier;
		badge.textContent = a.tier === "images" ? "image" : "pkg";
		tierCell.appendChild(badge);
		row.appendChild(tierCell);

		/*
		 * The name is the link, so the URL is not repeated in a
		 * column of its own. Hovering shows the full filename, which
		 * is what the store is actually keyed by.
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

		body.appendChild(row);
	}

	document.getElementById("artifact-empty").hidden = shown.length > 0;
	document.getElementById("pager").hidden = shown.length <= PAGE_SIZE;
	document.getElementById("page-prev").disabled = page === 0;
	document.getElementById("page-next").disabled = page >= pages - 1;
	setText("page-label", shown.length === 0
		? ""
		: (page * PAGE_SIZE + 1) + "-" + Math.min((page + 1) * PAGE_SIZE, shown.length) +
		  " of " + shown.length);
	setText("artifact-count", shown.length + " shown, " + humanBytes(bytes));
}

function renderImport() {
	const el = document.getElementById("import-state");

	if (!cache.importing) {
		el.textContent = "";
		return;
	}
	el.className = "pill warn";
	el.textContent = "";
	el.appendChild(document.createTextNode("import "));
	const v = document.createElement("strong");
	v.textContent = "running";
	el.appendChild(v);
}

/* ---------- refreshers ---------- */

async function refreshStatus() {
	cache.status = await apiRequest("GET", "/api/v1/status");
	renderStatus();
}

async function refreshArtifacts() {
	const data = await apiRequest("GET", "/api/v1/artifacts");

	cache.artifacts = data.artifacts || [];
	renderArtifacts();
}

async function refreshImport() {
	const data = await apiRequest("GET", "/api/v1/import-status");

	cache.importing = !!data.running;
	renderImport();
}

async function poll() {
	const reach = document.getElementById("pill-reach");

	if (document.hidden)
		return;
	try {
		await refreshStatus();
		await refreshArtifacts();
		await refreshImport();
		await refreshLog();
		reach.className = "pill ok";
		reach.textContent = "";
		reach.appendChild(document.createTextNode("server "));
		const v = document.createElement("strong");
		v.textContent = "reachable";
		reach.appendChild(v);
	} catch (e) {
		reach.className = "pill";
		reach.textContent = "";
		reach.appendChild(document.createTextNode("server "));
		const v = document.createElement("strong");
		v.textContent = "unreachable";
		reach.appendChild(v);
	}
}

/* ---------- actions ---------- */

async function runGc(dryRun) {
	try {
		const r = await apiRequest(dryRun ? "GET" : "POST", "/api/v1/gc", { auth: !dryRun });

		await poll();
	} catch (e) {
		logLine("gc failed: " + e.message, "err");
	}
}

async function runImport() {
	try {
		await apiRequest("POST", "/api/v1/import", { auth: true });
		await refreshImport();
	} catch (e) {
		logLine("import failed: " + e.message, "err");
	}
}

/* ---------- wiring ---------- */

document.getElementById("theme-toggle").addEventListener("click", () => {
	const root = document.documentElement;
	const current = root.getAttribute("data-theme");
	const next = current === "dark" ? "light" : "dark";

	root.setAttribute("data-theme", next);
	try {
		localStorage.setItem("cixcache-theme", next);
	} catch (e) {
		/* nothing persistent available -- the choice lasts this page only */
	}
});

document.getElementById("filter").addEventListener("input", (e) => {
	textFilter = e.target.value.trim();
	page = 0;
	renderArtifacts();
});

for (const btn of document.querySelectorAll(".tier-btn"))
	btn.addEventListener("click", () => {
		tierFilter = btn.getAttribute("data-tier");
		page = 0;
		renderArtifacts();
	});

document.getElementById("page-prev").addEventListener("click", () => {
	if (page > 0) {
		page--;
		renderArtifacts();
	}
});

document.getElementById("page-next").addEventListener("click", () => {
	page++;
	renderArtifacts();
});

document.getElementById("log-clear").addEventListener("click", () => {
	document.getElementById("log").textContent = "";
});

document.getElementById("btn-gc-dry").addEventListener("click", () => runGc(true));
document.getElementById("btn-gc").addEventListener("click", () => runGc(false));
document.getElementById("btn-import").addEventListener("click", runImport);

poll();
setInterval(poll, POLL_INTERVAL_MS);
