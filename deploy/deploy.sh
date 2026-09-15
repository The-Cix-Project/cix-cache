#!/usr/bin/env bash
#
# cix-cache deployer.
#
# One script, run as often as you like. It brings a machine to the state
# described by the latest release tag and does nothing when it is
# already there, so the same command is both the install and the update
# -- which is why the systemd timer it installs simply runs this file
# again. A separate updater would be a second copy of this logic, and
# the two would drift.
#
#   sudo ./deploy.sh
#   sudo CIXCACHE_DOMAIN=cache.example.org ./deploy.sh
#
# Everything is a variable; see the block below. Nothing here overwrites
# a store or a config that already exists -- a store is operator data
# and a config holds a credential.
#
set -euo pipefail

# useradd, install and systemctl live in sbin, which is on root's PATH
# but not on every caller's -- a sudo that preserves the invoking PATH,
# or a service unit with a trimmed one, loses them. Appended rather
# than prepended so an explicit PATH still wins for everything else.
case ":$PATH:" in
	*:/usr/sbin:*) ;;
	*) PATH="$PATH:/usr/sbin:/sbin" ;;
esac
export PATH

# ---------------------------------------------------------------- vars

DOMAIN=${CIXCACHE_DOMAIN:-cache.cix.world}
REPO=${CIXCACHE_REPO:-https://github.com/The-Cix-Project/cix-cache}

PREFIX=${CIXCACHE_PREFIX:-/opt/cixcache}
SRC=${CIXCACHE_SRC:-$PREFIX/src}
STORE=${CIXCACHE_STORE:-/var/lib/cixcache}
SVC_USER=${CIXCACHE_USER:-cixcache}
SERVICE=${CIXCACHE_SERVICE:-cixcache}

# Bound to loopback on purpose: Caddy is the only thing that should be
# reachable from outside, so there is no second door to remember to
# close. Change it and you have published an unencrypted copy.
BIND=${CIXCACHE_BIND:-127.0.0.1}
PORT=${CIXCACHE_PORT:-8080}

# systemd OnCalendar= expression for the update timer.
UPDATE_ON_CALENDAR=${CIXCACHE_UPDATE_ON_CALENDAR:-hourly}

# Optional. Let's Encrypt emails expiry warnings here; Caddy works
# without it, and without it you get no warning if renewal ever stops.
ACME_EMAIL=${CIXCACHE_ACME_EMAIL:-}

# Escape hatches, for testing this script somewhere that must not grow
# a Caddy or a unit file.
SKIP_CADDY=${CIXCACHE_SKIP_CADDY:-0}
SKIP_SYSTEMD=${CIXCACHE_SKIP_SYSTEMD:-0}
SKIP_PACKAGES=${CIXCACHE_SKIP_PACKAGES:-0}

# The site file this script owns, and the distribution's main Caddyfile
# that has to import it. Both variables because a distribution that
# puts them elsewhere should not need this script edited.
CADDY_MAIN=${CIXCACHE_CADDY_MAIN:-/etc/caddy/Caddyfile}
CADDYFILE=${CIXCACHE_CADDYFILE:-/etc/caddy/Caddyfile.d/$DOMAIN.caddy}
UNIT_DIR=${CIXCACHE_UNIT_DIR:-/etc/systemd/system}
CONF=$PREFIX/etc/cixcache.conf

# ------------------------------------------------------------- helpers

say()  { printf '\033[1m==>\033[0m %s\n' "$*"; }
info() { printf '    %s\n' "$*"; }
die()  { printf '\033[1;31mdeploy: %s\033[0m\n' "$*" >&2; exit 1; }

# Writes stdin to $1 only when the content differs, and says which it
# did. Every config this script owns goes through here, so a re-run
# reloads nothing it did not actually change.
changed=0
install_if_changed() {
	local dest=$1 mode=$2 tmp
	tmp=$(mktemp)
	cat >"$tmp"
	if [ -f "$dest" ] && cmp -s "$tmp" "$dest"; then
		rm -f "$tmp"
		info "unchanged: $dest"
		return 1
	fi
	install -D -m "$mode" "$tmp" "$dest"
	rm -f "$tmp"
	info "wrote:     $dest"
	changed=1
	return 0
}

# ------------------------------------------------------------ preflight

[ "$(id -u)" -eq 0 ] || die "run as root (sudo $0)"

case $DOMAIN in
	''|*[!A-Za-z0-9.-]*) die "CIXCACHE_DOMAIN '$DOMAIN' is not a hostname" ;;
