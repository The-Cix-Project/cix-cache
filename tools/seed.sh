#!/bin/sh
#
# cix-cache -- seed or update a remote instance from a local one.
#
# API to API, in both directions: this reads the source over its public
# GET contract and writes the target over its public PUT contract. It
# never reads the store directory and never rsyncs into one.
#
# That is not squeamishness about ssh. A store is a blob directory plus
# a symlink directory plus a canonical-name rule, and a file copied in
# from outside has had none of those applied to it -- no name
# validation, no canonicalisation, no digest check, no signature rule.
# Pushing means the receiving daemon enforces every invariant it exists
# to enforce, on every byte, exactly as it would for any other client.
# An rsync would make this script a second implementation of publish.
#
# Re-runnable by construction: it diffs the two listings and pushes
# what is missing. A second run with nothing new does one request.
#
# POSIX sh, no bashisms, matching deploy.sh.
#
set -eu

FROM="${CIXCACHE_FROM:-http://127.0.0.1:8080}"
TO="${CIXCACHE_TO:-}"
TOKEN_FILE="${CIXCACHE_TOKEN_FILE:-}"
KEEP="${CIXCACHE_KEEP:-1}"
SINCE=""
# Development debris that lives in the local store and should not reach
# a public instance. Printed in the summary rather than dropped
# silently -- an exclusion nobody can see is an exclusion nobody can
# correct.
EXCLUDE="${CIXCACHE_EXCLUDE:-^(probe-|cix-tests$|cix-aggressive-test$)}"
PRUNE=0
DRY_RUN=0
LIMIT=0

usage() {
	cat <<'USAGE'
usage: tools/seed.sh --to=URL [options]

  --from=URL        source instance      (default http://127.0.0.1:8080)
  --to=URL          target instance      (required, e.g. https://cache.cix.world)
  --token-file=PATH file holding the target's push token
                    (or set CIXCACHE_TOKEN in the environment)

  --keep=N          releases to keep per package identity (default 1)
  --since=DATE      after choosing the newest N, skip any older than
                    DATE (YYYY-MM-DD, or anything `date -d` accepts).
                    Narrows; it never promotes a superseded release.
  --exclude=REGEX   package names to leave behind. Default drops
                    probe-*, cix-tests and cix-aggressive-test.

  --prune           DELETE artifacts on the target that this run
                    supersedes. Off by default. Only touches identities
                    this run actually manages -- anything the source
                    has never heard of is left alone.

  --dry-run         print the plan and change nothing. Run this first.
  --limit=N         stop after N uploads (for trying it out)

Retention is by identity, not by name: packages are grouped by
(name, architecture) and ranked by the source daemon's own version
ordering, so --keep=1 means the newest release of each package,
including the newest installer ISO.

Signatures ride along with whatever they sign, and are pushed FIRST --
the store refuses an artifact whose policy requires a signature until
that signature is already published.
USAGE
}

for arg in "$@"; do
	case "$arg" in
	--from=*)       FROM="${arg#*=}" ;;
	--to=*)         TO="${arg#*=}" ;;
	--token-file=*) TOKEN_FILE="${arg#*=}" ;;
	--keep=*)       KEEP="${arg#*=}" ;;
	--since=*)      SINCE="${arg#*=}" ;;
	--exclude=*)    EXCLUDE="${arg#*=}" ;;
	--limit=*)      LIMIT="${arg#*=}" ;;
	--prune)        PRUNE=1 ;;
	--dry-run)      DRY_RUN=1 ;;
	-h|--help)      usage; exit 0 ;;
	*) echo "seed: unknown argument: $arg" >&2; usage >&2; exit 2 ;;
	esac
done

die() { echo "seed: $*" >&2; exit 1; }

[ -n "$TO" ] || { echo "seed: --to is required" >&2; usage >&2; exit 2; }
case "$KEEP" in ''|*[!0-9]*) die "--keep must be a number" ;; esac
[ "$KEEP" -ge 1 ] || die "--keep must be at least 1"
case "$LIMIT" in ''|*[!0-9]*) die "--limit must be a number" ;; esac

# Trailing slashes would produce "https://host//name", which Caddy and
# the daemon disagree about.
FROM="${FROM%/}"
TO="${TO%/}"

command -v curl >/dev/null 2>&1   || die "curl is required"
command -v python3 >/dev/null 2>&1 || die "python3 is required"
command -v sha256sum >/dev/null 2>&1 || die "sha256sum is required"

SINCE_EPOCH=0
if [ -n "$SINCE" ]; then
	SINCE_EPOCH=$(date -d "$SINCE" +%s 2>/dev/null) || die "--since: cannot read date '$SINCE'"
