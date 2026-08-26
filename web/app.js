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

let tierFilter = "";
let textFilter = "";

const ledTx = document.getElementById("led-tx");
const ledRx = document.getElementById("led-rx");

function ledBlink(led) {
	led.classList.add("lit");
	setTimeout(() => led.classList.remove("lit"), 120);
}

function logLine(what, detail, kind) {
	const log = document.getElementById("log");
	const line = document.createElement("div");

	if (kind)
		line.className = kind;
	line.textContent = new Date().toISOString().substring(11, 19) + "  " + what + "  " + detail;
	log.insertBefore(line, log.firstChild);
	while (log.childNodes.length > 200)
		log.removeChild(log.lastChild);
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

		if (method !== "GET")
			logLine(method + " " + path, "-> " + res.status + " " + message, "err");
		throw new Error(message);
	}
	if (method !== "GET")
		logLine(method + " " + path, "-> " + res.status, "ok");
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
			() => logLine("copied", digest, "ok"),
			() => logLine("copy failed", digest, "err"));
}

function renderArtifacts() {
	const body = document.getElementById("artifact-rows");
	const needle = textFilter.toLowerCase();
	let shown = 0;
	let bytes = 0;

	body.textContent = "";
	for (const a of cache.artifacts) {
		if (tierFilter && a.tier !== tierFilter)
			continue;
		if (needle && a.name.toLowerCase().indexOf(needle) < 0 &&
		    a.sha256.indexOf(needle) < 0)
			continue;

		const row = document.createElement("tr");

		const tierCell = document.createElement("td");
		const badge = document.createElement("span");
		badge.className = "badge badge-" + a.tier;
		badge.textContent = a.tier === "images" ? "image" : "pkg";
		tierCell.appendChild(badge);
		row.appendChild(tierCell);

		const nameCell = document.createElement("td");
		nameCell.className = "mono";
		nameCell.textContent = a.name;
		row.appendChild(nameCell);

		const digestCell = document.createElement("td");
		const digest = document.createElement("span");
		digest.className = "mono digest";
		digest.textContent = a.sha256.substring(0, 16) + "…";
		digest.title = a.sha256 + " (click to copy)";
		digest.addEventListener("click", () => copyDigest(a.sha256));
		digestCell.appendChild(digest);
		row.appendChild(digestCell);

		const sizeCell = document.createElement("td");
		sizeCell.className = "num";
		sizeCell.textContent = a.bytes < 0 ? "dangling" : humanBytes(a.bytes);
		row.appendChild(sizeCell);

		const urlCell = document.createElement("td");
		const link = document.createElement("a");
		link.className = "mono";
		link.href = a.url;
		link.textContent = a.url;
		urlCell.appendChild(link);
		row.appendChild(urlCell);

		body.appendChild(row);
		shown++;
		if (a.bytes > 0)
			bytes += a.bytes;
	}
	document.getElementById("artifact-empty").hidden = shown > 0;
	setText("artifact-count", shown + " shown, " + humanBytes(bytes));
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

		logLine(dryRun ? "gc dry-run" : "gc",
		        r.removed + " blobs, " + humanBytes(r.bytes_freed), "ok");
		await poll();
	} catch (e) {
		logLine("gc", e.message, "err");
	}
}

async function runImport() {
	try {
		await apiRequest("POST", "/api/v1/import", { auth: true });
		logLine("import", "started", "ok");
		await refreshImport();
	} catch (e) {
		logLine("import", e.message, "err");
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
	renderArtifacts();
});

for (const btn of document.querySelectorAll(".tier-btn"))
	btn.addEventListener("click", () => {
		tierFilter = btn.getAttribute("data-tier");
		renderArtifacts();
	});

document.getElementById("btn-gc-dry").addEventListener("click", () => runGc(true));
document.getElementById("btn-gc").addEventListener("click", () => runGc(false));
document.getElementById("btn-import").addEventListener("click", runImport);

poll();
setInterval(poll, POLL_INTERVAL_MS);
