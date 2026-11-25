/*
 * fpu_model_csv.h — 期待値 CSV の probe（key=value を ; で区切った文字列）の解析と応答の照合。
 *  の CSV を読む試験（test_fpu_model_transition.c、test_fpu_model_restore.c、
 * test_fpu_model_protocol.c、test_fpu_model_abort_window.c）が共有する。
 */
#pragma once
#include "fpu_model_util.h"

/* , 区切りの許可リストに key（長さ klen）があるか。strtok を使わないので、probe を strtok
 * で走査中の 呼出しからも使える（入れ子の strtok は外側の状態を壊す） */
static inline int csv_list_has(const char *list, const char *key, size_t klen) {
    const char *p = list;

    while (*p) {
        size_t n = strcspn(p, ",");

        if (n == klen && !strncmp(p, key, klen))
            return 1;
        p += n;
        if (*p == ',')
            p++;
    }
    return 0;
}

/* CSV の欄を固定長のバッファへ写す。入り切らない欄は異常終了する（strncpy の切り詰めで、
 * "invalidX" が "invalid" として許可リストを通るのを防ぐ） */
static inline void csv_copy(char *dst, size_t n, const char *src) {
    if (strlen(src) >= n) {
        fprintf(stderr, "CSV の欄が長すぎる（%zu 文字以上、上限 %zu）: %s\n", n, n - 1, src);
        exit(1);
    }
    strcpy(dst, src);
}

/* CSV の整数の欄（10進の非負整数だけ）を読む。数字以外を含む文字列（"5x"・" 1"・"+1"・"-1"
 * など）と 空文字列、int に入り切らない値は異常終了する（atoi が黙って読み捨てるのを防ぐ） */
static inline int csv_int(const char *what, const char *s) {
    char *end;
    long v;

    if (!*s || strspn(s, "0123456789") != strlen(s) || strlen(s) > 9) {
        fprintf(stderr, "CSV の整数でない値: %s=%s\n", what, s);
        exit(1);
    }
    v = strtol(s, &end, 10);
    return (int)v;
}

#define PROBE_BUF 192

/* probe を strtok 用のバッファへ写す。入り切らなければ異常終了する */
static inline void probe_copy(char *buf, const char *probe) {
    csv_copy(buf, PROBE_BUF, probe);
}

static inline int probe_get(const char *probe, const char *key, char *out, size_t n) {
    char buf[PROBE_BUF];
    char *tok;
    size_t klen = strlen(key);

    probe_copy(buf, probe);
    for (tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
        if (!strncmp(tok, key, klen) && tok[klen] == '=') {
            csv_copy(out, n, tok + klen + 1);
            return 1;
        }
    }
    return 0;
}

/* probe がキーを持つか（値は見ない） */
static inline int probe_has(const char *probe, const char *key) {
    char tmp[PROBE_BUF];

    return probe_get(probe, key, tmp, sizeof(tmp));
}

/* 数値の値を読む。数字でない文字列は異常終了する（strtol が黙って 0 にするのを防ぐ）。
 * key が "img" のときだけ16進、他は10進（どちらも符号・空白を許さず、4桁以内。img は 0xFFFF
 * まで） */
static inline int probe_int(const char *probe, const char *key, int *v) {
    char s[32];
    char *end;
    int hex = strcmp(key, "img") == 0;

    if (!probe_get(probe, key, s, sizeof(s)))
        return 0;
    if (!*s || strspn(s, hex ? "0123456789abcdefABCDEF" : "0123456789") != strlen(s) ||
        strlen(s) > 4) {
        fprintf(stderr, "probe の値が数値でない: %s=%s（%s）\n", key, s, probe);
        exit(1);
    }
    *v = (int)strtol(s, &end, hex ? 16 : 10);
    if (end == s || *end) {
        fprintf(stderr, "probe の値が数値でない: %s=%s（%s）\n", key, s, probe);
        exit(1);
    }
    return 1;
}

