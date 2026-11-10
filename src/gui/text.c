/* Unicode bidi (UAX #9) and script itemisation. See text.h.
 *
 * The implementation follows the rule order of the standard: explicit
 * embeddings, then the weak types (W1-W7), the bracket and neutral
 * resolution (N0-N2), the implicit levels (I1-I2) and finally L1, which
 * returns trailing whitespace to the paragraph level. Bracket pairs are the
 * common ones a page uses; an unpaired bracket simply stays neutral. */
#include "text.h"
#include "kernel.h"

enum { L, R, AL, EN, AN, ES, ET, CS, NSM, BN, B, S, WS, ON };

static int in(uint32_t c, uint32_t lo, uint32_t hi) { return c >= lo && c <= hi; }

static int is_nsm(uint32_t c) {
    return in(c, 0x0300, 0x036F) || in(c, 0x0483, 0x0489) ||
           in(c, 0x0591, 0x05BD) || c == 0x05BF || in(c, 0x05C1, 0x05C2) ||
           in(c, 0x05C4, 0x05C5) || c == 0x05C7 ||
           in(c, 0x0610, 0x061A) || in(c, 0x064B, 0x065F) || c == 0x0670 ||
           in(c, 0x06D6, 0x06DC) || in(c, 0x06DF, 0x06E4) ||
           in(c, 0x06E7, 0x06E8) || in(c, 0x06EA, 0x06ED) ||
           c == 0x0711 || in(c, 0x0730, 0x074A) || in(c, 0x07A6, 0x07B0) ||
           in(c, 0x07EB, 0x07F3) || in(c, 0x0816, 0x0819) ||
           in(c, 0x081B, 0x0823) || in(c, 0x0825, 0x0827) ||
           in(c, 0x0829, 0x082D) || in(c, 0x0859, 0x085B) ||
           in(c, 0x0898, 0x089F) || in(c, 0x08CA, 0x08E1) ||
           in(c, 0x08E3, 0x08FF) || in(c, 0x1AB0, 0x1AFF) ||
           in(c, 0x1DC0, 0x1DFF) || in(c, 0x20D0, 0x20FF) ||
           in(c, 0xFE00, 0xFE0F) || in(c, 0xFE20, 0xFE2F) ||
           in(c, 0xE0100, 0xE01EF);
}

