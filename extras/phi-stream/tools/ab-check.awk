# ab-check: my independent count for the lens/placebo comparison. guide.log
# lines between T0 and T1, runs of one src (one fork each in ab mode), the
# first N lines of each run; per src: forks, tokens, mean KL, flip share.
# awk -F'\t' -v T0=.. -v T1=.. -v N=128 -f ab-check.awk guide.log
BEGIN { if (N == "") N = 128 }
$1 >= T0 && $1 <= T1 {
    src = "chain"; kl = 0; flip = 0
    for (i = 2; i <= NF; i++) {
        if ($i ~ /^src=/) src = substr($i, 5)
        else if ($i ~ /^kl=/) kl = substr($i, 4) + 0
        else if ($i ~ /^flip=/) flip = substr($i, 6) + 0
    }
    if (src != last) { forks[src]++; k = 0; last = src }
    k++
    if (k <= N) { n[src]++; s[src] += kl; f[src] += flip }
}
END {
    for (x in n)
        printf "%-8s forks %4d  tokens %6d  mean KL %.4f  flips %.1f%%\n", x, forks[x], n[x], s[x] / n[x], 100 * f[x] / n[x]
}