esac

# A single flock around the whole run. The timer fires on a schedule and
# a build can outlast the interval; two of these racing would have one
# installing over the other's tree.
LOCK=/var/lock/cixcache-deploy.lock
exec 9>"$LOCK"
flock -n 9 || { echo "deploy: another run holds $LOCK; leaving it to finish"; exit 0; }

say "cix-cache -> $DOMAIN"
info "repo   $REPO"
info "prefix $PREFIX"
info "store  $STORE"

# ------------------------------------------------------------ packages

if [ "$SKIP_PACKAGES" != 1 ]; then
	say "Packages"
	missing=()
	for pkg in git make tcc curl ca-certificates; do
		dpkg -s "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
	done
	if [ ${#missing[@]} -gt 0 ]; then
		info "installing: ${missing[*]}"
		DEBIAN_FRONTEND=noninteractive apt-get update -qq
		DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "${missing[@]}"
	else
		info "already present"
	fi
	# tcc and nothing else: the project's toolchain mandate is not a
	# preference, and gcc produces a binary this project does not ship.
	command -v tcc >/dev/null || die "tcc is required and was not installed"
fi

if [ "$SKIP_CADDY" != 1 ] && ! command -v caddy >/dev/null; then
	say "Caddy"
	info "installing from the official repository"
	DEBIAN_FRONTEND=noninteractive apt-get install -y -qq debian-keyring debian-archive-keyring apt-transport-https
	curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' \
		| gpg --batch --yes --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
	curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' \
		>/etc/apt/sources.list.d/caddy-stable.list
	DEBIAN_FRONTEND=noninteractive apt-get update -qq
	DEBIAN_FRONTEND=noninteractive apt-get install -y -qq caddy
fi

# ---------------------------------------------------------- user, dirs

say "User and directories"
if id -u "$SVC_USER" >/dev/null 2>&1; then
	info "user $SVC_USER exists"
else
	# No shell and no home: it owns a directory of blobs and binds a
	# loopback port, and nothing about that needs a login.
	useradd --system --home-dir "$STORE" --no-create-home --shell /usr/sbin/nologin "$SVC_USER"
	info "created system user $SVC_USER"
fi
install -d -m 0755 "$PREFIX" "$PREFIX/bin" "$PREFIX/etc" "$PREFIX/share"
install -d -m 0755 -o "$SVC_USER" -g "$SVC_USER" "$STORE"

# --------------------------------------------------------------- source

say "Source"
if [ -d "$SRC/.git" ]; then
	git -C "$SRC" remote set-url origin "$REPO"
	git -C "$SRC" fetch --quiet --tags --prune origin
else
	install -d -m 0755 "$(dirname "$SRC")"
	git clone --quiet "$REPO" "$SRC"
	git -C "$SRC" fetch --quiet --tags origin
fi

# The latest RELEASE, not the latest commit. An in-progress afternoon on
# main is not a thing a public endpoint should pick up by itself, and
# the tag is what makes the version the binary reports mean something.
TAG=${CIXCACHE_REF:-$(git -C "$SRC" tag --list 'v*' --sort=-v:refname | head -1)}
[ -n "$TAG" ] || die "no v* tag found in $REPO"
info "latest release: $TAG"

git -C "$SRC" -c advice.detachedHead=false checkout --quiet --force "$TAG"

# --------------------------------------------------------- self-install

# The timer runs an INSTALLED copy of this script, not the one in the
# checkout.
#
# Pointing systemd at $SRC/deploy/deploy.sh looks tidier and is a trap:
# the checkout sits at a release tag, so any tag predating this file --
# every tag that exists today -- leaves the unit with an ExecStart that
# does not resolve, and the timer fails silently forever. An installed
# copy is always there, because the run that installs the timer is the
# run that puts it there.
#
# It still self-updates: each run lays down whatever the current tag
# carries, and keeps the previous copy when a tag carries none.
DEPLOY_BIN=$PREFIX/bin/cix-cache-deploy
self=$SRC/deploy/deploy.sh
[ -f "$self" ] || self=$0
if [ -f "$self" ] && ! cmp -s "$self" "$DEPLOY_BIN"; then
	# Renamed into place rather than written over: this script may BE
	# the file being replaced, and bash reads a script as it runs it.
	# Truncating it mid-run executes whatever lands at the old offset.
	tmp=$PREFIX/bin/.cix-cache-deploy.$$
	cp "$self" "$tmp"
	chmod 0755 "$tmp"
	mv -f "$tmp" "$DEPLOY_BIN"
	info "installed the deployer to $DEPLOY_BIN"
fi

# ---------------------------------------------------- build, gated by tests

# What is installed is asked what it is, rather than tracked in a state
# file beside it. The binary reports a canonical artifact name carrying
# its own tag (ADR-0013), so it is the only thing that can say for
# certain which release is on disk.
WANT="cix-cache-$TAG-1-$(uname -m)"
HAVE=$("$PREFIX/bin/cixcached" --version 2>/dev/null | awk '{print $1}' || true)

if [ "$HAVE" = "$WANT" ]; then
	say "Build: already at $TAG"
else
	say "Build: $HAVE -> $WANT"
	make -C "$SRC" --quiet clean >/dev/null
	make -C "$SRC" --quiet all

	# The gate. A tag that does not pass its own tests does not reach
	# the endpoint: make install is what publishes a binary, and it is
	# downstream of this line, so a failure here leaves the running
	# server exactly as it was.
	say "Tests"
	if ! make -C "$SRC" --quiet test; then
		die "$TAG fails its own tests -- refusing to install it. The running server is untouched."
	fi

	make -C "$SRC" --quiet install PREFIX="$PREFIX"
	changed=1
fi

# --------------------------------------------------------------- config

say "Configuration"
if [ -f "$CONF" ]; then
	info "keeping existing $CONF (holds a credential)"
else
	# Generated once and never regenerated: rewriting it on a re-run
	# would silently invalidate every publisher's token.
	token=$(head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n')
	install -d -m 0755 "$PREFIX/etc"
	umask 077
	cat >"$CONF" <<-EOF
		# Written by deploy.sh on $(date -u +%Y-%m-%dT%H:%M:%SZ). Yours to edit;
		# the deployer will not touch it again.

		root=$STORE
		bind=$BIND
		port=$PORT
		web_root=$PREFIX/share/web

		# Pull is open. Consumers verify every byte against a checksum that
		# comes from git over a different protocol, so an open pull surface
		# gives an attacker nothing a host would accept (ADR-0002).
		pull_token=

		# Push is not. An open push lets anyone fill the disk, or plant
		# blobs every puller then has to fetch and reject.
		push_token=$token

		# Suffixes that may not be published unsigned (ADR-0012).
		require_signature=.iso
	EOF
	chown root:"$SVC_USER" "$CONF"
	chmod 0640 "$CONF"
	info "wrote $CONF with a generated push token"
	changed=1
fi

# ------------------------------------------------------------ systemd

if [ "$SKIP_SYSTEMD" != 1 ]; then
	say "systemd"

	unit_changed=0
	install_if_changed "$UNIT_DIR/$SERVICE.service" 0644 <<-EOF && unit_changed=1
		[Unit]
		Description=cix-cache artifact registry
		Documentation=$REPO
		After=network-online.target
		Wants=network-online.target

		[Service]
		Type=simple
		User=$SVC_USER
		Group=$SVC_USER
		ExecStart=$PREFIX/bin/cixcached --config=$CONF
		Restart=on-failure
		RestartSec=2

		# The registry serves opaque bytes and is never a trust boundary, so
		# it needs no privilege of any kind beyond reading its store and
		# binding one port.
		NoNewPrivileges=true
		PrivateTmp=true
		ProtectSystem=strict
		ProtectHome=true
		ReadWritePaths=$STORE
		ProtectKernelTunables=true
		ProtectKernelModules=true
		ProtectControlGroups=true
		RestrictAddressFamilies=AF_INET AF_INET6
		RestrictNamespaces=true
		LockPersonality=true
		MemoryDenyWriteExecute=true
		SystemCallArchitectures=native

		StandardOutput=journal
		StandardError=journal

		[Install]
		WantedBy=multi-user.target
	EOF

	# The timer runs THIS script. That is the whole reason it is
	# idempotent: the update path and the install path are the same
	# code, so there is nothing to keep in step.
	install_if_changed "$UNIT_DIR/$SERVICE-update.service" 0644 <<-EOF || true
		[Unit]
		Description=cix-cache: deploy the latest release tag
		Documentation=$REPO
		After=network-online.target
		Wants=network-online.target

		[Service]
		Type=oneshot
		ExecStart=$DEPLOY_BIN
		Environment=CIXCACHE_DOMAIN=$DOMAIN
		Environment=CIXCACHE_REPO=$REPO
		Environment=CIXCACHE_PREFIX=$PREFIX
		Environment=CIXCACHE_SRC=$SRC
		Environment=CIXCACHE_STORE=$STORE
		Environment=CIXCACHE_USER=$SVC_USER
		Environment=CIXCACHE_SERVICE=$SERVICE
		Environment=CIXCACHE_BIND=$BIND
		Environment=CIXCACHE_PORT=$PORT
		Environment=CIXCACHE_UPDATE_ON_CALENDAR=$UPDATE_ON_CALENDAR
		Environment=CIXCACHE_ACME_EMAIL=$ACME_EMAIL
		Environment=CIXCACHE_CADDYFILE=$CADDYFILE
		Environment=CIXCACHE_CADDY_MAIN=$CADDY_MAIN
		# Carried forward so the timer reproduces the shape this machine
		# was deployed with. Without it, a deployment fronted by
		# something other than Caddy would grow one by itself, an hour
		# later, unattended.
		Environment=CIXCACHE_SKIP_CADDY=$SKIP_CADDY
		# Already installed; re-running apt on every tick buys nothing.
		Environment=CIXCACHE_SKIP_PACKAGES=1
	EOF

	install_if_changed "$UNIT_DIR/$SERVICE-update.timer" 0644 <<-EOF || true
		[Unit]
		Description=cix-cache: check for a new release tag

		[Timer]
		OnCalendar=$UPDATE_ON_CALENDAR
		# Survives a reboot that spanned a scheduled run.
		Persistent=true
		# Without this every machine on this schedule asks GitHub at
		# exactly the same second.
		RandomizedDelaySec=300

		[Install]
		WantedBy=timers.target
	EOF

	systemctl daemon-reload
	systemctl enable --quiet --now "$SERVICE-update.timer"

	if ! systemctl is-enabled --quiet "$SERVICE.service" 2>/dev/null; then
		systemctl enable --quiet "$SERVICE.service"
	fi
	if [ "$changed" = 1 ] || [ "$unit_changed" = 1 ] || ! systemctl is-active --quiet "$SERVICE.service"; then
		info "restarting $SERVICE"
		systemctl restart "$SERVICE.service"
	else
		info "$SERVICE already running the current release"
	fi
fi

# --------------------------------------------------------------- caddy

if [ "$SKIP_CADDY" != 1 ]; then
	say "Caddy"

	# This Caddy is assumed to be serving other sites. Everything below
	# is written so that a failure here leaves those sites exactly as
	# they were found, on disk as well as in the running process.

	# Our own file in an imported directory, never an edit to somebody
	# else's site block, so a package upgrade or another tool rewriting
	# the main Caddyfile cannot take this site down with it -- and
	# removing this site is deleting one file.
	install -d -m 0755 "$(dirname "$CADDYFILE")"

	# Refuse to fight over a domain another site block already claims.
	# Caddy would reject the duplicate anyway; saying so here names the
	# file to look in instead of printing an adapter error.
	if [ -f "$CADDY_MAIN" ] && grep -Eq "^[[:space:]]*(https?://)?$DOMAIN([[:space:]]|,|\{|$)" "$CADDY_MAIN"; then
		die "$CADDY_MAIN already has a site block for $DOMAIN -- refusing to add a second"
	fi

	# Snapshot before touching anything, so a config that does not
	# validate can be put back exactly. Without this, a bad render
	# leaves a broken Caddyfile on disk: the running Caddy is fine
	# until the next reload for an unrelated site, which then fails.
	caddy_backup=""
	if [ -f "$CADDY_MAIN" ]; then
		caddy_backup=$(mktemp)
		cp -a "$CADDY_MAIN" "$caddy_backup"
	fi
	caddyfile_existed=0
	[ -f "$CADDYFILE" ] && caddyfile_existed=1
	caddyfile_backup=""
	if [ "$caddyfile_existed" = 1 ]; then
		caddyfile_backup=$(mktemp)
		cp -a "$CADDYFILE" "$caddyfile_backup"
	fi

	caddy_rollback() {
		[ -n "$caddy_backup" ] && cp -a "$caddy_backup" "$CADDY_MAIN"
		if [ "$caddyfile_existed" = 1 ]; then
			cp -a "$caddyfile_backup" "$CADDYFILE"
		else
			rm -f "$CADDYFILE"
		fi
		info "rolled back; $CADDY_MAIN is as it was found"
	}

	if ! grep -q "$(dirname "$CADDYFILE")" "$CADDY_MAIN" 2>/dev/null; then
		install -d -m 0755 "$(dirname "$CADDY_MAIN")"
		printf '\n# cix-cache\nimport %s/*.caddy\n' "$(dirname "$CADDYFILE")" >>"$CADDY_MAIN"
		info "added an import line to $CADDY_MAIN"
	fi

	# Let's Encrypt emails renewal failures here. Omitted entirely when
	# unset, rather than rendered as an empty directive.
	acme_line=""
	[ -n "$ACME_EMAIL" ] && acme_line="
    tls $ACME_EMAIL"

	caddy_changed=0
	install_if_changed "$CADDYFILE" 0644 <<-EOF && caddy_changed=1
		# cix-cache. Written by deploy.sh -- edits here are overwritten.
		#
		# Caddy obtains and renews the certificate itself and redirects
		# :80 to :443, so http is only ever the redirect. cixcached is
		# bound to $BIND and is reachable no other way.
		#
		# Deliberately no "encode": artifacts are already-compressed
		# archives and ISOs, and re-encoding a 75 MB ISO spends CPU to
		# make it marginally bigger. The dashboard is small enough not
		# to care.
		$DOMAIN {$acme_line
		    reverse_proxy $BIND:$PORT

		    # Belt to the redirect's braces: the redirect fixes a wrong
		    # URL once, this stops a browser ever trying http again.
		    header Strict-Transport-Security "max-age=31536000"
		}
	EOF

	# Validated as a whole -- every other site included -- before
	# anything is asked to load it.
	if ! caddy validate --config "$CADDY_MAIN" --adapter caddyfile >/dev/null 2>&1; then
		caddy validate --config "$CADDY_MAIN" --adapter caddyfile 2>&1 | sed 's/^/    /' || true
		caddy_rollback
		die "the combined Caddy config does not validate; nothing was reloaded"
	fi

	if [ "$caddy_changed" = 1 ]; then
		# Reload, never restart. A restart drops every connection Caddy
		# is serving, and most of them belong to somebody else. A
		# reload that fails is reported and rolled back rather than
		# escalated into an outage.
		if systemctl reload caddy; then
			info "reloaded caddy"
		else
			caddy_rollback
			systemctl reload caddy || true
			die "caddy refused to reload; the previous config was restored"
		fi
	else
		info "caddy config unchanged; not reloading"
	fi
	rm -f "$caddy_backup" "$caddyfile_backup"

	# Only if it is not already running something. Never restarted.
	systemctl is-active --quiet caddy || systemctl start caddy
	systemctl is-enabled --quiet caddy || systemctl enable --quiet caddy
fi

# --------------------------------------------------------------- verify

say "Verify"
if [ "$SKIP_SYSTEMD" != 1 ]; then
	for _ in $(seq 20); do
		if curl -fsS --max-time 2 "http://$BIND:$PORT/api/v1/status" >/dev/null 2>&1; then
			break
		fi
		sleep 0.5
	done

	if status=$(curl -fsS --max-time 5 "http://$BIND:$PORT/api/v1/status" 2>/dev/null); then
		info "local:  $(printf '%s' "$status" | tr ',' '\n' | grep -o '"identity":"[^"]*"' | cut -d'"' -f4)"
	else
		die "cixcached is not answering on $BIND:$PORT -- see: journalctl -u $SERVICE -n 50"
	fi
else
	# Nothing started it, so there is nothing to answer. The binary can
	# still say what it is, which is what this step is really checking.
	info "local:  $("$PREFIX/bin/cixcached" --version | awk '{print $1}') installed (service skipped)"
fi

if [ "$SKIP_CADDY" != 1 ]; then
	# Not fatal. A brand new deployment fails here until the A record
	# exists and Let's Encrypt has issued, which is minutes and is not
	# something this script can do anything about.
	if curl -fsS --max-time 15 "https://$DOMAIN/api/v1/status" >/dev/null 2>&1; then
		info "public: https://$DOMAIN is serving"
	else
		info "public: https://$DOMAIN not answering yet"
		info "        needs an A record for $DOMAIN pointing here, and :80 and"
		info "        :443 reachable, before Caddy can complete the ACME challenge."
		info "        watch it with: journalctl -u caddy -f"
	fi
fi

say "Done. $TAG is deployed; the timer re-runs this $UPDATE_ON_CALENDAR."
