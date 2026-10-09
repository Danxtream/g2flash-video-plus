/** Compare each Y byte and entropy state across separately compiled decoders.
 *  SPDX-License-Identifier: MIT
 */
#include "chroma_build_probe.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char **argv)
{
    if (int error = chroma_trimmed_storage_check())
    {
        std::fprintf(stderr, "storage check failed: %d\n", error);
        return 1;
    }
    uint64_t frames = 0U, nals = 0U, errors = 0U;
    for (int arg = 1; arg < argc; ++arg)
    {
        std::ifstream input(argv[arg], std::ios::binary);
        if (!input) return 2;
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        void *a = chroma_reference_create(bytes.data(), static_cast<uint32_t>(bytes.size()), 1, 1);
        // Explicit color request must remain effectively skipped in this build.
        void *b = chroma_trimmed_create(bytes.data(), static_cast<uint32_t>(bytes.size()), 0, 1);
        uint32_t count = chroma_reference_nals(a), pictures = 0U, failures = 0U;
        if (count == 0U || count != chroma_trimmed_nals(b)) return 3;
        for (uint32_t index = 0U; index < count; ++index)
        {
            int sa = chroma_reference_step(a, index), sb = chroma_trimmed_step(b, index);
            auto va = chroma_reference_view(a), vb = chroma_trimmed_view(b);
            bool equal = sa == sb && va.frames == vb.frames && va.bit_offset == vb.bit_offset &&
                va.entered == vb.entered && va.completed == vb.completed &&
                va.context_bytes == vb.context_bytes && va.syntax_words == vb.syntax_words &&
                std::memcmp(va.contexts, vb.contexts, va.context_bytes) == 0;
            if (va.syntax_words)
                equal = equal && std::memcmp(va.syntax, vb.syntax, va.syntax_words * sizeof(uint32_t)) == 0;
            if (va.frames != pictures)
            {
                equal = equal && va.y && vb.y && !vb.has_chroma &&
                    va.width == vb.width && va.height == vb.height && va.stride == vb.stride;
                if (equal)
                    for (uint32_t row = 0U; row < va.height; ++row)
                        equal = equal && std::memcmp(va.y + row * va.stride,
                                                    vb.y + row * vb.stride, va.width) == 0;
                pictures = va.frames;
            }
            if (!equal)
            {
                std::fprintf(stderr, "Y/outcome/context mismatch: %s NAL %u\n", argv[arg], index);
                return 4;
            }
            failures += sa < 0;
        }
        chroma_reference_destroy(a);
        chroma_trimmed_destroy(b);
        frames += pictures; nals += count; errors += failures;
        std::printf("clip=%s nals=%u frames=%u errors=%u mismatches=0\n",
                    argv[arg], count, pictures, failures);
    }
    std::printf("PASS clips=%d nals=%llu frames=%llu errors=%llu mismatches=0\n",
                argc - 1, static_cast<unsigned long long>(nals),
                static_cast<unsigned long long>(frames), static_cast<unsigned long long>(errors));
    return 0;
}