/* probe のキーを検査する。probe 全体が "-"（観測なし）なら何も検査しない。そうでなければ:
 *  - キーが許可リスト（, 区切り）に無ければ打ち間違いとして異常終了する
 *  - 同じキーが2回出れば異常終了する（probe_get
 * は最初の一致だけを返すので、2つ目は死んだ欄になる）
 *  - キー無しの語は、head_ok のとき先頭（応答の種類）だけ許し、他は異常終了する
 * then の値は対象外（then の中身は応答 probe として resp_mismatch が検査する）。許可リストは
 * 行の種類（event・frame・応答の種類）ごとに絞って渡すこと */
static inline void probe_check_keys(const char *probe, const char *allowed, int head_ok) {
    const char *p = probe;
    int first = 1;

    if (!strcmp(probe, "-"))
        return;
    if (!*p || probe[strlen(probe) - 1] == ';') {
        fprintf(stderr, "probe が空か、末尾が ; で終わる（%s）\n", probe);
        exit(1);
    }
    while (*p) {
        size_t n = strcspn(p, ";");
        const char *eq = memchr(p, '=', n);

        if (eq) {
            size_t klen = (size_t)(eq - p);
            const char *q;

            if (!csv_list_has(allowed, p, klen)) {
                fprintf(stderr, "probe の未知のキー: %.*s（%s）\n", (int)klen, p, probe);
                exit(1);
            }
            for (q = probe; q < p;) {
                size_t qn = strcspn(q, ";");

                if (qn > klen && !strncmp(q, p, klen) && q[klen] == '=') {
                    fprintf(stderr, "probe のキーが重複: %.*s（%s）\n", (int)klen, p, probe);
                    exit(1);
                }
                q += qn;
                if (*q == ';')
                    q++;
            }
        } else if (!(first && head_ok)) {
            fprintf(stderr, "probe にキー無しの語がある（%s）: %.*s（%s）\n",
                    first ? "先頭は許可されない" : "先頭以外", (int)n, p, probe);
            exit(1);
        }
        first = 0;
        p += n;
        if (*p == ';')
            p++;
    }
}

/* probe が必須キー（, 区切り）を全て持たなければ異常終了する。キーが消えると、その観測が黙って
 * 未検査になるのを防ぐ */
static inline void probe_require_keys(const char *probe, const char *required) {
    const char *p = required;

    while (*p) {
        size_t n = strcspn(p, ",");
        char key[24];
        char tmp[PROBE_BUF];

        if (n >= sizeof(key)) {
            fprintf(stderr, "必須キーが長すぎる: %s\n", required);
            exit(1);
        }
        memcpy(key, p, n);
        key[n] = 0;
        if (!probe_get(probe, key, tmp, sizeof(tmp))) {
            fprintf(stderr, "probe に必須キーが無い: %s（%s）\n", key, probe);
            exit(1);
        }
        p += n;
        if (*p == ',')
            p++;
    }
}

/* CSV の列の値が許可リスト（, 区切り）に無ければ異常終了する。runner が未知の値を既定の分岐へ
 * 流して通すのを防ぐ（変異や打ち間違いで値が変わると、読む側が黙って別の分岐を取るため）。
 * 許可リストは列全体の和集合でなく、行の種類（event・frame）ごとに絞って渡すこと */
static inline void csv_check_one_of(const char *what, const char *value, const char *allowed) {
    if (csv_list_has(allowed, value, strlen(value)))
        return;
    fprintf(stderr, "CSV の未知の値: %s=%s（許可: %s）\n", what, value, allowed);
    exit(1);
}

/* 応答の種類ごとの probe のキー。RESP_KEYS_* は許可かつ必須（`probe_check_keys()` と
 * `probe_require_keys()` に同じ文字列を渡す）。ca・pc・ia・proc・tf は NULL の観測、pc・vec は
 * TAKE の 観測、dr・len は転送の観測。EA_TRANSFER・FP_REGISTERS・MP_REGISTER は
 * ca（コプロセッサが active）も 必須。FP_REGISTERS は fpu2_take_transition.csv
 * に行があり（MP_REGISTER の行は無く、ca は test_fpu_model_dialog.c の `check_response_kind()`
 * で検査する）、CA が常に立つ根拠は [881UM] §7.4.2.3・ §7.4.2.4（印字 p.7-15）。MP_REGISTER の
 * FMOVE.P の CA は Table 7-7 の $8C0x・$CC0x（p.7-20〜21） */
