#!/bin/sh
set -eu

platform=${1:?usage: tools/sandbox/run.sh linux|windows RUN_ID [TESTDATA_DIR]}
run_id=${2:?usage: tools/sandbox/run.sh linux|windows RUN_ID [TESTDATA_DIR]}
case "$run_id" in
    ''|*[!0-9]*) echo "RUN_ID must be a numeric GitHub Actions run ID" >&2; exit 2 ;;
esac
case "$platform" in
    linux|windows) ;;
    *) echo "unknown platform: $platform" >&2; exit 2 ;;
esac

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=${SIDPORTAL_SANDBOX_OUT:-"$root/.sandbox-out"}
repo=$(gh repo view --json nameWithOwner --jq .nameWithOwner)
mkdir -p "$out"
package_artifact="SidPortal-$([ "$platform" = linux ] && echo Linux-x86_64 || echo Windows-x64)"
test_artifact="SidPortal-$([ "$platform" = linux ] && echo Linux || echo Windows)-sandbox"
artifact_dir="$out/ci-run-$run_id/$platform"
rm -rf "$artifact_dir"
mkdir -p "$artifact_dir"
gh run download "$run_id" --repo "$repo" --name "$package_artifact" --dir "$artifact_dir"
gh run download "$run_id" --repo "$repo" --name "$test_artifact" --dir "$artifact_dir"
if [ "$platform" = linux ]; then chmod u+x "$artifact_dir/sidportal_vst3_hostcheck"; fi

docker build -f "$root/tools/sandbox/Dockerfile.$platform" \
    -t "sidportal-test-$platform:local" "$root"
testdata=${3:-${SIDPORTAL_TESTDATA:-$root}}
testdata=$(CDPATH= cd -- "$testdata" && pwd)
docker run --rm --init --network none --user "$(id -u):$(id -g)" \
    --mount "type=bind,src=$artifact_dir,dst=/artifacts,readonly" \
    --mount "type=bind,src=$testdata,dst=/testdata,readonly" \
    "sidportal-test-$platform:local" "$platform"