static int bidi_type(uint32_t c) {
    if (is_nsm(c)) return NSM;
    if (c == 0x000A || c == 0x000D || in(c, 0x001C, 0x001E) || c == 0x0085 || c == 0x2029) return B;
    if (c == 0x0009 || c == 0x000B || c == 0x000C || c == 0x001F) return S;
    if (in(c, 0x0000, 0x0008) || in(c, 0x000E, 0x001B) || in(c, 0x007F, 0x0084) ||
        in(c, 0x0086, 0x009F) || c == 0x00AD || c == 0x180E || c == 0xFEFF ||
        in(c, 0x200B, 0x200F) || in(c, 0x202A, 0x202E) || in(c, 0x2060, 0x206F) ||
        in(c, 0xFFF9, 0xFFFB)) return BN;
    if (in(c, 0x0660, 0x0669) || in(c, 0x06F0, 0x06F9) || in(c, 0x0890, 0x0891) ||
        c == 0x06DD || c == 0x08E2) return AN;
    if (in(c, 0x0030, 0x0039)) return EN;
    if (c == 0x002B || c == 0x002D || c == 0x207A || c == 0x207B || c == 0x208A ||
        c == 0x208B || c == 0x2212 || c == 0xFB29 || c == 0xFE62 || c == 0xFE63 ||
        c == 0xFF0B || c == 0xFF0D) return ES;
    if (c == 0x0023 || c == 0x0024 || c == 0x0025 || in(c, 0x00A2, 0x00A5) ||
        c == 0x00B0 || c == 0x00B1 || c == 0x058F || c == 0x0609 || c == 0x060A ||
        c == 0x066A || c == 0x09F2 || c == 0x09F3 || c == 0x09FB || c == 0x0AF1 ||
        c == 0x0BF9 || c == 0x0E3F || c == 0x17DB || in(c, 0x2030, 0x2034) ||
        in(c, 0x20A0, 0x20CF) || c == 0x212E || c == 0x2213 ||
        in(c, 0xFF03, 0xFF05) || in(c, 0xFFE0, 0xFFE6)) return ET;
    if (c == 0x002C || c == 0x002E || c == 0x002F || c == 0x003A || c == 0x00A0 ||
        c == 0x060C || c == 0x202F || c == 0x2044 || c == 0xFE50 || c == 0xFE52 ||
        c == 0xFE55 || c == 0xFF0C || c == 0xFF0E || c == 0xFF0F || c == 0xFF1A) return CS;
    if (in(c, 0x0590, 0x05FF) || in(c, 0xFB1D, 0xFB4F)) return R;
    if (in(c, 0x0600, 0x07BF) || in(c, 0x0800, 0x08FF) ||
        in(c, 0xFB50, 0xFDFF) || in(c, 0xFE70, 0xFEFE)) return AL;
    if (c == 0x0020 || c == 0x1680 || in(c, 0x2000, 0x200A) || c == 0x2028 ||
        c == 0x205F || c == 0x3000) return WS;
    if (in(c, 0x0041, 0x005A) || in(c, 0x0061, 0x007A) || c == 0x00AA || c == 0x00B5 ||
        c == 0x00BA || in(c, 0x00C0, 0x02B8) || in(c, 0x02BB, 0x02C1) ||
        in(c, 0x02D0, 0x02D1) || in(c, 0x02E0, 0x02E4) || c == 0x02EE ||
        in(c, 0x0370, 0x0373) || in(c, 0x0376, 0x0377) || in(c, 0x037A, 0x037D) ||
        c == 0x037F || c == 0x0386 || in(c, 0x0388, 0x03F5) || in(c, 0x03F7, 0x0481) ||
        in(c, 0x048A, 0x052F) || in(c, 0x0531, 0x0556) || in(c, 0x0561, 0x0587) ||
        in(c, 0x0900, 0x1FFF) || in(c, 0x2C00, 0x2C7F) || in(c, 0x2D00, 0x2D2D) ||
        in(c, 0x3040, 0x30FF) || in(c, 0x3400, 0x4DBF) || in(c, 0x4E00, 0x9FFF) ||
        in(c, 0xA640, 0xA69F) || in(c, 0xAC00, 0xD7A3) || in(c, 0xF900, 0xFAFF) ||
        in(c, 0x1E00, 0x1EFF) || in(c, 0x2C60, 0x2C7F) || in(c, 0xA720, 0xA7FF))
        return L;
    return ON;
}

static uint32_t mirror_pair(uint32_t c) {
    static const uint32_t pairs[][2] = {
        {0x0028, 0x0029}, {0x003C, 0x003E}, {0x005B, 0x005D}, {0x007B, 0x007D},
        {0x00AB, 0x00BB}, {0x2018, 0x2019}, {0x201C, 0x201D}, {0x201E, 0x201F},
        {0x2039, 0x203A}, {0x2032, 0x2033}, {0x2035, 0x2036}, {0x2045, 0x2046},
        {0x207D, 0x207E}, {0x208D, 0x208E}, {0x2329, 0x232A}, {0x27E8, 0x27E9},
        {0x27EA, 0x27EB}, {0x27EC, 0x27ED}, {0x27EE, 0x27EF}, {0x3008, 0x3009},
        {0x300A, 0x300B}, {0x300C, 0x300D}, {0x300E, 0x300F}, {0x3010, 0x3011},
        {0x3014, 0x3015}, {0x3016, 0x3017}, {0x3018, 0x3019}, {0x301A, 0x301B},
        {0xFF08, 0xFF09}, {0xFF1C, 0xFF1E}, {0xFF3B, 0xFF3D}, {0xFF5B, 0xFF5D},
    };
    for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; i++)
        if (pairs[i][0] == c) return pairs[i][1];
    for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; i++)
        if (pairs[i][1] == c) return pairs[i][0];
    return c;
}

uint32_t text_mirror(uint32_t cp) { return mirror_pair(cp); }

