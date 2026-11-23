# one-off edit: old Windows crypto (CryptoAPI) for 32-bit programs: MD5 / SHA-1 hashes and RC4 keys made from a hash
# (Dragon Dance / NESiCA: MD5 -> CryptDeriveKey(RC4, no salt) -> CryptEncrypt/Decrypt). The Xbox has no advapi32 Crypt*.
from pathlib import Path

p = Path(r"C:\Users\GamerDad\XboxEmu\ArcadeLoader\src\peload32.cpp")
s = p.read_text(encoding="utf-8")
code = r'''// --- CryptoAPI for 32-bit programs (the Xbox has none): MD5 and SHA-1 hashes, RC4 keys derived from a hash, as the
// Microsoft providers make them (Enhanced / Strong: 128-bit RC4 by default; Base: 40-bit, salted with 11 zero bytes
// unless CRYPT_NO_SALT). Handles are small numbers below 2 GB.
struct Md5 {
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint64_t len = 0;
    uint8_t buf[64];
    static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8,
            0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
            0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87,
            0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
            0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039,
            0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
            0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
            0xeb86d391};
        static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9,
                                  14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t m[16];
        for (int i = 0; i < 16; i++) m[i] = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | uint32_t(p[i * 4 + 3]) << 24;
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f;
            int g;
            if (i < 16) f = (b & c) | (~b & d), g = i;
            else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
            else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
            else f = c ^ (b | ~d), g = (7 * i) % 16;
            uint32_t t = d;
            d = c, c = b;
            b = b + rol(a + f + K[i] + m[g], R[i]);
            a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d;
    }
    void add(const uint8_t* p, size_t n) {
        while (n) {
            size_t at = len % 64, k = std::min<size_t>(64 - at, n);
            memcpy(buf + at, p, k);
            len += k, p += k, n -= k;
            if ((len % 64) == 0) block(buf);
        }
    }
    void done(uint8_t out[16]) {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80, z = 0;
        add(&pad, 1);
        while (len % 64 != 56) add(&z, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; i++) l[i] = uint8_t(bits >> (8 * i));
        add(l, 8);
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 4; k++) out[i * 4 + k] = uint8_t(h[i] >> (8 * k));
    }
};
struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint64_t len = 0;
    uint8_t buf[64];
    static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(p[i * 4]) << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    void add(const uint8_t* p, size_t n) {
        while (n) {
            size_t at = len % 64, k = std::min<size_t>(64 - at, n);
            memcpy(buf + at, p, k);
            len += k, p += k, n -= k;
            if ((len % 64) == 0) block(buf);
        }
    }
    void done(uint8_t out[20]) {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80, z = 0;
        add(&pad, 1);
        while (len % 64 != 56) add(&z, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; i++) l[i] = uint8_t(bits >> (56 - 8 * i));
        add(l, 8);
        for (int i = 0; i < 5; i++)
            for (int k = 0; k < 4; k++) out[i * 4 + k] = uint8_t(h[i] >> (24 - 8 * k));
    }
};
struct CryptObj {
    int kind = 0;   // 1 provider, 2 hash, 3 key
    bool enhanced = true;
    uint32_t alg = 0, prov = 0;
    Md5 md5;
    Sha1 sha1;
    bool finished = false;
    uint8_t value[20] = {};
    std::vector<uint8_t> key;   // RC4 key
    uint8_t S[256];
    int i = 0, j = 0;
    void rc4Init() {
        for (int k = 0; k < 256; k++) S[k] = uint8_t(k);
        for (int k = 0, m = 0; k < 256; k++) {
            m = (m + S[k] + key[k % key.size()]) & 255;
            std::swap(S[k], S[m]);
        }
        i = j = 0;
    }
    void rc4(uint8_t* p, size_t n) {
        for (size_t k = 0; k < n; k++) {
            i = (i + 1) & 255;
            j = (j + S[i]) & 255;
            std::swap(S[i], S[j]);
            p[k] ^= S[(S[i] + S[j]) & 255];
        }
    }
    size_t hashSize() const { return alg == 0x8004 ? 20 : 16; }
    void finish() {
        if (finished) return;
        finished = true;
        if (alg == 0x8004) sha1.done(value);
        else md5.done(value);
    }
};
static std::map<uint32_t, CryptObj> g_crypt;
static uint32_t g_cryptNext = 0x7E000000;
static std::mutex g_cryptLock;
static CryptObj* CryptGet(uint64_t h, int kind) {
    auto it = g_crypt.find((uint32_t)h);
    return it != g_crypt.end() && it->second.kind == kind ? &it->second : nullptr;
}
static uint32_t CryptNew(CryptObj o) {
    g_cryptNext += 4;
    g_crypt[g_cryptNext] = std::move(o);
    return g_cryptNext;
}
#define C32(p) ((uint32_t*)(uintptr_t)(uint32_t)(p))
static uint64_t CryptAcquire(uint64_t out, const std::string& provider) {
    std::lock_guard<std::mutex> l(g_cryptLock);
    CryptObj o;
    o.kind = 1;
    o.enhanced = provider.empty() || provider.find("Base") == std::string::npos;
    if (out) *C32(out) = CryptNew(o);
    static int told = 0;
    if (!told++) Log("32-bit crypto: provider \"%s\" (MD5, SHA-1, RC4 here)", provider.c_str());
    return 1;
}
static uint64_t B_CryptAcquireContextA(uint64_t out, uint64_t, uint64_t prov, uint64_t, uint64_t) { return CryptAcquire(out, RegA(prov)); }
static uint64_t B_CryptAcquireContextW(uint64_t out, uint64_t, uint64_t prov, uint64_t, uint64_t) { return CryptAcquire(out, RegW(prov)); }
static uint64_t B_CryptReleaseContext(uint64_t h, uint64_t) {
    std::lock_guard<std::mutex> l(g_cryptLock);
    return g_crypt.erase((uint32_t)h) ? 1 : 1;
}
static uint64_t B_CryptCreateHash(uint64_t prov, uint64_t alg, uint64_t, uint64_t, uint64_t out) {
    std::lock_guard<std::mutex> l(g_cryptLock);
    CryptObj* p = CryptGet(prov, 1);
    if (!p || (alg != 0x8003 && alg != 0x8004)) {
        Log("32-bit crypto: hash 0x%x not made here", (unsigned)alg);
        SetLastError(NTE_BAD_ALGID);
        return 0;
    }
    CryptObj o;
    o.kind = 2, o.alg = (uint32_t)alg, o.prov = (uint32_t)prov, o.enhanced = p->enhanced;
    *C32(out) = CryptNew(o);
    return 1;
}
static uint64_t B_CryptHashData(uint64_t h, uint64_t data, uint64_t n, uint64_t) {
    std::lock_guard<std::mutex> l(g_cryptLock);
    CryptObj* o = CryptGet(h, 2);
    if (!o || o->finished) return 0;
    if (o->alg == 0x8004) o->sha1.add((const uint8_t*)(uintptr_t)(uint32_t)data, (uint32_t)n);
    else o->md5.add((const uint8_t*)(uintptr_t)(uint32_t)data, (uint32_t)n);
    return 1;
}
static uint64_t B_CryptGetHashParam(uint64_t h, uint64_t what, uint64_t data, uint64_t size, uint64_t) {
    std::lock_guard<std::mutex> l(g_cryptLock);
    CryptObj* o = CryptGet(h, 2);
    if (!o || !size) return 0;
    uint32_t have = *C32(size);
    if (what == 4) {   // HP_HASHSIZE
        *C32(size) = 4;
        if (data && have >= 4) *C32(data) = (uint32_t)o->hashSize();
        return 1;
    }
    if (what == 1) {   // HP_ALGID
        *C32(size) = 4;
        if (data && have >= 4) *C32(data) = o->alg;
        return 1;
    }
    if (what != 2) return 0;   // HP_HASHVAL
    o->finish();
    *C32(size) = (uint32_t)o->hashSize();
    if (!data) return 1;
    if (have < o->hashSize()) { SetLastError(ERROR_MORE_DATA); return 0; }
    memcpy((void*)(uintptr_t)(uint32_t)data, o->value, o->hashSize());
    return 1;
}
static uint64_t B_CryptDeriveKey(uint64_t prov, uint64_t alg, uint64_t hash, uint64_t flags, uint64_t out) {
    std::lock_guard<std::mutex> l(g_cryptLock);
    CryptObj* hp = CryptGet(hash, 2);
    if (!hp || alg != 0x6801) {   // CALG_RC4 only
        Log("32-bit crypto: key 0x%x not made here", (unsigned)alg);
        SetLastError(NTE_BAD_ALGID);
        return 0;
    }
    hp->finish();
    uint32_t bits = uint32_t(flags) >> 16;
    if (!bits) bits = hp->enhanced ? 128 : 40;
    size_t n = std::min<size_t>(bits / 8, hp->hashSize());
    CryptObj o;
    o.kind = 3, o.alg = 0x6801;
    o.key.assign(hp->value, hp->value + n);
    if (!(flags & 0x10) && n < 16) o.key.resize(16, 0);   // the old providers' salt: zero bytes up to 128 bits
    o.rc4Init();
    *C32(out) = CryptNew(o);
    return 1;
}
static uint64_t CryptRun(uint64_t k, uint64_t final_, uint64_t data, uint64_t len) {
    std::lock_guard<std::mutex> l(g_cryptLock);
    CryptObj* o = CryptGet(k, 3);
    if (!o) return 0;
    if (data && len) o->rc4((uint8_t*)(uintptr_t)(uint32_t)data, *C32(len));
    if (final_) o->rc4Init();   // a final block: the key starts over for the next message
    return 1;
}
static uint64_t B_CryptEncrypt(uint64_t k, uint64_t, uint64_t fin, uint64_t, uint64_t data, uint64_t len, uint64_t) {
    return CryptRun(k, fin, data, len);
}
static uint64_t B_CryptDecrypt(uint64_t k, uint64_t, uint64_t fin, uint64_t, uint64_t data, uint64_t len) {
    return CryptRun(k, fin, data, len);
}
static uint64_t B_CryptDestroyHash(uint64_t h) {
    std::lock_guard<std::mutex> l(g_cryptLock);
    g_crypt.erase((uint32_t)h);
    return 1;
}
static uint64_t B_CryptDestroyKey(uint64_t h) { return B_CryptDestroyHash(h); }
'''
anchor = "static uint64_t B_GlobalMemoryStatus(uint64_t out) {"
assert s.count(anchor) == 1 and "B_CryptAcquireContextA" not in s
s = s.replace(anchor, code + anchor)
row = '''    {"kernel32.dll", "GlobalMemoryStatus", 1, false, (void*)B_GlobalMemoryStatus},'''
s = s.replace(row, row + '''
    {"advapi32.dll", "CryptAcquireContextA", 5, false, (void*)B_CryptAcquireContextA},
    {"advapi32.dll", "CryptAcquireContextW", 5, false, (void*)B_CryptAcquireContextW},
    {"advapi32.dll", "CryptReleaseContext", 2, false, (void*)B_CryptReleaseContext},
    {"advapi32.dll", "CryptCreateHash", 5, false, (void*)B_CryptCreateHash},
    {"advapi32.dll", "CryptHashData", 4, false, (void*)B_CryptHashData},
    {"advapi32.dll", "CryptGetHashParam", 5, false, (void*)B_CryptGetHashParam},
    {"advapi32.dll", "CryptDeriveKey", 5, false, (void*)B_CryptDeriveKey},
    {"advapi32.dll", "CryptEncrypt", 7, false, (void*)B_CryptEncrypt},
    {"advapi32.dll", "CryptDecrypt", 6, false, (void*)B_CryptDecrypt},
    {"advapi32.dll", "CryptDestroyHash", 1, false, (void*)B_CryptDestroyHash},
    {"advapi32.dll", "CryptDestroyKey", 1, false, (void*)B_CryptDestroyKey},''')
p.write_text(s, encoding="utf-8")
print("ok")
