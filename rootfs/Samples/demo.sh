#!/bin/sh
# Lumina OS shell script demo - runs on the built-in POSIX-style shell
echo -e "\e[96m=== Lumina shell script demo ===\e[0m"
echo "Running on: $(uname -a)"
echo "Today is $(date +%Y-%m-%d), user is $USER"

greet() {
    echo "Hello, $1! You passed $# argument(s)."
}
greet "World"

total=0
for i in $(seq 1 10); do
    total=$((total + i))
done
echo "Sum of 1..10 = $total"

count=3
while [ $count -gt 0 ]; do
    echo "Countdown: $count"
    count=$((count - 1))
    sleep 0.3
done

for f in C:/Samples/*; do
    case "$f" in
        *.exe) kind="Win32 program" ;;
        *.sh)  kind="shell script" ;;
        *.dmg) kind="Apple disk image" ;;
        *)     kind="file" ;;
    esac
    echo "  $(basename "$f") -> $kind"
done

echo "Writing a report to C:/tmp/report.txt ..."
echo "Report generated $(date)" > C:/tmp/report.txt
ps >> C:/tmp/report.txt
echo "Report has $(wc -l < C:/tmp/report.txt) lines."
if grep -q System C:/tmp/report.txt 2>/dev/null || grep System C:/tmp/report.txt > /dev/null; then
    echo -e "\e[92mAll checks passed.\e[0m"
fi