uint32_t text_script(uint32_t c) {
    if (in(c, 0x0600, 0x06FF) || in(c, 0x0750, 0x077F) || in(c, 0x08A0, 0x08FF) ||
        in(c, 0xFB50, 0xFDFF) || in(c, 0xFE70, 0xFEFF) || in(c, 0x1EE00, 0x1EEFF))
        return 0x61726162u;                                  /* 'arab' */
    if (in(c, 0x0590, 0x05FF) || in(c, 0xFB1D, 0xFB4F)) return 0x68656272u;  /* 'hebr' */
    if (in(c, 0x0370, 0x03FF) || in(c, 0x1F00, 0x1FFF)) return 0x6772656Bu;  /* 'grek' */
    if (in(c, 0x0400, 0x052F) || in(c, 0x2DE0, 0x2DFF) || in(c, 0xA640, 0xA69F))
        return 0x6379726Cu;                                  /* 'cyrl' */
    if (in(c, 0x0041, 0x005A) || in(c, 0x0061, 0x007A) ||
        in(c, 0x00C0, 0x02AF) || in(c, 0x1E00, 0x1EFF) ||
        in(c, 0x2C60, 0x2C7F) || in(c, 0xA720, 0xA7FF) || in(c, 0xFB00, 0xFB06))
        return 0x6C61746Eu;                                  /* 'latn' */
    return 0;
}

int text_bidi_base(const uint32_t *cps, int n) {
    for (int i = 0; i < n; i++) {
        int t = bidi_type(cps[i]);
        if (t == L) return 0;
        if (t == R || t == AL) return 1;
    }
    return 0;
}

static int dir_of(uint8_t t) {
    return t == L ? L : (t == R || t == AL || t == EN || t == AN) ? R : -1;
}

/* A second pass with the code points available: N0 needs to know which
 * character is a bracket, not just that it is neutral. */
static void bracket_pass(const uint32_t *cps, uint8_t *t, int n) {
    int stack_i[64]; uint32_t stack_c[64];
    int sp = 0;
    for (int i = 0; i < n; i++) {
        if (t[i] != ON) continue;
        uint32_t m = mirror_pair(cps[i]);
        if (m == cps[i]) continue;
        int is_open = cps[i] == 0x0028 || cps[i] == 0x003C || cps[i] == 0x005B ||
                      cps[i] == 0x007B || cps[i] == 0x00AB || cps[i] == 0x2018 ||
                      cps[i] == 0x201C || cps[i] == 0x201E || cps[i] == 0x2039 ||
                      cps[i] == 0x2032 || cps[i] == 0x2035 || cps[i] == 0x2045 ||
                      cps[i] == 0x207D || cps[i] == 0x208D || cps[i] == 0x2329 ||
                      cps[i] == 0x27E8 || cps[i] == 0x27EA || cps[i] == 0x27EC ||
                      cps[i] == 0x27EE || cps[i] == 0x3008 || cps[i] == 0x300A ||
                      cps[i] == 0x300C || cps[i] == 0x300E || cps[i] == 0x3010 ||
                      cps[i] == 0x3014 || cps[i] == 0x3016 || cps[i] == 0x3018 ||
                      cps[i] == 0x301A || cps[i] == 0xFF08 || cps[i] == 0xFF1C ||
                      cps[i] == 0xFF3B || cps[i] == 0xFF5B;
        if (is_open) {
            if (sp < 64) { stack_i[sp] = i; stack_c[sp] = m; sp++; }
            continue;
        }
        /* a closing bracket: find the nearest matching open */
        for (int k = sp - 1; k >= 0; k--) {
            if (stack_c[k] != cps[i]) continue;
            int a = stack_i[k];
            int lc = 0, rc = 0;
            for (int q = a + 1; q < i; q++) {
                if (t[q] == L) lc = 1;
                else if (t[q] == R || t[q] == AL || t[q] == EN || t[q] == AN) rc = 1;
            }
            if (lc != rc) t[a] = t[i] = lc ? L : R;
            sp = k;
            break;
        }
    }
}

