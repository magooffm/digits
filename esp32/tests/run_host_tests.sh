#!/usr/bin/env bash
# Run every ESP32 host test: pure C tests and the Python harnesses that
# compile production sources against mocks. Some harnesses need ESP-IDF
# headers and the managed components from one prior `idf.py build`.
#
#   esp32/tests/run_host_tests.sh            # normal + sanitizer runs
#   esp32/tests/run_host_tests.sh --quick    # skip sanitizer runs
set -u

repo="$(cd "$(dirname "$0")/../.." && pwd)"
tests="$repo/esp32/tests"
main="$repo/esp32/main"
quick=0
[ "${1:-}" = "--quick" ] && quick=1

if [ -z "${IDF_PATH:-}" ]; then
    export IDF_PATH="${IDF_PATH_DEFAULT:-$HOME/esp/esp-idf-v5.5.3}"
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/digits-host-tests.XXXXXX")"
trap 'rm -rf "$work"' EXIT

passed=0
failed=()

run() {
    local name="$1"; shift
    if "$@" >"$work/log" 2>&1; then
        passed=$((passed + 1))
        printf 'PASS  %s\n' "$name"
    else
        failed+=("$name")
        printf 'FAIL  %s\n' "$name"
        sed 's/^/      /' "$work/log" | tail -20
    fi
}

sanitize="-fsanitize=address,undefined -fno-omit-frame-pointer"
c_sources() {
    case "$1" in
        phone_fsm) echo "$main/phone_fsm.c $tests/phone_fsm_test.c" ;;
        tone_gen) echo "$main/tone_gen.c $tests/tone_gen_test.c" ;;
        ringtones) echo "$main/ringtones.c $tests/ringtones_test.c" ;;
        ring_select) echo "$main/ring_select.c $tests/ring_select_test.c" ;;
        sd_ringtone_util) echo "$main/sd_ringtone_util.c $tests/sd_ringtone_util_test.c" ;;
        call_audio_jitter) echo "$main/call_audio_jitter.c $tests/call_audio_jitter_test.c" ;;
        ring_tone) echo "$main/ring_tone.c $tests/ring_tone_test.c" ;;
        microphone_pcm) echo "$main/microphone_pcm.c $tests/microphone_pcm_test.c" ;;
        record_playback_pcm) echo "$main/record_playback_pcm.c $tests/record_playback_pcm_test.c" ;;
        ogg_opus) echo "$main/ogg_opus.c $tests/ogg_opus_test.c" ;;
        voicemail_store) echo "$main/voicemail_store.c $tests/voicemail_store_test.c" ;;
        voicemail_prompt) echo "$main/voicemail_prompt.c $tests/voicemail_prompt_test.c" ;;
        silence_policy) echo "$tests/silence_policy_test.c" ;;
        wifi_setup_form) echo "$main/wifi_setup_form.c $tests/wifi_setup_form_test.c" ;;
        log_ring) echo "$main/log_ring.c $tests/log_ring_test.c" ;;
        greeting_pcm) echo "$main/greeting_pcm.c $main/voicemail_prompt.c $tests/greeting_pcm_test.c" ;;
        voice_style) echo "$main/voice_style.c $tests/voice_style_test.c" ;;
    esac
}

for name in phone_fsm tone_gen ringtones ring_select sd_ringtone_util call_audio_jitter ring_tone microphone_pcm record_playback_pcm \
    ogg_opus voicemail_store voicemail_prompt silence_policy wifi_setup_form log_ring greeting_pcm voice_style; do
    # shellcheck disable=SC2046 # word splitting of the source list is intended.
    run "$name" sh -c 'out="$1"; shift; cc -std=c11 -Wall -Wextra -Werror -I "$0" "$@" -lm -o "$out" && "$out"' \
        "$main" "$work/$name" $(c_sources "$name")
    if [ "$quick" = 0 ]; then
        # shellcheck disable=SC2046,SC2086
        run "$name (sanitized)" sh -c 'out="$1"; shift; cc -std=c11 -Wall -Wextra -Werror '"$sanitize"' -I "$0" "$@" -lm -o "$out" && "$out"' \
            "$main" "$work/$name-san" $(c_sources "$name")
    fi
done

for script in "$tests"/*.py; do
    name="$(basename "$script" .py)"
    args=()
    grep -q -- '--idf-path' "$script" && args+=(--idf-path "$IDF_PATH")
    run "$name" python3 "$script" "${args[@]}"
    if [ "$quick" = 0 ] && grep -q -- '--sanitize' "$script"; then
        run "$name (sanitized)" python3 "$script" "${args[@]}" --sanitize
    fi
done

printf '\n%d passed, %d failed\n' "$passed" "${#failed[@]}"
[ "${#failed[@]}" = 0 ]