#define RESP_KEYS_NULL      "ca,pc,ia,proc,tf"
#define RESP_KEYS_TAKE      "pc,vec"
#define RESP_KEYS_EA        "ca,pc,dr,len"
#define RESP_KEYS_REGISTERS "ca,pc,dr,len"

static inline const char *kind_name(mc68k_fpu_response_kind_t k) {
    switch (k) {
    case MC68K_FPU_RESPONSE_NULL:
        return "NULL";
    case MC68K_FPU_RESPONSE_EA_TRANSFER:
        return "EA_TRANSFER";
    case MC68K_FPU_RESPONSE_MP_REGISTER:
        return "MP_REGISTER";
    case MC68K_FPU_RESPONSE_FP_REGISTERS:
        return "FP_REGISTERS";
    case MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION:
        return "TAKE_PRE";
    case MC68K_FPU_RESPONSE_TAKE_MID_EXCEPTION:
        return "TAKE_MID";
    }
    return "未知の応答の種類"; /* どの probe の語とも一致せず、照合で落ちる */
}

/* response probe（"NULL;ca=0;pc=0;tf=1"、"TAKE_PRE;vec=50;pc=0" など）と応答を照合する。
 * 食い違えば理由を返す（NULL なら一致） */
static inline const char *resp_mismatch(const char *probe, mc68k_fpu_response_t resp) {
    char head[24];

    size_t n = strcspn(probe, ";");
    int v;

    CHECK(n < sizeof(head));
    memcpy(head, probe, n);
    head[n] = 0;
    if (!strcmp(head, "NULL")) {
        probe_check_keys(probe, RESP_KEYS_NULL, 1);
        probe_require_keys(probe, RESP_KEYS_NULL);
    } else if (!strcmp(head, "TAKE_PRE") || !strcmp(head, "TAKE_MID")) {
        probe_check_keys(probe, RESP_KEYS_TAKE, 1);
        probe_require_keys(probe, RESP_KEYS_TAKE);
    } else if (!strcmp(head, "EA_TRANSFER")) {
        probe_check_keys(probe, RESP_KEYS_EA, 1);
        probe_require_keys(probe, RESP_KEYS_EA);
    } else if (!strcmp(head, "MP_REGISTER") || !strcmp(head, "FP_REGISTERS")) {
        probe_check_keys(probe, RESP_KEYS_REGISTERS, 1);
        probe_require_keys(probe, RESP_KEYS_REGISTERS);
    } else {
        fprintf(stderr, "応答 probe の種類が未知: %s（%s）\n", head, probe);
        exit(1);
    }
    if (strcmp(head, kind_name(resp.kind)))
        return "応答の種類";
    if (probe_int(probe, "ca", &v) && v != resp.coprocessor_active)
        return "CA";
    if (probe_int(probe, "pc", &v) && v != resp.request_pc)
        return "PC";
    if (probe_int(probe, "ia", &v) && v != resp.interruptible)
        return "IA";
    if (probe_int(probe, "proc", &v) && v != resp.processing)
        return "PF（processing=!PF）";
    if (probe_int(probe, "tf", &v) && v != resp.condition_true)
        return "TF";
    if (probe_int(probe, "vec", &v) && v != resp.vector)
        return "vector";
    if (probe_int(probe, "dr", &v) && v != resp.transfer_to_host)
        return "DR";
    if (probe_int(probe, "len", &v) && v != resp.length)
        return "length";
    return NULL;
}

/* "then=NULL:ca=0:..." の then の値を、応答 probe の形（: を ; に）にして返す */
static inline int probe_then(const char *probe, char *out, size_t n) {
    size_t k;

    if (!probe_get(probe, "then", out, n))
        return 0;
    for (k = 0; out[k]; k++)
        if (out[k] == ':')
            out[k] = ';';
    return 1;
}

/* カンマ区切りの1行を n 個の欄に分ける（最後の欄は残り全部）。line は破壊される */
static inline void split_csv(char *line, char **tok, unsigned n) {
    unsigned i;
    char *p = line;

    line[strcspn(line, "\r\n")] = 0;
    for (i = 0; i < n; i++) {
        tok[i] = p;
        p = strchr(p, ',');
        if (i + 1 < n) {
            CHECK(p != NULL);
            *p++ = 0;
        }
    }
}
