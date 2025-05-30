//-----------------------------------------------------------------------------
// MurmurHash3 was written by Austin Appleby, and is placed in the public
// domain. The author hereby disclaims copyright to this source code.

// Note - The x86 and x64 versions do _not_ produce the same results, as the
// algorithms are optimized for their respective platforms. You can still
// compile and run any of them on any platform, but your performance with the
// non-native version will be less than optimal.

//-----------------------------------------------------------------------------
// Platform-specific functions and macros
#include <stdlib.h>
#include <stdint.h>

static inline uint32_t rotl32(uint32_t x, int8_t r)
{
	 return (x << r) | (x >> (32 - r));
}

#define	ROTL32(x,y)	rotl32(x,y)

//-----------------------------------------------------------------------------
// Block read - if your platform needs to do endian-swapping or can only
// handle aligned reads, do the conversion here
static inline uint32_t getblock(const uint32_t * p, int i)
{
	 return p[i];
}

//-----------------------------------------------------------------------------
// Finalization mix - force all bits of a hash block to avalanche
static inline uint32_t fmix(uint32_t h)
{
	 h ^= h >> 16;
	 h *= 0x85ebca6b;
	 h ^= h >> 13;
	 h *= 0xc2b2ae35;
	 h ^= h >> 16;
	 return h;
}

//-----------------------------------------------------------------------------
uint32_t hash_murmur3(const void *key, size_t len, void *arg) {
    const uint8_t *data = (const uint8_t *)key;
    const int nblocks = len / 4;
    uint32_t h1 = 0xDEADBEEF; // Hardcoded seed value
    uint32_t c1 = 0xcc9e2d51;
    uint32_t c2 = 0x1b873593;

    // Body processing
    const uint32_t *blocks = (const uint32_t *)(data + nblocks * 4);
    for (int i = -nblocks; i; i++) {
        uint32_t k1 = getblock(blocks, i);
        k1 *= c1;
        k1 = rotl32(k1, 15);
        k1 *= c2;

        h1 ^= k1;
        h1 = rotl32(h1, 13);
        h1 = h1 * 5 + 0xe6546b64;
    }

    // Handle the tail (remaining bytes)
    const uint8_t *tail = (const uint8_t *)(data + nblocks * 4);
    uint32_t k1 = 0;

    switch (len & 3) { // Process remaining bytes
        case 3:
            k1 ^= tail[2] << 16;
            /* fall through */
        case 2:
            k1 ^= tail[1] << 8;
            /* fall through */
        case 1:
            k1 ^= tail[0];
            k1 *= c1;
            k1 = rotl32(k1, 15);
            k1 *= c2;
            h1 ^= k1;
    }

    // Finalization
    h1 ^= len; // Mix the length of the key
    h1 = fmix(h1); // Final mixing function

    return h1;
}