void text_bidi_levels(const uint32_t *cps, int n, uint8_t *levels, int base) {
    if (!cps || !levels || n <= 0) return;
    if (base < 0) base = text_bidi_base(cps, n);
    uint8_t *t = (uint8_t *)kmalloc((size_t)n * 2);
    if (!t) return;
    uint8_t *orig = t + n;
    for (int i = 0; i < n; i++) { t[i] = orig[i] = (uint8_t)bidi_type(cps[i]); }

    /* X1-X8: explicit levels and overrides */
    int stack_l[64]; uint8_t stack_o[64];
    int sp = 0, level = base; uint8_t over = 0;
    for (int i = 0; i < n; i++) {
        uint32_t cp = cps[i];
        if (cp == 0x202A || cp == 0x202B || cp == 0x202D || cp == 0x202E) {
            levels[i] = (uint8_t)level;
            t[i] = BN;
            int even = (cp == 0x202A || cp == 0x202D) ? 1 : 0;
            int nl = even ? (level + 2) & ~1 : (level + 1) | 1;
            if (nl > 125) nl = level;
            if (sp < 63) { stack_l[sp] = level; stack_o[sp] = over; sp++; }
            level = nl;
            over = cp == 0x202D ? L : cp == 0x202E ? R : 0;
        } else if (cp == 0x202C) {
            levels[i] = (uint8_t)level;
            t[i] = BN;
            if (sp) { sp--; level = stack_l[sp]; over = stack_o[sp]; }
        } else if (cp == 0x2066 || cp == 0x2067 || cp == 0x2068 || cp == 0x2069) {
            levels[i] = (uint8_t)level;
            t[i] = BN;
        } else {
            levels[i] = (uint8_t)level;
            if (over) t[i] = over;
        }
    }

    /* W1: a combining mark takes the type of what it sits on */
    int prev = -1;
    for (int i = 0; i < n; i++) {
        if (t[i] == BN) continue;
        if (t[i] == NSM) t[i] = (uint8_t)(prev >= 0 ? t[prev] : ((levels[i] & 1) ? R : L));
        prev = i;
    }
    /* W2: European digits after Arabic letters are Arabic numbers */
    int strong = (base & 1) ? R : L;
    for (int i = 0; i < n; i++) {
        if (t[i] == BN) continue;
        if (t[i] == EN && strong == AL) t[i] = AN;
        if (t[i] == L || t[i] == R || t[i] == AL) strong = t[i];
    }
    /* W3 */
    for (int i = 0; i < n; i++) if (t[i] == AL) t[i] = R;
    /* W4 */
    for (int i = 1; i + 1 < n; i++) {
        if (t[i] == ES && t[i - 1] == EN && t[i + 1] == EN) t[i] = EN;
        else if (t[i] == CS && ((t[i - 1] == EN && t[i + 1] == EN) ||
                               (t[i - 1] == AN && t[i + 1] == AN))) t[i] = t[i - 1];
    }
    /* W5: a run of terminators next to a number is part of the number */
    for (int i = 0; i < n; i++) {
        if (t[i] != ET) continue;
        int a = i, b = i;
        while (a > 0 && t[a - 1] == ET) a--;
        while (b + 1 < n && t[b + 1] == ET) b++;
        int near = (a > 0 && t[a - 1] == EN) || (b + 1 < n && t[b + 1] == EN);
        if (near) for (int q = a; q <= b; q++) t[q] = EN;
        i = b;
    }
    /* W6 */
    for (int i = 0; i < n; i++) if (t[i] == ES || t[i] == ET || t[i] == CS) t[i] = ON;
    /* W7 */
    strong = (base & 1) ? R : L;
    for (int i = 0; i < n; i++) {
        if (t[i] == BN) continue;
        if (t[i] == EN && strong == L) t[i] = L;
        if (t[i] == L || t[i] == R) strong = t[i];
    }

    bracket_pass(cps, t, n);

    /* N1/N2: neutrals take the direction of their surroundings */
    for (int i = 0; i < n; ) {
        if (t[i] != ON && t[i] != WS && t[i] != B && t[i] != S) { i++; continue; }
        int a = i, b = i;
        while (b + 1 < n && (t[b + 1] == ON || t[b + 1] == WS || t[b + 1] == B || t[b + 1] == S)) b++;
        int before = (a == 0) ? ((base & 1) ? R : L) : dir_of(t[a - 1]);
        int after = (b + 1 >= n) ? ((base & 1) ? R : L) : dir_of(t[b + 1]);
        int d = (before >= 0 && before == after) ? before : ((levels[a] & 1) ? R : L);
        for (int q = a; q <= b; q++) t[q] = (uint8_t)d;
        i = b + 1;
    }

    /* I1/I2: implicit levels */
    for (int i = 0; i < n; i++) {
        if (levels[i] & 1) {
            if (t[i] == L || t[i] == EN || t[i] == AN) levels[i]++;
        } else {
            if (t[i] == R) levels[i]++;
            else if (t[i] == EN || t[i] == AN) levels[i] += 2;
        }
    }

    /* L1: separators and trailing whitespace go back to the paragraph level */
    int trailing = 1;
    for (int i = n - 1; i >= 0; i--) {
        if (orig[i] == B || orig[i] == S) { levels[i] = (uint8_t)base; trailing = 1; continue; }
        if (trailing && (orig[i] == WS || orig[i] == BN)) { levels[i] = (uint8_t)base; continue; }
        trailing = 0;
    }
    kfree(t);
}
