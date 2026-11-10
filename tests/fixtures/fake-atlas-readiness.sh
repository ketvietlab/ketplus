#!/bin/sh
while [ "$#" -gt 0 ]; do
    if [ "$1" = --port ]; then shift; port="$1"; fi
    shift
done
printf '%s' "$port" > attempted-port
case @MODE@ in
    stop-banner)
        trap 'printf "KetAtlas: http://127.0.0.1:%s\n" "$port"; exit 0' TERM
        printf armed > stop-ready
        while :; do sleep 0.1; done;;
    wrong-port) printf 'KetAtlas: http://127.0.0.1:1\n'; exit 0;;
    invalid-port) printf 'KetAtlas: http://127.0.0.1:65536\n'; exit 0;;
    overflow-port) printf 'KetAtlas: http://127.0.0.1:999999999999999999999999\n'; exit 0;;
    embedded) printf 'unrelated log: KetAtlas: http://127.0.0.1:%s\n' "$port"; exit 0;;
    truncated-prefix)
        banner="KetAtlas: http://127.0.0.1:$port"
        printf 'unrelated log: %s\n' "$banner"
        head -c "$((32768 - ${#banner} - 1))" /dev/zero | tr '\000' x
        sleep 0.2
        exit 0;;
    partial-longer-port)
        printf 'KetAtlas: http://127.0.0.1:%s' "$port"
        sleep 0.2
        printf '0\n'
        exit 0;;
    wrong-then-right) printf 'KetAtlas: http://127.0.0.1:1\n';;
    split-valid)
        printf 'KetAtlas: http://127.0.0.1:'
        sleep 0.2
        printf '%s\n' "$port"
        exec sleep 30;;
    crlf) printf 'KetAtlas: http://127.0.0.1:%s\r\n' "$port"; exec sleep 30;;
    slash) printf 'KetAtlas: http://127.0.0.1:%s/\n' "$port"; exec sleep 30;;
esac
printf 'KetAtlas: http://127.0.0.1:%s\n' "$port"
exec sleep 30