fi

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PLANNER="$SELF_DIR/seed-plan.py"
[ -f "$PLANNER" ] || die "missing $PLANNER"

WORK=$(mktemp -d)
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT INT TERM

#
# The token reaches curl through a config file and never through argv.
# An -H "Authorization: Bearer ..." argument is visible in ps to every
# user on the box for the whole of a multi-gigabyte upload.
#
AUTH="$WORK/auth"
: > "$AUTH"
chmod 600 "$AUTH"
if [ -n "$TOKEN_FILE" ]; then
	[ -r "$TOKEN_FILE" ] || die "cannot read token file: $TOKEN_FILE"
	TOKEN=$(head -n 1 "$TOKEN_FILE" | tr -d '\r\n')
elif [ -n "${CIXCACHE_TOKEN:-}" ]; then
	TOKEN="$CIXCACHE_TOKEN"
else
	die "no push token: pass --token-file=PATH or set CIXCACHE_TOKEN"
fi
[ -n "$TOKEN" ] || die "push token is empty"
printf 'header = "Authorization: Bearer %s"\n' "$TOKEN" > "$AUTH"
unset TOKEN

#
# Preflight, and the token check is not optional.
#
# The target runs a fail2ban jail that bans an address after five 401s
# in ten minutes. Discovering a bad token inside the push loop would
# ban this machine partway through a seed, leaving the remote half
# populated and unreachable. One authenticated request up front costs
# nothing and cannot be a partial write: /api/v1/log is operator-gated
# and side-effect free.
#
echo "seed: source $FROM"
curl -fsS --max-time 15 "$FROM/api/v1/status" -o /dev/null \
	|| die "source is not reachable: $FROM"

echo "seed: target $TO"
code=$(curl -sS --max-time 20 -o /dev/null -w '%{http_code}' -K "$AUTH" "$TO/api/v1/log?lines=1" || echo 000)
case "$code" in
200) ;;
401|403) die "the target rejected the push token (HTTP $code) -- stopping before the jail counts five" ;;
000) die "target is not reachable: $TO" ;;
*)   die "unexpected reply from target: HTTP $code" ;;
esac

curl -fsS --max-time 60 "$FROM/api/v1/artifacts" -o "$WORK/local.json" \
	|| die "could not read the source listing"
curl -fsS --max-time 60 "$TO/api/v1/artifacts" -o "$WORK/remote.json" \
	|| die "could not read the target listing"

python3 "$PLANNER" "$WORK/local.json" "$WORK/remote.json" \
	"$KEEP" "$SINCE_EPOCH" "$EXCLUDE" "$PRUNE" > "$WORK/plan.tsv" \
	|| die "could not build a plan"

#
# The plan is printed in full before anything moves. A seed is a
# gigabyte leaving the box for a public server; the operator gets to
# read the list first, which is why --dry-run is the documented first
# run rather than a debugging aid.
#
fmt_mb() { awk -v b="$1" 'BEGIN { printf "%.0f MB", b / 1048576 }'; }

push_bytes=0; push_n=0; skip_n=0; conflict_n=0; prune_n=0; excl_n=0
while IFS="$(printf '\t')" read -r action name sha size artifact stem; do
	case "$action" in
	push)     push_n=$((push_n + 1)); push_bytes=$((push_bytes + size)) ;;
	skip)     skip_n=$((skip_n + 1)) ;;
	conflict) conflict_n=$((conflict_n + 1)) ;;
	prune)    prune_n=$((prune_n + 1)) ;;
	excluded) excl_n=$((excl_n + 1)) ;;
	esac
done < "$WORK/plan.tsv"

echo
awk -F'\t' '
	$1 == "push"     { printf "  push      %-52s %8.1f MB\n", $2, $4 / 1048576 }
	$1 == "conflict" { printf "  CONFLICT  %-52s different bytes already published\n", $2 }
	$1 == "prune"    { printf "  prune     %-52s superseded\n", $2 }
' "$WORK/plan.tsv"
if [ "$excl_n" -gt 0 ]; then
	echo
	echo "  excluded by --exclude:"
	awk -F'\t' '$1 == "excluded" { printf "    %s\n", $2 }' "$WORK/plan.tsv"
fi

echo
echo "seed: $push_n to push ($(fmt_mb "$push_bytes")), $skip_n already there, $prune_n to prune, $conflict_n conflicting, $excl_n excluded"

if [ "$conflict_n" -gt 0 ]; then
	#
	# A name published with different bytes is not something to
	# resolve by pushing harder. Artifact names are immutable
	# (ADR-0003): one name may only ever mean one byte sequence, and
	# two sources disagreeing about a name is a question for a person.
	# The push would be refused with a 409 anyway; stopping here says
	# why, once, instead of 120 times.
	#
	echo "seed: refusing to continue while a name means different bytes on each side" >&2
	echo "seed: delete it on the target if the target's copy is the wrong one" >&2
	exit 1
fi

if [ "$DRY_RUN" -eq 1 ]; then
	echo "seed: dry run, nothing sent"
	exit 0
fi

if [ "$push_n" -eq 0 ] && [ "$prune_n" -eq 0 ]; then
	echo "seed: target is already up to date"
	exit 0
fi

#
# Signatures first, artifacts second.
#
# Not a preference. store_needs_signature() is checked in
# begin_upload() before any of the body is staged, so an ISO whose
# .minisig is not already published is refused outright -- and the
# order is also the only one in which the target is never briefly
# holding an unsigned artifact.
#
sort -t"$(printf '\t')" -k2,2 "$WORK/plan.tsv" \
	| awk -F'\t' '$1 == "push"' \
	| awk -F'\t' '{ print ($2 ~ /\.minisig$/ ? "0" : "1") "\t" $0 }' \
	| sort -s -t"$(printf '\t')" -k1,1 \
	| cut -f2- > "$WORK/push.tsv"

sent=0
failed=0
BLOB="$WORK/blob"
while IFS="$(printf '\t')" read -r action name sha size artifact stem; do
	if [ "$LIMIT" -gt 0 ] && [ "$sent" -ge "$LIMIT" ]; then
		echo "seed: --limit=$LIMIT reached, stopping"
		break
	fi

	printf '  %s ... ' "$name"

	if ! curl -fsS --max-time 1800 "$FROM/$name" -o "$BLOB"; then
		echo "FAILED to fetch from source"
		failed=$((failed + 1))
		continue
	fi

	#
	# Verified here even though the target verifies it too. The target
	# checking means a corrupt push is refused; this checking means a
	# corrupt push is never sent, and names the source as the culprit
	# rather than leaving a 400 to be read as a network problem.
	#
	got=$(sha256sum "$BLOB" | cut -d' ' -f1)
	# "-" is the planner's placeholder for a digest the listing does not
	# carry, which is every detached signature.
	if [ "$sha" != "-" ] && [ "$got" != "$sha" ]; then
		echo "FAILED: source served $got, listing says $sha"
		failed=$((failed + 1))
		continue
	fi

	code=$(curl -sS --max-time 3600 -o "$WORK/reply" -w '%{http_code}' \
		-X PUT -T "$BLOB" -K "$AUTH" \
		-H "X-Cix-Sha256: $got" \
		"$TO/$name" || echo 000)
	case "$code" in
	201)
		echo "ok"
		sent=$((sent + 1))
		;;
	409)
		echo "CONFLICT (409) -- that name already means different bytes"
		failed=$((failed + 1))
		;;
	401|403)
		echo "REJECTED ($code)"
		die "the target stopped accepting the token mid-run -- stopping before the jail counts five"
		;;
	*)
		echo "FAILED (HTTP $code): $(head -c 200 "$WORK/reply" 2>/dev/null)"
		failed=$((failed + 1))
		;;
	esac
	rm -f "$BLOB"
done < "$WORK/push.tsv"

#
# Pruning runs only after the pushes, and only if they all landed.
# Deleting last year's build before this year's has arrived would
# leave the target with neither.
#
pruned=0
if [ "$PRUNE" -eq 1 ] && [ "$prune_n" -gt 0 ]; then
	if [ "$failed" -gt 0 ]; then
		echo "seed: not pruning -- $failed upload(s) failed, and pruning now could leave a gap"
	else
		echo
		#
		# Redirected, not piped: a `... | while read` loop runs in a
		# subshell, so every count it keeps is lost when the pipe
		# closes. This one reports what the DELETEs actually did, and
		# a prune that silently reported the plan back instead of the
		# outcome would be worse than no count at all.
		#
		awk -F'\t' '$1 == "prune"' "$WORK/plan.tsv" > "$WORK/prune.tsv"
		while IFS="$(printf '\t')" read -r action name sha size artifact stem; do
			printf '  prune %s ... ' "$name"
			code=$(curl -sS --max-time 60 -o /dev/null -w '%{http_code}' \
				-X DELETE -K "$AUTH" "$TO/$name" || echo 000)
			case "$code" in
			204|404)
				echo "ok"
				pruned=$((pruned + 1))
				;;
			*)
				echo "FAILED (HTTP $code)"
				failed=$((failed + 1))
				;;
			esac
		done < "$WORK/prune.tsv"
	fi
fi

echo
echo "seed: $sent published, $failed failed, $pruned pruned"
[ "$failed" -eq 0 ] || exit 1
